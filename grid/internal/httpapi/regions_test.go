package httpapi

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"sort"
	"testing"
	"time"

	"github.com/homeworldz/server/grid/internal/provisioning"
	"github.com/homeworldz/server/grid/internal/regions"
)

type memoryRegionStore struct {
	now     time.Time
	nextID  int
	regions map[string]regions.Region
}

func newMemoryRegionStore() *memoryRegionStore {
	return &memoryRegionStore{
		now:     time.Date(2026, 7, 13, 12, 0, 0, 0, time.UTC),
		regions: make(map[string]regions.Region),
	}
}

func (s *memoryRegionStore) Register(_ context.Context, input regions.Registration) (regions.Region, error) {
	if input.ViewerPort == 0 {
		input.ViewerPort = 42002
	}
	for id, region := range s.regions {
		if !region.LeaseExpiresAt.After(s.now) {
			delete(s.regions, id)
			continue
		}
		if region.GridX == input.GridX && region.GridY == input.GridY {
			return regions.Region{}, regions.ErrConflict
		}
	}
	s.nextID++
	region := regions.Region{
		ID:   fmt.Sprintf("00000000-0000-4000-8000-%012d", s.nextID),
		Name: input.Name, GridX: input.GridX, GridY: input.GridY,
		PublicEndpoint: input.PublicEndpoint, ViewerPort: input.ViewerPort,
		LeaseExpiresAt:  s.now.Add(input.LeaseDuration),
		SessionEndpoint: input.SessionEndpoint,
	}
	s.regions[region.ID] = region
	return region, nil
}

func (s *memoryRegionStore) RegisterProvisioned(_ context.Context, id string, input regions.Registration) (regions.Region, error) {
	for otherID, region := range s.regions {
		if otherID != id && region.LeaseExpiresAt.After(s.now) && region.GridX == input.GridX && region.GridY == input.GridY {
			return regions.Region{}, regions.ErrConflict
		}
	}
	region := regions.Region{ID: id, Name: input.Name, GridX: input.GridX, GridY: input.GridY,
		PublicEndpoint: input.PublicEndpoint, ViewerPort: input.ViewerPort,
		LeaseExpiresAt: s.now.Add(input.LeaseDuration), SessionEndpoint: input.SessionEndpoint}
	s.regions[id] = region
	return region, nil
}

func (s *memoryRegionStore) Renew(_ context.Context, id string, duration time.Duration) (regions.Region, error) {
	region, ok := s.regions[id]
	if !ok || !region.LeaseExpiresAt.After(s.now) {
		return regions.Region{}, regions.ErrNotFound
	}
	region.LeaseExpiresAt = s.now.Add(duration)
	s.regions[id] = region
	return region, nil
}

func (s *memoryRegionStore) RenewProvisioned(
	ctx context.Context, id string, duration time.Duration,
) (regions.Region, error) {
	return s.Renew(ctx, id, duration)
}

func (s *memoryRegionStore) Deregister(_ context.Context, id string) error {
	if _, ok := s.regions[id]; !ok {
		return regions.ErrNotFound
	}
	delete(s.regions, id)
	return nil
}

func (s *memoryRegionStore) DeregisterProvisioned(_ context.Context, id string) error {
	region, ok := s.regions[id]
	if !ok {
		return regions.ErrNotFound
	}
	region.LeaseExpiresAt = s.now
	s.regions[id] = region
	return nil
}

func (s *memoryRegionStore) Get(_ context.Context, id string) (regions.Region, error) {
	region, ok := s.regions[id]
	if !ok || !region.LeaseExpiresAt.After(s.now) {
		return regions.Region{}, regions.ErrNotFound
	}
	return region, nil
}

func (s *memoryRegionStore) List(context.Context) ([]regions.Region, error) {
	items := make([]regions.Region, 0)
	for _, region := range s.regions {
		if region.LeaseExpiresAt.After(s.now) {
			items = append(items, region)
		}
	}
	sort.Slice(items, func(i, j int) bool { return items[i].ID < items[j].ID })
	return items, nil
}

