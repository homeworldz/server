package identity

import (
	"context"
	"crypto/md5"
	"crypto/subtle"
	"database/sql"
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
	ErrConflict           = errors.New("username is already registered")
	ErrInvalidCredentials = errors.New("invalid credentials")
	// ErrBanned is correct credentials for a banned account. Viewer login
	// checked neither bans nor verification until 2026-08-24: banning an
	// account ended the sessions it held and left it free to log straight back
	// in from a viewer. The account_bans table is the website's, but the users
	// table is shared and a ban is a fact about the account rather than about
	// one way of reaching it.
	ErrBanned = errors.New("account is banned")
	ErrSessionNotFound    = errors.New("session not found")
	ErrUserNotFound       = errors.New("user not found")
)

type User struct {
	ID        string    `json:"id"`
	Username  string    `json:"username"`
	CreatedAt time.Time `json:"createdAt"`
}

type Session struct {
	ID                  string    `json:"id"`
	UserID              string    `json:"userId"`
	ExpiresAt           time.Time `json:"expiresAt"`
	SecureID            string    `json:"-"`
	ViewerCircuitCode   uint32    `json:"viewerCircuitCode,omitempty"`
	DestinationRegionID string    `json:"destinationRegionId,omitempty"`
}

type Store interface {
	CreateUser(context.Context, string, string) (User, error)
	FindUser(context.Context, string) (User, error)
	CreateSession(context.Context, string, string, time.Duration) (Session, error)
	CreateViewerSession(context.Context, string, string, time.Duration) (Session, error)
	AssignViewerDestination(context.Context, string, uint32, string) error
	ValidateSession(context.Context, string) (Session, error)
	RevokeSession(context.Context, string) error
}

type PostgresStore struct {
	db *sql.DB
}

func NewPostgresStore(db *sql.DB) *PostgresStore { return &PostgresStore{db: db} }

func (s *PostgresStore) CreateUser(ctx context.Context, username, password string) (User, error) {
	hash, err := bcrypt.GenerateFromPassword([]byte(password), bcrypt.DefaultCost)
	if err != nil {
		return User{}, fmt.Errorf("hash password: %w", err)
	}
	id, err := identifier.NewUUID()
	if err != nil {
		return User{}, err
	}
	var user User
	viewerDigest := md5.Sum([]byte(password))
	err = s.db.QueryRowContext(ctx, `
        INSERT INTO users (id, username, password_hash, viewer_password_hash) VALUES ($1, $2, $3, $4)
        RETURNING id, username, created_at`, id, username, string(hash),
		hex.EncodeToString(viewerDigest[:]),
	).Scan(&user.ID, &user.Username, &user.CreatedAt)
	var postgresError *pgconn.PgError
	if errors.As(err, &postgresError) && postgresError.Code == "23505" {
		return User{}, ErrConflict
	}
	if err != nil {
		return User{}, fmt.Errorf("create user: %w", err)
	}
	return user, nil
}

// UpsertUser creates a user with the given username and both password hashes
// (bcrypt for web/account login, MD5 for viewer login), or, when a user with that
// username already exists, replaces only its password hashes. It reports whether a
// new user was created. Intended for the command-line administration path, so it is
// deliberately not part of the Store interface.
func (s *PostgresStore) UpsertUser(ctx context.Context, username, password string) (User, bool, error) {
	if password == "" {
		return User{}, false, errors.New("password cannot be empty")
	}
	hash, err := bcrypt.GenerateFromPassword([]byte(password), bcrypt.DefaultCost)
	if err != nil {
		return User{}, false, fmt.Errorf("hash password: %w", err)
	}
	viewerDigest := md5.Sum([]byte(password))
	viewerHash := hex.EncodeToString(viewerDigest[:])
	var user User
	err = s.db.QueryRowContext(ctx, `
		UPDATE users SET password_hash = $2, viewer_password_hash = $3
		WHERE username = $1 RETURNING id, username, created_at`,
		username, string(hash), viewerHash,
	).Scan(&user.ID, &user.Username, &user.CreatedAt)
	if err == nil {
		return user, false, nil
	}
	if !errors.Is(err, sql.ErrNoRows) {
		return User{}, false, fmt.Errorf("update user: %w", err)
	}
	id, err := identifier.NewUUID()
	if err != nil {
		return User{}, false, err
	}
	// A CLI-created account is marked verified so it can log in on the website as
	// well as in the viewer immediately.
	err = s.db.QueryRowContext(ctx, `
		INSERT INTO users (id, username, password_hash, viewer_password_hash, verified_at)
		VALUES ($1, $2, $3, $4, now()) RETURNING id, username, created_at`,
		id, username, string(hash), viewerHash,
	).Scan(&user.ID, &user.Username, &user.CreatedAt)
	var postgresError *pgconn.PgError
	if errors.As(err, &postgresError) && postgresError.Code == "23505" {
		return User{}, false, ErrConflict
	}
	if err != nil {
		return User{}, false, fmt.Errorf("create user: %w", err)
	}
	return user, true, nil
}

