# Homeworldz Roadmap

This roadmap describes the major implementation sequence for Homeworldz. It is
organized at three levels: phases, milestones within each phase, and major work
items within each milestone. [`PLAN.md`](PLAN.md) remains the detailed
engineering checklist, while [`FEATURES.md`](FEATURES.md) records intentional
product differences and the ADRs record architectural decisions. 

Checkboxes describe the present state, not a promise of a release date. A
milestone is complete only when its automated tests and applicable Firestorm
acceptance tests pass.

## Progress snapshot

**Updated 2026-08-08**: These bars are effort-weighted engineering estimates, not
simple checkbox ratios. Later scripting, crossings, social systems, security,
recovery, and scale items are substantially larger than many completed viewer
protocol tasks. Percentages are deliberately approximate and should be revised
when scope or implementation evidence changes.

Two overall bars, because this repository carries two deliverables on
different clocks: the legacy-compatible server platform (phases 1-8, serving
Firestorm and compatible viewers), and the back-end grid/region support for
the modern Homeworldz client (phases 9-10). The client itself is tracked in
its own repository with its own roadmap and progress.

<p>
<label class="roadmap-overall-progress">
  <span>Legacy (Firestorm-compatible) services:</span>
  <progress data-color="primary" max="100" value="37">37%</progress>
  <strong>37%</strong>
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
| 2. Connected Multi-region World | <progress class="roadmap-phase-progress" data-color="primary" max="100" value="86" aria-label="Phase 2 progress: 86%">86%</progress> | 86% |
| 3. Interactive Physical World | <progress class="roadmap-phase-progress" data-color="primary" max="100" value="50" aria-label="Phase 3 progress: 50%">50%</progress> | 50% |
| 4. Mesh and Creator Platform | <progress class="roadmap-phase-progress" data-color="primary" max="100" value="38" aria-label="Phase 4 progress: 38%">38%</progress> | 38% |
| 5. LSL Scripting | <progress class="roadmap-phase-progress" data-color="primary" max="100" value="15" aria-label="Phase 5 progress: 15%">15%</progress> | 15% |
| 6. Social Communications | <progress class="roadmap-phase-progress" data-color="primary" max="100" value="6" aria-label="Phase 6 progress: 6%">6%</progress> | 6% |
| 7. Reliable Operations and Distribution | <progress class="roadmap-phase-progress" data-color="primary" max="100" value="23" aria-label="Phase 7 progress: 23%">23%</progress> | 23% |
| 8. Scale, Compatibility, and Ecosystem | <progress class="roadmap-phase-progress" data-color="primary" max="100" value="3" aria-label="Phase 8 progress: 3%">3%</progress> | 3% |
| 9. Modernized Communications Transport | <progress class="roadmap-phase-progress" data-color="primary" max="100" value="66" aria-label="Phase 9 progress: 66%">66%</progress> | 66% |
| 10. Modern Client Support | <progress class="roadmap-phase-progress" data-color="primary" max="100" value="8" aria-label="Phase 10 progress: 8%">8%</progress> | 8% |

The overall estimate is weighted by expected effort and therefore is not the
arithmetic mean of the phase percentages. The binary checkboxes below remain
the acceptance record; partially implemented work contributes to these bars
but stays unchecked until its complete wording is satisfied.

## Phase 1: Functional Single-region World

### Platform foundation

- [x] Establish the C++20 region server, Go grid service, PostgreSQL central
  state, and SQLite plus filesystem region-local state.
- [x] Define configuration, bootstrap, health, authentication, logging, CI, and
  service contracts.
- [x] Implement region registration, leases, discovery, login sessions, and
  online presence.
- [x] Establish the authoritative fixed-step scene loop and durable scene
  snapshots.

### Viewer connection and appearance

- [x] Complete the minimum supported Firestorm login, UDP circuit, handshake,
  capabilities, event delivery, terrain, chat, and static-object flow.
- [x] Provide default body parts, clothing, Current Outfit links, legacy avatar
  baking, and persistent appearance across relogs.
- [x] Provide a read-only system Library with default avatar and terrain
  content.
- [x] Synchronize nearby avatar presence, movement, appearance rebakes, and
  animation changes between concurrently connected viewers.
- [x] Bake avatar appearance **server-side**, so thin and headless clients rez
  correctly with no client-side baking (ADR 0029); per-user COF baking for
  arbitrary outfits and SSB delivery to full viewers remain.
- [x] Broadcast `KillObject` for a departing avatar through the one teardown
  point every removal path funnels into, so it stops lingering in other
  viewers' views. A lost connection is detected by missed `CompletePingCheck`
  replies (`region.connection_timeout_seconds`, default 60s) rather than the
  grid session TTL, and region shutdown sends each viewer a `KickUser` naming
  the reason.

### Authoritative avatar movement

- [x] Decode movement, camera, jump, and flight controls and persist provisional
  avatar state.
- [x] Stream authoritative position, velocity, and rotation changes
  back to viewers.
- [x] Complete viewer-visible walking, turning, jumping, stopping, flight
  toggling, ascent, and descent without requiring a relog.
- [x] Add animation-state selection and synchronization for standing, walking,
  running, jumping, falling, flying, hovering, and landing.
- [x] Reconcile viewer prediction with the authoritative region position
  without visible snapping or drift — a viewer-side outcome of streaming
  position and velocity, confirmed live in Firestorm rather than in code.

### Basic avatar physics

- [x] Integrate a Jolt avatar capsule into the production scene loop.
- [x] Support persistent viewer terrain editing with live patch updates for
  targeted slope, step, drop, and grounding tests.
