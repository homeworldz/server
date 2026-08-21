package httpapi

import (
	"bytes"
	"context"
	"crypto/md5"
	"encoding/hex"
	"encoding/xml"
	"errors"
	"fmt"
	"html"
	"io"
	"net/http"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/homeworldz/server/grid/internal/assetmeta"
	"github.com/homeworldz/server/grid/internal/identifier"
	"github.com/homeworldz/server/grid/internal/identity"
	"github.com/homeworldz/server/grid/internal/inventory"
)

func (a *API) inventoryAISCapability(w http.ResponseWriter, r *http.Request) {
	if a.identity == nil || a.inventory == nil {
		writeLLSDError(w, http.StatusServiceUnavailable, "inventory is unavailable")
		return
	}
	parts := strings.Split(strings.TrimPrefix(r.URL.Path, "/caps/inventory/ais/"), "/")
	if len(parts) < 2 || !validUUID(parts[0]) {
		writeLLSDError(w, http.StatusNotFound, "AIS inventory capability was not found")
		return
	}
	session, err := a.identity.ValidateSession(r.Context(), parts[0])
	if errors.Is(err, identity.ErrSessionNotFound) ||
		(err == nil && (session.ViewerCircuitCode == 0 || session.DestinationRegionID == "")) {
		writeLLSDError(w, http.StatusNotFound, "AIS inventory capability expired")
		return
	}
	if err != nil {
		writeLLSDError(w, http.StatusServiceUnavailable, "session validation failed")
		return
	}
	if r.Method == http.MethodGet {
		switch {
		case len(parts) == 3 && parts[1] == "item" && validUUID(parts[2]):
			a.fetchAISInventoryItem(w, r, session.UserID, parts[2])
			return
		case len(parts) == 4 && parts[1] == "category" && validUUID(parts[2]) && parts[3] == "children":
			if inventory.IsLibraryFolder(parts[2]) {
				writeAISInventoryFolder(w, r, inventory.LibraryOwnerID, parts[2],
					inventory.LibraryFolders(), inventory.LibraryItems(), false)
			} else {
				a.fetchAISInventoryFolder(w, r, session.UserID, parts[2], false)
			}
			return
		case len(parts) == 4 && parts[1] == "category" && parts[2] == "current" && parts[3] == "links":
			a.fetchAISInventoryFolder(w, r, session.UserID, inventory.SystemFolderID(session.UserID, 46), true)
			return
		case len(parts) == 4 && parts[1] == "category" && validUUID(parts[2]) && parts[3] == "links":
			// Fetch only the link items of an arbitrary folder (e.g. an outfit
			// folder). Firestorm reads a saved outfit's links this way during
			// "Replace Current Outfit" before slamming them into the COF.
			if inventory.IsLibraryFolder(parts[2]) {
				writeAISInventoryFolder(w, r, inventory.LibraryOwnerID, parts[2],
					inventory.LibraryFolders(), inventory.LibraryItems(), true)
			} else {
				a.fetchAISInventoryFolder(w, r, session.UserID, parts[2], true)
			}
			return
		case len(parts) == 2 && parts[1] == "orphans":
			writeAISEmptyOrphans(w)
			return
		default:
			writeLLSDError(w, http.StatusNotFound, "AIS inventory resource was not found")
			return
		}
	}
	// Everything past here mutates. Rather than name the handlers that can
	// touch the Current Outfit — wearing, taking off, slamming a saved outfit,
	// moving an item, deleting one — read the folder's version either side of
	// the dispatch and let the store say whether it changed. A path added later
	// is covered without anyone remembering to add it here, which is the whole
	// reason the check sits at the fork rather than in the handlers.
	// One at a time per user, for the whole of a mutation and the version read
	// that follows it. A folder's version is read after the change commits, so
	// two overlapping mutations each answer with the folder as it looked once
	// both had landed: neither response describes its own change, and the
	// versions reach the viewer out of order.
	//
	// Firestorm notices. Dressing an avatar in fifteen attachments produced
	// twenty-four "Possible version mismatch for category Current Outfit" with
	// AIS versions going backwards — 2371, 2365, 2364, 2366, 2363 — and each
	// one makes the viewer re-read and re-reconcile the outfit it is already
	// wearing (observed 2026-08-21). Serializing costs nothing here: these are
	// short database operations, and one user's inventory changes are ordered
	// from that user's point of view anyway.
	unlock := a.lockInventoryMutations(session.UserID)
	defer unlock()
	outfitBefore := a.currentOutfitVersion(r.Context(), session.UserID)
	defer func() {
		if a.currentOutfitVersion(r.Context(), session.UserID) != outfitBefore {
			a.notifyOutfitChanged(r.Context(), session.UserID)
		}
	}()
	if len(parts) == 4 && parts[1] == "category" && validUUID(parts[2]) && parts[3] == "children" {
		if r.Method != http.MethodDelete {
			w.Header().Set("Allow", "GET, DELETE")
			writeLLSDError(w, http.StatusMethodNotAllowed, "only GET and DELETE are supported")
			return
		}
		a.purgeAISTrash(w, r, session.UserID, parts[2])
		return
	}
	if len(parts) == 4 && parts[1] == "category" && validUUID(parts[2]) && parts[3] == "links" {
		if r.Method != http.MethodPut {
			w.Header().Set("Allow", http.MethodPut)
			writeLLSDError(w, http.StatusMethodNotAllowed, "only PUT is supported")
			return
		}
		a.slamAISInventoryLinks(w, r, session.UserID, parts[2])
		return
	}
	if len(parts) != 3 || !validUUID(parts[2]) {
		writeLLSDError(w, http.StatusNotFound, "AIS inventory capability was not found")
		return
	}
	switch parts[1] {
	case "category":
		switch r.Method {
		case http.MethodPost:
			a.createAISInventoryFolder(w, r, session.UserID, parts[2])
		case http.MethodPatch:
			a.updateAISInventoryFolder(w, r, session.UserID, parts[2])
		default:
			w.Header().Set("Allow", "POST, PATCH")
			writeLLSDError(w, http.StatusMethodNotAllowed, "only POST and PATCH are supported")
		}
	case "item":
		switch r.Method {
		case http.MethodPatch:
			a.updateAISInventoryItem(w, r, session.UserID, session.SecureID, parts[2])
		case http.MethodDelete:
			a.deleteAISInventoryItem(w, r, session.UserID, parts[2])
		default:
			w.Header().Set("Allow", "PATCH, DELETE")
			writeLLSDError(w, http.StatusMethodNotAllowed, "only PATCH and DELETE are supported")
		}
	default:
		writeLLSDError(w, http.StatusNotFound, "AIS inventory resource was not found")
	}
}

