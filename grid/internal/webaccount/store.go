// Package webaccount is the data layer for the browser-facing website API:
// email-verified avatar registration, self-service account management, and
// privileged user/ban administration. It operates on the shared users table
// (the same identities used for viewer login) plus the account_verifications
// and account_bans tables added in migration 000018.
package webaccount

import (
	"context"
	"crypto/md5"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"database/sql"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/homeworldz/server/grid/internal/identifier"
	"github.com/jackc/pgx/v5/pgconn"
	"golang.org/x/crypto/bcrypt"
)

var (
	// ErrConflict indicates the derived userid is already registered.
	ErrConflict = errors.New("userid is already registered")
	// ErrDisplayNameTaken indicates a display name whose normalized form already
	// matches another account's display name or userid.
	ErrDisplayNameTaken = errors.New("display name is already in use")
	// ErrNotFound indicates no such account.
	ErrNotFound = errors.New("account not found")
	// ErrInvalidCredentials indicates a failed login or an unverified account.
	ErrInvalidCredentials = errors.New("invalid credentials")
	// ErrWrongPassword indicates the supplied current password did not match.
	ErrWrongPassword = errors.New("current password is incorrect")
	// ErrInvalidCode indicates a missing, expired, or already-consumed code.
	ErrInvalidCode = errors.New("invalid or expired confirmation code")
	// ErrInvalidEmail indicates a malformed contact email.
	ErrInvalidEmail = errors.New("email is invalid")
	// ErrAlreadyVerified indicates the account is already verified.
	ErrAlreadyVerified = errors.New("account is already verified")
	// ErrLastSuper indicates an attempt to demote the final super account.
	ErrLastSuper = errors.New("the final super account cannot be demoted")
	// ErrBanned indicates correct credentials for a banned account.
	//
	// Distinct from ErrInvalidCredentials on purpose. Banning bumped the
	// account's authorization version, which invalidates the tokens it already
	// held — and nothing then stopped it signing in again a second later,
	// because authentication asked only for a password and a verified address
	// (found by an audit, 2026-08-24). Ending sessions is not denying access.
	//
	// A banned person is told they are banned rather than told their password
	// is wrong: they know they have been banned, the pretence only costs them
	// a support ticket, and a wrong-password answer here would send an honest
	// operator hunting a credential fault that does not exist.
	ErrBanned = errors.New("account is banned")
)

// AccountState values for ManagedAccount.State.
const (
	StateActive = "active"
	StateBanned = "banned"
)

const defaultVerificationTTL = 24 * time.Hour

// Shorter than verification deliberately: a reset token opens an account, while
// a verification code only confirms an address. An hour costs a resend at worst
// and narrows the window in which a later-read mailbox is enough to take over.
const defaultPasswordResetTTL = time.Hour

// Account is the website identity of an avatar.
type Account struct {
	ID          string
	Userid      string
	DisplayName string
	// Email is the address on file. It reaches a browser only through the
	// account's own session or an administrator's listing; nothing serves it to
	// a third party.
	Email       string
	RezDate     time.Time
	Privileges  string
	AuthVersion int
	Verified    bool
	Kind        string
	Tags        string
}

// Ban describes an account suspension.
type Ban struct {
	Reason    string
	ExpiresAt *time.Time
	BannedAt  time.Time
	BannedBy  string
}

// ManagedAccount is an Account with administrative state.
type ManagedAccount struct {
	Account
	State string
	Ban   *Ban
}

// InventoryProvisioner provisions any required initial inventory for a
// newly verified account. It is an optional seam: when set on the store it is
// invoked after verification. It is currently left unset until the inventory
// provisioning entry point is wired in, so verification does not yet create
// starter inventory.
type InventoryProvisioner interface {
	ProvisionInitialInventory(ctx context.Context, userID string) error
}

// PostgresStore implements the website account operations against Postgres.
type PostgresStore struct {
	db               *sql.DB
	verificationTTL  time.Duration
	passwordResetTTL time.Duration
	provisioner      InventoryProvisioner
}

// NewPostgresStore returns a store using the default verification lifetime.
func NewPostgresStore(db *sql.DB) *PostgresStore {
	return &PostgresStore{db: db, verificationTTL: defaultVerificationTTL,
		passwordResetTTL: defaultPasswordResetTTL}
}