func TestRegionDiscoveryIsReadOnly(t *testing.T) {
	store := newMemoryRegionStore()
	handler := New(checker{}, "test", Options{ServiceToken: "secret", Regions: store})
	created, err := store.RegisterProvisioned(context.Background(),
		"11111111-1111-4111-8111-111111111111", regions.Registration{
			Name: "Test Region", GridX: 1000, GridY: 1001,
			PublicEndpoint: "http://region.example:42001", ViewerPort: 42002,
			LeaseDuration: 60 * time.Second,
		})
	if err != nil {
		t.Fatal(err)
	}
	list := requestRegion[RegionList](t, handler, http.MethodGet, "/api/v1/regions", "", http.StatusOK)
	if len(list.Regions) != 1 || list.Regions[0].ID != created.ID {
		t.Fatalf("unexpected discovery response: %#v", list)
	}

	requestRegion[regions.Region](t, handler, http.MethodGet,
		"/api/v1/regions/"+created.ID, "", http.StatusOK)
	methodError := requestRegion[Error](t, handler, http.MethodPost, "/api/v1/regions", `{}`, http.StatusMethodNotAllowed)
	if methodError.Code != "method_not_allowed" {
		t.Fatalf("mutation response = %#v", methodError)
	}
}

func TestRegionNeighborDiscoveryReturnsSurroundingLiveRegions(t *testing.T) {
	store := newMemoryRegionStore()
	handler := New(checker{}, "test", Options{ServiceToken: "secret", Regions: store})
	centerID := "11111111-1111-4111-8111-111111111111"
	registrations := []struct {
		id   string
		name string
		x    int
		y    int
	}{
		{centerID, "Center", 1000, 1000},
		{"22222222-2222-4222-8222-222222222222", "North", 1000, 1001},
		{"33333333-3333-4333-8333-333333333333", "East", 1001, 1000},
		{"44444444-4444-4444-8444-444444444444", "South", 1000, 999},
		{"55555555-5555-4555-8555-555555555555", "West", 999, 1000},
		{"66666666-6666-4666-8666-666666666666", "Diagonal", 1001, 1001},
	}
	for _, registration := range registrations {
		_, err := store.RegisterProvisioned(context.Background(), registration.id, regions.Registration{
			Name: registration.name, GridX: registration.x, GridY: registration.y,
			PublicEndpoint: "http://" + registration.name + ".example:42001", ViewerPort: 42002,
			LeaseDuration: 60 * time.Second,
		})
		if err != nil {
			t.Fatal(err)
		}
	}
	store.regions["77777777-7777-4777-8777-777777777777"] = regions.Region{
		ID: "77777777-7777-4777-8777-777777777777", Name: "Expired North",
		GridX: 1000, GridY: 1001, LeaseExpiresAt: store.now.Add(-time.Second),
	}

	// Eight surrounding regions, not four. A viewer holds every region it can
	// see, and a corner left out stays dark on the minimap even while both
	// parties can see the region between them.
	//
	// The corner here is offered because its flanking edges are occupied: North
	// and East both exist, so Diagonal is genuinely the third region meeting at
	// that point rather than one across a void.
	response := requestRegion[RegionNeighborList](t, handler, http.MethodGet,
		"/api/v1/regions/"+centerID+"/neighbors", "", http.StatusOK)
	wantDirections := []string{"north", "east", "south", "west", "northeast"}
	wantNames := []string{"North", "East", "South", "West", "Diagonal"}
	if len(response.Neighbors) != len(wantDirections) {
		t.Fatalf("neighbors = %#v", response.Neighbors)
	}
	for index, neighbor := range response.Neighbors {
		if neighbor.Direction != wantDirections[index] || neighbor.Region.Name != wantNames[index] ||
			!neighbor.Region.Online || neighbor.Region.SizeX != 256 || neighbor.Region.SizeY != 256 ||
			neighbor.Region.Maturity != 0 {
			t.Fatalf("neighbor %d = %#v", index, neighbor)
		}
	}

	// A corner with both flanking edges empty is across a gap, not around a
	// corner, and announcing it would put a region on the minimap that nothing
	// connects to. Sole is diagonal from Island and has no edge neighbour.
	islandID := "a1111111-1111-4111-8111-111111111111"
	for _, isolated := range []struct {
		id   string
		name string
		x    int
		y    int
	}{
		{islandID, "Island", 2000, 2000},
		{"a2222222-2222-4222-8222-222222222222", "Sole", 2001, 2001},
	} {
		if _, err := store.RegisterProvisioned(context.Background(), isolated.id, regions.Registration{
			Name: isolated.name, GridX: isolated.x, GridY: isolated.y,
			PublicEndpoint: "http://" + isolated.name + ".example:42001", ViewerPort: 42002,
			LeaseDuration: 60 * time.Second,
		}); err != nil {
			t.Fatal(err)
		}
	}
	isolatedNeighbors := requestRegion[RegionNeighborList](t, handler, http.MethodGet,
		"/api/v1/regions/"+islandID+"/neighbors", "", http.StatusOK)
	if len(isolatedNeighbors.Neighbors) != 0 {
		t.Fatalf("neighbors across a gap = %#v", isolatedNeighbors.Neighbors)
	}

	methodError := requestRegion[Error](t, handler, http.MethodPost,
		"/api/v1/regions/"+centerID+"/neighbors", `{}`, http.StatusMethodNotAllowed)
	if methodError.Code != "method_not_allowed" {
		t.Fatalf("mutation response = %#v", methodError)
	}
	missing := requestRegion[Error](t, handler, http.MethodGet,
		"/api/v1/regions/88888888-8888-4888-8888-888888888888/neighbors", "", http.StatusNotFound)
	if missing.Code != "region_not_found" {
		t.Fatalf("missing response = %#v", missing)
	}
}

