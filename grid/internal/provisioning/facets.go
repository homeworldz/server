package provisioning

import "strconv"

// Facet math for ADR 0036. A region of any shape is presented to viewers as a
// grid of 256 m facets, one per map tile. Firestorm keeps a single world-wide
// region width, set by whichever region it added last, and one neighbour slot
// per side; a 512 m facet beside 256 m regions broke both, and a viewer told
// to drop Welcome after dropping its 512 m neighbour crashed (2026-10-15). A
// world of 256 m regions is the only one every viewer agrees about.
//
// Facets are ordered row by row from the region's map corner: eastward along
// the southern row, then the row north of it. Facet 0 is always the corner
// tile, and a one-tile-high rectangle numbers its facets as it always has.

// FacetEdge is the edge of each facet in tiles.
func (r Region) FacetEdge() int {
	return 1
}

// FacetCount is how many facets tile the region.
func (r Region) FacetCount() int {
	if r.SizeX <= 0 || r.SizeY <= 0 {
		return 1
	}
	return r.SizeX * r.SizeY
}

// FacetOrigin is the map-tile coordinate of facet index's southwest corner.
func (r Region) FacetOrigin(index int) (int, int) {
	columns := r.SizeX
	if columns <= 0 {
		columns = 1
	}
	return r.MapX + index%columns, r.MapY + index/columns
}

// FacetNameAt names a facet. Facet 0 carries the region's own name; the rest
// use the names given at provisioning, in facet order, and any facet beyond
// those is the region's name and its facet number, the pattern every
// provisioned rectangle has used.
func (r Region) FacetNameAt(index int) string {
	if index <= 0 {
		return r.Name
	}
	if index <= len(r.FacetNames) {
		return r.FacetNames[index-1]
	}
	return r.Name + " " + strconv.Itoa(index+1)
}

// FacetViewerPort is the viewer UDP port of a facet: consecutive ports upward
// from the region's base port, in facet order. The registration reply carries
// each facet's port explicitly, so the region binds what the grid advertises.
func (r Region) FacetViewerPort(index int) int {
	return r.ViewerPort + index
}

// FacetAtTile reports which facet covers a map tile, if any.
func (r Region) FacetAtTile(gridX, gridY int) (int, bool) {
	if gridX < r.MapX || gridX >= r.MapX+r.SizeX ||
		gridY < r.MapY || gridY >= r.MapY+r.SizeY {
		return 0, false
	}
	return (gridY-r.MapY)*r.SizeX + (gridX - r.MapX), true
}

// FacetAtPosition reports which facet contains a region-local position in
// metres, clamping positions outside the region onto its nearest facet so a
// caller resolving a slightly out-of-bounds arrival point still gets a facet.
// Both axes are clamped: a position north of the southern row is in a northern
// facet, and choosing wrongly starts a login on a facet that does not hold the
// spawn point, which crossed the viewer mid-login (2026-08-20).
func (r Region) FacetAtPosition(x, y float64) int {
	column := clampTile(x, r.SizeX)
	row := clampTile(y, r.SizeY)
	return row*r.SizeX + column
}

func clampTile(metres float64, tiles int) int {
	if tiles <= 1 || metres < 0 {
		return 0
	}
	tile := int(metres / 256)
	if tile >= tiles {
		return tiles - 1
	}
	return tile
}
