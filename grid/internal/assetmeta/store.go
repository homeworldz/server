// Package assetmeta is the grid's asset registry over the blob and asset
// layers of ADR 0027.
//
// A **blob** is immutable bytes named by a grid-assigned blob_id, carrying a
// byte length, an integrity checksum, and the endpoints serving it. An
// **asset** is a viewer-facing UUID naming exactly one blob, carrying creator,
// provenance, and the creator's exportable option. Instances — inventory items
// and rezzed objects — reference assets and hold every permission mask; they
// live in the inventory store, not here.
//
// blob_id is deliberately **grid-internal**. A region names assets and
// verifies bytes with the checksum, exactly as before the layers were
// separated, so the registry's wire shape is unchanged by the separation and
// no region needs upgrading for it. The indirection asset → blob_id → bytes
// is what lets the vault, deduplication, and retention work on bytes without
// touching assets or instances.
package assetmeta

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"time"
)

var (
	ErrNotFound = errors.New("asset metadata not found")
	ErrConflict = errors.New("asset metadata conflicts with immutable registration")
)

type Location struct {
	Endpoint   string    `json:"endpoint"`
	Origin     bool      `json:"origin"`
	VerifiedAt time.Time `json:"verifiedAt"`
}

// Asset is the registry's wire shape, unchanged across the layer separation:
// SHA256 and Size describe the blob the asset names, and Locations are the
// blob's serving endpoints. A region reads exactly these fields.
type Asset struct {
	ID            string     `json:"id"`
	CreatorUserID string     `json:"creatorUserId"`
	SHA256        string     `json:"sha256"`
	Size          int64      `json:"size"`
	Locations     []Location `json:"locations"`
	// Bake is an avatar bake (ADR 0029): the one kind of asset the public
	// appearance service serves. Latched on by any registration that says so.
	Bake bool `json:"bake"`
}

type Registration struct {
	ID            string
	CreatorUserID string
	SHA256        string
	Size          int64
	Endpoint      string
	Origin        bool
	Bake          bool
}

// Blob identifies the bytes an asset names, for the vault and anything else
// that works below the asset layer. Callers outside the grid never see it.
type Blob struct {
	BlobID            string
	ByteLength        int64
	Checksum          string
	ChecksumAlgorithm string
}

type Store interface {
	Register(context.Context, Registration) (Asset, error)
	Get(context.Context, string) (Asset, error)
	// Blob resolves the blob an asset names. ErrNotFound when the asset is
	// unknown.
	Blob(ctx context.Context, assetID string) (Blob, error)
}

type PostgresStore struct{ db *sql.DB }

func NewPostgresStore(db *sql.DB) *PostgresStore { return &PostgresStore{db: db} }