func (a *API) purgeAISTrash(w http.ResponseWriter, r *http.Request, userID, folderID string) {
	if folderID != inventory.SystemFolderID(userID, 14) {
		writeLLSDError(w, http.StatusForbidden, "only the Trash folder can be purged")
		return
	}
	folderIDs, itemIDs, folder, err := a.inventory.PurgeFolder(r.Context(), userID, folderID)
	if errors.Is(err, inventory.ErrFolderNotFound) {
		writeLLSDError(w, http.StatusNotFound, "AIS Trash folder was not found")
		return
	}
	if err != nil {
		writeLLSDError(w, http.StatusServiceUnavailable, "AIS Trash could not be emptied")
		return
	}
	var removedFolders, removedItems strings.Builder
	for _, id := range folderIDs {
		removedFolders.WriteString("<uuid>" + html.EscapeString(id) + "</uuid>")
	}
	for _, id := range itemIDs {
		removedItems.WriteString("<uuid>" + html.EscapeString(id) + "</uuid>")
	}
	w.Header().Set("Content-Type", "application/llsd+xml; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	_, _ = io.WriteString(w, "<?xml version=\"1.0\"?><llsd><map>"+
		"<key>_categories_removed</key><array>"+removedFolders.String()+"</array>"+
		"<key>_removed_items</key><array>"+removedItems.String()+"</array>"+
		inventoryFolderVersionsXML(map[string]int64{folder.ID: folder.Version})+
		"</map></llsd>")
}

func (a *API) fetchAISInventoryItem(w http.ResponseWriter, r *http.Request, userID, itemID string) {
	items, err := a.inventory.ListItems(r.Context(), userID)
	if err != nil {
		writeLLSDError(w, http.StatusServiceUnavailable, "AIS inventory item could not be loaded")
		return
	}
	for _, item := range items {
		if item.ID != itemID {
			continue
		}
		w.Header().Set("Content-Type", "application/llsd+xml; charset=utf-8")
		w.WriteHeader(http.StatusOK)
		_, _ = io.WriteString(w, "<?xml version=\"1.0\"?><llsd>"+inventoryAISItemXML(item, item.AssetType == 24)+"</llsd>")
		return
	}
	writeLLSDError(w, http.StatusNotFound, "AIS inventory item was not found")
}

func (a *API) libraryAISCapability(w http.ResponseWriter, r *http.Request) {
	if a.identity == nil || a.inventory == nil {
		writeLLSDError(w, http.StatusServiceUnavailable, "inventory library is unavailable")
		return
	}
	parts := strings.Split(strings.TrimPrefix(r.URL.Path, "/caps/inventory/library/"), "/")
	if len(parts) < 2 || !validUUID(parts[0]) {
		writeLLSDError(w, http.StatusNotFound, "AIS library capability was not found")
		return
	}
	session, err := a.identity.ValidateSession(r.Context(), parts[0])
	if errors.Is(err, identity.ErrSessionNotFound) ||
		(err == nil && (session.ViewerCircuitCode == 0 || session.DestinationRegionID == "")) {
		writeLLSDError(w, http.StatusNotFound, "AIS library capability expired")
		return
	}
	if err != nil {
		writeLLSDError(w, http.StatusServiceUnavailable, "session validation failed")
		return
	}
	switch {
	case r.Method == "COPY" && len(parts) == 3 && parts[1] == "category" && validUUID(parts[2]):
		a.copyAISLibraryCategory(w, r, session.UserID, parts[2])
	case len(parts) == 3 && parts[1] == "category" && validUUID(parts[2]):
		w.Header().Set("Allow", "COPY")
		writeLLSDError(w, http.StatusMethodNotAllowed, "only AIS library category copying is supported")
	case len(parts) == 4 && parts[1] == "category" && validUUID(parts[2]) && parts[3] == "children":
		if r.Method != http.MethodGet {
			w.Header().Set("Allow", http.MethodGet)
			writeLLSDError(w, http.StatusMethodNotAllowed, "this inventory library resource is read-only")
			return
		}
		writeAISInventoryFolder(w, r, inventory.LibraryOwnerID, parts[2],
			inventory.LibraryFolders(), inventory.LibraryItems(), false)
	case len(parts) == 3 && parts[1] == "item" && validUUID(parts[2]):
		if r.Method != http.MethodGet {
			w.Header().Set("Allow", http.MethodGet)
			writeLLSDError(w, http.StatusMethodNotAllowed, "this inventory library resource is read-only")
			return
		}
		writeAISLibraryItem(w, parts[2])
	case len(parts) == 2 && parts[1] == "orphans":
		if r.Method != http.MethodGet {
			w.Header().Set("Allow", http.MethodGet)
			writeLLSDError(w, http.StatusMethodNotAllowed, "this inventory library resource is read-only")
			return
		}
		writeAISEmptyOrphans(w)
	default:
		writeLLSDError(w, http.StatusNotFound, "AIS library resource was not found")
	}
}

func (a *API) copyAISLibraryCategory(w http.ResponseWriter, r *http.Request, userID, sourceID string) {
	destinationID := strings.TrimSpace(r.Header.Get("Destination"))
	if !validUUID(destinationID) || destinationID == nullInventoryFolderID {
		writeLLSDError(w, http.StatusBadRequest, "AIS library copy destination is invalid")
		return
	}
	var source inventory.Folder
	for _, folder := range inventory.LibraryFolders() {
		if folder.ID == sourceID {
			source = folder
			break
		}
	}
	if source.ID == "" {
		writeLLSDError(w, http.StatusNotFound, "AIS library category was not found")
		return
	}
	folderID, err := identifier.NewUUID()
	if err != nil {
		writeLLSDError(w, http.StatusServiceUnavailable, "inventory folder ID could not be allocated")
		return
	}
	createdFolder, err := a.inventory.CreateFolder(r.Context(), inventory.Folder{
		ID: folderID, OwnerUserID: userID, ParentID: destinationID,
		Name: source.Name, TypeDefault: -1,
	})
	if writeAISFolderError(w, err) {
		return
	}
	items := make([]inventory.Item, 0, 16)
	for _, sourceItem := range inventory.LibraryItems() {
		if sourceItem.FolderID != sourceID {
			continue
		}
		itemID, err := identifier.NewUUID()
		if err != nil {
			writeLLSDError(w, http.StatusServiceUnavailable, "inventory item ID could not be allocated")
			return
		}
		items = append(items, inventory.Item{
			ID: itemID, OwnerUserID: userID, CreatorUserID: sourceItem.CreatorUserID,
			FolderID: folderID, AssetID: sourceItem.AssetID, AssetType: sourceItem.AssetType,
			InventoryType: sourceItem.InventoryType, Name: sourceItem.Name,
			Description: sourceItem.Description, Flags: sourceItem.Flags,
			BasePermissions: sourceItem.BasePermissions, CurrentPermissions: sourceItem.CurrentPermissions,
			EveryonePermissions: sourceItem.EveryonePermissions, NextPermissions: sourceItem.NextPermissions,
			SaleType: sourceItem.SaleType, SalePrice: sourceItem.SalePrice,
		})
	}
	if len(items) > 0 {
		items, err = a.inventory.CreateItems(r.Context(), items)
		if writeAISItemError(w, err) {
			return
		}
	}
	versions, err := a.inventoryFolderVersions(r.Context(), userID, destinationID, folderID)
	if err != nil {
		writeLLSDError(w, http.StatusServiceUnavailable, "AIS inventory folder version could not be loaded")
		return
	}
	createdFolder.Version = versions[folderID]
	writeAISLibraryCategoryCopy(w, createdFolder, items, versions)
}