- [x] Mirror authoritative terrain into Jolt at startup and replace the collision
  heightfield immediately after viewer edits.
- [x] Sample terrain continuously for provisional grounding rather than
  retaining only the height at the login position.
- [x] Support terrain walking, slopes, steps, falling, jumping, landing, flight,
  and collision-safe motion.
- [x] Collide avatars with static scene objects while preserving practical
  viewer movement behavior.
- [x] Collide avatars with dynamic scene objects while preserving
  practical viewer movement behavior.
- [x] Persist and restore position, orientation, velocity, and flight state;
  safely recompute grounded contact from Jolt on entry.

### Core inventory, assets, and objects

- [x] Implement AIS v3 inventory fetch and mutation, viewer-authored wearables,
  named outfit saving, Library outfit copying, Current Outfit links, folder
  operations, and Trash lifecycle operations.
- [x] Implement free texture upload, required creator provenance,
  content-addressed assets, origin registration, and region replication.
- [x] Implement primitive rez, edit, permissions, ownership, take, delete,
  restore, and restart persistence.
- [x] Implement persistent nonphysical linksets with root and child transforms,
  whole-object and Edit Linked scaling, duplication, take, take-copy, return,
  derez, inventory round trips, and static child collision.
- [x] Implement task inventory (object contents) and complete its permissions,
  mutation, copy, derez, return, and inventory round-trip lifecycle.
- [x] Implement creator-attributed sound and animation uploads; personal
  notecard, gesture, and LSL-source creation; and task notecard and script
  updates, compiling and running the supported Falcon subset on save.
- [x] Complete Firestorm creation, editing, playback, object-contents, restart,
  and relog acceptance for those content types (viewer landmark creation and
  About Land ownership deferred to the Phase 2 land operations).

## Phase 2: Connected Multi-region World

### Region topology and variable size

- [x] Load an operator-owned JSON registry of provisioned regions and
  authenticate region startup by UUID plus per-region access key, returning the
  authoritative name and map coordinates.
- [x] Add authenticated grid-management endpoints to create, inspect, update,
  enable, disable, relocate, remove, and rotate credentials for provisioned
  regions.
- [x] Persist each region's UUID, unique name, owner UUID, X/Y location,
  endpoints, enabled state, and per-region access-key hash independently of its
  online lease.
- [x] Let a region authenticate by UUID or unique name plus its access key and
  fetch effective grid-wide and region-specific startup configuration.
- [x] Represent neighboring regions, coordinates, extents, public endpoints,
  maturity, and online state in grid discovery.
- [x] Support exactly 1x1 (256 m), 2x2 (512 m), and 4x4 (1024 m) provisioned
  Regions in runtime configuration, automated terrain/protocol/map tests, and
  disposable 512 m and 1024 m process-start checks.
- [x] Generalize terrain, physics bounds, viewer coordinates, storage, map
  tiles, and interest management to the three supported sizes.
- [x] Complete initial live Firestorm acceptance of a 2x2 Region as one
  continuous 512 by 512 metre simulator. Movement, terrain, and minimap
  position remained correct through all four quadrants and across both
  internal 256 metre lines.
- [x] Complete 2x2 terrain editing, object persistence, map-idle, and restart
  acceptance in the live Beta Region.
- [x] Repeat the full variable-size acceptance suite in a 4x4 Region.
- [x] Prevent overlaps and invalid neighbor layouts and define behavior beside
  offline or differently sized regions.

### Parcels and local authority

- [x] Implement parcel geometry, ownership, access, landing points, media, and
  object accounting — the full ParcelFlags/Category/LandingType set and a 4 m
  coverage bitmap at all three region sizes, with About Land, subdivide/join,
  and access/ban lists persisted in region SQLite. Per-parcel WindLight is
  deferred to the estate and region settings below.
- [x] Enforce build, rez, entry, script, and object-return policy at
  authoritative boundaries, including walk-in ejection, viewer-initiated return
  to Lost and Found, and periodic `OtherCleanTime` auto-return. The damage and
  push flags are carried but inert until the combat and `llPushObject` systems
  exist (Phase 5).
- [x] Implement estate and region settings needed for terrain, access, maturity,
  and covenant, including the Region/Estate Terrain tab in full — region restart
  and estate kick/teleport-home remain.
- [x] Pin the walkable slope limit with a test — done 2026-08-05,
  `jolt_walkable_slope_test`.
- [ ] Document the rest of the browser-facing API in `api/openapi-public.yaml`,
  which currently covers only the password-reset endpoints.
- [x] Password recovery, grid side — done 2026-08-05 ([ADR 0034](adr/0034-password-recovery.md)): reset token, both endpoints, mail, and `reset_url`.
- [x] Password recovery, management site — done 2026-08-05: the forgot and reset screens plus the login-page link.
- [ ] Trim this roadmap back to point form: one sentence per entry, milestones and
  remaining work only, with narrative moved to ADRs, a content-licensing record, or
  deleted where git history already holds it.
- [ ] Apply permissions recursively and consistently to linksets, object
  contents, attachments, and inventory transfers.

### Teleports and avatar crossings

- [x] Keep avatar appearance, inventory, and Current Outfit stable through an
  explicit teleport between registered regions.
- [ ] Keep avatar appearance stable while sitting and crossing region borders.
- [x] Build an authenticated, idempotent two-region handoff transaction with a
  transit UUID, generation, prepare, accept, activate, and rollback stages.
