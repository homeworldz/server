package httpapi

import (
	"bytes"
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/homeworldz/server/grid/internal/provisioning"
	"github.com/homeworldz/server/grid/internal/regions"
)

// The grid's whole view of ADR 0036: a rectangular region is one identity that
// every discovery surface presents as a row of square facets.
func TestRectangularRegionPresentsAsFacets(t *testing.T) {
	path := filepath.Join(t.TempDir(), "regions.json")
	if err := os.WriteFile(path, []byte(`[
  {"id":"11111111-1111-4111-8111-111111111111","name":"Shore","mapX":1000,"mapY":1000,"sizeX":4,"sizeY":2,"facetNames":["Cliffs"],"viewerPort":42012,"accessKey":"shore-key"},
  {"id":"22222222-2222-4222-8222-222222222222","name":"Uplands","mapX":1004,"mapY":1000,"accessKey":"uplands-key"}
]`), 0600); err != nil {
		t.Fatal(err)
	}
	registry, err := provisioning.Load(path)
	if err != nil {
		t.Fatal(err)
	}
	store := newMemoryRegionStore()
	if _, err := store.RegisterProvisioned(context.Background(), "22222222-2222-4222-8222-222222222222",
		regions.Registration{Name: "Uplands", GridX: 1004, GridY: 1000,
			PublicEndpoint: "http://uplands.example:42001", ViewerPort: 42002,
			LeaseDuration: time.Minute}); err != nil {
		t.Fatal(err)
	}
	handler := New(checker{}, "test", Options{ServiceToken: "secret", GridName: "Test",
		GridPublicURL: "https://grid.example", Regions: store, Provisioned: registry})

	// Registration: the region learns its macro extent and its facet list.
	request := httptest.NewRequest(http.MethodPost,
		"/api/v1/region-runtime/Shore",
		bytes.NewBufferString(`{"publicEndpoint":"http://shore.example:42011","viewerPort":42012,"leaseSeconds":60}`))
	request.Header.Set("Authorization", "Bearer shore-key")
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
	if registered.SizeX != 1024 || registered.SizeY != 512 {
		t.Fatalf("macro extent = %dx%d, want 1024x512", registered.SizeX, registered.SizeY)
	}
	if len(registered.Facets) != 2 ||
		registered.Facets[0].Name != "Shore" || registered.Facets[0].GridX != 1000 ||
		registered.Facets[0].ViewerPort != 42012 || registered.Facets[0].Edge != 512 ||
		registered.Facets[1].Name != "Cliffs" || registered.Facets[1].GridX != 1002 ||
		registered.Facets[1].GridY != 1000 || registered.Facets[1].ViewerPort != 42013 {
		t.Fatalf("facet list = %#v", registered.Facets)
	}

	// Topology: one square entry per facet, sharing the region's id.
	topology := requestRegion[RegionTopologyList](t, handler, http.MethodGet,
		"/api/v1/regions/topology", "", http.StatusOK)
	if len(topology.Regions) != 3 {
		t.Fatalf("topology entries = %d, want 3 (two facets + Uplands)", len(topology.Regions))
	}
	first, second := topology.Regions[0], topology.Regions[1]
	if first.Name != "Shore" || first.Facet != 0 || first.SizeX != 512 || first.SizeY != 512 ||
		second.Name != "Cliffs" || second.Facet != 1 || second.GridX != 1002 ||
		second.ViewerPort != 42013 || second.ID != first.ID || !second.Online {
		t.Fatalf("facet topology = %#v, %#v", first, second)
	}

	// Lookup by point: a tile inside the far facet answers with that facet's
	// name, corner, and port — an ordinary square region to the caller.
	facet := requestRegion[RegionTopology](t, handler, http.MethodGet,
		"/api/v1/regions/lookup?gridX=1003&gridY=1001", "", http.StatusOK)
	if facet.Name != "Cliffs" || facet.GridX != 1002 || facet.GridY != 1000 ||
		facet.SizeX != 512 || facet.ViewerPort != 42013 {
		t.Fatalf("facet lookup = %#v", facet)
	}

	// Neighbors: Uplands sits east of the rectangle, which makes the facet
	// beside it — Cliffs, not Shore — its western neighbor.
	neighbors := requestRegion[RegionNeighborList](t, handler, http.MethodGet,
		"/api/v1/regions/22222222-2222-4222-8222-222222222222/neighbors", "", http.StatusOK)
	if len(neighbors.Neighbors) != 1 || neighbors.Neighbors[0].Direction != "west" ||
		neighbors.Neighbors[0].Region.Name != "Cliffs" ||
		neighbors.Neighbors[0].Region.ViewerPort != 42013 {
		t.Fatalf("neighbors of Uplands = %#v", neighbors.Neighbors)
	}
}

// A rectangle's own facets are internal to its process: they must never appear
// in its own neighbor list, which drives real (inter-process) crossings.
func TestRectangleIsNotItsOwnNeighbor(t *testing.T) {
	path := filepath.Join(t.TempDir(), "regions.json")
	if err := os.WriteFile(path, []byte(`[
  {"id":"11111111-1111-4111-8111-111111111111","name":"Shore","mapX":1000,"mapY":1000,"sizeX":4,"sizeY":2,"facetNames":["Cliffs"],"accessKey":"shore-key"}
]`), 0600); err != nil {
		t.Fatal(err)
	}
	registry, err := provisioning.Load(path)
	if err != nil {
		t.Fatal(err)
	}
	store := newMemoryRegionStore()
	if _, err := store.RegisterProvisioned(context.Background(), "11111111-1111-4111-8111-111111111111",
		regions.Registration{Name: "Shore", GridX: 1000, GridY: 1000,
			PublicEndpoint: "http://shore.example:42011", ViewerPort: 42012,
			LeaseDuration: time.Minute}); err != nil {
		t.Fatal(err)
	}
	handler := New(checker{}, "test", Options{ServiceToken: "secret", Regions: store, Provisioned: registry})
	neighbors := requestRegion[RegionNeighborList](t, handler, http.MethodGet,
		"/api/v1/regions/11111111-1111-4111-8111-111111111111/neighbors", "", http.StatusOK)
	if len(neighbors.Neighbors) != 0 {
		t.Fatalf("rectangle lists its own facets as neighbors: %#v", neighbors.Neighbors)
	}
}