func writeAISLibraryCategoryCopy(w http.ResponseWriter, folder inventory.Folder, items []inventory.Item,
	versions map[string]int64) {
	var itemIDs, embeddedItems strings.Builder
	for _, item := range items {
		itemIDs.WriteString("<uuid>" + html.EscapeString(item.ID) + "</uuid>")
		embeddedItems.WriteString("<key>" + html.EscapeString(item.ID) + "</key>")
		embeddedItems.WriteString(inventoryAISItemXML(item, false))
	}
	w.Header().Set("Content-Type", "application/llsd+xml; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	_, _ = fmt.Fprintf(w, "<?xml version=\"1.0\"?><llsd><map>"+
		"<key>category_id</key><uuid>%s</uuid><key>parent_id</key><uuid>%s</uuid>"+
		"<key>agent_id</key><uuid>%s</uuid><key>type_default</key><integer>%d</integer>"+
		"<key>name</key><string>%s</string><key>version</key><integer>%d</integer>"+
		"<key>_created_categories</key><array><uuid>%s</uuid></array>"+
		"<key>_created_items</key><array>%s</array>%s"+
		"<key>_embedded</key><map><key>categories</key><map></map>"+
		"<key>items</key><map>%s</map><key>links</key><map></map></map></map></llsd>",
		html.EscapeString(folder.ID), html.EscapeString(folder.ParentID),
		html.EscapeString(folder.OwnerUserID), folder.TypeDefault, html.EscapeString(folder.Name), folder.Version,
		html.EscapeString(folder.ID), itemIDs.String(), inventoryFolderVersionsXML(versions), embeddedItems.String())
}

func writeAISLibraryItem(w http.ResponseWriter, itemID string) {
	for _, item := range inventory.LibraryItems() {
		if item.ID != itemID {
			continue
		}
		w.Header().Set("Content-Type", "application/llsd+xml; charset=utf-8")
		w.WriteHeader(http.StatusOK)
		_, _ = io.WriteString(w, "<?xml version=\"1.0\"?><llsd>"+inventoryAISItemXML(item, false)+"</llsd>")
		return
	}
	writeLLSDError(w, http.StatusNotFound, "AIS library item was not found")
}

func (a *API) fetchAISInventoryFolder(w http.ResponseWriter, r *http.Request, userID, folderID string, linksOnly bool) {
	folders, err := a.inventory.ListFolders(r.Context(), userID)
	if err != nil {
		writeLLSDError(w, http.StatusServiceUnavailable, "AIS inventory folders could not be loaded")
		return
	}
	items, err := a.inventory.ListItems(r.Context(), userID)
	if err != nil {
		writeLLSDError(w, http.StatusServiceUnavailable, "AIS inventory items could not be loaded")
		return
	}
	writeAISInventoryFolder(w, r, userID, folderID, folders, items, linksOnly)
}

func writeAISInventoryFolder(w http.ResponseWriter, r *http.Request, userID, folderID string,
	folders []inventory.Folder, items []inventory.Item, linksOnly bool) {
	var requested inventory.Folder
	for _, folder := range folders {
		if folder.ID == folderID {
			requested = folder
			break
		}
	}
	if requested.ID == "" {
		writeLLSDError(w, http.StatusNotFound, "AIS inventory folder was not found")
		return
	}
	depth := 0
	if value := r.URL.Query().Get("depth"); value != "" && value != "*" {
		if parsed, parseErr := strconv.Atoi(value); parseErr == nil && parsed > 0 {
			depth = min(parsed, 8)
		}
	} else if value == "*" {
		depth = 8
	}
	embedded := inventoryAISEmbeddedXML(requested.ID, userID, folders, items, depth, linksOnly)
	w.Header().Set("Content-Type", "application/llsd+xml; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	_, _ = io.WriteString(w, "<?xml version=\"1.0\"?><llsd><map>"+
		"<key>category_id</key><uuid>"+html.EscapeString(requested.ID)+"</uuid>"+
		"<key>folder_id</key><uuid>"+html.EscapeString(requested.ID)+"</uuid>"+
		"<key>agent_id</key><uuid>"+html.EscapeString(userID)+"</uuid>"+
		"<key>parent_id</key><uuid>"+html.EscapeString(requested.ParentID)+"</uuid>"+
		"<key>type_default</key><integer>"+fmt.Sprint(requested.TypeDefault)+"</integer>"+
		"<key>name</key><string>"+html.EscapeString(requested.Name)+"</string>"+
		"<key>version</key><integer>"+fmt.Sprint(requested.Version)+"</integer>"+
		embedded+"</map></llsd>")
}

func inventoryAISFolderXML(folder inventory.Folder, userID string) string {
	return fmt.Sprintf("<map><key>category_id</key><uuid>%s</uuid><key>folder_id</key><uuid>%s</uuid>"+
		"<key>agent_id</key><uuid>%s</uuid><key>parent_id</key><uuid>%s</uuid>"+
		"<key>type_default</key><integer>%d</integer><key>name</key><string>%s</string>"+
		"<key>version</key><integer>%d</integer></map>",
		html.EscapeString(folder.ID), html.EscapeString(folder.ID), html.EscapeString(userID),
		html.EscapeString(folder.ParentID), folder.TypeDefault, html.EscapeString(folder.Name), folder.Version)
}

