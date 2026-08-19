package provisioning

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"testing"
)

func TestShapeRule(t *testing.T) {
	valid := [][2]int{{1, 1}, {2, 2}, {4, 4}, {4, 8}, {8, 4}, {2, 6}, {1, 3}, {2, 10}, {4, 16}}
	for _, shape := range valid {
		if err := validateShape(shape[0], shape[1]); err != nil {
			t.Errorf("shape %dx%d rejected: %v", shape[0], shape[1], err)
		}
	}
	invalid := [][2]int{{2, 5}, {5, 2}, {8, 8}, {3, 3}, {3, 6}, {0, 1}, {1, 0}, {4, 6}}
	for _, shape := range invalid {
		if err := validateShape(shape[0], shape[1]); !errors.Is(err, ErrInvalid) {
			t.Errorf("shape %dx%d accepted", shape[0], shape[1])
		}
	}
}

func TestFacetMathHorizontal(t *testing.T) {
	region := Region{Name: "West End", MapX: 1000, MapY: 2000, SizeX: 8, SizeY: 4,
		FacetNames: []string{"East End"}, ViewerPort: 42002}
	if region.FacetEdge() != 4 || region.FacetCount() != 2 {
		t.Fatalf("edge/count = %d/%d, want 4/2", region.FacetEdge(), region.FacetCount())
	}
	x0, y0 := region.FacetOrigin(0)
	x1, y1 := region.FacetOrigin(1)
	if x0 != 1000 || y0 != 2000 || x1 != 1004 || y1 != 2000 {
		t.Fatalf("origins = (%d,%d),(%d,%d)", x0, y0, x1, y1)
	}
	if region.FacetNameAt(0) != "West End" || region.FacetNameAt(1) != "East End" {
		t.Fatalf("names = %q,%q", region.FacetNameAt(0), region.FacetNameAt(1))
	}
	if region.FacetViewerPort(1) != 42003 {
		t.Fatalf("facet 1 port = %d", region.FacetViewerPort(1))
	}
	if facet, ok := region.FacetAtTile(1005, 2003); !ok || facet != 1 {
		t.Fatalf("tile (1005,2003) facet = %d/%v", facet, ok)
	}
	if _, ok := region.FacetAtTile(1008, 2000); ok {
		t.Fatal("tile east of the region resolved to a facet")
	}
	if facet := region.FacetAtPosition(1023.9, 100); facet != 0 {
		t.Fatalf("position x=1023.9 facet = %d", facet)
	}
	if facet := region.FacetAtPosition(1024, 100); facet != 1 {
		t.Fatalf("position x=1024 facet = %d", facet)
	}
	// Out-of-bounds positions clamp onto the nearest facet, never out of range.
	if facet := region.FacetAtPosition(-5, 0); facet != 0 {
		t.Fatalf("clamped west facet = %d", facet)
	}
	if facet := region.FacetAtPosition(99999, 0); facet != 1 {
		t.Fatalf("clamped east facet = %d", facet)
	}
}

func TestFacetMathVertical(t *testing.T) {
	region := Region{Name: "South", MapX: 50, MapY: 60, SizeX: 2, SizeY: 6,
		FacetNames: []string{"Middle", "North"}}
	if region.FacetCount() != 3 || region.FacetEdge() != 2 {
		t.Fatalf("count/edge = %d/%d", region.FacetCount(), region.FacetEdge())
	}
	x2, y2 := region.FacetOrigin(2)
	if x2 != 50 || y2 != 64 {
		t.Fatalf("facet 2 origin = (%d,%d)", x2, y2)
	}
	if facet := region.FacetAtPosition(100, 1200); facet != 2 {
		t.Fatalf("position y=1200 facet = %d", facet)
	}
	if region.FacetNameAt(2) != "North" {
		t.Fatalf("facet 2 name = %q", region.FacetNameAt(2))
	}
}

func TestFacetNamesValidated(t *testing.T) {
	base := Region{ID: "11111111-1111-4111-8111-111111111111", Name: "Long Field",
		MapX: 10, MapY: 10, SizeX: 4, SizeY: 8, AccessKey: "key"}
	if err := validate(base); !errors.Is(err, ErrInvalid) {
		t.Fatalf("missing facet name accepted: %v", err)
	}
	base.FacetNames = []string{"Long Field II"}
	if err := validate(base); err != nil {
		t.Fatalf("named rectangle rejected: %v", err)
	}
	base.FacetNames = []string{"long field"}
	if err := validate(base); !errors.Is(err, ErrConflict) {
		t.Fatalf("self-colliding facet name accepted: %v", err)
	}
	square := base
	square.SizeY = 4
	square.FacetNames = []string{"Extra"}
	if err := validate(square); !errors.Is(err, ErrInvalid) {
		t.Fatalf("square with a facet name accepted: %v", err)
	}
}

func TestRegistryEnforcesFacetNameAndOverlapUniqueness(t *testing.T) {
	path := filepath.Join(t.TempDir(), "regions.json")
	if err := os.WriteFile(path, []byte("[]\n"), 0600); err != nil {
		t.Fatal(err)
	}
	registry, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	rectangle := Region{ID: "11111111-1111-4111-8111-111111111111", Name: "Shore",
		MapX: 1000, MapY: 1000, SizeX: 4, SizeY: 2, FacetNames: []string{"Cliffs"},
		Enabled: true, AccessKey: "key-one"}
	if _, err := registry.Create(context.Background(), rectangle); err != nil {
		t.Fatalf("create rectangle: %v", err)
	}
	// A facet name is a viewer-visible region name: a second region may not
	// claim it as its own name.
	clash := Region{ID: "22222222-2222-4222-8222-222222222222", Name: "cliffs",
		MapX: 2000, MapY: 2000, SizeX: 1, SizeY: 1, Enabled: true, AccessKey: "key-two"}
	if _, err := registry.Create(context.Background(), clash); !errors.Is(err, ErrConflict) {
		t.Fatalf("region named after a facet accepted: %v", err)
	}
	// The second facet's tiles are part of the rectangle's footprint.
	overlap := Region{ID: "33333333-3333-4333-8333-333333333333", Name: "Squatter",
		MapX: 1002, MapY: 1001, SizeX: 1, SizeY: 1, Enabled: true, AccessKey: "key-three"}
	if _, err := registry.Create(context.Background(), overlap); !errors.Is(err, ErrConflict) {
		t.Fatalf("overlap with the rectangle's far facet accepted: %v", err)
	}
	// Reload sees the same rectangle, proving persistence round-trips shape.
	reloaded, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	kept, err := reloaded.Get(context.Background(), rectangle.ID)
	if err != nil || kept.SizeX != 4 || kept.SizeY != 2 || len(kept.FacetNames) != 1 ||
		kept.FacetNames[0] != "Cliffs" {
		t.Fatalf("reloaded rectangle = %#v, %v", kept, err)
	}
}

func TestLoadAcceptsSquareShorthand(t *testing.T) {
	path := filepath.Join(t.TempDir(), "regions.json")
	content := `[
  {"id":"11111111-1111-4111-8111-111111111111","name":"Old","mapX":1,"mapY":1,"size":2,"accessKey":"k"}
]`
	if err := os.WriteFile(path, []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
	registry, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	region, err := registry.Get(context.Background(), "11111111-1111-4111-8111-111111111111")
	if err != nil || region.SizeX != 2 || region.SizeY != 2 {
		t.Fatalf("square shorthand = %#v, %v", region, err)
	}
}
