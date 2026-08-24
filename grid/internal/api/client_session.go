package api

import (
	"errors"
	"net/http"
	"net/url"
	"strings"
	"time"

	"github.com/homeworldz/server/grid/internal/arrival"
)

// World entry for the Homeworldz client (docs/CLIENT2.md, "Arrival on the grid
// user tier"): POST /v1/client/session resolves a destination region, opens a
// session in the shared session store, and mints the region-scoped ticket that
// is the only credential a region ever sees. The acting user comes from the
// bearer token and never from the path or body.

// clientSessionTTL matches the viewer session lifetime, so one revocation and
// expiry story covers both kinds of client.
const clientSessionTTL = 12 * time.Hour

// capabilityManifestVersion versions the per-region capability manifest in the
// session response. The manifest is data, not a negotiation: it states what
// this region serves, and it is re-resolved on every region crossing.
const capabilityManifestVersion = 1

type clientSessionRequest struct {
	// Start is "last" (the default), "home", or an explicit Region/x/y/z
	// arrival point. An explicitly named region that is unavailable is an
	// error rather than a diversion: the user asked for somewhere particular.
	Start string `json:"start,omitempty"`
}

// ClientSession is the world-entry response.
type ClientSession struct {
	Session      ClientSessionInfo  `json:"session"`
	Region       ClientRegion       `json:"region"`
	Ticket       ClientTicket       `json:"ticket"`
	Capabilities ClientCapabilities `json:"capabilities"`
}

type ClientSessionInfo struct {
	ID        string    `json:"id"`
	ExpiresAt time.Time `json:"expiresAt"`
}

// ClientRegion locates the destination region. Endpoint is the region's public
// endpoint; the modern session transport is not built yet, and the transports
// list in the capability manifest says so honestly.
type ClientRegion struct {
	ID       string      `json:"id"`
	Name     string      `json:"name"`
	GridX    int         `json:"gridX"`
	GridY    int         `json:"gridY"`
	Endpoint string      `json:"endpoint"`
	Position *[3]float64 `json:"position,omitempty"`
}

type ClientTicket struct {
	Token     string    `json:"token"`
	ExpiresAt time.Time `json:"expiresAt"`
}

// ClientCapabilities is the versioned per-region manifest. Transports lists
// what this region's session transport serves — ["websocket"] once the region
// reports a session endpoint, empty for a region that serves none — and
// SessionURL is where to connect. A client treats this as data and adapts
// rather than negotiating (docs/CLIENT2-TRANSPORT.md).
type ClientCapabilities struct {
	Version    int      `json:"version"`
	Transports []string `json:"transports"`
	SessionURL string   `json:"sessionURL,omitempty"`
}

func (a *API) clientSession(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		w.Header().Set("Allow", "POST")
		writeError(w, http.StatusMethodNotAllowed, Error{Code: "method_not_allowed", Message: "only POST is supported"})
		return
	}
	account, ok := a.requireAuth(w, r)
	if !ok {
		return
	}
	if a.sessions == nil || a.leases == nil || a.ticketSigner == nil {
		writeError(w, http.StatusServiceUnavailable, Error{Code: "world_entry_unavailable", Message: "world entry is not available"})
		return
	}
	var request clientSessionRequest
	if !decodeJSON(w, r, &request) {
		return
	}

	destination, position, ok := a.resolveClientDestination(w, r, account.ID, request.Start)
	if !ok {
		return
	}
	endpoint, err := url.Parse(destination.Region.PublicEndpoint)
	if err != nil || endpoint.Hostname() == "" {
		writeError(w, http.StatusServiceUnavailable, Error{Code: "destination_unavailable", Message: "the destination region endpoint is invalid"})
		return
	}

	session, err := a.sessions.CreateClientSession(r.Context(), account.ID, destination.Region.ID, clientSessionTTL)
	if err != nil {
		a.internalError(w, r, "create client session", err)
		return
	}
	// The arrival position rides the ticket so the region learns where world
	// entry placed this session without taking a spawn position from the
	// client (docs/CLIENT2-EMBODIMENT.md milestone E2).
	var arrival []float64
	if position != nil {
		arrival = []float64{position[0], position[1], position[2]}
	}
	ticket, ticketExpiry, err := a.ticketSigner.SignRegionTicket(time.Now(), account.ID, account.Userid,
		account.DisplayName, account.RezDate, account.Privileges, account.AuthVersion,
		destination.Region.ID, session.ID, arrival)
	if err != nil {
		a.internalError(w, r, "sign region ticket", err)
		return
	}

	w.Header().Set("Cache-Control", "no-store")
	writeJSON(w, http.StatusOK, ClientSession{
		Session: ClientSessionInfo{ID: session.ID, ExpiresAt: session.ExpiresAt},
		Region: ClientRegion{
			ID:       destination.Region.ID,
			Name:     destination.Region.Name,
			GridX:    destination.Region.GridX,
			GridY:    destination.Region.GridY,
			Endpoint: a.regionEndpointFor(destination.Region.ID, destination.Region.PublicEndpoint),
			Position: position,
		},
		Ticket:       ClientTicket{Token: ticket, ExpiresAt: ticketExpiry},
		Capabilities: capabilitiesOf(destination.Region.SessionEndpoint),
	})
}