func inventoryAISEmbeddedXML(folderID, userID string, folders []inventory.Folder, items []inventory.Item,
	depth int, linksOnly bool) string {
	var categories, ordinaryItems, links strings.Builder
	itemsByID := make(map[string]inventory.Item, len(items))
	for _, item := range items {
		itemsByID[item.ID] = item
	}
	if !linksOnly {
		for _, folder := range folders {
			if folder.ParentID != folderID {
				continue
			}
			categories.WriteString("<key>" + html.EscapeString(folder.ID) + "</key>")
			folderXML := inventoryAISFolderXML(folder, userID)
			if depth > 0 {
				folderXML = strings.TrimSuffix(folderXML, "</map>") +
					inventoryAISEmbeddedXML(folder.ID, userID, folders, items, depth-1, false) + "</map>"
			}
			categories.WriteString(folderXML)
		}
	}
	for _, item := range items {
		if item.FolderID != folderID {
			continue
		}
		if item.AssetType == 24 {
			links.WriteString("<key>" + html.EscapeString(item.ID) + "</key>")
			source, found := itemsByID[item.AssetID]
			links.WriteString(inventoryAISLinkXML(item, source, found))
		} else if !linksOnly {
			ordinaryItems.WriteString("<key>" + html.EscapeString(item.ID) + "</key>")
			ordinaryItems.WriteString(inventoryAISItemXML(item, false))
		}
	}
	return "<key>_embedded</key><map>" +
		"<key>categories</key><map>" + categories.String() + "</map>" +
		"<key>items</key><map>" + ordinaryItems.String() + "</map>" +
		"<key>links</key><map>" + links.String() + "</map></map>"
}

func inventoryAISLinkXML(link inventory.Item, source inventory.Item, sourceFound bool) string {
	content := inventoryAISItemXML(link, true)
	if !sourceFound {
		return content
	}
	return strings.TrimSuffix(content, "</map>") +
		"<key>_embedded</key><map><key>item</key>" + inventoryItemXML(source) + "</map></map>"
}

func inventoryAISItemXML(item inventory.Item, link bool) string {
	if link {
		created := item.CreatedAt.Unix()
		if item.CreatedAt.IsZero() {
			created = 0
		}
		return fmt.Sprintf("<map><key>item_id</key><uuid>%s</uuid>"+
			"<key>parent_id</key><uuid>%s</uuid><key>agent_id</key><uuid>%s</uuid>"+
			"<key>linked_id</key><uuid>%s</uuid><key>type</key><integer>%d</integer>"+
			"<key>inv_type</key><integer>%d</integer><key>name</key><string>%s</string>"+
			"<key>desc</key><string>%s</string><key>created_at</key><integer>%d</integer></map>",
			html.EscapeString(item.ID), html.EscapeString(item.FolderID),
			html.EscapeString(item.OwnerUserID), html.EscapeString(item.AssetID),
			item.AssetType, item.InventoryType, html.EscapeString(item.Name),
			html.EscapeString(item.Description), created)
	}
	return inventoryItemXML(item)
}

func writeAISEmptyOrphans(w http.ResponseWriter) {
	w.Header().Set("Content-Type", "application/llsd+xml; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	_, _ = io.WriteString(w, "<?xml version=\"1.0\"?><llsd><map><key>_embedded</key><map>"+
		"<key>categories</key><map></map><key>items</key><map></map>"+
		"<key>links</key><map></map></map></map></llsd>")
}

func (a *API) createAISInventoryFolder(w http.ResponseWriter, r *http.Request, userID, parentID string) {
	body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, 256*1024))
	if err != nil {
		writeLLSDError(w, http.StatusBadRequest, "invalid AIS inventory request")
		return
	}
	links, foundLinks, err := decodeAISInventoryLinks(bytes.NewReader(body))
	if foundLinks {
		if err != nil {
			writeLLSDError(w, http.StatusBadRequest, "invalid AIS inventory link request")
			return
		}
		a.createAISInventoryLinks(w, r, userID, parentID, links)
		return
	}
	request, err := parseInventoryFolderMutationRequest(bytes.NewReader(body), "folder_id")
	// Firestorm represents the not-yet-allocated category UUID as an empty
	// <uuid/> element. Treat it like the canonical null UUID while still
	// requiring the category_id field to be present in the AIS request.
	if request.ID == "" {
		request.ID = nullInventoryFolderID
	}
	validFolderType := request.Type == -1 ||
		(request.Type == 47 && parentID == inventory.SystemFolderID(userID, 48))
	if err != nil || !validUUID(request.ID) || !validUUID(request.ParentID) ||
		request.ParentID != parentID || !validFolderType {
		writeLLSDError(w, http.StatusBadRequest, "invalid AIS inventory folder request")
		return
	}
	if request.ID == nullInventoryFolderID {
		request.ID, err = identifier.NewUUID()
		if err != nil {
			writeLLSDError(w, http.StatusServiceUnavailable, "inventory folder ID could not be allocated")
			return
		}
	}
	folder, err := a.inventory.CreateFolder(r.Context(), inventory.Folder{
		ID: request.ID, OwnerUserID: userID, ParentID: request.ParentID,
		Name: request.Name, TypeDefault: request.Type,
	})
	if writeAISFolderError(w, err) {
		return
	}
	writeAISUUIDArray(w, "_created_categories", folder.ID)
}

type aisInventoryLink struct {
	LinkedID      string
	AssetType     int
	InventoryType int
	HasInvType    bool
	Name          string
	Description   string
}

func decodeAISInventoryLinks(reader io.Reader) ([]aisInventoryLink, bool, error) {
	decoder := xml.NewDecoder(reader)
	var key string
	sawLLSD := false
	for {
		token, err := decoder.Token()
		if errors.Is(err, io.EOF) {
			return nil, false, nil
		}
		if err != nil {
			return nil, false, err
		}
		switch value := token.(type) {
		case xml.StartElement:
			switch value.Name.Local {
			case "llsd":
				sawLLSD = true
			case "key":
				if err := decoder.DecodeElement(&key, &value); err != nil {
					return nil, false, err
				}
			case "array":
				if sawLLSD && (strings.TrimSpace(key) == "links" || strings.TrimSpace(key) == "") {
					links, err := decodeAISInventoryLinkArray(decoder)
					return links, true, err
				}
			}
		}
	}
}

