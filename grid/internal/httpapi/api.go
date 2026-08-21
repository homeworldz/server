package httpapi

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"

	"github.com/homeworldz/server/grid/internal/arrival"
	"github.com/homeworldz/server/grid/internal/assetmeta"
	"github.com/homeworldz/server/grid/internal/attachments"
	"github.com/homeworldz/server/grid/internal/durability"
	"github.com/homeworldz/server/grid/internal/estate"
	"github.com/homeworldz/server/grid/internal/gestures"
	"github.com/homeworldz/server/grid/internal/identity"
	"github.com/homeworldz/server/grid/internal/inventory"
	"github.com/homeworldz/server/grid/internal/locations"
	"github.com/homeworldz/server/grid/internal/presence"
	"github.com/homeworldz/server/grid/internal/provisioning"
	"github.com/homeworldz/server/grid/internal/regions"
	"github.com/homeworldz/server/grid/internal/renditions"
	"github.com/homeworldz/server/grid/internal/tasktransfer"
	"github.com/homeworldz/server/grid/internal/transit"
	"github.com/homeworldz/server/grid/internal/vault"
	"github.com/homeworldz/server/grid/internal/webtoken"
)

type ReadinessChecker interface {
	PingContext(context.Context) error
}

type API struct {
	ready         ReadinessChecker
	version       string
	publicURL     string
	gridName      string
	aboutURL      string
	supportURL    string
	registerURL   string
	passwordURL   string
	websiteAPIURL string
	welcomeText   string
	logger        *slog.Logger
	regions       regions.Store
	identity      identity.Store
	presence      presence.Store
	inventory     inventory.Store
	assets        assetmeta.Store
	renditions    renditions.Store
	workerToken   string
	durability    *durability.Keeper
	vault         vault.Store
	serviceToken  string
	provisioned   provisioning.Store
	terrainHTTP   *http.Client
	// Outbound to regions, for telling one that a wearer changed clothes.
	outfitHTTP     *http.Client
	terrainCache   terrainTileCache
	layerCache     terrainLayerCache
	transits       transit.Store
	taskTransfers  tasktransfer.Store
	locations      locations.Store
	gestures       gestures.Store
	attachments    attachments.Store
	estates        estate.Store
	welcomePoints  []arrival.Point
	ticketVerifier *webtoken.Signer
}

// regionExtents is a region's footprint in metres, one extent per axis; a
// rectangle (ADR 0036) is wider than it is deep or vice versa, so callers
// bounds-check each axis against its own extent.
func (a *API) regionExtents(ctx context.Context, id string) (float32, float32) {
	if a.provisioned != nil {
		if region, err := a.provisioned.Get(ctx, id); err == nil {
			return float32(region.SizeX * 256), float32(region.SizeY * 256)
		}
	}
	return 256, 256
}

type Options struct {
	ServiceToken  string
	GridPublicURL string
	GridName      string
	// AboutURL, SupportURL, RegisterURL and PasswordURL are the human-facing
	// pages published in get_grid_info. Each is omitted from the document when
	// empty, so an unconfigured grid advertises nothing rather than a dead link.
	AboutURL    string
	SupportURL  string
	RegisterURL string
	PasswordURL string
	// WelcomeMessage is the grid-wide login greeting template ([grid]
	// welcome_message; {grid} and {user} placeholders); empty disables the
	// login reply's message.
	WelcomeMessage string
	Logger         *slog.Logger
	Regions        regions.Store
	Identity       identity.Store
	Presence       presence.Store
	Inventory      inventory.Store
	Assets         assetmeta.Store
	Vault          vault.Store
	// Renditions stores derived encodings and the conversion queue
	// (ADR 0033); WorkerToken is the conversion-worker credential that
	// gates writing them. See renditions.go for why it is not the
	// service token.
	Renditions        renditions.Store
	WorkerToken       string
	Provisioned       provisioning.Store
	TerrainHTTPClient *http.Client
	// Outbound to regions for outfit-change notifications. Defaulted when nil;
	// a test supplies its own to see what would have been sent.
	OutfitHTTPClient *http.Client
	Transits         transit.Store
	TaskTransfers    tasktransfer.Store
	Locations        locations.Store
	Gestures         gestures.Store
	Attachments      attachments.Store
	Estates          estate.Store
	// Welcome is the ordered new-arrival list ([grid] welcome_locations),
	// shared with the client's world entry: where a viewer login lands when
	// no stored location decides it, and where it is diverted when the stored
	// region is offline. Empty preserves the legacy first-region fallback.
	Welcome []arrival.Point
	// TicketVerifier verifies region tickets on behalf of regions (the
	// signing secret never leaves the grid). It must carry the region-ticket
	// audience. Nil disables /region-runtime/{id}/validate-ticket.
	TicketVerifier *webtoken.Signer
	// Stats serves the public daily-summary CSV at /stats.csv. Nil leaves
	// the path unrouted.
	Stats http.Handler
	// WebsiteAPIURL is the public base of the browser-facing API ([website]
	// public_url), named in the API catalog (RFC 9727). Empty omits that
	// catalog entry rather than advertising a dead anchor.
	WebsiteAPIURL string
}

