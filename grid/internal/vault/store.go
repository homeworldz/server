// Package vault implements the grid-side asset vault of ADR 0026: a durable,
// replica-only blob store holding the bytes behind every inventory-referenced
// asset, so a user's inventory survives the permanent loss of any region.
//
// Three properties the rest of the grid depends on:
//
//   - It is replica-only. The vault never originates an asset, never assigns a
//     viewer-facing UUID, and never hosts an agent. Bytes only ever arrive here
//     as a copy of content a region already holds.
//   - It is never in the viewer data path. Viewers fetch asset bytes from the
//     region they are connected to; the vault serves regions, over the internal
//     service-token boundary, and nothing else.
//   - It fails closed. Bytes are verified against the registry's recorded
//     checksum and length before they become reachable, so the vault never
//     serves content that does not match what the grid says the blob is.
//
// Blobs are held by the grid-assigned blob_id of ADR 0027, which is what the
// inventory-commit invariant asks about, and stored on disk under the blob's
// SHA-256 integrity checksum, matching the region blob stores of ADR 0014. The
// caller never supplies the checksum or the length: both are read from the
// blob registry, so bytes that disagree with the registry can never be
// ingested, and no caller can talk the vault into vouching for the wrong
// content.
package vault

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"time"
)

// MaxBlobSize bounds a single ingest so a malformed or hostile declared length
// cannot fill the vault filesystem.
//
// **Raised to 128 MiB on 2026-08-10, from 64.** The old figure was described
// here as "far above any expected texture, mesh, or animation asset", and that
// stopped being true when source formats became storable (ADR 0035): the
// canonical blob is now the creator's own FBX rather than a converted GLB, and
// it carries every texture the character uses. Measured against the four
// Character Creator exports prepared that day, two exceeded 64 MiB — a fully
// dressed CC5 character with hair and beard is 105 MiB, and one of the plain
// CC3 bodies came in 19 KB over, which is the margin that says the old cap was
// not chosen against files like these.
//
// 128 MiB rather than more because this is a deliberately provisional number:
// Kevin is near the complex end of what a creator exports, and the cost of a
// larger cap is real. The region reads an upload into memory and copies it for
// its publish worker, so the peak is roughly twice the file.
const MaxBlobSize = 128 << 20

var (
	ErrNotFound = errors.New("vault does not hold the blob")
	ErrMismatch = errors.New("blob bytes do not match the registered checksum or length")
	ErrInvalid  = errors.New("blob is not registered, or its registration is unusable")
)

// Blob is a blob the vault durably holds, described by what the registry says
// it is.
type Blob struct {
	BlobID     string    `json:"blobId"`
	ByteLength int64     `json:"byteLength"`
	Checksum   string    `json:"checksum"`
	IngestedAt time.Time `json:"ingestedAt"`
}

type Store interface {
	// Ingest durably stores the bytes of a registered blob, verifying them
	// against the checksum and length the registry recorded, before the blob
	// becomes reachable. It is idempotent: ingesting a blob the vault already
	// holds succeeds and reports the existing record.
	Ingest(ctx context.Context, blobID string, content io.Reader) (Blob, error)
	// Held reports a blob the vault durably holds, or ErrNotFound. This is the
	// question the inventory-commit invariant of ADR 0026 asks.
	Held(ctx context.Context, blobID string) (Blob, error)
	// Open returns the bytes of a blob the vault holds. The caller closes them.
	Open(ctx context.Context, blobID string) (io.ReadCloser, Blob, error)
}

// PostgresStore keeps blob bytes on a local filesystem tree and indexes them in
// PostgreSQL. The index must never claim a blob the filesystem cannot produce,
// because that claim is what inventory commits trust; Held therefore confirms
// both.
type PostgresStore struct {
	files blobFiles
	db    *sql.DB
}