- [x] Teleport between registered regions with destination validation, viewer
  circuit establishment, arrival placement, source retirement, and durable
  last-location login.
- [x] Teleport within the current Region without creating a Grid transit,
  preserving flight state and returning Firestorm's `TeleportLocal` response.
- [x] Detect avatar border exits, select the online neighbor covering the exact
  mixed-size border coordinate, translate destination-local position, prepare
  the authenticated transit, emit Firestorm's crossing event, contain failed
  exits, and roll back an unactivated crossing after 30 seconds.
- [x] Complete initial live Firestorm acceptance for a two-way 1x1 border
  handoff between Welcome and Sandbox with one continuous viewer session,
  correct edge placement, facing and flight-state transfer, destination
  activation, and source retirement.
- [ ] Complete remote-host failure recovery and reconciliation for interrupted
  teleports.
- [ ] Cross a walking or flying avatar between adjacent regions while preserving
  appearance, controls, velocity, camera, and session continuity.
- [ ] Transfer the complete attachment set with the avatar and prevent duplicate
  activation at source and destination.
- [ ] Handle disconnects, destination failure, retries, stale transit records,
  and reconciliation after process restart.

### Object and vehicle crossings

- [ ] Define an off-region disposition for every moving entity — cross it to an
  accepting neighbor, or bounce, contain, or return it — so nothing continues
  outside all region authority.
- [x] At a border with no eligible online neighbor, constrain avatar and
  physical-object origins to the configured Region extent and cancel outward
  velocity at the crossed edge.
- [x] Resolve border neighbors from persistent grid region records plus their
  current online leases before choosing crossing versus containment.

- [ ] Cross individual objects and complete linksets without changing creator,
  owner, permissions, inventory, or physical state.
- [ ] Cross scripted and unscripted attachments as part of their avatar bundle.
- [ ] Cross vehicles while preserving linear and angular motion, vehicle
  parameters, and object inventory.
- [ ] Transfer a vehicle and all seated avatars as one coordinated bundle, with
  no passenger briefly becoming authoritative in both regions or neither.
- [ ] Establish event and collision cutoffs so crossing does not duplicate or
  silently lose observable actions.

### World navigation

- [x] Generate live terrain-derived region tiles and compose world-map zoom
  levels for 1x1 regions.
- [x] Extend region and world-map tile composition to the planned 2x2 and 4x4
  region sizes.
- [x] Implement viewer map-block and prefix-name discovery for registered live
  regions.
- [x] Implement landmark resolution, home location, and teleport routing.
  Login-to-home currently lands in the home region at the last in-region
  position; exact home-coordinate placement on login is deferred within this
  phase.
- [x] Serve region land data (ParcelProperties: owner, flags, area, bitmap,
  landing point, prim accounting) so the viewer's "Landmark This Place" creation
  and About Land ownership work. Delivered over the Event Queue as LLSD; full
  parcel geometry and local authority landed early with the Parcels work above.
- [ ] Add region and parcel search sufficient to find and reach destinations.
- [ ] Show friends and authorized users useful presence and location without
  leaking restricted information.

### Inventory asset durability

**Landed 2026-07-28.** A region that dies no longer takes its users' inventory
with it: the vault holds verified bytes for every inventory-referenced asset's
whole closure, and the grid refuses any commit it cannot make durable first.
The layer separation below lands before the write-through that fills the vault,
because re-keying an empty vault is free
([ADR 0026](adr/0026-vault-authoritative-inventory-assets.md),
[ADR 0027](adr/0027-asset-blob-instance-separation.md)).

- [x] Separate the blob, asset, and instance layers of
  [ADR 0027](adr/0027-asset-blob-instance-separation.md): a grid-assigned
  `blob_id` names bytes, the digest demotes to an integrity checksum, locations
  attach to blobs rather than assets, and back-link reference counts decide
  retention. `blob_id` stays grid-internal, so what a region speaks is
  unchanged.

- [x] Implement the grid asset vault — a durable, replica-only blob store that
  never originates assets, never hosts agents, and is never in the viewer fetch
  path (ADR 0026), with idempotent ingest that verifies checksum and length
  before an atomic rename.
- [x] Enforce the vault invariant grid-side: commit an inventory item only once
  the vault holds verified bytes for its whole reference closure, gathered by
  parsing the vault's own copy, so durability never depends on a region.
- [x] Treat region copies of vault-held assets as an evictable cache and
  scene-only assets as region-owned, demoting region-to-region fetch to an
  optimization; a region also materializes its scene's closure locally, so a
  backup of its own storage is self-contained.
- [x] Backfill existing inventory-referenced assets into the vault from live
  registered locations and report assets that are already unfetchable
  (`cmd/vaultbackfill`, idempotent, closure-aware; the first live run
  ingested 85 blobs and named 19 assets as already lost).
- [ ] Tier rarely accessed vault blobs onto slower S3-compatible storage with
  hash re-verification on rehydration, keeping tiering vault-internal.

## Phase 3: Interactive Physical World

### Production physics integration

- [x] Make Jolt the default production physics world while retaining the
  engine-independent plugin boundary.
- [x] Create, update, sleep, wake, remove, and restore physical bodies from
  authoritative scene changes.
- [x] Synchronize physical transforms and velocities to viewers at suitable
  rates with interest-aware throttling.
- [x] Exclude phantom objects from collision and implement an authoritative,
  nonpersistent 60-second temporary-on-rez lifecycle with viewer kill updates.
- [ ] Complete collision filtering, material behavior, volume detection, and
  collision events.