// WithInventoryProvisioner sets the optional initial-inventory seam.
func (s *PostgresStore) WithInventoryProvisioner(p InventoryProvisioner) *PostgresStore {
	s.provisioner = p
	return s
}

const accountColumns = "id, username, display_name, email, created_at, privileges, auth_version, verified_at, kind, tags"

type rowScanner interface {
	Scan(dest ...any) error
}

func scanAccount(row rowScanner) (Account, error) {
	var (
		account     Account
		displayName sql.NullString
		email       sql.NullString
		verifiedAt  sql.NullTime
	)
	if err := row.Scan(&account.ID, &account.Userid, &displayName, &email, &account.RezDate,
		&account.Privileges, &account.AuthVersion, &verifiedAt, &account.Kind, &account.Tags); err != nil {
		return Account{}, err
	}
	account.Email = email.String
	account.DisplayName = displayName.String
	if account.DisplayName == "" {
		account.DisplayName = account.Userid
	}
	account.Verified = verifiedAt.Valid
	return account, nil
}

// Register derives the userid from displayName, creates an unverified account,
// and stores a single-use confirmation code. It returns the created account and
// the plaintext code (delivered by email exactly once).
func (s *PostgresStore) Register(ctx context.Context, displayName, email string) (Account, string, error) {
	userid, err := ValidateDisplayName(displayName)
	if err != nil {
		return Account{}, "", err
	}
	email = strings.TrimSpace(email)
	if err := validateEmail(email); err != nil {
		return Account{}, "", err
	}
	id, err := identifier.NewUUID()
	if err != nil {
		return Account{}, "", err
	}
	code, err := newConfirmationCode()
	if err != nil {
		return Account{}, "", err
	}
	codeHash := hashCode(code)

	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Account{}, "", fmt.Errorf("begin registration: %w", err)
	}
	defer tx.Rollback() //nolint:errcheck // no-op after Commit

	var account Account
	row := tx.QueryRowContext(ctx, `
		INSERT INTO users (id, username, display_name, email, privileges)
		VALUES ($1, $2, $3, $4, '')
		RETURNING `+accountColumns, id, userid, strings.TrimSpace(displayName), email)
	account, err = scanAccount(row)
	var pgErr *pgconn.PgError
	if errors.As(err, &pgErr) && pgErr.Code == "23505" {
		return Account{}, "", ErrConflict
	}
	if err != nil {
		return Account{}, "", fmt.Errorf("create account: %w", err)
	}
	if _, err := tx.ExecContext(ctx, `
		INSERT INTO account_verifications (user_id, code_hash, email, expires_at)
		VALUES ($1, $2, $3, now() + $4 * interval '1 second')`,
		id, codeHash, email, int64(s.verificationTTL/time.Second)); err != nil {
		return Account{}, "", fmt.Errorf("create verification: %w", err)
	}
	if err := tx.Commit(); err != nil {
		return Account{}, "", fmt.Errorf("commit registration: %w", err)
	}
	return account, code, nil
}

// Verify consumes a confirmation code, sets the password (both representations),
// marks the account verified, and returns the verified account.
func (s *PostgresStore) Verify(ctx context.Context, code, password string) (Account, error) {
	codeHash := hashCode(code)
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return Account{}, fmt.Errorf("begin verification: %w", err)
	}
	defer tx.Rollback() //nolint:errcheck

	var (
		userID    string
		expiresAt time.Time
	)
	err = tx.QueryRowContext(ctx,
		"SELECT user_id, expires_at FROM account_verifications WHERE code_hash = $1 FOR UPDATE",
		codeHash).Scan(&userID, &expiresAt)
	if errors.Is(err, sql.ErrNoRows) {
		return Account{}, ErrInvalidCode
	}
	if err != nil {
		return Account{}, fmt.Errorf("find verification: %w", err)
	}
	if time.Now().After(expiresAt) {
		if _, delErr := tx.ExecContext(ctx, "DELETE FROM account_verifications WHERE user_id = $1", userID); delErr != nil {
			return Account{}, fmt.Errorf("discard expired verification: %w", delErr)
		}
		if err := tx.Commit(); err != nil {
			return Account{}, fmt.Errorf("commit expired verification: %w", err)
		}
		return Account{}, ErrInvalidCode
	}
	bcryptHash, viewerHash, err := hashPassword(password)
	if err != nil {
		return Account{}, err
	}
	row := tx.QueryRowContext(ctx, `
		UPDATE users
		SET password_hash = $2, viewer_password_hash = $3, verified_at = now()
		WHERE id = $1 AND verified_at IS NULL
		RETURNING `+accountColumns, userID, bcryptHash, viewerHash)
	account, err := scanAccount(row)
	if errors.Is(err, sql.ErrNoRows) {
		return Account{}, ErrInvalidCode
	}
	if err != nil {
		return Account{}, fmt.Errorf("verify account: %w", err)
	}
	if _, err := tx.ExecContext(ctx, "DELETE FROM account_verifications WHERE user_id = $1", userID); err != nil {
		return Account{}, fmt.Errorf("consume verification: %w", err)
	}
	if err := tx.Commit(); err != nil {
		return Account{}, fmt.Errorf("commit verification: %w", err)
	}
	if s.provisioner != nil {
		if err := s.provisioner.ProvisionInitialInventory(ctx, userID); err != nil {
			return Account{}, fmt.Errorf("provision initial inventory: %w", err)
		}
	}
	return account, nil
}