// NewPostgresStore prepares the vault rooted at directory. A grid that cannot
// open its vault must not start: with the ADR 0026 invariant enforced, an
// unavailable vault has to fail inventory writes rather than silently skip
// durability.
func NewPostgresStore(db *sql.DB, directory string) (*PostgresStore, error) {
	if db == nil {
		return nil, errors.New("vault requires a database connection")
	}
	root, err := filepath.Abs(directory)
	if err != nil {
		return nil, fmt.Errorf("resolve vault directory: %w", err)
	}
	if err := os.MkdirAll(root, 0o755); err != nil {
		return nil, fmt.Errorf("create vault directory: %w", err)
	}
	return &PostgresStore{files: blobFiles{root: root}, db: db}, nil
}

// Directory reports the resolved filesystem root, for startup logging.
func (s *PostgresStore) Directory() string { return s.files.root }

// registered reads what the blob layer says a blob is. An unregistered blob is
// ErrInvalid rather than ErrNotFound: the vault stores bytes for blobs the grid
// has registered, and being asked about one it has never heard of is a caller
// error, not an absent replica.
func (s *PostgresStore) registered(ctx context.Context, blobID string) (Blob, error) {
	if !validUUID(blobID) {
		return Blob{}, ErrInvalid
	}
	var blob Blob
	err := s.db.QueryRowContext(ctx, `
		SELECT blob_id, byte_length, checksum FROM blobs
		WHERE blob_id = $1 AND checksum_algorithm = 'sha256'`, blobID).
		Scan(&blob.BlobID, &blob.ByteLength, &blob.Checksum)
	if errors.Is(err, sql.ErrNoRows) {
		return Blob{}, ErrInvalid
	} else if err != nil {
		return Blob{}, fmt.Errorf("read blob registration: %w", err)
	}
	if !ValidDigest(blob.Checksum) || blob.ByteLength <= 0 || blob.ByteLength > MaxBlobSize {
		return Blob{}, ErrInvalid
	}
	return blob, nil
}

func (s *PostgresStore) Ingest(ctx context.Context, blobID string, content io.Reader) (Blob, error) {
	blob, err := s.registered(ctx, blobID)
	if err != nil {
		return Blob{}, err
	}
	// Bytes are published before the index row is written. A crash between the
	// two leaves an unindexed file, which a later ingest simply replaces; the
	// reverse order would leave the index claiming bytes the vault cannot serve,
	// and that claim is exactly what inventory commits trust.
	if err := s.files.write(blob.Checksum, blob.ByteLength, content); err != nil {
		return Blob{}, err
	}
	// Matching bytes mean a re-ingest has nothing to reconcile. The no-op update
	// exists only so RETURNING still yields the existing row, which a bare
	// DO NOTHING would not.
	if err := s.db.QueryRowContext(ctx, `
		INSERT INTO vault_blobs (blob_id, byte_length) VALUES ($1, $2)
		ON CONFLICT (blob_id) DO UPDATE SET byte_length = vault_blobs.byte_length
		RETURNING ingested_at`, blob.BlobID, blob.ByteLength).
		Scan(&blob.IngestedAt); err != nil {
		return Blob{}, fmt.Errorf("index vault blob: %w", err)
	}
	return blob, nil
}

func (s *PostgresStore) Held(ctx context.Context, blobID string) (Blob, error) {
	if !validUUID(blobID) {
		return Blob{}, ErrInvalid
	}
	var blob Blob
	err := s.db.QueryRowContext(ctx, `
		SELECT held.blob_id, held.byte_length, registered.checksum, held.ingested_at
		FROM vault_blobs AS held
		JOIN blobs AS registered ON registered.blob_id = held.blob_id
		WHERE held.blob_id = $1 AND registered.checksum_algorithm = 'sha256'`, blobID).
		Scan(&blob.BlobID, &blob.ByteLength, &blob.Checksum, &blob.IngestedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return Blob{}, ErrNotFound
	} else if err != nil {
		return Blob{}, fmt.Errorf("read vault blob index: %w", err)
	}
	// An index row whose bytes are missing or truncated is not a held blob. This
	// costs one stat call and keeps the durability answer honest.
	stored, err := s.files.size(blob.Checksum)
	if errors.Is(err, ErrNotFound) {
		return Blob{}, ErrNotFound
	} else if err != nil {
		return Blob{}, err
	}
	if stored != blob.ByteLength {
		return Blob{}, ErrNotFound
	}
	return blob, nil
}