- [x] Represent physical linksets as compound Jolt bodies with correct child
  shapes, mass properties, collision behavior, transforms, and persistence.
- [ ] Complete live Firestorm acceptance for compound collision, falling and
  rotation, editing, delinking, and restart persistence.
- [x] Verify deterministic-enough restart and handoff behavior through shared
  physics acceptance scenarios.

### Attachments and sitting

- [ ] Attach inventory objects to named avatar attachment points with stable
  local transforms, permissions, ownership, and persistence.
- [ ] Represent worn attachments as part of the authoritative avatar bundle and
  restore them on login.
- [ ] Implement sit targets, avatar seating, unsit, camera placement, and seated
  animation state.
- [ ] Support avatars as seated attachments to object linksets so their world
  transforms follow the root object correctly.
- [ ] Define lifecycle ordering for attachment, seated-avatar, physics, viewer,
  and later script events.

## Phase 4: Mesh and Creator Platform

What a creator needs before scripting matters: the content pipeline itself —
mesh and its collision sources, uploads and validation, inventory breadth, and
the economy boundary that decides whether creations can be sold. Scripting
operates on this content, which is why it now follows rather than precedes it.

### Mesh pipeline

Decided in [ADR 0033](adr/0033-mesh-pipeline-gltf-canonical.md): glTF (GLB) is
the canonical stored format, the creator's upload is never rewritten, and each
client family is served a derived rendition by a grid-side conversion worker.

- [x] M1 static mesh — complete 2026-07-29: a GLB uploaded through the session
  path stands in-world in Firestorm as a solid textured mesh, served over the
  GetMesh capabilities and rezzed on Jolt, with `homeworldz-meshsmith` deriving
  the type-49 rendition. Viewers render mesh only when the SimulatorFeatures
  `MeshRezEnabled` flag is set.
- [x] M2 Firestorm mesh uploads — complete 2026-07-29: the mesh branch of
  `NewFileAgentInventory` stores viewer-written type-49 payloads verbatim as
  canonical assets, and the `gltf` rendition derived from them (2026-07-30)
  lets each client family fetch one asset id and receive the form it can read.
- [ ] M3 material and texture renditions: glTF material JSON (type 57) for
  PBR-capable viewers, needing the `ModifyMaterialParams`,
  `UpdateMaterialAgentInventory` and `UpdateMaterialTaskInventory` capabilities.
- [ ] Terrain surface for session clients — remaining: decide the blend *curve*,
  the width and layer selection already being published.
- [x] Close the texture pipeline's asymmetry — live 2026-07-31: the
  `png-texture` rendition decodes canonical JPEG2000 and re-encodes lossless
  PNG, so textures created in Firestorm are readable by the first-party client.
  It recovers the stored pixels, not the detail the viewer's uploader
  discarded, so uploading through the client remains better for new art.
- [ ] V-HACD convex decomposition for mesh physics; the shipped physics
  block is the conservative bounding-box hull.
- [x] Regenerate stale renditions — live 2026-07-29: a generator column records
  which converter produced each rendition and the grid re-queues everything an
  older one produced, so deploying an upgraded converter reconverts existing
  content.
- [ ] M4 rigged mesh: glTF skins mapped onto the Bento skeleton (refusing
  rigs that do not map), attachments and body wearables. **Uploads are accepted
  as of 2026-08-08** and the reference Bento body converts and agrees with the
  skeleton — 16 joints agreed, 0 disagreed, worst 0.75 mm, below the skeleton's
  own 2 mm left/right asymmetry. Joint names resolve through the alias table or
  the upload is refused naming the joint, unused joints compact away, and the
  bind geometry is checked against the rest pose with a tolerance bracketed
  between that 2 mm and the 7.81 mm closest distinguishable joint pair.
  Turning acceptance on required two corrections, both found by measuring that
  body rather than by reading code: the axis map had only ever chosen *which
  axis is up* and left the lateral axis where it found it (a 90° yaw — see
  [ADR 0033](adr/0033-mesh-assets.md)), and inverse bind matrices were not
  mapped at all, which draws a body correctly at rest and deforms it wrongly on
  the first animation. **Attachments ship as of 2026-08-08** — wearing, taking
  off, and worn state kept on the grid so it survives a relog into a different
  region — though objects wear at the joint, since the offset an object was
  taken off at is not stored yet. Remaining: **Firestorm verification of a worn
  body**, body wearables, and **wiring the geometric check into the upload
  path** — it runs in the diagnostic tool today, so uploads are still accepted on
  names, joint counts and influence sets alone. A rig it cannot discriminate is
  accepted (decided 2026-08-08, provisional). `maxRigInfluences` is enforced and
  no ordinary export can trip it: glTF carries four influences per joint set by
  convention and no exporter emits a second set unaided, so four is the default
  rather than a ceiling content approaches.
- [ ] M5 import breadth: client-side FBX/OBJ/DAE import, documented Daz
  Studio export path, optional web import service on the management site.
- [ ] Rig retargeting, so a creator does not need Blender with Avastar or
  Bento Buddy to bring a body in
  ([AUTO-RIGGING.md](AUTO-RIGGING.md)). Design sketch only, nothing scheduled.
  The ranking there is the useful part and is the reverse of how the three
  cases sound: retargeting a body that is **already rigged** to some other
  skeleton is both the most common case and the most tractable, because the
  weights already exist and only the correspondence is missing. Transferring
  weights to an unrigged humanoid comes second and now has its prerequisite —
  a verified reference body. Fitting a skeleton to an arbitrary unrigged mesh
  is an open problem, recorded as open. Whatever is built derives a
  **rendition** rather than rewriting the upload, so a wrong early algorithm
  costs nothing and improving it reconverts existing content instead of asking
  creators to send their files again.
