# Homeworldz Roadmap

What is built and what is planned, by phase. Checkboxes describe the present
state, not a release date.

## Progress snapshot

Effort-weighted estimates, not checkbox ratios. Two bars, because this
repository carries two deliverables on different clocks: the server platform
serving Firestorm and compatible viewers, and the back end for the Homeworldz
client.

<p>
<label class="roadmap-overall-progress">
 <span>Legacy (Firestorm-compatible) services:</span>
  <progress data-color="primary" max="100" value="39">39%</progress>
 <strong>39%</strong>
</label>
</p>

<p>
<label class="roadmap-overall-progress">
 <span>Modern Homeworldz client + back-end:</span>
  <progress data-color="primary" max="100" value="36">36%</progress>
 <strong>36%</strong>
</label>
</p>

| Phase | Progress | Estimate |
| --- | --- | ---: |
| 1. Functional Single-region World | <progress class="roadmap-phase-progress" data-color="primary" max="100" value="100" aria-label="Phase 1 progress: 100%">100%</progress> | 100% |
| 2. Connected Multi-region World | <progress class="roadmap-phase-progress" data-color="primary" max="100" value="89" aria-label="Phase 2 progress: 89%">89%</progress> | 89% |
| 3. Interactive Physical World | <progress class="roadmap-phase-progress" data-color="primary" max="100" value="50" aria-label="Phase 3 progress: 50%">50%</progress> | 50% |
| 4. Mesh and Creator Platform | <progress class="roadmap-phase-progress" data-color="primary" max="100" value="50" aria-label="Phase 4 progress: 50%">50%</progress> | 50% |
| 5. Social Communications | <progress class="roadmap-phase-progress" data-color="primary" max="100" value="6" aria-label="Phase 5 progress: 6%">6%</progress> | 6% |
| 6. LSL Scripting | <progress class="roadmap-phase-progress" data-color="primary" max="100" value="16" aria-label="Phase 6 progress: 16%">16%</progress> | 16% |
| 7. Reliable Operations and Distribution | <progress class="roadmap-phase-progress" data-color="primary" max="100" value="23" aria-label="Phase 7 progress: 23%">23%</progress> | 23% |
| 8. Scale, Compatibility, and Ecosystem | <progress class="roadmap-phase-progress" data-color="primary" max="100" value="3" aria-label="Phase 8 progress: 3%">3%</progress> | 3% |
| 9. Modernized Communications Transport | <progress class="roadmap-phase-progress" data-color="primary" max="100" value="66" aria-label="Phase 9 progress: 66%">66%</progress> | 66% |
| 10. Modern Client Support | <progress class="roadmap-phase-progress" data-color="primary" max="100" value="8" aria-label="Phase 10 progress: 8%">8%</progress> | 8% |
| 11. Economy and Marketplace | <progress class="roadmap-phase-progress" data-color="primary" max="100" value="0" aria-label="Phase 11 progress: 0%">0%</progress> | 0% |

## Phase 1: Functional Single-region World

### Platform foundation

- [x] Establish the C++20 region server, Go grid service, PostgreSQL central state, and SQLite plus filesystem region-local state.
- [x] Define configuration, bootstrap, health, authentication, logging, CI, and service contracts.
- [x] Implement region registration, leases, discovery, login sessions, and online presence.
- [x] Establish the authoritative fixed-step scene loop and durable scene snapshots.

### Viewer connection and appearance

- [x] Complete the minimum supported Firestorm login, UDP circuit, handshake, capabilities, event delivery, terrain, chat, and static-object flow.
- [x] Provide default body parts, clothing, Current Outfit links, legacy avatar baking, and persistent appearance across relogs.
- [x] Provide a read-only system Library with default avatar and terrain content.
- [x] Synchronize nearby avatar presence, movement, appearance rebakes, and animation changes between concurrently connected viewers.
- [x] Bake avatar appearance **server-side**, so thin and headless clients rez correctly with no client-side baking; SSB delivery to full viewers remains.
- [x] Bake the wearer's own Current Outfit rather than one fixed outfit, cached per outfit.
- [x] Broadcast `KillObject` for a departing avatar through the one teardown point every removal path funnels into, so it stops lingering in other viewers' views.
- [x] Support alpha layers for viewers, so a mesh body hides the default body.
- [x] Apply alpha layers in the server-side bake, proven on a worn alpha: the masked region bakes fully transparent and no other slot moves.
- [x] Re-bake a wearer who changed outfit mid-session, on request; arrival already re-reads the outfit.
- [x] Call that re-bake automatically: the grid tells the region a wearer's Current Outfit changed, so wearing something takes effect without a relog.

### Authoritative avatar movement

