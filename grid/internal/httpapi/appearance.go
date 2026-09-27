package httpapi

import (
	"errors"
	"io"
	"net/http"
	"strconv"
	"strings"

	"github.com/homeworldz/server/grid/internal/assetmeta"
)

// appearanceServicePath is where the grid's appearance service lives
// (ADR 0029, "Announcing the bakes"). Its address is published at login as
// agent_appearance_service; a viewer holding a server-baked avatar builds
//
//	{address}texture/{avatarId}/{slot}/{textureId}
//
// by plain concatenation (llvoavatar.cpp, getImageURL), so the published
// address must end in a slash.
const appearanceServicePath = "/appearance/"

// bakeSlots are the slot names a viewer puts in that path: the default image
// names of the eleven baked texture entries (llavatarappearancedefines.cpp).
var bakeSlots = map[string]bool{
	"head": true, "upper": true, "lower": true, "eyes": true, "hair": true,
	"skirt": true, "leftarm": true, "leftleg": true, "aux1": true, "aux2": true,
	"aux3": true,
}

// appearanceServiceURL is the address published at login, or empty when this
// grid cannot serve a bake — a viewer that is never told the address is one
// that cannot be told a wrong one.
func (a *API) appearanceServiceURL() string {
	if a.assets == nil || a.vault == nil {
		return ""
	}
	return a.publicURL + appearanceServicePath
}

// appearanceTexture serves one baked texture from the vault.
//
// The request carries no credentials — a viewer fetches the bakes of every
// avatar it can see, with nothing but the published address — so this route is
// public, and that is why it serves only assets registered as bakes. Anything
// else, including a notecard or script whose uuid someone holds, is a 404
// indistinguishable from an asset that does not exist.
//
// The avatar id and slot are checked for shape and otherwise ignored: the
// texture id names content-addressed bytes, and a bake is shared by every
// avatar wearing the same outfit.
func (a *API) appearanceTexture(w http.ResponseWriter, r *http.Request) {
	parts := strings.Split(strings.TrimPrefix(r.URL.Path, appearanceServicePath), "/")
	if a.appearanceServiceURL() == "" || len(parts) != 4 || parts[0] != "texture" ||
		!validUUID(parts[1]) || !bakeSlots[parts[2]] || !validUUID(parts[3]) {
		a.notFound(w, r)
		return
	}
	textureID := parts[3]
	asset, err := a.assets.Get(r.Context(), textureID)
	if errors.Is(err, assetmeta.ErrNotFound) || (err == nil && !asset.Bake) {
		writeJSON(w, http.StatusNotFound, Error{Code: "bake_not_found", Message: "no bake has that texture id"})
		return
	} else if err != nil {
		writeJSON(w, http.StatusInternalServerError, Error{Code: "asset_store_error", Message: "bake lookup failed"})
		return
	}
	blob, err := a.assets.Blob(r.Context(), textureID)
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, Error{Code: "asset_store_error", Message: "bake lookup failed"})
		return
	}
	content, held, err := a.vault.Open(r.Context(), blob.BlobID)
	if err != nil {
		// A registered bake the vault does not hold is the region's
		// write-through having failed. Named in the log, because to the viewer
		// it is one grey slot on one avatar and nothing more.
		a.logger.Warn("registered bake is not in the vault",
			"textureId", textureID, "slot", parts[2], "error", err)
	}
	if writeVaultError(w, err) {
		return
	}
	defer content.Close()
	w.Header().Set("Content-Type", "image/x-j2c")
	w.Header().Set("Content-Length", strconv.FormatInt(held.ByteLength, 10))
	// Content-addressed: the bytes behind a texture id never change.
	w.Header().Set("Cache-Control", "public, max-age=31536000, immutable")
	w.WriteHeader(http.StatusOK)
	_, _ = io.Copy(w, content)
}
