package api

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"net/http"
	"net/url"
	"strings"

	"github.com/homeworldz/server/grid/internal/provisioning"
	"github.com/homeworldz/server/grid/internal/webaccount"
)

// Region state values.
const (
	regionOnline     = "online"
	regionOffline    = "offline"
	regionUndeployed = "undeployed"
)

const defaultViewerPort = 42002

// Region kinds capture provenance and are mutually exclusive: grid = provided
// by the grid operator, user = created by a resident. Essential/protected
// status is expressed with a tag (e.g. "system"), not a kind.
const (
	regionKindGrid = "grid"
	regionKindUser = "user"
)

var regionKinds = map[string]bool{regionKindGrid: true, regionKindUser: true}

// validFacetEdges are the proven square viewer sizes in grid units (each unit
// is 256 m): 1 = 256x256, 2 = 512x512, 4 = 1024x1024. A region's shorter
// dimension must be one of these; the longer must be a whole multiple of the
// shorter (the ADR 0036 shape rule), which the provisioning store enforces.
var validFacetEdges = map[int]bool{1: true, 2: true, 4: true}

// adminRegionsRoot handles GET/POST /v1/admin/regions.
func (a *API) adminRegionsRoot(w http.ResponseWriter, r *http.Request) {
	account, ok := a.requireAuth(w, r)
	if !ok {
		return
	}
	if a.regions == nil {
		writeError(w, http.StatusServiceUnavailable, Error{Code: "region_store_unavailable", Message: "region storage is unavailable"})
		return
	}
	switch r.Method {
	case http.MethodGet:
		if !a.requirePrivilege(w, account, webaccount.PrivRegions) {
			return
		}
		items, err := a.regions.List(r.Context())
		if err != nil {
			a.internalError(w, r, "list regions", err)
			return
		}
		regionsOut := make([]ManagedRegion, 0, len(items))
		for _, item := range items {
			regionsOut = append(regionsOut, a.managedRegionOf(r.Context(), item))
		}
		writeJSON(w, http.StatusOK, RegionList{Regions: regionsOut})
	case http.MethodPost:
		if !a.requirePrivilege(w, account, webaccount.PrivDeploy) {
			return
		}
		a.createRegion(w, r)
	default:
		methodNotAllowed(w, http.MethodGet, http.MethodPost)
	}
}

func (a *API) createRegion(w http.ResponseWriter, r *http.Request) {
	var request createRegionRequest
	if !decodeJSON(w, r, &request) {
		return
	}
	name := strings.TrimSpace(request.Name)
	if name == "" || len(name) > 128 {
		writeError(w, http.StatusBadRequest, Error{Code: "invalid_region", Message: "name must be 1-128 characters", Field: "name"})
		return
	}
	if request.GridX == nil || request.GridY == nil || *request.GridX < 0 || *request.GridY < 0 {
		writeError(w, http.StatusBadRequest, Error{Code: "invalid_region", Message: "gridX and gridY are required and must be non-negative"})
		return
	}
	if !validUUID(request.OwnerUserID) {
		writeError(w, http.StatusBadRequest, Error{Code: "invalid_region", Message: "ownerUserId must be a UUID", Field: "ownerUserId"})
		return
	}
	if !validRegionEndpoint(request.PublicEndpoint) {
		writeError(w, http.StatusBadRequest, Error{Code: "invalid_region", Message: "publicEndpoint must be an http(s) URL", Field: "publicEndpoint"})
		return
	}
	viewerPort := defaultViewerPort
	if request.ViewerPort != nil {
		viewerPort = *request.ViewerPort
	}
	if viewerPort < 1 || viewerPort > 65535 {
		writeError(w, http.StatusBadRequest, Error{Code: "invalid_region", Message: "viewerPort must be between 1 and 65535", Field: "viewerPort"})
		return
	}
	sizeX, sizeY := 1, 1
	if request.Size != nil {
		sizeX, sizeY = *request.Size, *request.Size
	}
	if request.SizeX != nil {
		sizeX = *request.SizeX
	}
	if request.SizeY != nil {
		sizeY = *request.SizeY
	}
	shorter, longer := sizeX, sizeY
	if shorter > longer {
		shorter, longer = longer, shorter
	}
	if !validFacetEdges[shorter] || longer%shorter != 0 {
		writeError(w, http.StatusBadRequest, Error{Code: "invalid_region",
			Message: "the shorter side must be 1, 2, or 4 tiles and divide the longer", Field: "size"})
		return
	}
	kind := regionKindUser
	if request.Kind != "" {
		if !regionKinds[request.Kind] {
			writeError(w, http.StatusBadRequest, Error{Code: "invalid_kind", Message: "kind must be grid or user", Field: "kind"})
			return
		}
		kind = request.Kind
	}
	tags, err := webaccount.NormalizeTags(request.Tags)
	if err != nil {
		writeError(w, http.StatusBadRequest, Error{Code: "invalid_tags", Message: "tags must be a comma-separated list of lowercase tokens", Field: "tags"})
		return
	}
	id, err := newRegionID()
	if err != nil {
		a.internalError(w, r, "create region id", err)
		return
	}
	accessKey, err := newRegionAccessKey()
	if err != nil {
		a.internalError(w, r, "create region key", err)
		return
	}
	created, err := a.regions.Create(r.Context(), provisioning.Region{
		ID: id, Name: name, OwnerUserID: request.OwnerUserID,
		MapX: *request.GridX, MapY: *request.GridY, SizeX: sizeX, SizeY: sizeY,
		FacetNames: request.FacetNames, Maturity: 0,
		PublicEndpoint: strings.TrimSpace(request.PublicEndpoint), ViewerPort: viewerPort,
		Enabled: true, Kind: kind, Tags: tags, AccessKey: accessKey,
	})
	if handled := a.writeRegionStoreError(w, r, err, "create region"); handled {
		return
	}
	w.Header().Set("Location", "/v1/admin/regions/"+id)
	writeJSON(w, http.StatusCreated, RegionDeployment{
		Region: a.managedRegionOf(r.Context(), created), AccessKey: accessKey,
	})
}