- [x] Decode movement, camera, jump, and flight controls and persist provisional avatar state.
- [x] Stream authoritative position, velocity, and rotation changes back to viewers.
- [x] Complete viewer-visible walking, turning, jumping, stopping, flight toggling, ascent, and descent without requiring a relog.
- [x] Add animation-state selection and synchronization for standing, walking, running, jumping, falling, flying, hovering, and landing.
- [x] Reconcile viewer prediction with the authoritative region position without visible snapping or drift.

### Basic avatar physics

- [x] Integrate a Jolt avatar capsule into the production scene loop.
- [x] Support persistent viewer terrain editing with live patch updates for targeted slope, step, drop, and grounding tests.
- [x] Mirror authoritative terrain into Jolt at startup and replace the collision heightfield immediately after viewer edits.
- [x] Sample terrain continuously for provisional grounding rather than retaining only the height at the login position.
- [x] Support terrain walking, slopes, steps, falling, jumping, landing, flight, and collision-safe motion.
- [x] Collide avatars with static scene objects while preserving practical viewer movement behavior.
- [x] Collide avatars with dynamic scene objects while preserving practical viewer movement behavior.
- [x] Persist and restore position, orientation, velocity, and flight state; safely recompute grounded contact from Jolt on entry.

### Core inventory, assets, and objects

- [x] Implement AIS v3 inventory fetch and mutation, viewer-authored wearables, named outfit saving, Library outfit copying, Current Outfit links, folder operations, and Trash lifecycle operations.
- [x] Implement free texture upload, required creator provenance, content-addressed assets, origin registration, and region replication.
- [x] Implement primitive rez, edit, permissions, ownership, take, delete, restore, and restart persistence.
- [x] Implement persistent nonphysical linksets with root and child transforms, whole-object and Edit Linked scaling, duplication, take, take-copy, return, derez, inventory round trips, and static child collision.
- [x] Implement task inventory (object contents) and complete its permissions, mutation, copy, derez, return, and inventory round-trip lifecycle.
- [x] Implement creator-attributed sound and animation uploads; personal notecard, gesture, and LSL-source creation; and task notecard and script updates, compiling and running the supported Falcon subset on save.
- [x] Complete Firestorm creation, editing, playback, object-contents, restart, and relog acceptance for those content types (viewer landmark creation and About Land ownership deferred to the Phase 2 land operations).

## Phase 2: Connected Multi-region World

### Region topology and variable size

- [x] Load an operator-owned JSON registry of provisioned regions and authenticate region startup by UUID plus per-region access key, returning the authoritative name and map coordinates.
- [x] Add authenticated grid-management endpoints to create, inspect, update, enable, disable, relocate, remove, and rotate credentials for provisioned regions.
- [x] Persist each region's UUID, unique name, owner UUID, X/Y location, endpoints, enabled state, and per-region access-key hash independently of its online lease.
- [x] Let a region authenticate by UUID or unique name plus its access key and fetch effective grid-wide and region-specific startup configuration.
- [x] Represent neighboring regions, coordinates, extents, public endpoints, maturity, and online state in grid discovery.
- [x] Support exactly 1x1 (256 m), 2x2 (512 m), and 4x4 (1024 m) provisioned Regions in runtime configuration, automated terrain/protocol/map tests, and disposable 512 m and 1024 m process-start checks.
- [x] Generalize terrain, physics bounds, viewer coordinates, storage, map tiles, and interest management to the three supported sizes.
- [x] Complete initial live Firestorm acceptance of a 2x2 Region as one continuous 512 by 512 metre simulator.
- [x] Complete 2x2 terrain editing, object persistence, map-idle, and restart acceptance in the live Beta Region.
- [x] Repeat the full variable-size acceptance suite in a 4x4 Region.
- [x] Prevent overlaps and invalid neighbor layouts and define behavior beside offline or differently sized regions.
- [x] Provision rectangular regions ([ADR 0036](adr/0036-rectangular-regions-via-facet-presentation.md)): `size_x`/`size_y` under the facet shape rule, per-facet names and consecutive viewer ports, and one square facet per entry on every discovery surface — topology, lookup, neighbors, map, and login.
- [x] Run a rectangular region as one macro simulation presented to viewers as square facets: per-facet sockets and handles, rebased object and terrain encoding, windowed parcel views and map blocks, per-facet collision fields, and an internal-line crossing ceremony that re-tags the circuit and moves nothing else.
- [x] Serve every facet through standing child circuits: one circuit per facet per viewer, established at arrival and backfilled at handshake, updates partitioned by the facet containing each object or avatar, an atomic kill-then-update when an entity crosses an internal line, and a crossing reduced to promoting a circuit that already exists.
- [x] Complete live Firestorm acceptance of standing child circuits: seamless internal crossings with fast hand-off, the correct facet reported as the current region, movement live immediately after promotion, and attachments intact — after fixing five same-day live defects, including circuit correlation by session identity rather than address (symmetric NATs give each facet socket its own source port).
- [ ] Finish the child-circuit acceptance tail: a sibling facet's objects and avatars deliberately observed before crossing, a second avatar's crossing watched from the other side, clean teardown at relog and logout, and a login through a symmetric NAT.
- [x] Finish the rectangle-only seams: inbound position rebasing (rez rays, multi-object edits, land edits, parcel-request rectangles, Set-Home fallback), per-facet chat positions, within-macro teleports across a facet line answered as a promotion of the standing sibling circuit, and the session transport's width-by-height terrain descriptor.
- [ ] Complete live Firestorm acceptance of a provisioned rectangle, walked end to end across its internal facet line.