func New(ready ReadinessChecker, version string, options Options) http.Handler {
	a := &API{ready: ready, version: version, publicURL: strings.TrimRight(options.GridPublicURL, "/"),
		gridName:      strings.TrimSpace(options.GridName),
		aboutURL:      strings.TrimSpace(options.AboutURL),
		supportURL:    strings.TrimSpace(options.SupportURL),
		registerURL:   strings.TrimSpace(options.RegisterURL),
		passwordURL:   strings.TrimSpace(options.PasswordURL),
		websiteAPIURL: strings.TrimRight(strings.TrimSpace(options.WebsiteAPIURL), "/"),
		logger:        options.Logger,
		regions:       options.Regions, identity: options.Identity, presence: options.Presence,
		inventory: options.Inventory, assets: options.Assets, vault: options.Vault,
		renditions: options.Renditions, workerToken: options.WorkerToken,
		serviceToken: options.ServiceToken,
		provisioned:  options.Provisioned, terrainHTTP: options.TerrainHTTPClient,
		outfitHTTP:   options.OutfitHTTPClient,
		terrainCache: newTerrainTileCache(), layerCache: newTerrainLayerCache(), transits: options.Transits,
		taskTransfers: options.TaskTransfers, locations: options.Locations,
		gestures: options.Gestures, attachments: options.Attachments, estates: options.Estates,
		welcomePoints: options.Welcome, ticketVerifier: options.TicketVerifier,
		welcomeText: options.WelcomeMessage}
	if a.outfitHTTP == nil {
		a.outfitHTTP = &http.Client{Timeout: 10 * time.Second}
	}
	if a.publicURL == "" {
		a.publicURL = "http://127.0.0.1:42000"
	}
	if a.gridName == "" {
		a.gridName = "Homeworldz"
	}
	// The inventory-commit invariant of ADR 0026 is installed here, around the
	// store, rather than left to each handler: every path that commits an
	// inventory reference goes through it, including ones written later. A
	// deployment without a vault or an asset registry keeps the bare store, so
	// tools and tests that have neither still work — but the grid proper always
	// configures both, and then no inventory row can name bytes the vault has
	// not vouched for.
	if a.inventory != nil && a.vault != nil && a.assets != nil {
		a.durability = durability.New(a.assets, a.vault, options.ServiceToken,
			&http.Client{Timeout: 30 * time.Second}).WithLogger(options.Logger)
		a.inventory = inventory.WithDurability(a.inventory, a.durability)
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/get_grid_info", getOnly(a.gridInfo))
	mux.HandleFunc("/welcome", getOnly(a.welcome))
	mux.HandleFunc("/assets/homeworldz.svg", getOnly(a.logo))
	mux.HandleFunc("/map/", getOnly(a.mapTile))
	mux.HandleFunc("/ping", getOnly(a.ping))
	mux.HandleFunc("/ready", getOnly(a.readiness))
	mux.HandleFunc("/version", getOnly(a.buildVersion))
	mux.HandleFunc("/.well-known/api-catalog", getOnly(a.apiCatalog))
	mux.HandleFunc("/openapi.yaml", getOnly(a.apiSpecification("openapi.yaml")))
	mux.HandleFunc("/openapi-public.yaml", getOnly(a.apiSpecification("openapi-public.yaml")))
	// The raw CSV lives at /stats.csv; /stats itself is reserved for the
	// human-facing stats page to come.
	if options.Stats != nil {
		mux.Handle("/stats.csv", options.Stats)
	}
	mux.HandleFunc("/login", a.viewerLogin)
	mux.HandleFunc("/caps/inventory/descendents/", a.inventoryDescendentsCapability)
	mux.HandleFunc("/caps/inventory/library-descendents/", a.libraryDescendentsCapability)
	mux.HandleFunc("/caps/inventory/items/", a.inventoryItemsCapability)
	mux.HandleFunc("/caps/inventory/create-folder/", a.createInventoryFolderCapability)
	mux.HandleFunc("/caps/inventory/ais/", a.inventoryAISCapability)
	mux.HandleFunc("/caps/inventory/library/", a.libraryAISCapability)
	mux.HandleFunc("/api/v1/regions", a.regionsRoot)
	mux.HandleFunc("/api/v1/regions/", a.regionByID)
	mux.HandleFunc("/api/v1/provisioned-regions", a.provisionedRegionsRoot)
	mux.HandleFunc("/api/v1/provisioned-regions/", a.provisionedRegionByID)
	mux.HandleFunc("/api/v1/region-runtime/", a.provisionedRegionRuntime)
	mux.HandleFunc("/api/v1/users", a.usersRoot)
	mux.HandleFunc("/api/v1/users/", a.userByID)
	mux.HandleFunc("/api/v1/sessions", a.sessionsRoot)
	mux.HandleFunc("/api/v1/sessions/", a.sessionByID)
	mux.HandleFunc("/api/v1/presence", a.presenceRoot)
	mux.HandleFunc("/api/v1/presence/", a.presenceByUser)
	mux.HandleFunc("/api/v1/locations/", a.locationByUser)
	mux.HandleFunc("/api/v1/gestures/", a.gesturesByUser)
	mux.HandleFunc("/api/v1/attachments/", a.attachmentsByUser)
	mux.HandleFunc("/api/v1/inventory/", a.inventoryByUser)
	mux.HandleFunc("/api/v1/assets", a.assetsRoot)
	mux.HandleFunc("/api/v1/assets/", a.assetByID)
	mux.HandleFunc("/api/v1/vault/assets/", a.vaultAsset)
	mux.HandleFunc("/api/v1/rendition-jobs/", a.renditionJobs)
	mux.HandleFunc("/api/v1/transits", a.transitsRoot)
	mux.HandleFunc("/api/v1/transits/", a.transitByID)
	mux.HandleFunc("/api/v1/task-transfers", a.taskTransfersRoot)
	mux.HandleFunc("/api/v1/task-transfers/", a.taskTransferByID)
	mux.HandleFunc("/api/v1/task-extractions", a.taskExtractionsRoot)
	mux.HandleFunc("/api/v1/task-extractions/", a.taskExtractionByID)
	mux.HandleFunc("/api/v1/object-rezzes", a.objectRezzesRoot)
	mux.HandleFunc("/api/v1/object-rezzes/", a.objectRezByID)
	mux.HandleFunc("/", a.root)
	return withRequestID(withRequestLogging(
		authenticateInternal(mux, options.ServiceToken, options.WorkerToken), options.Logger,
	))
}

// gridRegionProtocol is the grid-region protocol version this grid requires
// (docs/CLIENT2.md, "the region protocol version"). It increments only when a
// change genuinely requires region software to be upgraded, and a region
// reporting a different version is refused registration. A region reporting
// nothing predates the handshake and is accepted, so enforcement began as a
// no-op; that allowance is removed once deployed regions all report.
const gridRegionProtocol = 1

// regionProtocolAccepted enforces the match rule, writing the refusal that
// names both versions when a reported protocol differs. It returns true when
// the caller should proceed.
func regionProtocolAccepted(w http.ResponseWriter, reported int) bool {
	if reported == 0 || reported == gridRegionProtocol {
		return true
	}
	remedy := "upgrade the region software"
	if reported > gridRegionProtocol {
		remedy = "the grid is behind this region software"
	}
	writeJSON(w, http.StatusConflict, Error{
		Code: "region_protocol_mismatch",
		Message: fmt.Sprintf("region is running grid-region protocol %d; this grid requires %d — %s",
			reported, gridRegionProtocol, remedy),
	})
	return false
}

// validSessionEndpoint accepts an absent session endpoint, or a ws:// or
// wss:// URL with a host — WebSocket is the region-session transport by
// decision (docs/CLIENT2-TRANSPORT.md).
func validSessionEndpoint(w http.ResponseWriter, endpoint string) bool {
	if endpoint == "" {
		return true
	}
	parsed, err := url.Parse(endpoint)
	if err == nil && (parsed.Scheme == "ws" || parsed.Scheme == "wss") && parsed.Host != "" {
		return true
	}
	writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_session_endpoint",
		Message: "sessionEndpoint must be a ws:// or wss:// URL"})
	return false
}