// adminRegionByID dispatches /v1/admin/regions/{id} and its sub-resources.
func (a *API) adminRegionByID(w http.ResponseWriter, r *http.Request) {
	account, ok := a.requireAuth(w, r)
	if !ok {
		return
	}
	if a.regions == nil {
		writeError(w, http.StatusServiceUnavailable, Error{Code: "region_store_unavailable", Message: "region storage is unavailable"})
		return
	}
	parts := strings.Split(strings.Trim(strings.TrimPrefix(r.URL.Path, "/v1/admin/regions/"), "/"), "/")
	if len(parts) == 0 || !validUUID(parts[0]) {
		a.notFound(w, r)
		return
	}
	id := parts[0]

	switch {
	case len(parts) == 1 && r.Method == http.MethodGet:
		if !a.requirePrivilege(w, account, webaccount.PrivRegions) {
			return
		}
		region, err := a.regions.Get(r.Context(), id)
		if handled := a.writeRegionStoreError(w, r, err, "get region"); handled {
			return
		}
		writeJSON(w, http.StatusOK, a.managedRegionOf(r.Context(), region))
	case len(parts) == 1 && r.Method == http.MethodPatch:
		if !a.requirePrivilege(w, account, webaccount.PrivRegions) {
			return
		}
		a.updateRegion(w, r, id)
	case len(parts) == 1:
		methodNotAllowed(w, http.MethodGet, http.MethodPatch)
	case len(parts) == 2 && parts[1] == "map-position" && r.Method == http.MethodPut:
		if !a.requirePrivilege(w, account, webaccount.PrivMap) {
			return
		}
		a.moveRegion(w, r, id)
	case len(parts) == 2 && parts[1] == "map-position":
		methodNotAllowed(w, http.MethodPut)
	case len(parts) == 2 && parts[1] == "deployment" && r.Method == http.MethodPost:
		if !a.requirePrivilege(w, account, webaccount.PrivDeploy) {
			return
		}
		a.deployRegion(w, r, id)
	case len(parts) == 2 && parts[1] == "deployment" && r.Method == http.MethodDelete:
		if !a.requirePrivilege(w, account, webaccount.PrivUndeploy) {
			return
		}
		a.undeployRegion(w, r, id)
	case len(parts) == 2 && parts[1] == "deployment":
		methodNotAllowed(w, http.MethodPost, http.MethodDelete)
	case len(parts) == 2 && parts[1] == "tags" && r.Method == http.MethodPut:
		if !a.requirePrivilege(w, account, webaccount.PrivRegions) {
			return
		}
		a.setRegionTags(w, r, id)
	case len(parts) == 2 && parts[1] == "tags":
		methodNotAllowed(w, http.MethodPut)
	default:
		a.notFound(w, r)
	}
}

