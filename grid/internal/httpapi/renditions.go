package httpapi

import (
	"errors"
	"io"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/homeworldz/server/grid/internal/renditions"
)

// Rendition routes (ADR 0033), split by who may call them:
//
//	POST /api/v1/assets/{id}/renditions           request a conversion (service token —
//	                                              regions enqueue at upload)
//	GET  /api/v1/assets/{id}/renditions           list (service token)
//	GET  /api/v1/assets/{id}/renditions/{kind}    bytes (service token — regions serve viewers)
//	PUT  /api/v1/assets/{id}/renditions/{kind}    store bytes (worker token ONLY)
//	POST /api/v1/rendition-jobs/claim             lease a job (worker token ONLY)
//	POST /api/v1/rendition-jobs/{id}/fail         release a failed job (worker token ONLY)
//	POST /api/v1/rendition-jobs/regenerate        re-queue renditions a newer
//	                                              generator supersedes (worker token ONLY)
//
// The split is ADR 0028 doing its job: regions hold the service token and are
// untrusted, and a rendition upload cannot be checksum-verified the way
// region-served asset bytes can — the grid cannot verify a conversion without
// redoing it. So writing renditions and taking conversion work require the
// worker credential, which only the grid's own conversion workers hold.

// assetRenditions dispatches the /assets/{id}/renditions[...] suffix. The
// caller has already validated the asset id.
func (a *API) assetRenditions(w http.ResponseWriter, r *http.Request, assetID string, kind string) {
	if a.renditions == nil {
		writeJSON(w, http.StatusServiceUnavailable, Error{Code: "renditions_unavailable",
			Message: "rendition storage is unavailable"})
		return
	}
	if kind == "" {
		switch r.Method {
		case http.MethodGet:
			a.listRenditions(w, r, assetID)
		case http.MethodPost:
			a.requestRendition(w, r, assetID)
		default:
			w.Header().Set("Allow", "GET, POST")
			writeJSON(w, http.StatusMethodNotAllowed, Error{Code: "method_not_allowed",
				Message: "only GET and POST are supported"})
		}
		return
	}
	switch r.Method {
	case http.MethodGet:
		a.openRendition(w, r, assetID, kind)
	case http.MethodPut:
		if !a.workerAuthorized(w, r) {
			return
		}
		a.putRendition(w, r, assetID, kind)
	default:
		w.Header().Set("Allow", "GET, PUT")
		writeJSON(w, http.StatusMethodNotAllowed, Error{Code: "method_not_allowed",
			Message: "only GET and PUT are supported"})
	}
}

// workerAuthorized enforces the worker credential on top of the internal
// boundary the request already passed. No worker token configured means no
// conversion endpoints, not open ones.
func (a *API) workerAuthorized(w http.ResponseWriter, r *http.Request) bool {
	if a.workerToken == "" {
		writeJSON(w, http.StatusServiceUnavailable, Error{Code: "worker_auth_unconfigured",
			Message: "conversion worker authentication is not configured"})
		return false
	}
	if !validBearerToken(r.Header.Get("Authorization"), a.workerToken) {
		w.Header().Set("WWW-Authenticate", "Bearer")
		writeJSON(w, http.StatusForbidden, Error{Code: "worker_token_required",
			Message: "this endpoint requires the conversion worker credential"})
		return false
	}
	return true
}

type requestRenditionBody struct {
	Kind string `json:"kind"`
}

func (a *API) requestRendition(w http.ResponseWriter, r *http.Request, assetID string) {
	var request requestRenditionBody
	if !decodeJSON(w, r, &request) {
		return
	}
	job, err := a.renditions.Request(r.Context(), assetID, request.Kind)
	switch {
	case errors.Is(err, renditions.ErrInvalid):
		writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_rendition_request",
			Message: "rendition kind is not recognized"})
	case errors.Is(err, renditions.ErrUnknownAsset):
		writeJSON(w, http.StatusNotFound, Error{Code: "asset_not_found",
			Message: "asset is not registered"})
	case err != nil:
		writeJSON(w, http.StatusInternalServerError, Error{Code: "rendition_store_error",
			Message: "rendition request failed"})
	default:
		writeJSON(w, http.StatusOK, job)
	}
}

func (a *API) listRenditions(w http.ResponseWriter, r *http.Request, assetID string) {
	values, err := a.renditions.List(r.Context(), assetID)
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, Error{Code: "rendition_store_error",
			Message: "rendition listing failed"})
		return
	}
	if values == nil {
		values = []renditions.Rendition{}
	}
	writeJSON(w, http.StatusOK, struct {
		Renditions []renditions.Rendition `json:"renditions"`
	}{values})
}

func (a *API) openRendition(w http.ResponseWriter, r *http.Request, assetID, kind string) {
	content, value, err := a.renditions.Open(r.Context(), assetID, kind)
	switch {
	case errors.Is(err, renditions.ErrInvalid):
		a.notFound(w, r)
		return
	case errors.Is(err, renditions.ErrNotFound):
		// Not-yet rather than never: the asset may simply not be converted
		// yet, which callers treat as pending (ADR 0033).
		writeJSON(w, http.StatusNotFound, Error{Code: "rendition_not_found",
			Message: "the asset has no such rendition yet"})
		return
	case err != nil:
		writeJSON(w, http.StatusInternalServerError, Error{Code: "rendition_store_error",
			Message: "rendition read failed"})
		return
	}
	defer content.Close()
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Length", strconv.FormatInt(value.ByteLength, 10))
	w.WriteHeader(http.StatusOK)
	_, _ = io.Copy(w, content)
}