### Parcels and local authority

- [x] Implement parcel geometry, ownership, access, landing points, media, and object accounting.
- [x] Enforce build, rez, entry, script, and object-return policy at authoritative boundaries, including walk-in ejection, viewer-initiated return to Lost and Found, and periodic `OtherCleanTime` auto-return.
- [x] Implement estate and region settings needed for terrain, access, maturity, and covenant, including the Region/Estate Terrain tab in full.
- [x] Pin the walkable slope limit with a test.
- [x] Document the whole browser-facing API in `api/openapi-public.yaml`: discovery, registration, authentication, account, client world entry, the grid channel, client inventory, and every administrative user and region operation.
- [x] Server support for password recovery.
- [x] Password recovery screens on the management site.
- [ ] Apply permissions recursively and consistently to linksets, object contents, attachments, and inventory transfers.

### Teleports and avatar crossings

- [x] Keep avatar appearance, inventory, and Current Outfit stable through an explicit teleport between registered regions.
- [ ] Keep avatar appearance stable while sitting and crossing region borders.
- [x] Build an authenticated, idempotent two-region handoff transaction with a transit UUID, generation, prepare, accept, activate, and rollback stages.
- [x] Teleport between registered regions with destination validation, viewer circuit establishment, arrival placement, source retirement, and durable last-location login.
- [x] Teleport within the current Region without creating a Grid transit, preserving flight state and returning Firestorm's `TeleportLocal` response.
- [x] Hand an avatar across a region border, preserving position, velocity, and appearance.
- [x] Pass live Firestorm acceptance for a two-way border handoff in one continuous session.
- [ ] Complete remote-host failure recovery and reconciliation for interrupted teleports.
- [ ] Cross a walking or flying avatar between adjacent regions while preserving appearance, controls, velocity, camera, and session continuity.
- [ ] Transfer the complete attachment set with the avatar and prevent duplicate activation at source and destination.
- [ ] Handle disconnects, destination failure, retries, stale transit records, and reconciliation after process restart.

### Objects, Sitting and Crossings

Vehicle crossings are **not** here. A vehicle is a scripted object driven by
the Second Life vehicle parameter model, neither of which exists yet, so
crossing one waits on Phase 6 and is listed at the end of it. What this phase
owns is every crossing a vehicle would later be built on: a physical object
carrying its motion across the line, an object with avatars attached to it, and
an object with avatars sitting on it.

Sitting comes before the crossing that carries it, and the sit target comes
before sitting. The order runs the other way from the way it reads: `llSitTarget`
lands first, at the top of the scripting phase, because it is what *puts* a sit
target on a prim and there is no point implementing seating against a target
nothing can set. Sitting on that target is next, here. Carrying a sitter across
a border is last, because it is the only one of the three that needs the other
two finished.

- [ ] Sit on a prim: sit targets honored where a script has set one and a
  default seat derived from the prim where none is, with the avatar's world
  transform following the root object, plus unsit, camera placement, and seated
  animation state.
- [x] Define an off-region disposition for every moving entity ([ADR 0037](adr/0037-object-border-crossings.md)): avatars, child prims, attachments, temporary-on-rez objects, non-physical objects, and the no-neighbor case each have a stated answer rather than whichever one the containment clamp produced.
- [x] At a border with no eligible online neighbor, constrain avatar and physical-object origins to the configured Region extent and cancel outward velocity at the crossed edge.
- [x] Resolve border neighbors from persistent grid region records plus their current online leases before choosing crossing versus containment.
- [ ] Cross individual objects and complete linksets without changing creator, owner, permissions, inventory, or physical state. *Built and unit-tested; awaiting proof between two live regions.*
- [ ] Carry a physical object's linear and angular motion across the border so its path is continuous through the crossing. *Built and unit-tested; awaiting proof between two live regions.*
- [ ] Cross attachments as part of their avatar bundle.
- [ ] Transfer an object and every avatar sitting on it as one coordinated bundle — sit target, seat assignment, and seated transform included — with no passenger briefly becoming authoritative in both regions or neither.
- [ ] Establish event and collision cutoffs so crossing does not duplicate or silently lose observable actions.

### World navigation

