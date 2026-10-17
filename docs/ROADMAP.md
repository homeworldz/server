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
  <progress data-color="primary" max="100" value="47">47%</progress>
 <strong>47%</strong>
</label>
</p>

<p>
<label class="roadmap-overall-progress">
 <span>Modern Homeworldz client + back-end:</span>
  <progress data-color="primary" max="100" value="38">38%</progress>
 <strong>38%</strong>
</label>
</p>

| Phase | Progress | Estimate |
| --- | --- | ---: |
| 1. Functional Single-region World | <progress class="roadmap-phase-progress" data-color="primary" max="100" value="100" aria-label="Phase 1 progress: 100%">100%</progress> | 100% |
| 2. Connected Multi-region World | <progress class="roadmap-phase-progress" data-color="primary" max="100" value="97" aria-label="Phase 2 progress: 97%">97%</progress> | 97% |
| 3. Interactive Physical World | <progress class="roadmap-phase-progress" data-color="primary" max="100" value="50" aria-label="Phase 3 progress: 50%">50%</progress> | 50% |
| 4. Mesh and Creator Platform | <progress class="roadmap-phase-progress" data-color="primary" max="100" value="50" aria-label="Phase 4 progress: 50%">50%</progress> | 50% |
| 5. Social Communications | <progress class="roadmap-phase-progress" data-color="primary" max="100" value="6" aria-label="Phase 5 progress: 6%">6%</progress> | 6% |
| 6. LSL Scripting | <progress class="roadmap-phase-progress" data-color="primary" max="100" value="19" aria-label="Phase 6 progress: 19%">19%</progress> | 19% |
| 7. Reliable Operations and Distribution | <progress class="roadmap-phase-progress" data-color="primary" max="100" value="25" aria-label="Phase 7 progress: 25%">25%</progress> | 25% |
| 8. Scale, Compatibility, and Ecosystem | <progress class="roadmap-phase-progress" data-color="primary" max="100" value="3" aria-label="Phase 8 progress: 3%">3%</progress> | 3% |
| 9. Modernized Communications Transport | <progress class="roadmap-phase-progress" data-color="primary" max="100" value="66" aria-label="Phase 9 progress: 66%">66%</progress> | 66% |
| 10. Modern Client Support | <progress class="roadmap-phase-progress" data-color="primary" max="100" value="10" aria-label="Phase 10 progress: 10%">10%</progress> | 10% |
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
- [x] Bake avatar appearance server-side, so thin and headless clients rez correctly with no client-side baking.
- [x] Bake the wearer's own Current Outfit rather than one fixed outfit, cached per outfit.
- [x] Remove a departing avatar from other viewers' views through a single teardown path.
- [x] Support alpha layers for viewers, so a mesh body hides the default body.
- [x] Apply alpha layers in the server-side bake.
- [x] Re-bake a wearer automatically when their Current Outfit changes mid-session.
- [x] Deliver server-side bakes to full viewers on every region, so Firestorm stops baking for itself.

### Authoritative avatar movement

- [x] Decode movement, camera, jump, and flight controls and persist provisional avatar state.
- [x] Stream authoritative position, velocity, and rotation changes back to viewers.
- [x] Complete viewer-visible walking, turning, jumping, stopping, flight toggling, ascent, and descent without requiring a relog.
- [x] Add animation-state selection and synchronization for standing, walking, running, jumping, falling, flying, hovering, and landing.
- [x] Reconcile viewer prediction with the authoritative region position without visible snapping or drift.

### Basic avatar physics

- [x] Integrate a Jolt avatar capsule into the production scene loop.
- [x] Support persistent viewer terrain editing with live patch updates.
- [x] Mirror authoritative terrain into Jolt at startup and after every viewer edit.
- [x] Sample terrain continuously for grounding.
- [x] Support terrain walking, slopes, steps, falling, jumping, landing, flight, and collision-safe motion.
- [x] Collide avatars with static and dynamic scene objects.
- [x] Persist and restore position, orientation, velocity, and flight state.

### Core inventory, assets, and objects

