// Package renditions implements the derived-encoding registry and conversion
// queue of ADR 0033. A rendition is a derived encoding of an asset — sl-mesh
// for viewers, gltf for the modern client, extracted materials and textures —
// regenerable from the asset's canonical blob.
//
// Renditions are deliberately *not* vault blobs: they carry no durability
// obligation (ADR 0026 exempts derived data exactly as it does bakes), so
// they get blob rows for identity and integrity but no vault index entry and
// no location rows. Their bytes live in a sharded file tree beside the vault,
// keyed by checksum, and are served only through the grid's rendition
// endpoint. Losing one costs a reconversion, never content.
//
// The queue uses leases rather than locks: a worker that dies mid-job lets
// its lease lapse and the job becomes claimable again. One live job per
// (asset, kind) — requesting an already-queued conversion is a no-op, and
// requesting a failed one re-queues it, which is also how an operator retries
// after fixing whatever failed.
package renditions

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
	"strings"
	"time"
)

var (
	ErrNotFound = errors.New("rendition not found")
	ErrInvalid  = errors.New("rendition request is invalid")
	// ErrUnknownAsset refuses work against an asset the registry has never
	// heard of; a rendition derives from a canonical blob or from nothing.
	ErrUnknownAsset = errors.New("asset is not registered")
)

// Kinds the schema accepts, mirrored here so requests fail fast with
// ErrInvalid instead of a constraint error.
var validKinds = map[string]bool{
	"gltf": true, "sl-mesh": true, "sl-material": true, "j2c-texture": true,
	// The reverse of j2c-texture: a viewer-uploaded texture is canonically
	// JPEG2000, which the first-party client refuses by rule, so it needs a
	// modern copy derived from it (migration 000030).
	"png-texture": true,
}

// MaxRenditionSize bounds a stored rendition. It used to match the vault's blob
// cap exactly, on the reasoning that a derived encoding larger than any storable
// source is a converter bug rather than content.
//
// The two parted company on 2026-08-10, when the vault's cap rose to hold
// source-format uploads (ADR 0035) — 256 MiB as of that day. This one stays at
// 64 MiB, and the gap is now four-fold rather than two, because the
// reasoning behind it did not change: what is stored here derives from *one*
// mesh or one image, and an import splits a source file into one asset per mesh
// before anything is derived from it. A rendition approaching a whole
// character's upload is still the converter bug this was guarding against.
//
// That is safe by a chain of bounds rather than by habit, and the middle one is
// what holds it up:
//
//	source upload      256 MiB   this package's MaxBlobSize, and the region's
//	                             max_source_bytes pre-limit
//	an imported part    32 MiB   max_glb_bytes, enforced on every part by
//	                             validate_glb(..., Origin::Import)
//	an embedded image    8 MiB   max_image_bytes
//
// Nothing a rendition can derive from exceeds 32 MiB, and renditions are
// smaller than their source by construction: an sl-mesh carries geometry only,
// its textures having become assets of their own, and a texture rendition
// derives from one image. The largest part measured on a fully dressed CC5
// character was 20.1 MiB of GLB, most of it texture bytes that its type-49 does
// not carry.
//
// **The one case that would break this is currently unreachable, and whoever
// makes it reachable needs to raise this number.** meshsmith can convert an FBX
// straight to a `gltf` rendition, which for a single-mesh source produces one
// rendition roughly the size of the whole upload — up to the full 256 MiB. That
// branch has no caller: the region imports source files on its own publish
// worker and requests no rendition of them, because an import yields one asset
// per mesh and a rendition is one blob per (asset, kind). If that path is ever
// wired to something, this constant is the thing it will hit.
const MaxRenditionSize = 64 << 20

// maxAttempts parks a job that keeps failing instead of burning a worker on
// it forever; a re-request after a fix returns it to the queue.
const maxAttempts = 5

type Rendition struct {
	AssetID     string    `json:"assetId"`
	Kind        string    `json:"kind"`
	BlobID      string    `json:"blobId"`
	Generator   string    `json:"generator"`
	GeneratedAt time.Time `json:"generatedAt"`
	ByteLength  int64     `json:"byteLength"`
	Checksum    string    `json:"checksum"`
}

type Job struct {
	ID        string    `json:"id"`
	AssetID   string    `json:"assetId"`
	Kind      string    `json:"kind"`
	State     string    `json:"state"`
	Attempts  int       `json:"attempts"`
	Error     string    `json:"error,omitempty"`
	CreatedAt time.Time `json:"createdAt"`
	UpdatedAt time.Time `json:"updatedAt"`
}

