package httpapi

import (
	"html"
	"net/http"
	"regexp"
	"strconv"
	"strings"

	"github.com/homeworldz/server/grid/internal/inventory"
)

// llsdLoginKeyString matches top-level string members of the LLSD login request
// map (`<key>name</key><string>value</string>`). Non-string members (e.g. the
// `options` array) are intentionally ignored — the login only needs the string
// fields first/last/passwd/start.
var llsdLoginKeyString = regexp.MustCompile(`(?s)<key>\s*([A-Za-z0-9_\-]+)\s*</key>\s*<string>(.*?)</string>`)

// viewerLoginLLSD handles an LLSD-format login as used by modern viewers and
// LibreMetaverse: the request is an LLSD map and the reply is an LLSD map with
// the same fields the XML-RPC path produces.
func (a *API) viewerLoginLLSD(w http.ResponseWriter, r *http.Request, body []byte) {
	request := map[string]string{}
	for _, match := range llsdLoginKeyString.FindAllSubmatch(body, -1) {
		request[string(match[1])] = html.UnescapeString(string(match[2]))
	}
	result, reason, message := a.resolveViewerLogin(r,
		request["first"], request["last"], request["passwd"], request["start"])
	if reason != "" {
		writeLLSD(w, llsdLoginFailure(reason, message))
		return
	}
	writeLLSD(w, a.llsdLoginResponse(result))
}

func writeLLSD(w http.ResponseWriter, doc string) {
	w.Header().Set("Content-Type", "application/llsd+xml; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write([]byte(doc))
}

func llsdLoginFailure(reason, message string) string {
	var b strings.Builder
	b.WriteString(`<?xml version="1.0"?><llsd><map>`)
	llsdKeyString(&b, "login", "false")
	llsdKeyString(&b, "reason", reason)
	llsdKeyString(&b, "message", message)
	b.WriteString("</map></llsd>")
	return b.String()
}

func (a *API) llsdLoginResponse(f *loginFields) string {
	var b strings.Builder
	b.WriteString(`<?xml version="1.0"?><llsd><map>`)
	llsdKeyString(&b, "login", "true")
	llsdKeyString(&b, "message", "Welcome to "+a.gridName)
	llsdKeyUUID(&b, "agent_id", f.agentID)
	llsdKeyUUID(&b, "session_id", f.sessionID)
	llsdKeyUUID(&b, "secure_session_id", f.secureID)
	llsdKeyString(&b, "first_name", f.first)
	llsdKeyString(&b, "last_name", f.last)
	llsdKeyInt(&b, "circuit_code", int(f.circuit))
	llsdKeyString(&b, "sim_ip", f.simIP)
	llsdKeyInt(&b, "sim_port", f.simPort)
	llsdKeyInt(&b, "region_x", f.regionX)
	llsdKeyInt(&b, "region_y", f.regionY)
	llsdKeyInt(&b, "region_size_x", f.regionSizeX)
	llsdKeyInt(&b, "region_size_y", f.regionSizeY)
	llsdKeyString(&b, "start_location", f.startLocation)
	llsdKeyString(&b, "look_at", f.lookAt)
	llsdKeyString(&b, "seed_capability", f.seedCapability)
	if f.appearanceService != "" {
		llsdKeyString(&b, "agent_appearance_service", f.appearanceService)
	}

	b.WriteString("<key>inventory-root</key><array><map>")
	llsdKeyUUID(&b, "folder_id", inventoryRootID(f.folders))
	b.WriteString("</map></array>")
	b.WriteString("<key>inventory-skeleton</key>")
	llsdFolderArray(&b, f.folders)

	b.WriteString("<key>inventory-lib-root</key><array><map>")
	llsdKeyUUID(&b, "folder_id", inventory.LibraryRootID)
	b.WriteString("</map></array>")
	b.WriteString("<key>inventory-lib-owner</key><array><map>")
	llsdKeyUUID(&b, "agent_id", inventory.LibraryOwnerID)
	b.WriteString("</map></array>")
	b.WriteString("<key>inventory-skel-lib</key>")
	llsdFolderArray(&b, f.libFolders)

	b.WriteString("<key>login-flags</key><array></array>")
	b.WriteString("<key>gestures</key><array>")
	for _, g := range f.gestures {
		b.WriteString("<map>")
		llsdKeyUUID(&b, "item_id", g.ItemID)
		llsdKeyUUID(&b, "asset_id", g.AssetID)
		b.WriteString("</map>")
	}
	b.WriteString("</array>")
	b.WriteString("<key>buddy-list</key><array></array>")
	b.WriteString("</map></llsd>")
	return b.String()
}

func inventoryRootID(folders []inventory.Folder) string {
	for _, folder := range folders {
		if folder.TypeDefault == 8 {
			return folder.ID
		}
	}
	return ""
}

func llsdFolderArray(b *strings.Builder, folders []inventory.Folder) {
	b.WriteString("<array>")
	for _, folder := range folders {
		b.WriteString("<map>")
		llsdKeyString(b, "name", folder.Name)
		llsdKeyUUID(b, "folder_id", folder.ID)
		llsdKeyUUID(b, "parent_id", folder.ParentID)
		llsdKeyInt(b, "version", int(folder.Version))
		llsdKeyInt(b, "type_default", folder.TypeDefault)
		b.WriteString("</map>")
	}
	b.WriteString("</array>")
}

func llsdKeyString(b *strings.Builder, key, value string) {
	b.WriteString("<key>" + key + "</key><string>" + html.EscapeString(value) + "</string>")
}

func llsdKeyUUID(b *strings.Builder, key, value string) {
	b.WriteString("<key>" + key + "</key><uuid>" + html.EscapeString(value) + "</uuid>")
}

func llsdKeyInt(b *strings.Builder, key string, value int) {
	b.WriteString("<key>" + key + "</key><integer>" + strconv.Itoa(value) + "</integer>")
}
