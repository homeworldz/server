// Package api implements the browser-facing Homeworldz website API described
// by homeworldz.com/api/openapi.yaml: email-verified avatar registration,
// stateless website authentication, self-service account management, and
// privileged user/ban/region administration.
//
// It is intentionally separate from the grid's service-token internal API and
// from viewer login: it runs as its own binary on its own port, applies a
// browser-oriented middleware chain (CORS, rate limiting), and authenticates
// with short-lived website JWTs that carry no in-world meaning.
package api

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"strings"
	"time"

	"github.com/homeworldz/server/grid/internal/arrival"
	"github.com/homeworldz/server/grid/internal/eventlog"
	"github.com/homeworldz/server/grid/internal/identity"
	"github.com/homeworldz/server/grid/internal/inventory"
	"github.com/homeworldz/server/grid/internal/locations"
	"github.com/homeworldz/server/grid/internal/mailer"
	"github.com/homeworldz/server/grid/internal/messages"
	"github.com/homeworldz/server/grid/internal/presence"
	"github.com/homeworldz/server/grid/internal/provisioning"
	"github.com/homeworldz/server/grid/internal/regions"
	"github.com/homeworldz/server/grid/internal/stats"
	"github.com/homeworldz/server/grid/internal/webaccount"
	"github.com/homeworldz/server/grid/internal/webtoken"
)

// AccountStore is the account persistence the API depends on. It is satisfied
// by *webaccount.PostgresStore.
type AccountStore interface {
	Register(ctx context.Context, displayName, email string) (webaccount.Account, string, error)
	Verify(ctx context.Context, code, password string) (webaccount.Account, error)
	ResendVerification(ctx context.Context, userid string) (code, email string, err error)
	Authenticate(ctx context.Context, userid, password string) (webaccount.Account, error)
	Get(ctx context.Context, id string) (webaccount.Account, error)
	GetManaged(ctx context.Context, id string) (webaccount.ManagedAccount, error)
	UpdateProfile(ctx context.Context, id, displayName string) (webaccount.Account, error)
	ChangePassword(ctx context.Context, id, currentPassword, newPassword string) error
	RequestPasswordReset(ctx context.Context, ident string) (token, email string, err error)
	ConsumePasswordReset(ctx context.Context, token, newPassword string) error
	List(ctx context.Context, search, cursor string, limit int) ([]webaccount.ManagedAccount, string, error)
	ReplacePrivileges(ctx context.Context, id, privs string) (webaccount.ManagedAccount, error)
	Ban(ctx context.Context, id, reason string, expiresAt *time.Time, bannedBy string) (webaccount.ManagedAccount, error)
	Unban(ctx context.Context, id string) (webaccount.ManagedAccount, error)
	SetClassification(ctx context.Context, id, kind, tags string) (webaccount.ManagedAccount, error)
}

// RegionStore is the provisioned-region persistence the API depends on. It is
// satisfied by *provisioning.PostgresStore.
type RegionStore interface {
	List(ctx context.Context) ([]provisioning.Region, error)
	Get(ctx context.Context, id string) (provisioning.Region, error)
	Create(ctx context.Context, region provisioning.Region) (provisioning.Region, error)
	Update(ctx context.Context, id string, update provisioning.Update) (provisioning.Region, error)
	RotateAccessKey(ctx context.Context, id, accessKey string) (provisioning.Region, error)
	Delete(ctx context.Context, id string) error
}

// LeaseStore exposes just enough of the live-region lease store to derive
// online state, list leased regions for destination resolution, and end a
// lease on undeploy. It is satisfied by *regions.PostgresStore.
type LeaseStore interface {
	Get(ctx context.Context, id string) (regions.Region, error)
	List(ctx context.Context) ([]regions.Region, error)
	DeregisterProvisioned(ctx context.Context, id string) error
}

// SessionStore opens client sessions in the session store shared with viewer
// logins. It is satisfied by *identity.PostgresStore.
type SessionStore interface {
	CreateClientSession(ctx context.Context, userID, regionID string, duration time.Duration) (identity.Session, error)
}

// LocationStore reads a user's stored last and home locations for world
// entry. It is satisfied by *locations.PostgresStore.
type LocationStore interface {
	Get(ctx context.Context, userID string) (locations.Location, error)
	GetHome(ctx context.Context, userID string) (locations.Location, error)
}

// InventoryStore exposes a user's own folders and items. Both methods are
// scoped by user id, which is what lets the read-side helpers in the inventory
// package enforce ownership structurally. Satisfied by *inventory.PostgresStore.
type InventoryStore interface {
	ListFolders(ctx context.Context, userID string) ([]inventory.Folder, error)
	ListItems(ctx context.Context, userID string) ([]inventory.Item, error)
}

// PresenceStore exposes active viewer presences for system status. It is
// satisfied by *presence.PostgresStore.
type PresenceStore interface {
	List(ctx context.Context) ([]presence.Presence, error)
}