- [x] Implement AIS v3 inventory fetch and mutation, wearables, outfits, Current Outfit links, folder operations, and Trash.
- [x] Implement free texture upload, creator provenance, content-addressed assets, origin registration, and region replication.
- [x] Implement primitive rez, edit, permissions, ownership, take, delete, restore, and restart persistence.
- [x] Implement persistent nonphysical linksets, including linked editing, duplication, take, return, and static child collision.
- [x] Implement task inventory (object contents) with its full permissions and copy lifecycle.
- [x] Implement sound and animation uploads, notecard, gesture, and script creation, and compiling scripts on save.
- [x] Complete Firestorm acceptance for creating, editing, and playing those content types.

## Phase 2: Connected Multi-region World

### Region topology and variable size

- [x] Load a registry of provisioned regions and authenticate region startup by UUID and access key.
- [x] Add grid-management endpoints to create, inspect, update, enable, disable, relocate, remove, and rotate credentials for regions.
- [x] Persist each region's identity, name, owner, location, endpoints, enabled state, and access-key hash independently of its lease.
- [x] Let a region fetch its grid-wide and region-specific startup configuration.
- [x] Represent neighboring regions, coordinates, extents, endpoints, maturity, and online state in grid discovery.
- [x] Support 2x2 and 4x4 regions as one continuous simulator, with terrain, physics, storage, map tiles, and interest management.
- [x] Prevent overlapping regions and define behavior beside offline neighbors.
- [x] Provision rectangular regions ([ADR 0036](adr/0036-rectangular-regions-via-facet-presentation.md)).
- [x] Run a large region as one simulation presented to viewers as 256 m facets.
- [x] Keep a standing child circuit to every facet, so crossing an internal line is a promotion rather than a handoff.
- [x] Rebase positions, chat, edits, and teleports across facet lines.
- [x] Hold an avatar at a map edge it cannot leave by, and let it walk away again.

### Parcels and local authority

- [x] Implement parcel geometry, ownership, access, landing points, media, and object accounting.
- [x] Enforce build, rez, entry, script, and object-return policy, including ejection and auto-return.
- [x] Implement the estate and region settings for terrain, access, maturity, and covenant.
- [x] Document the browser-facing API in `api/openapi-public.yaml`.
- [x] Support password recovery on the server and the management site.
- [ ] Apply permissions recursively and consistently to linksets, object contents, attachments, and inventory transfers.

### Teleports and avatar crossings

- [x] Keep appearance, inventory, and Current Outfit stable through a teleport between regions.
- [ ] Keep avatar appearance stable while sitting and crossing region borders.
- [x] Build an idempotent two-region handoff transaction.
- [x] Teleport between regions with arrival placement, source retirement, and last-location login.
- [x] Teleport within a region without a grid transit.
- [x] Cross a walking or flying avatar between adjacent regions, preserving appearance, controls, velocity, camera, and session.
- [x] Carry the avatar's attachments with it across a crossing.
- [ ] Recover and reconcile teleports interrupted by a remote-host failure.
- [ ] Handle disconnects, destination failure, retries, stale transit records, and reconciliation after a restart.

### Objects, Sitting and Crossings

Vehicle crossings follow vehicles, in Phase 6.

- [x] Sit on a prim, at its sit target or a default seat, with unsit, camera placement, and seated animation.
- [x] Sit on the ground.
- [x] Define what happens to every kind of entity that moves off a region ([ADR 0037](adr/0037-object-border-crossings.md)).
- [x] Contain avatars and physical objects at a border with no neighbor.
- [x] Resolve border neighbors from grid records and current leases.
- [x] Cross a single object between regions with its identity, ownership, permissions, and contents.
- [ ] Cross complete linksets between regions.
- [ ] Carry a physical object's motion continuously across a border.
- [ ] Cross attachments as part of their avatar bundle.
- [ ] Cross an object and everyone sitting on it as one bundle.
- [ ] Prevent a crossing from duplicating or losing events and collisions.

### Seeing across a region border

- [x] Announce every online neighbor to a viewer, so it holds a standing child circuit to each.
- [x] Reduce a border crossing to promoting a circuit the viewer already holds.
- [x] Clear what a region showed a watching viewer when it restarts or its child expires.
- [x] Drop the regions a viewer has left behind after a far teleport.
- [ ] Serve objects to a viewer in a neighboring region, filtered by its camera and draw distance.
- [ ] Serve avatars to a viewer in a neighboring region the same way.
- [ ] Tell a neighboring viewer when something leaves its interest.
- [ ] Define what a viewer is shown when a neighbor is unreachable, restarting, or newly online.