func TestRegionNeighborDiscoveryIncludesOfflineProvisionedState(t *testing.T) {
	path := filepath.Join(t.TempDir(), "regions.json")
	if err := os.WriteFile(path, []byte(`[
  {"id":"11111111-1111-4111-8111-111111111111","name":"Center","mapX":1000,"mapY":1000,"accessKey":"center-key"},
  {"id":"22222222-2222-4222-8222-222222222222","name":"Offline East","mapX":1001,"mapY":1000,"publicEndpoint":"https://east.example/region","viewerPort":42022,"accessKey":"east-key"}
]`), 0600); err != nil {
		t.Fatal(err)
	}
	registry, err := provisioning.Load(path)
	if err != nil {
		t.Fatal(err)
	}
	store := newMemoryRegionStore()
	_, err = store.RegisterProvisioned(context.Background(), "11111111-1111-4111-8111-111111111111",
		regions.Registration{Name: "Center", GridX: 1000, GridY: 1000,
			PublicEndpoint: "http://center.example:42001", ViewerPort: 42002, LeaseDuration: time.Minute})
	if err != nil {
		t.Fatal(err)
	}
	handler := New(checker{}, "test", Options{ServiceToken: "secret", Regions: store, Provisioned: registry})
	response := requestRegion[RegionNeighborList](t, handler, http.MethodGet,
		"/api/v1/regions/11111111-1111-4111-8111-111111111111/neighbors", "", http.StatusOK)
	if len(response.Neighbors) != 1 || response.Neighbors[0].Direction != "east" ||
		response.Neighbors[0].Region.Name != "Offline East" || response.Neighbors[0].Region.Online ||
		response.Neighbors[0].Region.PublicEndpoint != "https://east.example/region" ||
		response.Neighbors[0].Region.ViewerPort != 42022 {
		t.Fatalf("offline topology response = %#v", response)
	}
}

