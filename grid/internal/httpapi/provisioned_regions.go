package httpapi

import (
	"crypto/rand"
	"encoding/hex"
	"errors"
	"net/http"
	"strings"

	"github.com/homeworldz/server/grid/internal/identifier"
	"github.com/homeworldz/server/grid/internal/provisioning"
)

func (a *API) provisionedRegionsRoot(w http.ResponseWriter, r *http.Request) {
	if a.provisioned == nil {
		writeJSON(w, http.StatusServiceUnavailable, Error{Code: "region_provisioning_unavailable", Message: "region provisioning is unavailable"})
		return
	}
	switch r.Method {
	case http.MethodGet:
		items, err := a.provisioned.List(r.Context())
		if writeProvisioningError(w, err) {
			return
		}
		writeJSON(w, http.StatusOK, ProvisionedRegionList{Regions: items})
	case http.MethodPost:
		var request CreateProvisionedRegionRequest
		if !decodeJSON(w, r, &request) {
			return
		}
		id := strings.TrimSpace(request.ID)
		if id == "" {
			var err error
			id, err = identifier.NewUUID()
			if err != nil {
				writeJSON(w, http.StatusInternalServerError, Error{Code: "credential_generation_failed", Message: "region identity generation failed"})
				return
			}
		}
		accessKey, err := newRegionAccessKey()
		if err != nil {
			writeJSON(w, http.StatusInternalServerError, Error{Code: "credential_generation_failed", Message: "region access-key generation failed"})
			return
		}
		enabled := true
		if request.Enabled != nil {
			enabled = *request.Enabled
		}
		sizeX, sizeY := resolveSizes(request.Size, request.SizeX, request.SizeY)
		region, err := a.provisioned.Create(r.Context(), provisioning.Region{ID: id, Name: request.Name,
			OwnerUserID: request.OwnerUserID, MapX: request.MapX, MapY: request.MapY,
			SizeX: sizeX, SizeY: sizeY, FacetNames: request.FacetNames, Maturity: request.Maturity,
			PublicEndpoint: request.PublicEndpoint, ViewerPort: request.ViewerPort,
			Enabled: enabled, AccessKey: accessKey})
		if !writeProvisioningError(w, err) {
			writeJSON(w, http.StatusCreated, ProvisionedRegionResult{Region: region, AccessKey: accessKey})
		}
	default:
		w.Header().Set("Allow", "GET, POST")
		writeJSON(w, http.StatusMethodNotAllowed, Error{Code: "method_not_allowed", Message: "only GET and POST are supported"})
	}
}

func (a *API) provisionedRegionByID(w http.ResponseWriter, r *http.Request) {
	if a.provisioned == nil {
		writeJSON(w, http.StatusServiceUnavailable, Error{Code: "region_provisioning_unavailable", Message: "region provisioning is unavailable"})
		return
	}
	parts := strings.Split(strings.Trim(strings.TrimPrefix(r.URL.Path, "/api/v1/provisioned-regions/"), "/"), "/")
	if len(parts) == 0 || !validUUID(parts[0]) {
		a.notFound(w, r)
		return
	}
	id := parts[0]
	if len(parts) == 2 && parts[1] == "rotate-access-key" {
		if r.Method != http.MethodPost {
			w.Header().Set("Allow", http.MethodPost)
			writeJSON(w, http.StatusMethodNotAllowed, Error{Code: "method_not_allowed", Message: "only POST is supported"})
			return
		}
		accessKey, err := newRegionAccessKey()
		if err != nil {
			writeJSON(w, http.StatusInternalServerError, Error{Code: "credential_generation_failed", Message: "region access-key generation failed"})
			return
		}
		region, err := a.provisioned.RotateAccessKey(r.Context(), id, accessKey)
		if !writeProvisioningError(w, err) {
			writeJSON(w, http.StatusOK, ProvisionedRegionResult{Region: region, AccessKey: accessKey})
		}
		return
	}
	if len(parts) != 1 {
		a.notFound(w, r)
		return
	}
	switch r.Method {
	case http.MethodGet:
		region, err := a.provisioned.Get(r.Context(), id)
		if !writeProvisioningError(w, err) {
			writeJSON(w, http.StatusOK, ProvisionedRegionResult{Region: region})
		}
	case http.MethodPatch:
		var request UpdateProvisionedRegionRequest
		if !decodeJSON(w, r, &request) {
			return
		}
		sizeX, sizeY := request.SizeX, request.SizeY
		if request.Size != nil {
			if sizeX == nil {
				sizeX = request.Size
			}
			if sizeY == nil {
				sizeY = request.Size
			}
		}
		region, err := a.provisioned.Update(r.Context(), id, provisioning.Update{Name: request.Name,
			OwnerUserID: request.OwnerUserID, MapX: request.MapX, MapY: request.MapY,
			SizeX: sizeX, SizeY: sizeY, FacetNames: request.FacetNames, Maturity: request.Maturity,
			PublicEndpoint: request.PublicEndpoint, ViewerPort: request.ViewerPort, Enabled: request.Enabled})
		if !writeProvisioningError(w, err) {
			writeJSON(w, http.StatusOK, ProvisionedRegionResult{Region: region})
		}
	case http.MethodDelete:
		if !writeProvisioningError(w, a.provisioned.Delete(r.Context(), id)) {
			w.WriteHeader(http.StatusNoContent)
		}
	default:
		w.Header().Set("Allow", "GET, PATCH, DELETE")
		writeJSON(w, http.StatusMethodNotAllowed, Error{Code: "method_not_allowed", Message: "only GET, PATCH, and DELETE are supported"})
	}
}

// resolveSizes folds the square `size` shorthand into per-axis sizes; explicit
// sizeX/sizeY win where both forms are given.
func resolveSizes(size, sizeX, sizeY int) (int, int) {
	resolvedX, resolvedY := sizeX, sizeY
	if size != 0 {
		if resolvedX == 0 {
			resolvedX = size
		}
		if resolvedY == 0 {
			resolvedY = size
		}
	}
	return resolvedX, resolvedY
}

func newRegionAccessKey() (string, error) {
	value := make([]byte, 32)
	if _, err := rand.Read(value); err != nil {
		return "", err
	}
	return hex.EncodeToString(value), nil
}

func writeProvisioningError(w http.ResponseWriter, err error) bool {
	if err == nil {
		return false
	}
	switch {
	case errors.Is(err, provisioning.ErrNotFound):
		writeJSON(w, http.StatusNotFound, Error{Code: "provisioned_region_not_found", Message: "provisioned region was not found"})
	case errors.Is(err, provisioning.ErrConflict):
		writeJSON(w, http.StatusConflict, Error{Code: "provisioned_region_conflict", Message: "region UUID, name, or coordinates conflict with an existing region"})
	case errors.Is(err, provisioning.ErrInvalid):
		writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_provisioned_region", Message: "region UUID, name, owner, or coordinates are invalid"})
	default:
		writeJSON(w, http.StatusInternalServerError, Error{Code: "region_provisioning_error", Message: "region provisioning update failed"})
	}
	return true
}