- [x] Generate live terrain-derived region tiles and compose world-map zoom levels for 1x1 regions.
- [x] Extend region and world-map tile composition to the planned 2x2 and 4x4 region sizes.
- [x] Implement viewer map-block and prefix-name discovery for registered live regions.
- [x] Implement landmark resolution, home location, and teleport routing.
- [x] Serve region land data (ParcelProperties: owner, flags, area, bitmap, landing point, prim accounting) so the viewer's "Landmark This Place" creation and About Land ownership work.
- [ ] Add region and parcel search sufficient to find and reach destinations.
- [ ] Show friends and authorized users useful presence and location without leaking restricted information.

### Inventory asset durability

**.** A region that dies no longer takes its users' inventory
with it: the vault holds verified bytes for every inventory-referenced asset's
whole closure, and the grid refuses any commit it cannot make durable first.
The layer separation below lands before the write-through that fills the vault.
because re-keying an empty vault is free
(.
).

- [x] Separate the blob, asset, and instance layers.

- [x] Implement the grid asset vault.
- [x] Enforce the vault invariant, committing an inventory item only once its whole closure is durable.
- [x] Treat region copies of vault-held assets as an evictable cache.
- [x] Backfill existing inventory-referenced assets into the vault, reporting any that are unfetchable.
- [ ] Tier rarely accessed vault blobs onto slower S3-compatible storage with hash re-verification on rehydration, keeping tiering vault-internal.

## Phase 3: Interactive Physical World

### Production physics integration

- [x] Make Jolt the default production physics world while retaining the engine-independent plugin boundary.
- [x] Create, update, sleep, wake, remove, and restore physical bodies from authoritative scene changes.
- [x] Synchronize physical transforms and velocities to viewers at suitable rates with interest-aware throttling.
- [x] Exclude phantom objects from collision and implement an authoritative, nonpersistent 60-second temporary-on-rez lifecycle with viewer kill updates.
- [ ] Complete collision filtering, material behavior, volume detection, and collision events.
- [x] Represent physical linksets as compound Jolt bodies with correct child shapes, mass properties, collision behavior, transforms, and persistence.
- [ ] Complete live Firestorm acceptance for compound collision, falling and rotation, editing, delinking, and restart persistence.
- [x] Verify deterministic-enough restart and handoff behavior through shared physics acceptance scenarios.

### Attachments

Sitting moved to Phase 2, where the crossing that has to carry it lives.

- [ ] Attach inventory objects to named avatar attachment points with stable local transforms, permissions, ownership, and persistence. *Wearing, permissions, ownership and persistence are live (2026-08-08); the local transform is not — an object is worn at the joint itself because the offset it was last taken off at is stored nowhere.*
- [x] Represent worn attachments as part of the authoritative avatar bundle and restore them on login, and on arrival in any region.
- [ ] Define lifecycle ordering for attachment, seated-avatar, physics, viewer, and later script events.

## Phase 4: Mesh and Creator Platform

What a creator needs before scripting matters: the content pipeline itself —
mesh and its collision sources, uploads and validation, and inventory breadth.
Scripting operates on this content, which is why it now follows rather than
precedes it.

### Mesh pipeline

glTF (GLB) is
the canonical stored format, the creator's upload is never rewritten, and each
client family is served a derived rendition by a conversion worker.

- [x] M1 static mesh.
- [x] M2 Firestorm mesh uploads.
- [ ] M3 material and texture renditions. Texture renditions are done, and so is the half a region drives: a published face that needs a material gets a glTF material asset (`AT_MATERIAL`, type 57) named through ObjectUpdate's render-material ExtraParams, which is how two-sidedness reaches a viewer. **The capabilities once listed here are not needed for that** — `ModifyMaterialParams`, `UpdateMaterialAgentInventory` and `UpdateMaterialTaskInventory` are for *editing* a material in a viewer, and a viewer fetches one to render through the ordinary asset pipeline. What remains is that editing: a face carrying a region-assigned material cannot currently be changed in the build tool, because a material overrides the TextureEntry and there is no capability to alter it.
- [ ] Terrain surface for session clients.
- [x] Close the texture pipeline's asymmetry.
- [ ] V-HACD convex decomposition for mesh physics; the shipped physics block is the conservative bounding-box hull.
- [x] Regenerate stale renditions.
- [x] M4 rigged mesh: glTF skins mapped onto the Bento skeleton, attachments and body wearables. A rig that does *not* map is no longer refused — an upload is, but an imported file keeps its geometry and converts as static, because refusing produced an asset that drew nothing and still collided.
- [ ] M5 import breadth: **server-side** FBX/OBJ/DAE and archive import ([ADR 0035](adr/0035-server-side-source-format-import.md)), so every client uploads the source and gets the same result — and a third-party viewer gains import by uploading a file rather than implementing a converter. Documented Daz Studio export path, optional web import service on the management site.
  - [x] **FBX** import, live and proven against the cloud grid: an upload is stored canonically, imported off the region's loop into one asset per mesh, and lands as inventory items. A 105 MB Character Creator character imports as 15 assets carrying 89 textures.
  - [ ] OBJ and DAE, and the archive/bundle layer for sources whose textures are external.
