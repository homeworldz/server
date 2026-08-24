package api

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/homeworldz/server/grid/internal/arrival"
	"github.com/homeworldz/server/grid/internal/identifier"
	"github.com/homeworldz/server/grid/internal/identity"
	"github.com/homeworldz/server/grid/internal/locations"
	"github.com/homeworldz/server/grid/internal/regions"
	"github.com/homeworldz/server/grid/internal/webaccount"
	"github.com/homeworldz/server/grid/internal/webtoken"
)

// memoryAccountStore satisfies AccountStore for requireAuth: only Get is
// exercised by these tests.
type memoryAccountStore struct {
	accounts map[string]webaccount.Account
	// Password-reset scaffolding. resetToken empty means "no such account", so a
	// test can drive both the matched and unmatched paths and assert the handler
	// cannot be told them apart.
	resetToken    string
	resetRequests []string
	resetConsumed string
}

var errUnimplemented = errors.New("not implemented in test store")

func (s *memoryAccountStore) Get(_ context.Context, id string) (webaccount.Account, error) {
	account, ok := s.accounts[id]
	if !ok {
		return webaccount.Account{}, webaccount.ErrNotFound
	}
	return account, nil
}

func (s *memoryAccountStore) Register(context.Context, string, string) (webaccount.Account, string, error) {
	return webaccount.Account{}, "", errUnimplemented
}

func (s *memoryAccountStore) Verify(context.Context, string, string) (webaccount.Account, error) {
	return webaccount.Account{}, errUnimplemented
}

func (s *memoryAccountStore) ResendVerification(context.Context, string) (string, string, error) {
	return "", "", errUnimplemented
}

func (s *memoryAccountStore) Authenticate(context.Context, string, string) (webaccount.Account, error) {
	return webaccount.Account{}, errUnimplemented
}

func (s *memoryAccountStore) GetManaged(context.Context, string) (webaccount.ManagedAccount, error) {
	return webaccount.ManagedAccount{}, errUnimplemented
}

func (s *memoryAccountStore) UpdateProfile(context.Context, string, string) (webaccount.Account, error) {
	return webaccount.Account{}, errUnimplemented
}

func (s *memoryAccountStore) ChangePassword(context.Context, string, string, string) error {
	return errUnimplemented
}

// Recorded rather than stubbed, so a test can assert the request handler answers
// identically whether or not an account matched - which is the property that
// stops the endpoint enumerating accounts.
func (s *memoryAccountStore) RequestPasswordReset(_ context.Context, ident string) (string, string, error) {
	s.resetRequests = append(s.resetRequests, ident)
	if s.resetToken == "" {
		return "", "", webaccount.ErrNotFound
	}
	return s.resetToken, "someone@example.test", nil
}

func (s *memoryAccountStore) ConsumePasswordReset(_ context.Context, token, password string) error {
	if s.resetToken == "" || token != s.resetToken {
		return webaccount.ErrInvalidCode
	}
	s.resetConsumed = password
	return nil
}

func (s *memoryAccountStore) List(context.Context, string, string, int) ([]webaccount.ManagedAccount, string, error) {
	return nil, "", errUnimplemented
}

func (s *memoryAccountStore) ReplacePrivileges(context.Context, string, string) (webaccount.ManagedAccount, error) {
	return webaccount.ManagedAccount{}, errUnimplemented
}

func (s *memoryAccountStore) Ban(context.Context, string, string, *time.Time, string) (webaccount.ManagedAccount, error) {
	return webaccount.ManagedAccount{}, errUnimplemented
}

func (s *memoryAccountStore) Unban(context.Context, string) (webaccount.ManagedAccount, error) {
	return webaccount.ManagedAccount{}, errUnimplemented
}

func (s *memoryAccountStore) SetClassification(context.Context, string, string, string) (webaccount.ManagedAccount, error) {
	return webaccount.ManagedAccount{}, errUnimplemented
}

// memoryLeaseStore satisfies LeaseStore with a fixed leased-region set.
type memoryLeaseStore struct{ items []regions.Region }

func (s *memoryLeaseStore) Get(_ context.Context, id string) (regions.Region, error) {
	for _, item := range s.items {
		if item.ID == id {
			return item, nil
		}
	}
	return regions.Region{}, regions.ErrNotFound
}

func (s *memoryLeaseStore) List(context.Context) ([]regions.Region, error) { return s.items, nil }

func (s *memoryLeaseStore) DeregisterProvisioned(context.Context, string) error { return nil }

// memorySessionStore records created client sessions.
type memorySessionStore struct{ created []identity.Session }

