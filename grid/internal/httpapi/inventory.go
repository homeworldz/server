package httpapi

import (
	"errors"
	"net/http"
	"strconv"
	"strings"

	"github.com/homeworldz/server/grid/internal/assetmeta"
	"github.com/homeworldz/server/grid/internal/identifier"
	"github.com/homeworldz/server/grid/internal/inventory"
)

// inventoryStoreError answers a store failure that no caller input explains.
// The response stays generic; the cause goes to the log, the only place an
// operator can see it, keyed by requestId to the request line.
func (a *API) inventoryStoreError(w http.ResponseWriter, r *http.Request, message string, err error) {
	if a.logger != nil {
		a.logger.Error(message, "requestId", requestIDFromContext(r.Context()), "error", err)
	}
	writeJSON(w, http.StatusInternalServerError, Error{Code: "inventory_store_error", Message: message})
}

func (a *API) inventoryByUser(w http.ResponseWriter, r *http.Request) {
	if a.inventory == nil {
		writeJSON(w, http.StatusServiceUnavailable, Error{Code: "inventory_store_unavailable", Message: "inventory storage is unavailable"})
		return
	}
	path := strings.TrimPrefix(r.URL.Path, "/api/v1/inventory/")
	userID, suffix, found := strings.Cut(path, "/")
	if !found || !validUUID(userID) {
		a.notFound(w, r)
		return
	}
	if suffix == "items" {
		a.inventoryItemsByUser(w, r, userID)
		return
	}
	if suffix == "copy-library-item" {
		a.copyLibraryInventoryItem(w, r, userID)
		return
	}
	if suffix == "copy-item" {
		a.copyInventoryItem(w, r, userID)
		return
	}
	if strings.HasPrefix(suffix, "system-folders/") {
		folderTypeText := strings.TrimPrefix(suffix, "system-folders/")
		folderType, err := strconv.Atoi(folderTypeText)
		if err != nil || strings.Contains(folderTypeText, "/") {
			a.notFound(w, r)
			return
		}
		a.inventorySystemFolderByUser(w, r, userID, folderType)
		return
	}
	if strings.HasPrefix(suffix, "folders/") {
		folderPath := strings.TrimPrefix(suffix, "folders/")
		if folderID, listSuffix, found := strings.Cut(folderPath, "/"); found && listSuffix == "items" &&
			validUUID(folderID) {
			a.inventoryFolderItemsByUser(w, r, userID, folderID)
			return
		}
		folderID := folderPath
		if !validUUID(folderID) || strings.Contains(folderID, "/") {
			a.notFound(w, r)
			return
		}
		a.inventoryFolderByUser(w, r, userID, folderID)
		return
	}
	if strings.HasPrefix(suffix, "items/") {
		itemPath := strings.TrimPrefix(suffix, "items/")
		if itemID, assetSuffix, found := strings.Cut(itemPath, "/"); found && assetSuffix == "asset" &&
			validUUID(itemID) {
			a.inventoryItemAssetByUser(w, r, userID, itemID)
			return
		}
		itemID := itemPath
		if !validUUID(itemID) || strings.Contains(itemID, "/") {
			a.notFound(w, r)
			return
		}
		a.inventoryItemByUser(w, r, userID, itemID)
		return
	}
	if suffix != "folders" {
		a.notFound(w, r)
		return
	}
	switch r.Method {
	case http.MethodGet:
		folders, err := a.inventory.ListFolders(r.Context(), userID)
		if err != nil {
			a.inventoryStoreError(w, r, "inventory folders could not be listed", err)
			return
		}
		if folders == nil {
			folders = []inventory.Folder{}
		}
		writeJSON(w, http.StatusOK, InventoryFolderList{Folders: folders})
	case http.MethodPost:
		var request CreateInventoryFolderRequest
		if !decodeJSON(w, r, &request) {
			return
		}
		if !validUUID(request.ID) || !validUUID(request.ParentID) || request.TypeDefault != -1 {
			writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_inventory_folder", Message: "inventory folder is invalid"})
			return
		}
		folder, err := a.inventory.CreateFolder(r.Context(), inventory.Folder{
			ID: request.ID, OwnerUserID: userID, ParentID: request.ParentID,
			Name: request.Name, TypeDefault: request.TypeDefault,
		})
		if errors.Is(err, inventory.ErrInvalidFolder) {
			writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_inventory_folder", Message: "inventory folder is invalid"})
			return
		}
		if errors.Is(err, inventory.ErrFolderNotFound) {
			writeJSON(w, http.StatusNotFound, Error{Code: "inventory_parent_not_found", Message: "inventory parent folder was not found"})
			return
		}
		if errors.Is(err, inventory.ErrFolderConflict) {
			writeJSON(w, http.StatusConflict, Error{Code: "inventory_folder_exists", Message: "inventory folder already exists"})
			return
		}
		if err != nil {
			a.inventoryStoreError(w, r, "inventory folder could not be created", err)
			return
		}
		w.Header().Set("Location", "/api/v1/inventory/"+userID+"/folders/"+folder.ID)
		writeJSON(w, http.StatusCreated, folder)
	default:
		w.Header().Set("Allow", "GET, POST")
		writeJSON(w, http.StatusMethodNotAllowed, Error{Code: "method_not_allowed", Message: "only GET and POST are supported"})
	}
}