- [x] Rig retargeting, so a creator does not need Blender with Avastar or Bento Buddy to bring a body in. A Character Creator character is worn and correct in-world; what remains is authoring guidance and one unbuilt case, below.
  - [x] **Character Creator correspondence**, which the design could not assume: all 85 joints a CC skin binds map onto 60 Bento joints, none unmapped, and joints with no equivalent fold into the nearest ancestor that has one. Proportions ride on joint position overrides rather than moving the mesh.
  - [x] **Joint positions measured against the worn set, not the mesh in hand.** An override is an offset from a parent that another *part* places, so a body converted one mesh at a time measured against Linden's skeleton and missed by however far the body had moved that parent — teeth 31 mm through a chin, a tongue 39, eyes out of their sockets. The question is now answered once per joint from the whole source skeleton, and `mesh-diag` measures a set of files as one worn skeleton rather than each alone.
  - [ ] **Sizing the avatar the viewer builds to the mesh it draws.** An imported body stands with its feet above the ground and reads as too tall, so its head looks small for it. The viewer derives `mPelvisToFoot` and body size from *local joint positions* — which joint position overrides do set — but the set we emit omits `mSkull` and `mFootLeft`/`mFootRight`, which have no Character Creator counterpart and so fold into an ancestor rather than being overridden. Both feed those formulas, so the viewer sizes a Character Creator body using Linden's skull and foot offsets spliced into everything else: measured on Talking-Kevin it models a **1.683 m** avatar for a **1.805 m** mesh, and places the top of the head 78 mm below where the mesh's crown actually is. Deriving those two overrides from the mesh's own extents is the likely fix; that it accounts for the whole offset is not yet established.
  - [ ] **Fixing an A-pose bind during retarget**, which is the one unbuilt case. Exporting with *Use T-Pose As Bind Pose* is the working answer and is documented ([IMPORTING.md](IMPORTING.md)); nine of the ten characters in the reference corpus measure T-pose and wear correctly. An export that arrives A-posed still lands with its arms about 30° low, because overrides move a joint's rest position while animations rotate from it.
  - [x] **Carrying two-sidedness**, so hair arrives as authored. Character Creator builds hair from single-sided cards and the viewer culls back faces, which halved every head of hair. Firestorm honours `doubleSided` only for a face with a PBR material, so a face that needs it is now given a glTF material asset of its own (`AT_MATERIAL`, type 57) named by ObjectUpdate's render-material ExtraParams block. No new capabilities were needed: the three this milestone once listed are for *editing* materials in a viewer, not for rendering ones a region assigns. Two things had to be right that documents alone do not say — the asset is an LLSD envelope carrying the glTF as a string, not the glTF itself, and an image's `uri` inside it is a bare asset UUID.
  - [x] **Alpha mode measured rather than assumed.** Blending every composited mask made hair render as haze: a soft mask blended over itself never accumulates to solid. Alpha *testing* is what hair wants, but a cutoff ruins a genuinely soft map, and the material's name cannot tell them apart — one export's eyelash map is a crisper cutout than another's hair. The composited alpha is measured instead, and the two populations sit ten times apart with the threshold in the gap.

### Content creation and inventory breadth

- [ ] Complete viewer building workflows for linksets, materials, sculpt, animation, sound, gesture, notecard, landmark, and script content.
- [ ] Store mesh collision sources separately from visual LODs and build validated physics shapes from them.
- [ ] Implement uploads, validation, dependencies, creator attribution, asset replication, and inventory creation for each supported asset type.
- [x] Add viewer-authored wearable creation, editing, and named outfit saving beyond the initial default-avatar flow.
- [ ] Provide bulk inventory, search, copy, transfer, export-policy, recovery, and large-inventory performance behavior.

## Phase 5: Social Communications

Who people are to each other, and how they reach each other: identity and
profiles, direct and group messaging, voice, friendship, and the group and
role machinery that shared ownership rests on.

This comes before scripting, having previously followed it. It is the smaller
and safer body of work of the two, and it is worth more to the first people on a
grid: they can find and talk to each other long before anyone needs a script to
run.

### Identity, profiles, and communication

- [ ] Implement user-visible names, profiles, interests, images, privacy, and account administration.
- [ ] Implement direct messages, offline messages, group chat, conference chat, mute/block behavior, and delivery history where appropriate.
- [ ] Provide voice via **WebRTC**.
- [ ] Implement friendship, calling cards, presence permissions, and offers.
- [ ] Add abuse reporting and the minimum moderation evidence needed by grid operators.

### Groups, roles, and shared ownership

- [ ] Implement groups, roles, powers, membership, invitations, notices, and group communication.
- [ ] Support group-owned land and objects without weakening creator provenance or transfer permissions.
- [ ] Apply group powers consistently to parcels, estates, object editing, inventory sharing, and moderation.
- [ ] Audit sensitive group and ownership changes.

