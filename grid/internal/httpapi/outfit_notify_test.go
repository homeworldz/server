package httpapi

import (
	"context"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/homeworldz/server/grid/internal/inventory"
	"github.com/homeworldz/server/grid/internal/regions"
)

// A round-tripper that records where the grid tried to send an outfit change.
type recordingTransport struct {
	requests chan *http.Request
}

func (t *recordingTransport) RoundTrip(request *http.Request) (*http.Response, error) {
	t.requests <- request
	return &http.Response{StatusCode: http.StatusOK, Body: http.NoBody, Request: request}, nil
}

// Wearing something has to reach the region holding the avatar. The outfit is
// changed here, in inventory, and a client that does not bake for itself has no
// way to tell anyone — so if the grid stays quiet the wearer keeps whatever the
// region baked at arrival, for the rest of the session.
func TestChangingTheCurrentOutfitTellsTheRegion(t *testing.T) {
	const regionID = "30000000-0000-4000-8000-000000000001"
	identities := newMemoryIdentityStore()
	user, err := identities.CreateUser(context.Background(), "inventory.ais.notify", "development-password")
	if err != nil {
		t.Fatal(err)
	}
	session, err := identities.CreateSession(context.Background(), "inventory.ais.notify", "development-password", time.Hour)
	if err != nil {
		t.Fatal(err)
	}
	if err := identities.AssignViewerDestination(context.Background(), session.ID, 123456, regionID); err != nil {
		t.Fatal(err)
	}
	inventories := &memoryInventoryStore{folders: make(map[string][]inventory.Folder)}
	_, _ = inventories.EnsureSystemFolders(context.Background(), user.ID)
	source := inventory.Item{
		ID: "40000000-0000-4000-8000-000000000051", OwnerUserID: user.ID, CreatorUserID: user.ID,
		FolderID: inventory.SystemFolderID(user.ID, 5), AssetID: "50000000-0000-4000-8000-000000000051",
		AssetType: 5, InventoryType: 18, Name: "Lower Alpha", Flags: 16,
		BasePermissions: 0x7fffffff, CurrentPermissions: 0x7fffffff, NextPermissions: 0x7fffffff}
	if _, err := inventories.CreateItem(context.Background(), source); err != nil {
		t.Fatal(err)
	}
	presences := newMemoryPresenceStore()
	if _, err := presences.Update(context.Background(), user.ID, regionID); err != nil {
		t.Fatal(err)
	}
	regionStore := newMemoryRegionStore()
	if _, err := regionStore.RegisterProvisioned(context.Background(), regionID, regions.Registration{
		Name: "Welcome", GridX: 1000, GridY: 1000, LeaseDuration: time.Hour,
		PublicEndpoint: "http://region.invalid:42011", ViewerPort: 42012}); err != nil {
		t.Fatal(err)
	}
	transport := &recordingTransport{requests: make(chan *http.Request, 4)}
	handler := New(checker{}, "test", Options{
		Identity: identities, Inventory: inventories, Presence: presences, Regions: regionStore,
		ServiceToken: "secret", OutfitHTTPClient: &http.Client{Transport: transport}})

	currentOutfitID := inventory.SystemFolderID(user.ID, 46)
	body := `<?xml version="1.0"?><llsd><map><key>links</key><array>` +
		`<map><key>linked_id</key><uuid>` + source.ID + `</uuid><key>type</key><integer>24</integer>` +
		`<key>name</key><string>Lower Alpha</string><key>desc</key><string></string></map>` +
		`</array></map></llsd>`
	request := httptest.NewRequest(http.MethodPost,
		"/caps/inventory/ais/"+session.ID+"/category/"+currentOutfitID, strings.NewReader(body))
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("wearing status = %d: %s", response.Code, response.Body.String())
	}

	select {
	case sent := <-transport.requests:
		if sent.Method != http.MethodPost {
			t.Fatalf("method = %s, want POST", sent.Method)
		}
		if want := "http://region.invalid:42011/appearance/refresh/" + user.ID; sent.URL.String() != want {
			t.Fatalf("url = %s, want %s", sent.URL.String(), want)
		}
		if sent.Header.Get("Authorization") != "Bearer secret" {
			t.Fatalf("authorization = %q", sent.Header.Get("Authorization"))
		}
	case <-time.After(2 * time.Second):
		t.Fatal("the region was never told the outfit changed")
	}

	// Reading inventory changes no outfit, and a region asked to re-bake for
	// every fetch would re-bake constantly — a viewer reads far more than it
	// wears.
	readRequest := httptest.NewRequest(http.MethodGet,
		"/caps/inventory/ais/"+session.ID+"/category/"+currentOutfitID+"/children", nil)
	handler.ServeHTTP(httptest.NewRecorder(), readRequest)
	select {
	case sent := <-transport.requests:
		t.Fatalf("a read told the region to re-bake: %s", sent.URL.String())
	case <-time.After(250 * time.Millisecond):
	}
}

// An outfit change for someone who is not logged in has nowhere to go, and that
// is ordinary rather than a fault: no call, and nothing said about it.
func TestOutfitChangeWithNoPresenceIsSilent(t *testing.T) {
	identities := newMemoryIdentityStore()
	user, err := identities.CreateUser(context.Background(), "inventory.ais.absent", "development-password")
	if err != nil {
		t.Fatal(err)
	}
	session, err := identities.CreateSession(context.Background(), "inventory.ais.absent", "development-password", time.Hour)
	if err != nil {
		t.Fatal(err)
	}
	if err := identities.AssignViewerDestination(context.Background(), session.ID, 123456,
		"30000000-0000-4000-8000-000000000001"); err != nil {
		t.Fatal(err)
	}
	inventories := &memoryInventoryStore{folders: make(map[string][]inventory.Folder)}
	_, _ = inventories.EnsureSystemFolders(context.Background(), user.ID)
	source := inventory.Item{
		ID: "40000000-0000-4000-8000-000000000061", OwnerUserID: user.ID, CreatorUserID: user.ID,
		FolderID: inventory.SystemFolderID(user.ID, 5), AssetID: "50000000-0000-4000-8000-000000000061",
		AssetType: 5, InventoryType: 18, Name: "Lower Alpha", Flags: 16,
		BasePermissions: 0x7fffffff, CurrentPermissions: 0x7fffffff, NextPermissions: 0x7fffffff}
	if _, err := inventories.CreateItem(context.Background(), source); err != nil {
		t.Fatal(err)
	}
	transport := &recordingTransport{requests: make(chan *http.Request, 4)}
	handler := New(checker{}, "test", Options{
		Identity: identities, Inventory: inventories, Presence: newMemoryPresenceStore(),
		Regions: newMemoryRegionStore(), ServiceToken: "secret",
		OutfitHTTPClient: &http.Client{Transport: transport}})

	body := `<?xml version="1.0"?><llsd><map><key>links</key><array>` +
		`<map><key>linked_id</key><uuid>` + source.ID + `</uuid><key>type</key><integer>24</integer>` +
		`<key>name</key><string>Lower Alpha</string><key>desc</key><string></string></map>` +
		`</array></map></llsd>`
	request := httptest.NewRequest(http.MethodPost,
		"/caps/inventory/ais/"+session.ID+"/category/"+inventory.SystemFolderID(user.ID, 46),
		strings.NewReader(body))
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("wearing status = %d: %s", response.Code, response.Body.String())
	}
	select {
	case sent := <-transport.requests:
		t.Fatalf("called a region for an absent wearer: %s", sent.URL.String())
	case <-time.After(250 * time.Millisecond):
	}
}
