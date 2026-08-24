package httpapi

import (
	"errors"
	"io"
	"net/http"
	"strconv"
	"strings"

	"github.com/homeworldz/server/grid/internal/assetmeta"
	"github.com/homeworldz/server/grid/internal/durability"
	"github.com/homeworldz/server/grid/internal/inventory"
	"github.com/homeworldz/server/grid/internal/vault"
)

// vaultAsset serves the asset vault of ADR 0026 at
// /api/v1/vault/assets/{assetId}. It sits under the internal service-token
// boundary with the rest of /api/, which is what keeps the vault out of the
// viewer data path: only regions reach it, and viewers keep fetching asset
// bytes from the region they are connected to.
//
//	PUT   writes bytes through to the vault, verified against the checksum and
//	      length the registry recorded for the asset, and is idempotent.
//	HEAD  reports whether the vault holds the asset's blob, which is the
//	      question the inventory-commit invariant asks.
//	GET   returns the bytes, so a region that cannot reach a peer can always
//	      fall back to the vault for inventory-referenced content.
//
// Addressed by asset rather than by digest because the asset UUID is what a
// region knows: blob_id is grid-internal by ADR 0027, and a bare digest cannot
// say which registered blob an ingest is meant to vouch for.
func (a *API) vaultAsset(w http.ResponseWriter, r *http.Request) {
	if a.vault == nil || a.assets == nil {
		writeJSON(w, http.StatusServiceUnavailable, Error{Code: "vault_unavailable", Message: "asset vault is unavailable"})
		return
	}
	assetID := strings.TrimPrefix(r.URL.Path, "/api/v1/vault/assets/")
	// One name under this prefix is not an asset id: the bulk question a
	// region asks at startup, which is what keeps it from writing every
	// bundled asset through one round trip at a time.
	if assetID == "missing" {
		a.vaultMissingAssets(w, r)
		return
	}
	if strings.Contains(assetID, "/") || !validUUID(assetID) {
		a.notFound(w, r)
		return
	}
	blob, err := a.assets.Blob(r.Context(), assetID)
	if errors.Is(err, assetmeta.ErrNotFound) {
		writeJSON(w, http.StatusNotFound, Error{Code: "asset_not_found", Message: "asset is not registered"})
		return
	} else if err != nil {
		writeJSON(w, http.StatusInternalServerError, Error{Code: "asset_store_error", Message: "asset registry lookup failed"})
		return
	}
	switch r.Method {
	case http.MethodPut:
		a.ingestVaultAsset(w, r, blob.BlobID)
	case http.MethodHead:
		held, err := a.vault.Held(r.Context(), blob.BlobID)
		if writeVaultError(w, err) {
			return
		}
		w.Header().Set("Content-Type", "application/octet-stream")
		w.Header().Set("Content-Length", strconv.FormatInt(held.ByteLength, 10))
		w.WriteHeader(http.StatusOK)
	case http.MethodGet:
		content, held, err := a.vault.Open(r.Context(), blob.BlobID)
		if writeVaultError(w, err) {
			return
		}
		defer content.Close()
		w.Header().Set("Content-Type", "application/octet-stream")
		w.Header().Set("Content-Length", strconv.FormatInt(held.ByteLength, 10))
		w.WriteHeader(http.StatusOK)
		// A copy failure here is a broken connection, not a vault fault, and the
		// status line has already gone out.
		_, _ = io.Copy(w, content)
	default:
		w.Header().Set("Allow", "GET, HEAD, PUT")
		writeJSON(w, http.StatusMethodNotAllowed, Error{Code: "method_not_allowed", Message: "only GET, HEAD, and PUT are supported"})
	}
}

// ingestVaultAsset writes bytes through to the vault. Neither the length nor
// the checksum is taken from the request: both come from the blob registration,
// so a region can only ever confirm the bytes the grid already believes the
// asset to be.
//
// The response is 200 rather than 201 because ingest is idempotent and the same
// bytes commonly arrive more than once — from a re-upload, a second region, or
// a backfill — and the useful answer is "the vault holds this", not whether
// this particular request is what put it there.
func (a *API) ingestVaultAsset(w http.ResponseWriter, r *http.Request, blobID string) {
	if r.ContentLength > vault.MaxBlobSize {
		writeJSON(w, http.StatusRequestEntityTooLarge, Error{Code: "vault_blob_too_large", Message: "blob exceeds the maximum vault blob size"})
		return
	}
	blob, err := a.vault.Ingest(r.Context(), blobID,
		http.MaxBytesReader(w, r.Body, vault.MaxBlobSize+1))
	if writeVaultError(w, err) {
		return
	}
	writeJSON(w, http.StatusOK, blob)
}

