// Package eventlog records what happened on the grid, with the time it
// happened, so questions about a period of time have an answer.
//
// The rest of the grid's storage is state: users says how many accounts exist,
// presence says who is in-world at this second and forgets them when they
// leave, regions holds a lease that expires. None of it can say how many
// people logged in last month, which is precisely what a public statistics
// page reports.
//
// Recording is best-effort at every call site (see Note): a login that
// succeeded must not fail because a log row could not be written, and a
// statistic is worth less than the thing it counts. What must not happen is a
// silent miss, so a failed write is logged as an error.
//
// The store's own tests need a database and skip without one, so `go test
// ./...` printing `ok` for this package means they did not run:
//
//	HOMEWORLDZ_TEST_DATABASE_URL=postgres://…/homeworldz go test ./internal/eventlog/
//
// Worth saying out loud because a skip and a pass are the same word here.
package eventlog

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"log/slog"
	"time"
)

// Kind names an event type. New kinds are added here rather than in the
// schema: the column is free text under a length cap, so recording a kind
// never waits on a migration.
type Kind string

const (
	// KindLogin is a viewer login that resolved to a region — the avatar
	// entered the world, not merely that a password matched.
	KindLogin Kind = "login"
	// KindLogout is the region reporting an avatar gone, whether the viewer
	// logged out or the session was retired.
	KindLogout Kind = "logout"
	// KindRegistration is a new account created on the website. Migration 34
	// backfilled one per existing user from its created_at.
	KindRegistration Kind = "registration"
	// KindTeleport is an avatar transit the region attributed to a teleport.
	KindTeleport Kind = "teleport"
	// KindCrossing is an avatar transit the region attributed to walking over
	// a region border. Separate from KindTeleport because a busy border
	// crossing would otherwise read as heavy teleport traffic.
	KindCrossing Kind = "crossing"
	// KindTransit is a transit whose origin the region did not state — an
	// older region binary, which predates the kind field. It is neither
	// counted as a teleport nor discarded.
	KindTransit Kind = "transit"
	// KindGridStart is the grid service starting. Uptime is measured from the
	// most recent one, which is why it survives a restart of the website API
	// (a different process) and of anything else that reports it.
	KindGridStart Kind = "grid_start"
)

// ErrNoEvent reports that no event of the requested kind has ever been
// recorded, which is not a failure: a grid whose log predates the event has
// nothing to say rather than a zero to state.
var ErrNoEvent = errors.New("no event of this kind has been recorded")

// Event is one thing that happened. Only Kind is required; a grid start names
// no user, a registration names no region.
type Event struct {
	Kind     Kind
	UserID   string
	RegionID string
	Detail   string
}

// Recorder writes events. Narrow on purpose: handlers that record events have
// no business reading counts.
type Recorder interface {
	Record(ctx context.Context, event Event) error
}

// Counter answers the questions a statistics page asks.
type Counter interface {
	// CountSince counts events of a kind at or after the instant given.
	CountSince(ctx context.Context, kind Kind, since time.Time) (int, error)
	// DistinctUsersSince counts the distinct users named by events of a kind
	// at or after the instant given. Rows naming no user are ignored, so an
	// account deleted since its login stops being counted as a person rather
	// than becoming an anonymous one — SQL gives that for free, see the
	// implementation.
	DistinctUsersSince(ctx context.Context, kind Kind, since time.Time) (int, error)
	// LatestAt reports when an event of the kind last happened, or
	// ErrNoEvent.
	LatestAt(ctx context.Context, kind Kind) (time.Time, error)
}

// Store is both halves, satisfied by *PostgresStore.
type Store interface {
	Recorder
	Counter
}

type PostgresStore struct{ db *sql.DB }

func NewPostgresStore(db *sql.DB) *PostgresStore { return &PostgresStore{db: db} }

func (s *PostgresStore) Record(ctx context.Context, event Event) error {
	if event.Kind == "" {
		return errors.New("event kind is empty")
	}
	_, err := s.db.ExecContext(ctx, `
        INSERT INTO event_log (kind, user_id, region_id, detail)
        VALUES ($1, $2, $3, $4)`,
		string(event.Kind), nullableUUID(event.UserID), nullableUUID(event.RegionID),
		nullableText(event.Detail))
	if err != nil {
		return fmt.Errorf("record %s event: %w", event.Kind, err)
	}
	return nil
}

func (s *PostgresStore) CountSince(ctx context.Context, kind Kind, since time.Time) (int, error) {
	var count int
	err := s.db.QueryRowContext(ctx,
		"SELECT COUNT(*) FROM event_log WHERE kind = $1 AND occurred_at >= $2",
		string(kind), since.UTC()).Scan(&count)
	if err != nil {
		return 0, fmt.Errorf("count %s events: %w", kind, err)
	}
	return count, nil
}

// The `user_id IS NOT NULL` predicate is not what excludes deleted accounts
// from the count — COUNT(DISTINCT …) ignores nulls on its own, and removing
// the predicate changes no answer (checked, 2026-08-24, by removing it and
// watching the test still pass). It is here to match the partial index
// event_log_kind_user_occurred_at_idx, which is declared over the same
// condition, and for nothing else.
func (s *PostgresStore) DistinctUsersSince(ctx context.Context, kind Kind, since time.Time) (int, error) {
	var count int
	err := s.db.QueryRowContext(ctx, `
        SELECT COUNT(DISTINCT user_id) FROM event_log
        WHERE kind = $1 AND occurred_at >= $2 AND user_id IS NOT NULL`,
		string(kind), since.UTC()).Scan(&count)
	if err != nil {
		return 0, fmt.Errorf("count distinct %s users: %w", kind, err)
	}
	return count, nil
}

func (s *PostgresStore) LatestAt(ctx context.Context, kind Kind) (time.Time, error) {
	var at time.Time
	err := s.db.QueryRowContext(ctx,
		"SELECT occurred_at FROM event_log WHERE kind = $1 ORDER BY occurred_at DESC LIMIT 1",
		string(kind)).Scan(&at)
	if errors.Is(err, sql.ErrNoRows) {
		return time.Time{}, ErrNoEvent
	}
	if err != nil {
		return time.Time{}, fmt.Errorf("find latest %s event: %w", kind, err)
	}
	return at.UTC(), nil
}

// Note records an event without letting the recording affect the caller: it
// takes its own deadline detached from the request's, because the events worth
// recording are often recorded as a request finishes — a logout hands its last
// act to a context that is about to be cancelled, and an insert cancelled
// halfway is a lost event, not a slow one.
//
// A nil recorder is a deployment without an event log (tools, tests), not an
// error. A failed write is logged: the whole point of the log is that a miss
// is visible.
func Note(ctx context.Context, recorder Recorder, logger *slog.Logger, event Event) {
	if recorder == nil {
		return
	}
	detached, cancel := context.WithTimeout(context.WithoutCancel(ctx), 5*time.Second)
	defer cancel()
	if err := recorder.Record(detached, event); err != nil {
		if logger == nil {
			logger = slog.Default()
		}
		logger.Error("record grid event", "kind", string(event.Kind), "error", err)
	}
}

func nullableUUID(value string) any {
	if value == "" {
		return nil
	}
	return value
}

func nullableText(value string) any {
	if value == "" {
		return nil
	}
	if len(value) > 512 {
		value = value[:512]
	}
	return value
}