// capabilitiesOf derives the per-region manifest from what the region
// reported at registration: a session endpoint means the WebSocket transport
// is served there, and none means the region predates or disables it.
func capabilitiesOf(sessionEndpoint string) ClientCapabilities {
	capabilities := ClientCapabilities{Version: capabilityManifestVersion, Transports: []string{}}
	if sessionEndpoint != "" {
		capabilities.Transports = []string{"websocket"}
		capabilities.SessionURL = sessionEndpoint
	}
	return capabilities
}

// resolveClientDestination picks the destination region and arrival position
// for a session. It writes the error response itself when resolution fails.
func (a *API) resolveClientDestination(w http.ResponseWriter, r *http.Request,
	userID, start string) (arrival.Destination, *[3]float64, bool) {
	failed := func(code int, slug, message string) (arrival.Destination, *[3]float64, bool) {
		writeError(w, code, Error{Code: slug, Message: message})
		return arrival.Destination{}, nil, false
	}

	normalized := strings.TrimSpace(start)
	switch strings.ToLower(normalized) {
	case "", "last", "home":
		preferred, position := "", (*[3]float64)(nil)
		if a.locations != nil {
			lookup := a.locations.Get
			if strings.EqualFold(normalized, "home") {
				lookup = a.locations.GetHome
			}
			if location, err := lookup(r.Context(), userID); err == nil {
				preferred = location.RegionID
				position = &[3]float64{
					float64(location.Position[0]), float64(location.Position[1]), float64(location.Position[2])}
			}
		}
		destination, err := arrival.Resolve(r.Context(), a.leases, preferred, a.welcome)
		if err != nil {
			return failed(http.StatusServiceUnavailable, "destination_unavailable",
				"no online region can accept this session")
		}
		// A stored position only holds if the stored region is the one selected;
		// a welcome diversion supplies its own.
		if preferred == "" || destination.Region.ID != preferred {
			position = nil
		}
		if destination.Position != nil {
			position = &[3]float64{
				float64(destination.Position.X), float64(destination.Position.Y), float64(destination.Position.Z)}
		}
		return destination, position, true
	default:
		point, err := arrival.ParsePoint(normalized)
		if err != nil {
			return failed(http.StatusBadRequest, "invalid_start",
				`start must be "last", "home", or Region/x/y/z`)
		}
		position := &[3]float64{float64(point.X), float64(point.Y), float64(point.Z)}
		destination, err := arrival.ResolveNamed(r.Context(), a.leases, point.Region)
		if errors.Is(err, arrival.ErrNoDestination) {
			// Not a region name. It may still be the name of a facet, which is
			// a whole region to whoever asked — and is what a border crossing
			// asks for whenever the region beyond the line is a rectangle.
			if facetDestination, facet, ok := a.resolveFacetNamed(r.Context(), point.Region); ok {
				if defined, found := a.definedRegion(r.Context(), facetDestination.Region.ID); found {
					position = rebaseFacetPosition(defined, facet, position)
				}
				return facetDestination, position, true
			}
			return failed(http.StatusNotFound, "destination_unavailable",
				"the requested region is not online")
		}
		if err != nil {
			return failed(http.StatusServiceUnavailable, "destination_unavailable",
				"no online region can accept this session")
		}
		return destination, position, true
	}
}

// regionEndpointFor answers where the caller should reach this region's HTTP
// API. A browser cannot fetch http:// from an https:// page, and a region
// cannot serve TLS, so a deployment that terminates TLS in front of its
// regions configures the base and every region is reached through it by id.
// Without that configuration the region's own endpoint is handed out, which is
// what a deployment where the two are the same wants.
//
// By id rather than by name deliberately: a name can change, and one region's
// name can be a prefix of another's — routing /session/nova* in front of
// /session/novab* silently sent every Nova B connection to Nova (2026-08-24).
func (a *API) regionEndpointFor(regionID, publicEndpoint string) string {
	if a.regionPublicBase == "" || regionID == "" {
		return strings.TrimRight(publicEndpoint, "/")
	}
	return a.regionPublicBase + "/" + regionID
}