func (s *PostgresStore) FindUser(ctx context.Context, id string) (User, error) {
	var user User
	err := s.db.QueryRowContext(ctx,
		"SELECT id, username, created_at FROM users WHERE id = $1", id,
	).Scan(&user.ID, &user.Username, &user.CreatedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return User{}, ErrUserNotFound
	}
	if err != nil {
		return User{}, fmt.Errorf("find user: %w", err)
	}
	return user, nil
}

// CountUsers is the account total the daily stats row records. Deliberately
// not on the Store interface: only the stats recorder wants it, and it takes
// the concrete Postgres store rather than widening every test double.
func (s *PostgresStore) CountUsers(ctx context.Context) (int, error) {
	var count int
	if err := s.db.QueryRowContext(ctx, "SELECT COUNT(*) FROM users").Scan(&count); err != nil {
		return 0, fmt.Errorf("count users: %w", err)
	}
	return count, nil
}

// ConfigureSystemUser assigns interactive credentials to a stable, reserved
// identity created by a migration. It is intentionally not part of Store and
// is therefore unavailable through the public user API.
func (s *PostgresStore) ConfigureSystemUser(ctx context.Context, id, username, password string) (User, error) {
	if password == "" {
		return User{}, errors.New("system user password cannot be empty")
	}
	hash, err := bcrypt.GenerateFromPassword([]byte(password), bcrypt.DefaultCost)
	if err != nil {
		return User{}, fmt.Errorf("hash system user password: %w", err)
	}
	viewerDigest := md5.Sum([]byte(password))
	var user User
	err = s.db.QueryRowContext(ctx, `
		UPDATE users SET username = $2, password_hash = $3, viewer_password_hash = $4
		WHERE id = $1 RETURNING id, username, created_at`, id, username, string(hash),
		hex.EncodeToString(viewerDigest[:]),
	).Scan(&user.ID, &user.Username, &user.CreatedAt)
	var postgresError *pgconn.PgError
	if errors.As(err, &postgresError) && postgresError.Code == "23505" {
		return User{}, ErrConflict
	}
	if errors.Is(err, sql.ErrNoRows) {
		return User{}, fmt.Errorf("system user %s is not installed; apply pending migrations", id)
	}
	if err != nil {
		return User{}, fmt.Errorf("configure system user: %w", err)
	}
	return user, nil
}

// SetPassword replaces a user's password, found by username. It exists because
// the account API's ChangePassword deliberately requires the current password,
// which is the right rule for a user and leaves an operator with no way to
// recover an account whose password is lost. Both digests are written: bcrypt
// for the web account and the viewer's MD5 for legacy login, because an account
// that can sign in to one and not the other is worse than one that cannot sign
// in at all.
//
// The username is matched case-insensitively, as login does.
func (s *PostgresStore) SetPassword(ctx context.Context, username, password string) (User, error) {
	if strings.TrimSpace(username) == "" {
		return User{}, errors.New("username is required")
	}
	if password == "" {
		return User{}, errors.New("password is required")
	}
	hash, err := bcrypt.GenerateFromPassword([]byte(password), bcrypt.DefaultCost)
	if err != nil {
		return User{}, fmt.Errorf("hash password: %w", err)
	}
	viewerDigest := md5.Sum([]byte(password))
	var user User
	err = s.db.QueryRowContext(ctx, `
		UPDATE users SET password_hash = $2, viewer_password_hash = $3
		WHERE lower(username) = lower($1) RETURNING id, username, created_at`,
		username, string(hash), hex.EncodeToString(viewerDigest[:]),
	).Scan(&user.ID, &user.Username, &user.CreatedAt)
	if errors.Is(err, sql.ErrNoRows) {
		// Naming the format in the failure, because a viewer shows a display
		// name ("Jim Tarber") while the account is first.last ("jim.tarber"),
		// and the operator lost a round to exactly that.
		return User{}, fmt.Errorf(
			"no account named %q; usernames are first.last as at login, not the display name",
			username)
	}
	if err != nil {
		return User{}, fmt.Errorf("set password: %w", err)
	}
	return user, nil
}

func (s *PostgresStore) CreateSession(ctx context.Context, username, password string, duration time.Duration) (Session, error) {
	var userID, passwordHash string
	err := s.db.QueryRowContext(ctx, "SELECT id, password_hash FROM users WHERE username = $1", username).
		Scan(&userID, &passwordHash)
	if errors.Is(err, sql.ErrNoRows) {
		return Session{}, ErrInvalidCredentials
	}
	if err != nil {
		return Session{}, fmt.Errorf("find user credentials: %w", err)
	}
	if bcrypt.CompareHashAndPassword([]byte(passwordHash), []byte(password)) != nil {
		return Session{}, ErrInvalidCredentials
	}
	return s.insertSession(ctx, userID, duration)
}