## Phase 6: LSL Scripting

### Language and compiler

- [x] Establish the dependency-free handwritten Falcon lexer, parser, semantic analyzer, versioned bytecode format, compiler, and automated proof-of-concept suite for an initial typed LSL subset.
- [x] Return Falcon compilation success and escaped error arrays through the Firestorm task-script capability protocol, including line and column locations for lexical errors.
- [ ] Inventory the complete Second Life LSL language and built-in surface plus Halcyon/InWorldz extensions, explicitly excluding OpenSimulator extensions.
- [ ] Complete the handwritten lexer, parser, semantic analysis, diagnostics, and versioned Homeworldz bytecode compiler for that full supported language.
- [ ] Make `float`, `vector` and `rotation` first-class types: variables, parameters, return values, arithmetic, component access, and integer-to-float promotion, with the operators LSL defines on vectors and rotations. What exists now is a deliberate stopgap — literal-only vectors and rotations that can be written and passed to a host call and nothing else — put in so `llSitTarget` could be real before the type system was.
- [x] Store creator-attributed LSL source in personal and task inventory, with Firestorm creation, retrieval, editing, saving, and drag-to-contents behavior.
- [ ] Cache immutable bytecode by source hash, compiler version, and runtime ABI.
- [ ] Build compatibility tests for syntax, types, conversions, lists, strings, states, constants, built-ins, and observable errors.

### Cooperative runtime and resource control

- [x] Integrate the single-threaded C++ Falcon bytecode VM into the authoritative Region thread with explicit instruction-level execution state and no native thread per script.
- [x] Apply bounded aggregate and per-script instruction slices on every Region tick so an infinite loop yields cooperatively instead of blocking the world.
- [ ] Schedule scripts fairly using bounded weighted instruction and wall-clock budgets across scripts, objects, owners, and parcels.
- [ ] Enforce memory, stack, call-depth, event-queue, string, list, payload, owner, object, and parcel limits.
- [ ] Make slow host operations asynchronous and represent waits as serializable tokens or continuations.
- [ ] Add operator metrics, throttling, diagnostics, stopping, resetting, and isolation for inefficient or faulty scripts.

### LSL Events and Region Interaction

- [x] Decode Firestorm `RezScript`, create or transfer the task inventory item, compile its source, instantiate an enabled VM, and dispatch `state_entry`.
- [x] Recompile task scripts after Firestorm edits, preserve the previous running instance after a failed compile, honor the viewer's running flag, and remove the live VM when its task inventory item is deleted.
- [x] Route the initial `llSay` and `llOwnerSay` host calls to Firestorm object chat with owner-only and distance behavior, confirmed in the live cloud Grid.
- [x] Advertise the `SCRIPTED` and `HANDLE_TOUCH` object-update flags so Firestorm enables Touch, then dispatch `touch_start` from `ObjectGrab`.
- [x] Implement `llSitTarget`, which puts a seat offset and rotation on a prim and is what makes a sit target exist at all. It came first among the host functions because the Phase 2 sitting work is meaningless against a target nothing can set. Float literals and literal-only `vector` and `rotation` values were added to Falcon to carry the arguments; the seat is stored on the prim, persisted, and carried by the object asset, so a take, a rez, and a border crossing all preserve it.
- [ ] Implement the remaining object lifecycle, sustained/ended touch, timer, listen, sensor, control, permission, inventory, changed, link-message, collision, land-collision, attachment, and moving events.
- [ ] Implement bounded LSL host functions for scene, physics, inventory, communication, parcel, avatar, HTTP, and data operations.
- [ ] Preserve Second Life event ordering and delay semantics where observable and document intentional Homeworldz differences.
- [ ] Integrate script ownership and permissions with linksets, attachments, seated avatars, parcels, and estate policy.

### Script persistence and crossings

- [x] Demonstrate automated Falcon snapshots after every completed instruction, restoration into a fresh VM, preservation of globals, and continuation from the middle of a `touch_start` handler.
- [ ] Restore enabled task scripts across Region restarts.
- [ ] Serialize bytecode identity, instruction pointer, stacks, frames, globals, current event, event queue, timers, listens, permissions, and pending work in a compact versioned binary format.
- [ ] Integrate stop-and-restore after any completed bytecode instruction into live task scripts and Region persistence without relying on the native C++ stack.
- [ ] Snapshot scripts atomically with their attachment, object, or vehicle physics bundle.
- [ ] Cross heavily scripted attachments and vehicles within defined latency, memory, duplication, and event-loss limits.
- [ ] Version the runtime ABI and provide safe upgrade, incompatibility, and rollback behavior for stored script state.

### Vehicles and physical objects

