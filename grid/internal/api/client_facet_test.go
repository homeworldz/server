package api

import (
	"context"
	"testing"

	"github.com/homeworldz/server/grid/internal/provisioning"
	"github.com/homeworldz/server/grid/internal/regions"
)

// novaB is a 2x1 rectangle: one region, two facets, and only the region's own
// name reaches the lease table. "Nova B 2" is the second facet, and it is what
// a client crossing the border from the region above asks for.
func novaB() provisioning.Region {
	return provisioning.Region{
		ID: "4aa21394-7aa9-4294-bf3b-584dca7a62a3", Name: "Nova B",
		MapX: 899, MapY: 899, SizeX: 2, SizeY: 1, Enabled: true,
		FacetNames: []string{"Nova B 2"},
	}
}

// leasedRegions answers the lease store the way the real one does: Get finds
// only a region whose lease is live, which is what makes "named a facet of a
// region that is not running" distinguishable from "no such facet".
type leasedRegions struct{ items []regions.Region }

func (l leasedRegions) List(context.Context) ([]regions.Region, error) { return l.items, nil }

func (l leasedRegions) Get(_ context.Context, id string) (regions.Region, error) {
	for _, region := range l.items {
		if region.ID == id {
			return region, nil
		}
	}
	return regions.Region{}, regions.ErrNotFound
}

func (l leasedRegions) DeregisterProvisioned(context.Context, string) error { return nil }

func facetAPI(live bool) *API {
	api := &API{regions: &memoryRegionStore{items: []provisioning.Region{novaB()}}}
	if live {
		api.leases = leasedRegions{items: []regions.Region{{
			ID: novaB().ID, Name: "Nova B", GridX: 899, GridY: 899,
			PublicEndpoint: "http://region.example:42111",
		}}}
	} else {
		api.leases = leasedRegions{}
	}
	return api
}

// TestFacetNameResolvesToItsRegion covers the refusal seen in-world: a client
// crossing into "Nova B 2" was told the region was not online, because only
// "Nova B" is in the lease table.
func TestFacetNameResolvesToItsRegion(t *testing.T) {
	api := facetAPI(true)

	destination, facet, ok := api.resolveFacetNamed(context.Background(), "Nova B 2")
	if !ok || facet != 1 || destination.Region.ID != novaB().ID {
		t.Fatalf("facet lookup = %#v facet %d ok %v", destination.Region, facet, ok)
	}
	// Case-insensitively, the way every other name comparison here works.
	if _, _, ok := api.resolveFacetNamed(context.Background(), "nova b 2"); !ok {
		t.Fatal("facet names should match case-insensitively")
	}
	// A name that is nobody's facet stays unresolved rather than matching the
	// nearest region.
	if _, _, ok := api.resolveFacetNamed(context.Background(), "Nova B 3"); ok {
		t.Fatal("an unknown facet name resolved to something")
	}
	// Facet 0 carries the region's own name, which the lease lookup already
	// answers; resolving it here too would return facet 0 for a plain region
	// name and rebase nothing, which is harmless but means two answers for one
	// question.
	if _, _, ok := api.resolveFacetNamed(context.Background(), "Nova B"); ok {
		t.Fatal("the region's own name should be left to the lease lookup")
	}
}

// TestFacetOfAnOfflineRegionIsNotResolved: naming a facet of a region that is
// not running must not produce a destination. "Not online" is then the honest
// answer, rather than handing out a region nothing is serving.
func TestFacetOfAnOfflineRegionIsNotResolved(t *testing.T) {
	if _, _, ok := facetAPI(false).resolveFacetNamed(context.Background(), "Nova B 2"); ok {
		t.Fatal("a facet of an offline region resolved")
	}
}

// TestFacetPositionIsRebased: a position stated in a facet's own coordinates
// is in that facet, not that far into the region. Passed through unrebased,
// "Nova B 2/84/255/22" lands 84 m into facet 0 — the right region, the wrong
// place, which reads as a spawn bug rather than a coordinate one.
func TestFacetPositionIsRebased(t *testing.T) {
	region := novaB()
	position := &[3]float64{84, 255, 22}

	rebased := rebaseFacetPosition(region, 1, position)
	if rebased[0] != 84+256 || rebased[1] != 255 || rebased[2] != 22 {
		t.Fatalf("rebased = %v, want [340 255 22]", *rebased)
	}
	// Facet 0 is already in the region's frame, and so is a facet index the
	// region does not have.
	if same := rebaseFacetPosition(region, 0, position); same[0] != 84 {
		t.Fatalf("facet 0 was rebased: %v", *same)
	}
	if same := rebaseFacetPosition(region, 9, position); same[0] != 84 {
		t.Fatalf("an out-of-range facet was rebased: %v", *same)
	}
	if rebaseFacetPosition(region, 1, nil) != nil {
		t.Fatal("a position nobody stated must stay unstated")
	}
}
