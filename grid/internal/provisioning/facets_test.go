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
	// One tile high: facets run eastward exactly as they always have.
	region := Region{Name: "West End", MapX: 1000, MapY: 2000, SizeX: 4, SizeY: 1,
		FacetNames: []string{"East End"}, ViewerPort: 42002}
	if region.FacetEdge() != 1 || region.FacetCount() != 4 {
		t.Fatalf("edge/count = %d/%d, want 1/4", region.FacetEdge(), region.FacetCount())
	}
	x1, y1 := region.FacetOrigin(1)
	x3, y3 := region.FacetOrigin(3)
	if x1 != 1001 || y1 != 2000 || x3 != 1003 || y3 != 2000 {
		t.Fatalf("origins = (%d,%d),(%d,%d)", x1, y1, x3, y3)
	}
	if region.FacetNameAt(0) != "West End" || region.FacetNameAt(1) != "East End" ||
		region.FacetNameAt(2) != "West End 3" {
		t.Fatalf("names = %q,%q,%q", region.FacetNameAt(0), region.FacetNameAt(1), region.FacetNameAt(2))
	}
	if region.FacetViewerPort(3) != 42005 {
		t.Fatalf("facet 3 port = %d", region.FacetViewerPort(3))
	}
	if facet, ok := region.FacetAtTile(1002, 2000); !ok || facet != 2 {
		t.Fatalf("tile (1002,2000) facet = %d/%v", facet, ok)
	}
	if _, ok := region.FacetAtTile(1004, 2000); ok {
		t.Fatal("tile east of the region resolved to a facet")
	}
	if facet := region.FacetAtPosition(255.9, 100); facet != 0 {
		t.Fatalf("position x=255.9 facet = %d", facet)
	}
	if facet := region.FacetAtPosition(256, 100); facet != 1 {
		t.Fatalf("position x=256 facet = %d", facet)
	}
	// Out-of-bounds positions clamp onto the nearest facet, never out of range.
	if facet := region.FacetAtPosition(-5, 0); facet != 0 {
		t.Fatalf("clamped west facet = %d", facet)
	}
	if facet := region.FacetAtPosition(99999, 0); facet != 3 {
		t.Fatalf("clamped east facet = %d", facet)
	}
}

func TestFacetMathGrid(t *testing.T) {
	// A 4x2 - Homeworldz Strait's shape - is eight 256 m facets, row by row
	// from the map corner. As two 512 m facets it crashed Firestorm, which
	// keeps one region width for the whole world and one neighbour per side.
	region := Region{Name: "Strait", MapX: 1000, MapY: 998, SizeX: 4, SizeY: 2,
		FacetNames: []string{"Strait 2"}, ViewerPort: 42042}
	if region.FacetCount() != 8 {
		t.Fatalf("count = %d, want 8", region.FacetCount())
	}
	for index, want := range [][2]int{{1000, 998}, {1001, 998}, {1002, 998}, {1003, 998},
		{1000, 999}, {1001, 999}, {1002, 999}, {1003, 999}} {
		if x, y := region.FacetOrigin(index); x != want[0] || y != want[1] {
			t.Fatalf("facet %d origin = (%d,%d), want %v", index, x, y, want)
		}
		if facet, ok := region.FacetAtTile(want[0], want[1]); !ok || facet != index {
			t.Fatalf("tile %v facet = %d/%v, want %d", want, facet, ok, index)
		}
	}
	// A point in the northern row is in a northern facet. With the old
	// single-axis rule every northern point fell into the southern row, and a
	// login there would start on the wrong facet.
	if facet := region.FacetAtPosition(300, 400); facet != 5 {
		t.Fatalf("position (300,400) facet = %d, want 5", facet)
	}
	if facet := region.FacetAtPosition(1023, 511); facet != 7 {
		t.Fatalf("north-east corner facet = %d, want 7", facet)
	}
	if facet := region.FacetAtPosition(5000, 5000); facet != 7 {
		t.Fatalf("clamped beyond the north-east facet = %d", facet)
	}
	if region.FacetNameAt(1) != "Strait 2" || region.FacetNameAt(7) != "Strait 8" {
		t.Fatalf("names = %q,%q", region.FacetNameAt(1), region.FacetNameAt(7))
	}
	if region.FacetViewerPort(7) != 42049 {
		t.Fatalf("facet 7 port = %d", region.FacetViewerPort(7))
	}
}

func TestFacetMathVertical(t *testing.T) {
	region := Region{Name: "South", MapX: 50, MapY: 60, SizeX: 1, SizeY: 3,
		FacetNames: []string{"Middle", "North"}}
	if region.FacetCount() != 3 {
		t.Fatalf("count = %d", region.FacetCount())
	}
	x2, y2 := region.FacetOrigin(2)
	if x2 != 50 || y2 != 62 {
		t.Fatalf("facet 2 origin = (%d,%d)", x2, y2)
	}
	if facet := region.FacetAtPosition(100, 600); facet != 2 {
		t.Fatalf("position y=600 facet = %d", facet)
	}
	if region.FacetNameAt(2) != "North" {
		t.Fatalf("facet 2 name = %q", region.FacetNameAt(2))
	}
}

func TestFacetNamesValidated(t *testing.T) {
	base := Region{ID: "11111111-1111-4111-8111-111111111111", Name: "Long Field",
		MapX: 10, MapY: 10, SizeX: 2, SizeY: 1, AccessKey: "key"}
	// A facet without a name takes its derived one.
	if err := validate(base); err != nil {
		t.Fatalf("unnamed facet rejected: %v", err)
	}
	base.FacetNames = []string{"Long Field II"}
	if err := validate(base); err != nil {
		t.Fatalf("named rectangle rejected: %v", err)
	}
	base.FacetNames = []string{"long field"}
	if err := validate(base); !errors.Is(err, ErrConflict) {
		t.Fatalf("self-colliding facet name accepted: %v", err)
	}
	base.FacetNames = []string{"One", "Two"}
	if err := validate(base); !errors.Is(err, ErrInvalid) {
		t.Fatalf("more names than facets accepted: %v", err)
	}
	single := base
	single.SizeX = 1
	single.FacetNames = []string{"Extra"}
	if err := validate(single); !errors.Is(err, ErrInvalid) {
		t.Fatalf("one-tile region with a facet name accepted: %v", err)
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
