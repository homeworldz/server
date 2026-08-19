package httpapi

import (
	"errors"
	"math"
	"net/http"
	"strings"

	"github.com/homeworldz/server/grid/internal/locations"
)

func (a *API) locationByUser(w http.ResponseWriter, r *http.Request) {
	if a.locations == nil {
		writeJSON(w, http.StatusServiceUnavailable, Error{
			Code: "location_store_unavailable", Message: "last-location storage is unavailable"})
		return
	}
	userID := strings.TrimPrefix(r.URL.Path, "/api/v1/locations/")
	if !validUUID(userID) {
		a.notFound(w, r)
		return
	}
	// ?scope=home targets the user's Home location; otherwise the last location.
	home := r.URL.Query().Get("scope") == "home"

	// GET is supported only for the Home scope (to serve Teleport Home).
	if r.Method == http.MethodGet {
		if !home {
			w.Header().Set("Allow", "GET, PUT")
			writeJSON(w, http.StatusMethodNotAllowed, Error{
				Code: "method_not_allowed", Message: "GET is only supported for scope=home"})
			return
		}
		value, err := a.locations.GetHome(r.Context(), userID)
		if errors.Is(err, locations.ErrNotFound) {
			writeJSON(w, http.StatusNotFound, Error{
				Code: "home_not_set", Message: "user has no home location"})
		} else if err != nil {
			writeJSON(w, http.StatusInternalServerError, Error{
				Code: "location_store_error", Message: "home lookup failed"})
		} else {
			writeJSON(w, http.StatusOK, value)
		}
		return
	}
	if r.Method != http.MethodPut {
		w.Header().Set("Allow", "GET, PUT")
		writeJSON(w, http.StatusMethodNotAllowed, Error{
			Code: "method_not_allowed", Message: "only GET and PUT are supported"})
		return
	}
	var request UpdateLocationRequest
	if !decodeJSON(w, r, &request) {
		return
	}
	finite := func(value float32) bool { return !float32NaNOrInf(value) }
	extentX, extentY := a.regionExtents(r.Context(), request.RegionID)
	if !validUUID(request.RegionID) ||
		!finite(request.Position.X) || !finite(request.Position.Y) || !finite(request.Position.Z) ||
		!finite(request.LookAt.X) || !finite(request.LookAt.Y) || !finite(request.LookAt.Z) ||
		request.Position.X < 0 || request.Position.X > extentX ||
		request.Position.Y < 0 || request.Position.Y > extentY ||
		math.Hypot(float64(request.LookAt.X), float64(request.LookAt.Y)) < 0.001 {
		writeJSON(w, http.StatusBadRequest, Error{
			Code: "invalid_location", Message: "regionId, position, and lookAt must describe a valid Region location"})
		return
	}
	value := locations.Location{
		UserID: userID, RegionID: request.RegionID,
		Position: [3]float32{request.Position.X, request.Position.Y, request.Position.Z},
		LookAt:   [3]float32{request.LookAt.X, request.LookAt.Y, request.LookAt.Z},
		Flying:   request.Flying,
	}
	var err error
	if home {
		value, err = a.locations.UpdateHome(r.Context(), value)
	} else {
		value, err = a.locations.Update(r.Context(), value)
	}
	if errors.Is(err, locations.ErrNotFound) {
		writeJSON(w, http.StatusNotFound, Error{
			Code: "location_subject_not_found", Message: "user or Region was not found"})
	} else if err != nil {
		writeJSON(w, http.StatusInternalServerError, Error{
			Code: "location_store_error", Message: "location update failed"})
	} else {
		writeJSON(w, http.StatusOK, value)
	}
}