// ResendVerification issues a fresh code for an unverified account and returns
// the plaintext code and contact email. It returns ErrNotFound when no such
// unverified account exists (the handler still responds 202 to avoid disclosing
// account state).
func (s *PostgresStore) ResendVerification(ctx context.Context, userid string) (string, string, error) {
	code, err := newConfirmationCode()
	if err != nil {
		return "", "", err
	}
	codeHash := hashCode(code)
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return "", "", fmt.Errorf("begin resend: %w", err)
	}
	defer tx.Rollback() //nolint:errcheck

	var (
		userID     string
		email      sql.NullString
		verifiedAt sql.NullTime
	)
	err = tx.QueryRowContext(ctx,
		"SELECT id, email, verified_at FROM users WHERE username = $1 FOR UPDATE",
		userid).Scan(&userID, &email, &verifiedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return "", "", ErrNotFound
	}
	if err != nil {
		return "", "", fmt.Errorf("find account: %w", err)
	}
	if verifiedAt.Valid {
		return "", "", ErrAlreadyVerified
	}
	if !email.Valid || email.String == "" {
		return "", "", ErrNotFound
	}
	if _, err := tx.ExecContext(ctx, `
		INSERT INTO account_verifications (user_id, code_hash, email, expires_at)
		VALUES ($1, $2, $3, now() + $4 * interval '1 second')
		ON CONFLICT (user_id) DO UPDATE
		SET code_hash = EXCLUDED.code_hash, email = EXCLUDED.email,
		    expires_at = EXCLUDED.expires_at, created_at = now()`,
		userID, codeHash, email.String, int64(s.verificationTTL/time.Second)); err != nil {
		return "", "", fmt.Errorf("refresh verification: %w", err)
	}
	if err := tx.Commit(); err != nil {
		return "", "", fmt.Errorf("commit resend: %w", err)
	}
	return code, email.String, nil
}