type updateInventoryItemAssetRequest struct {
	AssetID string `json:"assetId"`
}

func (a *API) inventoryItemAssetByUser(w http.ResponseWriter, r *http.Request, userID, itemID string) {
	if r.Method != http.MethodPut {
		w.Header().Set("Allow", http.MethodPut)
		writeJSON(w, http.StatusMethodNotAllowed, Error{Code: "method_not_allowed", Message: "only PUT is supported"})
		return
	}
	var request updateInventoryItemAssetRequest
	if !decodeJSON(w, r, &request) || !validUUID(request.AssetID) {
		return
	}
	if a.assets == nil {
		writeJSON(w, http.StatusServiceUnavailable, Error{Code: "asset_store_unavailable", Message: "asset metadata is unavailable"})
		return
	}
	asset, err := a.assets.Get(r.Context(), request.AssetID)
	if errors.Is(err, assetmeta.ErrNotFound) {
		writeJSON(w, http.StatusNotFound, Error{Code: "asset_not_found", Message: "asset was not found"})
		return
	}
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, Error{Code: "asset_store_error", Message: "asset metadata could not be loaded"})
		return
	}
	if asset.CreatorUserID != userID {
		writeJSON(w, http.StatusForbidden, Error{Code: "asset_creator_mismatch", Message: "inventory asset was not created by its uploader"})
		return
	}
	item, err := a.inventory.UpdateItemAsset(r.Context(), userID, itemID, request.AssetID)
	switch {
	case writeDurabilityError(w, err):
	case errors.Is(err, inventory.ErrInvalidItem):
		writeJSON(w, http.StatusForbidden, Error{Code: "inventory_item_not_editable", Message: "inventory item cannot accept an asset update"})
	case errors.Is(err, inventory.ErrItemNotFound):
		writeJSON(w, http.StatusNotFound, Error{Code: "inventory_item_not_found", Message: "inventory item was not found"})
	case err != nil:
		a.inventoryStoreError(w, r, "inventory item asset could not be updated", err)
	default:
		writeJSON(w, http.StatusOK, item)
	}
}

func (a *API) inventorySystemFolderByUser(w http.ResponseWriter, r *http.Request, userID string, folderType int) {
	if r.Method != http.MethodGet {
		w.Header().Set("Allow", http.MethodGet)
		writeJSON(w, http.StatusMethodNotAllowed, Error{Code: "method_not_allowed", Message: "only GET is supported"})
		return
	}
	// Ensured on read, deliberately: this internal endpoint exists so a
	// region can place an item, and every caller wants the folder to exist.
	// Viewer logins ensure the same set; a session client's first upload must
	// not depend on the user having once logged in with a viewer. Idempotent —
	// the folder ids are deterministic per user.
	folders, err := a.inventory.EnsureSystemFolders(r.Context(), userID)
	if err != nil {
		a.inventoryStoreError(w, r, "inventory folder could not be loaded", err)
		return
	}
	for _, folder := range folders {
		if folder.TypeDefault == folderType && folder.ID == inventory.SystemFolderID(userID, folderType) {
			writeJSON(w, http.StatusOK, folder)
			return
		}
	}
	writeJSON(w, http.StatusNotFound, Error{Code: "inventory_system_folder_not_found", Message: "inventory system folder was not found"})
}