func TestMapTileIsPublicForAnOnlineRegion(t *testing.T) {
	store := newMemoryRegionStore()
	_, err := store.RegisterProvisioned(context.Background(),
		"11111111-1111-4111-8111-111111111111", regions.Registration{
			Name: "Welcome", GridX: 1000, GridY: 1000,
			PublicEndpoint: "http://region.example:42001", ViewerPort: 42002,
			LeaseDuration: 60 * time.Second,
		})
	if err != nil {
		t.Fatal(err)
	}
	_, err = store.RegisterProvisioned(context.Background(),
		"22222222-2222-4222-8222-222222222222", regions.Registration{
			Name: "Sandbox", GridX: 1001, GridY: 1000,
			PublicEndpoint: "http://region.example:42011", ViewerPort: 42012,
			LeaseDuration: 60 * time.Second,
		})
	if err != nil {
		t.Fatal(err)
	}
	handler := New(checker{}, "test", Options{ServiceToken: "secret", Regions: store})
	request := httptest.NewRequest(http.MethodGet, "/map/map-1-1000-1000-objects.jpg", nil)
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	if response.Code != http.StatusOK || response.Header().Get("Content-Type") != "image/jpeg" ||
		response.Body.Len() == 0 {
		t.Fatalf("map response = %d, %q, %d bytes", response.Code,
			response.Header().Get("Content-Type"), response.Body.Len())
	}
	request = httptest.NewRequest(http.MethodGet, "/map/map-2-1000-1000-objects.jpg", nil)
	response = httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	if response.Code != http.StatusOK || response.Header().Get("Content-Type") != "image/jpeg" ||
		response.Body.Len() == 0 {
		t.Fatalf("composite map response = %d, %q, %d bytes", response.Code,
			response.Header().Get("Content-Type"), response.Body.Len())
	}

	request = httptest.NewRequest(http.MethodGet, "/map/map-2-1002-1000-objects.jpg", nil)
	response = httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	if response.Code != http.StatusNotFound {
		t.Fatalf("offline coordinate status = %d, want 404", response.Code)
	}
}

func TestRegionRegistrationValidationAndAuthentication(t *testing.T) {
	store := newMemoryRegionStore()
	handler := New(checker{}, "test", Options{ServiceToken: "secret", Regions: store})

	r := httptest.NewRequest(http.MethodGet, "/api/v1/regions", nil)
	w := httptest.NewRecorder()
	handler.ServeHTTP(w, r)
	if w.Code != http.StatusUnauthorized {
		t.Fatalf("unauthenticated status = %d, want 401", w.Code)
	}

	errorResponse := requestRegion[Error](t, handler, http.MethodPost, "/api/v1/regions", `{}`,
		http.StatusMethodNotAllowed)
	if errorResponse.Code != "method_not_allowed" {
		t.Fatalf("validation code = %q", errorResponse.Code)
	}
}

func TestProvisionedRegionRegistrationUsesPerRegionCredentials(t *testing.T) {
	path := filepath.Join(t.TempDir(), "regions.json")
	if err := os.WriteFile(path, []byte(`[
  {"id":"11111111-1111-4111-8111-111111111111","name":"Welcome","mapX":1000,"mapY":1000,"accessKey":"welcome-key"},
  {"id":"22222222-2222-4222-8222-222222222222","name":"Sandbox","ownerUserId":"33333333-3333-4333-8333-333333333333","mapX":1001,"mapY":1000,"publicEndpoint":"https://sandbox.example/region","viewerPort":43002,"accessKey":"sandbox-key"}
]`), 0600); err != nil {
		t.Fatal(err)
	}
	registry, err := provisioning.Load(path)
	if err != nil {
		t.Fatal(err)
	}
	store := newMemoryRegionStore()
	handler := New(checker{}, "test", Options{ServiceToken: "grid-secret", GridName: "Homeworldz Test",
		GridPublicURL: "https://grid.example", Regions: store, Provisioned: registry})

	request := httptest.NewRequest(http.MethodPost,
		"/api/v1/region-runtime/Sandbox",
		bytes.NewBufferString(`{"publicEndpoint":"http://127.0.0.1:42011","viewerPort":42012,"leaseSeconds":60}`))
	request.Header.Set("Authorization", "Bearer sandbox-key")
	request.Header.Set("Content-Type", "application/json")
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("registration status = %d: %s", response.Code, response.Body.String())
	}
	var registered ProvisionedRegionRuntimeResult
	if err := json.NewDecoder(response.Body).Decode(&registered); err != nil {
		t.Fatal(err)
	}
	if registered.ID != "22222222-2222-4222-8222-222222222222" || registered.Name != "Sandbox" ||
		registered.GridX != 1001 || registered.GridY != 1000 ||
		registered.SizeX != 256 || registered.SizeY != 256 || registered.Maturity != 0 ||
		registered.PublicEndpoint != "https://sandbox.example/region" || registered.ViewerPort != 43002 ||
		registered.GridName != "Homeworldz Test" || registered.GridPublicURL != "https://grid.example" ||
		registered.OwnerUserID != "33333333-3333-4333-8333-333333333333" {
		t.Fatalf("unexpected registered region: %#v", registered)
	}

	request = httptest.NewRequest(http.MethodPut,
		"/api/v1/region-runtime/22222222-2222-4222-8222-222222222222/lease",
		bytes.NewBufferString(`{"leaseSeconds":120}`))
	request.Header.Set("Authorization", "Bearer sandbox-key")
	request.Header.Set("Content-Type", "application/json")
	response = httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("renewal status = %d: %s", response.Code, response.Body.String())
	}

	request = httptest.NewRequest(http.MethodPost,
		"/api/v1/region-runtime/11111111-1111-4111-8111-111111111111",
		bytes.NewBufferString(`{"publicEndpoint":"http://127.0.0.1:42001","leaseSeconds":60}`))
	request.Header.Set("Authorization", "Bearer wrong-key")
	response = httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	if response.Code != http.StatusUnauthorized {
		t.Fatalf("wrong key status = %d, want 401", response.Code)
	}

	request = httptest.NewRequest(http.MethodDelete,
		"/api/v1/region-runtime/22222222-2222-4222-8222-222222222222", nil)
	request.Header.Set("Authorization", "Bearer sandbox-key")
	response = httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	if response.Code != http.StatusNoContent {
		t.Fatalf("deregistration status = %d: %s", response.Code, response.Body.String())
	}
	if retained, ok := store.regions[registered.ID]; !ok || retained.LeaseExpiresAt.After(store.now) {
		t.Fatalf("provisioned shutdown removed identity or left lease online: %#v", retained)
	}
}