// RequestPasswordReset issues a single-use reset token for the account matching
// ident, which may be a username or the email address on file.
//
// The token is returned with the address to send it to, and the caller mails it.
// A missing account, an unverified one, or one with no address on file all yield
// ErrNotFound — and the handler must answer the caller identically in every case
// including success, or this endpoint becomes a way to discover which accounts
// exist (ADR 0034).
//
// Any pending reset for the same account is replaced rather than added to, so a
// person clicking "forgot password" twice has one live token rather than two.
func (s *PostgresStore) RequestPasswordReset(ctx context.Context, ident string) (string, string, error) {
	if strings.TrimSpace(ident) == "" {
		return "", "", ErrNotFound
	}
	token, err := newConfirmationCode()
	if err != nil {
		return "", "", err
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return "", "", fmt.Errorf("begin password reset: %w", err)
	}
	defer tx.Rollback() //nolint:errcheck

	var (
		userID     string
		email      sql.NullString
		verifiedAt sql.NullTime
	)
	// The same identifier login accepts, resolved the same way: the userid.
	//
	// Email is deliberately not among them. Nothing constrains it to one account
	// — two avatars may legitimately share an address — and this query took the
	// first arbitrary row when more than one matched, with no ORDER BY to make
	// even that repeatable. On 2026-08-06 an address supplied here reset a
	// different account from the one intended, and the reply is identical for
	// every outcome by design, so nothing said so. An address still receives the
	// mail; it just cannot choose whose password changes.
	key := DeriveUserid(ident)
	if key == "" {
		return "", "", ErrNotFound
	}
	// An account that has never been verified has no password to reset and no
	// confirmed address to send to, so it is not eligible.
	err = tx.QueryRowContext(ctx, `
		SELECT id, email, verified_at FROM users
		WHERE username = $1
		FOR UPDATE`, key).Scan(&userID, &email, &verifiedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return "", "", ErrNotFound
	}
	if err != nil {
		return "", "", fmt.Errorf("find account for reset: %w", err)
	}
	if !verifiedAt.Valid || !email.Valid || email.String == "" {
		return "", "", ErrNotFound
	}
	if _, err := tx.ExecContext(ctx, `
		INSERT INTO account_password_resets (user_id, token_hash, email, expires_at)
		VALUES ($1, $2, $3, now() + $4 * interval '1 second')
		ON CONFLICT (user_id) DO UPDATE
		SET token_hash = EXCLUDED.token_hash, email = EXCLUDED.email,
		    expires_at = EXCLUDED.expires_at, used_at = NULL, created_at = now()`,
		userID, hashCode(token), email.String,
		int64(s.passwordResetTTL/time.Second)); err != nil {
		return "", "", fmt.Errorf("store reset token: %w", err)
	}
	if err := tx.Commit(); err != nil {
		return "", "", fmt.Errorf("commit password reset: %w", err)
	}
	return token, email.String, nil
}

// ConsumePasswordReset sets a new password for the account holding token.
//
// This is a password change with a different proof of identity: the token stands
// in for knowing the current password, and after it is accepted the operation is
// the same one — write the digests, touch nothing else. Sessions are deliberately
// left alone (ADR 0034).
//
// Unknown, already-used and expired tokens are one error, ErrInvalidCode,
// because telling them apart would say whether a token had ever existed.
func (s *PostgresStore) ConsumePasswordReset(ctx context.Context, token, newPassword string) error {
	// Length is validated by the handler, as it is for ChangePassword.
	if strings.TrimSpace(token) == "" {
		return ErrInvalidCode
	}
	bcryptHash, viewerHash, err := hashPassword(newPassword)
	if err != nil {
		return err
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return fmt.Errorf("begin consume reset: %w", err)
	}
	defer tx.Rollback() //nolint:errcheck

	var userID string
	// Unused and unexpired, decided in the query so a stale token cannot be
	// spent by a race between the check and the write.
	err = tx.QueryRowContext(ctx, `
		SELECT user_id FROM account_password_resets
		WHERE token_hash = $1 AND used_at IS NULL AND expires_at > now()
		FOR UPDATE`, hashCode(token)).Scan(&userID)
	if errors.Is(err, sql.ErrNoRows) {
		return ErrInvalidCode
	}
	if err != nil {
		return fmt.Errorf("find reset token: %w", err)
	}
	if _, err := tx.ExecContext(ctx, `
		UPDATE users SET password_hash = $2, viewer_password_hash = $3
		WHERE id = $1`, userID, bcryptHash, viewerHash); err != nil {
		return fmt.Errorf("write reset password: %w", err)
	}
	// Marked rather than deleted, so a second click on the same link is refused
	// for the right reason instead of looking like an unknown token.
	if _, err := tx.ExecContext(ctx, `
		UPDATE account_password_resets SET used_at = now() WHERE user_id = $1`,
		userID); err != nil {
		return fmt.Errorf("mark reset used: %w", err)
	}
	if err := tx.Commit(); err != nil {
		return fmt.Errorf("commit consume reset: %w", err)
	}
	return nil
}

