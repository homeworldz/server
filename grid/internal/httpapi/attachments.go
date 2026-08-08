package httpapi

import (
	"net/http"
	"strings"

	"github.com/homeworldz/server/grid/internal/attachments"
)

type attachmentRequest struct {
	ItemID          string `json:"itemId"`
	AttachmentPoint int    `json:"attachmentPoint"`
	Worn            bool   `json:"worn"`
}

// attachmentsByUser manages what a user is wearing. The Region calls PUT with
// {itemId, attachmentPoint, worn} as attachments are worn and taken off, and
// GET when an avatar arrives, to rez back what it had on somewhere else.
func (a *API) attachmentsByUser(w http.ResponseWriter, r *http.Request) {
	if a.attachments == nil {
		writeJSON(w, http.StatusServiceUnavailable, Error{
			Code: "attachment_store_unavailable", Message: "attachment storage is unavailable"})
		return
	}
	userID := strings.TrimPrefix(r.URL.Path, "/api/v1/attachments/")
	if !validUUID(userID) {
		a.notFound(w, r)
		return
	}
	if r.Method == http.MethodGet {
		worn, err := a.attachments.ListWorn(r.Context(), userID)
		if err != nil {
			writeJSON(w, http.StatusInternalServerError, Error{
				Code: "attachment_store_error", Message: "attachment lookup failed"})
			return
		}
		// An empty list is what "wearing nothing" looks like, and it has to be
		// distinguishable from a lookup that failed — which is the 500 above.
		if worn == nil {
			worn = []attachments.Attachment{}
		}
		writeJSON(w, http.StatusOK, worn)
		return
	}
	if r.Method != http.MethodPut {
		w.Header().Set("Allow", "GET, PUT")
		writeJSON(w, http.StatusMethodNotAllowed, Error{
			Code: "method_not_allowed", Message: "only GET and PUT are supported"})
		return
	}
	var request attachmentRequest
	if !decodeJSON(w, r, &request) {
		return
	}
	// The point is required when wearing and meaningless when taking off. Zero
	// means "wherever the item says" on the wire the viewer speaks, and the
	// region resolves that before it gets here — an unresolved point stored as
	// worn state is a question mistaken for an answer.
	if !validUUID(request.ItemID) ||
		(request.Worn && (request.AttachmentPoint < 1 || request.AttachmentPoint > 127)) {
		writeJSON(w, http.StatusBadRequest, Error{
			Code:    "invalid_attachment",
			Message: "itemId must be a valid UUID and attachmentPoint 1-127 when worn"})
		return
	}
	var err error
	if request.Worn {
		err = a.attachments.Wear(r.Context(), userID, request.ItemID, request.AttachmentPoint)
	} else {
		err = a.attachments.TakeOff(r.Context(), userID, request.ItemID)
	}
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, Error{
			Code: "attachment_store_error", Message: "attachment update failed"})
		return
	}
	w.WriteHeader(http.StatusNoContent)
}
