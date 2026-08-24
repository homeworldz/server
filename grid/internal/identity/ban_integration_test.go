package identity

import (
	"context"
	"crypto/md5"
	"database/sql"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"testing"
	"time"

	_ "github.com/jackc/pgx/v5/stdlib"
)

// viewerPasswordHash is what a viewer sends: the MD5 of the password, which is
// the legacy protocol and not this store's choice.
func viewerPasswordHash(password string) string {
	digest := md5.Sum([]byte(password))
	return hex.EncodeToString(digest[:])
}

// TestBannedAccountCannotOpenAViewerSession covers what a ban is for.
//
// Until 2026-08-24 a ban bumped the account's authorization version, which
// invalidated the website tokens it already held, and stopped there: viewer
// login asked for a username and a password hash and nothing else, so a banned
// account could log straight back in through Firestorm. Ending sessions is not
// denying access.
//
// The ordering matters as much as the refusal: a wrong password on a banned
// account must still answer wrong-password, or this becomes a way to find out
// who is banned without holding anybody's credentials.
func TestBannedAccountCannotOpenAViewerSession(t *testing.T) {
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

	store := NewPostgresStore(db)
	username := fmt.Sprintf("ban.%d", time.Now().UnixNano())
	const password = "integration-password"
	user, err := store.CreateUser(ctx, username, password)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _, _ = db.Exec("DELETE FROM users WHERE id = $1", user.ID) })

	hash := viewerPasswordHash(password)
	if _, err := store.CreateViewerSession(ctx, username, hash, time.Minute); err != nil {
		t.Fatalf("an unbanned account could not log in: %v", err)
	}

	if _, err := db.ExecContext(ctx,
		"INSERT INTO account_bans (user_id, reason, banned_by) VALUES ($1, $2, $1)",
		user.ID, "integration test"); err != nil {
		t.Fatal(err)
	}

	if _, err := store.CreateViewerSession(ctx, username, hash, time.Minute); !errors.Is(err, ErrBanned) {
		t.Fatalf("banned login = %v, want ErrBanned", err)
	}
	// A wrong password is still a wrong password, not a ban notice.
	if _, err := store.CreateViewerSession(ctx, username, viewerPasswordHash("wrong"), time.Minute); !errors.Is(err, ErrInvalidCredentials) {
		t.Fatalf("wrong password on a banned account = %v, want ErrInvalidCredentials", err)
	}

	// An expired ban is not a ban. Lifting one has to restore access without
	// anybody deleting a row by hand.
	if _, err := db.ExecContext(ctx,
		"UPDATE account_bans SET expires_at = now() - interval '1 minute' WHERE user_id = $1",
		user.ID); err != nil {
		t.Fatal(err)
	}
	if _, err := store.CreateViewerSession(ctx, username, hash, time.Minute); err != nil {
		t.Fatalf("an expired ban still refused login: %v", err)
	}
}
