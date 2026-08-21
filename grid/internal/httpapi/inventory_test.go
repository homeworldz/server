package httpapi

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/homeworldz/server/grid/internal/assetmeta"
	"github.com/homeworldz/server/grid/internal/inventory"
)

type memoryInventoryStore struct {
	folders map[string][]inventory.Folder
	items   map[string][]inventory.Item
}

func (s *memoryInventoryStore) EnsureSystemFolders(_ context.Context, userID string) ([]inventory.Folder, error) {
	values := inventory.SystemFolders(userID)
	s.folders[userID] = values
	return values, nil
}

func (s *memoryInventoryStore) ListFolders(_ context.Context, userID string) ([]inventory.Folder, error) {
	return s.folders[userID], nil
}

func (s *memoryInventoryStore) CreateFolder(_ context.Context, folder inventory.Folder) (inventory.Folder, error) {
	if folder.TypeDefault != -1 && folder.TypeDefault != 47 {
		return inventory.Folder{}, inventory.ErrInvalidFolder
	}
	for _, existing := range s.folders[folder.OwnerUserID] {
		if existing.ID == folder.ID {
			return inventory.Folder{}, inventory.ErrFolderConflict
		}
	}
	parentFound := false
	for index := range s.folders[folder.OwnerUserID] {
		if s.folders[folder.OwnerUserID][index].ID == folder.ParentID {
			if folder.TypeDefault == 47 && s.folders[folder.OwnerUserID][index].TypeDefault != 48 {
				return inventory.Folder{}, inventory.ErrInvalidFolder
			}
			s.folders[folder.OwnerUserID][index].Version++
			parentFound = true
		}
	}
	if !parentFound {
		return inventory.Folder{}, inventory.ErrFolderNotFound
	}
	folder.Version = 1
	s.folders[folder.OwnerUserID] = append(s.folders[folder.OwnerUserID], folder)
	return folder, nil
}

func (s *memoryInventoryStore) UpdateFolder(_ context.Context, folder inventory.Folder) (inventory.Folder, error) {
	for index, existing := range s.folders[folder.OwnerUserID] {
		if existing.ID != folder.ID {
			continue
		}
		if existing.TypeDefault != folder.TypeDefault ||
			(existing.TypeDefault != -1 && existing.TypeDefault != 47) || folder.ParentID == folder.ID {
			return inventory.Folder{}, inventory.ErrInvalidFolder
		}
		destinationFound := existing.ParentID == folder.ParentID
		if !destinationFound {
			for _, candidate := range s.folders[folder.OwnerUserID] {
				if candidate.ID == folder.ParentID {
					if folder.TypeDefault == 47 && candidate.TypeDefault != 48 && candidate.TypeDefault != 14 {
						return inventory.Folder{}, inventory.ErrInvalidFolder
					}
					destinationFound = true
					break
				}
			}
		}
		if !destinationFound {
			return inventory.Folder{}, inventory.ErrFolderNotFound
		}
		for parent := folder.ParentID; parent != "00000000-0000-0000-0000-000000000000"; {
			if parent == folder.ID {
				return inventory.Folder{}, inventory.ErrInvalidFolder
			}
			next := "00000000-0000-0000-0000-000000000000"
			for _, candidate := range s.folders[folder.OwnerUserID] {
				if candidate.ID == parent {
					next = candidate.ParentID
					break
				}
			}
			parent = next
		}
		folder.Version = existing.Version + 1
		s.folders[folder.OwnerUserID][index] = folder
		if existing.ParentID != folder.ParentID {
			for parentIndex := range s.folders[folder.OwnerUserID] {
				parent := &s.folders[folder.OwnerUserID][parentIndex]
				if parent.ID == existing.ParentID || parent.ID == folder.ParentID {
					parent.Version++
				}
			}
		}
		return folder, nil
	}
	return inventory.Folder{}, inventory.ErrFolderNotFound
}

func (s *memoryInventoryStore) EnsureItem(_ context.Context, item inventory.Item) (bool, error) {
	if s.items == nil {
		s.items = make(map[string][]inventory.Item)
	}
	for index, existing := range s.items[item.OwnerUserID] {
		if existing.ID == item.ID {
			if existing == item {
				return false, nil
			}
			s.items[item.OwnerUserID][index] = item
			return true, nil
		}
	}
	s.items[item.OwnerUserID] = append(s.items[item.OwnerUserID], item)
	return true, nil
}