func (s *memorySessionStore) CreateClientSession(_ context.Context, userID, regionID string, duration time.Duration) (identity.Session, error) {
	id, err := identifier.NewUUID()
	if err != nil {
		return identity.Session{}, err
	}
	session := identity.Session{ID: id, UserID: userID,
		ExpiresAt: time.Now().Add(duration).UTC(), DestinationRegionID: regionID}
	s.created = append(s.created, session)
	return session, nil
}

// memoryLocationStore serves stored last/home locations.
type memoryLocationStore struct{ last, home map[string]locations.Location }

func (s *memoryLocationStore) Get(_ context.Context, userID string) (locations.Location, error) {
	value, ok := s.last[userID]
	if !ok {
		return locations.Location{}, locations.ErrNotFound
	}
	return value, nil
}

func (s *memoryLocationStore) GetHome(_ context.Context, userID string) (locations.Location, error) {
	value, ok := s.home[userID]
	if !ok {
		return locations.Location{}, locations.ErrNotFound
	}
	return value, nil
}

const testUserID = "44444444-4444-4444-8444-444444444444"

type worldEntryHarness struct {
	handler  http.Handler
	sessions *memorySessionStore
	signer   *webtoken.Signer
	ticket   *webtoken.Signer
	token    string
}

func newWorldEntryHarness(t *testing.T, mutate ...func(*Options)) *worldEntryHarness {
	t.Helper()
	account := webaccount.Account{ID: testUserID, Userid: "test.user", DisplayName: "Test User",
		RezDate: time.Date(2026, 1, 1, 0, 0, 0, 0, time.UTC), Privileges: "", AuthVersion: 1}
	sessions := &memorySessionStore{}
	harness := &worldEntryHarness{sessions: sessions}
	harness.handler = newTestAPI(t, append([]func(*Options){func(options *Options) {
		ticketSigner, err := webtoken.NewSigner(
			[]byte("0123456789abcdef0123456789abcdef"), "https://issuer.test",
			webtoken.RegionTicketAudience, 5*time.Minute)
		if err != nil {
			t.Fatalf("ticket signer: %v", err)
		}
		options.Accounts = &memoryAccountStore{accounts: map[string]webaccount.Account{testUserID: account}}
		options.Sessions = sessions
		options.TicketSigner = ticketSigner
		options.Leases = &memoryLeaseStore{items: []regions.Region{
			{ID: "aaaaaaaa-0000-4000-8000-000000000001", Name: "Welcome", GridX: 1000, GridY: 1000,
				PublicEndpoint: "http://welcome.example:9000/", ViewerPort: 9001,
				SessionEndpoint: "wss://welcome.example/session"},
			{ID: "aaaaaaaa-0000-4000-8000-000000000002", Name: "Sandbox", GridX: 1001, GridY: 1000,
				PublicEndpoint: "http://sandbox.example:9000", ViewerPort: 9001},
		}}
		options.Welcome = []arrival.Point{{Region: "Welcome", X: 127, Y: 127, Z: 23}}
		harness.ticket = ticketSigner
	}}, mutate...)...)

	// Mint the account bearer token the way issueToken does.
	signer, err := webtoken.NewSigner(
		[]byte("0123456789abcdef0123456789abcdef"), "https://issuer.test", "https://audience.test", time.Hour)
	if err != nil {
		t.Fatalf("signer: %v", err)
	}
	harness.signer = signer
	token, _, err := signer.Sign(time.Now(), account.ID, account.Userid, account.DisplayName,
		account.RezDate, account.Privileges, account.AuthVersion)
	if err != nil {
		t.Fatalf("sign account token: %v", err)
	}
	harness.token = token
	return harness
}

func (h *worldEntryHarness) open(t *testing.T, body, bearer string) *httptest.ResponseRecorder {
	t.Helper()
	request := httptest.NewRequest(http.MethodPost, "/v1/client/session", strings.NewReader(body))
	if bearer != "" {
		request.Header.Set("Authorization", "Bearer "+bearer)
	}
	request.Header.Set("Content-Type", "application/json")
	response := httptest.NewRecorder()
	h.handler.ServeHTTP(response, request)
	return response
}

// TestRegionEndpointIsReachableFromABrowser covers the address handed to a
// client: a region serves plain http and a browser will not fetch that from an
// https page, so a deployment that terminates TLS in front of its regions
// configures a base and every region is reached through it by id.
//
// By id, not by name: one region's name can be a prefix of another's, which is
// how every Nova B session ended up routed to Nova.
func TestRegionEndpointIsReachableFromABrowser(t *testing.T) {
	const regionID = "60ed06a2-dbd0-40f8-bd3f-e23786752f81"
	direct := &API{}
	if got := direct.regionEndpointFor(regionID, "http://grid.example:42101/"); got != "http://grid.example:42101" {
		t.Fatalf("unconfigured = %q, want the region's own endpoint", got)
	}
	proxied := &API{regionPublicBase: "https://grid.example/region"}
	if got := proxied.regionEndpointFor(regionID, "http://grid.example:42101"); got !=
		"https://grid.example/region/"+regionID {
		t.Fatalf("configured = %q", got)
	}
	// A region the grid cannot name falls back rather than producing a base
	// with nothing after it, which would route to whatever answers first.
	if got := proxied.regionEndpointFor("", "http://grid.example:42101"); got != "http://grid.example:42101" {
		t.Fatalf("nameless region = %q", got)
	}
}