func (a *API) setRegionTags(w http.ResponseWriter, r *http.Request, id string) {
	var request setTagsRequest
	if !decodeJSON(w, r, &request) {
		return
	}
	if !regionKinds[request.Kind] {
		writeError(w, http.StatusBadRequest, Error{Code: "invalid_kind", Message: "kind must be grid or user", Field: "kind"})
		return
	}
	tags, err := webaccount.NormalizeTags(request.Tags)
	if err != nil {
		writeError(w, http.StatusBadRequest, Error{Code: "invalid_tags", Message: "tags must be a comma-separated list of lowercase tokens", Field: "tags"})
		return
	}
	kind := request.Kind
	updated, err := a.regions.Update(r.Context(), id, provisioning.Update{Kind: &kind, Tags: &tags})
	if handled := a.writeRegionStoreError(w, r, err, "set region tags"); handled {
		return
	}
	writeJSON(w, http.StatusOK, a.managedRegionOf(r.Context(), updated))
}

func (a *API) updateRegion(w http.ResponseWriter, r *http.Request, id string) {
	var request updateRegionRequest
	if !decodeJSON(w, r, &request) {
		return
	}
	update := provisioning.Update{}
	set := false
	if request.Name != nil {
		name := strings.TrimSpace(*request.Name)
		if name == "" || len(name) > 128 {
			writeError(w, http.StatusBadRequest, Error{Code: "invalid_region", Message: "name must be 1-128 characters", Field: "name"})
			return
		}
		update.Name = &name
		set = true
	}
	if request.OwnerUserID != nil {
		if !validUUID(*request.OwnerUserID) {
			writeError(w, http.StatusBadRequest, Error{Code: "invalid_region", Message: "ownerUserId must be a UUID", Field: "ownerUserId"})
			return
		}
		update.OwnerUserID = request.OwnerUserID
		set = true
	}
	if request.PublicEndpoint != nil {
		if !validRegionEndpoint(*request.PublicEndpoint) {
			writeError(w, http.StatusBadRequest, Error{Code: "invalid_region", Message: "publicEndpoint must be an http(s) URL", Field: "publicEndpoint"})
			return
		}
		update.PublicEndpoint = request.PublicEndpoint
		set = true
	}
	if request.ViewerPort != nil {
		if *request.ViewerPort < 1 || *request.ViewerPort > 65535 {
			writeError(w, http.StatusBadRequest, Error{Code: "invalid_region", Message: "viewerPort must be between 1 and 65535", Field: "viewerPort"})
			return
		}
		update.ViewerPort = request.ViewerPort
		set = true
	}
	if !set {
		writeError(w, http.StatusBadRequest, Error{Code: "empty_update", Message: "at least one field must be provided"})
		return
	}
	updated, err := a.regions.Update(r.Context(), id, update)
	if handled := a.writeRegionStoreError(w, r, err, "update region"); handled {
		return
	}
	writeJSON(w, http.StatusOK, a.managedRegionOf(r.Context(), updated))
}

func (a *API) moveRegion(w http.ResponseWriter, r *http.Request, id string) {
	var request mapPositionRequest
	if !decodeJSON(w, r, &request) {
		return
	}
	if request.GridX == nil || request.GridY == nil || *request.GridX < 0 || *request.GridY < 0 {
		writeError(w, http.StatusBadRequest, Error{Code: "invalid_position", Message: "gridX and gridY are required and must be non-negative"})
		return
	}
	updated, err := a.regions.Update(r.Context(), id, provisioning.Update{MapX: request.GridX, MapY: request.GridY})
	if handled := a.writeRegionStoreError(w, r, err, "move region"); handled {
		return
	}
	writeJSON(w, http.StatusOK, a.managedRegionOf(r.Context(), updated))
}

func (a *API) deployRegion(w http.ResponseWriter, r *http.Request, id string) {
	enabled := true
	if _, err := a.regions.Update(r.Context(), id, provisioning.Update{Enabled: &enabled}); err != nil {
		if handled := a.writeRegionStoreError(w, r, err, "deploy region"); handled {
			return
		}
	}
	accessKey, err := newRegionAccessKey()
	if err != nil {
		a.internalError(w, r, "deploy region key", err)
		return
	}
	region, err := a.regions.RotateAccessKey(r.Context(), id, accessKey)
	if handled := a.writeRegionStoreError(w, r, err, "deploy region"); handled {
		return
	}
	writeJSON(w, http.StatusOK, RegionDeployment{
		Region: a.managedRegionOf(r.Context(), region), AccessKey: accessKey,
	})
}