- [ ] Possible with M5: retarget a Character Creator rig onto the Bento
  skeleton — the worked example of the case above. CC bodies name their joints
  `CC_Base_*` and are refused today, and
  a rename is not enough — the skeletons differ in bind pose, so weights must
  be retargeted and CC's twist bones merged into their nearest mapped ancestor.
  The format can carry it: a skin's joint position overrides
  (`mAlternateBindMatrix`) let a body ship its own proportions — which is why a
  retarget must **write** those rather than flatten the skeleton to Bento's
  rest pose, the one instruction most likely to be got backwards. Licensing is
  the gate rather than the code — Reallusion's grant is non-transferable, so
  this is a path for a licensee to upload their own creation, never for the
  project to ship a body.

### Content creation and inventory breadth

- [ ] Complete viewer building workflows for linksets, materials, sculpt,
  animation, sound, gesture, notecard, landmark, and script content.
- [ ] Store portable mesh collision sources separately from visual LODs; build
  validated static triangle shapes or dynamic convex compounds through the
  selected physics adapter, with immutable collision capture for deforming
  meshes and non-colliding attachments by default.
- [ ] Implement uploads, validation, dependencies, creator attribution, asset
  replication, and inventory creation for each supported asset type.
- [x] Add viewer-authored wearable creation, editing, and named outfit saving
  beyond the initial default-avatar flow.
- [ ] Provide bulk inventory, search, copy, transfer, export-policy, recovery,
  and large-inventory performance behavior.

### Economy and marketplace boundary

What a creator can sell and how the value moves. Whether a given grid runs an
economy at all is deployment configuration rather than creator tooling, and
lives with the operator's other settings in Phase 7.

- [ ] Define whether credits remain display-only or become a transferable grid
  balance before implementing paid behavior.
- [ ] If enabled, implement auditable balances, idempotent transactions, object
  sales, parcel payments, gifts, and refunds.
- [ ] Treat external payment processing and marketplace integration as separate,
  explicitly approved security projects.
- [ ] Record whether an asset may be sold, distinctly from whether it may be
  transferred. Legacy permissions have no "give it away but do not sell it",
  which is exactly the shape a licence like Character Creator's needs: its
  holder may publish a creation to an interactive service but not sell it in a
  third-party marketplace. Lands with its enforcement — a listing check and the
  in-world set-for-sale path — rather than ahead of it. The `exportable` column
  beside it has been declared and read by nothing since it was added, which is
  the failure this must avoid: a flag that reads as policy and enforces
  nothing. Whether it propagates to derivatives is the open question, and is
  the same closure problem asset durability already answers for bytes.

## Phase 5: LSL Scripting

### Language and compiler

- [x] Establish the dependency-free handwritten Falcon lexer, parser, semantic
  analyzer, versioned bytecode format, compiler, and automated proof-of-concept
  suite for an initial typed LSL subset.
- [x] Return Falcon compilation success and escaped error arrays through the
  Firestorm task-script capability protocol, including line and column locations
  for lexical errors.
- [ ] Inventory the complete Second Life LSL language and built-in surface plus
  Halcyon/InWorldz extensions, explicitly excluding OpenSimulator extensions.
- [ ] Complete the handwritten lexer, parser, semantic analysis, diagnostics,
  and versioned Homeworldz bytecode compiler for that full supported language.
- [x] Store creator-attributed LSL source in personal and task inventory, with
  Firestorm creation, retrieval, editing, saving, and drag-to-contents behavior.
- [ ] Cache immutable bytecode by source hash, compiler version, and runtime ABI.
- [ ] Build compatibility tests for syntax, types, conversions, lists, strings,
  states, constants, built-ins, and observable errors.

### Cooperative runtime and resource control

- [x] Integrate the single-threaded C++ Falcon bytecode VM into the authoritative
  Region thread with explicit instruction-level execution state and no native
  thread per script.
- [x] Apply bounded aggregate and per-script instruction slices on every Region
  tick so an infinite loop yields cooperatively instead of blocking the world.
- [ ] Schedule scripts fairly using bounded weighted instruction and wall-clock
  budgets across scripts, objects, owners, and parcels.
- [ ] Enforce memory, stack, call-depth, event-queue, string, list, payload,
  owner, object, and parcel limits.
- [ ] Make slow host operations asynchronous and represent waits as serializable
  tokens or continuations.
- [ ] Add operator metrics, throttling, diagnostics, stopping, resetting, and
  isolation for inefficient or faulty scripts.

### LSL Events and Region Interaction

- [x] Decode Firestorm `RezScript`, create or transfer the task inventory item,
  compile its source, instantiate an enabled VM, and dispatch `state_entry`.
- [x] Recompile task scripts after Firestorm edits, preserve the previous running
  instance after a failed compile, honor the viewer's running flag, and remove
  the live VM when its task inventory item is deleted.
- [x] Route the initial `llSay` and `llOwnerSay` host calls to Firestorm object
  chat with owner-only and distance behavior, confirmed in the live cloud Grid.
- [x] Advertise the `SCRIPTED` and `HANDLE_TOUCH` object-update flags so
  Firestorm enables Touch, then dispatch `touch_start` from `ObjectGrab` —
  resolving the clicked child and linkset root — through a bounded per-script
  event queue, distinct from the physical `ObjectGrabUpdate` drag path.