func (s *PostgresStore) CreateViewerSession(ctx context.Context, username, passwordHash string, duration time.Duration) (Session, error) {
	var userID, expected string
	var banned bool
	err := s.db.QueryRowContext(ctx, `
		SELECT u.id, u.viewer_password_hash,
		       EXISTS (SELECT 1 FROM account_bans b
		               WHERE b.user_id = u.id
		                 AND (b.expires_at IS NULL OR b.expires_at > now()))
		  FROM users u WHERE u.username = $1`, username).
		Scan(&userID, &expected, &banned)
	if errors.Is(err, sql.ErrNoRows) {
		return Session{}, ErrInvalidCredentials
	}
	if err != nil {
		return Session{}, fmt.Errorf("find viewer credentials: %w", err)
	}
	if expected == "" || len(expected) != len(passwordHash) ||
		subtle.ConstantTimeCompare([]byte(expected), []byte(passwordHash)) != 1 {
		return Session{}, ErrInvalidCredentials
	}
	// Checked after the password, so this cannot be used to learn who is
	// banned without already holding their credentials.
	if banned {
		return Session{}, ErrBanned
	}
	return s.insertSession(ctx, userID, duration)
}

// CreateClientSession opens a session for the Homeworldz client's world entry
// (docs/CLIENT2.md, "One session store for every kind of client"). The caller
// has already authenticated a bearer token, so no credential is checked here;
// the destination region is known at creation and no viewer circuit is ever
// assigned. It is deliberately not part of the Store interface, which serves
// the viewer paths.
func (s *PostgresStore) CreateClientSession(ctx context.Context, userID, regionID string, duration time.Duration) (Session, error) {
	id, err := identifier.NewUUID()
	if err != nil {
		return Session{}, err
	}
	secureID, err := identifier.NewUUID()
	if err != nil {
		return Session{}, err
	}
	var session Session
	err = s.db.QueryRowContext(ctx, `
        INSERT INTO sessions (id, user_id, expires_at, secure_session_id, destination_region_id)
        VALUES ($1, $2, now() + $3 * interval '1 second', $4, $5)
        RETURNING id, user_id, expires_at, secure_session_id, destination_region_id`,
		id, userID, int64(duration/time.Second), secureID, regionID,
	).Scan(&session.ID, &session.UserID, &session.ExpiresAt, &session.SecureID, &session.DestinationRegionID)
	if err != nil {
		return Session{}, fmt.Errorf("create client session: %w", err)
	}
	return session, nil
}

func (s *PostgresStore) insertSession(ctx context.Context, userID string, duration time.Duration) (Session, error) {
	id, err := identifier.NewUUID()
	if err != nil {
		return Session{}, err
	}
	secureID, err := identifier.NewUUID()
	if err != nil {
		return Session{}, err
	}
	var session Session
	err = s.db.QueryRowContext(ctx, `
        INSERT INTO sessions (id, user_id, expires_at, secure_session_id)
        VALUES ($1, $2, now() + $3 * interval '1 second', $4)
        RETURNING id, user_id, expires_at, secure_session_id`, id, userID, int64(duration/time.Second), secureID,
	).Scan(&session.ID, &session.UserID, &session.ExpiresAt, &session.SecureID)
	if err != nil {
		return Session{}, fmt.Errorf("create session: %w", err)
	}
	return session, nil
}

func (s *PostgresStore) ValidateSession(ctx context.Context, id string) (Session, error) {
	var session Session
	var circuit sql.NullInt64
	var regionID sql.NullString
	err := s.db.QueryRowContext(ctx, `
		SELECT id, user_id, expires_at, COALESCE(secure_session_id::text, ''),
		       viewer_circuit_code, destination_region_id::text FROM sessions
        WHERE id = $1 AND expires_at > now()`, id,
	).Scan(&session.ID, &session.UserID, &session.ExpiresAt, &session.SecureID, &circuit, &regionID)
	if errors.Is(err, sql.ErrNoRows) {
		return Session{}, ErrSessionNotFound
	}
	if err != nil {
		return Session{}, fmt.Errorf("validate session: %w", err)
	}
	if circuit.Valid {
		session.ViewerCircuitCode = uint32(circuit.Int64)
	}
	if regionID.Valid {
		session.DestinationRegionID = regionID.String
	}
	return session, nil
}

func (s *PostgresStore) AssignViewerDestination(ctx context.Context, sessionID string, circuit uint32, regionID string) error {
	result, err := s.db.ExecContext(ctx, `
		UPDATE sessions SET viewer_circuit_code = $2, destination_region_id = $3
		WHERE id = $1 AND expires_at > now()`, sessionID, circuit, regionID)
	if err != nil {
		return fmt.Errorf("assign viewer destination: %w", err)
	}
	count, err := result.RowsAffected()
	if err != nil {
		return fmt.Errorf("count assigned viewer destinations: %w", err)
	}
	if count == 0 {
		return ErrSessionNotFound
	}
	return nil
}

func (s *PostgresStore) RevokeSession(ctx context.Context, id string) error {
	result, err := s.db.ExecContext(ctx, "DELETE FROM sessions WHERE id = $1", id)
	if err != nil {
		return fmt.Errorf("revoke session: %w", err)
	}
	count, err := result.RowsAffected()
	if err != nil {
		return fmt.Errorf("count revoked sessions: %w", err)
	}
	if count == 0 {
		return ErrSessionNotFound
	}
	return nil
}