### World navigation

- [x] Generate terrain-derived region tiles and world-map zoom levels for every region size.
- [x] Implement map-block and name-prefix region discovery.
- [x] Implement landmarks, home location, and teleport routing.
- [x] Serve parcel land data, so landmark creation and About Land work.
- [ ] Add region and parcel search.
- [ ] Show friends' presence and location without leaking restricted information.

### Inventory asset durability

- [x] Separate the blob, asset, and instance layers.
- [x] Implement the grid asset vault.
- [x] Commit an inventory item only once everything it references is durable.
- [x] Treat region copies of vault-held assets as an evictable cache.
- [x] Backfill existing inventory-referenced assets into the vault.
- [ ] Tier rarely used vault blobs onto slower S3-compatible storage.

## Phase 3: Interactive Physical World

### Production physics integration

- [x] Make Jolt the default production physics world behind an engine-independent boundary.
- [x] Create, update, sleep, wake, remove, and restore physical bodies from scene changes.
- [x] Synchronize physical transforms and velocities to viewers with interest-aware throttling.
- [x] Support phantom objects and temporary-on-rez objects.
- [ ] Complete collision filtering, material behavior, volume detection, and collision events.
- [x] Represent physical linksets as compound bodies.
- [ ] Complete Firestorm acceptance for compound collision, falling, rotation, editing, delinking, and restart persistence.
- [x] Keep physics behavior consistent across restarts and handoffs.

### Attachments

- [x] Wear inventory objects on named attachment points with permissions, ownership, and persistence.
- [ ] Wear an attachment at the offset it was last worn at.
- [x] Restore worn attachments on login and on arrival in any region.
- [ ] Define the order of attachment, seated-avatar, physics, viewer, and script events.

## Phase 4: Mesh and Creator Platform

### Mesh pipeline

glTF (GLB) is the stored format, and each client is served a derived rendition.

- [x] M1 static mesh.
- [x] M2 Firestorm mesh uploads.
- [x] M3 texture renditions.
- [x] M3 glTF materials on published faces, so two-sided faces render in viewers.
- [ ] M3 editing a region-assigned material in a viewer's build tool.
- [ ] Terrain surface for session clients.
- [x] Close the texture pipeline's asymmetry.
- [ ] V-HACD convex decomposition for mesh physics.
- [x] Regenerate stale renditions.
- [x] M4 rigged mesh mapped onto the Bento skeleton, for attachments and body wearables.
- [ ] M5 server-side import of source formats ([ADR 0035](adr/0035-server-side-source-format-import.md)).
  - [x] FBX import.
  - [ ] OBJ and DAE import.
  - [ ] Archive import for sources with external textures.
- [x] Retarget Character Creator rigs onto the Bento skeleton.
  - [x] Measure joint positions against the whole worn set.
  - [x] Carry two-sided faces, so hair arrives as authored.
  - [x] Choose alpha blending or alpha testing by measuring each mask.
  - [ ] Size the avatar the viewer builds to the mesh it draws.
  - [ ] Correct an A-pose bind during retargeting.

### Content creation and inventory breadth

- [ ] Complete viewer building workflows for linksets, materials, sculpt, animation, sound, gesture, notecard, landmark, and script content.
- [ ] Store mesh collision sources separately from visual LODs.
- [ ] Implement upload, validation, and inventory creation for every supported asset type.
- [x] Add wearable creation, editing, and named outfit saving.
- [ ] Provide bulk inventory, search, copy, transfer, export policy, recovery, and large-inventory performance.

## Phase 5: Social Communications

### Identity, profiles, and communication

- [ ] Implement names, profiles, interests, images, privacy, and account administration.
- [ ] Implement direct, offline, group, and conference messages, with mute and block.
- [ ] Provide voice via WebRTC.
- [ ] Implement friendship, calling cards, presence permissions, and offers.
- [ ] Add abuse reporting and the moderation evidence grid operators need.

### Groups, roles, and shared ownership

- [ ] Implement groups, roles, powers, membership, invitations, notices, and group chat.
- [ ] Support group-owned land and objects.
- [ ] Apply group powers to parcels, estates, object editing, inventory sharing, and moderation.
- [ ] Audit sensitive group and ownership changes.