func (a *API) provisionedRegionRuntime(w http.ResponseWriter, r *http.Request) {
	if a.regions == nil || a.provisioned == nil {
		writeJSON(w, http.StatusServiceUnavailable, Error{Code: "region_registration_unavailable", Message: "provisioned region registration is unavailable"})
		return
	}
	parts := strings.Split(strings.Trim(strings.TrimPrefix(r.URL.Path, "/api/v1/region-runtime/"), "/"), "/")
	if len(parts) == 0 || strings.TrimSpace(parts[0]) == "" || len(parts[0]) > 128 {
		a.notFound(w, r)
		return
	}
	scheme, accessKey, found := strings.Cut(r.Header.Get("Authorization"), " ")
	provisioned, authenticated := a.provisioned.Authenticate(r.Context(), parts[0], accessKey)
	if !found || !strings.EqualFold(scheme, "Bearer") || !authenticated {
		w.Header().Set("WWW-Authenticate", "Bearer")
		writeJSON(w, http.StatusUnauthorized, Error{Code: "unauthorized_region", Message: "the region UUID or access key is invalid"})
		return
	}
	// A disabled region may do exactly one thing: give up its lease. Anything
	// else is refused, and refused distinctly from a bad credential, because
	// "you are switched off" and "your key is wrong" send an operator to
	// entirely different places.
	//
	// The one permitted call matters more than it looks. A provisioned
	// region's lease row reserves its coordinates and is not purged when it
	// expires, so a disabled region that cannot deregister leaves that
	// reservation standing — and the next region provisioned there cannot
	// register at all (found the hard way, 2026-08-21).
	if !provisioned.Enabled && r.Method != http.MethodDelete {
		writeJSON(w, http.StatusForbidden, Error{Code: "region_disabled",
			Message: "the region is disabled; it may deregister but not run"})
		return
	}
	id := provisioned.ID
	if len(parts) == 1 && r.Method == http.MethodPost {
		var request StartProvisionedRegionRequest
		if !decodeJSON(w, r, &request) {
			return
		}
		lease, ok := validateLease(w, request.LeaseSeconds)
		publicEndpoint := request.PublicEndpoint
		if provisioned.PublicEndpoint != "" {
			publicEndpoint = provisioned.PublicEndpoint
		}
		viewerPort := request.ViewerPort
		if provisioned.ViewerPort != 0 {
			viewerPort = provisioned.ViewerPort
		}
		validation := RegisterRegionRequest{Name: provisioned.Name, GridX: provisioned.MapX, GridY: provisioned.MapY,
			PublicEndpoint: publicEndpoint, ViewerPort: viewerPort}
		if !ok || !validateRegistration(w, validation) || !regionProtocolAccepted(w, request.RegionProtocol) ||
			!validSessionEndpoint(w, request.SessionEndpoint) {
			return
		}
		region, err := a.regions.RegisterProvisioned(r.Context(), id, regions.Registration{
			Name: provisioned.Name, GridX: provisioned.MapX, GridY: provisioned.MapY,
			PublicEndpoint: publicEndpoint, ViewerPort: viewerPort, LeaseDuration: lease,
			SessionEndpoint: request.SessionEndpoint,
		})
		if errors.Is(err, regions.ErrConflict) {
			writeJSON(w, http.StatusConflict, Error{Code: "region_coordinates_in_use", Message: "region coordinates are already leased"})
		} else if err != nil {
			writeJSON(w, http.StatusInternalServerError, Error{Code: "region_store_error", Message: "region registration failed"})
		} else {
			result := ProvisionedRegionRuntimeResult{
				Region: region, GridName: a.gridName, GridPublicURL: a.publicURL,
				SizeX: provisioned.SizeX * 256, SizeY: provisioned.SizeY * 256,
				Maturity: provisioned.Maturity, OwnerUserID: provisioned.OwnerUserID,
				Facets:         facetsOf(provisioned, viewerPort),
				RegionProtocol: gridRegionProtocol}
			if a.estates != nil {
				if est, eerr := a.estates.ForRegion(r.Context(), id, provisioned.OwnerUserID); eerr == nil {
					result.Estate = &est
				}
			}
			writeJSON(w, http.StatusOK, result)
		}
		return
	}
	if len(parts) >= 2 && parts[1] == "estate" {
		a.provisionedRegionEstate(w, r, id, provisioned.OwnerUserID, parts)
		return
	}
	if len(parts) == 2 && parts[1] == "validate-ticket" && r.Method == http.MethodPost {
		a.validateRegionTicket(w, r, id)
		return
	}
	if len(parts) == 2 && parts[1] == "lease" && r.Method == http.MethodPut {
		var request RenewRegionLeaseRequest
		if !decodeJSON(w, r, &request) {
			return
		}
		lease, ok := validateLease(w, request.LeaseSeconds)
		if !ok || !regionProtocolAccepted(w, request.RegionProtocol) {
			return
		}
		region, err := a.regions.RenewProvisioned(r.Context(), id, lease)
		a.writeRegionResult(w, region, err)
		return
	}
	if len(parts) == 1 && r.Method == http.MethodDelete {
		err := a.regions.DeregisterProvisioned(r.Context(), id)
		if err != nil && !errors.Is(err, regions.ErrNotFound) {
			writeJSON(w, http.StatusInternalServerError, Error{Code: "region_store_error", Message: "region deregistration failed"})
		} else {
			w.WriteHeader(http.StatusNoContent)
		}
		return
	}
	a.notFound(w, r)
}