func (s *memoryInventoryStore) CreateItem(_ context.Context, item inventory.Item) (inventory.Item, error) {
	if s.items == nil {
		s.items = make(map[string][]inventory.Item)
	}
	folderFound := false
	for index := range s.folders[item.OwnerUserID] {
		if s.folders[item.OwnerUserID][index].ID == item.FolderID {
			s.folders[item.OwnerUserID][index].Version++
			folderFound = true
		}
	}
	if !folderFound {
		return inventory.Item{}, inventory.ErrItemFolderNotFound
	}
	for _, existing := range s.items[item.OwnerUserID] {
		if existing.ID == item.ID {
			return inventory.Item{}, inventory.ErrItemConflict
		}
	}
	s.items[item.OwnerUserID] = append(s.items[item.OwnerUserID], item)
	return item, nil
}

func (s *memoryInventoryStore) CreateItems(_ context.Context, items []inventory.Item) ([]inventory.Item, inventory.FolderVersions, error) {
	if len(items) == 0 || len(items) > 256 {
		return nil, nil, inventory.ErrInvalidItem
	}
	ownerID, folderID := items[0].OwnerUserID, items[0].FolderID
	folderIndex := -1
	for index, folder := range s.folders[ownerID] {
		if folder.ID == folderID {
			folderIndex = index
			break
		}
	}
	if folderIndex < 0 {
		return nil, nil, inventory.ErrItemFolderNotFound
	}
	seen := make(map[string]bool, len(items))
	for _, item := range items {
		if item.OwnerUserID != ownerID || item.FolderID != folderID || item.ID == "" ||
			item.CreatorUserID == "" || item.AssetID == "" || item.Name == "" || seen[item.ID] {
			return nil, nil, inventory.ErrInvalidItem
		}
		seen[item.ID] = true
		for _, existing := range s.items[item.OwnerUserID] {
			if existing.ID == item.ID {
				return nil, nil, inventory.ErrItemConflict
			}
		}
	}
	s.items[ownerID] = append(s.items[ownerID], items...)
	s.folders[ownerID][folderIndex].Version += int64(len(items))
	return items, inventory.FolderVersions{folderID: s.folders[ownerID][folderIndex].Version}, nil
}

func (s *memoryInventoryStore) UpdateItem(_ context.Context, item inventory.Item) (inventory.Item, error) {
	for itemIndex, existing := range s.items[item.OwnerUserID] {
		if existing.ID != item.ID {
			continue
		}
		destinationFound := false
		for folderIndex := range s.folders[item.OwnerUserID] {
			if s.folders[item.OwnerUserID][folderIndex].ID == item.FolderID {
				destinationFound = true
				break
			}
		}
		if !destinationFound {
			return inventory.Item{}, inventory.ErrItemFolderNotFound
		}
		for folderIndex := range s.folders[item.OwnerUserID] {
			folder := &s.folders[item.OwnerUserID][folderIndex]
			if folder.ID == existing.FolderID || (existing.FolderID != item.FolderID && folder.ID == item.FolderID) {
				folder.Version++
			}
		}
		item.CreatorUserID = existing.CreatorUserID
		item.AssetID = existing.AssetID
		item.AssetType = existing.AssetType
		item.InventoryType = existing.InventoryType
		item.CreatedAt = existing.CreatedAt
		s.items[item.OwnerUserID][itemIndex] = item
		return item, nil
	}
	return inventory.Item{}, inventory.ErrItemNotFound
}

func (s *memoryInventoryStore) UpdateItemAsset(_ context.Context, ownerID, itemID, assetID string) (inventory.Item, error) {
	for index, item := range s.items[ownerID] {
		if item.ID != itemID {
			continue
		}
		if (item.AssetType != 5 && item.AssetType != 13) || item.InventoryType != 18 ||
			item.CurrentPermissions&0x00004000 == 0 {
			return inventory.Item{}, inventory.ErrInvalidItem
		}
		s.items[ownerID][index].AssetID = assetID
		for folderIndex := range s.folders[ownerID] {
			if s.folders[ownerID][folderIndex].ID == item.FolderID {
				s.folders[ownerID][folderIndex].Version++
			}
		}
		return s.items[ownerID][index], nil
	}
	return inventory.Item{}, inventory.ErrItemNotFound
}

