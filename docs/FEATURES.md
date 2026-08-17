# Homeworldz Feature Differences

This document tracks major intentional differences between Homeworldz and the
Second Life, OpenSimulator, and Halcyon models. It is a design ledger, not a
general implementation checklist. Missing compatibility work belongs in
[`PLAN.md`](PLAN.md); protocol observations belong in
[`FIRESTORM.md`](FIRESTORM.md).

## Implemented differences

### Live terrain-derived world maps

Homeworldz world-map tiles are rendered from each running Region's current
heightfield over the Grid-authenticated service boundary rather than generated
only as an offline or administrator-triggered snapshot. Viewer terrain edits
can therefore become visible on the world map
without a separate map-generation job. The Grid colors and shades each live
heightfield, caches it briefly, and composes adjacent Regions at every viewer
zoom level. An unreachable Region falls back to a packaged default tile.

Live Firestorm acceptance on 2026-07-16 distinguished two initially identical
Regions by carving a large X into Welcome. The X appeared immediately on the
world map while Sandbox remained unchanged.

This is intentionally different from the InWorldz/Halcyon deployment model,
where terrain was processed into map textures on a scheduled cycle, commonly
once per day. Homeworldz terrain edits feed the live map pipeline immediately,
subject only to its brief tile cache, so operators do not wait for a daily map
refresh or run a separate terrain-map generation job.

### Asset creation provenance

Every Homeworldz asset metadata record has a required `creator_id` UUID. For a
viewer-created asset, this is the authenticated UUID of the user whose upload
created the asset record. Bundled or migrated assets whose original creator is
unknown use the zero UUID as an explicit system/unknown value.

The creator is asset provenance, not current ownership:

- Transferring an inventory item does not change the asset creator.
- Creating another inventory item that references an existing asset does not
  change the asset creator.
- Re-registering an existing viewer-facing asset UUID does not replace its
  original creator.
- Inventory creator and owner fields may still be retained independently where
  the viewer protocol requires them.

This differs from compatibility models that can keep creator and ownership
primarily on inventory items or scene objects without requiring authenticated
uploader provenance on every stored asset mapping. Homeworldz enforces the
provenance rule at the asset-storage boundary regardless of what metadata a
viewer exposes. Second Life's internal asset-store schema is not public, so
this comparison concerns observable/protocol semantics rather than a claim
about its private implementation.

### Region-local, content-addressed assets

Homeworldz stores immutable asset bytes as region-local SHA-256-addressed blobs.
Viewer UUIDs map to those blobs in SQLite, allowing different viewer IDs to
share bytes without duplication. This deliberately does not preserve the
central or pluggable legacy asset-service boundaries commonly used by
OpenSimulator and Halcyon, and it does not attempt to reproduce Second Life's
private backend layout. Regions register stable origin and replica locations
with the grid, fetch missing assets directly from another region using an
authenticated internal endpoint, verify the advertised size and SHA-256, and
retain the original UUID and creator provenance in the local replica.

Durability of inventory content does not rely on regions staying alive. A
planned grid-side asset vault (ADR 0026) durably holds the bytes of every
inventory-referenced asset, ingested and verified when the inventory item is
created, so a user's inventory survives the permanent loss of any region. The
vault is replica-only: it never originates assets and is never in the viewer
fetch path, which still always goes to the connected region.

### Free texture uploads

Homeworldz does not charge users to upload textures. Regions advertise a zero
upload price through the viewer economy protocol and identify the grid's
currency as credits (`C$`) for viewer interfaces that insist on displaying a
currency beside zero. Credits may support other features later, but texture
upload pricing is not reserved as an economy mechanism.

Primitive texture entries are retained as viewer-protocol data, including
per-face texture selection and color tint. They persist across region restarts
and are included when an object is taken to inventory and re-rezzed.

### Internal service boundaries

Homeworldz uses a C++20 region server, Go grid services, HTTP/JSON internal
APIs, PostgreSQL central state, and region-local SQLite/filesystem state. It
does not preserve OpenSimulator or Halcyon internal APIs, database schemas,
WHIP/Aperture boundaries, C#/.NET implementation constraints, or protobuf as
an architectural default. Viewer-protocol compatibility remains a goal even
when the server internals differ.

