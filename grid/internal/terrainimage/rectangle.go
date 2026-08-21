package terrainimage

// Rectangular plateaus, for the macro regions of ADR 0036.
//
// The square plateaus above are shorelines: seabed, waterline, gentle rise.
// These are the other thing an operator wants — a flat working surface with a
// hard edge — and they exist because a rectangle could not have terrain at all
// until the region's heightmap loader learned to read non-square files.
//
// The interesting part is `joined`. A region ramps down at its edges, but an
// edge it shares with a neighbour must not: two regions that both ramp at a
// shared border make a trough along it, and if that trough falls below the
// waterline the border becomes a canal you swim rather than a line you walk.
// So each span that abuts a neighbour is named, and stays at plateau height.

// Edge names one side of a region in its own local coordinates.
type Edge string

const (
	West  Edge = "west"
	East  Edge = "east"
	South Edge = "south"
	North Edge = "north"
)

// Span is the part of one edge that abuts a neighbour, as a half-open range of
// samples along that edge. A whole edge is Begin 0, End the edge's length.
type Span struct {
	Edge       Edge
	Begin, End int
}

func (s Span) covers(edge Edge, x, y int) bool {
	if s.Edge != edge {
		return false
	}
	along := x
	if edge == West || edge == East {
		along = y
	}
	return along >= s.Begin && along < s.End
}

// RampedPlateau returns a width-by-height heightmap flat at `plateau` metres,
// falling linearly to zero over the outermost `ramp` metres of every edge
// except the spans listed in `joined`, which stay flat so they meet a
// neighbour's ground without a step.
func RampedPlateau(width, height int, plateau, ramp float64, joined []Span) []float32 {
	heights := make([]float32, width*height)
	for y := 0; y < height; y++ {
		for x := 0; x < width; x++ {
			distance := -1.0
			consider := func(edge Edge, d int) {
				for _, span := range joined {
					if span.covers(edge, x, y) {
						return
					}
				}
				if distance < 0 || float64(d) < distance {
					distance = float64(d)
				}
			}
			consider(West, x)
			consider(East, width-1-x)
			consider(South, y)
			consider(North, height-1-y)
			// Every edge joined: nothing to fall away from.
			if distance < 0 || distance >= ramp {
				heights[y*width+x] = float32(plateau)
				continue
			}
			heights[y*width+x] = float32(plateau * (distance / ramp))
		}
	}
	return heights
}