type Store interface {
	// Request queues a conversion, idempotently: an existing queued, leased,
	// or done job is returned as it stands, a failed one is re-queued.
	Request(ctx context.Context, assetID, kind string) (Job, error)
	// Claim leases the oldest claimable job of one of the given kinds to a
	// worker. The second result is false when nothing is claimable.
	Claim(ctx context.Context, kinds []string, lease time.Duration) (Job, bool, error)
	// Fail records a conversion failure and releases the job. Attempts
	// exhausted parks it as failed; otherwise it returns to the queue.
	Fail(ctx context.Context, jobID, reason string) error
	// Put stores rendition bytes, minting their blob row, upserting the
	// rendition record, and marking any job for (asset, kind) done — one
	// transaction, so a recorded rendition always has its bytes.
	Put(ctx context.Context, assetID, kind, generator string, content io.Reader) (Rendition, error)
	// RequeueStale re-queues conversion for every stored rendition of the
	// kind whose generator differs from current — how a deployed converter
	// upgrade sweeps the content its predecessors produced. Jobs already
	// queued or leased are left alone. Returns how many jobs were (re)queued.
	RequeueStale(ctx context.Context, kind, currentGenerator string) (int64, error)
	// List reports an asset's renditions.
	List(ctx context.Context, assetID string) ([]Rendition, error)
	// Open returns a rendition's bytes. The caller closes them.
	Open(ctx context.Context, assetID, kind string) (io.ReadCloser, Rendition, error)
}

type PostgresStore struct {
	db   *sql.DB
	root string
}

// NewPostgresStore prepares the rendition store rooted at directory (a
// sibling of the vault's shards; the two never collide because shard names
// are two hex characters).
func NewPostgresStore(db *sql.DB, directory string) (*PostgresStore, error) {
	if db == nil {
		return nil, errors.New("rendition store requires a database connection")
	}
	root, err := filepath.Abs(directory)
	if err != nil {
		return nil, fmt.Errorf("resolve rendition directory: %w", err)
	}
	if err := os.MkdirAll(root, 0o755); err != nil {
		return nil, fmt.Errorf("create rendition directory: %w", err)
	}
	return &PostgresStore{db: db, root: root}, nil
}

// Directory reports the resolved filesystem root, for startup logging.
func (s *PostgresStore) Directory() string { return s.root }

func (s *PostgresStore) Request(ctx context.Context, assetID, kind string) (Job, error) {
	if !validUUID(assetID) || !validKinds[kind] {
		return Job{}, ErrInvalid
	}
	var job Job
	err := s.db.QueryRowContext(ctx, `
		INSERT INTO rendition_jobs (asset_id, kind)
		VALUES ($1, $2)
		ON CONFLICT (asset_id, kind) DO UPDATE SET
			state = CASE WHEN rendition_jobs.state = 'failed' THEN 'queued'
			             ELSE rendition_jobs.state END,
			attempts = CASE WHEN rendition_jobs.state = 'failed' THEN 0
			                ELSE rendition_jobs.attempts END,
			error = CASE WHEN rendition_jobs.state = 'failed' THEN ''
			             ELSE rendition_jobs.error END,
			updated_at = now()
		RETURNING id, asset_id, kind, state, attempts, error, created_at, updated_at`,
		assetID, kind).
		Scan(&job.ID, &job.AssetID, &job.Kind, &job.State, &job.Attempts,
			&job.Error, &job.CreatedAt, &job.UpdatedAt)
	if err != nil {
		if isForeignKeyViolation(err) {
			return Job{}, ErrUnknownAsset
		}
		return Job{}, fmt.Errorf("request rendition job: %w", err)
	}
	return job, nil
}

func (s *PostgresStore) Claim(ctx context.Context, kinds []string, lease time.Duration) (Job, bool, error) {
	if len(kinds) == 0 || lease <= 0 {
		return Job{}, false, ErrInvalid
	}
	for _, kind := range kinds {
		if !validKinds[kind] {
			return Job{}, false, ErrInvalid
		}
	}
	var job Job
	// SKIP LOCKED keeps concurrent workers from serializing on the head of
	// the queue; an expired lease is claimable exactly like a queued job.
	err := s.db.QueryRowContext(ctx, `
		UPDATE rendition_jobs SET state = 'leased', attempts = attempts + 1,
			leased_until = now() + $2 * interval '1 second', updated_at = now()
		WHERE id = (
			SELECT id FROM rendition_jobs
			WHERE kind = ANY(string_to_array($1, ',')) AND attempts < $3
			  AND (state = 'queued' OR (state = 'leased' AND leased_until < now()))
			ORDER BY created_at
			FOR UPDATE SKIP LOCKED
			LIMIT 1)
		RETURNING id, asset_id, kind, state, attempts, error, created_at, updated_at`,
		strings.Join(kinds, ","), int64(lease/time.Second), maxAttempts).
		Scan(&job.ID, &job.AssetID, &job.Kind, &job.State, &job.Attempts,
			&job.Error, &job.CreatedAt, &job.UpdatedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return Job{}, false, nil
	}
	if err != nil {
		return Job{}, false, fmt.Errorf("claim rendition job: %w", err)
	}
	return job, true, nil
}