// Options configures New.
type Options struct {
	Accounts        AccountStore
	Regions         RegionStore
	Leases          LeaseStore
	Presence        PresenceStore
	Signer          *webtoken.Signer
	Mailer          mailer.Mailer
	Logger          *slog.Logger
	AllowedOrigins  []string
	VerificationURL string
	ResetURL        string
	RatePerMinute   int
	RateBurst       int
	// Version is the build version reported by GET /v1/version.
	Version string
	// GridName is the operator-facing grid name ([grid] name), reported to the
	// Homeworldz client for its login screen.
	GridName string
	// WelcomeMessage is the grid-wide greeting template ([grid]
	// welcome_message; {grid} and {user} placeholders), delivered once per
	// login in the grid channel hello; empty disables it.
	WelcomeMessage string
	// Welcome is the ordered new-arrival list ([grid] welcome_locations); the
	// probe's welcome field derives from its first entry.
	Welcome []arrival.Point
	// Sessions and Locations serve client world entry; TicketSigner mints the
	// region-scoped ticket and must carry the region-ticket audience and a
	// short TTL, never the account-token audience.
	Sessions  SessionStore
	Locations LocationStore
	// Inventory serves the client's read-only view of its own inventory. The
	// same rows the AIS capability serves a viewer; see client_inventory.go.
	Inventory    InventoryStore
	TicketSigner *webtoken.Signer
	// ChannelURL is the absolute wss:// URL of the grid channel, reported in
	// the probe when the deployment knows its public URL; a client that
	// probed can otherwise derive it from the API base it already used.
	ChannelURL string
	// Messages persists instant messages for store-and-forward delivery over
	// the grid channel. Nil disables POST /v1/client/messages.
	Messages messages.Store
	// RegionPublicBase is where a browser reaches a region's HTTP API; a
	// region id is appended to it. Empty hands out the region's own endpoint.
	// See config.Grid.RegionPublicBase for why the two differ.
	RegionPublicBase string
	// Stats collects the public grid statistics served at GET /v1/stats.
	// Nil answers that route 503 rather than publishing zeros.
	Stats *stats.Collector
	// Events records registrations for those statistics (ADR 0039).
	// Recording is best-effort: a registration that succeeded is never failed
	// because its log row was not written.
	Events eventlog.Recorder
}

// API is the website API handler.
type API struct {
	accounts         AccountStore
	regions          RegionStore
	leases           LeaseStore
	presence         PresenceStore
	signer           *webtoken.Signer
	mailer           mailer.Mailer
	logger           *slog.Logger
	allowedOrigins   map[string]bool
	verificationURL  string
	resetURL         string
	limiter          *rateLimiter
	version          string
	gridName         string
	welcomeText      string
	welcome          []arrival.Point
	sessions         SessionStore
	locations        LocationStore
	inventory        InventoryStore
	ticketSigner     *webtoken.Signer
	channelURL       string
	channels         *channelHub
	messages         messages.Store
	regionPublicBase string
	stats            *stats.Collector
	statsCache       *statsCache
	events           eventlog.Recorder
}

// New validates options and returns the composed website API handler.
func New(options Options) (http.Handler, error) {
	if options.Signer == nil {
		return nil, errors.New("api: signer is required")
	}
	if options.Mailer == nil {
		return nil, errors.New("api: mailer is required")
	}
	origins := map[string]bool{}
	for _, origin := range options.AllowedOrigins {
		trimmed := strings.TrimRight(strings.TrimSpace(origin), "/")
		if trimmed != "" {
			origins[trimmed] = true
		}
	}
	perMinute := options.RatePerMinute
	if perMinute <= 0 {
		perMinute = 30
	}
	burst := options.RateBurst
	if burst <= 0 {
		burst = 10
	}
	a := &API{
		accounts:         options.Accounts,
		regions:          options.Regions,
		leases:           options.Leases,
		presence:         options.Presence,
		signer:           options.Signer,
		mailer:           options.Mailer,
		logger:           options.Logger,
		allowedOrigins:   origins,
		verificationURL:  strings.TrimRight(options.VerificationURL, "/"),
		resetURL:         strings.TrimRight(options.ResetURL, "/"),
		limiter:          newRateLimiter(float64(perMinute)/60.0, burst),
		version:          options.Version,
		gridName:         options.GridName,
		welcomeText:      options.WelcomeMessage,
		welcome:          options.Welcome,
		sessions:         options.Sessions,
		locations:        options.Locations,
		inventory:        options.Inventory,
		ticketSigner:     options.TicketSigner,
		channelURL:       options.ChannelURL,
		channels:         newChannelHub(),
		messages:         options.Messages,
		regionPublicBase: strings.TrimRight(strings.TrimSpace(options.RegionPublicBase), "/"),
		stats:            options.Stats,
		statsCache:       newStatsCache(),
		events:           options.Events,
	}

	mux := http.NewServeMux()
	// Method-specific patterns would 405 wrong methods by themselves, but the
	// "/" catch-all below out-matches that fallback and turns them into 404s,
	// so handlers keep the explicit method check the rest of the mux uses.
	mux.HandleFunc("/v1/version", a.clientVersion)
	mux.HandleFunc("/v1/stats", a.gridStats)
	mux.HandleFunc("/v1/client/session", a.clientSession)
	mux.HandleFunc("/v1/client/channel", a.clientChannel)
	mux.HandleFunc("/v1/client/messages", a.clientMessages)
	mux.HandleFunc("/v1/client/inventory", a.clientInventoryRoot)
	mux.HandleFunc("/v1/client/inventory/", a.clientInventoryByID)
	mux.HandleFunc("/v1/registrations", a.registrations)
	mux.HandleFunc("/v1/verifications", a.verifications)
	mux.HandleFunc("/v1/verifications/resend", a.resendVerification)
	mux.HandleFunc("/v1/password-resets", a.passwordResets)
	mux.HandleFunc("/v1/password-resets/", a.consumePasswordReset)
	mux.HandleFunc("/v1/tokens", a.tokens)
	mux.HandleFunc("/v1/account", a.account)
	mux.HandleFunc("/v1/account/profile", a.accountProfile)
	mux.HandleFunc("/v1/account/password", a.accountPassword)
	mux.HandleFunc("/v1/admin/users", a.adminUsersRoot)
	mux.HandleFunc("/v1/admin/users/", a.adminUserByID)
	mux.HandleFunc("/v1/admin/regions", a.adminRegionsRoot)
	mux.HandleFunc("/v1/admin/regions/", a.adminRegionByID)
	mux.HandleFunc("/v1/admin/system/status", a.systemStatus)
	mux.HandleFunc("/", a.notFound)

	return withRecovery(withRequestID(withRequestLogging(
		a.withCORS(mux), a.logger)), a.logger), nil
}

