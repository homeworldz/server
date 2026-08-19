package httpapi

import (
	"context"
	"errors"
	"math"
	"net/http"
	"strings"
	"time"

	"github.com/homeworldz/server/grid/internal/identity"
	"github.com/homeworldz/server/grid/internal/regions"
	"github.com/homeworldz/server/grid/internal/transit"
)

func (a *API) transitsRoot(w http.ResponseWriter, r *http.Request) {
	if a.transits == nil || a.identity == nil || a.regions == nil {
		writeJSON(w, http.StatusServiceUnavailable, Error{Code: "transit_unavailable", Message: "avatar transit coordination is unavailable"})
		return
	}
	if r.Method != http.MethodPost {
		w.Header().Set("Allow", http.MethodPost)
		writeJSON(w, http.StatusMethodNotAllowed, Error{Code: "method_not_allowed", Message: "only POST is supported"})
		return
	}
	var request PrepareTransitRequest
	if !decodeJSON(w, r, &request) || !a.validateTransitRequest(r.Context(), w, request) {
		return
	}
	session, err := a.identity.ValidateSession(r.Context(), request.SessionID)
	if errors.Is(err, identity.ErrSessionNotFound) {
		writeJSON(w, http.StatusConflict, Error{Code: "transit_session_mismatch", Message: "viewer session is not authoritative in the source region"})
		return
	}
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, Error{Code: "identity_store_error", Message: "viewer session validation failed"})
		return
	}
	if session.UserID != request.AgentID || session.DestinationRegionID != request.SourceRegionID {
		writeJSON(w, http.StatusConflict, Error{Code: "transit_session_mismatch", Message: "viewer session is not authoritative in the source region"})
		return
	}
	if _, err := a.regions.Get(r.Context(), request.SourceRegionID); err != nil {
		a.writeTransitRegionError(w, "source", err)
		return
	}
	if _, err := a.regions.Get(r.Context(), request.DestinationRegionID); err != nil {
		a.writeTransitRegionError(w, "destination", err)
		return
	}
	lifetime := request.LifetimeSeconds
	if lifetime == 0 {
		lifetime = 30
	}
	value, err := a.transits.Prepare(r.Context(), transit.Prepare{
		ID: request.ID, AgentID: request.AgentID, SessionID: request.SessionID,
		SourceRegionID: request.SourceRegionID, DestinationRegionID: request.DestinationRegionID,
		Position: request.Position, LookAt: request.LookAt, Flying: request.Flying,
		Lifetime: time.Duration(lifetime) * time.Second,
	})
	a.writeTransitResult(w, value, err)
}

func (a *API) transitByID(w http.ResponseWriter, r *http.Request) {
	if a.transits == nil {
		writeJSON(w, http.StatusServiceUnavailable, Error{Code: "transit_unavailable", Message: "avatar transit coordination is unavailable"})
		return
	}
	parts := strings.Split(strings.Trim(strings.TrimPrefix(r.URL.Path, "/api/v1/transits/"), "/"), "/")
	if len(parts) == 0 || !validUUID(parts[0]) {
		a.notFound(w, r)
		return
	}
	if len(parts) == 1 && r.Method == http.MethodGet {
		value, err := a.transits.Get(r.Context(), parts[0])
		a.writeTransitResult(w, value, err)
		return
	}
	if len(parts) != 2 || r.Method != http.MethodPost ||
		(parts[1] != "accept" && parts[1] != "activate" && parts[1] != "rollback") {
		a.notFound(w, r)
		return
	}
	var request TransitActionRequest
	if !decodeJSON(w, r, &request) {
		return
	}
	if !validUUID(request.RegionID) {
		writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_transit", Message: "regionId must be a UUID"})
		return
	}
	if a.regions != nil {
		if _, err := a.regions.Get(r.Context(), request.RegionID); err != nil {
			a.writeTransitRegionError(w, "acting", err)
			return
		}
	}
	var value transit.Transit
	var err error
	switch parts[1] {
	case "accept":
		value, err = a.transits.Accept(r.Context(), parts[0], request.RegionID)
	case "activate":
		value, err = a.transits.Activate(r.Context(), parts[0], request.RegionID)
	case "rollback":
		value, err = a.transits.Rollback(r.Context(), parts[0], request.RegionID, strings.TrimSpace(request.Reason))
	}
	a.writeTransitResult(w, value, err)
}

func (a *API) validateTransitRequest(ctx context.Context, w http.ResponseWriter, request PrepareTransitRequest) bool {
	finite := func(value float32) bool { return !float32NaNOrInf(value) }
	validVector := func(value transit.Vector3) bool { return finite(value.X) && finite(value.Y) && finite(value.Z) }
	extentX, extentY := a.regionExtents(ctx, request.DestinationRegionID)
	valid := validUUID(request.ID) && validUUID(request.AgentID) && validUUID(request.SessionID) &&
		validUUID(request.SourceRegionID) && validUUID(request.DestinationRegionID) &&
		request.SourceRegionID != request.DestinationRegionID && validVector(request.Position) &&
		validVector(request.LookAt) && request.Position.X >= 0 && request.Position.X <= extentX &&
		request.Position.Y >= 0 && request.Position.Y <= extentY && request.Position.Z >= -4096 &&
		request.Position.Z <= 4096 && (request.LifetimeSeconds == 0 ||
		(request.LifetimeSeconds >= 10 && request.LifetimeSeconds <= 120))
	if !valid {
		writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_transit", Message: "transit identity, destination, arrival state, or lifetime is invalid"})
	}
	return valid
}

func float32NaNOrInf(value float32) bool {
	return math.IsNaN(float64(value)) || math.IsInf(float64(value), 0)
}

func (a *API) writeTransitRegionError(w http.ResponseWriter, role string, err error) {
	if errors.Is(err, regions.ErrNotFound) {
		writeJSON(w, http.StatusConflict, Error{Code: "transit_region_offline", Message: role + " region is not online"})
	} else {
		writeJSON(w, http.StatusInternalServerError, Error{Code: "region_store_error", Message: role + " region lookup failed"})
	}
}

func (a *API) writeTransitResult(w http.ResponseWriter, value transit.Transit, err error) {
	switch {
	case err == nil:
		writeJSON(w, http.StatusOK, value)
	case errors.Is(err, transit.ErrNotFound):
		writeJSON(w, http.StatusNotFound, Error{Code: "transit_not_found", Message: "avatar transit was not found"})
	case errors.Is(err, transit.ErrConflict):
		writeJSON(w, http.StatusConflict, Error{Code: "transit_conflict", Message: "avatar already has a conflicting active transit"})
	case errors.Is(err, transit.ErrExpired):
		writeJSON(w, http.StatusConflict, Error{Code: "transit_expired", Message: "avatar transit expired"})
	case errors.Is(err, transit.ErrInvalidTransition):
		writeJSON(w, http.StatusConflict, Error{Code: "invalid_transit_transition", Message: "avatar transit state or acting region is invalid"})
	default:
		writeJSON(w, http.StatusInternalServerError, Error{Code: "transit_store_error", Message: "avatar transit operation failed"})
	}
}