func (s *memoryInventoryStore) DeleteItem(_ context.Context, userID, itemID string) (inventory.Item, error) {
	for itemIndex, item := range s.items[userID] {
		if item.ID != itemID {
			continue
		}
		s.items[userID] = append(s.items[userID][:itemIndex], s.items[userID][itemIndex+1:]...)
		for folderIndex := range s.folders[userID] {
			if s.folders[userID][folderIndex].ID == item.FolderID {
				s.folders[userID][folderIndex].Version++
			}
		}
		return item, nil
	}
	return inventory.Item{}, inventory.ErrItemNotFound
}

func (s *memoryInventoryStore) ListItems(_ context.Context, userID string) ([]inventory.Item, error) {
	return s.items[userID], nil
}

func (s *memoryInventoryStore) PurgeFolder(_ context.Context, userID, folderID string) ([]string, []string, inventory.Folder, error) {
	folderIndex := -1
	for index, folder := range s.folders[userID] {
		if folder.ID == folderID {
			folderIndex = index
			break
		}
	}
	if folderIndex < 0 {
		return nil, nil, inventory.Folder{}, inventory.ErrFolderNotFound
	}
	removedFolders := make(map[string]bool)
	for changed := true; changed; {
		changed = false
		for _, folder := range s.folders[userID] {
			if (folder.ParentID == folderID || removedFolders[folder.ParentID]) && !removedFolders[folder.ID] {
				removedFolders[folder.ID] = true
				changed = true
			}
		}
	}
	folderIDs := make([]string, 0, len(removedFolders))
	keptFolders := s.folders[userID][:0]
	directCount := int64(0)
	for _, folder := range s.folders[userID] {
		if removedFolders[folder.ID] {
			folderIDs = append(folderIDs, folder.ID)
			if folder.ParentID == folderID {
				directCount++
			}
			continue
		}
		keptFolders = append(keptFolders, folder)
	}
	s.folders[userID] = keptFolders
	itemIDs := make([]string, 0, 16)
	keptItems := s.items[userID][:0]
	for _, item := range s.items[userID] {
		if item.FolderID == folderID || removedFolders[item.FolderID] {
			itemIDs = append(itemIDs, item.ID)
			if item.FolderID == folderID {
				directCount++
			}
			continue
		}
		keptItems = append(keptItems, item)
	}
	s.items[userID] = keptItems
	var result inventory.Folder
	for index := range s.folders[userID] {
		if s.folders[userID][index].ID == folderID {
			s.folders[userID][index].Version += directCount
			result = s.folders[userID][index]
			break
		}
	}
	return folderIDs, itemIDs, result, nil
}

func TestInventoryFoldersEndpoint(t *testing.T) {
	const userID = "20000000-0000-4000-8000-000000000001"
	store := &memoryInventoryStore{folders: make(map[string][]inventory.Folder)}
	_, _ = store.EnsureSystemFolders(context.Background(), userID)
	handler := New(checker{}, "test", Options{ServiceToken: "secret", Inventory: store})

	result := requestRegion[InventoryFolderList](t, handler, http.MethodGet,
		"/api/v1/inventory/"+userID+"/folders", "", http.StatusOK)
	if len(result.Folders) != 23 || result.Folders[0].TypeDefault != 8 {
		t.Fatalf("inventory folders = %#v", result.Folders)
	}
	objects := requestRegion[inventory.Folder](t, handler, http.MethodGet,
		"/api/v1/inventory/"+userID+"/system-folders/6", "", http.StatusOK)
	if objects.ID != inventory.SystemFolderID(userID, 6) || objects.TypeDefault != 6 || objects.Name != "Objects" {
		t.Fatalf("objects system folder = %#v", objects)
	}
	requestRegion[Error](t, handler, http.MethodGet,
		"/api/v1/inventory/"+userID+"/system-folders/99", "", http.StatusNotFound)
	requestRegion[Error](t, handler, http.MethodGet,
		"/api/v1/inventory/not-a-uuid/folders", "", http.StatusNotFound)
	const folderID = "40000000-0000-4000-8000-000000000001"
	created := requestRegion[inventory.Folder](t, handler, http.MethodPost,
		"/api/v1/inventory/"+userID+"/folders",
		`{"id":"`+folderID+`","parentId":"`+inventory.SystemFolderID(userID, 8)+`","name":"Projects","typeDefault":-1}`,
		http.StatusCreated)
	if created.ID != folderID || created.Name != "Projects" || created.Version != 1 {
		t.Fatalf("created inventory folder = %#v", created)
	}
	requestRegion[Error](t, handler, http.MethodPost,
		"/api/v1/inventory/"+userID+"/folders",
		`{"id":"`+folderID+`","parentId":"`+inventory.SystemFolderID(userID, 8)+`","name":"Projects","typeDefault":-1}`,
		http.StatusConflict)
	moved := requestRegion[inventory.Folder](t, handler, http.MethodPut,
		"/api/v1/inventory/"+userID+"/folders/"+folderID,
		`{"parentId":"`+inventory.SystemFolderID(userID, 14)+`"}`,
		http.StatusOK)
	if moved.ParentID != inventory.SystemFolderID(userID, 14) || moved.Name != "Projects" {
		t.Fatalf("moved inventory folder = %#v", moved)
	}
}

