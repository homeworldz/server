package api

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/homeworldz/server/grid/internal/inventory"
)

const otherUserID = "bbbbbbbb-0000-4000-8000-000000000009"

// memoryInventoryStore scopes by user id exactly as the Postgres store does,
// because that scoping is the whole ownership mechanism: a store that ignored
// the user id would make every test below pass while the route leaked.
type memoryInventoryStore struct {
	folders []inventory.Folder
	items   []inventory.Item
	// What world entry wrote, for the bootstrap tests.
	ensuredFolders int
	ensuredItems   []inventory.Item
}

func (s *memoryInventoryStore) ListFolders(_ context.Context, userID string) ([]inventory.Folder, error) {
	owned := make([]inventory.Folder, 0)
	for _, folder := range s.folders {
		if folder.OwnerUserID == userID {
			owned = append(owned, folder)
		}
	}
	return owned, nil
}

func (s *memoryInventoryStore) ListItems(_ context.Context, userID string) ([]inventory.Item, error) {
	owned := make([]inventory.Item, 0)
	for _, item := range s.items {
		if item.OwnerUserID == userID {
			owned = append(owned, item)
		}
	}
	return owned, nil
}

// EnsureSystemFolders and EnsureItem are what world entry writes. Recorded
// rather than stubbed: whether a client-only account gets its skeleton is a
// thing a test needs to be able to ask, and the answer is these two lists.
func (s *memoryInventoryStore) EnsureSystemFolders(_ context.Context, userID string) ([]inventory.Folder, error) {
	for _, folder := range inventory.SystemFolders(userID) {
		if !s.hasFolder(folder.ID) {
			s.folders = append(s.folders, folder)
			s.ensuredFolders++
		}
	}
	return s.ListFolders(context.Background(), userID)
}

func (s *memoryInventoryStore) EnsureItem(_ context.Context, item inventory.Item) (bool, error) {
	for _, existing := range s.items {
		if existing.ID == item.ID {
			return false, nil
		}
	}
	s.items = append(s.items, item)
	s.ensuredItems = append(s.ensuredItems, item)
	return true, nil
}

func (s *memoryInventoryStore) hasFolder(id string) bool {
	for _, folder := range s.folders {
		if folder.ID == id {
			return true
		}
	}
	return false
}

func newInventoryHarness(t *testing.T) *worldEntryHarness {
	t.Helper()
	store := &memoryInventoryStore{
		folders: []inventory.Folder{
			{ID: "f0000000-0000-4000-8000-000000000001", OwnerUserID: testUserID,
				ParentID: "00000000-0000-0000-0000-000000000000", Name: "My Inventory", Version: 3},
			{ID: "f0000000-0000-4000-8000-000000000002", OwnerUserID: testUserID,
				ParentID: "f0000000-0000-4000-8000-000000000001", Name: "Objects", TypeDefault: 6},
			// Owned by somebody else, and reachable only if the route forgets
			// to scope. Present in the store precisely so it can be asked for.
			{ID: "f0000000-0000-4000-8000-000000000099", OwnerUserID: otherUserID,
				ParentID: "00000000-0000-0000-0000-000000000000", Name: "Not Yours"},
		},
		items: []inventory.Item{
			{ID: "10000000-0000-4000-8000-000000000001", OwnerUserID: testUserID,
				FolderID: "f0000000-0000-4000-8000-000000000002", Name: "Probe Cube",
				AssetID: "a0000000-0000-4000-8000-000000000001", AssetType: 6, InventoryType: 6},
			{ID: "10000000-0000-4000-8000-000000000099", OwnerUserID: otherUserID,
				FolderID: "f0000000-0000-4000-8000-000000000099", Name: "Also Not Yours"},
		},
	}
	return newWorldEntryHarness(t, func(options *Options) { options.Inventory = store })
}

func inventoryGet(t *testing.T, harness *worldEntryHarness, path, bearer string) *httptest.ResponseRecorder {
	t.Helper()
	request := httptest.NewRequest(http.MethodGet, path, nil)
	if bearer != "" {
		request.Header.Set("Authorization", "Bearer "+bearer)
	}
	recorder := httptest.NewRecorder()
	harness.handler.ServeHTTP(recorder, request)
	return recorder
}

func TestClientInventoryRootListsOnlyOwnFolders(t *testing.T) {
	harness := newInventoryHarness(t)
	recorder := inventoryGet(t, harness, "/v1/client/inventory", harness.token)
	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200: %s", recorder.Code, recorder.Body.String())
	}
	var body struct {
		RootID  string             `json:"rootId"`
		Folders []inventory.Folder `json:"folders"`
	}
	if err := json.Unmarshal(recorder.Body.Bytes(), &body); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if len(body.Folders) != 2 {
		t.Fatalf("folders = %d, want 2 (the third belongs to another user)", len(body.Folders))
	}
	// The root is derived from the parent that is not itself an owned folder,
	// not from a hard-coded id.
	if body.RootID != "f0000000-0000-4000-8000-000000000001" {
		t.Fatalf("rootId = %q, want the folder whose parent is not owned", body.RootID)
	}
}