func (a *API) inventoryItemByUser(w http.ResponseWriter, r *http.Request, userID, itemID string) {
	if r.Method != http.MethodGet && r.Method != http.MethodPut {
		w.Header().Set("Allow", "GET, PUT")
		writeJSON(w, http.StatusMethodNotAllowed, Error{Code: "method_not_allowed", Message: "only GET and PUT are supported"})
		return
	}
	items, err := a.inventory.ListItems(r.Context(), userID)
	if err != nil {
		a.inventoryStoreError(w, r, "inventory item could not be loaded", err)
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
		writeJSON(w, http.StatusNotFound, Error{Code: "inventory_item_not_found", Message: "inventory item was not found"})
		return
	}
	if r.Method == http.MethodGet {
		writeJSON(w, http.StatusOK, item)
		return
	}
	var request MoveInventoryItemRequest
	if !decodeJSON(w, r, &request) {
		return
	}
	if !validUUID(request.FolderID) || len(request.Name) > 255 {
		writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_inventory_item_move", Message: "inventory item move is invalid"})
		return
	}
	item.FolderID = request.FolderID
	if request.Name != "" {
		item.Name = request.Name
	}
	item, err = a.inventory.UpdateItem(r.Context(), item)
	switch {
	case writeDurabilityError(w, err):
	case errors.Is(err, inventory.ErrInvalidItem):
		writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_inventory_item_move", Message: "inventory item move is invalid"})
	case errors.Is(err, inventory.ErrItemFolderNotFound):
		writeJSON(w, http.StatusNotFound, Error{Code: "inventory_folder_not_found", Message: "inventory destination folder was not found"})
	case errors.Is(err, inventory.ErrItemNotFound):
		writeJSON(w, http.StatusNotFound, Error{Code: "inventory_item_not_found", Message: "inventory item was not found"})
	case err != nil:
		a.inventoryStoreError(w, r, "inventory item could not be moved", err)
	default:
		writeJSON(w, http.StatusOK, item)
	}
}

// One item of a folder's contents. A link (asset type 24) carries the id of
// the item it names in its assetId and no asset of its own, so a caller that
// only saw the link would have to fetch each target separately — and a region
// fetching one at a time blocks its own HTTP thread on every hop. The target
// travels with the link instead, resolved from the same read.
type InventoryFolderItem struct {
	inventory.Item
	LinkedItem *inventory.Item `json:"linkedItem,omitempty"`
}

// The contents of one folder. This exists for the Current Outfit folder: a
// region baking a wearer's appearance needs to know what that wearer has on,
// which is the COF's links and what they point at.
//
// A folder that holds nothing answers 200 with an empty array. A folder that
// does not exist answers 404 — the two are different facts, and a caller that
// cannot tell them apart will read a mistyped id as a naked avatar.
func (a *API) inventoryFolderItemsByUser(w http.ResponseWriter, r *http.Request, userID, folderID string) {
	if r.Method != http.MethodGet {
		w.Header().Set("Allow", http.MethodGet)
		writeJSON(w, http.StatusMethodNotAllowed, Error{Code: "method_not_allowed", Message: "only GET is supported"})
		return
	}
	folders, err := a.inventory.ListFolders(r.Context(), userID)
	if err != nil {
		a.inventoryStoreError(w, r, "inventory folder could not be loaded", err)
		return
	}
	found := false
	for _, folder := range folders {
		if folder.ID == folderID {
			found = true
			break
		}
	}
	if !found {
		writeJSON(w, http.StatusNotFound, Error{Code: "inventory_folder_not_found", Message: "inventory folder was not found"})
		return
	}
	items, err := a.inventory.ListItems(r.Context(), userID)
	if err != nil {
		a.inventoryStoreError(w, r, "inventory items could not be loaded", err)
		return
	}
	byID := make(map[string]inventory.Item, len(items))
	for _, item := range items {
		byID[item.ID] = item
	}
	contents := []InventoryFolderItem{}
	for _, item := range items {
		if item.FolderID != folderID {
			continue
		}
		entry := InventoryFolderItem{Item: item}
		// A link whose target is gone stays in the response as a link with
		// nothing attached. Dropping it would hide a broken outfit from the
		// only caller positioned to report one.
		if item.AssetType == 24 {
			if target, ok := byID[item.AssetID]; ok {
				entry.LinkedItem = &target
			}
		}
		contents = append(contents, entry)
	}
	writeJSON(w, http.StatusOK, contents)
}