func (s *PostgresStore) Fail(ctx context.Context, jobID, reason string) error {
	if !validUUID(jobID) {
		return ErrInvalid
	}
	if len(reason) > 2048 {
		reason = reason[:2048]
	}
	result, err := s.db.ExecContext(ctx, `
		UPDATE rendition_jobs SET
			state = CASE WHEN attempts >= $3 THEN 'failed' ELSE 'queued' END,
			leased_until = NULL, error = $2, updated_at = now()
		WHERE id = $1 AND state = 'leased'`, jobID, reason, maxAttempts)
	if err != nil {
		return fmt.Errorf("fail rendition job: %w", err)
	}
	affected, err := result.RowsAffected()
	if err != nil {
		return fmt.Errorf("count failed rendition job: %w", err)
	}
	if affected == 0 {
		return ErrNotFound
	}
	return nil
}

func (s *PostgresStore) Put(ctx context.Context, assetID, kind, generator string,
	content io.Reader) (Rendition, error) {
	if !validUUID(assetID) || !validKinds[kind] ||
		generator == "" || len(generator) > 128 {
		return Rendition{}, ErrInvalid
	}
	// Bytes first, index after, same discipline as the vault: a crash between
	// the two leaves an unindexed file a later Put replaces, never an index
	// row whose bytes are missing.
	checksum, length, err := s.write(content)
	if err != nil {
		return Rendition{}, err
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Rendition{}, fmt.Errorf("begin rendition put: %w", err)
	}
	defer tx.Rollback()
	rendition := Rendition{AssetID: assetID, Kind: kind, Generator: generator,
		ByteLength: length, Checksum: checksum}
	if err := tx.QueryRowContext(ctx, `
		INSERT INTO blobs (byte_length, checksum, checksum_algorithm)
		VALUES ($1, $2, 'sha256') RETURNING blob_id`, length, checksum).
		Scan(&rendition.BlobID); err != nil {
		return Rendition{}, fmt.Errorf("mint rendition blob: %w", err)
	}
	// A regenerated rendition replaces the record; the superseded blob row
	// becomes unreferenced, which the deferred collection reclaims.
	if err := tx.QueryRowContext(ctx, `
		INSERT INTO asset_renditions (asset_id, kind, blob_id, generator)
		VALUES ($1, $2, $3, $4)
		ON CONFLICT (asset_id, kind) DO UPDATE SET
			blob_id = EXCLUDED.blob_id, generator = EXCLUDED.generator,
			generated_at = now()
		RETURNING generated_at`, assetID, kind, rendition.BlobID, generator).
		Scan(&rendition.GeneratedAt); err != nil {
		if isForeignKeyViolation(err) {
			return Rendition{}, ErrUnknownAsset
		}
		return Rendition{}, fmt.Errorf("record rendition: %w", err)
	}
	if _, err := tx.ExecContext(ctx, `
		UPDATE rendition_jobs SET state = 'done', leased_until = NULL,
			error = '', updated_at = now()
		WHERE asset_id = $1 AND kind = $2`, assetID, kind); err != nil {
		return Rendition{}, fmt.Errorf("complete rendition job: %w", err)
	}
	if err := tx.Commit(); err != nil {
		return Rendition{}, fmt.Errorf("commit rendition put: %w", err)
	}
	return rendition, nil
}

func (s *PostgresStore) RequeueStale(ctx context.Context, kind, currentGenerator string) (int64, error) {
	if !validKinds[kind] || currentGenerator == "" || len(currentGenerator) > 128 {
		return 0, ErrInvalid
	}
	// The generator column exists for exactly this query. Conflicting rows in
	// flight (queued or leased) are skipped — their outcome is either the
	// current generator's work already, or a stale result the next sweep
	// catches. Done and failed jobs return to the queue with fresh attempts.
	result, err := s.db.ExecContext(ctx, `
		INSERT INTO rendition_jobs (asset_id, kind)
		SELECT r.asset_id, r.kind FROM asset_renditions AS r
		WHERE r.kind = $1 AND r.generator <> $2
		ON CONFLICT (asset_id, kind) DO UPDATE SET
			state = 'queued', attempts = 0, error = '', leased_until = NULL,
			updated_at = now()
		WHERE rendition_jobs.state IN ('done', 'failed')`,
		kind, currentGenerator)
	if err != nil {
		return 0, fmt.Errorf("requeue stale renditions: %w", err)
	}
	requeued, err := result.RowsAffected()
	if err != nil {
		return 0, fmt.Errorf("count requeued renditions: %w", err)
	}
	return requeued, nil
}