### Independently restartable Grid services

The central Grid service can be rebuilt or restarted without restarting its
connected Regions. Active viewer circuits, avatar simulation, physics, and
scene state are owned by the Region processes and continue running while the
Grid service is briefly unavailable. Viewer sessions and other central state
are persisted in PostgreSQL, so Regions and viewers resume Grid-backed
operations such as AIS inventory access when the service returns without
requiring a new Region process or a new viewer login.

This differs from deployments where grid-level services share a process or
runtime lifecycle with simulators, or where restarting the central service
requires coordinated simulator restarts. Requests that arrive during the
brief outage may still fail and require retry; independent restartability does
not make the Grid service optional or remove the need for availability
monitoring.

### Pluggable physics engines

Homeworldz keeps simulation behind an engine-independent physics plugin
boundary rather than making one third-party engine part of the scene model.
[Jolt Physics](https://github.com/jrouwe/JoltPhysics) is the initial target and
default engine. NVIDIA PhysX 5.x is also intended to become a supported plugin;
its current adapter and shared acceptance lab remain the foundation for that
eventual production support. A region selects one engine implementation while
the authoritative scene, persistence, and transfer contracts remain common.
The common physics contract uses the same right-handed, Z-up coordinates as
regions and viewers; adapters must not expose an engine-native vertical axis.
The current Jolt production path includes synchronized terrain heightfields,
appearance-sized avatar capsules, static prim collision, and viewer-toggled
dynamic box, sphere, cylinder, and convex-prism bodies with persisted
Physical/Phantom flags and streamed linear and angular motion. Canonical
spheres and cylinders use native analytic Jolt shapes; prisms use a portable
six-point convex hull matching Firestorm's wedge preset. The revolved basic
shapes — Torus, Tube, and Ring — approximate collision as a solid cylinder
over the prim's extents; the central hole, like cut, hollow, and twist on
every shape, is visual only and not yet physical. Their shape and
collision flags survive Take and re-rez through portable object assets.
Reshaping an existing prim in the build tool (`ObjectShape`) applies the new
path/profile block authoritatively, persists it, and rebuilds the collision
body to match.
Cylinder collision must retain its round circumference so flattened cylinders
remain suitable as wheels; physics adapters may not replace them with boxes.
Viewer Extra Physics values are decoded from `ObjectFlagUpdate` and persisted
with the scene: physics shape type, density, friction, restitution, and gravity
multiplier. Density, friction, and restitution feed the physics-body mirror;
`PhysicsShapeType.None` disables collision for the object.

Avatar contact with dynamic objects is resolved natively by Jolt. The virtual
character's horizontal contact-force ceiling is derived from avatar mass and a
maximum push acceleration, preserving a strong relative response between
small movable prims and multi-tonne bodies without region-side push impulses.
Low static obstacles retain stair climbing, while dynamic obstacles stay in
Jolt's contact solver rather than being automatically stepped over.
Live Firestorm acceptance on 2026-07-15 confirmed that a 0.5 m dynamic cube
slides a controlled distance under avatar contact, while a 1 x 1 x 1 m dynamic
cube remains effectively stationary and blocks or deflects avatar movement.
Selecting an owned, modifiable physical object temporarily suspends its dynamic
simulation until deselection, matching viewer edit expectations without changing
the persisted Physical flag. Viewer mouse dragging is handled separately through
`ObjectGrabUpdate` and a bounded, mass-scaled physics impulse.
Live Firestorm acceptance confirmed edit suspension and restoration, mouse-hand
dragging, and mass-relative avatar contact after resizing a physical prim to
1 x 0.5 x 0.75 m.

The planned vehicle layer preserves the Second Life/Halcyon LSL model while
using native physics-plugin capabilities. A single `llSetVehicleType` call is
intended to activate a usable named preset; standard vehicle parameters then
refine its behavior without exposing Jolt- or PhysX-specific APIs to scripts.

### Static-capture mesh collision

Homeworldz plans to keep visual mesh geometry separate from a portable physics
representation. Static geometry may use validated triangle collision meshes;
dynamic mesh objects use convex hulls or compound hulls. Animated, skinned, or
deforming visuals retain a static collision capture while the object moves as
a rigid body, rather than rebuilding collision geometry with every visual
deformation. Attachments remain non-colliding by default. Engine-specific Jolt
and PhysX cooked shapes are caches, not authoritative assets. See
[`ADR 0023`](adr/0023-portable-mesh-collision-representations.md).

### Recursively folded object permissions

Homeworldz treats Halcyon's effective-permission behavior as the compatibility
reference for nested object contents. Modify, Copy, Transfer, and derived
Export restrictions are folded across every prim in a linkset and every task
inventory item. A containing object inventory item records those effective
current and Next Owner masks when the object is taken, so the restriction
remains compositional when an object is placed inside another object, taken
again, rezzed, returned, duplicated, or eventually transferred to another
user. The literal masks on each underlying item remain intact; viewer aggregate
permissions and action authorization consume the folded result.

This deliberately includes nested cases that Second Life has historically left
unsupported or incomplete and must not be simplified to OpenSimulator's
permission behavior. The permission core has a three-level nested-object test
as well as linkset-child Contents coverage.

### AIS-first viewer inventory

Homeworldz requires Second Life AIS v3 for supported viewer-facing inventory
mutation workflows. Legacy inventory capabilities and UDP messages may remain
as compatibility shims for OpenSimulator, Halcyon, Firestorm, and other
viewers, but feature completion does not require a parallel legacy path. All
adapters use the same grid-owned inventory model rather than creating separate
legacy and AIS stores. See
[`ADR 0018`](adr/0018-ais-first-viewer-inventory.md).

The implemented AIS v3 path includes personal folder and item mutation,
texture-backed item creation, saving named outfit folders, copying a complete
Library outfit into personal Inventory, replacing Current Outfit links, and
recursively emptying Trash.
Firestorm's individual item and folder moves use narrow legacy UDP adapters;
those adapters update the same authoritative inventory store.

Read-only legacy inventory browsing is a stretch compatibility goal for older
viewers. It is best effort, may remain incomplete, and does not imply support
for legacy mutation workflows or a commitment to any particular older viewer.

### Parcel land data over the Event Queue

Homeworldz delivers `ParcelProperties` to viewers as an LLSD message over the
Event Queue rather than as the legacy `ParcelProperties` UDP packet. This matches
the current Halcyon/OpenSimulator behaviour and, more importantly, avoids the
LLUDP datagram size limit: a full-region parcel bitmap is 512 bytes on a 256 m
region but 8192 bytes on a 1024 m region, which does not fit a single UDP
message. The viewer-to-simulator parcel requests (`ParcelPropertiesRequest`,
`ParcelPropertiesUpdate`, `ParcelDivide`, `ParcelJoin`, `ParcelAccessListRequest`,
`ParcelAccessListUpdate`) are still decoded from UDP; only the potentially large
`ParcelProperties` reply moves to the Event Queue. The `ParcelOverlay` that draws
parcel boundaries stays on UDP but is chunked at 1024 cells per packet with an
incrementing `SequenceID`, so it also scales to the larger region sizes.

The default region owner is authoritative from the grid region record. A region
whose grid record has no owner (older provisioning) creates its default parcel as
public land and reports the connecting viewer as a non-owner, rather than the
previous placeholder that told every viewer it owned the whole region.

Parcel enforcement is authoritative for build/rez (`CreateObjects`), script
execution (`AllowOtherScripts`), teleport entry and continuous walk-in ejection
(ban/access lists plus landing-point routing), viewer-initiated object return,
and periodic `OtherCleanTime` auto-return. Group-scoped flags currently fall back
to owner-only behaviour because groups are not yet modelled (Phase 5). The damage
(`AllowDamage`) and push (`RestrictPushObject`) flags are carried but inert until
the combat/health and `llPushObject` systems exist.

### Honest simulator feature advertisement

Homeworldz advertises a `SimulatorFeatures` flag only when the region actually
implements the behavior behind it, and advertises a definite `false` rather than
omitting the key when it does not. A viewer enables interface on the strength of
these flags, so an over-claim produces controls that silently do nothing and an
under-claim hides behavior that works.

Two consequences are visible to creators:

- **Physics shape types** report `prim` and `none` as supported and `convex` as
  **not** supported. Prim is the default collision shape and `None` is honored by
  excluding the entity from the collision mirror, but Convex Hull, while accepted,
  clamped, and persisted through take and re-rez, does not change the collision
  shape — that is selected from the prim's own shape. The viewer therefore does
  not offer a Convex Hull option the region would ignore. (Prisms and pyramids do
  use convex hulls, but as a consequence of their prim shape, not of this
  setting.)
- **Mesh, dynamic pathfinding, and avatar hover height** report `false`. Mesh
  assets are Phase 4 and ADR 0023 work. The region emits a hover-height block in
  `AvatarAppearance` but accepts no hover-height update, so the control stays
  hidden rather than appearing inert.

Physics materials (density, friction, restitution, gravity multiplier) and the
Export permission bit are implemented and advertised as such. Chat ranges —
whisper 10 m, say 20 m, shout 100 m — are advertised from the same constants the
region enforces when relaying a chat message, so the viewer's chat interface
cannot disagree with what is actually audible.

This differs from deployments that advertise a broad static feature map
regardless of what the simulator implements. Adding a feature here means changing
one flag beside the code that backs it.

### Grid-level estates

Estates are stored centrally on the grid (Postgres), not per region, because an
estate spans regions: every region maps to exactly one estate and all regions
sharing an estate id share its owner, managers, allowed-user/group and ban lists,
and its deny/voice/teleport flags. A region with no estate is lazily assigned a
default estate (id 100, "My Estate") owned by the region owner, and a single
default estate is reused for all regions owned by the same owner. The region
fetches its estate at registration and applies it to `RegionHandshake`/`RegionInfo`
flags, `ParcelProperties` region deny flags, and region-entry enforcement; estate
edits from the Region/Estate floater are written back through the grid so sibling
regions can pick them up. Viewer-driven region terrain-texture settings, region
restart, and estate kick/teleport-home remain future work.

## Planned differences

### Variable-sized regions

Homeworldz supports exactly three OpenSimulator-style region sizes:
1x1 (256 by 256 metres), 2x2 (512 by 512 metres), and 4x4 (1024 by 1024
metres). Larger sizes and arbitrary whole multiples are intentionally out of
scope. This differs from Second Life's public 256-by-256-metre region model
while preserving the most useful OpenSim content convention. A larger Region
is one authority with one scene, terrain, physics world, inventory endpoint,
and access key—not a group of loosely joined 256-metre Regions. Provisioned
extents drive terrain persistence and extended viewer packets, Jolt heightfield
and movement bounds, login/teleport/map size metadata, and live map slicing.

### Server-side appearance baking

Homeworldz will bake avatar appearance **server-side**: the region composites
the bake layers from a user's worn wearables and serves the baked textures, so
any client — including thin or headless ones such as LibreMetaverse/TestClient —
rezzes correctly without performing its own baking. Second Life moved viewer
appearance baking to the server ("server-side appearance"); Homeworldz adopts
the same model rather than depending on every client to fetch wearables, bake,
and upload textures. Planned; today appearance relies on client-side baking,
which leaves non-baking clients rendered as clouds.

### Voice via WebRTC

Homeworldz will provide voice using **WebRTC**, matching the direction Second
Life and current viewers (including the supported Firestorm release) are moving.
**Vivox is explicitly not pursued.** Planned and lower priority than server-side
baking; no voice is provided today.

## Maintenance rule

Add an entry here when a behavior is an intentional product or architecture
difference that operators, developers, or content creators could observe or
must account for. State whether it is implemented or planned, and avoid listing
temporary gaps as features.