- [ ] Implement the remaining object lifecycle, sustained/ended touch, timer,
  listen, sensor, control, permission, inventory, changed, link-message,
  collision, land-collision, attachment, and moving events.
- [ ] Implement bounded LSL host functions for scene, physics, inventory,
  communication, parcel, avatar, HTTP, and data operations.
- [ ] Preserve Second Life event ordering and delay semantics where observable
  and document intentional Homeworldz differences.
- [ ] Integrate script ownership and permissions with linksets, attachments,
  seated avatars, parcels, and estate policy.

### Script persistence and crossings

- [x] Demonstrate automated Falcon snapshots after every completed instruction,
  restoration into a fresh VM, preservation of globals, and continuation from
  the middle of a `touch_start` handler.
- [ ] Restore enabled task scripts across Region restarts. Startup now re-rezzes
  enabled task scripts so they run and re-advertise touch, but each restart still
  re-runs `state_entry` because VM state is not yet persisted; full state-carrying
  restoration remains outstanding.
- [ ] Serialize bytecode identity, instruction pointer, stacks, frames, globals,
  current event, event queue, timers, listens, permissions, and pending work in
  a compact versioned binary format.
- [ ] Integrate stop-and-restore after any completed bytecode instruction into
  live task scripts and Region persistence without relying on the native C++
  stack.
- [ ] Snapshot scripts atomically with their attachment, object, or vehicle
  physics bundle.
- [ ] Cross heavily scripted attachments and vehicles within defined latency,
  memory, duplication, and event-loss limits.
- [ ] Version the runtime ABI and provide safe upgrade, incompatibility, and
  rollback behavior for stored script state.

### Vehicles and physical objects

- [x] Implement stable dynamic-object movement, editing, taking, and restoration
  without losing physics state.
- [ ] Add the Second Life vehicle parameter model required by LSL vehicles.
- [ ] Make a single `llSetVehicleType(VEHICLE_TYPE_*)` call activate a usable
  SL/Halcyon-compatible car, sled, boat, airplane, balloon, sailboat, or motorcycle
  preset; map presets and later parameter overrides to each physics plugin's
  native vehicle, motor, and constraint facilities.
- [ ] Synchronize driver controls, vehicle motion, cameras, passengers, and
  seated-avatar transforms.
- [ ] Preserve object, linkset, inventory, permission, passenger, and physical
  state as one transferable vehicle bundle.
- [ ] Add load, tunneling, stacking, recovery, and abusive-object safeguards.

## Phase 6: Social Communications

Who people are to each other, and how they reach each other: identity and
profiles, direct and group messaging, voice, friendship, and the group and
role machinery that shared ownership rests on.

### Identity, profiles, and communication

- [ ] Implement user-visible names, profiles, interests, images, privacy, and
  account administration.
- [ ] Implement direct messages, offline messages, group chat, conference chat,
  mute/block behavior, and delivery history where appropriate.
- [ ] Provide voice via **WebRTC** — the direction Second Life and current
  viewers (including Firestorm) are moving to. Vivox is explicitly not pursued.
  Lower priority than server-side baking, but wanted sooner rather than later.
- [ ] Implement friendship, calling cards, presence permissions, and offers.
- [ ] Add abuse reporting and the minimum moderation evidence needed by grid
  operators.

### Groups, roles, and shared ownership

- [ ] Implement groups, roles, powers, membership, invitations, notices, and
  group communication.
- [ ] Support group-owned land and objects without weakening creator provenance
  or transfer permissions.
- [ ] Apply group powers consistently to parcels, estates, object editing,
  inventory sharing, and moderation.
- [ ] Audit sensitive group and ownership changes.

## Phase 7: Reliable Operations and Distribution

### Known hazards on the current deployment

- [ ] **Nothing watches free disk space, on a host that is the only home for the
  grid, the API, the conversion worker, and all four regions.** Measured
  2026-07-31 at 86 GB free of 96 GB, so not urgent — but a process that fills
  the disk takes the whole grid with it, and regions would fail before any
  warning. A free-space threshold in region health reporting is the cheap
  answer.

- [x] Treat presence as a lease rather than durable state, so a session that
  dies without logging out cannot stay "present" — verified 2026-08-06 against
  the live grid: a row planted ten minutes stale is answered 404 by the per-user
  route and swept by the list route (`presence.StaleAfter`, 90s). A stale row
  left in the table is inert, not a claim.

### Grid and region packages

- [x] Produce separate versioned grid-owner and region-owner packages containing
  prebuilt executables, runtime dependencies, examples, bootstrap tools, and
  end-user installation guides.
- [ ] Support clean install, unattended install, upgrade, downgrade where safe,
  uninstall, and configuration preservation.
- [ ] Sign release artifacts, publish checksums and provenance, and generate a
  machine-readable release manifest.
- [ ] Validate supported Windows and Linux installations without requiring a
  source checkout or development toolchain.

### Backups, upgrades, and reconciliation

- [x] Restart or replace the central grid service without restarting connected
  regions, retaining PostgreSQL-backed viewer sessions — which holds only while
  grid services return inside a region's lease-renewal window.

- [x] Report one version everywhere, dated and traceable: `0.<progress>.<yymmdd>`
  in the root `VERSION` file, where progress is the sum of the two bars above
  and never decreases (`scripts/update-version.sh`); binaries are stamped from
  that file plus the short commit, and an unstamped build says so.