func TestVariableExtentTopologyAllowsMultipleNeighborsPerBorder(t *testing.T) {
	path := filepath.Join(t.TempDir(), "regions.json")
	if err := os.WriteFile(path, []byte(`[
  {"id":"11111111-1111-4111-8111-111111111111","name":"Large Center","mapX":1000,"mapY":1000,"size":2,"accessKey":"center-key"},
  {"id":"22222222-2222-4222-8222-222222222222","name":"East South","mapX":1002,"mapY":1000,"size":1,"accessKey":"south-key"},
  {"id":"33333333-3333-4333-8333-333333333333","name":"East North","mapX":1002,"mapY":1001,"size":1,"accessKey":"north-key"}
]`), 0600); err != nil {
		t.Fatal(err)
	}
	registry, err := provisioning.Load(path)
	if err != nil {
		t.Fatal(err)
	}
	store := newMemoryRegionStore()
	_, err = store.RegisterProvisioned(context.Background(), "11111111-1111-4111-8111-111111111111",
		regions.Registration{Name: "Large Center", GridX: 1000, GridY: 1000,
			PublicEndpoint: "http://center.example:42001", ViewerPort: 42002, LeaseDuration: time.Minute})
	if err != nil {
		t.Fatal(err)
	}
	handler := New(checker{}, "test", Options{ServiceToken: "secret", Regions: store, Provisioned: registry})
	response := requestRegion[RegionNeighborList](t, handler, http.MethodGet,
		"/api/v1/regions/11111111-1111-4111-8111-111111111111/neighbors", "", http.StatusOK)
	if len(response.Neighbors) != 2 || response.Neighbors[0].Direction != "east" ||
		response.Neighbors[1].Direction != "east" || response.Neighbors[0].Region.GridY != 1000 ||
		response.Neighbors[1].Region.GridY != 1001 {
		t.Fatalf("variable extent topology = %#v", response.Neighbors)
	}
}