// Authenticate verifies a password for a verified account and returns it. An
// unverified account or a wrong password both yield ErrInvalidCredentials.
func (s *PostgresStore) Authenticate(ctx context.Context, ident, password string) (Account, error) {
	// The userid, and only the userid. DeriveUserid still normalizes, so "Jim
	// Tarber" resolves to jim.tarber — that is a lenient spelling of the userid,
	// not a second identifier. A display name that has since been changed no
	// longer logs in, which is the point: the userid is permanent and the display
	// name is not.
	key := DeriveUserid(ident)
	if key == "" {
		return Account{}, ErrInvalidCredentials
	}
	var passwordHash sql.NullString
	var id string
	var banned bool
	err := s.db.QueryRowContext(ctx,
		`SELECT u.id, u.password_hash,
		        EXISTS (SELECT 1 FROM account_bans b
		                WHERE b.user_id = u.id
		                  AND (b.expires_at IS NULL OR b.expires_at > now()))
		   FROM users u
		   WHERE u.username = $1 AND u.verified_at IS NOT NULL
		   LIMIT 1`,
		key).Scan(&id, &passwordHash, &banned)
	if errors.Is(err, sql.ErrNoRows) {
		return Account{}, ErrInvalidCredentials
	}
	if err != nil {
		return Account{}, fmt.Errorf("find credentials: %w", err)
	}
	if !passwordHash.Valid || bcrypt.CompareHashAndPassword([]byte(passwordHash.String), []byte(password)) != nil {
		return Account{}, ErrInvalidCredentials
	}
	// After the password, so a wrong password on a banned account still
	// answers wrong-password and this cannot be used to discover who is
	// banned without knowing their credentials.
	if banned {
		return Account{}, ErrBanned
	}
	return s.Get(ctx, id)
}

// Get returns an account by id.
func (s *PostgresStore) Get(ctx context.Context, id string) (Account, error) {
	row := s.db.QueryRowContext(ctx, "SELECT "+accountColumns+" FROM users WHERE id = $1", id)
	account, err := scanAccount(row)
	if errors.Is(err, sql.ErrNoRows) {
		return Account{}, ErrNotFound
	}
	if err != nil {
		return Account{}, fmt.Errorf("get account: %w", err)
	}
	return account, nil
}

// GetManaged returns an account with its administrative state.
func (s *PostgresStore) GetManaged(ctx context.Context, id string) (ManagedAccount, error) {
	account, err := s.Get(ctx, id)
	if err != nil {
		return ManagedAccount{}, err
	}
	ban, err := s.activeBan(ctx, id)
	if err != nil {
		return ManagedAccount{}, err
	}
	return toManaged(account, ban), nil
}

// SetClassification sets an account's kind and tags. The kind must be a
// recognized user kind; tags are normalized. Classification does not affect
// authorization, so the authorization version is left unchanged.
func (s *PostgresStore) SetClassification(ctx context.Context, id, kind, tags string) (ManagedAccount, error) {
	if !ValidUserKind(kind) {
		return ManagedAccount{}, ErrInvalidKind
	}
	normalized, err := NormalizeTags(tags)
	if err != nil {
		return ManagedAccount{}, err
	}
	result, err := s.db.ExecContext(ctx,
		"UPDATE users SET kind = $2, tags = $3 WHERE id = $1", id, kind, normalized)
	if err != nil {
		return ManagedAccount{}, fmt.Errorf("set classification: %w", err)
	}
	affected, err := result.RowsAffected()
	if err != nil {
		return ManagedAccount{}, fmt.Errorf("set classification: %w", err)
	}
	if affected == 0 {
		return ManagedAccount{}, ErrNotFound
	}
	return s.GetManaged(ctx, id)
}

// UpdateProfile updates the display name of an account and returns it. It does
// not change the userid, which is fixed at registration. The new display name
// must be two words yielding a valid userid form, and its normalized key must
// not collide with any other account's userid or normalized display name.
func (s *PostgresStore) UpdateProfile(ctx context.Context, id, displayName string) (Account, error) {
	key, err := ValidateDisplayName(displayName)
	if err != nil {
		return Account{}, err
	}
	name := strings.TrimSpace(displayName)

	// Reject a normalized display name that matches another account's userid or
	// normalized display name. The unique index on display_name_key is the
	// backstop against a concurrent writer racing between this check and the
	// UPDATE; it surfaces as a 23505 handled below.
	var taken bool
	if err := s.db.QueryRowContext(ctx,
		`SELECT EXISTS(SELECT 1 FROM users
		   WHERE id <> $1 AND (username = $2 OR display_name_key = $2))`,
		id, key).Scan(&taken); err != nil {
		return Account{}, fmt.Errorf("check display name: %w", err)
	}
	if taken {
		return Account{}, ErrDisplayNameTaken
	}

	row := s.db.QueryRowContext(ctx,
		"UPDATE users SET display_name = $2 WHERE id = $1 RETURNING "+accountColumns, id, name)
	account, err := scanAccount(row)
	if errors.Is(err, sql.ErrNoRows) {
		return Account{}, ErrNotFound
	}
	var pgErr *pgconn.PgError
	if errors.As(err, &pgErr) && pgErr.Code == "23505" {
		return Account{}, ErrDisplayNameTaken
	}
	if err != nil {
		return Account{}, fmt.Errorf("update profile: %w", err)
	}
	return account, nil
}