func decodeAISInventoryLinkArray(decoder *xml.Decoder) ([]aisInventoryLink, error) {
	links := make([]aisInventoryLink, 0, 8)
	for {
		token, err := decoder.Token()
		if err != nil {
			return nil, err
		}
		switch value := token.(type) {
		case xml.StartElement:
			if value.Name.Local != "map" {
				return nil, errors.New("AIS inventory links array contains a non-map value")
			}
			link, err := decodeAISInventoryLinkMap(decoder)
			if err != nil {
				return nil, err
			}
			links = append(links, link)
			if len(links) > 256 {
				return nil, errors.New("AIS inventory link batch is too large")
			}
		case xml.EndElement:
			if value.Name.Local == "array" {
				return links, nil
			}
		}
	}
}

func decodeAISInventoryLinkMap(decoder *xml.Decoder) (aisInventoryLink, error) {
	var link aisInventoryLink
	var key string
	seen := make(map[string]bool)
	for {
		token, err := decoder.Token()
		if err != nil {
			return link, err
		}
		switch value := token.(type) {
		case xml.StartElement:
			if value.Name.Local == "key" {
				if err := decoder.DecodeElement(&key, &value); err != nil {
					return link, err
				}
				key = strings.TrimSpace(key)
				continue
			}
			if key == "" {
				return link, errors.New("AIS inventory link value has no key")
			}
			if seen[key] {
				return link, errors.New("AIS inventory link repeats a field")
			}
			var field string
			if err := decoder.DecodeElement(&field, &value); err != nil {
				return link, err
			}
			field = strings.TrimSpace(field)
			switch key {
			case "linked_id":
				link.LinkedID = field
			case "type":
				link.AssetType, err = strconv.Atoi(field)
			case "inv_type":
				link.InventoryType, err = strconv.Atoi(field)
				link.HasInvType = err == nil
			case "name":
				link.Name = field
			case "desc":
				link.Description = field
			default:
				key = ""
				continue
			}
			if err != nil {
				return link, err
			}
			seen[key] = true
			key = ""
		case xml.EndElement:
			if value.Name.Local == "map" {
				if !seen["linked_id"] || !seen["type"] || !seen["name"] {
					return link, errors.New("AIS inventory link omits a required field")
				}
				return link, nil
			}
		}
	}
}

func (a *API) createAISInventoryLinks(w http.ResponseWriter, r *http.Request, userID, parentID string,
	requests []aisInventoryLink) {
	if len(requests) == 0 {
		writeLLSDError(w, http.StatusBadRequest, "AIS inventory link batch is empty")
		return
	}
	folders, err := a.inventory.ListFolders(r.Context(), userID)
	if err != nil {
		writeLLSDError(w, http.StatusServiceUnavailable, "AIS inventory folder could not be loaded")
		return
	}
	parentFound := false
	folderByID := make(map[string]inventory.Folder, len(folders))
	for _, folder := range folders {
		folderByID[folder.ID] = folder
		if folder.ID == parentID {
			parentFound = true
		}
	}
	if !parentFound {
		writeLLSDError(w, http.StatusNotFound, "AIS inventory destination folder was not found")
		return
	}
	sourceItems, err := a.inventory.ListItems(r.Context(), userID)
	if err != nil {
		writeLLSDError(w, http.StatusServiceUnavailable, "AIS inventory items could not be loaded")
		return
	}
	sources := make(map[string]inventory.Item, len(sourceItems))
	for _, item := range sourceItems {
		sources[item.ID] = item
	}
	created := make([]inventory.Item, 0, len(requests))
	seenSources := make(map[string]bool, len(requests))
	for _, request := range requests {
		if !validUUID(request.LinkedID) || request.Name == "" ||
			len(request.Name) > 255 || len(request.Description) > 1024 || seenSources[request.LinkedID] {
			writeLLSDError(w, http.StatusBadRequest, "invalid AIS inventory link request")
			return
		}
		itemID, err := identifier.NewUUID()
		if err != nil {
			writeLLSDError(w, http.StatusServiceUnavailable, "inventory link ID could not be allocated")
			return
		}
		switch request.AssetType {
		case 24: // item link
			source, ok := sources[request.LinkedID]
			if !ok {
				writeLLSDError(w, http.StatusNotFound, "AIS inventory link source was not found")
				return
			}
			if request.HasInvType && request.InventoryType != source.InventoryType {
				writeLLSDError(w, http.StatusBadRequest, "invalid AIS inventory link request")
				return
			}
			creatorID := source.CreatorUserID
			if creatorID == "" || creatorID == nullInventoryFolderID {
				creatorID = userID
			}
			created = append(created, inventory.Item{
				ID: itemID, OwnerUserID: userID, CreatorUserID: creatorID, FolderID: parentID,
				AssetID: request.LinkedID, AssetType: 24, InventoryType: source.InventoryType,
				Name: request.Name, Description: request.Description, Flags: source.Flags,
				BasePermissions: source.BasePermissions, CurrentPermissions: source.CurrentPermissions,
				EveryonePermissions: source.EveryonePermissions, NextPermissions: source.NextPermissions,
			})
		case 25: // folder link (outfit link) — points at a folder, not an item
			if _, ok := folderByID[request.LinkedID]; !ok {
				writeLLSDError(w, http.StatusNotFound, "AIS inventory link source folder was not found")
				return
			}
			created = append(created, inventory.Item{
				ID: itemID, OwnerUserID: userID, CreatorUserID: userID, FolderID: parentID,
				AssetID: request.LinkedID, AssetType: 25, InventoryType: 8, // AT_LINK_FOLDER / IT_CATEGORY
				Name: request.Name, Description: request.Description,
			})
		default:
			writeLLSDError(w, http.StatusBadRequest, "invalid AIS inventory link request")
			return
		}
		seenSources[request.LinkedID] = true
	}
	created, err = a.inventory.CreateItems(r.Context(), created)
	if writeAISItemError(w, err) {
		return
	}
	versions, err := a.inventoryFolderVersions(r.Context(), userID, parentID)
	if err != nil {
		writeLLSDError(w, http.StatusServiceUnavailable, "AIS inventory folder version could not be loaded")
		return
	}
	writeAISLinkCreation(w, created, versions)
}