func TestProvisionedRegionManagementLifecycle(t *testing.T) {
	path := filepath.Join(t.TempDir(), "regions.json")
	if err := os.WriteFile(path, []byte("[]\n"), 0600); err != nil {
		t.Fatal(err)
	}
	registry, err := provisioning.Load(path)
	if err != nil {
		t.Fatal(err)
	}
	handler := New(checker{}, "test", Options{ServiceToken: "secret", Provisioned: registry})
	id := "11111111-1111-4111-8111-111111111111"
	created := requestRegion[ProvisionedRegionResult](t, handler, http.MethodPost,
		"/api/v1/provisioned-regions",
		`{"id":"`+id+`","name":"Welcome","mapX":1000,"mapY":1000,"size":2,"maturity":1,`+
			`"publicEndpoint":"https://welcome.example/region","viewerPort":42012}`,
		http.StatusCreated)
	if created.Region.ID != id || created.Region.Name != "Welcome" || !created.Region.Enabled ||
		created.Region.SizeX != 2 || created.Region.SizeY != 2 || created.Region.Maturity != 1 ||
		created.Region.PublicEndpoint != "https://welcome.example/region" || created.Region.ViewerPort != 42012 ||
		len(created.AccessKey) != 64 {
		t.Fatalf("created provisioned region = %#v", created)
	}
	listed := requestRegion[ProvisionedRegionList](t, handler, http.MethodGet,
		"/api/v1/provisioned-regions", "", http.StatusOK)
	if len(listed.Regions) != 1 || listed.Regions[0].AccessKey != "" {
		t.Fatalf("listed provisioned regions = %#v", listed)
	}

	updated := requestRegion[ProvisionedRegionResult](t, handler, http.MethodPatch,
		"/api/v1/provisioned-regions/"+id,
		`{"name":"Welcome Region","mapX":1002,"enabled":false}`,
		http.StatusOK)
	if updated.Region.Name != "Welcome Region" || updated.Region.MapX != 1002 || updated.Region.Enabled ||
		updated.AccessKey != "" {
		t.Fatalf("updated provisioned region = %#v", updated)
	}
	rotated := requestRegion[ProvisionedRegionResult](t, handler, http.MethodPost,
		"/api/v1/provisioned-regions/"+id+"/rotate-access-key", "{}", http.StatusOK)
	if len(rotated.AccessKey) != 64 || rotated.AccessKey == created.AccessKey {
		t.Fatalf("rotated access key was not returned exactly once: %#v", rotated)
	}
	// The rotated key identifies the region even while it is disabled, and the
	// runtime endpoint is what enforces the difference: it may deregister and
	// nothing else.
	if _, ok, _ := registry.Authenticate(context.Background(), id, rotated.AccessKey); !ok {
		t.Fatal("disabled region should still authenticate with its rotated key")
	}

	requestRegion[struct{}](t, handler, http.MethodDelete,
		"/api/v1/provisioned-regions/"+id, "", http.StatusNoContent)
	missing := requestRegion[Error](t, handler, http.MethodGet,
		"/api/v1/provisioned-regions/"+id, "", http.StatusNotFound)
	if missing.Code != "provisioned_region_not_found" {
		t.Fatalf("missing response = %#v", missing)
	}
	reloaded, err := provisioning.Load(path)
	items, listErr := reloaded.List(context.Background())
	if err != nil || listErr != nil || len(items) != 0 {
		t.Fatalf("deleted configuration was not persisted: %#v, %v, %v", items, err, listErr)
	}
}

func requestRegion[T any](t *testing.T, handler http.Handler, method, path, body string, wantStatus int) T {
	t.Helper()
	r := httptest.NewRequest(method, path, bytes.NewBufferString(body))
	r.Header.Set("Authorization", "Bearer secret")
	if body != "" {
		r.Header.Set("Content-Type", "application/json")
	}
	w := httptest.NewRecorder()
	handler.ServeHTTP(w, r)
	if w.Code != wantStatus {
		t.Fatalf("%s %s status = %d, want %d: %s", method, path, w.Code, wantStatus, w.Body.String())
	}
	var value T
	if wantStatus == http.StatusNoContent {
		return value
	}
	if err := json.NewDecoder(w.Body).Decode(&value); err != nil {
		t.Fatalf("decode %s %s response: %v", method, path, err)
	}
	return value
}