// ChangePassword verifies the current password, atomically replaces both
// password representations, and increments the authorization version so
// previously issued tokens stop authorizing requests.
func (s *PostgresStore) ChangePassword(ctx context.Context, id, currentPassword, newPassword string) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return fmt.Errorf("begin password change: %w", err)
	}
	defer tx.Rollback() //nolint:errcheck

	var current sql.NullString
	err = tx.QueryRowContext(ctx, "SELECT password_hash FROM users WHERE id = $1 FOR UPDATE", id).Scan(&current)
	if errors.Is(err, sql.ErrNoRows) {
		return ErrNotFound
	}
	if err != nil {
		return fmt.Errorf("find account: %w", err)
	}
	if !current.Valid || bcrypt.CompareHashAndPassword([]byte(current.String), []byte(currentPassword)) != nil {
		return ErrWrongPassword
	}
	bcryptHash, viewerHash, err := hashPassword(newPassword)
	if err != nil {
		return err
	}
	if _, err := tx.ExecContext(ctx, `
		UPDATE users SET password_hash = $2, viewer_password_hash = $3, auth_version = auth_version + 1
		WHERE id = $1`, id, bcryptHash, viewerHash); err != nil {
		return fmt.Errorf("change password: %w", err)
	}
	if err := tx.Commit(); err != nil {
		return fmt.Errorf("commit password change: %w", err)
	}
	return nil
}

// List returns a page of accounts filtered by an optional case-insensitive
// search over userid and display name, ordered by userid. The returned cursor,
// when non-empty, fetches the next page.
func (s *PostgresStore) List(ctx context.Context, search, cursor string, limit int) ([]ManagedAccount, string, error) {
	if limit < 1 {
		limit = 50
	}
	if limit > 100 {
		limit = 100
	}
	pattern := "%" + strings.ReplaceAll(strings.ReplaceAll(search, "%", `\%`), "_", `\_`) + "%"
	rows, err := s.db.QueryContext(ctx, `
		SELECT u.id, u.username, u.display_name, u.email, u.created_at, u.privileges, u.auth_version, u.verified_at, u.kind, u.tags,
		       b.reason, b.expires_at, b.banned_at, b.banned_by
		FROM users u
		LEFT JOIN account_bans b ON b.user_id = u.id
		WHERE ($1 = '' OR u.username ILIKE $2 OR COALESCE(u.display_name, '') ILIKE $2)
		  AND ($3 = '' OR u.username > $3)
		ORDER BY u.username ASC
		LIMIT $4`, search, pattern, cursor, limit+1)
	if err != nil {
		return nil, "", fmt.Errorf("list accounts: %w", err)
	}
	defer rows.Close()

	items := make([]ManagedAccount, 0, limit)
	for rows.Next() {
		account, ban, err := scanManagedRow(rows)
		if err != nil {
			return nil, "", err
		}
		items = append(items, toManaged(account, ban))
	}
	if err := rows.Err(); err != nil {
		return nil, "", fmt.Errorf("scan accounts: %w", err)
	}
	nextCursor := ""
	if len(items) > limit {
		nextCursor = items[limit-1].Userid
		items = items[:limit]
	}
	return items, nextCursor, nil
}