// facetsOf lists a provisioned region's facets for its registration reply,
// with ports counted from the effective (possibly operator-overridden) base
// viewer port so the region binds exactly what the grid advertises.
func facetsOf(provisioned provisioning.Region, baseViewerPort int) []RegionFacet {
	facets := make([]RegionFacet, 0, provisioned.FacetCount())
	for index := 0; index < provisioned.FacetCount(); index++ {
		originX, originY := provisioned.FacetOrigin(index)
		facets = append(facets, RegionFacet{
			Index: index, Name: provisioned.FacetNameAt(index),
			GridX: originX, GridY: originY, Edge: provisioned.FacetEdge() * 256,
			ViewerPort: baseViewerPort + index})
	}
	return facets
}

// provisionedRegionEstate handles the estate sub-routes of an authenticated
// region: GET/POST /api/v1/region-runtime/{id}/estate (fetch/update settings) and
// POST /api/v1/region-runtime/{id}/estate/members (add/remove an access-list member).
// The region has already authenticated with its access key; it is responsible for
// checking the acting agent is the estate owner or a manager before calling.
func (a *API) provisionedRegionEstate(w http.ResponseWriter, r *http.Request, regionID,
	ownerUserID string, parts []string) {
	if a.estates == nil {
		writeJSON(w, http.StatusServiceUnavailable, Error{Code: "estate_unavailable", Message: "estate service is unavailable"})
		return
	}
	current, err := a.estates.ForRegion(r.Context(), regionID, ownerUserID)
	if writeEstateError(w, err) {
		return
	}
	if len(parts) == 2 {
		switch r.Method {
		case http.MethodGet:
			writeJSON(w, http.StatusOK, EstateResult{Estate: current})
		case http.MethodPost:
			var request EstateSettingsRequest
			if !decodeJSON(w, r, &request) {
				return
			}
			updated, err := a.estates.UpdateSettings(r.Context(), current.ID, request.toUpdate())
			if !writeEstateError(w, err) {
				writeJSON(w, http.StatusOK, EstateResult{Estate: updated})
			}
		default:
			w.Header().Set("Allow", "GET, POST")
			writeJSON(w, http.StatusMethodNotAllowed, Error{Code: "method_not_allowed", Message: "only GET and POST are supported"})
		}
		return
	}
	if len(parts) == 3 && parts[2] == "members" && r.Method == http.MethodPost {
		var request EstateMemberRequest
		if !decodeJSON(w, r, &request) {
			return
		}
		updated, err := a.estates.SetMember(r.Context(), current.ID, request.MemberID, request.Role, request.Present)
		if !writeEstateError(w, err) {
			writeJSON(w, http.StatusOK, EstateResult{Estate: updated})
		}
		return
	}
	a.notFound(w, r)
}