func TestClientInventoryFolderReturnsChildrenAndItems(t *testing.T) {
	harness := newInventoryHarness(t)
	recorder := inventoryGet(t, harness,
		"/v1/client/inventory/folder/f0000000-0000-4000-8000-000000000002", harness.token)
	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200: %s", recorder.Code, recorder.Body.String())
	}
	var body struct {
		Folder  inventory.Folder   `json:"folder"`
		Folders []inventory.Folder `json:"folders"`
		Items   []inventory.Item   `json:"items"`
	}
	if err := json.Unmarshal(recorder.Body.Bytes(), &body); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if body.Folder.Name != "Objects" {
		t.Fatalf("folder = %q, want Objects", body.Folder.Name)
	}
	if len(body.Items) != 1 || body.Items[0].Name != "Probe Cube" {
		t.Fatalf("items = %+v, want just Probe Cube", body.Items)
	}
	// Empty rather than null: a client iterating the field must not have to
	// special-case a folder with no subfolders.
	if body.Folders == nil {
		t.Fatal("folders = null, want an empty array")
	}
}

// The property the whole design rests on. Another user's folder is not refused
// by a comparison that could be omitted — it is absent from the rows the route
// loaded, so forgetting the check is not a reachable state.
func TestClientInventoryCannotReadAnotherUsersFolder(t *testing.T) {
	harness := newInventoryHarness(t)
	recorder := inventoryGet(t, harness,
		"/v1/client/inventory/folder/f0000000-0000-4000-8000-000000000099", harness.token)
	if recorder.Code != http.StatusNotFound {
		t.Fatalf("status = %d, want 404: %s", recorder.Code, recorder.Body.String())
	}
	// And the same answer a nonexistent id gets, so the response cannot be used
	// to probe whether an id exists at all.
	missing := inventoryGet(t, harness,
		"/v1/client/inventory/folder/f0000000-0000-4000-8000-0000000000ff", harness.token)
	if missing.Code != recorder.Code || missing.Body.String() != recorder.Body.String() {
		t.Fatalf("owned-by-another and nonexistent differ:\n %s\n %s",
			recorder.Body.String(), missing.Body.String())
	}
}

func TestClientInventoryCannotReadAnotherUsersItem(t *testing.T) {
	harness := newInventoryHarness(t)
	recorder := inventoryGet(t, harness,
		"/v1/client/inventory/item/10000000-0000-4000-8000-000000000099", harness.token)
	if recorder.Code != http.StatusNotFound {
		t.Fatalf("status = %d, want 404: %s", recorder.Code, recorder.Body.String())
	}
}

func TestClientInventoryItemReturnsOwnItem(t *testing.T) {
	harness := newInventoryHarness(t)
	recorder := inventoryGet(t, harness,
		"/v1/client/inventory/item/10000000-0000-4000-8000-000000000001", harness.token)
	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200: %s", recorder.Code, recorder.Body.String())
	}
	var body struct {
		Item inventory.Item `json:"item"`
	}
	if err := json.Unmarshal(recorder.Body.Bytes(), &body); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if body.Item.AssetID != "a0000000-0000-4000-8000-000000000001" {
		t.Fatalf("assetId = %q, want the stored asset id", body.Item.AssetID)
	}
}

// A route this build does not serve and a resource the caller may not see are
// both 404 and must not look alike. Sharing a code sent the client team to
// examine auth and ownership when the real answer was a tier that had not been
// deployed yet.
func TestUnservedRouteIsDistinguishableFromResourceMiss(t *testing.T) {
	harness := newInventoryHarness(t)
	unserved := inventoryGet(t, harness, "/v1/client/inventory-that-does-not-exist", harness.token)
	notYours := inventoryGet(t, harness,
		"/v1/client/inventory/folder/f0000000-0000-4000-8000-000000000099", harness.token)
	if unserved.Code != http.StatusNotFound || notYours.Code != http.StatusNotFound {
		t.Fatalf("statuses = %d and %d, want both 404", unserved.Code, notYours.Code)
	}
	var missRoute, missResource struct {
		Code string `json:"code"`
	}
	if err := json.Unmarshal(unserved.Body.Bytes(), &missRoute); err != nil {
		t.Fatalf("decode unserved: %v", err)
	}
	if err := json.Unmarshal(notYours.Body.Bytes(), &missResource); err != nil {
		t.Fatalf("decode resource miss: %v", err)
	}
	if missRoute.Code != "route_not_found" {
		t.Fatalf("unserved route code = %q, want route_not_found", missRoute.Code)
	}
	if missResource.Code != "not_found" {
		t.Fatalf("resource miss code = %q, want not_found", missResource.Code)
	}
}

func TestClientInventoryRequiresAuthentication(t *testing.T) {
	harness := newInventoryHarness(t)
	for _, path := range []string{
		"/v1/client/inventory",
		"/v1/client/inventory/folder/f0000000-0000-4000-8000-000000000001",
		"/v1/client/inventory/item/10000000-0000-4000-8000-000000000001",
	} {
		if recorder := inventoryGet(t, harness, path, ""); recorder.Code != http.StatusUnauthorized {
			t.Fatalf("%s unauthenticated = %d, want 401", path, recorder.Code)
		}
	}
}