// A region baking a wearer's appearance reads the Current Outfit folder, and
// what it finds there is links. The link carries the id of the item it names,
// never the asset, so the endpoint has to resolve one hop or the region is left
// making a grid call per worn item on the thread it serves HTTP from.
func TestInventoryFolderItemsResolveLinksForTheCurrentOutfit(t *testing.T) {
	const userID = "20000000-0000-4000-8000-000000000001"
	const shirtItemID = "40000000-0000-4000-8000-000000000031"
	const shirtAssetID = "50000000-0000-4000-8000-000000000031"
	const linkItemID = "40000000-0000-4000-8000-000000000032"
	store := &memoryInventoryStore{folders: make(map[string][]inventory.Folder)}
	_, _ = store.EnsureSystemFolders(context.Background(), userID)
	clothingID := inventory.SystemFolderID(userID, 5)
	currentOutfitID := inventory.SystemFolderID(userID, 46)
	if _, err := store.CreateItem(context.Background(), inventory.Item{
		ID: shirtItemID, OwnerUserID: userID, CreatorUserID: userID, FolderID: clothingID,
		AssetID: shirtAssetID, AssetType: 5, InventoryType: 18, Name: "Shirt",
	}); err != nil {
		t.Fatalf("create shirt: %v", err)
	}
	// The link's assetId is the item it names, and its own assetType is 24.
	if _, err := store.CreateItem(context.Background(), inventory.Item{
		ID: linkItemID, OwnerUserID: userID, CreatorUserID: userID, FolderID: currentOutfitID,
		AssetID: shirtItemID, AssetType: 24, InventoryType: 18, Name: "Shirt",
	}); err != nil {
		t.Fatalf("create outfit link: %v", err)
	}
	handler := New(checker{}, "test", Options{ServiceToken: "secret", Inventory: store})

	contents := requestRegion[[]InventoryFolderItem](t, handler, http.MethodGet,
		"/api/v1/inventory/"+userID+"/folders/"+currentOutfitID+"/items", "", http.StatusOK)
	if len(contents) != 1 {
		t.Fatalf("current outfit contents = %#v", contents)
	}
	if contents[0].ID != linkItemID || contents[0].AssetType != 24 {
		t.Fatalf("outfit entry is not the link: %#v", contents[0])
	}
	if contents[0].LinkedItem == nil {
		t.Fatal("the link was not resolved; a region cannot bake what it cannot name")
	}
	if contents[0].LinkedItem.AssetID != shirtAssetID || contents[0].LinkedItem.AssetType != 5 {
		t.Fatalf("resolved item = %#v", contents[0].LinkedItem)
	}

	// A folder that holds nothing is not a folder that is missing.
	empty := requestRegion[[]InventoryFolderItem](t, handler, http.MethodGet,
		"/api/v1/inventory/"+userID+"/folders/"+inventory.SystemFolderID(userID, 20)+"/items",
		"", http.StatusOK)
	if len(empty) != 0 {
		t.Fatalf("empty folder contents = %#v", empty)
	}
	requestRegion[Error](t, handler, http.MethodGet,
		"/api/v1/inventory/"+userID+"/folders/40000000-0000-4000-8000-0000000000ff/items",
		"", http.StatusNotFound)
	requestRegion[Error](t, handler, http.MethodPost,
		"/api/v1/inventory/"+userID+"/folders/"+currentOutfitID+"/items", "", http.StatusMethodNotAllowed)
}

