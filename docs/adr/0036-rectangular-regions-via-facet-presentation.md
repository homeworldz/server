# ADR 0036: Rectangular Regions via Facet Presentation

Status: Accepted

This ADR records **current expectation and intent**, not a commitment, and is
expected to be revised as evidence arrives.

**Implementation state (2026-08-19).** The grid half is built: rectangular
provisioning under the shape rule, per-facet names and consecutive ports,
and per-facet squares on every discovery surface (migration 33). The region
half's core is built: per-facet sockets, facet-keyed circuits, rebased
object/avatar/terrain/parcel encoding, per-facet physics fields, and the
internal-line ceremony with circuit re-tag. Still open, all reachable only
on a rectangle: inbound position rebasing (rez rays, multi-object edits,
land edits, parcel request rectangles), per-facet chat positions,
within-macro teleports across a facet line, the session transport's
rectangular terrain descriptor — and live viewer acceptance.

## The problem

A region larger than one tile is desirable for the same reason variable-size
regions exist at all: land that behaves as one place — one physics world, one
script runtime, one process to operate — with no border crossings inside it.
The single physics world cuts both ways: continuous simulation across the whole
land, and one simulation bearing the whole land's load. But two constraints pull against each other:

- **Viewers cannot render a non-square region.** Firestorm accepts the
  `RegionSizeY` wire field and discards it: `LLWorld::addRegion` receives
  `region_size_y` as a parameter and never reads it, `LLViewerRegion` stores a
  single `mWidth` "on a side", and `LLSurface` allocates terrain as
  `mGridsPerEdge * mGridsPerEdge` — square by construction. The minimap tile
  code hardcodes `totalY = totalX` under a `/* TODO: Nonsquare regions? */`
  comment. A 256x1024 region would render as 256x256 with three quarters of it
  missing. [ADR 0016](0016-firestorm-compatibility-target.md) makes viewers the
  compatibility target, so this is a hard boundary.
- **Splitting land into real adjacent regions buys shape at the worst price.**
  Each border becomes a server-to-server handoff: transit protocol, state
  serialization, physics discontinuity, script migration — the classic Second
  Life border-crossing failures, multiplied by every internal line.

## Decision

A large region runs as **one simulation core in macro coordinates**
(0..W x 0..H metres) and is presented to viewers as a row of **facets**:
square viewer-facing regions, each with its own UDP port, region handle,
seed capability, and event queue, all served by the same process.

**The facet is the largest square that tiles the shape: the shorter
dimension.** The shape rule follows from that — the longer dimension must be a
whole multiple of the shorter one. A 4x8 is two 4x4 facets with a single
internal line; a 2x6 is three 2x2 facets; a 2x5 is invalid. Facets are never
smaller than they must be, because every facet boundary costs a crossing
ceremony and a circuit per avatar. Facet sizes are limited to the square sizes
viewers have proven live (1, 2, and 4 tiles today), so the shorter dimension
must be one of those.

The viewer believes it is standing in one 256 m region surrounded by live
neighbors. When an avatar walks over an internal line, the region sends the
ordinary crossing ceremony — `EnableSimulator` is already established,
`CrossedRegion` names the destination facet's endpoint and seed capability —
and then simply re-tags the avatar's primary circuit to the other socket.
Nothing else moves. There is no transit, no serialization, no handoff, because
there is no second server.

Three facts about the viewer make this sound:

- **Viewers key regions by `IP:port`, not by process.** `LLWorld::getRegion`
  resolves every inbound UDP message by sender host, and region removal is
  host-keyed too. One process behind N ports is indistinguishable from N
  processes.
- **The crossing ceremony is already viewer-driven.** The visible stall in a
  real crossing is mostly the server-to-server handoff behind the messages.
  With the handoff gone, the ceremony is reduced to a circuit switch.
- **Square variable-size regions are proven.** 512 and 1024 presentation
  passed live acceptance in full (terrain, map, movement, persistence), so a
  facet larger than one tile relies only on demonstrated viewer behavior.

The viewer only ever sees squares of sizes it has already rendered correctly.
The square constraint stops being a protocol fact and becomes a shape rule:
rectangles whose longer side is a multiple of the shorter. One caveat is
inherited rather than removed — crossings **between** variable-size regions
(the internal line of a 4x8) are listed in the live-acceptance record as open,
so that acceptance must pass before large facets are trusted.

## Why not the alternatives

- **Variable-size rectangles on the wire** fail at the viewer, as above. No
  server-side work routes around a square `LLSurface`.
- **N real regions** work today and remain the fallback, but every internal
  border is a genuine handoff with its full failure surface, and physics is
  discontinuous across each line.
- **OpenSim megaregions** are the cautionary cousin: they made one southwest
  region physically own everything and left the covered tiles as broken
  ghosts — wrong minimap, wrong world map, confused neighbor logic. Facets are
  the honest inversion: every tile the viewer sees is a fully served region by
  the normal rules, so map, minimap, and neighbor rendering need no special
  cases.

