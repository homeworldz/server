package provisioning

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"errors"
	"fmt"
	"strings"

	"github.com/jackc/pgx/v5/pgconn"
)

type PostgresStore struct{ db *sql.DB }

func NewPostgresStore(db *sql.DB) *PostgresStore { return &PostgresStore{db: db} }

const regionColumns = `id,name,owner_user_id,grid_x,grid_y,size_x,size_y,maturity,public_endpoint,viewer_port,enabled,kind,tags`

func (s *PostgresStore) Import(ctx context.Context, items []Region) error {
	for _, item := range items {
		if err := validate(item); err != nil {
			return err
		}
		hash := sha256.Sum256([]byte(item.AccessKey))
		result, err := s.db.ExecContext(ctx, `INSERT INTO provisioned_regions
			(`+regionColumns+`,access_key_hash)
			VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14) ON CONFLICT (id) DO NOTHING`,
			item.ID, item.Name, nullableOwner(item.OwnerUserID), item.MapX, item.MapY,
			item.SizeX, item.SizeY, item.Maturity, item.PublicEndpoint, item.ViewerPort, item.Enabled,
			regionKindOrDefault(item.Kind), item.Tags, hash[:])
		if err != nil {
			return classify("import provisioned region", err)
		}
		if inserted, err := result.RowsAffected(); err != nil || inserted == 0 {
			continue
		}
		if err := s.insertFacetNames(ctx, s.db, item.ID, item.FacetNames); err != nil {
			return classify("import provisioned region facets", err)
		}
	}
	return nil
}

// Authenticate proves which region is calling, and nothing more. It
// deliberately does not filter on `enabled`: that is an authorization
// question, and the caller asks it, because there is one thing a disabled
// region must still be able to do — release the lease it is holding. Folding
// the two together meant a disabled region could not deregister, so its
// coordinates stayed occupied by a region that was not allowed to run
// (2026-08-21).
func (s *PostgresStore) Authenticate(ctx context.Context, id, accessKey string) (Region, bool) {
	hash := sha256.Sum256([]byte(accessKey))
	region, err := s.get(ctx, `(id::text = $1 OR lower(name) = lower($1))
		AND access_key_hash = $2`, id, hash[:])
	return region, err == nil
}