func TestCreateTextureInventoryItemEndpoint(t *testing.T) {
	const userID = "20000000-0000-4000-8000-000000000001"
	store := &memoryInventoryStore{folders: make(map[string][]inventory.Folder)}
	_, _ = store.EnsureSystemFolders(context.Background(), userID)
	handler := New(checker{}, "test", Options{ServiceToken: "secret", Inventory: store})
	folderID := inventory.SystemFolderID(userID, 0)
	body := `{"id":"40000000-0000-4000-8000-000000000010",` +
		`"creatorUserId":"` + userID + `","folderId":"` + folderID + `",` +
		`"assetId":"50000000-0000-4000-8000-000000000010","assetType":0,` +
		`"inventoryType":0,"name":"Uploaded Terrain","description":"Library source",` +
		`"basePermissions":647168,"currentPermissions":647168,` +
		`"everyonePermissions":0,"nextPermissions":2147483647}`
	created := requestRegion[inventory.Item](t, handler, http.MethodPost,
		"/api/v1/inventory/"+userID+"/items", body, http.StatusCreated)
	if created.OwnerUserID != userID || created.CreatorUserID != userID ||
		created.FolderID != folderID || created.AssetType != 0 || created.InventoryType != 0 ||
		created.BasePermissions != 0x0009e000 || created.CurrentPermissions != 0x0009e000 {
		t.Fatalf("created inventory item = %#v", created)
	}
	requestRegion[Error](t, handler, http.MethodPost,
		"/api/v1/inventory/"+userID+"/items", body, http.StatusConflict)
	moved := requestRegion[inventory.Item](t, handler, http.MethodPut,
		"/api/v1/inventory/"+userID+"/items/"+created.ID,
		`{"folderId":"`+inventory.SystemFolderID(userID, 14)+`","name":""}`,
		http.StatusOK)
	if moved.FolderID != inventory.SystemFolderID(userID, 14) || moved.Name != created.Name ||
		moved.CreatorUserID != created.CreatorUserID || moved.AssetID != created.AssetID {
		t.Fatalf("moved inventory item = %#v", moved)
	}
}

func TestCreateUploadedAssetTypePairs(t *testing.T) {
	const userID = "20000000-0000-4000-8000-000000000001"
	store := &memoryInventoryStore{folders: make(map[string][]inventory.Folder)}
	_, _ = store.EnsureSystemFolders(context.Background(), userID)
	handler := New(checker{}, "test", Options{ServiceToken: "secret", Inventory: store})
	tests := []struct {
		id, assetID, assetType, inventoryType string
		folderType                            int
	}{
		{"40000000-0000-4000-8000-000000000021", "50000000-0000-4000-8000-000000000021", "0", "15", 15},
		{"40000000-0000-4000-8000-000000000022", "50000000-0000-4000-8000-000000000022", "1", "1", 1},
		{"40000000-0000-4000-8000-000000000023", "50000000-0000-4000-8000-000000000023", "20", "19", 20},
		{"40000000-0000-4000-8000-000000000024", "50000000-0000-4000-8000-000000000024", "3", "3", 3},
		{"40000000-0000-4000-8000-000000000025", "50000000-0000-4000-8000-000000000025", "7", "7", 7},
		{"40000000-0000-4000-8000-000000000026", "50000000-0000-4000-8000-000000000026", "10", "10", 10},
		{"40000000-0000-4000-8000-000000000027", "50000000-0000-4000-8000-000000000027", "21", "20", 21},
	}
	for _, test := range tests {
		body := `{"id":"` + test.id + `","creatorUserId":"` + userID +
			`","folderId":"` + inventory.SystemFolderID(userID, test.folderType) +
			`","assetId":"` + test.assetID + `","assetType":` + test.assetType +
			`,"inventoryType":` + test.inventoryType + `,"name":"Upload","description":"",` +
			`"basePermissions":647168,"currentPermissions":647168,` +
			`"everyonePermissions":0,"nextPermissions":581632}`
		created := requestRegion[inventory.Item](t, handler, http.MethodPost,
			"/api/v1/inventory/"+userID+"/items", body, http.StatusCreated)
		if created.AssetID != test.assetID || created.FolderID != inventory.SystemFolderID(userID, test.folderType) {
			t.Fatalf("created uploaded item = %#v", created)
		}
	}
}