- [x] Implement stable dynamic-object movement, editing, taking, and restoration without losing physics state.
- [ ] Add the Second Life vehicle parameter model required by LSL vehicles.
- [ ] Make a single `llSetVehicleType` call activate a usable car, sled, boat, airplane, balloon, sailboat, or motorcycle.
- [ ] Synchronize driver controls, vehicle motion, cameras, passengers, and seated-avatar transforms.
- [ ] Preserve object, linkset, inventory, permission, passenger, and physical state as one transferable vehicle bundle.
- [ ] Add load, tunneling, stacking, recovery, and abusive-object safeguards.
- [ ] Cross a vehicle at a region border with its linear and angular motion, vehicle parameters, script state, and seated passengers intact, on top of the Phase 2 object crossing.

## Phase 7: Reliable Operations and Distribution

### Known hazards on the current deployment

- [ ] **Nothing watches free disk space, on a host that is the only home for the grid, the API, the conversion worker, and all four regions.** Measured at 86 GB free of 96 GB, so not urgent.

- [x] Treat presence as a lease rather than durable state, so a session that dies without logging out cannot stay "present".

### Grid and region packages

- [x] Produce separate versioned grid-owner and region-owner packages containing prebuilt executables, runtime dependencies, examples, bootstrap tools, and end-user installation guides.
- [ ] Support clean install, unattended install, upgrade, downgrade where safe, uninstall, and configuration preservation.
- [ ] Sign release artifacts, publish checksums and provenance, and generate a machine-readable release manifest.
- [ ] Validate supported Windows and Linux installations without requiring a source checkout or development toolchain.

### Backups, upgrades, and reconciliation

- [x] Restart or replace the central grid service without restarting connected regions, retaining PostgreSQL-backed viewer sessions.

- [x] Report one version everywhere, dated and traceable.
- [x] Refuse to start a grid service against a database older than the build requires.
- [ ] Back up and restore PostgreSQL grid state, region SQLite state, assets, terrain, configuration, and compatible runtime state.
- [ ] Export and import OpenSim-compatible region archives (OAR) and user inventory archives (IAR).
- [ ] Write only the latest supported OAR and IAR format versions while reading older format versions where practical, since archives are long-lived files that outlive the software that wrote them.
- [ ] Test full-grid, single-region, and selected-user recovery with documented recovery-point and recovery-time expectations.
- [ ] Version schemas and protocols and support rolling grid and region upgrades within a documented compatibility window.
- [ ] Reconcile leases, presence, inventory, assets, crossings, and duplicated or orphaned state after crashes or partial restores.

### Observability and administration

- [ ] Provide metrics, structured logs, traces, health detail, dashboards, and actionable alerts for grid and region owners.
- [ ] Add command-line and authenticated web administration for users, regions, estates, assets, inventory repair, scripts, crossings, and moderation.
- [ ] Make the economy an operator setting: enable or disable it per grid, keep texture uploads free, preserve a useful no-economy deployment mode, and provide the controls a running economy needs.
- [ ] Record tamper-evident audit events for privileged and security-sensitive operations.
- [ ] Define capacity indicators and load-shedding behavior before a region becomes unresponsive.

### Security and deployment hardening

- [ ] Add transport encryption, service identity, credential rotation, scoped authorization, secret-management guidance, and secure defaults for non-local deployments.
- [ ] Validate all viewer, inter-region, asset, inventory, script, and operator inputs against resource-exhaustion and malformed-data attacks.
- [ ] Add dependency, artifact, and configuration scanning plus a vulnerability response and supported-version policy.
- [ ] Perform fault-injection, abuse, denial-of-service, and recovery testing before describing a release as production-ready.

## Phase 8: Scale, Compatibility, and Ecosystem

### Performance and scale

- [ ] Establish repeatable concurrency, scene-complexity, physics, inventory, asset, crossing, script, and network benchmarks.
- [ ] Implement interest management, packet prioritization, backpressure, and adaptive update rates for crowded or complex regions.
- [ ] Scale central services horizontally where measurements justify it while keeping each region's authority unambiguous.
- [ ] Publish tested capacity envelopes rather than relying on nominal limits.

### Compatibility

- [ ] Maintain conformance tests against the pinned supported Firestorm release and evaluate newer releases deliberately.
- [ ] Add read-only legacy inventory access only if its older-viewer benefit justifies the maintenance cost; AIS v3 remains authoritative.
- [x] Support thin and headless clients such as LibreMetaverse: advertise the per-region `FetchInventoryDescendents2`/`FetchLibDescendents2` capabilities and keep the HTTP asset-fetch capabilities LMV-compatible.
- [ ] Validate Halcyon/InWorldz LSL extensions without admitting OpenSimulator scripting extensions accidentally.
- [ ] Document import and migration tools separately from live legacy service or database compatibility.

### Physics and service extensions

- [ ] Promote the existing PhysX 5 adapter to an optional supported physics plugin after it passes the same production scenarios as Jolt.
- [ ] Stabilize versioned plugin contracts only for boundaries with demonstrated operational value.
- [ ] Define safe extension points for grid services without exposing region authority or script execution to untrusted in-process plugins.
- [ ] Maintain deterministic transfer and persistence contracts across every supported physics implementation.

