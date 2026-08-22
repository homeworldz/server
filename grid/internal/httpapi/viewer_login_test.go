package httpapi

import (
	"bytes"
	"context"
	"crypto/md5"
	"encoding/hex"
	"encoding/xml"
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/homeworldz/server/grid/internal/arrival"
	"github.com/homeworldz/server/grid/internal/inventory"
	"github.com/homeworldz/server/grid/internal/locations"
	"github.com/homeworldz/server/grid/internal/provisioning"
	"github.com/homeworldz/server/grid/internal/regions"
)

type memoryLocationStore struct{ value locations.Location }

func (s memoryLocationStore) Get(context.Context, string) (locations.Location, error) {
	if s.value.RegionID == "" {
		return locations.Location{}, locations.ErrNotFound
	}
	return s.value, nil
}

func (s memoryLocationStore) Update(_ context.Context, value locations.Location) (locations.Location, error) {
	return value, nil
}

func (s memoryLocationStore) GetHome(context.Context, string) (locations.Location, error) {
	if s.value.RegionID == "" {
		return locations.Location{}, locations.ErrNotFound
	}
	return s.value, nil
}

func (s memoryLocationStore) UpdateHome(_ context.Context, value locations.Location) (locations.Location, error) {
	return value, nil
}

func viewerRequest(first, last, password, start string) string {
	digest := md5.Sum([]byte(password))
	start = strings.ReplaceAll(start, "&", "&amp;")
	return `<?xml version="1.0"?><methodCall><methodName>login_to_simulator</methodName><params><param><value><struct>` +
		`<member><name>first</name><value><string>` + first + `</string></value></member>` +
		`<member><name>last</name><value><string>` + last + `</string></value></member>` +
		`<member><name>passwd</name><value><string>$1$` + hex.EncodeToString(digest[:]) + `</string></value></member>` +
		`<member><name>start</name><value><string>` + start + `</string></value></member>` +
		`</struct></value></param></params></methodCall>`
}

func viewerResponse(t *testing.T, handler http.Handler, body string) map[string]rpcValue {
	t.Helper()
	r := httptest.NewRequest(http.MethodPost, "/login", bytes.NewBufferString(body))
	r.Header.Set("Content-Type", "text/xml")
	w := httptest.NewRecorder()
	handler.ServeHTTP(w, r)
	if w.Code != http.StatusOK {
		t.Fatalf("status = %d: %s", w.Code, w.Body.String())
	}
	if !strings.HasPrefix(w.Header().Get("Content-Type"), "text/xml") {
		t.Fatalf("content type = %q", w.Header().Get("Content-Type"))
	}
	var response struct {
		Value rpcValue `xml:"params>param>value"`
	}
	if err := xml.NewDecoder(w.Body).Decode(&response); err != nil {
		t.Fatalf("decode response: %v\n%s", err, w.Body.String())
	}
	return response.Value.fields()
}