func (s *PostgresStore) List(ctx context.Context, assetID string) ([]Rendition, error) {
	if !validUUID(assetID) {
		return nil, ErrInvalid
	}
	rows, err := s.db.QueryContext(ctx, `
		SELECT r.asset_id, r.kind, r.blob_id, r.generator, r.generated_at,
		       b.byte_length, b.checksum
		FROM asset_renditions AS r JOIN blobs AS b ON b.blob_id = r.blob_id
		WHERE r.asset_id = $1 ORDER BY r.kind`, assetID)
	if err != nil {
		return nil, fmt.Errorf("list renditions: %w", err)
	}
	defer rows.Close()
	var values []Rendition
	for rows.Next() {
		var value Rendition
		if err := rows.Scan(&value.AssetID, &value.Kind, &value.BlobID, &value.Generator,
			&value.GeneratedAt, &value.ByteLength, &value.Checksum); err != nil {
			return nil, err
		}
		values = append(values, value)
	}
	return values, rows.Err()
}

func (s *PostgresStore) Open(ctx context.Context, assetID, kind string) (io.ReadCloser, Rendition, error) {
	if !validUUID(assetID) || !validKinds[kind] {
		return nil, Rendition{}, ErrInvalid
	}
	var value Rendition
	err := s.db.QueryRowContext(ctx, `
		SELECT r.asset_id, r.kind, r.blob_id, r.generator, r.generated_at,
		       b.byte_length, b.checksum
		FROM asset_renditions AS r JOIN blobs AS b ON b.blob_id = r.blob_id
		WHERE r.asset_id = $1 AND r.kind = $2`, assetID, kind).
		Scan(&value.AssetID, &value.Kind, &value.BlobID, &value.Generator,
			&value.GeneratedAt, &value.ByteLength, &value.Checksum)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, Rendition{}, ErrNotFound
	}
	if err != nil {
		return nil, Rendition{}, fmt.Errorf("read rendition: %w", err)
	}
	file, err := os.Open(s.path(value.Checksum))
	if errors.Is(err, os.ErrNotExist) {
		// An index row whose bytes are gone is not a rendition; the honest
		// answer invites a re-request rather than a broken read.
		return nil, Rendition{}, ErrNotFound
	}
	if err != nil {
		return nil, Rendition{}, fmt.Errorf("open rendition bytes: %w", err)
	}
	return file, value, nil
}

func (s *PostgresStore) path(checksum string) string {
	return filepath.Join(s.root, checksum[:2], checksum[2:])
}

// write streams content to the sharded tree, computing its checksum as it
// goes — the grid mints rendition identity rather than verifying a claim,
// which is why only the worker credential may reach this path.
func (s *PostgresStore) write(content io.Reader) (checksum string, length int64, err error) {
	temporary, err := os.CreateTemp(s.root, "*.partial")
	if err != nil {
		return "", 0, fmt.Errorf("create rendition temporary file: %w", err)
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
	length, err = io.Copy(io.MultiWriter(temporary, hasher),
		io.LimitReader(content, MaxRenditionSize+1))
	if err != nil {
		return "", 0, fmt.Errorf("write rendition bytes: %w", err)
	}
	if length == 0 || length > MaxRenditionSize {
		return "", 0, ErrInvalid
	}
	checksum = hex.EncodeToString(hasher.Sum(nil))
	if err := temporary.Sync(); err != nil {
		return "", 0, fmt.Errorf("flush rendition bytes: %w", err)
	}
	if err := temporary.Close(); err != nil {
		return "", 0, fmt.Errorf("close rendition bytes: %w", err)
	}
	if err := os.MkdirAll(filepath.Dir(s.path(checksum)), 0o755); err != nil {
		return "", 0, fmt.Errorf("create rendition shard: %w", err)
	}
	if err := os.Rename(name, s.path(checksum)); err != nil {
		return "", 0, fmt.Errorf("publish rendition bytes: %w", err)
	}
	published = true
	return checksum, length, nil
}

// isForeignKeyViolation matches without importing a driver-specific error
// type: SQLSTATE 23503 appears in the error text for both pgx and lib/pq.
func isForeignKeyViolation(err error) bool {
	return err != nil && (strings.Contains(err.Error(), "23503") ||
		strings.Contains(err.Error(), "foreign key"))
}

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