func TestRegionLookupResolvesDestinationsAnywhereOnTheGrid(t *testing.T) {
	// The failure this endpoint exists for: teleporting to a region that is
	// nowhere near the one you are standing in. Neighbors cannot answer that.
	path := filepath.Join(t.TempDir(), "regions.json")
	if err := os.WriteFile(path, []byte(`[
  {"id":"11111111-1111-4111-8111-111111111111","name":"Welcome","mapX":1000,"mapY":1000,"accessKey":"welcome-key"},
  {"id":"22222222-2222-4222-8222-222222222222","name":"Gamma","mapX":1004,"mapY":1000,"size":2,"publicEndpoint":"https://gamma.example/region","viewerPort":42042,"accessKey":"gamma-key"}
]`), 0600); err != nil {
		t.Fatal(err)
	}
	registry, err := provisioning.Load(path)
	if err != nil {
		t.Fatal(err)
	}
	store := newMemoryRegionStore()
	if _, err := store.RegisterProvisioned(context.Background(), "11111111-1111-4111-8111-111111111111",
		regions.Registration{Name: "Welcome", GridX: 1000, GridY: 1000,
			PublicEndpoint: "http://welcome.example:42001", ViewerPort: 42002,
			LeaseDuration: time.Minute}); err != nil {
		t.Fatal(err)
	}
	handler := New(checker{}, "test", Options{ServiceToken: "secret", Regions: store, Provisioned: registry})

	live := requestRegion[RegionTopology](t, handler, http.MethodGet,
		"/api/v1/regions/lookup?gridX=1000&gridY=1000", "", http.StatusOK)
	if live.ID != "11111111-1111-4111-8111-111111111111" || !live.Online ||
		live.PublicEndpoint != "http://welcome.example:42001" || live.ViewerPort != 42002 {
		t.Fatalf("live lookup = %#v", live)
	}

	// A var region answers for every square it covers, not only its corner,
	// and an offline one is reported as placed-but-down rather than absent.
	inner := requestRegion[RegionTopology](t, handler, http.MethodGet,
		"/api/v1/regions/lookup?gridX=1005&gridY=1001", "", http.StatusOK)
	if inner.Name != "Gamma" || inner.Online || inner.SizeX != 512 || inner.SizeY != 512 {
		t.Fatalf("var-region lookup = %#v", inner)
	}
	byID := requestRegion[RegionTopology](t, handler, http.MethodGet,
		"/api/v1/regions/lookup?id=22222222-2222-4222-8222-222222222222", "", http.StatusOK)
	if byID.Name != "Gamma" || byID.GridX != 1004 || byID.GridY != 1000 {
		t.Fatalf("id lookup = %#v", byID)
	}

	empty := requestRegion[Error](t, handler, http.MethodGet,
		"/api/v1/regions/lookup?gridX=1003&gridY=1000", "", http.StatusNotFound)
	if empty.Code != "region_not_found" {
		t.Fatalf("empty square = %#v", empty)
	}
	for _, query := range []string{"", "?id=not-a-uuid", "?gridX=1000",
		"?gridX=1000&gridY=-1", "?id=11111111-1111-4111-8111-111111111111&gridX=1000&gridY=1000"} {
		invalid := requestRegion[Error](t, handler, http.MethodGet,
			"/api/v1/regions/lookup"+query, "", http.StatusBadRequest)
		if invalid.Code != "invalid_lookup" {
			t.Fatalf("lookup%q = %#v", query, invalid)
		}
	}
	methodError := requestRegion[Error](t, handler, http.MethodPost,
		"/api/v1/regions/lookup", `{}`, http.StatusMethodNotAllowed)
	if methodError.Code != "method_not_allowed" {
		t.Fatalf("mutation response = %#v", methodError)
	}
}

func TestRegionTopologyListsThePlacedGrid(t *testing.T) {
	path := filepath.Join(t.TempDir(), "regions.json")
	if err := os.WriteFile(path, []byte(`[
  {"id":"11111111-1111-4111-8111-111111111111","name":"Welcome","mapX":1000,"mapY":1000,"accessKey":"welcome-key"},
  {"id":"22222222-2222-4222-8222-222222222222","name":"Gamma","mapX":1004,"mapY":1000,"accessKey":"gamma-key"}
]`), 0600); err != nil {
		t.Fatal(err)
	}
	registry, err := provisioning.Load(path)
	if err != nil {
		t.Fatal(err)
	}
	store := newMemoryRegionStore()
	if _, err := store.RegisterProvisioned(context.Background(), "11111111-1111-4111-8111-111111111111",
		regions.Registration{Name: "Welcome", GridX: 1000, GridY: 1000,
			PublicEndpoint: "http://welcome.example:42001", ViewerPort: 42002,
			LeaseDuration: time.Minute}); err != nil {
		t.Fatal(err)
	}
	handler := New(checker{}, "test", Options{ServiceToken: "secret", Regions: store, Provisioned: registry})

	// Both regions, whether or not they hold a lease and whether or not they
	// are anywhere near each other: a map that omitted either would be wrong.
	response := requestRegion[RegionTopologyList](t, handler, http.MethodGet,
		"/api/v1/regions/topology", "", http.StatusOK)
	if len(response.Regions) != 2 || response.Regions[0].Name != "Welcome" ||
		!response.Regions[0].Online || response.Regions[1].Name != "Gamma" ||
		response.Regions[1].Online || response.Regions[1].GridX != 1004 {
		t.Fatalf("topology = %#v", response.Regions)
	}
	methodError := requestRegion[Error](t, handler, http.MethodPost,
		"/api/v1/regions/topology", `{}`, http.StatusMethodNotAllowed)
	if methodError.Code != "method_not_allowed" {
		t.Fatalf("mutation response = %#v", methodError)
	}
}