## Phase 6: LSL Scripting

### Language and compiler

- [x] Build the Falcon lexer, parser, semantic analyzer, bytecode format, and compiler for an initial LSL subset.
- [x] Report compile results and errors to Firestorm.
- [ ] Inventory the full Second Life LSL surface plus Halcyon/InWorldz extensions.
- [ ] Compile that full language.
- [ ] Make `float`, `vector`, and `rotation` first-class types.
- [x] Store LSL source in personal and task inventory, editable from Firestorm.
- [ ] Cache compiled bytecode by source hash, compiler version, and runtime ABI.
- [ ] Build LSL compatibility tests.

### Cooperative runtime and resource control

- [x] Run the Falcon VM on the region thread with no native thread per script.
- [x] Slice script execution per tick, so an infinite loop yields instead of blocking the world.
- [ ] Schedule scripts fairly across scripts, objects, owners, and parcels.
- [ ] Enforce memory, stack, event-queue, and payload limits.
- [ ] Make slow host operations asynchronous.
- [ ] Add operator metrics, throttling, diagnostics, and controls for faulty scripts.

### LSL Events and Region Interaction

- [x] Rez a script into an object and run its `state_entry`.
- [x] Recompile scripts after edits, keep the running instance on a failed compile, and remove deleted ones.
- [x] Implement `llSay` and `llOwnerSay`.
- [x] Implement touch, starting with `touch_start`.
- [x] Implement `llSitTarget`, `llSetLinkSitTarget`, and the other sit-target functions.
- [x] Implement `changed` and `run_time_permissions`.
- [x] Grant animation permission to a seated avatar and play animations with `llStartAnimation` and `llStopAnimation`.
- [ ] Implement the remaining LSL events.
- [ ] Implement the LSL host functions.
- [ ] Preserve Second Life event ordering and delays where they are observable.
- [ ] Integrate script ownership and permissions with linksets, attachments, sitters, parcels, and estates.

### Script persistence and crossings

- [x] Snapshot a running script after any instruction and restore it in a fresh VM.
- [ ] Restore running scripts across region restarts.
- [ ] Serialize complete script state in a compact versioned format.
- [ ] Stop and restore live scripts at any instruction.
- [ ] Snapshot scripts together with their attachment, object, or vehicle.
- [ ] Cross heavily scripted attachments and vehicles.
- [ ] Version the runtime ABI with safe upgrade and rollback of stored script state.

### Vehicles and physical objects

- [x] Move, edit, take, and restore dynamic objects without losing physics state.
- [ ] Add the Second Life vehicle parameter model.
- [ ] Make `llSetVehicleType` produce each standard vehicle type.
- [ ] Synchronize driver controls, vehicle motion, cameras, and passengers.
- [ ] Keep a vehicle's object, contents, permissions, passengers, and physics together as one bundle.
- [ ] Add load, tunneling, stacking, recovery, and abusive-object safeguards.
- [ ] Cross a vehicle at a region border with its motion, script, and passengers intact.

## Phase 7: Reliable Operations and Distribution

### Known hazards on the current deployment

- [ ] Monitor free disk space on the shared host.
- [x] Treat presence as a lease, so a session that dies cannot stay present.

### Grid and region packages

- [x] Produce versioned grid-owner and region-owner packages with installation guides.
- [ ] Support clean, unattended, upgrade, downgrade, and uninstall installs that preserve configuration.
- [ ] Sign release artifacts and publish checksums, provenance, and a release manifest.
- [ ] Validate Windows and Linux installs without a source checkout.

### Backups, upgrades, and reconciliation

- [x] Restart the grid service without restarting regions or ending viewer sessions.
- [x] Report one version everywhere.
- [x] Refuse to start a grid service against a database older than it requires.
- [ ] Back up and restore grid, region, asset, terrain, and configuration state.
- [ ] Export and import OpenSim-compatible region (OAR) and inventory (IAR) archives.
- [ ] Write the latest OAR and IAR versions and read older ones where practical.
- [ ] Test full-grid, single-region, and single-user recovery.
- [ ] Support rolling grid and region upgrades within a documented compatibility window.
- [ ] Reconcile leases, presence, inventory, assets, and crossings after crashes or partial restores.

### Observability and administration