func (a *API) slamAISInventoryLinks(w http.ResponseWriter, r *http.Request, userID, folderID string) {
	requests, found, err := decodeAISInventoryLinks(http.MaxBytesReader(w, r.Body, 256*1024))
	if err != nil || !found || len(requests) > 256 {
		writeLLSDError(w, http.StatusBadRequest, "invalid AIS folder links replacement")
		return
	}
	folders, err := a.inventory.ListFolders(r.Context(), userID)
	if err != nil {
		writeLLSDError(w, http.StatusServiceUnavailable, "AIS inventory folder could not be loaded")
		return
	}
	folderFound := false
	folderByID := make(map[string]inventory.Folder, len(folders))
	for _, folder := range folders {
		folderByID[folder.ID] = folder
		if folder.ID == folderID {
			folderFound = true
		}
	}
	if !folderFound {
		writeLLSDError(w, http.StatusNotFound, "AIS inventory folder was not found")
		return
	}
	existing, err := a.inventory.ListItems(r.Context(), userID)
	if err != nil {
		writeLLSDError(w, http.StatusServiceUnavailable, "AIS inventory items could not be loaded")
		return
	}
	sources := make(map[string]inventory.Item, len(existing))
	for _, item := range existing {
		sources[item.ID] = item
	}
	created := make([]inventory.Item, 0, len(requests))
	seenSources := make(map[string]bool, len(requests))
	for _, request := range requests {
		if request.Name == "" || len(request.Name) > 255 ||
			len(request.Description) > 1024 || seenSources[request.LinkedID] {
			writeLLSDError(w, http.StatusBadRequest, "invalid AIS folder links replacement")
			return
		}
		itemID, err := identifier.NewUUID()
		if err != nil {
			writeLLSDError(w, http.StatusServiceUnavailable, "inventory link ID could not be allocated")
			return
		}
		switch request.AssetType {
		case 24: // item link — points at an inventory item
			source, ok := sources[request.LinkedID]
			if !ok {
				writeLLSDError(w, http.StatusNotFound, "AIS inventory link source was not found")
				return
			}
			creatorID := source.CreatorUserID
			if creatorID == "" || creatorID == nullInventoryFolderID {
				creatorID = userID
			}
			created = append(created, inventory.Item{
				ID: itemID, OwnerUserID: userID, CreatorUserID: creatorID, FolderID: folderID,
				AssetID: source.ID, AssetType: 24, InventoryType: source.InventoryType,
				Name: request.Name, Description: request.Description, Flags: source.Flags,
				BasePermissions: source.BasePermissions, CurrentPermissions: source.CurrentPermissions,
				EveryonePermissions: source.EveryonePermissions, NextPermissions: source.NextPermissions,
			})
		case 25: // folder link — the COF "outfit link" points at a folder, not an item
			if _, ok := folderByID[request.LinkedID]; !ok {
				writeLLSDError(w, http.StatusNotFound, "AIS inventory link source folder was not found")
				return
			}
			created = append(created, inventory.Item{
				ID: itemID, OwnerUserID: userID, CreatorUserID: userID, FolderID: folderID,
				AssetID: request.LinkedID, AssetType: 25, InventoryType: 8, // AT_LINK_FOLDER / IT_CATEGORY
				Name: request.Name, Description: request.Description,
			})
		default:
			writeLLSDError(w, http.StatusBadRequest, "invalid AIS folder links replacement")
			return
		}
		seenSources[request.LinkedID] = true
	}
	removed := make([]string, 0, 16)
	for _, item := range existing {
		if item.FolderID != folderID || (item.AssetType != 24 && item.AssetType != 25) {
			continue
		}
		if _, err := a.inventory.DeleteItem(r.Context(), userID, item.ID); err != nil {
			writeLLSDError(w, http.StatusServiceUnavailable, "AIS existing inventory links could not be removed")
			return
		}
		removed = append(removed, item.ID)
	}
	if len(created) > 0 {
		created, err = a.inventory.CreateItems(r.Context(), created)
		if writeAISItemError(w, err) {
			return
		}
	}
	versions, err := a.inventoryFolderVersions(r.Context(), userID, folderID)
	if err != nil {
		writeLLSDError(w, http.StatusServiceUnavailable, "AIS inventory folder version could not be loaded")
		return
	}
	writeAISLinksReplacement(w, created, removed, versions)
}

func writeAISLinksReplacement(w http.ResponseWriter, created []inventory.Item, removed []string,
	versions map[string]int64) {
	var createdIDs, removedIDs, embedded strings.Builder
	for _, item := range created {
		createdIDs.WriteString("<uuid>" + html.EscapeString(item.ID) + "</uuid>")
		embedded.WriteString("<key>" + html.EscapeString(item.ID) + "</key>")
		embedded.WriteString(inventoryAISItemXML(item, true))
	}
	for _, id := range removed {
		removedIDs.WriteString("<uuid>" + html.EscapeString(id) + "</uuid>")
	}
	w.Header().Set("Content-Type", "application/llsd+xml; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	_, _ = io.WriteString(w, "<?xml version=\"1.0\"?><llsd><map>"+
		"<key>_created_items</key><array>"+createdIDs.String()+"</array>"+
		"<key>_removed_items</key><array>"+removedIDs.String()+"</array>"+
		inventoryFolderVersionsXML(versions)+
		"<key>_embedded</key><map><key>categories</key><map></map>"+
		"<key>items</key><map></map><key>links</key><map>"+embedded.String()+
		"</map></map></map></llsd>")
}

func writeAISLinkCreation(w http.ResponseWriter, items []inventory.Item, versions map[string]int64) {
	var created, embedded strings.Builder
	for _, item := range items {
		created.WriteString("<uuid>" + html.EscapeString(item.ID) + "</uuid>")
		embedded.WriteString("<key>" + html.EscapeString(item.ID) + "</key>")
		embedded.WriteString(inventoryAISItemXML(item, true))
	}
	w.Header().Set("Content-Type", "application/llsd+xml; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	_, _ = io.WriteString(w, "<?xml version=\"1.0\"?><llsd><map>"+
		"<key>_created_items</key><array>"+created.String()+"</array>"+
		inventoryFolderVersionsXML(versions)+
		"<key>_embedded</key><map><key>categories</key><map></map>"+
		"<key>items</key><map></map><key>links</key><map>"+embedded.String()+
		"</map></map></map></llsd>")
}