func TestViewerLoginResolvesNamedRegion(t *testing.T) {
	var testUserID string
	startState := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/api/v1/agents/"+testUserID+"/start-state" ||
			r.Header.Get("Authorization") != "Bearer region-secret" {
			http.Error(w, "unexpected start-state request", http.StatusBadRequest)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"position":[202,144,28],"lookAt":[-0.995,-0.098,0],"flying":true}`))
	}))
	defer startState.Close()
	identities := newMemoryIdentityStore()
	user, err := identities.CreateUser(context.Background(), "test.user", "development-password")
	if err != nil {
		t.Fatal(err)
	}
	testUserID = user.ID
	regionStore := newMemoryRegionStore()
	_, _ = regionStore.Register(context.Background(), regions.Registration{Name: "Fallback", GridX: 1000, GridY: 1000,
		PublicEndpoint: "http://fallback.example:42001", LeaseDuration: time.Minute})
	target, _ := regionStore.Register(context.Background(), regions.Registration{Name: "Welcome", GridX: 1001, GridY: 1002,
		PublicEndpoint: startState.URL, ViewerPort: 43002, LeaseDuration: time.Minute})
	provisionedPath := filepath.Join(t.TempDir(), "regions.json")
	provisionedJSON := fmt.Sprintf(`[{"id":%q,"name":"Welcome","mapX":1001,"mapY":1002,"size":2,"accessKey":"welcome-key"}]`, target.ID)
	if err := os.WriteFile(provisionedPath, []byte(provisionedJSON), 0600); err != nil {
		t.Fatal(err)
	}
	provisioned, err := provisioning.Load(provisionedPath)
	if err != nil {
		t.Fatal(err)
	}
	inventories := &memoryInventoryStore{folders: make(map[string][]inventory.Folder)}
	handler := New(checker{}, "test", Options{ServiceToken: "region-secret", Identity: identities,
		Regions: regionStore, Provisioned: provisioned, Inventory: inventories})
	fields := viewerResponse(t, handler, viewerRequest("Test", "User", "development-password", "uri:Welcome&128&128&25"))
	if fields["login"].text() != "true" {
		t.Fatalf("login = %q, reason = %q, message = %q", fields["login"].text(), fields["reason"].text(), fields["message"].text())
	}
	if fields["agent_id"].text() == "" || fields["session_id"].text() == "" || fields["secure_session_id"].text() == "" {
		t.Fatalf("missing session identity: %#v", fields)
	}
	if fields["sim_ip"].text() != "127.0.0.1" || fields["sim_port"].text() != "43002" ||
		fields["region_x"].text() != "256256" || fields["region_y"].text() != "256512" ||
		fields["region_size_x"].text() != "512" || fields["region_size_y"].text() != "512" {
		t.Fatalf("unexpected destination: %#v", fields)
	}
	wantSeed := strings.TrimRight(target.PublicEndpoint, "/") + "/caps/seed/" + fields["session_id"].text()
	if fields["seed_capability"].text() != wantSeed {
		t.Fatalf("seed = %q, want %q", fields["seed_capability"].text(), wantSeed)
	}
	if fields["look_at"].text() != "[r-0.995,r-0.098,r0]" {
		t.Fatalf("look_at = %q", fields["look_at"].text())
	}
	rootValues := fields["inventory-root"].Array.Values
	skeletonValues := fields["inventory-skeleton"].Array.Values
	if len(rootValues) != 1 || len(skeletonValues) != 23 {
		t.Fatalf("inventory root or skeleton missing: %#v", fields)
	}
	rootID := rootValues[0].fields()["folder_id"].text()
	types := make(map[string]bool)
	ids := make(map[string]bool)
	for index, value := range skeletonValues {
		folder := value.fields()
		id := folder["folder_id"].text()
		parent := folder["parent_id"].text()
		if id == "" || ids[id] || folder["version"].text() == "" {
			t.Fatalf("invalid inventory folder %d: %#v", index, folder)
		}
		ids[id] = true
		types[folder["type_default"].text()] = true
		if index == 0 {
			if id != rootID || folder["name"].text() != "My Inventory" ||
				parent != "00000000-0000-0000-0000-000000000000" {
				t.Fatalf("invalid inventory root: %#v", folder)
			}
		} else if folder["name"].text() != "Friends" && folder["name"].text() != "All" && parent != rootID {
			t.Fatalf("inventory folder %d parent = %q, want %q", index, parent, rootID)
		}
	}
	libraryRoots := fields["inventory-lib-root"].Array.Values
	libraryOwners := fields["inventory-lib-owner"].Array.Values
	librarySkeleton := fields["inventory-skel-lib"].Array.Values
	if len(libraryRoots) != 1 || libraryRoots[0].fields()["folder_id"].text() != inventory.LibraryRootID ||
		len(libraryOwners) != 1 || libraryOwners[0].fields()["agent_id"].text() != inventory.LibraryOwnerID ||
		len(librarySkeleton) != len(inventory.LibraryFolders()) {
		t.Fatalf("inventory library login data missing: %#v", fields)
	}
	libraryNames := make(map[string]bool)
	for _, value := range librarySkeleton {
		name := value.fields()["name"].text()
		libraryNames[name] = true
		if strings.Contains(name, "Homeworldz") || strings.HasPrefix(name, "My ") {
			t.Fatalf("branded or personal library folder name = %q", name)
		}
	}
	for _, name := range []string{"Library", "Clothing", "Body Parts", "Textures", "Terrain", "Initial Outfits", "Default Avatar"} {
		if !libraryNames[name] {
			t.Fatalf("inventory library lacks %q: %#v", name, librarySkeleton)
		}
	}
	items, err := inventories.ListItems(context.Background(), fields["agent_id"].text())
	if err != nil || len(items) != 12 {
		t.Fatalf("default inventory items = %#v, error = %v", items, err)
	}
	currentOutfitID := inventory.SystemFolderID(fields["agent_id"].text(), 46)
	var currentLinkIDs []string
	for _, item := range items {
		if item.FolderID == currentOutfitID {
			currentLinkIDs = append(currentLinkIDs, item.ID)
		}
	}
	for _, itemID := range currentLinkIDs {
		if _, err := inventories.DeleteItem(context.Background(), fields["agent_id"].text(), itemID); err != nil {
			t.Fatal(err)
		}
	}
	items, _ = inventories.ListItems(context.Background(), fields["agent_id"].text())
	if len(items) != 6 {
		t.Fatalf("inventory after simulated interrupted outfit initialization = %#v", items)
	}
	secondFields := viewerResponse(t, handler,
		viewerRequest("Test", "User", "development-password", "uri:Welcome&128&128&25"))
	items, _ = inventories.ListItems(context.Background(), secondFields["agent_id"].text())
	cofItems := 0
	for _, item := range items {
		if item.FolderID == currentOutfitID {
			cofItems++
		}
	}
	if len(items) != 12 || cofItems != 6 {
		t.Fatalf("repaired default inventory items = %#v", items)
	}
	for _, required := range []string{"0", "1", "5", "6", "7", "10", "13", "15", "16", "20", "21"} {
		if !types[required] {
			t.Fatalf("inventory skeleton lacks required folder type %s", required)
		}
	}
	session, err := identities.ValidateSession(context.Background(), fields["session_id"].text())
	if err != nil || session.ViewerCircuitCode == 0 || session.DestinationRegionID != target.ID ||
		fields["circuit_code"].text() != fmt.Sprint(session.ViewerCircuitCode) {
		t.Fatalf("viewer circuit was not persisted: session=%#v error=%v", session, err)
	}
}

// The login facet follows the region's own persisted spawn (ADR 0036): an
// avatar the region remembers standing on facet 1 must be handed facet 1's
// port and origin, whatever the grid's locations store says. A login handed
// the wrong facet arrives standing across an internal line and fires the
// crossing ceremony into a viewer still logging in (seen live 2026-08-20).
func TestViewerLoginPicksFacetFromRegionSpawn(t *testing.T) {
	var testUserID string
	startState := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/api/v1/agents/"+testUserID+"/start-state" ||
			r.Header.Get("Authorization") != "Bearer region-secret" {
			http.Error(w, "unexpected start-state request", http.StatusBadRequest)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		// Macro coordinates on the northern facet of a 1x2 rectangle.
		_, _ = w.Write([]byte(`{"position":[100,400,30],"lookAt":[1,0,0],"flying":false}`))
	}))
	defer startState.Close()
	identities := newMemoryIdentityStore()
	user, err := identities.CreateUser(context.Background(), "facet.user", "development-password")
	if err != nil {
		t.Fatal(err)
	}
	testUserID = user.ID
	regionStore := newMemoryRegionStore()
	target, _ := regionStore.Register(context.Background(), regions.Registration{Name: "Sandbox", GridX: 1001, GridY: 1000,
		PublicEndpoint: startState.URL, ViewerPort: 43002, LeaseDuration: time.Minute})
	provisionedPath := filepath.Join(t.TempDir(), "regions.json")
	provisionedJSON := fmt.Sprintf(`[{"id":%q,"name":"Sandbox","mapX":1001,"mapY":1000,"sizeX":1,"sizeY":2,"facetNames":["Sandbox 2"],"accessKey":"sandbox-key"}]`, target.ID)
	if err := os.WriteFile(provisionedPath, []byte(provisionedJSON), 0600); err != nil {
		t.Fatal(err)
	}
	provisioned, err := provisioning.Load(provisionedPath)
	if err != nil {
		t.Fatal(err)
	}
	inventories := &memoryInventoryStore{folders: make(map[string][]inventory.Folder)}
	handler := New(checker{}, "test", Options{ServiceToken: "region-secret", Identity: identities,
		Regions: regionStore, Provisioned: provisioned, Inventory: inventories})
	fields := viewerResponse(t, handler, viewerRequest("Facet", "User", "development-password", "uri:Sandbox&128&128&25"))
	if fields["login"].text() != "true" {
		t.Fatalf("login = %q, reason = %q, message = %q", fields["login"].text(), fields["reason"].text(), fields["message"].text())
	}
	// Facet 1 of a 1x2 at (1001,1000): one square north, on the next port.
	if fields["sim_port"].text() != "43003" ||
		fields["region_x"].text() != "256256" || fields["region_y"].text() != "256256" ||
		fields["region_size_x"].text() != "256" || fields["region_size_y"].text() != "256" {
		t.Fatalf("unexpected facet destination: port=%q x=%q y=%q sizeX=%q sizeY=%q",
			fields["sim_port"].text(), fields["region_x"].text(), fields["region_y"].text(),
			fields["region_size_x"].text(), fields["region_size_y"].text())
	}
}

// A facet of a rectangular region is a region to the viewer (ADR 0036), so
// its name must work in the login location field like any region name. The
// facet name lives only on the provisioned record; the login must still land
// on the live lease, on the facet's own port and map corner.
func TestViewerLoginResolvesFacetName(t *testing.T) {
	identities := newMemoryIdentityStore()
	if _, err := identities.CreateUser(context.Background(), "facet.name", "development-password"); err != nil {
		t.Fatal(err)
	}
	regionStore := newMemoryRegionStore()
	target, _ := regionStore.Register(context.Background(), regions.Registration{Name: "Sandbox", GridX: 1001, GridY: 1000,
		PublicEndpoint: "http://127.0.0.13:42021", ViewerPort: 43002, LeaseDuration: time.Minute})
	provisionedPath := filepath.Join(t.TempDir(), "regions.json")
	provisionedJSON := fmt.Sprintf(`[{"id":%q,"name":"Sandbox","mapX":1001,"mapY":1000,"sizeX":1,"sizeY":2,"facetNames":["Sandbox 2"],"accessKey":"sandbox-key"}]`, target.ID)
	if err := os.WriteFile(provisionedPath, []byte(provisionedJSON), 0600); err != nil {
		t.Fatal(err)
	}
	provisioned, err := provisioning.Load(provisionedPath)
	if err != nil {
		t.Fatal(err)
	}
	handler := New(checker{}, "test", Options{Identity: identities, Regions: regionStore, Provisioned: provisioned,
		Inventory: &memoryInventoryStore{folders: make(map[string][]inventory.Folder)}})
	fields := viewerResponse(t, handler, viewerRequest("Facet", "Name", "development-password", "uri:Sandbox 2&128&128&25"))
	if fields["login"].text() != "true" {
		t.Fatalf("login = %q, reason = %q, message = %q", fields["login"].text(), fields["reason"].text(), fields["message"].text())
	}
	// Facet 1 of a 1x2 at (1001,1000): one square north, on the next port.
	if fields["sim_port"].text() != "43003" ||
		fields["region_x"].text() != "256256" || fields["region_y"].text() != "256256" {
		t.Fatalf("unexpected facet destination: port=%q x=%q y=%q",
			fields["sim_port"].text(), fields["region_x"].text(), fields["region_y"].text())
	}
}

func TestViewerLoginUsesDurableLastRegion(t *testing.T) {
	identities := newMemoryIdentityStore()
	user, err := identities.CreateUser(context.Background(), "last.user", "development-password")
	if err != nil {
		t.Fatal(err)
	}
	regionStore := newMemoryRegionStore()
	_, _ = regionStore.Register(context.Background(), regions.Registration{Name: "Welcome", GridX: 1000, GridY: 1000,
		PublicEndpoint: "http://127.0.0.11:42011", ViewerPort: 42012, LeaseDuration: time.Minute})
	sandbox, _ := regionStore.Register(context.Background(), regions.Registration{Name: "Sandbox", GridX: 1001, GridY: 1000,
		PublicEndpoint: "http://127.0.0.12:42001", ViewerPort: 42002, LeaseDuration: time.Minute})
	handler := New(checker{}, "test", Options{Identity: identities, Regions: regionStore,
		Inventory: &memoryInventoryStore{folders: make(map[string][]inventory.Folder)},
		Locations: memoryLocationStore{value: locations.Location{UserID: user.ID, RegionID: sandbox.ID}}})
	fields := viewerResponse(t, handler, viewerRequest("Last", "User", "development-password", "last"))
	if fields["login"].text() != "true" || fields["sim_ip"].text() != "127.0.0.12" ||
		fields["region_x"].text() != "256256" {
		t.Fatalf("last-location destination = %#v", fields)
	}
}

// The welcome list decides where a viewer with no usable stored location
// lands, replacing the old first-listed-region fallback (docs/CLIENT2.md,
// "Default and fallback arrival points"). And when a welcome list is
// configured but none of its regions is online, the login is refused rather
// than landing the user somewhere the operator never named.
func TestViewerLoginLandsOnWelcomeList(t *testing.T) {
	identities := newMemoryIdentityStore()
	_, _ = identities.CreateUser(context.Background(), "new.user", "development-password")
	regionStore := newMemoryRegionStore()
	// Registered first, so the legacy fallback would have chosen it.
	_, _ = regionStore.Register(context.Background(), regions.Registration{Name: "Elsewhere", GridX: 999, GridY: 999,
		PublicEndpoint: "http://127.0.0.13:42021", ViewerPort: 42022, LeaseDuration: time.Minute})
	_, _ = regionStore.Register(context.Background(), regions.Registration{Name: "Welcome", GridX: 1000, GridY: 1000,
		PublicEndpoint: "http://127.0.0.11:42011", ViewerPort: 42012, LeaseDuration: time.Minute})

	handler := New(checker{}, "test", Options{Identity: identities, Regions: regionStore,
		Inventory: &memoryInventoryStore{folders: make(map[string][]inventory.Folder)},
		Welcome:   []arrival.Point{{Region: "Welcome", X: 127, Y: 127, Z: 23}}})
	fields := viewerResponse(t, handler, viewerRequest("New", "User", "development-password", "last"))
	if fields["login"].text() != "true" || fields["sim_ip"].text() != "127.0.0.11" {
		t.Fatalf("welcome-list destination = %#v", fields)
	}

	offlineOnly := New(checker{}, "test", Options{Identity: identities, Regions: regionStore,
		Inventory: &memoryInventoryStore{folders: make(map[string][]inventory.Folder)},
		Welcome:   []arrival.Point{{Region: "Not Leased", X: 1, Y: 1, Z: 1}}})
	fields = viewerResponse(t, offlineOnly, viewerRequest("New", "User", "development-password", "last"))
	if fields["login"].text() != "false" || fields["reason"].text() != "destination" {
		t.Fatalf("exhausted welcome list = %#v", fields)
	}
}

// sim_ip is a 32-bit address the viewer parses with inet_addr, so a region
// endpoint named by hostname must reach the viewer resolved. Passing the
// hostname through produces a login the viewer accepts and then cannot build a
// circuit from, which is what configuring real grid URLs did. A host that
// cannot be resolved fails the login rather than shipping a name the viewer
// will reject with nothing naming the cause.
func TestViewerLoginResolvesRegionHostnameToAddress(t *testing.T) {
	identities := newMemoryIdentityStore()
	_, _ = identities.CreateUser(context.Background(), "host.user", "development-password")
	regionStore := newMemoryRegionStore()
	_, _ = regionStore.Register(context.Background(), regions.Registration{Name: "Welcome", GridX: 1000, GridY: 1000,
		PublicEndpoint: "http://localhost:42011", ViewerPort: 42012, LeaseDuration: time.Minute})
	handler := New(checker{}, "test", Options{Identity: identities, Regions: regionStore,
		Inventory: &memoryInventoryStore{folders: make(map[string][]inventory.Folder)},
		Welcome:   []arrival.Point{{Region: "Welcome", X: 127, Y: 127, Z: 23}}})
	fields := viewerResponse(t, handler, viewerRequest("Host", "User", "development-password", "last"))
	if fields["login"].text() != "true" || net.ParseIP(fields["sim_ip"].text()).To4() == nil {
		t.Fatalf("hostname endpoint = %#v", fields)
	}
	// The seed capability keeps the hostname: it is an HTTP URL the viewer
	// resolves normally, and rewriting it would break TLS certificate matching.
	if !strings.Contains(fields["seed_capability"].text(), "localhost:42011") {
		t.Fatalf("seed capability lost the hostname = %q", fields["seed_capability"].text())
	}

	unresolvable := newMemoryRegionStore()
	_, _ = unresolvable.Register(context.Background(), regions.Registration{Name: "Welcome", GridX: 1000, GridY: 1000,
		PublicEndpoint: "http://welcome.invalid:42011", ViewerPort: 42012, LeaseDuration: time.Minute})
	refusing := New(checker{}, "test", Options{Identity: identities, Regions: unresolvable,
		Inventory: &memoryInventoryStore{folders: make(map[string][]inventory.Folder)},
		Welcome:   []arrival.Point{{Region: "Welcome", X: 127, Y: 127, Z: 23}}})
	fields = viewerResponse(t, refusing, viewerRequest("Host", "User", "development-password", "last"))
	if fields["login"].text() != "false" || fields["reason"].text() != "unavailable" {
		t.Fatalf("unresolvable endpoint = %#v", fields)
	}
}

func TestViewerLoginRejectsCredentialsAndMissingDestination(t *testing.T) {
	identities := newMemoryIdentityStore()
	_, _ = identities.CreateUser(context.Background(), "test.user", "development-password")
	regionStore := newMemoryRegionStore()
	_, _ = regionStore.Register(context.Background(), regions.Registration{Name: "Welcome", GridX: 1, GridY: 2,
		PublicEndpoint: "http://127.0.0.1:42001", LeaseDuration: time.Minute})
	handler := New(checker{}, "test", Options{Identity: identities, Regions: regionStore})
	fields := viewerResponse(t, handler, viewerRequest("Test", "User", "wrong-password", "home"))
	if fields["login"].text() != "false" || fields["reason"].text() != "key" {
		t.Fatalf("credential failure = %#v", fields)
	}
	fields = viewerResponse(t, handler, viewerRequest("Test", "User", "development-password", "uri:Missing&1&2&3"))
	if fields["login"].text() != "false" || fields["reason"].text() != "destination" {
		t.Fatalf("destination failure = %#v", fields)
	}
}