func TestCreateObjectInventoryItemEndpoint(t *testing.T) {
	const userID = "20000000-0000-4000-8000-000000000001"
	const creatorID = "30000000-0000-4000-8000-000000000001"
	store := &memoryInventoryStore{folders: make(map[string][]inventory.Folder)}
	_, _ = store.EnsureSystemFolders(context.Background(), userID)
	handler := New(checker{}, "test", Options{ServiceToken: "secret", Inventory: store})
	folderID := inventory.SystemFolderID(userID, 14)
	body := `{"id":"40000000-0000-4000-8000-000000000011",` +
		`"creatorUserId":"` + creatorID + `","folderId":"` + folderID + `",` +
		`"assetId":"50000000-0000-4000-8000-000000000011","assetType":6,` +
		`"inventoryType":6,"name":"Primitive","description":"",` +
		`"basePermissions":647168,"currentPermissions":647168,` +
		`"everyonePermissions":0,"nextPermissions":581632}`
	created := requestRegion[inventory.Item](t, handler, http.MethodPost,
		"/api/v1/inventory/"+userID+"/items", body, http.StatusCreated)
	if created.OwnerUserID != userID || created.CreatorUserID != creatorID ||
		created.FolderID != folderID || created.AssetType != 6 || created.InventoryType != 6 ||
		created.BasePermissions != 0x0009e000 || created.NextPermissions != 0x0008e000 {
		t.Fatalf("created object inventory item = %#v", created)
	}
	fetched := requestRegion[inventory.Item](t, handler, http.MethodGet,
		"/api/v1/inventory/"+userID+"/items/"+created.ID, "", http.StatusOK)
	if fetched.ID != created.ID || fetched.AssetID != created.AssetID || fetched.CreatorUserID != creatorID {
		t.Fatalf("fetched object inventory item = %#v", fetched)
	}
}

func TestUpdateWearableInventoryAssetEndpoint(t *testing.T) {
	const userID = "20000000-0000-4000-8000-000000000001"
	const itemID = "40000000-0000-4000-8000-000000000012"
	const assetID = "50000000-0000-4000-8000-000000000012"
	store := &memoryInventoryStore{folders: make(map[string][]inventory.Folder),
		items: make(map[string][]inventory.Item)}
	_, _ = store.EnsureSystemFolders(context.Background(), userID)
	store.items[userID] = []inventory.Item{{
		ID: itemID, OwnerUserID: userID, CreatorUserID: inventory.LibraryOwnerID,
		FolderID: inventory.SystemFolderID(userID, 13), AssetID: "50000000-0000-4000-8000-000000000011",
		AssetType: 13, InventoryType: 18, Name: "Default Shape",
		BasePermissions: 0x7fffffff, CurrentPermissions: 0x7fffffff,
	}}
	assets := &memoryAssetStore{assets: map[string]assetmeta.Asset{
		assetID: {ID: assetID, CreatorUserID: userID, SHA256: "aa", Size: 100},
	}}
	handler := New(checker{}, "test", Options{ServiceToken: "secret", Inventory: store, Assets: assets})
	updated := requestRegion[inventory.Item](t, handler, http.MethodPut,
		"/api/v1/inventory/"+userID+"/items/"+itemID+"/asset",
		`{"assetId":"`+assetID+`"}`, http.StatusOK)
	if updated.AssetID != assetID || updated.CreatorUserID != inventory.LibraryOwnerID {
		t.Fatalf("updated wearable item = %#v", updated)
	}
}

func TestCopyLibraryInventoryItemEndpoint(t *testing.T) {
	const userID = "20000000-0000-4000-8000-000000000001"
	store := &memoryInventoryStore{folders: make(map[string][]inventory.Folder)}
	_, _ = store.EnsureSystemFolders(context.Background(), userID)
	handler := New(checker{}, "test", Options{ServiceToken: "secret", Inventory: store})
	var source inventory.Item
	for _, item := range inventory.LibraryItems() {
		if item.Name == "Default Shape" {
			source = item
			break
		}
	}
	if source.ID == "" {
		t.Fatal("Default Shape is missing from the Library")
	}
	created := requestRegion[inventory.Item](t, handler, http.MethodPost,
		"/api/v1/inventory/"+userID+"/copy-library-item",
		`{"sourceItemId":"`+source.ID+`","destinationFolderId":"00000000-0000-0000-0000-000000000000","name":""}`,
		http.StatusCreated)
	if created.ID == "" || created.ID == source.ID || created.OwnerUserID != userID ||
		created.CreatorUserID != inventory.LibraryOwnerID ||
		created.FolderID != inventory.SystemFolderID(userID, 13) || created.AssetID != source.AssetID ||
		created.AssetType != source.AssetType || created.InventoryType != source.InventoryType ||
		created.Name != source.Name || created.Flags != source.Flags ||
		created.BasePermissions != source.BasePermissions || created.NextPermissions != source.NextPermissions {
		t.Fatalf("copied inventory item = %#v", created)
	}
	missing := requestRegion[Error](t, handler, http.MethodPost,
		"/api/v1/inventory/"+userID+"/copy-library-item",
		`{"sourceItemId":"40000000-0000-4000-8000-000000000099","destinationFolderId":"00000000-0000-0000-0000-000000000000","name":""}`,
		http.StatusNotFound)
	if missing.Code != "library_item_not_found" {
		t.Fatalf("missing library copy error = %#v", missing)
	}
}