// A disabled region is switched off, not disowned. It must still be able to
// hand back the lease it holds, because a provisioned region's lease row
// reserves its map coordinates and is not purged when it expires: a disabled
// region that cannot deregister leaves that reservation standing, and the next
// region provisioned on those coordinates cannot register at all.
func TestDisabledRegionMayOnlyReleaseItsLease(t *testing.T) {
	path := filepath.Join(t.TempDir(), "regions.json")
	if err := os.WriteFile(path, []byte(`[
  {"id":"33333333-3333-4333-8333-333333333333","name":"Retired","mapX":900,"mapY":900,"enabled":false,"viewerPort":42102,"accessKey":"retired-key"}
]`), 0600); err != nil {
		t.Fatal(err)
	}
	registry, err := provisioning.Load(path)
	if err != nil {
		t.Fatal(err)
	}
	store := newMemoryRegionStore()
	if _, err := store.RegisterProvisioned(context.Background(), "33333333-3333-4333-8333-333333333333",
		regions.Registration{Name: "Retired", GridX: 900, GridY: 900,
			PublicEndpoint: "http://retired.example:42101", ViewerPort: 42102,
			LeaseDuration: time.Minute}); err != nil {
		t.Fatal(err)
	}
	handler := New(checker{}, "test", Options{ServiceToken: "secret", GridName: "Test",
		GridPublicURL: "https://grid.example", Regions: store, Provisioned: registry})

	call := func(method, key, body string) *httptest.ResponseRecorder {
		var reader *bytes.Buffer
		if body == "" {
			reader = bytes.NewBufferString("")
		} else {
			reader = bytes.NewBufferString(body)
		}
		request := httptest.NewRequest(method,
			"/api/v1/region-runtime/33333333-3333-4333-8333-333333333333", reader)
		request.Header.Set("Authorization", "Bearer "+key)
		request.Header.Set("Content-Type", "application/json")
		response := httptest.NewRecorder()
		handler.ServeHTTP(response, request)
		return response
	}

	// Running is refused, and refused as "disabled" rather than as a bad
	// credential: the two send an operator to entirely different places.
	start := call(http.MethodPost, "retired-key", `{"publicEndpoint":"http://retired.example:42101","viewerPort":42102,"leaseSeconds":60}`)
	if start.Code != http.StatusForbidden {
		t.Fatalf("disabled region start = %d, want 403: %s", start.Code, start.Body.String())
	}
	var refusal Error
	if err := json.Unmarshal(start.Body.Bytes(), &refusal); err != nil || refusal.Code != "region_disabled" {
		t.Fatalf("refusal = %#v (%v)", refusal, err)
	}
	// A wrong key is still a wrong key.
	if bad := call(http.MethodPost, "not-the-key", "{}"); bad.Code != http.StatusUnauthorized {
		t.Fatalf("wrong key = %d, want 401", bad.Code)
	}
	// Releasing the lease is the one thing it may do.
	if release := call(http.MethodDelete, "retired-key", ""); release.Code != http.StatusNoContent {
		t.Fatalf("disabled region deregister = %d, want 204", release.Code)
	}
	live, err := store.Get(context.Background(), "33333333-3333-4333-8333-333333333333")
	if err == nil && live.LeaseExpiresAt.After(time.Now()) {
		t.Fatalf("lease still held after deregistration: %#v", live)
	}
}