- [x] Refuse to start a grid service against a database older than the build
  requires, naming both versions and the fix, so a pending migration cannot go
  unnoticed; each migration stamps its own version and a test enforces that.
- [ ] Back up and restore PostgreSQL grid state, region SQLite state, assets,
  terrain, configuration, and compatible runtime state.
- [ ] Export and import OpenSim-compatible region archives (OAR) and user
  inventory archives (IAR). OAR is the portable scene backup and migration
  format; IAR covers user inventory transfer.
- [ ] Write only the latest supported OAR and IAR format versions while
  reading older format versions where practical, since archives are
  long-lived files that outlive the software that wrote them.
- [ ] Test full-grid, single-region, and selected-user recovery with documented
  recovery-point and recovery-time expectations.
- [ ] Version schemas and protocols and support rolling grid and region upgrades
  within a documented compatibility window.
- [ ] Reconcile leases, presence, inventory, assets, crossings, and duplicated or
  orphaned state after crashes or partial restores.

### Observability and administration

- [ ] Provide metrics, structured logs, traces, health detail, dashboards, and
  actionable alerts for grid and region owners.
- [ ] Add command-line and authenticated web administration for users, regions,
  estates, assets, inventory repair, scripts, crossings, and moderation.
- [ ] Make the economy an operator setting: enable or disable it per grid, keep
  texture uploads free, preserve a useful no-economy deployment mode, and
  provide the controls a running economy needs — limits, freezes, corrections,
  and their audit trail. The mechanics themselves are Phase 4.
- [ ] Record tamper-evident audit events for privileged and security-sensitive
  operations.
- [ ] Define capacity indicators and load-shedding behavior before a region
  becomes unresponsive.

### Security and deployment hardening

- [ ] Add transport encryption, service identity, credential rotation, scoped
  authorization, secret-management guidance, and secure defaults for non-local
  deployments.
- [ ] Validate all viewer, inter-region, asset, inventory, script, and operator
  inputs against resource-exhaustion and malformed-data attacks.
- [ ] Add dependency, artifact, and configuration scanning plus a vulnerability
  response and supported-version policy.
- [ ] Perform fault-injection, abuse, denial-of-service, and recovery testing
  before describing a release as production-ready.

## Phase 8: Scale, Compatibility, and Ecosystem

### Performance and scale

- [ ] Establish repeatable concurrency, scene-complexity, physics, inventory,
  asset, crossing, script, and network benchmarks.
- [ ] Implement interest management, packet prioritization, backpressure, and
  adaptive update rates for crowded or complex regions.
- [ ] Scale central services horizontally where measurements justify it while
  keeping each region's authority unambiguous.
- [ ] Publish tested capacity envelopes rather than relying on nominal limits.

### Compatibility

- [ ] Maintain conformance tests against the pinned supported Firestorm release
  and evaluate newer releases deliberately.
- [ ] Add read-only legacy inventory access only if its older-viewer benefit
  justifies the maintenance cost; AIS v3 remains authoritative.
- [x] Support thin and headless clients such as LibreMetaverse: advertise the
  per-region `FetchInventoryDescendents2`/`FetchLibDescendents2` capabilities
  and keep the HTTP asset-fetch capabilities LMV-compatible
  (see `tools/testclient/README.md`).
- [ ] Validate Halcyon/InWorldz LSL extensions without admitting OpenSimulator
  scripting extensions accidentally.
- [ ] Document import and migration tools separately from live legacy service or
  database compatibility.

### Physics and service extensions

- [ ] Promote the existing PhysX 5 adapter to an optional supported physics
  plugin after it passes the same production scenarios as Jolt.
- [ ] Stabilize versioned plugin contracts only for boundaries with demonstrated
  operational value.
- [ ] Define safe extension points for grid services without exposing region
  authority or script execution to untrusted in-process plugins.
- [ ] Maintain deterministic transfer and persistence contracts across every
  supported physics implementation.

### Release readiness

- [ ] Publish administrator, region-owner, creator, scripter, and contributor
  documentation appropriate to the supported feature set.
- [ ] Run sustained multi-region worlds with real viewers, scripts, attachments,
  vehicles, failures, upgrades, and restores.
- [ ] Resolve all release-blocking correctness, data-loss, permissions,
  crossing, security, and viewer-compatibility findings.
- [ ] Define the supported platform matrix, compatibility guarantees, upgrade
  policy, and long-term maintenance expectations for the first stable release.

## Phase 9: Modernized Communications Transport

The modern client-facing wire surface: REST bootstrap, a grid-anchored
notification channel, and a region-anchored session — replacing LLUDP,
capability HTTP, and long polling for the first-party client while legacy
viewers keep all three untouched. [CLIENT2.md](CLIENT2.md) is the
implementation companion, [CLIENT2-TRANSPORT.md](CLIENT2-TRANSPORT.md) records
the transport decision, and [ROADMAP2.md](ROADMAP2.md) keeps the detailed
sequence this summarizes.

### Arrival and bootstrap

- [x] Serve the unauthenticated compatibility probe at `GET /v1/version`,
  reporting protocol versions, grid capabilities, and the welcome region.
- [x] Open world entry at `POST /v1/client/session`: destination resolution on
  the shared arrival logic, a session in the store viewer logins share, and a
  short-lived region-scoped ticket so the account token never reaches a region.
- [x] Enforce the grid-region protocol handshake in both directions, at
  registration and at renewal, so the probe's region claims rest on enforced
  leases.

### The grid channel

- [x] Serve the grid-anchored WebSocket at `GET /v1/client/channel` with
  first-message token auth, ping/pong, and error envelopes.
