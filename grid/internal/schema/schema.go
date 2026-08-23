// Package schema checks at startup that the database carries the schema this
// binary was built against.
//
// It exists because nothing reported the alternative. Migration 30 was written
// on 31 July and the code that needed it was deployed the same day, but the
// migration was never applied; the grid ran for five days at version 29 serving
// code that expected 30, and the only symptom was a feature quietly not working.
// Every query that needed the new column failed one request at a time, deep in a
// handler, where the error read as a bug in the feature rather than a missing
// migration.
//
// A version mismatch is therefore fatal at startup rather than diagnosed later:
// a process that refuses to start is impossible to miss, and a half-migrated
// grid has no correct behaviour to offer anyway.
package schema

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"log/slog"
)

// Required is the highest migration version in db/migrations that this build
// expects the database to have applied.
//
// Bump it in the same commit that adds a migration. TestRequiredMatchesMigrations
// fails if the two ever disagree, so this cannot drift silently — which is the
// whole point of stating it here rather than reading the directory at runtime: a
// deployed binary has no migrations directory to read, and the number it was
// built against is exactly what it needs to assert.
const Required = 34

// ErrNotInitialized reports that schema_metadata itself is absent, which means
// no migration has ever run against this database.
var ErrNotInitialized = errors.New("schema_metadata is missing; the database has never been migrated")

// Verify compares the database's applied schema version against Required.
//
// A database behind this build is an error: the code will reference tables and
// columns that are not there. A database ahead of it is not — that is a
// deliberately rolled-back binary, and newer schema is additive by convention —
// so it is logged and allowed. The logger may be nil.
func Verify(ctx context.Context, db *sql.DB, logger *slog.Logger) error {
	var current int
	err := db.QueryRowContext(ctx,
		`SELECT COALESCE(MAX(version), 0) FROM schema_metadata`).Scan(&current)
	if err != nil {
		return classify(err)
	}

	return check(current, logger)
}

// classify separates "this database was never migrated" from every other query
// failure, split out so the distinction is testable without a database — it is
// otherwise the sort of branch that first runs on the day it is needed.
func classify(err error) error {
	// pgx surfaces *pgconn.PgError, matched by behaviour rather than type so this
	// does not depend on the driver package.
	var pgErr interface{ SQLState() string }
	if errors.As(err, &pgErr) && pgErr.SQLState() == undefinedTable {
		return ErrNotInitialized
	}
	return fmt.Errorf("read schema version: %w", err)
}

// undefinedTable is PostgreSQL's SQLSTATE for a missing relation.
const undefinedTable = "42P01"

// check is the decision Verify makes once it has a version, split out so the
// three outcomes are testable without a database.
func check(current int, logger *slog.Logger) error {
	switch {
	case current < Required:
		return fmt.Errorf(
			"database schema is at version %d but this build requires %d: apply the pending migrations (go run ./grid/cmd/dbmigrate) and start again",
			current, Required)
	case current > Required:
		if logger != nil {
			logger.Warn("database schema is newer than this build",
				"database_version", current, "build_requires", Required)
		}
	}
	return nil
}