func writeEstateError(w http.ResponseWriter, err error) bool {
	if err == nil {
		return false
	}
	switch {
	case errors.Is(err, estate.ErrNotFound):
		writeJSON(w, http.StatusNotFound, Error{Code: "estate_not_found", Message: "estate was not found"})
	case errors.Is(err, estate.ErrInvalid):
		writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_estate", Message: "estate request is invalid"})
	default:
		writeJSON(w, http.StatusInternalServerError, Error{Code: "estate_error", Message: "estate operation failed"})
	}
	return true
}

func getOnly(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			w.Header().Set("Allow", http.MethodGet)
			writeJSON(w, http.StatusMethodNotAllowed, Error{
				Code: "method_not_allowed", Message: "only GET is supported",
			})
			return
		}
		next(w, r)
	}
}

func (a *API) ping(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, Status{Status: "ok"})
}

func (a *API) readiness(w http.ResponseWriter, r *http.Request) {
	if a.ready == nil {
		writeJSON(w, http.StatusServiceUnavailable, Error{
			Code: "database_unconfigured", Message: "database is not configured",
		})
		return
	}

	ctx, cancel := context.WithTimeout(r.Context(), 2*time.Second)
	defer cancel()
	if err := a.ready.PingContext(ctx); err != nil {
		writeJSON(w, http.StatusServiceUnavailable, Error{
			Code: "database_unavailable", Message: "database is unavailable",
		})
		return
	}
	writeJSON(w, http.StatusOK, Status{Status: "ready"})
}