- [x] Deliver server-initiated notifications to connected users (system
  notices via the per-user delivery hub; best-effort, honestly reported).
- [x] Add the first store-and-forward notification kind: instant messages,
  stored before delivery, delivered live to open channels, and replayed in
  sent order on the next connection otherwise.
- [ ] Add the remaining store-and-forward kinds — inventory offers and
  friendship requests — which need producers and tables that do not exist
  yet.

### The region session

- [x] Decide the transport: TLS + WebSocket now on libwebsockets, with
  QUIC/WebTransport revisited when the RFC lands or measurements demand it
  ([CLIENT2-TRANSPORT.md](CLIENT2-TRANSPORT.md)).
- [x] Serve the region-session listener with ticket authentication (validated
  by a grid round trip — the signing secret never reaches a region), hello,
  heartbeat, and the region's public chat delivered server-initiated.
- [x] Advertise the session per region as data: registration reports the
  session endpoint, and world entry's capability manifest carries
  `transports` and `sessionURL`.
- [x] Carry scene traffic: avatar embodiment, object and avatar updates,
  movement, and client-to-region chat over the session
  ([CLIENT2-EMBODIMENT.md](CLIENT2-EMBODIMENT.md) milestone E1; crossings
  and appearance are its later milestones). Object updates were only half
  carried until 2026-08-08: physics-driven motion streamed correctly, while
  every change made from a *viewer* — rez, move, link, texture, and eight
  others — reached other viewers and no session client. Found by the client
  team asking what the channel was supposed to carry, not by a test here.
- [x] Carry a session avatar across a region border
  ([CLIENT2-EMBODIMENT.md](CLIENT2-EMBODIMENT.md) milestone E2), landing on the
  arrival point the grid resolved — deliberately not atomic the way a viewer's
  handoff is.
- [x] Narrow avatar traffic by interest for sessions: transforms flow only
  within draw distance, with arrival and departure emitted by a sweep that
  evaluates both parties' motion
  ([CLIENT2-EMBODIMENT.md](CLIENT2-EMBODIMENT.md)).
- [ ] Narrow it for viewers too. The mechanism is the same, but it changes
  what a legacy viewer sees (bodies killed and re-rezzed at the boundary),
  so it needs a **manual Firestorm regression pass** before shipping —
  viewers stay region-wide until then.
- [ ] Serve home-hosted regions through the call-home relay — an outbound
  connection to the grid in place of a listening socket and a certificate —
  with direct service preferred and verified by dial-back.
- [ ] Add WebTransport as a second advertised transport when its RFC
  publishes, per the version-floor rule.

## Phase 10: Modern Client Support

The grid/region back end for what the first-party client can do that a legacy
viewer cannot — served through negotiated region extensions so Firestorm never
sees a change ([ADR 0032](adr/0032-region-extensions-for-new-client.md)). The
**client itself** — the engine-neutral C++ core and its native and browser
frontends — is developed and tracked in its own repository, with its own
roadmap, status, and progress; phases 9 and 10 here are the server-side surface
it builds against.

- [ ] A canonical avatar body — blocked on content licensing rather than code:
  the legacy body and skeleton are viewer-licensed, and a wearable body must be
  re-rigged to the viewer's own named joints: 133 bones plus 26 collision
  volumes, all 159 of which may legally appear in a skin. **Corrected from 71
  on 2026-08-08**, which counted the pre-Bento skeleton no current viewer runs.
  A re-rig cannot recover from being aimed at a skeleton half the size of the
  real one, and this line is where a re-rigger would have read it.
- [x] Dress a session avatar for viewers: spawn seeds the server-side
  default-outfit bake and derives body geometry from it, so a viewer rezzes
  a properly shaped, clothed avatar rather than a default one
  ([CLIENT2-EMBODIMENT.md](CLIENT2-EMBODIMENT.md) milestone E3, viewer half).
- [x] Publish the region's water to session clients — live 2026-07-31: both the
  viewer and session paths now read `region.water_height` (default 20 m), and
  the hello states `water: {height}`.
- [x] Bound the terrain alignment invariant by each sample's own quantization
  step instead of a flat 1 cm tolerance, since Jolt stores every height in 8
  bits scaled to its 2×2 block's range measured over a 3×3 span — tighter on
  flat ground, roomy only beside a cliff.
- [x] Rule out LayerData compression as the reason ground looks rougher in
  Firestorm than in the client's own render: an independent decode returns 50 m
  of rise inside one patch within 0.12 m, so both families draw the same
  heights and the difference is in the drawing.
- [ ] Serve appearance *to* session clients, so they can render each other —
  the remaining half of E3, waiting on the asset formats below rather than
  on legacy texture-entry blobs.
- [ ] Store modern asset formats at rest — KTX2 textures, glTF meshes — with
  down-conversion serving legacy viewers what they expect; the mesh half is the
  Phase 4 pipeline of
  [ADR 0033](adr/0033-mesh-pipeline-gltf-canonical.md) and KTX2 becomes one
  more rendition kind on it.
- [ ] Mesh prims server-side, so the client renders one geometry pipeline and
  prim meshing logic is written once.
- [ ] Extend the session's capability manifest as extensions ship, keeping
  per-region capabilities data the client adapts to rather than negotiates.
- [ ] Add voice and modern presence surfaces appropriate to the new client.
- [ ] Build creator tooling on the modern pipeline: visual scripting and a
  modern content workflow ([ROADMAP2.md](ROADMAP2.md) Phase 6).