// notFound answers a request for a route this build does not serve.
//
// The code is deliberately *not* `not_found`, which resource misses use. The two
// are the same status and mean entirely different things: `not_found` says the
// thing you named is not yours or is not there, and `route_not_found` says this
// deployment has no such endpoint — usually that it predates one.
//
// Sharing a code made them indistinguishable without matching message strings,
// and a client hitting the inventory routes before they were deployed read the
// router's 404 as a permission decision. That sends somebody to examine auth and
// ownership when the answer is a tier that has not shipped yet (client core,
// 2026-08-08). A caller upgrading past a deployment should be able to tell "not
// deployed" from "not allowed" without parsing prose.
func (a *API) notFound(w http.ResponseWriter, _ *http.Request) {
	writeError(w, http.StatusNotFound, Error{Code: "route_not_found", Message: "route not found"})
}

// methodNotAllowed writes a 405 with an Allow header listing supported methods.
func methodNotAllowed(w http.ResponseWriter, allow ...string) {
	w.Header().Set("Allow", strings.Join(allow, ", "))
	writeError(w, http.StatusMethodNotAllowed, Error{Code: "method_not_allowed", Message: "method not allowed"})
}

func writeJSON(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(value)
}

// writeError writes an error body without caching.
func writeError(w http.ResponseWriter, status int, body Error) {
	writeJSON(w, status, body)
}

// decodeJSON reads exactly one JSON object into target, rejecting unknown
// fields and trailing data, matching the contract's additionalProperties:false.
func decodeJSON(w http.ResponseWriter, r *http.Request, target any) bool {
	decoder := json.NewDecoder(http.MaxBytesReader(w, r.Body, 64*1024))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(target); err != nil {
		writeError(w, http.StatusBadRequest, Error{Code: "invalid_json", Message: "request body must be valid JSON"})
		return false
	}
	if err := decoder.Decode(&struct{}{}); !errors.Is(err, io.EOF) {
		writeError(w, http.StatusBadRequest, Error{Code: "invalid_json", Message: "request body must contain one JSON object"})
		return false
	}
	return true
}

func validUUID(value string) bool {
	if len(value) != 36 || value[8] != '-' || value[13] != '-' || value[18] != '-' || value[23] != '-' {
		return false
	}
	for index, character := range value {
		if index == 8 || index == 13 || index == 18 || index == 23 {
			continue
		}
		if !((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') || (character >= 'A' && character <= 'F')) {
			return false
		}
	}
	return true
}

// identityOf maps a stored account to the public Identity DTO.
func identityOf(account webaccount.Account) Identity {
	return Identity{
		ID:          account.ID,
		Userid:      account.Userid,
		DisplayName: account.DisplayName,
		// Every route that returns an Identity is either the account's own
		// session or an administrator's listing; none serves it to a third party.
		Email:   account.Email,
		RezDate: account.RezDate.UTC(),
		Privs:   account.Privileges,
	}
}

// managedUserOf maps a managed account to the ManagedUser DTO.
func managedUserOf(account webaccount.ManagedAccount) ManagedUser {
	user := ManagedUser{
		Identity: identityOf(account.Account),
		State:    account.State,
		Kind:     account.Kind,
		Tags:     account.Tags,
	}
	if account.Ban != nil {
		ban := &Ban{Reason: account.Ban.Reason, BannedAt: account.Ban.BannedAt.UTC(), BannedBy: account.Ban.BannedBy}
		if account.Ban.ExpiresAt != nil {
			expires := account.Ban.ExpiresAt.UTC()
			ban.ExpiresAt = &expires
		}
		user.Ban = ban
	}
	return user
}