func (a *API) updateAISInventoryFolder(w http.ResponseWriter, r *http.Request, userID, folderID string) {
	request, seen, err := decodeInventoryFolderMutationRequest(http.MaxBytesReader(w, r.Body, 64*1024), "item_id")
	if err != nil || (!seen["name"] && !seen["parent_id"]) || (seen["item_id"] && request.ID != folderID) ||
		(seen["parent_id"] && !validUUID(request.ParentID)) || (seen["type"] && request.Type != -1) {
		writeLLSDError(w, http.StatusBadRequest, "invalid AIS inventory folder update")
		return
	}
	folders, err := a.inventory.ListFolders(r.Context(), userID)
	if err != nil {
		writeLLSDError(w, http.StatusServiceUnavailable, "AIS inventory folder could not be loaded")
		return
	}
	var existing inventory.Folder
	for _, folder := range folders {
		if folder.ID == folderID {
			existing = folder
			break
		}
	}
	if existing.ID == "" {
		writeLLSDError(w, http.StatusNotFound, "AIS inventory folder was not found")
		return
	}
	oldParentID := existing.ParentID
	if seen["name"] {
		existing.Name = request.Name
	}
	if seen["parent_id"] {
		existing.ParentID = request.ParentID
	}
	folder, err := a.inventory.UpdateFolder(r.Context(), inventory.Folder{
		ID: existing.ID, OwnerUserID: userID, ParentID: existing.ParentID,
		Name: existing.Name, TypeDefault: existing.TypeDefault,
	})
	if writeAISFolderError(w, err) {
		return
	}
	versions := map[string]int64{}
	if oldParentID != folder.ParentID {
		versions, err = a.inventoryFolderVersions(r.Context(), userID, oldParentID, folder.ParentID)
		if err != nil {
			writeLLSDError(w, http.StatusServiceUnavailable, "AIS inventory folder versions could not be loaded")
			return
		}
	}
	writeAISFolderUpdate(w, folder, versions)
}

func writeAISFolderError(w http.ResponseWriter, err error) bool {
	if err == nil {
		return false
	}
	switch {
	case errors.Is(err, inventory.ErrInvalidFolder):
		writeLLSDError(w, http.StatusBadRequest, "invalid AIS inventory folder request")
	case errors.Is(err, inventory.ErrFolderNotFound):
		writeLLSDError(w, http.StatusNotFound, "AIS inventory folder was not found")
	case errors.Is(err, inventory.ErrFolderConflict):
		writeLLSDError(w, http.StatusConflict, "AIS inventory folder already exists")
	default:
		writeLLSDError(w, http.StatusServiceUnavailable, "AIS inventory folder operation failed")
	}
	return true
}

func writeAISUUIDArray(w http.ResponseWriter, key, id string) {
	w.Header().Set("Content-Type", "application/llsd+xml; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	_, _ = fmt.Fprintf(w, "<?xml version=\"1.0\"?><llsd><map><key>%s</key>"+
		"<array><uuid>%s</uuid></array></map></llsd>", html.EscapeString(key), html.EscapeString(id))
}

func writeAISFolderUpdate(w http.ResponseWriter, folder inventory.Folder, versions map[string]int64) {
	w.Header().Set("Content-Type", "application/llsd+xml; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	_, _ = fmt.Fprintf(w, "<?xml version=\"1.0\"?><llsd><map>"+
		"<key>_updated_categories</key><array><uuid>%s</uuid></array>"+
		"<key>category_id</key><uuid>%s</uuid><key>parent_id</key><uuid>%s</uuid>"+
		"<key>type_default</key><integer>%d</integer><key>name</key><string>%s</string>"+
		"<key>version</key><integer>%d</integer>%s</map></llsd>",
		html.EscapeString(folder.ID), html.EscapeString(folder.ID),
		html.EscapeString(folder.ParentID), folder.TypeDefault,
		html.EscapeString(folder.Name), folder.Version, inventoryFolderVersionsXML(versions))
}

type aisInventoryItemMutation struct {
	ID          string
	ParentID    string
	Name        string
	Description string
	HashID      string
}

func decodeAISInventoryItemMutation(reader io.Reader) (aisInventoryItemMutation, map[string]bool, error) {
	decoder := xml.NewDecoder(reader)
	var request aisInventoryItemMutation
	seen := make(map[string]bool)
	var key string
	mapDepth := 0
	sawLLSD := false
	for {
		token, err := decoder.Token()
		if errors.Is(err, io.EOF) {
			break
		}
		if err != nil {
			return request, seen, err
		}
		switch value := token.(type) {
		case xml.StartElement:
			switch value.Name.Local {
			case "llsd":
				sawLLSD = true
			case "map":
				mapDepth++
			case "key":
				if mapDepth == 1 {
					if err := decoder.DecodeElement(&key, &value); err != nil {
						return request, seen, err
					}
				}
			default:
				if mapDepth != 1 || key == "" ||
					(value.Name.Local != "uuid" && value.Name.Local != "string") {
					continue
				}
				var field string
				if err := decoder.DecodeElement(&field, &value); err != nil {
					return request, seen, err
				}
				field = strings.TrimSpace(field)
				normalized := key
				if normalized == "desc" {
					normalized = "description"
				}
				if seen[normalized] {
					return request, seen, errors.New("AIS inventory item update repeats a field")
				}
				switch normalized {
				case "item_id":
					request.ID = field
				case "parent_id":
					request.ParentID = field
				case "name":
					request.Name = field
				case "description":
					request.Description = field
				case "hash_id":
					request.HashID = field
				default:
					key = ""
					continue
				}
				seen[normalized] = true
				key = ""
			}
		case xml.EndElement:
			if value.Name.Local == "map" {
				mapDepth--
			}
		}
	}
	if !sawLLSD || (!seen["name"] && !seen["description"] && !seen["parent_id"] && !seen["hash_id"]) {
		return request, seen, errors.New("AIS inventory item update is empty")
	}
	return request, seen, nil
}