func writeVaultError(w http.ResponseWriter, err error) bool {
	if err == nil {
		return false
	}
	switch {
	case errors.Is(err, vault.ErrNotFound):
		writeJSON(w, http.StatusNotFound, Error{Code: "vault_blob_not_found", Message: "the vault does not hold the blob"})
	case errors.Is(err, vault.ErrMismatch):
		writeJSON(w, http.StatusBadRequest, Error{Code: "vault_blob_mismatch", Message: "blob bytes do not match the registered checksum or length"})
	case errors.Is(err, vault.ErrInvalid):
		writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_vault_blob", Message: "the asset's blob registration is unusable"})
	default:
		writeJSON(w, http.StatusInternalServerError, Error{Code: "vault_error", Message: "vault operation failed"})
	}
	return true
}

// writeDurabilityError maps a refused inventory write onto a status a region
// can act on. A 409 says the item was not created and why: retrying is
// pointless until the bytes exist somewhere the grid can reach, which is a
// materially different situation from a transient store failure.
//
// It reports false for anything that is not a durability refusal, so callers
// can put it in front of their existing error switch without changing it.
func writeDurabilityError(w http.ResponseWriter, err error) bool {
	if err == nil || !errors.Is(err, inventory.ErrAssetNotDurable) {
		return false
	}
	switch {
	case errors.Is(err, durability.ErrUnregistered):
		writeJSON(w, http.StatusConflict, Error{Code: "asset_not_registered",
			Message: "the item's asset is not registered with the grid"})
	case errors.Is(err, durability.ErrUnfetchable):
		writeJSON(w, http.StatusConflict, Error{Code: "asset_not_durable",
			Message: "the item's asset bytes could not be stored durably: no registered location served them"})
	case errors.Is(err, durability.ErrVaultUnavailable):
		writeJSON(w, http.StatusServiceUnavailable, Error{Code: "vault_unavailable",
			Message: "asset vault is unavailable, so inventory cannot be committed"})
	default:
		// The refusal stands even when the cause is one this mapping has not
		// met: the item was not created, and reporting that as a server error
		// would invite a retry that cannot succeed.
		writeJSON(w, http.StatusConflict, Error{Code: "asset_not_durable",
			Message: "the item's asset bytes could not be stored durably"})
	}
	return true
}

// maxVaultQuery bounds one bulk question. A region asks about thousands of
// bundled assets, so this is a page size rather than a limit on what it may
// ask overall: welcome asks about 3180 in seven requests instead of making
// 3180 uploads.
const maxVaultQuery = 512

// vaultMissingAssets answers POST /api/v1/vault/assets/missing: given asset
// ids, which of them the vault does NOT durably hold.
//
// Missing rather than held, because missing is what the caller acts on, and
// because the safe direction to fail in is "write it again". An asset this
// grid has never heard of is missing too — the caller's write will say so
// properly, with the registry's own 404.
func (a *API) vaultMissingAssets(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		w.Header().Set("Allow", http.MethodPost)
		writeJSON(w, http.StatusMethodNotAllowed, Error{Code: "method_not_allowed", Message: "only POST is supported"})
		return
	}
	var request VaultAssetQuery
	if !decodeJSON(w, r, &request) {
		return
	}
	if len(request.AssetIDs) == 0 || len(request.AssetIDs) > maxVaultQuery {
		writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_vault_query",
			Message: "assetIds must name between 1 and " + strconv.Itoa(maxVaultQuery) + " assets"})
		return
	}
	for _, assetID := range request.AssetIDs {
		if !validUUID(assetID) {
			writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_vault_query",
				Message: "every assetId must be a UUID"})
			return
		}
	}
	held, err := a.vault.HeldAssets(r.Context(), request.AssetIDs)
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, Error{Code: "vault_error", Message: "vault lookup failed"})
		return
	}
	holding := make(map[string]bool, len(held))
	for _, assetID := range held {
		holding[assetID] = true
	}
	missing := make([]string, 0, len(request.AssetIDs)-len(held))
	for _, assetID := range request.AssetIDs {
		if !holding[assetID] {
			missing = append(missing, assetID)
		}
	}
	writeJSON(w, http.StatusOK, VaultMissingAssets{Missing: missing})
}
