package httpapi

import (
	"errors"
	"net/http"
	"strings"

	"github.com/homeworldz/server/grid/internal/eventlog"
	"github.com/homeworldz/server/grid/internal/presence"
)

func (a *API) presenceRoot(w http.ResponseWriter, r *http.Request) {
	if a.presence == nil {
		presenceUnavailable(w)
		return
	}
	if r.Method != http.MethodGet {
		w.Header().Set("Allow", http.MethodGet)
		writeJSON(w, http.StatusMethodNotAllowed, Error{Code: "method_not_allowed", Message: "only GET is supported"})
		return
	}
	values, err := a.presence.List(r.Context())
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, Error{Code: "presence_store_error", Message: "presence discovery failed"})
		return
	}
	writeJSON(w, http.StatusOK, PresenceList{Presence: values})
}

func (a *API) presenceByUser(w http.ResponseWriter, r *http.Request) {
	if a.presence == nil {
		presenceUnavailable(w)
		return
	}
	userID := strings.TrimPrefix(r.URL.Path, "/api/v1/presence/")
	if !validUUID(userID) {
		a.notFound(w, r)
		return
	}
	switch r.Method {
	case http.MethodPut:
		var request UpdatePresenceRequest
		if !decodeJSON(w, r, &request) {
			return
		}
		if !validUUID(request.RegionID) {
			writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_presence", Message: "regionId must be a UUID"})
			return
		}
		value, err := a.presence.Update(r.Context(), userID, request.RegionID)
		a.writePresenceResult(w, value, err)
	case http.MethodGet:
		value, err := a.presence.Get(r.Context(), userID)
		a.writePresenceResult(w, value, err)
	case http.MethodDelete:
		if err := a.presence.Clear(r.Context(), userID); errors.Is(err, presence.ErrNotFound) {
			writeJSON(w, http.StatusNotFound, Error{Code: "presence_not_found", Message: "online presence was not found"})
		} else if err != nil {
			writeJSON(w, http.StatusInternalServerError, Error{Code: "presence_store_error", Message: "presence cleanup failed"})
		} else {
			// A cleared presence is the grid's only account of an avatar
			// leaving: the region clears it both when a viewer logs out and
			// when a session is retired without one, and there is no other
			// signal for either.
			eventlog.Note(r.Context(), a.events, a.logger, eventlog.Event{
				Kind: eventlog.KindLogout, UserID: userID,
			})
			w.WriteHeader(http.StatusNoContent)
		}
	default:
		w.Header().Set("Allow", "GET, PUT, DELETE")
		writeJSON(w, http.StatusMethodNotAllowed, Error{Code: "method_not_allowed", Message: "only GET, PUT, and DELETE are supported"})
	}
}

func (a *API) writePresenceResult(w http.ResponseWriter, value presence.Presence, err error) {
	if errors.Is(err, presence.ErrNotFound) {
		writeJSON(w, http.StatusNotFound, Error{Code: "presence_not_found", Message: "user, active region, or online presence was not found"})
	} else if err != nil {
		writeJSON(w, http.StatusInternalServerError, Error{Code: "presence_store_error", Message: "presence operation failed"})
	} else {
		writeJSON(w, http.StatusOK, value)
	}
}

func presenceUnavailable(w http.ResponseWriter) {
	writeJSON(w, http.StatusServiceUnavailable, Error{Code: "presence_store_unavailable", Message: "presence storage is unavailable"})
}