func (a *API) undeployRegion(w http.ResponseWriter, r *http.Request, id string) {
	disabled := false
	region, err := a.regions.Update(r.Context(), id, provisioning.Update{Enabled: &disabled})
	if handled := a.writeRegionStoreError(w, r, err, "undeploy region"); handled {
		return
	}
	// Revoke the region access key by rotating it to a fresh, undisclosed value.
	if rotated, err := newRegionAccessKey(); err == nil {
		if _, rotateErr := a.regions.RotateAccessKey(r.Context(), id, rotated); rotateErr != nil {
			a.internalError(w, r, "revoke region key", rotateErr)
			return
		}
	}
	// End any active lease so the region drops out of discovery immediately.
	if a.leases != nil {
		if err := a.leases.DeregisterProvisioned(r.Context(), id); err != nil && a.logger != nil {
			a.logger.Warn("end region lease on undeploy", "regionId", id, "error", err)
		}
	}
	// NOTE: the contract also specifies releasing the map position. The
	// provisioned_regions schema stores grid_x/grid_y as NOT NULL, so an
	// undeployed region retains its reserved coordinates; reuse requires moving
	// or deleting it. This is the one documented deviation from the contract.
	writeJSON(w, http.StatusOK, a.managedRegionOf(r.Context(), region))
}

// managedRegionOf maps a provisioned region to the ManagedRegion DTO, deriving
// online/offline/undeployed state from the enabled flag and any live lease.
func (a *API) managedRegionOf(ctx context.Context, region provisioning.Region) ManagedRegion {
	x, y := region.MapX, region.MapY
	squareSize := 0
	if region.SizeX == region.SizeY {
		squareSize = region.SizeX
	}
	managed := ManagedRegion{
		ID: region.ID, Name: region.Name, OwnerUserID: region.OwnerUserID,
		GridX: &x, GridY: &y, PublicEndpoint: region.PublicEndpoint,
		ViewerPort: region.ViewerPort, Enabled: region.Enabled,
		Size: squareSize, SizeX: region.SizeX, SizeY: region.SizeY,
		FacetNames: region.FacetNames, Kind: region.Kind, Tags: region.Tags,
	}
	if !region.Enabled {
		managed.State = regionUndeployed
		return managed
	}
	if a.leases != nil {
		if live, err := a.leases.Get(ctx, region.ID); err == nil {
			expires := live.LeaseExpiresAt.UTC()
			managed.State = regionOnline
			managed.LeaseExpiresAt = &expires
			return managed
		}
	}
	managed.State = regionOffline
	return managed
}

// writeRegionStoreError maps provisioning store errors to responses and reports
// whether it handled the error (including err == nil producing no output).
func (a *API) writeRegionStoreError(w http.ResponseWriter, r *http.Request, err error, operation string) bool {
	switch {
	case err == nil:
		return false
	case errors.Is(err, provisioning.ErrNotFound):
		a.writeNotFound(w)
	case errors.Is(err, provisioning.ErrConflict):
		writeError(w, http.StatusConflict, Error{Code: "region_conflict", Message: "region coordinates or name conflict with an existing region"})
	case errors.Is(err, provisioning.ErrInvalid):
		writeError(w, http.StatusBadRequest, Error{Code: "invalid_region", Message: "region details are invalid"})
	default:
		a.internalError(w, r, operation, err)
	}
	return true
}

func validRegionEndpoint(value string) bool {
	endpoint, err := url.ParseRequestURI(strings.TrimSpace(value))
	return err == nil && (endpoint.Scheme == "http" || endpoint.Scheme == "https") && endpoint.Host != ""
}

func newRegionAccessKey() (string, error) {
	buffer := make([]byte, 32)
	if _, err := rand.Read(buffer); err != nil {
		return "", err
	}
	return hex.EncodeToString(buffer), nil
}

func newRegionID() (string, error) {
	buffer := make([]byte, 16)
	if _, err := rand.Read(buffer); err != nil {
		return "", err
	}
	buffer[6] = (buffer[6] & 0x0f) | 0x40
	buffer[8] = (buffer[8] & 0x3f) | 0x80
	encoded := hex.EncodeToString(buffer)
	return encoded[0:8] + "-" + encoded[8:12] + "-" + encoded[12:16] + "-" + encoded[16:20] + "-" + encoded[20:32], nil
}