func (s *PostgresStore) List(ctx context.Context) ([]Region, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT `+regionColumns+`
        FROM provisioned_regions ORDER BY grid_y,grid_x,id`)
	if err != nil {
		return nil, fmt.Errorf("list provisioned regions: %w", err)
	}
	defer rows.Close()
	items := make([]Region, 0)
	for rows.Next() {
		item, err := scanRegion(rows)
		if err != nil {
			return nil, fmt.Errorf("scan provisioned region: %w", err)
		}
		items = append(items, item)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate provisioned regions: %w", err)
	}
	if err := s.attachFacetNames(ctx, items); err != nil {
		return nil, err
	}
	return items, nil
}

func (s *PostgresStore) Get(ctx context.Context, id string) (Region, error) {
	return s.get(ctx, `id = $1`, id)
}

func (s *PostgresStore) Create(ctx context.Context, item Region) (Region, error) {
	item.Name = normalize(item.Name)
	item.OwnerUserID = normalize(item.OwnerUserID)
	item.PublicEndpoint = normalize(item.PublicEndpoint)
	item.FacetNames = trimmedNames(item.FacetNames)
	if item.SizeX == 0 {
		item.SizeX = 1
	}
	if item.SizeY == 0 {
		item.SizeY = 1
	}
	if err := validate(item); err != nil {
		return Region{}, err
	}
	hash := sha256.Sum256([]byte(item.AccessKey))
	created, err := s.inTransaction(ctx, func(tx *sql.Tx) (Region, error) {
		if err := s.checkNamesAcrossTables(ctx, tx, item); err != nil {
			return Region{}, err
		}
		row := tx.QueryRowContext(ctx, `INSERT INTO provisioned_regions
			(`+regionColumns+`,access_key_hash)
			VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14)
			RETURNING `+regionColumns,
			item.ID, item.Name, nullableOwner(item.OwnerUserID), item.MapX, item.MapY,
			item.SizeX, item.SizeY, item.Maturity, item.PublicEndpoint, item.ViewerPort, item.Enabled,
			regionKindOrDefault(item.Kind), item.Tags, hash[:])
		created, err := scanRegion(row)
		if err != nil {
			return Region{}, err
		}
		if err := s.insertFacetNames(ctx, tx, created.ID, item.FacetNames); err != nil {
			return Region{}, err
		}
		created.FacetNames = item.FacetNames
		return created, nil
	})
	if err != nil {
		return Region{}, classify("create provisioned region", err)
	}
	return created, nil
}

func (s *PostgresStore) Update(ctx context.Context, id string, update Update) (Region, error) {
	current, err := s.Get(ctx, id)
	if err != nil {
		return Region{}, err
	}
	if update.Name != nil {
		current.Name = normalize(*update.Name)
	}
	if update.OwnerUserID != nil {
		current.OwnerUserID = normalize(*update.OwnerUserID)
	}
	if update.MapX != nil {
		current.MapX = *update.MapX
	}
	if update.MapY != nil {
		current.MapY = *update.MapY
	}
	if update.SizeX != nil {
		current.SizeX = *update.SizeX
	}
	if update.SizeY != nil {
		current.SizeY = *update.SizeY
	}
	if update.FacetNames != nil {
		current.FacetNames = trimmedNames(*update.FacetNames)
	}
	if update.Maturity != nil {
		current.Maturity = *update.Maturity
	}
	if update.PublicEndpoint != nil {
		current.PublicEndpoint = normalize(*update.PublicEndpoint)
	}
	if update.ViewerPort != nil {
		current.ViewerPort = *update.ViewerPort
	}
	if update.Enabled != nil {
		current.Enabled = *update.Enabled
	}
	if update.Kind != nil {
		current.Kind = *update.Kind
	}
	if update.Tags != nil {
		current.Tags = *update.Tags
	}
	current.AccessKey = "stored-key"
	if err := validate(current); err != nil {
		return Region{}, err
	}
	updated, err := s.inTransaction(ctx, func(tx *sql.Tx) (Region, error) {
		if err := s.checkNamesAcrossTables(ctx, tx, current); err != nil {
			return Region{}, err
		}
		row := tx.QueryRowContext(ctx, `UPDATE provisioned_regions SET
			name=$2,owner_user_id=$3,grid_x=$4,grid_y=$5,size_x=$6,size_y=$7,maturity=$8,
			public_endpoint=$9,viewer_port=$10,enabled=$11,kind=$12,tags=$13,updated_at=now()
			WHERE id=$1 RETURNING `+regionColumns,
			id, current.Name, nullableOwner(current.OwnerUserID), current.MapX, current.MapY,
			current.SizeX, current.SizeY, current.Maturity, current.PublicEndpoint, current.ViewerPort, current.Enabled,
			regionKindOrDefault(current.Kind), current.Tags)
		item, err := scanRegion(row)
		if err != nil {
			return Region{}, err
		}
		if _, err := tx.ExecContext(ctx, `DELETE FROM provisioned_region_facets WHERE region_id=$1`, id); err != nil {
			return Region{}, err
		}
		if err := s.insertFacetNames(ctx, tx, id, current.FacetNames); err != nil {
			return Region{}, err
		}
		item.FacetNames = current.FacetNames
		return item, nil
	})
	if err != nil {
		return Region{}, classify("update provisioned region", err)
	}
	return updated, nil
}

func (s *PostgresStore) RotateAccessKey(ctx context.Context, id, accessKey string) (Region, error) {
	if normalize(accessKey) == "" {
		return Region{}, fmt.Errorf("%w: access key is empty", ErrInvalid)
	}
	hash := sha256.Sum256([]byte(accessKey))
	row := s.db.QueryRowContext(ctx, `UPDATE provisioned_regions
		SET access_key_hash=$2,updated_at=now() WHERE id=$1
		RETURNING `+regionColumns, id, hash[:])
	item, err := scanRegion(row)
	if err != nil {
		return Region{}, classify("rotate provisioned region access key", err)
	}
	if err := s.attachFacetNames(ctx, []Region{item}); err != nil {
		return Region{}, err
	}
	return item, nil
}

func (s *PostgresStore) Delete(ctx context.Context, id string) error {
	result, err := s.db.ExecContext(ctx, `DELETE FROM provisioned_regions WHERE id=$1`, id)
	if err != nil {
		return fmt.Errorf("delete provisioned region: %w", err)
	}
	count, err := result.RowsAffected()
	if err != nil {
		return fmt.Errorf("count deleted provisioned regions: %w", err)
	}
	if count == 0 {
		return ErrNotFound
	}
	return nil
}

func (s *PostgresStore) get(ctx context.Context, predicate string, arguments ...any) (Region, error) {
	row := s.db.QueryRowContext(ctx, `SELECT `+regionColumns+`
        FROM provisioned_regions WHERE `+predicate, arguments...)
	item, err := scanRegion(row)
	if errors.Is(err, sql.ErrNoRows) {
		return Region{}, ErrNotFound
	}
	if err != nil {
		return Region{}, fmt.Errorf("get provisioned region: %w", err)
	}
	items := []Region{item}
	if err := s.attachFacetNames(ctx, items); err != nil {
		return Region{}, err
	}
	return items[0], nil
}

// execer covers *sql.DB and *sql.Tx for the facet-name writes.
type execer interface {
	ExecContext(context.Context, string, ...any) (sql.Result, error)
}

func (s *PostgresStore) insertFacetNames(ctx context.Context, db execer, regionID string, names []string) error {
	for index, name := range names {
		if _, err := db.ExecContext(ctx, `INSERT INTO provisioned_region_facets
			(region_id, facet_index, name) VALUES ($1, $2, $3)`,
			regionID, index+1, name); err != nil {
			return err
		}
	}
	return nil
}

// attachFacetNames fills FacetNames for the given regions in one query.
func (s *PostgresStore) attachFacetNames(ctx context.Context, items []Region) error {
	if len(items) == 0 {
		return nil
	}
	byID := make(map[string]int, len(items))
	for index, item := range items {
		byID[item.ID] = index
	}
	rows, err := s.db.QueryContext(ctx, `SELECT region_id, name
		FROM provisioned_region_facets ORDER BY region_id, facet_index`)
	if err != nil {
		return fmt.Errorf("list provisioned region facets: %w", err)
	}
	defer rows.Close()
	for rows.Next() {
		var regionID, name string
		if err := rows.Scan(&regionID, &name); err != nil {
			return fmt.Errorf("scan provisioned region facet: %w", err)
		}
		if index, found := byID[regionID]; found {
			items[index].FacetNames = append(items[index].FacetNames, name)
		}
	}
	if err := rows.Err(); err != nil {
		return fmt.Errorf("iterate provisioned region facets: %w", err)
	}
	return nil
}

// checkNamesAcrossTables enforces the half of name uniqueness the two
// single-table indexes cannot: a region name may not match another region's
// facet name, and a facet name may not match another region's name. Runs
// inside the write transaction, so a losing race still lands on the indexes.
func (s *PostgresStore) checkNamesAcrossTables(ctx context.Context, tx *sql.Tx, item Region) error {
	for _, name := range allNames(item) {
		var collision bool
		err := tx.QueryRowContext(ctx, `SELECT EXISTS (
				SELECT 1 FROM provisioned_regions WHERE lower(name) = lower($1) AND id <> $2
				UNION ALL
				SELECT 1 FROM provisioned_region_facets WHERE lower(name) = lower($1) AND region_id <> $2
			)`, name, item.ID).Scan(&collision)
		if err != nil {
			return err
		}
		if collision {
			return fmt.Errorf("%w: name %q is already claimed by another region", ErrConflict, name)
		}
	}
	return nil
}

func (s *PostgresStore) inTransaction(ctx context.Context, work func(*sql.Tx) (Region, error)) (Region, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Region{}, err
	}
	item, err := work(tx)
	if err != nil {
		tx.Rollback()
		return Region{}, err
	}
	if err := tx.Commit(); err != nil {
		return Region{}, err
	}
	return item, nil
}

type rowScanner interface{ Scan(...any) error }

func scanRegion(row rowScanner) (Region, error) {
	var item Region
	var owner sql.NullString
	err := row.Scan(&item.ID, &item.Name, &owner, &item.MapX, &item.MapY, &item.SizeX, &item.SizeY, &item.Maturity,
		&item.PublicEndpoint, &item.ViewerPort, &item.Enabled, &item.Kind, &item.Tags)
	if owner.Valid {
		item.OwnerUserID = owner.String
	}
	return item, err
}

func classify(operation string, err error) error {
	if errors.Is(err, sql.ErrNoRows) {
		return ErrNotFound
	}
	if errors.Is(err, ErrConflict) || errors.Is(err, ErrInvalid) || errors.Is(err, ErrNotFound) {
		return err
	}
	var databaseError *pgconn.PgError
	if errors.As(err, &databaseError) && (databaseError.Code == "23505" || databaseError.Code == "23P01") {
		return fmt.Errorf("%w: %s", ErrConflict, operation)
	}
	if errors.As(err, &databaseError) && (databaseError.Code == "23503" || databaseError.Code == "23514") {
		return fmt.Errorf("%w: %s", ErrInvalid, operation)
	}
	return fmt.Errorf("%s: %w", operation, err)
}

func nullableOwner(owner string) any {
	if owner == "" {
		return nil
	}
	return owner
}

// regionKindOrDefault falls back to the "user" kind when none is supplied, so a
// row is never written with an empty kind.
func regionKindOrDefault(kind string) string {
	if strings.TrimSpace(kind) == "" {
		return "user"
	}
	return kind
}

func normalize(value string) string { return strings.TrimSpace(value) }