### Release readiness

- [ ] Publish administrator, region-owner, creator, scripter, and contributor documentation appropriate to the supported feature set.
- [ ] Run sustained multi-region worlds with real viewers, scripts, attachments, vehicles, failures, upgrades, and restores.
- [ ] Resolve all release-blocking correctness, data-loss, permissions, crossing, security, and viewer-compatibility findings.
- [ ] Define the supported platform matrix, compatibility guarantees, upgrade policy, and long-term maintenance expectations for the first stable release.

## Phase 9: Modernized Communications Transport

The modern client-facing wire surface: REST bootstrap, a grid-anchored
notification channel, and a region-anchored session — replacing LLUDP.
capability HTTP, and long polling for the first-party client while legacy
viewers keep all three untouched. CLIENT2.md is the
implementation companion, CLIENT2-TRANSPORT.md records
the transport decision, and ROADMAP2.md keeps the detailed
sequence this summarizes.

### Arrival and bootstrap

- [x] Serve the unauthenticated compatibility probe at `GET /v1/version`, reporting protocol versions, grid capabilities, and the welcome region.
- [x] Open world entry for the Homeworldz client at `POST /v1/client/session`.
- [x] Enforce the grid-region protocol handshake in both directions, at registration and at renewal, so the probe's region claims rest on enforced leases.

### The grid channel

- [x] Serve the grid-anchored WebSocket at `GET /v1/client/channel` with first-message token auth, ping/pong, and error envelopes.
- [x] Deliver server-initiated notifications to connected users (system notices via the per-user delivery hub; best-effort, honestly reported).
- [x] Add the first store-and-forward notification kind: instant messages, stored before delivery, delivered live to open channels, and replayed in sent order on the next connection otherwise.
- [ ] Add the remaining store-and-forward kinds.

### The region session

- [x] Decide the transport: TLS + WebSocket now on libwebsockets, with QUIC/WebTransport revisited when the RFC lands or measurements demand it.
- [x] Serve the region-session listener with ticket authentication (validated by a grid round trip.
- [x] Advertise the session per region as data: registration reports the session endpoint, and world entry's capability manifest carries `transports` and `sessionURL`.
- [x] Carry scene traffic: avatar embodiment, object and avatar updates, movement, and client-to-region chat over the session (CLIENT2-EMBODIMENT.md milestone E1; crossings and appearance are its later milestones).
- [x] Carry a session avatar across a region border (CLIENT2-EMBODIMENT.md milestone E2), landing on the arrival point the grid resolved.
- [x] Narrow avatar traffic by interest for sessions: transforms flow only within draw distance, with arrival and departure emitted by a sweep that evaluates both parties' motion.
- [ ] Narrow it for viewers too.
- [ ] Serve home-hosted regions through the call-home relay.
- [ ] Add WebTransport as a second advertised transport when its RFC publishes, per the version-floor rule.

## Phase 10: Modern Client Support

The grid/region back end for what the first-party client can do that a legacy
viewer cannot — served through negotiated region extensions so Firestorm never
sees a change. The
**client itself** — the engine-neutral C++ core and its native and browser
frontends — is developed and tracked in its own repository, with its own
roadmap, status, and progress; phases 9 and 10 here are the server-side surface
it builds against.

- [ ] A canonical avatar body.
- [x] Dress a session avatar for viewers, so it rezzes as a proper avatar rather than a cloud.
- [x] Publish the region's water to session clients.
- [x] Bound the terrain alignment invariant by each sample's own quantization step.
- [x] Rule out LayerData compression as the cause of rough-looking ground in Firestorm.
- [ ] Serve appearance *to* session clients, so they can render each other.
- [ ] Store modern asset formats at rest.
- [ ] Mesh prims server-side, so the client renders one geometry pipeline and prim meshing logic is written once.
- [ ] Extend the session's capability manifest as extensions ship, keeping per-region capabilities data the client adapts to rather than negotiates.
- [ ] Add voice and modern presence surfaces appropriate to the new client.
- [ ] Build creator tooling on the modern pipeline: visual scripting and a modern content workflow (ROADMAP2.md Phase 6).

## Phase 11: Economy and Marketplace

What a creator can sell and how the value moves. Whether a given grid runs an
economy at all is deployment configuration rather than creator tooling, and
lives with the operator's other settings in Phase 7.

- [ ] Define whether credits remain display-only or become a transferable grid balance before implementing paid behavior.
- [ ] If enabled, implement auditable balances, idempotent transactions, object sales, parcel payments, gifts, and refunds.
- [ ] Treat external payment processing and marketplace integration as separate, explicitly approved security projects.
- [ ] Record whether an asset may be sold, distinctly from whether it may be transferred.