func (a *API) inventoryFolderByUser(w http.ResponseWriter, r *http.Request, userID, folderID string) {
	if r.Method != http.MethodPut {
		w.Header().Set("Allow", http.MethodPut)
		writeJSON(w, http.StatusMethodNotAllowed, Error{Code: "method_not_allowed", Message: "only PUT is supported"})
		return
	}
	var request MoveInventoryFolderRequest
	if !decodeJSON(w, r, &request) {
		return
	}
	if !validUUID(request.ParentID) {
		writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_inventory_folder_move", Message: "inventory folder destination is invalid"})
		return
	}
	folders, err := a.inventory.ListFolders(r.Context(), userID)
	if err != nil {
		a.inventoryStoreError(w, r, "inventory folder could not be loaded", err)
		return
	}
	var folder inventory.Folder
	for _, existing := range folders {
		if existing.ID == folderID {
			folder = existing
			break
		}
	}
	if folder.ID == "" {
		writeJSON(w, http.StatusNotFound, Error{Code: "inventory_folder_not_found", Message: "inventory folder was not found"})
		return
	}
	folder.ParentID = request.ParentID
	folder, err = a.inventory.UpdateFolder(r.Context(), folder)
	switch {
	case errors.Is(err, inventory.ErrInvalidFolder):
		writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_inventory_folder_move", Message: "inventory folder move is invalid"})
	case errors.Is(err, inventory.ErrFolderNotFound):
		writeJSON(w, http.StatusNotFound, Error{Code: "inventory_folder_not_found", Message: "inventory folder or destination was not found"})
	case err != nil:
		a.inventoryStoreError(w, r, "inventory folder could not be moved", err)
	default:
		writeJSON(w, http.StatusOK, folder)
	}
}