func TestCopyLibraryTextureUsesPersonalTexturesFolder(t *testing.T) {
	const userID = "20000000-0000-4000-8000-000000000001"
	store := &memoryInventoryStore{folders: make(map[string][]inventory.Folder)}
	_, _ = store.EnsureSystemFolders(context.Background(), userID)
	handler := New(checker{}, "test", Options{ServiceToken: "secret", Inventory: store})
	var source inventory.Item
	for _, item := range inventory.LibraryItems() {
		if item.Name == "Blank" {
			source = item
			break
		}
	}
	if source.ID == "" {
		t.Fatal("Blank is missing from the Library")
	}
	created := requestRegion[inventory.Item](t, handler, http.MethodPost,
		"/api/v1/inventory/"+userID+"/copy-library-item",
		`{"sourceItemId":"`+source.ID+`","destinationFolderId":"00000000-0000-0000-0000-000000000000","name":""}`,
		http.StatusCreated)
	if created.FolderID != inventory.SystemFolderID(userID, 0) || created.AssetID != source.AssetID ||
		created.CreatorUserID != inventory.LibraryOwnerID || created.AssetType != 0 || created.InventoryType != 0 {
		t.Fatalf("copied library texture = %#v", created)
	}
}

func TestCopyPersonalInventoryItemEndpoint(t *testing.T) {
	const userID = "20000000-0000-4000-8000-000000000001"
	const sourceID = "40000000-0000-4000-8000-000000000020"
	store := &memoryInventoryStore{folders: make(map[string][]inventory.Folder), items: make(map[string][]inventory.Item)}
	_, _ = store.EnsureSystemFolders(context.Background(), userID)
	objectsID := inventory.SystemFolderID(userID, 6)
	source := inventory.Item{
		ID: sourceID, OwnerUserID: userID, CreatorUserID: "30000000-0000-4000-8000-000000000001",
		FolderID: objectsID, AssetID: "50000000-0000-4000-8000-000000000020",
		AssetType: 6, InventoryType: 6, Name: "Prim1", Description: "Tall box", Flags: 7,
		BasePermissions: 0x0009e000, CurrentPermissions: 0x0009e000,
		EveryonePermissions: 0x00080000, NextPermissions: 0x00086000, SaleType: 1, SalePrice: 25,
	}
	store.items[userID] = []inventory.Item{source}
	handler := New(checker{}, "test", Options{ServiceToken: "secret", Inventory: store})
	created := requestRegion[inventory.Item](t, handler, http.MethodPost,
		"/api/v1/inventory/"+userID+"/copy-item",
		`{"sourceItemId":"`+sourceID+`","destinationFolderId":"`+objectsID+`","name":"Prim1 copy"}`,
		http.StatusCreated)
	if created.ID == "" || created.ID == source.ID || created.OwnerUserID != userID ||
		created.CreatorUserID != source.CreatorUserID || created.FolderID != objectsID ||
		created.AssetID != source.AssetID || created.AssetType != source.AssetType ||
		created.InventoryType != source.InventoryType || created.Name != "Prim1 copy" ||
		created.Description != source.Description || created.Flags != source.Flags ||
		created.BasePermissions != source.BasePermissions ||
		created.CurrentPermissions != source.CurrentPermissions ||
		created.EveryonePermissions != source.EveryonePermissions ||
		created.NextPermissions != source.NextPermissions || created.SaleType != source.SaleType ||
		created.SalePrice != source.SalePrice {
		t.Fatalf("copied personal inventory item = %#v", created)
	}
	store.items[userID][0].CurrentPermissions &^= 0x00008000
	denied := requestRegion[Error](t, handler, http.MethodPost,
		"/api/v1/inventory/"+userID+"/copy-item",
		`{"sourceItemId":"`+sourceID+`","destinationFolderId":"`+objectsID+`","name":""}`,
		http.StatusForbidden)
	if denied.Code != "inventory_item_not_copyable" {
		t.Fatalf("no-copy error = %#v", denied)
	}
}