func (s *PostgresStore) Open(ctx context.Context, blobID string) (io.ReadCloser, Blob, error) {
	blob, err := s.Held(ctx, blobID)
	if err != nil {
		return nil, Blob{}, err
	}
	// No checksum recompute on read. The vault trusts its own storage layer as
	// any storage layer is trusted (ADR 0026); verification concentrates at the
	// untrusted boundary, where a fetching region checks these bytes against the
	// grid-recorded checksum (ADR 0027, ADR 0028). This is deliberately unlike
	// the region blob store, which re-hashes because it serves viewers directly.
	file, err := os.Open(s.files.path(blob.Checksum))
	if errors.Is(err, os.ErrNotExist) {
		return nil, Blob{}, ErrNotFound
	} else if err != nil {
		return nil, Blob{}, fmt.Errorf("open vault blob: %w", err)
	}
	return file, blob, nil
}

// blobFiles stores blob bytes sharded by the first checksum byte, the same
// layout the region blob store uses. Two blob_ids that share a checksum share
// the file, which is safe because the file's name is what its bytes hash to:
// the sharing is storage deduplication, not the identity coalescing ADR 0027
// refuses.
type blobFiles struct{ root string }

func (f blobFiles) path(digest string) string {
	return filepath.Join(f.root, digest[:2], digest[2:])
}

// write streams content into the shard for digest, verifying the bytes against
// the registered digest and length before publishing them. Verification happens
// on a temporary file in the destination directory, so bytes that fail it are
// never reachable under the digest and the publish itself is an atomic rename.
func (f blobFiles) write(digest string, declared int64, content io.Reader) error {
	shard := filepath.Join(f.root, digest[:2])
	if err := os.MkdirAll(shard, 0o755); err != nil {
		return fmt.Errorf("create vault shard: %w", err)
	}
	temporary, err := os.CreateTemp(shard, digest[2:]+".*.partial")
	if err != nil {
		return fmt.Errorf("create vault temporary file: %w", err)
	}
	name := temporary.Name()
	published := false
	defer func() {
		temporary.Close()
		if !published {
			os.Remove(name)
		}
	}()
	hasher := sha256.New()
	// Reading one byte past the declared length is enough to detect an over-long
	// body without writing an unbounded amount of it to disk.
	written, err := io.Copy(io.MultiWriter(temporary, hasher),
		io.LimitReader(content, declared+1))
	if err != nil {
		return fmt.Errorf("write vault blob: %w", err)
	}
	if written != declared || hex.EncodeToString(hasher.Sum(nil)) != digest {
		return ErrMismatch
	}
	if err := temporary.Sync(); err != nil {
		return fmt.Errorf("flush vault blob: %w", err)
	}
	if err := temporary.Close(); err != nil {
		return fmt.Errorf("close vault blob: %w", err)
	}
	if err := os.Rename(name, f.path(digest)); err != nil {
		return fmt.Errorf("publish vault blob: %w", err)
	}
	published = true
	return nil
}

// size reports the stored length of a blob, or ErrNotFound.
func (f blobFiles) size(digest string) (int64, error) {
	info, err := os.Stat(f.path(digest))
	if errors.Is(err, os.ErrNotExist) {
		return 0, ErrNotFound
	} else if err != nil {
		return 0, fmt.Errorf("stat vault blob: %w", err)
	}
	return info.Size(), nil
}

// ValidDigest reports whether value is a lowercase hexadecimal SHA-256.
func ValidDigest(value string) bool {
	if len(value) != 64 {
		return false
	}
	for _, character := range value {
		if !((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f')) {
			return false
		}
	}
	return true
}

// validUUID accepts the canonical 8-4-4-4-12 hexadecimal form, the shape every
// grid-assigned blob_id takes.
func validUUID(value string) bool {
	if len(value) != 36 {
		return false
	}
	for index, character := range value {
		switch index {
		case 8, 13, 18, 23:
			if character != '-' {
				return false
			}
		default:
			if !((character >= '0' && character <= '9') ||
				(character >= 'a' && character <= 'f') ||
				(character >= 'A' && character <= 'F')) {
				return false
			}
		}
	}
	return true
}
