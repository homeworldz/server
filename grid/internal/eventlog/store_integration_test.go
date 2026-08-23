package eventlog

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"os"
	"testing"
	"time"

	"github.com/homeworldz/server/grid/internal/identity"
	_ "github.com/jackc/pgx/v5/stdlib"
)

// TestPostgresEventLog exercises the three questions the statistics page asks
// against real SQL: how many of a kind, how many distinct people, and when the
// last one happened.
func TestPostgresEventLog(t *testing.T) {
	databaseURL := os.Getenv("HOMEWORLDZ_TEST_DATABASE_URL")
	if databaseURL == "" {
		t.Skip("HOMEWORLDZ_TEST_DATABASE_URL is not configured")
	}
	db, err := sql.Open("pgx", databaseURL)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()

	nonce := time.Now().UnixNano()
	users := identity.NewPostgresStore(db)
	first, err := users.CreateUser(ctx, fmt.Sprintf("eventlog.a.%d", nonce), "integration-password")
	if err != nil {
		t.Fatal(err)
	}
	second, err := users.CreateUser(ctx, fmt.Sprintf("eventlog.b.%d", nonce), "integration-password")
	if err != nil {
		t.Fatal(err)
	}
	// The users are deleted last, which also removes their backfilled
	// registration rows; the login rows below are removed by id.
	t.Cleanup(func() {
		_, _ = db.Exec("DELETE FROM users WHERE id = ANY($1)",
			[]string{first.ID, second.ID})
	})

	store := NewPostgresStore(db)
	// A kind nobody else writes, so the counts below are this test's rows and
	// not whatever the grid recorded while the test ran.
	kind := Kind(fmt.Sprintf("test_%d", nonce%100000))
	t.Cleanup(func() { _, _ = db.Exec("DELETE FROM event_log WHERE kind = $1", string(kind)) })

	if _, err := store.LatestAt(ctx, kind); !errors.Is(err, ErrNoEvent) {
		t.Fatalf("latest of an unrecorded kind = %v, want ErrNoEvent", err)
	}

	start := time.Now().UTC().Add(-time.Minute)
	for _, event := range []Event{
		{Kind: kind, UserID: first.ID, Detail: "one"},
		{Kind: kind, UserID: first.ID, Detail: "two"},
		{Kind: kind, UserID: second.ID},
		{Kind: kind},
	} {
		if err := store.Record(ctx, event); err != nil {
			t.Fatal(err)
		}
	}

	count, err := store.CountSince(ctx, kind, start)
	if err != nil {
		t.Fatal(err)
	}
	if count != 4 {
		t.Fatalf("count = %d, want 4", count)
	}
	// Three rows name a user, two people: the row naming nobody is not a third.
	people, err := store.DistinctUsersSince(ctx, kind, start)
	if err != nil {
		t.Fatal(err)
	}
	if people != 2 {
		t.Fatalf("distinct users = %d, want 2", people)
	}
	// A window that opens after the events were recorded sees none of them.
	later, err := store.CountSince(ctx, kind, time.Now().UTC().Add(time.Minute))
	if err != nil {
		t.Fatal(err)
	}
	if later != 0 {
		t.Fatalf("count after the window = %d, want 0", later)
	}
	latest, err := store.LatestAt(ctx, kind)
	if err != nil {
		t.Fatal(err)
	}
	if latest.Before(start) {
		t.Fatalf("latest = %s, want at or after %s", latest, start)
	}

	// Deleting the account keeps the rows and drops the name, so the person
	// stops being counted rather than becoming an anonymous one.
	if _, err := db.ExecContext(ctx, "DELETE FROM users WHERE id = $1", second.ID); err != nil {
		t.Fatal(err)
	}
	people, err = store.DistinctUsersSince(ctx, kind, start)
	if err != nil {
		t.Fatal(err)
	}
	if people != 1 {
		t.Fatalf("distinct users after deletion = %d, want 1", people)
	}
	count, err = store.CountSince(ctx, kind, start)
	if err != nil {
		t.Fatal(err)
	}
	if count != 4 {
		t.Fatalf("count after deletion = %d, want 4", count)
	}
}
