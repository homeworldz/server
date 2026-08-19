package provisioning

// Facet math for ADR 0036. A rectangular region is presented to viewers as a
// row of square facets whose edge is the shorter dimension; the shape rule
// (shorter divides longer, shorter is a proven square size) guarantees the row
// tiles the rectangle exactly. Facets are ordered in map-coordinate order:
// eastward along a horizontal row, northward along a vertical one, so facet 0
// always sits at the region's own map corner.

// FacetEdge is the edge of each square facet in tiles: the shorter dimension.
func (r Region) FacetEdge() int {
	if r.SizeX < r.SizeY {
		return r.SizeX
	}
	return r.SizeY
}

// FacetCount is how many facets tile the region. A square region is one facet.
func (r Region) FacetCount() int {
	edge := r.FacetEdge()
	if edge <= 0 {
		return 1
	}
	longer := r.SizeX
	if r.SizeY > longer {
		longer = r.SizeY
	}
	return longer / edge
}

// FacetOrigin is the map-tile coordinate of facet index's southwest corner.
func (r Region) FacetOrigin(index int) (int, int) {
	edge := r.FacetEdge()
	if r.SizeX >= r.SizeY {
		return r.MapX + index*edge, r.MapY
	}
	return r.MapX, r.MapY + index*edge
}

// FacetNameAt names a facet. Facet 0 carries the region's own name; the rest
// were named at provisioning, in facet order.
func (r Region) FacetNameAt(index int) string {
	if index <= 0 || index > len(r.FacetNames) {
		return r.Name
	}
	return r.FacetNames[index-1]
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
	edge := r.FacetEdge()
	if edge <= 0 {
		return 0, true
	}
	if r.SizeX >= r.SizeY {
		return (gridX - r.MapX) / edge, true
	}
	return (gridY - r.MapY) / edge, true
}

// FacetAtPosition reports which facet contains a region-local position in
// metres, clamping positions outside the region onto its nearest facet so a
// caller resolving a slightly out-of-bounds arrival point still gets a facet.
func (r Region) FacetAtPosition(x, y float64) int {
	along := x
	extent := float64(r.SizeX * 256)
	if r.SizeY > r.SizeX {
		along = y
		extent = float64(r.SizeY * 256)
	}
	if along < 0 {
		along = 0
	}
	if along >= extent {
		along = extent - 1
	}
	edgeMetres := float64(r.FacetEdge() * 256)
	if edgeMetres <= 0 {
		return 0
	}
	index := int(along / edgeMetres)
	if index >= r.FacetCount() {
		index = r.FacetCount() - 1
	}
	return index
}