- [x] Publish grid statistics and an event log ([ADR 0039](adr/0039-grid-statistics-and-event-log.md)).
- [ ] Provide metrics, structured logs, traces, dashboards, and alerts.
- [ ] Add command-line and web administration for users, regions, estates, assets, inventory, scripts, and moderation.
- [ ] Make the economy an operator setting.
- [ ] Record tamper-evident audit events for privileged operations.
- [ ] Define capacity indicators and load shedding.

### Security and deployment hardening

- [ ] Add transport encryption, service identity, credential rotation, scoped authorization, and secure defaults.
- [ ] Validate every external input against resource exhaustion and malformed data.
- [ ] Add dependency, artifact, and configuration scanning and a vulnerability policy.
- [ ] Perform fault-injection, abuse, and recovery testing before a production release.

## Phase 8: Scale, Compatibility, and Ecosystem

### Performance and scale

- [ ] Establish repeatable concurrency, scene, physics, inventory, asset, crossing, script, and network benchmarks.
- [ ] Implement packet prioritization, backpressure, and adaptive update rates for crowded regions.
- [ ] Scale central services horizontally where measurements justify it.
- [ ] Publish tested capacity envelopes.

### Compatibility

- [ ] Maintain conformance tests against the supported Firestorm release.
- [ ] Add read-only legacy inventory access only if its benefit justifies the cost.
- [x] Support thin and headless clients such as LibreMetaverse.
- [ ] Validate Halcyon/InWorldz LSL extensions without admitting OpenSimulator ones.
- [ ] Document import and migration tools.

### Physics and service extensions

- [ ] Promote the PhysX 5 adapter to an optional supported physics plugin.
- [ ] Stabilize versioned plugin contracts where they have proven value.
- [ ] Define safe extension points for grid services.
- [ ] Keep transfer and persistence contracts identical across physics implementations.

### Release readiness

- [ ] Publish administrator, region-owner, creator, scripter, and contributor documentation.
- [ ] Run sustained multi-region worlds with real viewers, scripts, vehicles, failures, upgrades, and restores.
- [ ] Resolve all release-blocking findings.
- [ ] Define the supported platforms, compatibility guarantees, and maintenance policy for the first stable release.

## Phase 9: Modernized Communications Transport

The first-party client's wire surface, replacing LLUDP while legacy viewers keep it.

### Arrival and bootstrap

- [x] Serve the compatibility probe at `GET /v1/version`.
- [x] Open world entry for the Homeworldz client at `POST /v1/client/session`.
- [x] Enforce the grid-region protocol handshake at registration and renewal.

### The grid channel

- [x] Serve the grid WebSocket at `GET /v1/client/channel`.
- [x] Deliver server notifications to connected users.
- [x] Store and forward instant messages.
- [ ] Add the remaining store-and-forward kinds.

### The region session

- [x] Choose TLS + WebSocket as the session transport.
- [x] Serve the region session with ticket authentication.
- [x] Advertise each region's session endpoint to the client.
- [x] Carry avatars, objects, movement, and chat over the session.
- [x] Carry a session avatar across a region border.
- [x] Filter avatar traffic by interest for session clients.
- [ ] Filter avatar traffic by interest for viewers.
- [ ] Serve home-hosted regions through the call-home relay.
- [ ] Add WebTransport as a second transport when its RFC publishes.

## Phase 10: Modern Client Support

The server side of what the first-party client can do that a legacy viewer cannot.

- [ ] A canonical avatar body.
- [x] Dress a session avatar for viewers.
- [x] Publish the region's water to session clients.
- [x] Keep terrain sample alignment within each sample's quantization step.
- [x] Serve the browser client from the account site's own origin.
- [ ] Serve appearance to session clients.
- [ ] Store modern asset formats at rest.
- [ ] Mesh prims server-side.
- [ ] Extend the session's capability manifest as extensions ship.
- [ ] Add voice and modern presence for the new client.
- [ ] Build creator tooling on the modern pipeline.

## Phase 11: Economy and Marketplace

What a creator can sell and how value moves.

- [ ] Decide whether credits stay display-only or become a transferable balance.
- [ ] Implement auditable balances, transactions, sales, payments, gifts, and refunds.
- [ ] Treat external payments and marketplace integration as separate security projects.
- [ ] Record whether an asset may be sold, separately from whether it may be transferred.