## What the facet layer consists of

- **N UDP sockets and per-facet capabilities.** The process binds one viewer
  socket per facet. Avatars are already keyed by transport endpoint; the key
  extends to (facet, endpoint). Each facet serves its own seed capability path
  and event queue over the shared HTTP listener.
- **A coordinate rebasing seam.** Outbound positions become facet-local
  (macro position minus the facet origin); inbound positions rebase back. The
  object update encoder already takes a region handle per call, which is the
  natural choke point. This seam is the bulk of the cost — wide but
  mechanical.
- **Object-to-facet assignment.** An object's updates go out on the circuit of
  the facet containing it. An object moving across an internal line is a
  `KillObject` on one circuit and an `ObjectUpdate` on the other — atomic
  server-side, since both circuits are the same process. Viewer-side
  re-instantiation still happens, so a seated vehicle crossing flickers like a
  good crossing, not a bad one.
- **Terrain slicing.** The macro heightmap is authoritative; each facet's
  `LayerData` serves its facet-sized square window. Facets never expose the
  macro extent, so the terrain wire format carries nothing it has not already
  carried for square regions.
- **Parcel views.** Parcels live in macro coordinates. The parcel overlay is
  served per facet; a parcel spanning an internal line is split into per-facet
  views but stays one parcel server-side.
- **Neighbor visibility.** The viewer sees into adjacent facets through the
  same child circuits real crossings already exercise. Every facet is a
  permanently live neighbor served from the same memory.

## What the grid needs

The provisioned-region schema currently records one square `size`. Rectangles
need `size_x` and `size_y`, validated by the shape rule — the shorter must be a
supported facet size and must divide the longer; the no-overlap exclusion
constraint generalizes directly (its two ranges are already built
independently).

**Facets are named at provisioning.** The website prompts for one region name
per facet — the shape determines how many — and the names are assigned in map
coordinate order: eastward along a horizontal row, northward along a vertical
one. Each name is subject to the grid's ordinary region-name uniqueness rules,
since to a viewer each facet is an ordinary region. The region registers
**one endpoint per facet** — one region row with a facet endpoint list, rather
than N rows sharing a process, so identity, lease, and ownership stay singular.
Login and teleports address a facet handle; the map already renders per-tile.

## What this buys

- **No inter-server handoffs inside the region** — the stated goal. Transit
  ([ADR 0025](0025-idempotent-avatar-transits.md)) remains for real borders
  between separate regions.
- **Continuous physics and scripts across internal lines.** One Jolt world
  simulates the whole macro region, so an object straddling a line — the case
  Second Life never handled well — is simply an object.
- **One process, one lease, one snapshot, one script runtime** for the whole
  land, regardless of shape.

## Costs and caveats

- Viewers still see the crossing **ceremony** at each internal line: a brief
  circuit switch, far shorter than a real crossing, but not literally
  invisible.
- Region-name toasts fire per facet, so walking the length of a macro region
  announces each facet's name in turn — a consequence of facets carrying real,
  distinct names (see "What the grid needs").
- Every avatar holds N circuits and N event queues instead of one; the region
  carries that fan-out for every facet in view range.
- **One physics world bears the whole land's load.** Splitting into real
  regions is also how simulation cost is spread across processes and hosts;
  a macro region gives that up. A 2x10 is twenty tiles of terrain, objects,
  and avatars in one Jolt world on one core's scene loop, so the practical
  ceiling on macro size is simulation load, not protocol.
- The rebasing seam must be complete. A single unrebased position leaks macro
  coordinates onto a facet circuit and produces objects hundreds of metres out
  of bounds — the failure mode is loud, which is the good kind.

## The first-party client skips all of it

The facet layer is a **viewer-compatibility shim**, exactly where
[ADR 0016](0016-firestorm-compatibility-target.md) and
[ADR 0032](0032-region-extensions-for-new-client.md) put such costs. The
Homeworldz client ([ADR 0030](0030-client-architecture.md)) speaks macro
coordinates natively over its own transport and sees one WxH region, no facets,
no ceremony. As viewer traffic declines, the shim is what ages out — the
simulation core never carried the compromise.

## Relationship to other ADRs

- **ADR 0016** — viewers remain the compatibility target; facets exist so that
  target never receives a shape it cannot render.
- **ADR 0024** — provisioned identity gains `size_x`/`size_y` and per-facet
  endpoints; identity itself stays one row, one region.
- **ADR 0025** — idempotent transits are unchanged and apply only at real
  region borders; internal lines never invoke them.
- **ADR 0008** — one lease per region process, unchanged; facets are not
  leaseholders.
- **ADR 0030 / 0032** — the client consumes the macro region directly; the
  facet layer is legacy-facing only.