func (a *API) buildVersion(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, Version{
		Service: "grid", Version: a.version, APIVersion: APIVersion,
	})
}

func (a *API) notFound(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusNotFound, Error{Code: "not_found", Message: "route not found"})
}

// root answers the bare site root with a small index naming the grid and its
// public artifacts — a person landing on the service host sees where things
// are instead of a 404, and automated discovery scanners accept the site as
// existing. Every other unrouted path stays a 404; this handler is the mux's
// catch-all, so it must tell the two apart itself.
func (a *API) root(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		a.notFound(w, r)
		return
	}
	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		w.Header().Set("Allow", "GET, HEAD")
		writeJSON(w, http.StatusMethodNotAllowed, Error{
			Code: "method_not_allowed", Message: "the index answers GET"})
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	page := "<!doctype html>\n<html lang=\"en\">\n<head><meta charset=\"utf-8\">" +
		"<title>" + a.gridName + " grid</title></head>\n<body>\n" +
		"<h1>" + a.gridName + " grid service</h1>\n" +
		"<p>Version " + a.version + ".</p>\n<ul>\n" +
		"<li><a href=\"/get_grid_info\">Grid information</a></li>\n" +
		"<li><a href=\"/stats.csv\">Daily statistics</a></li>\n" +
		"<li><a href=\"/.well-known/api-catalog\">API catalog</a> (RFC 9727)</li>\n" +
		"<li><a href=\"/openapi.yaml\">Grid API description</a></li>\n" +
		"<li><a href=\"/openapi-public.yaml\">Public API description</a></li>\n"
	if a.aboutURL != "" {
		page += "<li><a href=\"" + a.aboutURL + "\">About</a></li>\n"
	}
	page += "</ul>\n</body>\n</html>\n"
	_, _ = w.Write([]byte(page))
}

func (a *API) regionsRoot(w http.ResponseWriter, r *http.Request) {
	if a.regions == nil {
		writeJSON(w, http.StatusServiceUnavailable, Error{Code: "region_store_unavailable", Message: "region storage is unavailable"})
		return
	}
	switch r.Method {
	case http.MethodGet:
		items, err := a.regions.List(r.Context())
		if err != nil {
			writeJSON(w, http.StatusInternalServerError, Error{Code: "region_store_error", Message: "region discovery failed"})
			return
		}
		writeJSON(w, http.StatusOK, RegionList{Regions: items})
	default:
		w.Header().Set("Allow", "GET")
		writeJSON(w, http.StatusMethodNotAllowed, Error{Code: "method_not_allowed", Message: "only GET is supported"})
	}
}

func (a *API) regionByID(w http.ResponseWriter, r *http.Request) {
	if a.regions == nil {
		writeJSON(w, http.StatusServiceUnavailable, Error{Code: "region_store_unavailable", Message: "region storage is unavailable"})
		return
	}
	parts := strings.Split(strings.TrimPrefix(r.URL.Path, "/api/v1/regions/"), "/")
	if len(parts) == 1 && parts[0] == "lookup" {
		a.regionLookup(w, r)
		return
	}
	if len(parts) == 1 && parts[0] == "topology" {
		if r.Method != http.MethodGet {
			w.Header().Set("Allow", http.MethodGet)
			writeJSON(w, http.StatusMethodNotAllowed, Error{Code: "method_not_allowed", Message: "only GET is supported"})
			return
		}
		topology, err := a.gridTopology(r.Context())
		if err != nil {
			writeJSON(w, http.StatusInternalServerError, Error{Code: "region_store_error", Message: "region discovery failed"})
			return
		}
		writeJSON(w, http.StatusOK, RegionTopologyList{Regions: topology})
		return
	}
	if len(parts) == 0 || !validUUID(parts[0]) {
		a.notFound(w, r)
		return
	}
	id := parts[0]
	if len(parts) == 1 && r.Method == http.MethodGet {
		region, err := a.regions.Get(r.Context(), id)
		a.writeRegionResult(w, region, err)
		return
	}
	if len(parts) == 2 && parts[1] == "neighbors" {
		if r.Method != http.MethodGet {
			w.Header().Set("Allow", http.MethodGet)
			writeJSON(w, http.StatusMethodNotAllowed, Error{Code: "method_not_allowed", Message: "only GET is supported"})
			return
		}
		a.regionNeighbors(w, r, id)
		return
	}
	if len(parts) == 1 {
		w.Header().Set("Allow", "GET")
		writeJSON(w, http.StatusMethodNotAllowed, Error{Code: "method_not_allowed", Message: "only GET is supported"})
		return
	}
	a.notFound(w, r)
}

