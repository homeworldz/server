package httpapi

import (
	"errors"
	"net/http"
	"net/url"
	"strings"

	"github.com/homeworldz/server/grid/internal/assetmeta"
)

func (a *API) assetsRoot(w http.ResponseWriter, r *http.Request) {
	if a.assets == nil {
		writeJSON(w, http.StatusServiceUnavailable, Error{Code: "asset_store_unavailable", Message: "asset metadata storage is unavailable"})
		return
	}
	if r.Method != http.MethodPost {
		w.Header().Set("Allow", http.MethodPost)
		writeJSON(w, http.StatusMethodNotAllowed, Error{Code: "method_not_allowed", Message: "only POST is supported"})
		return
	}
	var request RegisterAssetRequest
	if !decodeJSON(w, r, &request) {
		return
	}
	endpoint, err := url.ParseRequestURI(request.Endpoint)
	if !validUUID(request.ID) || !validUUID(request.CreatorUserID) || !validSHA256(request.SHA256) ||
		request.Size <= 0 || err != nil || endpoint.Host == "" ||
		(endpoint.Scheme != "http" && endpoint.Scheme != "https") {
		writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_asset_registration", Message: "asset metadata or endpoint is invalid"})
		return
	}
	asset, err := a.assets.Register(r.Context(), assetmeta.Registration{
		ID: request.ID, CreatorUserID: request.CreatorUserID, SHA256: request.SHA256,
		Size: request.Size, Endpoint: strings.TrimRight(request.Endpoint, "/"), Origin: request.Origin,
		Bake: request.Bake,
	})
	if errors.Is(err, assetmeta.ErrConflict) {
		writeJSON(w, http.StatusConflict, Error{Code: "asset_registration_conflict", Message: "asset UUID is already registered with different immutable metadata"})
		return
	}
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, Error{Code: "asset_store_error", Message: "asset metadata could not be registered"})
		return
	}
	w.Header().Set("Location", "/api/v1/assets/"+asset.ID)
	writeJSON(w, http.StatusCreated, asset)
}

func (a *API) assetByID(w http.ResponseWriter, r *http.Request) {
	if a.assets == nil {
		writeJSON(w, http.StatusServiceUnavailable, Error{Code: "asset_store_unavailable", Message: "asset metadata storage is unavailable"})
		return
	}
	id := strings.TrimPrefix(r.URL.Path, "/api/v1/assets/")
	if suffix, remainder, found := strings.Cut(id, "/"); found {
		// /assets/{id}/renditions and /assets/{id}/renditions/{kind}
		// (ADR 0033). Kind names contain no slash by construction.
		if !validUUID(suffix) {
			a.notFound(w, r)
			return
		}
		if remainder == "renditions" {
			a.assetRenditions(w, r, suffix, "")
			return
		}
		if kind, extra, nested := strings.Cut(strings.TrimPrefix(remainder, "renditions/"),
			"/"); strings.HasPrefix(remainder, "renditions/") && !nested && kind != "" && extra == "" {
			a.assetRenditions(w, r, suffix, kind)
			return
		}
		a.notFound(w, r)
		return
	}
	if !validUUID(id) {
		a.notFound(w, r)
		return
	}
	if r.Method != http.MethodGet {
		w.Header().Set("Allow", http.MethodGet)
		writeJSON(w, http.StatusMethodNotAllowed, Error{Code: "method_not_allowed", Message: "only GET is supported"})
		return
	}
	asset, err := a.assets.Get(r.Context(), id)
	if errors.Is(err, assetmeta.ErrNotFound) {
		writeJSON(w, http.StatusNotFound, Error{Code: "asset_not_found", Message: "asset metadata was not found"})
	} else if err != nil {
		writeJSON(w, http.StatusInternalServerError, Error{Code: "asset_store_error", Message: "asset metadata could not be loaded"})
	} else {
		writeJSON(w, http.StatusOK, asset)
	}
}

func validSHA256(value string) bool {
	if len(value) != 64 {
		return false
	}
	for _, character := range value {
		if !((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f')) {
			return false
		}
	}
	return true
}
