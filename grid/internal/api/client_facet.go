package api

import (
	"context"
	"strings"

	"github.com/homeworldz/server/grid/internal/arrival"
	"github.com/homeworldz/server/grid/internal/provisioning"
)

// resolveFacetNamed finds a region by the name of one of its facets, and says
// which facet was named.
//
// A rectangular region presents to a client as a row of square facets, each
// with its own name, and each is a whole region as far as that client is
// concerned (ADR 0036). Nova B is one region called "Nova B" with a second
// facet called "Nova B 2"; only the region's own name is in the lease table.
// So a client asking for "Nova B 2" — which is exactly what a border crossing
// asks for, because the region beyond the line is a facet — was told the
// region it could see was not online (found in-world 2026-08-24: the crossing
// was refused and the session stayed where it started).
//
// Viewer login has resolved facet names since rectangles shipped. This is that
// same resolution for the client path, which never had it.
func (a *API) resolveFacetNamed(ctx context.Context, name string) (arrival.Destination, int, bool) {
	if a.regions == nil || a.leases == nil {
		return arrival.Destination{}, -1, false
	}
	defined, err := a.regions.List(ctx)
	if err != nil {
		return arrival.Destination{}, -1, false
	}
	trimmed := strings.TrimSpace(name)
	for _, region := range defined {
		// Facet 0 carries the region's own name and is already resolvable by
		// the lease lookup this falls back from, so only 1..N-1 are new here.
		for facet := 1; facet < region.FacetCount(); facet++ {
			if !strings.EqualFold(region.FacetNameAt(facet), trimmed) {
				continue
			}
			live, liveErr := a.leases.Get(ctx, region.ID)
			if liveErr != nil {
				// Named a facet of a region that is not running. "Not online"
				// is then the truthful answer rather than "no such place".
				return arrival.Destination{}, -1, false
			}
			return arrival.Destination{Region: live}, facet, true
		}
	}
	return arrival.Destination{}, -1, false
}

// rebaseFacetPosition converts a position stated in a named facet's own
// coordinates into the region's, which is the frame everything downstream
// works in.
//
// "Nova B 2/84/255/22" means 84 m into Nova B 2, and Nova B 2 begins 256 m
// along Nova B. Passed through unrebased it lands 84 m into facet 0 — a
// different facet of the right region, which looks like arriving in the wrong
// place rather than like a bug in name resolution. The viewer login path
// records making exactly this mistake and correcting it; this is the same
// correction on the client path.
func rebaseFacetPosition(region provisioning.Region, facet int, position *[3]float64) *[3]float64 {
	if position == nil || facet <= 0 || facet >= region.FacetCount() {
		return position
	}
	originX, originY := region.FacetOrigin(facet)
	return &[3]float64{
		position[0] + float64((originX-region.MapX)*256),
		position[1] + float64((originY-region.MapY)*256),
		position[2],
	}
}

// definedRegion reads the provisioned record for a live region, for the facet
// arithmetic above. Absent means "treat it as a plain square", which is what
// every region was before rectangles existed.
func (a *API) definedRegion(ctx context.Context, regionID string) (provisioning.Region, bool) {
	if a.regions == nil {
		return provisioning.Region{}, false
	}
	region, err := a.regions.Get(ctx, regionID)
	if err != nil {
		return provisioning.Region{}, false
	}
	return region, true
}