type failingInventoryStore struct{ inventory.Store }

func (failingInventoryStore) ListFolders(context.Context, string) ([]inventory.Folder, error) {
	return nil, errors.New("the metadata database refused the connection")
}

// TestInventoryStoreErrorIsLogged pins the operator's view of a store failure:
// the HTTP response stays a generic 500, so the underlying cause must reach
// the log or it is lost entirely.
func TestInventoryStoreErrorIsLogged(t *testing.T) {
	const userID = "20000000-0000-4000-8000-000000000001"
	var output bytes.Buffer
	logger := slog.New(slog.NewJSONHandler(&output, nil))
	handler := New(checker{}, "test", Options{ServiceToken: "secret",
		Inventory: failingInventoryStore{}, Logger: logger})
	failure := requestRegion[Error](t, handler, http.MethodGet,
		"/api/v1/inventory/"+userID+"/folders", "", http.StatusInternalServerError)
	if failure.Code != "inventory_store_error" {
		t.Fatalf("store failure = %#v", failure)
	}
	if !strings.Contains(output.String(), "the metadata database refused the connection") {
		t.Fatalf("store failure cause is missing from the log: %s", output.String())
	}
}

// TestInventoryCommitRequiresDurableAsset is the ADR 0026 invariant end to end:
// an item may not be committed while the vault cannot vouch for the bytes it
// references, and it commits the moment the vault can.
func TestInventoryCommitRequiresDurableAsset(t *testing.T) {
	const userID = "20000000-0000-4000-8000-000000000002"
	var source inventory.Item
	for _, item := range inventory.LibraryItems() {
		if item.Name == "Default Shape" {
			source = item
			break
		}
	}
	if source.ID == "" {
		t.Fatal("Default Shape is missing from the Library")
	}
	content := []byte("the wearable bytes behind the library shape")
	sum := sha256.Sum256(content)
	registry := &memoryRegistry{blobs: map[string]assetmeta.Blob{
		source.AssetID: {BlobID: testBlobID, ByteLength: int64(len(content)),
			Checksum: hex.EncodeToString(sum[:]), ChecksumAlgorithm: "sha256"},
	}}
	// A region that no longer holds the bytes — the state that lost the first
	// grid's outfits. The registry still names it, and that is not enough.
	gone := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusNotFound)
	}))
	defer gone.Close()
	registry.locations = []assetmeta.Location{{Endpoint: gone.URL, Origin: true}}

	store := &memoryInventoryStore{folders: make(map[string][]inventory.Folder)}
	_, _ = store.EnsureSystemFolders(context.Background(), userID)
	vaultStore := &memoryVault{registry: registry, blobs: make(map[string][]byte)}
	handler := New(checker{}, "test", Options{ServiceToken: "secret",
		Inventory: store, Assets: registry, Vault: vaultStore})

	body := `{"sourceItemId":"` + source.ID +
		`","destinationFolderId":"00000000-0000-0000-0000-000000000000","name":""}`
	refused := requestRegion[Error](t, handler, http.MethodPost,
		"/api/v1/inventory/"+userID+"/copy-library-item", body, http.StatusConflict)
	if refused.Code != "asset_not_durable" {
		t.Fatalf("refusal = %#v", refused)
	}
	if len(store.items[userID]) != 0 {
		t.Fatalf("items after refusal = %#v", store.items[userID])
	}

	// The same request succeeds once a location serves the bytes: the commit
	// path repairs durability rather than merely reporting on it.
	alive := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.Write(content)
	}))
	defer alive.Close()
	registry.locations = []assetmeta.Location{{Endpoint: alive.URL, Origin: true}}
	created := requestRegion[inventory.Item](t, handler, http.MethodPost,
		"/api/v1/inventory/"+userID+"/copy-library-item", body, http.StatusCreated)
	if created.AssetID != source.AssetID {
		t.Fatalf("created item = %#v", created)
	}
	if !bytes.Equal(vaultStore.blobs[testBlobID], content) {
		t.Fatalf("vault holds %q", vaultStore.blobs[testBlobID])
	}
}