func (a *API) updateAISInventoryItem(w http.ResponseWriter, r *http.Request, userID, secureSessionID, itemID string) {
	request, seen, err := decodeAISInventoryItemMutation(http.MaxBytesReader(w, r.Body, 64*1024))
	if err != nil || (seen["item_id"] && request.ID != itemID) ||
		(seen["parent_id"] && !validUUID(request.ParentID)) ||
		(seen["hash_id"] && !validUUID(request.HashID)) {
		writeLLSDError(w, http.StatusBadRequest, "invalid AIS inventory item update")
		return
	}
	items, err := a.inventory.ListItems(r.Context(), userID)
	if err != nil {
		writeLLSDError(w, http.StatusServiceUnavailable, "AIS inventory item could not be loaded")
		return
	}
	var item inventory.Item
	for _, existing := range items {
		if existing.ID == itemID {
			item = existing
			break
		}
	}
	if item.ID == "" {
		writeLLSDError(w, http.StatusNotFound, "AIS inventory item was not found")
		return
	}
	var uploadedAssetID string
	if seen["hash_id"] {
		if a.assets == nil {
			writeLLSDError(w, http.StatusServiceUnavailable, "AIS wearable asset metadata is unavailable")
			return
		}
		var ok bool
		uploadedAssetID, ok = combineViewerAssetID(request.HashID, secureSessionID)
		if !ok {
			writeLLSDError(w, http.StatusBadRequest, "invalid AIS wearable asset transaction")
			return
		}
		var asset assetmeta.Asset
		for attempt := 0; attempt < 50; attempt++ {
			asset, err = a.assets.Get(r.Context(), uploadedAssetID)
			if err == nil || !errors.Is(err, assetmeta.ErrNotFound) {
				break
			}
			select {
			case <-r.Context().Done():
				err = r.Context().Err()
				attempt = 50
			case <-time.After(10 * time.Millisecond):
			}
		}
		if err != nil || asset.CreatorUserID != userID {
			writeLLSDError(w, http.StatusNotFound, "AIS wearable asset was not found")
			return
		}
	}
	existingFolderID := item.FolderID
	if seen["name"] {
		item.Name = request.Name
	}
	if seen["description"] {
		item.Description = request.Description
	}
	if seen["parent_id"] {
		item.FolderID = request.ParentID
	}
	item, err = a.inventory.UpdateItem(r.Context(), item)
	if writeAISItemError(w, err) {
		return
	}
	if uploadedAssetID != "" {
		item, err = a.inventory.UpdateItemAsset(r.Context(), userID, itemID, uploadedAssetID)
		if writeAISItemError(w, err) {
			return
		}
	}
	versions, err := a.inventoryFolderVersions(r.Context(), userID, existingFolderID, item.FolderID)
	if err != nil {
		writeLLSDError(w, http.StatusServiceUnavailable, "AIS inventory folder version could not be loaded")
		return
	}
	w.Header().Set("Content-Type", "application/llsd+xml; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	content := strings.TrimSuffix(inventoryAISItemXML(item, item.AssetType == 24), "</map>")
	_, _ = io.WriteString(w, "<?xml version=\"1.0\"?><llsd>"+content+
		inventoryFolderVersionsXML(versions)+"</map></llsd>")
}

func combineViewerAssetID(transactionID, secureSessionID string) (string, bool) {
	decode := func(value string) ([]byte, bool) {
		value = strings.ReplaceAll(value, "-", "")
		if len(value) != 32 {
			return nil, false
		}
		decoded, err := hex.DecodeString(value)
		return decoded, err == nil && len(decoded) == 16
	}
	transaction, ok := decode(transactionID)
	if !ok {
		return "", false
	}
	secure, ok := decode(secureSessionID)
	if !ok {
		return "", false
	}
	digest := md5.Sum(append(transaction, secure...))
	encoded := hex.EncodeToString(digest[:])
	return encoded[0:8] + "-" + encoded[8:12] + "-" + encoded[12:16] + "-" +
		encoded[16:20] + "-" + encoded[20:32], true
}

func (a *API) deleteAISInventoryItem(w http.ResponseWriter, r *http.Request, userID, itemID string) {
	item, err := a.inventory.DeleteItem(r.Context(), userID, itemID)
	if writeAISItemError(w, err) {
		return
	}
	versions, err := a.inventoryFolderVersions(r.Context(), userID, item.FolderID)
	if err != nil {
		writeLLSDError(w, http.StatusServiceUnavailable, "AIS inventory folder version could not be loaded")
		return
	}
	w.Header().Set("Content-Type", "application/llsd+xml; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	_, _ = io.WriteString(w, "<?xml version=\"1.0\"?><llsd><map>"+
		"<key>_removed_items</key><array><uuid>"+html.EscapeString(itemID)+"</uuid></array>"+
		inventoryFolderVersionsXML(versions)+"</map></llsd>")
}

func (a *API) inventoryFolderVersions(ctx context.Context, userID string, folderIDs ...string) (map[string]int64, error) {
	folders, err := a.inventory.ListFolders(ctx, userID)
	if err != nil {
		return nil, err
	}
	wanted := make(map[string]bool, len(folderIDs))
	for _, id := range folderIDs {
		wanted[id] = true
	}
	versions := make(map[string]int64, len(wanted))
	for _, folder := range folders {
		if wanted[folder.ID] {
			versions[folder.ID] = folder.Version
		}
	}
	if len(versions) != len(wanted) {
		return nil, inventory.ErrFolderNotFound
	}
	return versions, nil
}

func inventoryFolderVersionsXML(versions map[string]int64) string {
	ids := make([]string, 0, len(versions))
	for id := range versions {
		ids = append(ids, id)
	}
	sort.Strings(ids)
	var content strings.Builder
	content.WriteString("<key>_updated_category_versions</key><map>")
	for _, id := range ids {
		content.WriteString("<key>" + html.EscapeString(id) + "</key><integer>" +
			strconv.FormatInt(versions[id], 10) + "</integer>")
	}
	content.WriteString("</map>")
	return content.String()
}

func writeAISItemError(w http.ResponseWriter, err error) bool {
	if err == nil {
		return false
	}
	switch {
	case errors.Is(err, inventory.ErrAssetNotDurable):
		// LLSD, not JSON: this arm answers the viewer's AIS capability, so it
		// keeps that surface's error shape even though the cause is the same
		// ADR 0026 refusal the internal tier reports as asset_not_durable.
		writeLLSDError(w, http.StatusConflict,
			"AIS inventory item asset could not be stored durably")
	case errors.Is(err, inventory.ErrInvalidItem):
		writeLLSDError(w, http.StatusBadRequest, "invalid AIS inventory item update")
	case errors.Is(err, inventory.ErrItemNotFound):
		writeLLSDError(w, http.StatusNotFound, "AIS inventory item was not found")
	case errors.Is(err, inventory.ErrItemFolderNotFound):
		writeLLSDError(w, http.StatusNotFound, "AIS inventory destination folder was not found")
	case errors.Is(err, inventory.ErrItemConflict):
		writeLLSDError(w, http.StatusConflict, "AIS inventory item already exists")
	default:
		writeLLSDError(w, http.StatusServiceUnavailable, "AIS inventory item operation failed")
	}
	return true
}

// lockInventoryMutations serializes one user's inventory mutations, returning
// the release. Keyed per user so unrelated residents never wait on each other.
func (a *API) lockInventoryMutations(userID string) func() {
	value, _ := a.inventoryMutations.LoadOrStore(userID, &sync.Mutex{})
	mutex := value.(*sync.Mutex)
	mutex.Lock()
	return mutex.Unlock
}