// gridTopology is where every region the grid knows about sits and how big it
// is — provisioned placement, overlaid with the live endpoint of the regions
// currently holding a lease. Adjacency (neighbors) and destination resolution
// (teleports) are two views of this one list, so they cannot disagree about
// where a region is.
//
// A rectangular region (ADR 0036) appears here as one entry per facet: the
// square viewer-facing regions its process serves, each with its own name and
// consecutive viewer port. Every consumer of this list — neighbors, teleport
// lookup, map blocks — addresses what a viewer can render, and a viewer only
// ever renders the facets. Facet entries share the region's id; facet 0 sits
// at the region's own corner and carries its own name, so square regions are
// exactly one unchanged entry.
func (a *API) gridTopology(ctx context.Context) ([]RegionTopology, error) {
	liveItems, err := a.regions.List(ctx)
	if err != nil {
		return nil, err
	}
	if a.provisioned == nil {
		topology := make([]RegionTopology, 0, len(liveItems))
		for _, item := range liveItems {
			topology = append(topology, RegionTopology{ID: item.ID, Name: item.Name,
				GridX: item.GridX, GridY: item.GridY, SizeX: 256, SizeY: 256, Maturity: 0,
				PublicEndpoint: item.PublicEndpoint, ViewerPort: item.ViewerPort, Online: true,
				SessionEndpoint: item.SessionEndpoint})
		}
		return topology, nil
	}
	liveByID := make(map[string]regions.Region, len(liveItems))
	for _, item := range liveItems {
		liveByID[item.ID] = item
	}
	provisioned, err := a.provisioned.List(ctx)
	if err != nil {
		return nil, err
	}
	topology := make([]RegionTopology, 0, len(provisioned))
	for _, item := range provisioned {
		endpoint, viewerPort, online := item.PublicEndpoint, item.ViewerPort, false
		sessionEndpoint := ""
		if live, isLive := liveByID[item.ID]; isLive {
			endpoint, viewerPort, online = live.PublicEndpoint, live.ViewerPort, true
			sessionEndpoint = live.SessionEndpoint
		}
		edge := item.FacetEdge() * 256
		for facet := 0; facet < item.FacetCount(); facet++ {
			originX, originY := item.FacetOrigin(facet)
			topology = append(topology, RegionTopology{
				ID: item.ID, Name: item.FacetNameAt(facet), Facet: facet,
				GridX: originX, GridY: originY,
				SizeX: edge, SizeY: edge, Maturity: item.Maturity,
				PublicEndpoint: endpoint, ViewerPort: viewerPort + facet,
				Online: online, SessionEndpoint: sessionEndpoint})
		}
	}
	return topology, nil
}

// regionLookup answers GET /api/v1/regions/lookup?id=<uuid> or ?gridX=&gridY=,
// returning one region's placement. Regions resolve teleport destinations
// here: the neighbor list knows only adjacent regions, while a map, landmark,
// or home teleport can name any region on the grid. Offline regions are
// returned too, with online false, so the destination can be reported as down
// rather than as not existing.
func (a *API) regionLookup(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		w.Header().Set("Allow", http.MethodGet)
		writeJSON(w, http.StatusMethodNotAllowed, Error{Code: "method_not_allowed", Message: "only GET is supported"})
		return
	}
	query := r.URL.Query()
	id := query.Get("id")
	gridXText, gridYText := query.Get("gridX"), query.Get("gridY")
	byPoint := gridXText != "" || gridYText != ""
	gridX, xErr := strconv.Atoi(gridXText)
	gridY, yErr := strconv.Atoi(gridYText)
	if (id != "") == byPoint {
		writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_lookup",
			Message: "provide either id or both gridX and gridY"})
		return
	}
	if id != "" && !validUUID(id) {
		writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_lookup", Message: "id must be a UUID"})
		return
	}
	if byPoint && (xErr != nil || yErr != nil || gridX < 0 || gridY < 0) {
		writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_lookup",
			Message: "gridX and gridY must be non-negative integers"})
		return
	}
	topology, err := a.gridTopology(r.Context())
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, Error{Code: "region_store_error", Message: "region discovery failed"})
		return
	}
	for _, candidate := range topology {
		// A point names a square the destination covers, not necessarily its
		// corner: a var region covers several, and a viewer teleporting to one
		// of them means the region that owns it.
		found := candidate.ID == id ||
			(byPoint && gridX >= candidate.GridX && gridX < candidate.GridX+candidate.SizeX/256 &&
				gridY >= candidate.GridY && gridY < candidate.GridY+candidate.SizeY/256)
		if found {
			writeJSON(w, http.StatusOK, candidate)
			return
		}
	}
	writeJSON(w, http.StatusNotFound, Error{Code: "region_not_found", Message: "no region occupies that location"})
}