func TestClientSessionRequiresAuthentication(t *testing.T) {
	harness := newWorldEntryHarness(t)
	if response := harness.open(t, `{}`, ""); response.Code != http.StatusUnauthorized {
		t.Fatalf("unauthenticated status = %d", response.Code)
	}
}

func TestClientSessionWelcomeArrival(t *testing.T) {
	harness := newWorldEntryHarness(t)
	response := harness.open(t, `{}`, harness.token)
	if response.Code != http.StatusOK {
		t.Fatalf("status = %d: %s", response.Code, response.Body.String())
	}
	var opened ClientSession
	if err := json.Unmarshal(response.Body.Bytes(), &opened); err != nil {
		t.Fatal(err)
	}

	// A user with no stored location lands on the welcome arrival point.
	if opened.Region.Name != "Welcome" || opened.Region.GridX != 1000 || opened.Region.GridY != 1000 {
		t.Fatalf("unexpected region: %+v", opened.Region)
	}
	if opened.Region.Endpoint != "http://welcome.example:9000" {
		t.Fatalf("endpoint not normalized: %q", opened.Region.Endpoint)
	}
	if opened.Region.Position == nil || *opened.Region.Position != [3]float64{127, 127, 23} {
		t.Fatalf("unexpected position: %v", opened.Region.Position)
	}

	// The session row anchors the destination region.
	if len(harness.sessions.created) != 1 {
		t.Fatalf("sessions created = %d", len(harness.sessions.created))
	}
	created := harness.sessions.created[0]
	if created.UserID != testUserID || created.DestinationRegionID != opened.Region.ID {
		t.Fatalf("unexpected session: %+v", created)
	}
	if opened.Session.ID != created.ID {
		t.Fatalf("session id mismatch: %q != %q", opened.Session.ID, created.ID)
	}

	// The manifest is versioned data derived from what the region reported:
	// this region serves the WebSocket session transport.
	if opened.Capabilities.Version != 1 || len(opened.Capabilities.Transports) != 1 ||
		opened.Capabilities.Transports[0] != "websocket" ||
		opened.Capabilities.SessionURL != "wss://welcome.example/session" {
		t.Fatalf("unexpected capabilities: %+v", opened.Capabilities)
	}

	// A region that reported no session endpoint advertises no transport.
	sandbox := harness.open(t, `{"start":"Sandbox/10/20/30"}`, harness.token)
	var sandboxSession ClientSession
	if err := json.Unmarshal(sandbox.Body.Bytes(), &sandboxSession); err != nil {
		t.Fatal(err)
	}
	if len(sandboxSession.Capabilities.Transports) != 0 || sandboxSession.Capabilities.SessionURL != "" {
		t.Fatalf("unexpected sandbox capabilities: %+v", sandboxSession.Capabilities)
	}

	// The ticket verifies against the region-ticket signer and binds the
	// region and session.
	claims, err := harness.ticket.Verify(opened.Ticket.Token, time.Now())
	if err != nil {
		t.Fatalf("verify ticket: %v", err)
	}
	if claims.RegionID != opened.Region.ID || claims.SessionID != opened.Session.ID {
		t.Fatalf("ticket binding wrong: %+v", claims)
	}
	if claims.Audience != webtoken.RegionTicketAudience || claims.Subject != testUserID {
		t.Fatalf("ticket identity wrong: %+v", claims)
	}
}

func TestClientSessionTicketIsRefusedByAccountRoutes(t *testing.T) {
	harness := newWorldEntryHarness(t)
	response := harness.open(t, `{}`, harness.token)
	var opened ClientSession
	if err := json.Unmarshal(response.Body.Bytes(), &opened); err != nil {
		t.Fatal(err)
	}

	// The audience separation is structural: presenting the region ticket as
	// an account bearer fails without any route-specific check.
	request := httptest.NewRequest(http.MethodGet, "/v1/account", nil)
	request.Header.Set("Authorization", "Bearer "+opened.Ticket.Token)
	rejected := httptest.NewRecorder()
	harness.handler.ServeHTTP(rejected, request)
	if rejected.Code != http.StatusUnauthorized {
		t.Fatalf("ticket accepted by account route: %d", rejected.Code)
	}

	// And the account token is refused by the ticket verifier the same way.
	if _, err := harness.ticket.Verify(harness.token, time.Now()); err == nil {
		t.Fatal("account token verified as a region ticket")
	}
}