// ReplacePrivileges sets the normalized privileges of an account and increments
// its authorization version. It refuses to demote the final super account.
func (s *PostgresStore) ReplacePrivileges(ctx context.Context, id, privs string) (ManagedAccount, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return ManagedAccount{}, fmt.Errorf("begin privileges update: %w", err)
	}
	defer tx.Rollback() //nolint:errcheck

	var currentPrivs string
	err = tx.QueryRowContext(ctx, "SELECT privileges FROM users WHERE id = $1 FOR UPDATE", id).Scan(&currentPrivs)
	if errors.Is(err, sql.ErrNoRows) {
		return ManagedAccount{}, ErrNotFound
	}
	if err != nil {
		return ManagedAccount{}, fmt.Errorf("find account: %w", err)
	}
	if IsSuper(currentPrivs) && !IsSuper(privs) {
		var otherSupers int
		if err := tx.QueryRowContext(ctx, `
			SELECT count(*) FROM users
			WHERE id <> $1
			  AND (privileges = 'super' OR privileges LIKE 'super,%' OR privileges LIKE '%,super' OR privileges LIKE '%,super,%')
			  AND NOT EXISTS (
				SELECT 1 FROM account_bans b
				WHERE b.user_id = users.id AND (b.expires_at IS NULL OR b.expires_at > now()))`,
			id).Scan(&otherSupers); err != nil {
			return ManagedAccount{}, fmt.Errorf("count super accounts: %w", err)
		}
		if otherSupers == 0 {
			return ManagedAccount{}, ErrLastSuper
		}
	}
	if _, err := tx.ExecContext(ctx,
		"UPDATE users SET privileges = $2, auth_version = auth_version + 1 WHERE id = $1", id, privs); err != nil {
		return ManagedAccount{}, fmt.Errorf("replace privileges: %w", err)
	}
	if err := tx.Commit(); err != nil {
		return ManagedAccount{}, fmt.Errorf("commit privileges update: %w", err)
	}
	return s.GetManaged(ctx, id)
}

// Ban suspends an account and increments its authorization version.
func (s *PostgresStore) Ban(ctx context.Context, id, reason string, expiresAt *time.Time, bannedBy string) (ManagedAccount, error) {
	reason = strings.TrimSpace(reason)
	if reason == "" || len(reason) > 1024 {
		return ManagedAccount{}, fmt.Errorf("%w: reason must be 1-1024 characters", ErrInvalidCode)
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return ManagedAccount{}, fmt.Errorf("begin ban: %w", err)
	}
	defer tx.Rollback() //nolint:errcheck

	var exists bool
	if err := tx.QueryRowContext(ctx, "SELECT true FROM users WHERE id = $1 FOR UPDATE", id).Scan(&exists); errors.Is(err, sql.ErrNoRows) {
		return ManagedAccount{}, ErrNotFound
	} else if err != nil {
		return ManagedAccount{}, fmt.Errorf("find account: %w", err)
	}
	if _, err := tx.ExecContext(ctx, `
		INSERT INTO account_bans (user_id, reason, expires_at, banned_by)
		VALUES ($1, $2, $3, $4)
		ON CONFLICT (user_id) DO UPDATE
		SET reason = EXCLUDED.reason, expires_at = EXCLUDED.expires_at,
		    banned_at = now(), banned_by = EXCLUDED.banned_by`,
		id, reason, expiresAt, bannedBy); err != nil {
		return ManagedAccount{}, fmt.Errorf("ban account: %w", err)
	}
	if _, err := tx.ExecContext(ctx, "UPDATE users SET auth_version = auth_version + 1 WHERE id = $1", id); err != nil {
		return ManagedAccount{}, fmt.Errorf("bump auth version: %w", err)
	}
	if err := tx.Commit(); err != nil {
		return ManagedAccount{}, fmt.Errorf("commit ban: %w", err)
	}
	return s.GetManaged(ctx, id)
}

// Unban lifts an account suspension and increments its authorization version.
func (s *PostgresStore) Unban(ctx context.Context, id string) (ManagedAccount, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return ManagedAccount{}, fmt.Errorf("begin unban: %w", err)
	}
	defer tx.Rollback() //nolint:errcheck

	var exists bool
	if err := tx.QueryRowContext(ctx, "SELECT true FROM users WHERE id = $1 FOR UPDATE", id).Scan(&exists); errors.Is(err, sql.ErrNoRows) {
		return ManagedAccount{}, ErrNotFound
	} else if err != nil {
		return ManagedAccount{}, fmt.Errorf("find account: %w", err)
	}
	result, err := tx.ExecContext(ctx, "DELETE FROM account_bans WHERE user_id = $1", id)
	if err != nil {
		return ManagedAccount{}, fmt.Errorf("unban account: %w", err)
	}
	if affected, _ := result.RowsAffected(); affected > 0 {
		if _, err := tx.ExecContext(ctx, "UPDATE users SET auth_version = auth_version + 1 WHERE id = $1", id); err != nil {
			return ManagedAccount{}, fmt.Errorf("bump auth version: %w", err)
		}
	}
	if err := tx.Commit(); err != nil {
		return ManagedAccount{}, fmt.Errorf("commit unban: %w", err)
	}
	return s.GetManaged(ctx, id)
}