func (a *API) regionNeighbors(w http.ResponseWriter, r *http.Request, id string) {
	source, err := a.regions.Get(r.Context(), id)
	if err != nil {
		a.writeRegionResult(w, regions.Region{}, err)
		return
	}
	sourceSizeX, sourceSizeY := 1, 1
	if a.provisioned != nil {
		provisionedSource, sourceErr := a.provisioned.Get(r.Context(), id)
		if sourceErr != nil {
			writeProvisioningError(w, sourceErr)
			return
		}
		sourceSizeX, sourceSizeY = provisionedSource.SizeX, provisionedSource.SizeY
	}
	topology, err := a.gridTopology(r.Context())
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, Error{Code: "region_store_error", Message: "region discovery failed"})
		return
	}

	directions := []struct {
		name string
		dx   int
		dy   int
	}{
		{name: "north", dy: 1},
		{name: "east", dx: 1},
		{name: "south", dy: -1},
		{name: "west", dx: -1},
	}
	neighbors := make([]RegionNeighbor, 0, len(directions))
	overlaps := func(firstStart, firstSize, secondStart, secondSize int) bool {
		return firstStart < secondStart+secondSize && secondStart < firstStart+firstSize
	}
	for _, direction := range directions {
		for _, candidate := range topology {
			// Topology entries are facets, which are square; still, read each
			// axis on its own so nothing here assumes that.
			candidateSizeX := candidate.SizeX / 256
			candidateSizeY := candidate.SizeY / 256
			adjacent := false
			switch direction.name {
			case "north":
				adjacent = candidate.GridY == source.GridY+sourceSizeY &&
					overlaps(source.GridX, sourceSizeX, candidate.GridX, candidateSizeX)
			case "east":
				adjacent = candidate.GridX == source.GridX+sourceSizeX &&
					overlaps(source.GridY, sourceSizeY, candidate.GridY, candidateSizeY)
			case "south":
				adjacent = candidate.GridY+candidateSizeY == source.GridY &&
					overlaps(source.GridX, sourceSizeX, candidate.GridX, candidateSizeX)
			case "west":
				adjacent = candidate.GridX+candidateSizeX == source.GridX &&
					overlaps(source.GridY, sourceSizeY, candidate.GridY, candidateSizeY)
			}
			if adjacent {
				neighbors = append(neighbors, RegionNeighbor{Direction: direction.name, Region: candidate})
			}
		}
	}
	writeJSON(w, http.StatusOK, RegionNeighborList{Neighbors: neighbors})
}

func (a *API) writeRegionResult(w http.ResponseWriter, region regions.Region, err error) {
	if errors.Is(err, regions.ErrNotFound) {
		writeJSON(w, http.StatusNotFound, Error{Code: "region_not_found", Message: "region was not found or its lease expired"})
	} else if err != nil {
		writeJSON(w, http.StatusInternalServerError, Error{Code: "region_store_error", Message: "region lookup failed"})
	} else {
		writeJSON(w, http.StatusOK, region)
	}
}

func decodeJSON(w http.ResponseWriter, r *http.Request, target any) bool {
	decoder := json.NewDecoder(http.MaxBytesReader(w, r.Body, 64*1024))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(target); err != nil {
		writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_json", Message: "request body must be valid JSON"})
		return false
	}
	if err := decoder.Decode(&struct{}{}); !errors.Is(err, io.EOF) {
		writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_json", Message: "request body must contain one JSON object"})
		return false
	}
	return true
}

func validateRegistration(w http.ResponseWriter, request RegisterRegionRequest) bool {
	name := strings.TrimSpace(request.Name)
	endpoint, err := url.ParseRequestURI(request.PublicEndpoint)
	if request.ViewerPort == 0 {
		request.ViewerPort = 42002
	}
	if name == "" || len(name) > 128 || request.GridX < 0 || request.GridY < 0 ||
		request.ViewerPort < 1 || request.ViewerPort > 65535 || err != nil ||
		(endpoint.Scheme != "http" && endpoint.Scheme != "https") || endpoint.Host == "" {
		writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_region", Message: "region name, coordinates, or public endpoint is invalid"})
		return false
	}
	return true
}

func validateLease(w http.ResponseWriter, seconds int) (time.Duration, bool) {
	if seconds == 0 {
		seconds = 60
	}
	if seconds < 10 || seconds > 300 {
		writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_lease", Message: "leaseSeconds must be between 10 and 300"})
		return 0, false
	}
	return time.Duration(seconds) * time.Second, true
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

func writeJSON(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(value)
}