func (a *API) copyLibraryInventoryItem(w http.ResponseWriter, r *http.Request, userID string) {
	if r.Method != http.MethodPost {
		w.Header().Set("Allow", http.MethodPost)
		writeJSON(w, http.StatusMethodNotAllowed, Error{Code: "method_not_allowed", Message: "only POST is supported"})
		return
	}
	var request CopyLibraryInventoryItemRequest
	if !decodeJSON(w, r, &request) {
		return
	}
	if !validUUID(request.SourceItemID) || !validUUID(request.DestinationFolderID) || len(request.Name) > 255 {
		writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_library_copy", Message: "library inventory copy is invalid"})
		return
	}
	var source inventory.Item
	for _, item := range inventory.LibraryItems() {
		if item.ID == request.SourceItemID {
			source = item
			break
		}
	}
	if source.ID == "" {
		writeJSON(w, http.StatusNotFound, Error{Code: "library_item_not_found", Message: "library inventory item was not found"})
		return
	}
	destinationID := request.DestinationFolderID
	if destinationID == "00000000-0000-0000-0000-000000000000" {
		switch source.AssetType {
		case 0:
			destinationID = inventory.SystemFolderID(userID, 0)
		case 5:
			destinationID = inventory.SystemFolderID(userID, 5)
		case 13:
			destinationID = inventory.SystemFolderID(userID, 13)
		default:
			writeJSON(w, http.StatusBadRequest, Error{Code: "unsupported_library_copy", Message: "this library item type cannot yet be copied automatically"})
			return
		}
	}
	itemID, err := identifier.NewUUID()
	if err != nil {
		writeJSON(w, http.StatusServiceUnavailable, Error{Code: "inventory_id_unavailable", Message: "inventory item ID could not be allocated"})
		return
	}
	name := request.Name
	if name == "" {
		name = source.Name
	}
	item, err := a.inventory.CreateItem(r.Context(), inventory.Item{
		ID: itemID, OwnerUserID: userID, CreatorUserID: source.CreatorUserID,
		FolderID: destinationID, AssetID: source.AssetID, AssetType: source.AssetType,
		InventoryType: source.InventoryType, Name: name, Description: source.Description,
		Flags: source.Flags, BasePermissions: source.BasePermissions,
		CurrentPermissions: source.CurrentPermissions, EveryonePermissions: source.EveryonePermissions,
		NextPermissions: source.NextPermissions, SaleType: source.SaleType, SalePrice: source.SalePrice,
	})
	switch {
	case writeDurabilityError(w, err):
	case errors.Is(err, inventory.ErrItemFolderNotFound):
		writeJSON(w, http.StatusNotFound, Error{Code: "inventory_folder_not_found", Message: "inventory destination folder was not found"})
	case errors.Is(err, inventory.ErrInvalidItem):
		writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_library_copy", Message: "library inventory copy is invalid"})
	case err != nil:
		a.inventoryStoreError(w, r, "library inventory item could not be copied", err)
	default:
		w.Header().Set("Location", "/api/v1/inventory/"+userID+"/items/"+item.ID)
		writeJSON(w, http.StatusCreated, item)
	}
}

func (a *API) copyInventoryItem(w http.ResponseWriter, r *http.Request, userID string) {
	if r.Method != http.MethodPost {
		w.Header().Set("Allow", http.MethodPost)
		writeJSON(w, http.StatusMethodNotAllowed, Error{Code: "method_not_allowed", Message: "only POST is supported"})
		return
	}
	var request CopyInventoryItemRequest
	if !decodeJSON(w, r, &request) {
		return
	}
	if !validUUID(request.SourceItemID) || !validUUID(request.DestinationFolderID) || len(request.Name) > 255 {
		writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_inventory_copy", Message: "inventory item copy is invalid"})
		return
	}
	items, err := a.inventory.ListItems(r.Context(), userID)
	if err != nil {
		a.inventoryStoreError(w, r, "inventory item could not be loaded", err)
		return
	}
	var source inventory.Item
	for _, item := range items {
		if item.ID == request.SourceItemID {
			source = item
			break
		}
	}
	if source.ID == "" {
		writeJSON(w, http.StatusNotFound, Error{Code: "inventory_item_not_found", Message: "inventory item was not found"})
		return
	}
	const permissionCopy uint32 = 0x00008000
	if source.CurrentPermissions&permissionCopy == 0 {
		writeJSON(w, http.StatusForbidden, Error{Code: "inventory_item_not_copyable", Message: "inventory item does not grant copy permission"})
		return
	}
	destinationID := request.DestinationFolderID
	if destinationID == "00000000-0000-0000-0000-000000000000" {
		destinationID = source.FolderID
	}
	itemID, err := identifier.NewUUID()
	if err != nil {
		writeJSON(w, http.StatusServiceUnavailable, Error{Code: "inventory_id_unavailable", Message: "inventory item ID could not be allocated"})
		return
	}
	name := request.Name
	if name == "" {
		name = source.Name
	}
	created, err := a.inventory.CreateItem(r.Context(), inventory.Item{
		ID: itemID, OwnerUserID: userID, CreatorUserID: source.CreatorUserID,
		FolderID: destinationID, AssetID: source.AssetID, AssetType: source.AssetType,
		InventoryType: source.InventoryType, Name: name, Description: source.Description,
		Flags: source.Flags, BasePermissions: source.BasePermissions,
		CurrentPermissions: source.CurrentPermissions, EveryonePermissions: source.EveryonePermissions,
		NextPermissions: source.NextPermissions, SaleType: source.SaleType, SalePrice: source.SalePrice,
	})
	switch {
	case writeDurabilityError(w, err):
	case errors.Is(err, inventory.ErrItemFolderNotFound):
		writeJSON(w, http.StatusNotFound, Error{Code: "inventory_folder_not_found", Message: "inventory destination folder was not found"})
	case errors.Is(err, inventory.ErrInvalidItem):
		writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_inventory_copy", Message: "inventory item copy is invalid"})
	case err != nil:
		a.inventoryStoreError(w, r, "inventory item could not be copied", err)
	default:
		w.Header().Set("Location", "/api/v1/inventory/"+userID+"/items/"+created.ID)
		writeJSON(w, http.StatusCreated, created)
	}
}