func (s *PostgresStore) activeBan(ctx context.Context, id string) (*Ban, error) {
	var (
		reason    string
		expiresAt sql.NullTime
		bannedAt  time.Time
		bannedBy  string
	)
	err := s.db.QueryRowContext(ctx,
		"SELECT reason, expires_at, banned_at, banned_by FROM account_bans WHERE user_id = $1",
		id).Scan(&reason, &expiresAt, &bannedAt, &bannedBy)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("load ban: %w", err)
	}
	ban := &Ban{Reason: reason, BannedAt: bannedAt, BannedBy: bannedBy}
	if expiresAt.Valid {
		ban.ExpiresAt = &expiresAt.Time
	}
	return ban, nil
}

func scanManagedRow(row rowScanner) (Account, *Ban, error) {
	var (
		account     Account
		displayName sql.NullString
		email       sql.NullString
		verifiedAt  sql.NullTime
		reason      sql.NullString
		expiresAt   sql.NullTime
		bannedAt    sql.NullTime
		bannedBy    sql.NullString
	)
	if err := row.Scan(&account.ID, &account.Userid, &displayName, &email, &account.RezDate,
		&account.Privileges, &account.AuthVersion, &verifiedAt, &account.Kind, &account.Tags,
		&reason, &expiresAt, &bannedAt, &bannedBy); err != nil {
		return Account{}, nil, fmt.Errorf("scan managed account: %w", err)
	}
	account.Email = email.String
	account.DisplayName = displayName.String
	if account.DisplayName == "" {
		account.DisplayName = account.Userid
	}
	account.Verified = verifiedAt.Valid
	var ban *Ban
	if reason.Valid {
		ban = &Ban{Reason: reason.String, BannedAt: bannedAt.Time, BannedBy: bannedBy.String}
		if expiresAt.Valid {
			ban.ExpiresAt = &expiresAt.Time
		}
	}
	return account, ban, nil
}

// toManaged computes administrative state. A ban is active when it exists and
// has no expiry or a future expiry; an expired ban leaves the account active.
func toManaged(account Account, ban *Ban) ManagedAccount {
	managed := ManagedAccount{Account: account, State: StateActive}
	if ban != nil && (ban.ExpiresAt == nil || ban.ExpiresAt.After(time.Now())) {
		managed.State = StateBanned
		managed.Ban = ban
	}
	return managed
}

func hashPassword(password string) (bcryptHash, viewerHash string, err error) {
	hash, err := bcrypt.GenerateFromPassword([]byte(password), bcrypt.DefaultCost)
	if err != nil {
		return "", "", fmt.Errorf("hash password: %w", err)
	}
	viewerDigest := md5.Sum([]byte(password))
	return string(hash), hex.EncodeToString(viewerDigest[:]), nil
}

func newConfirmationCode() (string, error) {
	buffer := make([]byte, 32)
	if _, err := rand.Read(buffer); err != nil {
		return "", fmt.Errorf("generate confirmation code: %w", err)
	}
	return base64.RawURLEncoding.EncodeToString(buffer), nil
}

func hashCode(code string) []byte {
	digest := sha256.Sum256([]byte(code))
	return digest[:]
}

func validateEmail(email string) error {
	if email == "" || len(email) > 254 || strings.ContainsAny(email, "\r\n ") {
		return ErrInvalidEmail
	}
	at := strings.IndexByte(email, '@')
	if at <= 0 || at == len(email)-1 || strings.Contains(email[at+1:], "@") {
		return ErrInvalidEmail
	}
	if !strings.Contains(email[at+1:], ".") {
		return ErrInvalidEmail
	}
	return nil
}

// ConstantTimeEquals is a small helper for comparing opaque tokens.
func ConstantTimeEquals(a, b string) bool {
	return subtle.ConstantTimeCompare([]byte(a), []byte(b)) == 1
}