func (a *API) putRendition(w http.ResponseWriter, r *http.Request, assetID, kind string) {
	generator := strings.TrimSpace(r.Header.Get("X-Homeworldz-Generator"))
	if generator == "" {
		// The worker's plain HTTP transport carries no custom headers; the
		// query parameter is the same declaration in the only place it can
		// put one.
		generator = strings.TrimSpace(r.URL.Query().Get("generator"))
	}
	if generator == "" {
		writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_rendition",
			Message: "the X-Homeworldz-Generator header must name the converter version"})
		return
	}
	value, err := a.renditions.Put(r.Context(), assetID, kind, generator,
		http.MaxBytesReader(w, r.Body, renditions.MaxRenditionSize+1))
	switch {
	case errors.Is(err, renditions.ErrInvalid):
		writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_rendition",
			Message: "rendition kind, generator, or size is invalid"})
	case errors.Is(err, renditions.ErrUnknownAsset):
		writeJSON(w, http.StatusNotFound, Error{Code: "asset_not_found",
			Message: "asset is not registered"})
	case err != nil:
		writeJSON(w, http.StatusInternalServerError, Error{Code: "rendition_store_error",
			Message: "rendition store failed"})
	default:
		writeJSON(w, http.StatusOK, value)
	}
}

type claimRenditionJobRequest struct {
	Kinds        []string `json:"kinds"`
	LeaseSeconds int      `json:"leaseSeconds"`
}

func (a *API) renditionJobs(w http.ResponseWriter, r *http.Request) {
	if a.renditions == nil {
		writeJSON(w, http.StatusServiceUnavailable, Error{Code: "renditions_unavailable",
			Message: "rendition storage is unavailable"})
		return
	}
	if !a.workerAuthorized(w, r) {
		return
	}
	suffix := strings.TrimPrefix(r.URL.Path, "/api/v1/rendition-jobs/")
	if r.Method != http.MethodPost {
		w.Header().Set("Allow", http.MethodPost)
		writeJSON(w, http.StatusMethodNotAllowed, Error{Code: "method_not_allowed",
			Message: "only POST is supported"})
		return
	}
	if suffix == "claim" {
		var request claimRenditionJobRequest
		if !decodeJSON(w, r, &request) {
			return
		}
		if request.LeaseSeconds <= 0 || request.LeaseSeconds > 3600 {
			request.LeaseSeconds = 300
		}
		job, claimed, err := a.renditions.Claim(r.Context(), request.Kinds,
			time.Duration(request.LeaseSeconds)*time.Second)
		switch {
		case errors.Is(err, renditions.ErrInvalid):
			writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_claim",
				Message: "claim kinds are missing or not recognized"})
		case err != nil:
			writeJSON(w, http.StatusInternalServerError, Error{Code: "rendition_store_error",
				Message: "rendition claim failed"})
		case !claimed:
			// An empty queue is the normal resting state, not an error.
			w.WriteHeader(http.StatusNoContent)
		default:
			writeJSON(w, http.StatusOK, job)
		}
		return
	}
	if suffix == "regenerate" {
		var request struct {
			Kind      string `json:"kind"`
			Generator string `json:"generator"`
		}
		if !decodeJSON(w, r, &request) {
			return
		}
		requeued, err := a.renditions.RequeueStale(r.Context(), request.Kind, request.Generator)
		switch {
		case errors.Is(err, renditions.ErrInvalid):
			writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_regenerate",
				Message: "regenerate needs a known kind and the current generator"})
		case err != nil:
			writeJSON(w, http.StatusInternalServerError, Error{Code: "rendition_store_error",
				Message: "rendition regenerate failed"})
		default:
			writeJSON(w, http.StatusOK, struct {
				Requeued int64 `json:"requeued"`
			}{requeued})
		}
		return
	}
	if jobID, action, found := strings.Cut(suffix, "/"); found && action == "fail" && validUUID(jobID) {
		var request struct {
			Error string `json:"error"`
			// Permanent says the worker read the input and it will never
			// convert. Absent means retryable, which is the safe default: a
			// job retried needlessly costs attempts, a job parked wrongly
			// costs the rendition.
			Permanent bool `json:"permanent,omitempty"`
		}
		if !decodeJSON(w, r, &request) {
			return
		}
		switch err := a.renditions.Fail(r.Context(), jobID, request.Error, request.Permanent); {
		case errors.Is(err, renditions.ErrNotFound):
			writeJSON(w, http.StatusNotFound, Error{Code: "job_not_found",
				Message: "no leased job with that id"})
		case err != nil:
			writeJSON(w, http.StatusInternalServerError, Error{Code: "rendition_store_error",
				Message: "rendition failure record failed"})
		default:
			w.WriteHeader(http.StatusNoContent)
		}
		return
	}
	a.notFound(w, r)
}