// Register records an asset and one serving location for its blob.
//
// A first registration mints a blob for the declared bytes. A repeat — the
// same asset registered by another region as a replica, or a re-registration
// at startup — must agree with what is stored: same creator, same checksum,
// same length, or ErrConflict. That check is what makes an asset UUID's
// binding to its content immutable, and it now rests on the asset's blob
// rather than on a hash column.
//
// A new blob is minted per new asset even when byte-identical bytes are
// already stored, deliberately: coalescing on a checksum match would be
// hash-trusting deduplication, and ADR 0027 requires literal byte comparison
// for that, done asynchronously. Duplicate bytes cost disk, never
// correctness.
func (s *PostgresStore) Register(ctx context.Context, input Registration) (Asset, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Asset{}, fmt.Errorf("begin asset registration: %w", err)
	}
	defer tx.Rollback()

	var blobID, creator, checksum string
	var length int64
	err = tx.QueryRowContext(ctx, `
		SELECT asset.blob_id, asset.creator_user_id, blob.checksum, blob.byte_length
		FROM assets AS asset JOIN blobs AS blob ON blob.blob_id = asset.blob_id
		WHERE asset.asset_id = $1`, input.ID).Scan(&blobID, &creator, &checksum, &length)
	switch {
	case errors.Is(err, sql.ErrNoRows):
		if err := tx.QueryRowContext(ctx, `
			INSERT INTO blobs (byte_length, checksum, checksum_algorithm)
			VALUES ($1, $2, 'sha256') RETURNING blob_id`,
			input.Size, input.SHA256).Scan(&blobID); err != nil {
			return Asset{}, fmt.Errorf("insert blob: %w", err)
		}
		if _, err := tx.ExecContext(ctx, `
			INSERT INTO assets (asset_id, blob_id, creator_user_id, is_bake)
			VALUES ($1, $2, $3, $4)`, input.ID, blobID, input.CreatorUserID, input.Bake); err != nil {
			return Asset{}, fmt.Errorf("insert asset: %w", err)
		}
	case err != nil:
		return Asset{}, fmt.Errorf("load asset: %w", err)
	default:
		if creator != input.CreatorUserID || checksum != input.SHA256 || length != input.Size {
			return Asset{}, ErrConflict
		}
		if input.Bake {
			if _, err := tx.ExecContext(ctx, `
				UPDATE assets SET is_bake = true WHERE asset_id = $1 AND NOT is_bake`,
				input.ID); err != nil {
				return Asset{}, fmt.Errorf("mark asset as bake: %w", err)
			}
		}
	}

	// Locations attach to the blob: identical bytes reachable at an endpoint
	// are reachable regardless of which asset names them. Origin latches on,
	// as it did when locations hung off the asset.
	if _, err := tx.ExecContext(ctx, `
		INSERT INTO blob_locations (blob_id, endpoint, is_origin)
		VALUES ($1, $2, $3)
		ON CONFLICT (blob_id, endpoint) DO UPDATE
		SET is_origin = blob_locations.is_origin OR EXCLUDED.is_origin, verified_at = now()`,
		blobID, input.Endpoint, input.Origin); err != nil {
		return Asset{}, fmt.Errorf("register blob location: %w", err)
	}
	if err := tx.Commit(); err != nil {
		return Asset{}, fmt.Errorf("commit asset registration: %w", err)
	}
	return s.Get(ctx, input.ID)
}

func (s *PostgresStore) Get(ctx context.Context, id string) (Asset, error) {
	asset := Asset{ID: id, Locations: make([]Location, 0)}
	var blobID string
	if err := s.db.QueryRowContext(ctx, `
		SELECT asset.creator_user_id, blob.checksum, blob.byte_length, blob.blob_id, asset.is_bake
		FROM assets AS asset JOIN blobs AS blob ON blob.blob_id = asset.blob_id
		WHERE asset.asset_id = $1`, id).
		Scan(&asset.CreatorUserID, &asset.SHA256, &asset.Size, &blobID, &asset.Bake); errors.Is(err, sql.ErrNoRows) {
		return Asset{}, ErrNotFound
	} else if err != nil {
		return Asset{}, fmt.Errorf("get asset: %w", err)
	}
	rows, err := s.db.QueryContext(ctx, `
		SELECT endpoint, is_origin, verified_at FROM blob_locations
		WHERE blob_id = $1 ORDER BY is_origin DESC, verified_at DESC, endpoint`, blobID)
	if err != nil {
		return Asset{}, fmt.Errorf("list blob locations: %w", err)
	}
	defer rows.Close()
	for rows.Next() {
		var location Location
		if err := rows.Scan(&location.Endpoint, &location.Origin, &location.VerifiedAt); err != nil {
			return Asset{}, fmt.Errorf("scan blob location: %w", err)
		}
		asset.Locations = append(asset.Locations, location)
	}
	if err := rows.Err(); err != nil {
		return Asset{}, fmt.Errorf("iterate blob locations: %w", err)
	}
	return asset, nil
}

func (s *PostgresStore) Blob(ctx context.Context, assetID string) (Blob, error) {
	var blob Blob
	if err := s.db.QueryRowContext(ctx, `
		SELECT blob.blob_id, blob.byte_length, blob.checksum, blob.checksum_algorithm
		FROM assets AS asset JOIN blobs AS blob ON blob.blob_id = asset.blob_id
		WHERE asset.asset_id = $1`, assetID).
		Scan(&blob.BlobID, &blob.ByteLength, &blob.Checksum,
			&blob.ChecksumAlgorithm); errors.Is(err, sql.ErrNoRows) {
		return Blob{}, ErrNotFound
	} else if err != nil {
		return Blob{}, fmt.Errorf("get asset blob: %w", err)
	}
	return blob, nil
}