func TestClientSessionTicketBindsOneRegion(t *testing.T) {
	harness := newWorldEntryHarness(t)
	welcome := harness.open(t, `{}`, harness.token)
	sandbox := harness.open(t, `{"start":"Sandbox/10/20/30"}`, harness.token)
	var first, second ClientSession
	if err := json.Unmarshal(welcome.Body.Bytes(), &first); err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(sandbox.Body.Bytes(), &second); err != nil {
		t.Fatal(err)
	}
	claimsFirst, err := harness.ticket.Verify(first.Ticket.Token, time.Now())
	if err != nil {
		t.Fatal(err)
	}
	claimsSecond, err := harness.ticket.Verify(second.Ticket.Token, time.Now())
	if err != nil {
		t.Fatal(err)
	}
	// One region's ticket names that region and no other; the region-side
	// comparison this feeds is the acceptance test in the client plan.
	if claimsFirst.RegionID == claimsSecond.RegionID {
		t.Fatalf("tickets for different regions share a region id: %q", claimsFirst.RegionID)
	}
	if second.Region.Name != "Sandbox" || second.Region.Position == nil ||
		*second.Region.Position != [3]float64{10, 20, 30} {
		t.Fatalf("unexpected named destination: %+v", second.Region)
	}
}

func TestClientSessionStoredLastLocationWins(t *testing.T) {
	harness := newWorldEntryHarness(t, func(options *Options) {
		options.Locations = &memoryLocationStore{
			last: map[string]locations.Location{testUserID: {
				UserID: testUserID, RegionID: "aaaaaaaa-0000-4000-8000-000000000002",
				Position: [3]float32{64, 65, 30}}},
		}
	})
	response := harness.open(t, `{"start":"last"}`, harness.token)
	if response.Code != http.StatusOK {
		t.Fatalf("status = %d: %s", response.Code, response.Body.String())
	}
	var opened ClientSession
	if err := json.Unmarshal(response.Body.Bytes(), &opened); err != nil {
		t.Fatal(err)
	}
	if opened.Region.Name != "Sandbox" {
		t.Fatalf("stored location ignored: %+v", opened.Region)
	}
	if opened.Region.Position == nil || *opened.Region.Position != [3]float64{64, 65, 30} {
		t.Fatalf("stored position lost: %v", opened.Region.Position)
	}
}

func TestClientSessionDivertsWhenStoredRegionOffline(t *testing.T) {
	harness := newWorldEntryHarness(t, func(options *Options) {
		options.Locations = &memoryLocationStore{
			last: map[string]locations.Location{testUserID: {
				UserID: testUserID, RegionID: "bbbbbbbb-0000-4000-8000-00000000dead",
				Position: [3]float32{64, 65, 30}}},
		}
	})
	response := harness.open(t, `{"start":"last"}`, harness.token)
	if response.Code != http.StatusOK {
		t.Fatalf("status = %d: %s", response.Code, response.Body.String())
	}
	var opened ClientSession
	if err := json.Unmarshal(response.Body.Bytes(), &opened); err != nil {
		t.Fatal(err)
	}
	// The stored region is not leased, so the welcome diversion runs and the
	// stale stored position must not leak into the diverted arrival.
	if opened.Region.Name != "Welcome" {
		t.Fatalf("expected welcome diversion, got %+v", opened.Region)
	}
	if opened.Region.Position == nil || *opened.Region.Position != [3]float64{127, 127, 23} {
		t.Fatalf("diverted position wrong: %v", opened.Region.Position)
	}
}

func TestClientSessionNamedRegionOfflineIsNotDiverted(t *testing.T) {
	harness := newWorldEntryHarness(t)
	response := harness.open(t, `{"start":"Nowhere/1/1/1"}`, harness.token)
	if response.Code != http.StatusNotFound {
		t.Fatalf("status = %d: %s", response.Code, response.Body.String())
	}
}

func TestClientSessionInvalidStartRejected(t *testing.T) {
	harness := newWorldEntryHarness(t)
	response := harness.open(t, `{"start":"not a destination"}`, harness.token)
	if response.Code != http.StatusBadRequest {
		t.Fatalf("status = %d: %s", response.Code, response.Body.String())
	}
}

func TestClientSessionWithNoLeasedRegions(t *testing.T) {
	harness := newWorldEntryHarness(t, func(options *Options) {
		options.Leases = &memoryLeaseStore{}
	})
	response := harness.open(t, `{}`, harness.token)
	if response.Code != http.StatusServiceUnavailable {
		t.Fatalf("status = %d: %s", response.Code, response.Body.String())
	}
}