func (a *API) inventoryItemsByUser(w http.ResponseWriter, r *http.Request, userID string) {
	if r.Method != http.MethodPost {
		w.Header().Set("Allow", http.MethodPost)
		writeJSON(w, http.StatusMethodNotAllowed, Error{Code: "method_not_allowed", Message: "only POST is supported"})
		return
	}
	var request CreateInventoryItemRequest
	if !decodeJSON(w, r, &request) {
		return
	}
	validType := (request.AssetType == 0 && request.InventoryType == 0) ||
		(request.AssetType == 0 && request.InventoryType == 15) ||
		(request.AssetType == 1 && request.InventoryType == 1) ||
		// Calling card. Its asset_id is the avatar it names rather than an
		// asset, so nothing backs it in the vault — see referencesBytes in
		// inventory/durability.go, which excludes it for the same reason.
		// Firestorm creates the agent's own card on any login where
		// Friends/All does not already hold one, so refusing the pair refused
		// every first login.
		(request.AssetType == 2 && request.InventoryType == 2) ||
		(request.AssetType == 3 && request.InventoryType == 3) ||
		(request.AssetType == 7 && request.InventoryType == 7) ||
		(request.AssetType == 10 && request.InventoryType == 10) ||
		(request.AssetType == 20 && request.InventoryType == 19) ||
		(request.AssetType == 21 && request.InventoryType == 20) ||
		(request.AssetType == 6 && request.InventoryType == 6) ||
		// Mesh (ADR 0033): asset type 49, inventory type 22, the pair viewers
		// use for mesh items.
		(request.AssetType == 49 && request.InventoryType == 22) ||
		((request.AssetType == 5 || request.AssetType == 13) && request.InventoryType == 18)
	if !validUUID(request.ID) || !validUUID(request.CreatorUserID) ||
		!validUUID(request.FolderID) ||
		!validUUID(request.AssetID) || !validType {
		writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_inventory_item", Message: "inventory item is invalid"})
		return
	}
	item, err := a.inventory.CreateItem(r.Context(), inventory.Item{
		ID: request.ID, OwnerUserID: userID, CreatorUserID: request.CreatorUserID,
		FolderID: request.FolderID, AssetID: request.AssetID, AssetType: request.AssetType,
		InventoryType: request.InventoryType, Name: request.Name, Description: request.Description,
		Flags:           request.Flags,
		BasePermissions: request.BasePermissions, CurrentPermissions: request.CurrentPermissions,
		EveryonePermissions: request.EveryonePermissions, NextPermissions: request.NextPermissions,
	})
	switch {
	case writeDurabilityError(w, err):
	case errors.Is(err, inventory.ErrInvalidItem):
		writeJSON(w, http.StatusBadRequest, Error{Code: "invalid_inventory_item", Message: "inventory item is invalid"})
	case errors.Is(err, inventory.ErrItemFolderNotFound):
		writeJSON(w, http.StatusNotFound, Error{Code: "inventory_folder_not_found", Message: "inventory item folder was not found"})
	case errors.Is(err, inventory.ErrItemConflict):
		writeJSON(w, http.StatusConflict, Error{Code: "inventory_item_exists", Message: "inventory item already exists"})
	case err != nil:
		a.inventoryStoreError(w, r, "inventory item could not be created", err)
	default:
		w.Header().Set("Location", "/api/v1/inventory/"+userID+"/items/"+item.ID)
		writeJSON(w, http.StatusCreated, item)
	}
}
