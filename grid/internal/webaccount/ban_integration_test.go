package webaccount

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"os"
	"testing"
	"time"

	_ "github.com/jackc/pgx/v5/stdlib"
)

// TestBannedAccountCannotSignIn is the website half of what a ban means.
//
// Banning bumped the authorization version, which invalidated the tokens the
// account already held, and left the login door open: it would sign straight
// back in and be issued a fresh token. The invalidation looked like enforcement
// because the person was logged out at the moment the ban landed.
//
// The refusal is deliberately placed after the bcrypt comparison. A wrong
// password on a banned account still answers wrong-password, so this cannot be
// turned into a way to enumerate who is banned without holding their password.
func TestBannedAccountCannotSignIn(t *testing.T) {
	databaseURL := os.Getenv("HOMEWORLDZ_TEST_DATABASE_URL")
	if databaseURL == "" {
		t.Skip("HOMEWORLDZ_TEST_DATABASE_URL is not configured")
	}
	db, err := sql.Open("pgx", databaseURL)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()

	store := NewPostgresStore(db)
	stamp := time.Now().UnixNano()
	displayName := fmt.Sprintf("Ban Test%d", stamp)
	const password = "integration-password"

	account, code, err := store.Register(ctx, displayName, fmt.Sprintf("ban.%d@example.invalid", stamp))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _, _ = db.Exec("DELETE FROM users WHERE id = $1", account.ID) })
	if _, err := store.Verify(ctx, code, password); err != nil {
		t.Fatal(err)
	}

	if _, err := store.Authenticate(ctx, account.Userid, password); err != nil {
		t.Fatalf("an unbanned account could not sign in: %v", err)
	}

	if _, err := store.Ban(ctx, account.ID, "integration test", nil, account.ID); err != nil {
		t.Fatal(err)
	}

	if _, err := store.Authenticate(ctx, account.Userid, password); !errors.Is(err, ErrBanned) {
		t.Fatalf("banned sign-in = %v, want ErrBanned", err)
	}
	if _, err := store.Authenticate(ctx, account.Userid, "wrong"); !errors.Is(err, ErrInvalidCredentials) {
		t.Fatalf("wrong password on a banned account = %v, want ErrInvalidCredentials", err)
	}

	// Unban restores access. Without this the refusal above could be coming
	// from anything the ban row touched, not from the ban being active.
	if _, err := store.Unban(ctx, account.ID); err != nil {
		t.Fatal(err)
	}
	if _, err := store.Authenticate(ctx, account.Userid, password); err != nil {
		t.Fatalf("an unbanned account was still refused: %v", err)
	}

	// A ban that has run out is not a ban, and nobody has to delete a row for
	// that to be true.
	if _, err := store.Ban(ctx, account.ID, "expiring", ptr(time.Now().Add(-time.Minute)), account.ID); err != nil {
		t.Fatal(err)
	}
	if _, err := store.Authenticate(ctx, account.Userid, password); err != nil {
		t.Fatalf("an expired ban still refused sign-in: %v", err)
	}
}

func ptr(t time.Time) *time.Time { return &t }
