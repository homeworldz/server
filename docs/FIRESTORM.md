# Firestorm Compatibility Target

## Pinned Viewer

Homeworldz targets the OpenSim-enabled **Firestorm 7.2.4** viewer for the first
playable vertical slice.

- Official source branch: [`Firestorm_7.2.4`](https://github.com/FirestormViewer/phoenix-firestorm/tree/Firestorm_7.2.4)
- Pinned source commit: [`10bd3c9f930c76e1427ddd4ecece6cdf36b4406d`](https://github.com/FirestormViewer/phoenix-firestorm/commit/10bd3c9f930c76e1427ddd4ecece6cdf36b4406d)
- Firestorm version file: `7.2.4`
- Upstream viewer version file: `26.1.1`
- Build flavor: 64-bit OpenSim-enabled release build

The source commit, not a moving download URL, is the protocol reference. A
repeatable smoke-test record must additionally capture the installed viewer's
full About dialog version and installer checksum when the binary is introduced
into the manual test workflow.

Firestorm's official repository advises downstream users to use official
release branches rather than `master`, preview, or nightly builds. Homeworldz
therefore upgrades this pin deliberately and reruns the login smoke test instead
of silently following new viewer releases.

## Minimum Login Request

Firestorm adds Homeworldz using the base grid URI
`http://<grid-host>:42000/`. It discovers viewer metadata from
`GET /get_grid_info`, including the grid name, login URI, welcome page, and
helper URI. The default login endpoint is `http://<grid-host>:42000/login`.

The OpenSim-enabled viewer posts a `login_to_simulator` request to that URI. The
wire encoding is the viewer-compatible XML-RPC/LLSD login envelope; it is an
edge protocol and does not become an internal Homeworldz service protocol.

The request parameters relevant to the first slice are:

- credentials: `first`, `last`, and `passwd`;
- destination: `start`, using `home`, `last`, or
  `uri:<region>&<x>&<y>&<z>`;
- viewer identity: `version`, `channel`, `platform`, `platform_version`, and
  `address_size`;
- device/session metadata: `mac`, `id0`, `host_id`, `last_exec_event`,
  `last_exec_duration`, and `last_exec_session_id`;
- protocol controls: `agree_to_tos`, `read_critical`, `extended_errors`, and
  the requested `options` array.

Firestorm 7.2.4 requests inventory roots and skeletons, initial outfit,
gestures, buddy data, UI/login flags, global textures, map/voice settings, and
OpenSim extensions such as currency and maximum groups. Homeworldz may return
empty optional collections, but it must preserve their expected LLSD types.

## Minimum Successful Response

The login endpoint returns a successful viewer login envelope with at least:

- `login`: `true`;
- identity: `agent_id`, `session_id`, `secure_session_id`, `first_name`, and
  `last_name`;
- destination circuit: nonzero `circuit_code`, `sim_ip`, `sim_port`,
  `region_x`, `region_y`, and `seed_capability`;
- placement: `start_location`, `look_at`, `region_size_x`, and `region_size_y`;
- startup text/time: `message` and `seconds_since_epoch`;
- inventory shapes: one `inventory-root` entry, one `inventory-lib-root` entry,
  one `inventory-lib-owner` entry, plus array-valued `inventory-skeleton` and
  `inventory-skel-lib`;
- array-valued `login-flags`, `gestures`, and `buddy-list` (initially allowed to
  be empty where Firestorm tolerates it).

Failures return `login: false`, a stable `reason`, and a human-readable
`message`. Passwords, session IDs, and secure session IDs must not be logged.

## Startup Sequence After Login

The minimum observable flow is:

1. Firestorm posts `login_to_simulator` to the grid login URI.
2. The grid authenticates the development user, resolves `start`, creates the
   viewer session, and selects an online region lease.
3. The grid returns the session IDs, destination UDP circuit, and region-local
   seed-capability URL.
4. Firestorm creates the destination region and requests the seed capability;
   the region returns the capability map needed by the slice.
5. Firestorm sends reliable UDP `UseCircuitCode` with circuit code, session ID,
   and agent ID. The region acknowledges it.
6. Firestorm and the region exchange `RegionHandshake` and
   `RegionHandshakeReply`.
7. Firestorm sends `CompleteAgentMovement`; the region replies with
   `AgentMovementComplete` and begins required event/object delivery.
8. The smoke test passes when the viewer leaves the startup screen, renders the
   destination region, and can move the avatar and use nearby chat.

The region loads `assets/region/terrain/plateau-square.raw` as its initial
256-by-256 metre terrain and advertises the four matching Library terrain
textures in `RegionHandshake`. The `region.terrain_path` INI setting may select
a different raw heightmap; invalid or missing input falls back to flat terrain.

## System Library

Homeworldz presents a grid-owned, read-only inventory root named `Library`.
Its initial viewer-facing structure follows the familiar Second Life layout:
`Library / Clothing / Initial Outfits / Default Avatar`, with a standard
top-level `Body Parts` category. The default-avatar folder contains the same
shape, skin, hair, eyes, shirt, and pants used to clothe a newly created user.
`Library / Textures / Terrain` contains the licensed default sand/dirt, grass,
mountain, and rock textures used by the compatible terrain protocol.

Firestorm reads this shared catalog through the separate `LibraryAPIv3`
capability when supported. Firestorm 7.2.4 may instead request known Library
folder UUIDs through `InventoryAPIv3`; Homeworldz recognizes those UUIDs and
returns the same read-only catalog. Library data is not copied into personal
inventory and never changes an existing outfit merely because the viewer
reads it.

The stable `homeworldz.library` service identity owns this catalog and is
shown to viewers as `Homeworldz Library` when identity names are available.
Its UUID, `00000000-0000-0000-0000-000000000002`, is also recorded as creator
for bundled Library inventory items and as uploader for their region asset
mappings. The account is created locked; an administrator must explicitly set
a password before using it for interactive content preparation.

## Personal Inventory Mutation

`InventoryAPIv3` supports authenticated rename, description update, move, and
delete operations for personal inventory items. Item updates preserve the
original asset UUID and creator UUID, and moving an item updates both source
and destination folder versions. The shared Library remains read-only through
both its dedicated capability and the compatibility reads exposed through the
personal AIS endpoint. Folder creation and rename use the same AIS-first model;
legacy viewer mutations are not a second source of inventory truth. AIS batch
link creation is atomic and supports viewer-managed Current Outfit replacement
without having the grid reapply the default outfit to established avatars.

## Ordinary Asset Upload

The region advertises `NewFileAgentInventory` for the initial ordinary-upload
slice. Firestorm sends metadata as LLSD, receives a one-shot uploader URL,
converts the selected source, and posts the binary asset. Homeworldz accepts
the exact texture, snapshot, sound, and animation type pairs used by Firestorm;
it validates JPEG2000 image signatures, Ogg Vorbis sound framing, and the
version-one animation header. The active viewer session is authoritative for
the uploader and creator UUID. The Region stores and registers the asset, then
asks the Grid to create a correctly typed inventory item in the requested
folder owned by that user. The completion response contains distinct new asset
and inventory-item UUIDs. These uploads cost zero. The UDP economy response
advertises that zero price, while `SimulatorFeatures.OpenSimExtras.currency`
identifies the grid's viewer-facing currency as credits (`C$`). Texture,
snapshot, sound, and animation are the complete set of ordinary Firestorm
file-upload types: the viewer's Bulk upload iterates the selected files
through this same single-file capability, so it needs no separate server
path, and objects enter inventory through Take/DeRez rather than file upload.
Mesh (and with it the `NewFileAgentInventoryVariablePrice` capability) remains
outside this slice.

The Region also advertises Firestorm's agent-inventory update capabilities for
notecards, gestures, and LSL source. Each request is bound to the authenticated
session and an owned, modifiable item of the exact expected type. Its one-shot
uploader stores a fresh asset with the editor's creator UUID, registers that
origin with the Grid, and atomically relinks the personal inventory item.
Notecard and gesture completion returns the new asset UUID. LSL source is
preserved now but compilation is intentionally reported as unavailable until
the Phase 6 Homeworldz compiler and runtime are implemented.

Each region registers its viewer UDP port with the grid. Login advertises that
stored port, while `region.viewer_port` controls the matching region listener
(default `42002`). The first reliable `UseCircuitCode` datagram is
accepted only when its circuit code, session, agent, and destination region all
match the grid's live session record.

The region returns `EventQueueGet` from the authenticated seed capability. Its
first poll delivers `EstablishAgentCommunication`; later polls return an empty
event array with a monotonically increasing queue ID. UDP startup enforces
`UseCircuitCode`, `RegionHandshakeReply`, then `CompleteAgentMovement` before
returning `AgentMovementComplete`.

After movement completes, authenticated `AgentUpdate` packets drive the
provisional avatar controller. It retains viewer camera axes and draw distance,
resolves movement relative to body orientation, persists the resulting
position, and streams authoritative position, velocity, and rotation changes
to viewers at a bounded rate. Walking, turning, flying, falling, ascent, and
descent were accepted in Firestorm on 2026-07-15.

`AgentSetAppearance` supplies the viewer size and visual-parameter block. The
provisional controller mirrors the Halcyon/InWorldz avatar-height calculation
and treats the avatar root as the center of that height, so the support point is
`terrain height + avatar height / 2`. Terrain is resampled beneath the avatar as
it moves; following the loaded terrain was accepted on 2026-07-15. The
viewer-facing position separately applies Halcyon's appearance-derived hip
offset, while the authoritative physics center remains unchanged. Acceptance
of stopped and moving foot placement with that correction passed on
2026-07-15. On a grounded transition into flight, the provisional controller
also mirrors Halcyon's 2 m/s upward impulse with short damping, producing
approximately 0.5 metres of lift without teleporting the avatar. Viewer
acceptance of that launch and the production Jolt capsule remain pending.

Firestorm also sends `AgentAnimation` start/stop changes for viewer-driven
states such as appearance customization. The region maintains the active set,
starts each avatar in the standard `STAND` animation, and returns the complete
simulator-authoritative `AvatarAnimation` set. This prevents a local customize
pose from becoming the avatar's indefinite rendered state. Viewer acceptance
of appearance-mode exit remains pending.

Once movement completes, the region streams all 256 flat 16-by-16 terrain
patches in bounded reliable batches and sends a full static volume-primitive
update. Channel-zero viewer chat is rebroadcast reliably to avatars within the
viewer whisper, normal, or shout radius.

The seed also advertises a session-scoped `GetTexture` capability. Texture
UUIDs are validated, resolved through the region's SQLite mapping, read from
the immutable SHA-256 blob store with hash verification, and returned as
`image/x-j2c`; absent or corrupt mappings return `404`.

The grid supplies the system inventory skeleton, item metadata, four required
default body parts, a default shirt and pants, and Current Outfit Folder links.
The region imports the bundled Halcyon-compatible wearable and source-texture
assets and serves them through `ViewerAsset`. The complete default outfit is
seeded only when an account has no established initial outfit; later deployment
changes do not add clothing or alter the COF of existing avatars.

For an uncached outfit, the region answers `AgentCachedTexture` with explicit
misses. Firestorm locally composites the five legacy bakes and uploads them
through the authenticated `UploadBakedTexture` capability. The region stores
the JPEG2000 assets with the uploader's creator UUID and serves Firestorm's
host-bake refetches through reliable, throttled UDP `ImageData` and
`ImagePacket` transfers. The final `AgentSetAppearance` associates each
wearable hash and texture index with its baked asset in persistent SQLite.
Later logins return only exact hash/index matches; changed outfit inputs remain
misses and trigger a new bake.

The pinned viewer source constructs the request in
[`lllogininstance.cpp`](https://github.com/FirestormViewer/phoenix-firestorm/blob/10bd3c9f930c76e1427ddd4ecece6cdf36b4406d/indra/newview/lllogininstance.cpp#L155),
consumes the circuit response in
[`llstartup.cpp`](https://github.com/FirestormViewer/phoenix-firestorm/blob/10bd3c9f930c76e1427ddd4ecece6cdf36b4406d/indra/newview/llstartup.cpp#L4748),
and starts the UDP circuit with `UseCircuitCode` in the same
[`llstartup.cpp`](https://github.com/FirestormViewer/phoenix-firestorm/blob/10bd3c9f930c76e1427ddd4ecece6cdf36b4406d/indra/newview/llstartup.cpp#L2455).

## Windows Smoke Test

After bootstrapping PostgreSQL and building both services, run:

```bat
scripts\firestorm-smoke-test.cmd
```

The launcher reads ignored local configuration, starts the grid and region on
loopback, and verifies both readiness endpoints. Put reusable local credentials
in `config/smoke-test.ini`:

```ini
[user]
first_name = Jim
last_name = Tarber
password = your-local-test-password
```

When the file or password is absent, the launcher prompts for a password. It
creates the configured `jim.tarber` smoke-test account or
validates the supplied password when that account already exists, then launches
the installed OpenSim-enabled Firestorm with the Homeworldz login URI. Service
logs are written beneath ignored `var/smoke-test/`, and the services stop when
Firestorm exits or the launcher is interrupted.

Inside Firestorm, log in with the configured name and password. Verify initial
region entry, avatar movement,
nearby chat, terrain and the welcome primitive. Log out to prove circuit and
session disconnection, then log in again to prove reconnection. Restart the
launcher and repeat once to verify persisted scene restoration and grid region
re-registration. Record the About-dialog version and executable SHA-256 with
the smoke-test result before completing the milestone.

Use `-first` and `-last` to override the configured account, `-config` to select
a different credentials file, or `-validate-only` to check service startup
without prompting or launching the viewer.

## Milestone 3 Smoke-Test Result

Milestone 3 passed on 2026-07-13 using the Windows OpenSim release executable:

- About/log version: `Firestorm-Releasex64 7.2.4.80712 [10bd3c9f93]`;
- executable: `FirestormOS-Releasex64.exe` (54,033,256 bytes);
- executable SHA-256:
  `9c891171eecab1c24f2eec47aa77f26b5e7f559dd2da72b49dd0e3db6de45bb1`.

The viewer discovered and authenticated against Homeworldz, entered the region,
rendered the complete 256-by-256 terrain, water, environment lighting, shadows,
and the welcome primitive, and accepted avatar movement. The UDP circuit
remained connected for more than 30 minutes without a heartbeat timeout. A
normal logout received an immediate server reply, cleared grid presence,
revoked the viewer session, and stopped both development services. A subsequent
launcher run restored the scene and reconnected successfully; the avatar
returned to its persisted position of approximately `(140.144, 142.803, 25)`.
Repeated pre-fix avatar entities were consolidated to the one persisted entity.

The follow-on appearance acceptance passed on 2026-07-13. Firestorm fetched and
wore the four required default body parts, automatically created and uploaded
the five legacy baked textures without a manual rebake, refetched those bakes
over simulator UDP, and rendered the avatar. A subsequent clean launcher run
returned five server-side wearable-cache hits, zero misses, and made no baked
upload requests. Firestorm displayed the avatar immediately and logout again
cleared presence and revoked the viewer session normally.

A 2026-07-15 shape edit and wearable copy exposed a provenance conflict in
rebaking: baked uploads derived their viewer UUID from texture bytes, so the
same baked content uploaded by another avatar collided with the immutable
creator recorded by the Grid. Firestorm retried the resulting failed upload
and displayed `Error in upload request`; an uncaught upload-path exception
could also terminate the region. Baked uploads now receive fresh viewer asset
UUIDs while identical bytes remain deduplicated by the region's SHA-256 blob
store, and the capability converts storage or registration exceptions into a
bounded `500` response without stopping the region. On startup, legacy local
mappings are reconciled to the Grid's authoritative creator only when their
viewer UUID, SHA-256 digest, and byte length all match; a content mismatch
still fails closed. Viewer acceptance of the fix remains pending.

Editing a worn shape or clothing item produces a new wearable asset before the
viewer updates the inventory item's asset reference. Homeworldz accepts the
narrow legacy asset-transaction packets Firestorm still uses for this binary
upload, including bounded multi-packet `xfer`, but keeps AISv3 as the inventory
authority. Transfer UUIDs use the protocol-defined MD5 combination of the
transaction and private secure-session UUID; the secure value is exposed only
to authenticated region services and is never logged. The Grid relinks only
an owned, modifiable clothing or body-part
item, and only to an asset whose recorded creator is the editing avatar. The
inventory item's original creator is preserved while the new asset records its
actual uploader. Firestorm performs that relink through an AISv3 item `PATCH`:
it replaces `asset_id` with the upload transaction in `hash_id`, which the
Grid resolves with the same secure-session combination before accepting the
new asset. Viewer acceptance passed on 2026-07-15: changing the worn shape to
male survived leaving appearance editing, the uploaded body-part asset was
linked by a successful AIS patch, and all six full-resolution bakes completed.
Hair temporarily changed from brown to pale during editing and dark immediately
afterward, then settled back to brown after a later six-bake refresh. This was
observed as transient composite-bake rendering; the hair wearable itself was
not edited or replaced.

The read-only system Library acceptance passed on 2026-07-14. Firestorm showed
the shared `Library / Clothing / Initial Outfits / Default Avatar` hierarchy
and all six default wearable items. This Firestorm build requested the Library
folder UUIDs through `InventoryAPIv3`; Homeworldz returned the shared catalog
without copying it into Demo Avatar's personal inventory or changing the worn
outfit.

The terrain-texture extension passed in the same Firestorm version on
2026-07-14. The viewer showed all four entries beneath
`Library / Textures / Terrain` and opened the bundled JPEG2000 asset for visual
inspection without a missing-asset or decode error.

The first shaped-terrain implementation passed on 2026-07-14. Firestorm
reconstructed the user-supplied `terrain-island5.png` heightmap with the
intended island/channel geometry, correct water separation, and visible
blending among the four advertised terrain textures. No manual terrain or
texture refresh was required. It was subsequently replaced as the development
default because its vertical relief was too dramatic. Experimental terrain
authored from the rendered `Image-island3` preview established the correct
north/south orientation but was not accepted as a default because rendered
texture and lighting do not make a clean heightmap.

The final default-terrain acceptance passed on 2026-07-14. Firestorm rendered
the project-generated square plateau at the intended approximately
250-by-250-metre above-water footprint, with water visible on every side,
slightly softened corners, and a calm 22-metre surface above the standard
20-metre waterline. This large square terrain is the development default; the
separate bundled 200-metre round plateau remains an optional alternative.

The first ordinary texture upload passed on 2026-07-14 while logged in as
`Homeworldz Library`. Firestorm uploaded `terrain-island5.png` virtually
instantly and created `Terrain Island 5 Heightmap` in the account's personal
Textures folder. The region stored a 3,990-byte JPEG2000 asset, returned
distinct asset and inventory-item UUIDs, and recorded the stable Library UUID
`00000000-0000-0000-0000-000000000002` as both authenticated uploader and
creator provenance. The initial button showed `OS$-1` because the region had
not yet answered `EconomyDataRequest`; the subsequent zero-price economy
response was accepted and changed the button to `OS$0`. Firestorm obtains the
currency symbol separately from the region's `SimulatorFeatures` capability;
the subsequent capability response was accepted and the preview button showed
`Upload (C$0)`.

The first personal AIS item rename passed on 2026-07-14. While logged in as
`Homeworldz Library`, Firestorm renamed the uploaded `terrain-island5` texture
to `Terrain Island 5`; both viewer PATCH requests returned success and the
stored item immediately reported the new name. A subsequent clean viewer login
showed `Terrain Island 5`, and Firestorm opened the texture successfully. A
second rename from `terrain-paw` to `Terrain Paw` was also accepted immediately;
its persistence across the next clean login remains to be confirmed.

AIS Current Outfit link creation passed on 2026-07-14. An earlier incomplete
outfit replacement had left the `Homeworldz Library` account's Current Outfit
empty while its four body parts and two clothing items remained intact. After
adding atomic AIS link creation and compatibility handling for legacy null
creator metadata, Firestorm successfully wore all six personal items. Each
Current Outfit POST returned success, the cloud cleared, and the baked avatar
appeared without a manual viewer rebake. This recovery was explicitly initiated
by the avatar; the grid did not reapply defaults automatically to an established
account.

The packaged login-logo update passed on 2026-07-14. After rebuilding and
restarting the grid, Firestorm's login screen displayed the swapped-color
Homeworldz SVG. An automated test also requires the grid's embedded SVG to
remain byte-for-byte identical to the project-root logo.

The AIS v3 Library-outfit flow passed on 2026-07-14 with `Jim Tarber`.
Firestorm copied `Library/Clothing/Initial Outfits/Default Avatar` into a new
personal Clothing subfolder through `LibraryAPIv3` HTTP `COPY`, then replaced
the Current Outfit through `InventoryAPIv3` HTTP `PUT .../links`. All six
personal wearables immediately showed as worn, the shirt and pants rendered,
and Current Outfit showed six valid links. Copying the Library outfit again
intentionally creates another personal folder; duplicate folders observed
during development reflected multiple explicit test attempts.

The copied-outfit persistence and cleanup checks passed in the same acceptance
session. After a clean viewer restart, all six wearables remained linked in
Current Outfit and rendered correctly. Firestorm moves personal folders to
Trash with legacy UDP `MoveInventoryFolder` even when AIS v3 is available;
the region's compatibility adapter forwarded two inactive `Default Avatar`
folder moves to the authoritative grid inventory service. Both moves returned
success, and a subsequent clean login showed only the active copy in Clothing
and both inactive copies in Trash without disturbing the worn outfit.

AIS v3 Trash purge also passed on 2026-07-14. Firestorm sent HTTP `DELETE` to
`InventoryAPIv3/category/{trash-id}/children`; the grid recursively removed the
two inactive outfit folders and their items while retaining the protected
Trash folder. The request returned success, Trash remained empty after a clean
login, and the active Clothing folder, six Current Outfit links, worn state,
and rendered avatar were unchanged.

Personal item moves to Trash passed on 2026-07-14. Firestorm moved an uploaded
texture with legacy UDP `MoveInventoryItem`, encoding its optional `NewName`
field with zero bytes. The region compatibility adapter accepted that encoding
and forwarded the move to the authoritative grid inventory service, which
returned success. On a clean login the texture remained in Trash and was absent
from Textures; Jim Tarber's clothing, Current Outfit links, and rendered avatar
were unchanged.

The complete personal-item Trash lifecycle passed in the same session.
Firestorm restored the texture from Trash to Textures through another
authoritative `MoveInventoryItem`, moved it back to Trash, and then emptied
Trash through AIS v3. The purge returned success and a direct AIS lookup of the
item returned `404`. After a clean login both Trash and Textures remained free
of the purged item, while clothing, Current Outfit, and avatar rendering stayed
correct.

The packaged-region configuration regression passed on 2026-07-14. After the
region gained direct `config/region.ini` support, the extracted Release package
started from its packaged example and app-local runtime files on isolated
ports. The relinked Debug service then completed a normal Jim Tarber login:
terrain and Library loaded, the clothed avatar and six Current Outfit links
were correct, Trash and Textures remained empty, and the purged test texture
did not return.

The first user-created primitive acceptance passed on 2026-07-14. Jim Tarber
created two standard boxes with Firestorm's Build tool; the region assigned
distinct object UUIDs and local IDs, recorded Jim's owner UUID, persisted their
0.5-metre scales and wood material, and restored both after a complete service
and viewer restart. A follow-up box used authoritative terrain height for a
land click: its stored centre was 22.25 metres, placing its lower face exactly
on the plateau's 22-metre surface rather than at the viewer ray endpoint.

Object-surface placement passed on 2026-07-14. Firestorm created a fourth
standard box on the upper face of the terrain-aligned box, preserving the
clicked horizontal offset. The stored centres were 22.25 and 22.75 metres
respectively for two 0.5-metre boxes, so their adjoining faces met exactly at
22.5 metres without overlap or a gap.

Initial object ownership, permissions, and delete-to-Trash acceptance passed
on 2026-07-14. Scene snapshots retained Jim Tarber's UUID independently as
both the original creator and current owner of his new boxes. Recipient-aware
object update flags and standard object-property replies made Firestorm show
the expected edit rights without its earlier non-owner confirmation warning.
The grid-backed UUID-name reply resolved creator and owner UUID
`169221cb-f2c9-48b3-a79d-c1d3e01d3723` to `Jim Tarber` in object details and
the avatar profile. Deleting the upper stacked box removed it from the
authoritative scene snapshot and created a `Primitive` object item in Jim's
Trash. A clean restart subsequently passed: the deleted upper box remained
absent, the `Primitive` item remained in Trash, and Firestorm again resolved
both Creator and Owner as `Jim Tarber`.

Viewer primitive transform and name editing passed its live-session acceptance
on 2026-07-14. Firestorm moved a box to `202, 144, 23`, resized it to
`1 x 2 x 3` metres, and renamed it from `Primitive` to `Prim1`. The region
decoded the standard `MultipleObjectUpdate` and `ObjectName` messages, checked
Jim Tarber's ownership and modify mask, persisted all three values, and sent
authoritative state back to the viewer. Closing and reopening Firestorm's Edit
panel retained `Prim1`. Jim then renamed the other two boxes `Prim2` and
`Prim3`; all three names reached the authoritative snapshot. After a complete
service and viewer restart, all three names persisted and Prim1 remained at
`202, 144, 23` with size `1 x 2 x 3` metres.

Viewer primitive rotation passed its live-session acceptance on 2026-07-14.
Firestorm rotated Prim1 to `0, 0, 45` degrees; the region validated and stored
the packed quaternion vector `[0, 0, 0.382683]`, returned it in the
authoritative object update, and Firestorm retained the visible rotation and
numeric fields after closing and reopening Edit. After a complete service and
viewer restart, the object remained named `Prim1`, visibly rotated, and the
Edit fields again showed `0, 0, 45` degrees.

Viewer primitive description editing passed its live-session acceptance on
2026-07-14. Firestorm set Prim1's description to `Tall rotated box`; the region
checked ownership and modify permission, persisted the value, and returned it
through `ObjectProperties`. Closing and reopening Edit retained the canonical
description. A complete service and viewer restart retained `Tall rotated box`
in Prim1's Edit panel.

Viewer primitive permission editing passed its live-session and persistence
acceptance on 2026-07-14. Firestorm enabled Anyone Move on Prim1 and removed
Copy from its next-owner permissions. The region accepted the standard
`ObjectPermissions` changes only for Jim Tarber as owner, stored the masks as
hexadecimal semantics (`Everyone = 0x00080000`, `Next owner = 0x00086000`),
and returned authoritative object properties. Firestorm consequently kept
Transfer enabled but unavailable for editing: a no-copy object must remain
transferable under the fair-use rule. After a complete service and viewer
restart, Anyone Move remained enabled, next-owner Copy remained disabled, and
next-owner Transfer remained enabled and greyed out.

Viewer primitive duplication passed its live-session acceptance on 2026-07-14.
With Prim2 selected, Firestorm's `Ctrl+D` sent standard `ObjectDuplicate` and
immediately displayed and selected one same-sized copy. The region assigned a
new object UUID and local ID, placed the copy exactly `0.5` metres higher in X
and Y, retained the source name and shape, retained Jim Tarber independently as
creator and owner, inherited the source permission masks, and saved the copy in
the authoritative scene snapshot. After a complete service and viewer restart,
Firestorm showed both objects named `Prim2`, with the copy still offset exactly
`0.5` metres diagonally from its source.

Viewer primitive physics-material editing passed its live-session acceptance
on 2026-07-14. Firestorm changed Prim3 from Wood to Metal in the Object tab;
the region decoded standard `ObjectMaterial`, required Jim Tarber's ownership
and modify permission, stored the canonical material code `0x01`, and returned
an authoritative object update. Closing and reopening Edit retained Metal.
After a complete service and viewer restart, Prim3's Material field still
reported Metal.

Viewer primitive Take passed its live-session acceptance on 2026-07-14.
Firestorm took the diagonally offset Prim2 copy with a one-packet, zero-based
`DeRezObject` batch (`PacketNumber = 0`, `PacketCount = 1`). The region resolved
Firestorm's null destination UUID to Jim Tarber's standard Objects folder,
stored an object asset and inventory item with the scene object's creator,
owner, permissions, name, and serialized state, removed exactly one object
from the authoritative scene snapshot, and immediately showed `Prim2` in
Firestorm's Objects folder.
After a complete service and viewer restart, only the original Prim2 remained
in the scene and the taken Prim2 remained in Jim Tarber's Objects folder.

Viewer inventory-object rez passed its live-session acceptance on 2026-07-14.
Jim dragged the taken Prim2 from his Objects folder onto terrain. The region
loaded the authoritative grid item and its `homeworldz-object-v1` asset,
created a new scene object UUID and local ID at the requested surface, retained
the `0.5 x 0.5 x 0.5` scale and Wood material (`0x03`), retained Jim Tarber as
both creator and owner, applied the inventory permission masks, and left the
copyable source item in Objects. Jim renamed the rezzed clone to `Prim2b`, and
the new name reached the authoritative scene snapshot.
After a complete service and viewer restart, both Prim2 and Prim2b remained in
the scene; Prim2b retained its name, size, Wood material, creator, and owner;
the source Prim2 item remained in Objects. Prim3 independently remained the
only test prim whose material was Metal.

The modified-permission Take/rez round-trip passed its live-session acceptance
on 2026-07-14. Taking Prim1 created an Objects item with current-owner
permissions `0x0009e000`, Anyone Move `0x00080000`, and next-owner permissions
`0x00086000` (Copy disabled, Transfer enabled). Rezzing that item retained the
copyable source item and restored Prim1's `1 x 2 x 3` size, 45-degree rotation,
Wood material, creator, owner, and permission masks. This test exposed two
edge cases that were corrected: Firestorm numbers Take batches as `0 of 1` but
Delete batches as `1 of 1`, and the inventory round-trip initially discarded
the scene description. After the fixes, deleting the intermediate Prim1 moved
it to Trash and rezzing the original Objects item restored `Tall rotated box`.
After a complete service and viewer restart, the rezzed Prim1 retained its
description, size, rotation, Anyone Move and next-owner masks; its source item
remained in Objects and the deleted intermediate item remained in Trash.

Firestorm continues to send the legacy UDP `CopyInventoryItem` message for
personal item copy/paste even when AISv3 capabilities are advertised. The
region therefore provides a narrow compatibility adapter for that message:
it authenticates the agent and source owner, asks the grid inventory service
to create the copy, enforces the source's Copy bit (`0x00008000`), and returns
`UpdateCreateInventoryItem`. AIS-backed grid storage remains authoritative;
the region does not maintain a parallel inventory. Firestorm encodes the
usual unchanged copy name as a zero-length `NewName`; this is valid for the
protocol's variable field and means the source name must be retained.
Personal Objects-item copy/paste passed live Firestorm acceptance on
2026-07-15: copying Prim1 created a distinct inventory item through the Grid,
and the region returned it through the viewer's registered callback.

The same persistence restart confirmed the copied Prim1, male shape, and brown
hair, but exposed a pre-crossing boundary error: Jim Tarber returned at local
`257, 251`, outside a 256 m region. Until region crossings are implemented,
the movement controller confines the full horizontal avatar capsule to the
configured region dimensions and clears outward velocity at an edge. Region
dimensions are controller inputs so future 2x2 and 4x4 regions are not tied to
a hard-coded 256 m boundary. Live Firestorm acceptance passed on 2026-07-15:
eastward and northward movement stopped at the hard viewer-visible coordinate
of `256` (the viewer rounds the internal `255.7` m capsule center), with no
continued motion to an invalid `257+` local coordinate.

Authoritative locomotion animation selection passed live Firestorm acceptance
on 2026-07-15. The region selects the canonical stand, walk, run, jump, fall,
fly, hover, hover-up, hover-down, and land animations from authoritative
controller state and broadcasts transitions with the avatar as their source.
Jim Tarber's standing, movement, jumping, flight, and landing animations were
visibly smooth and correct. That session also exposed one initially retained
forward input. `AgentUpdate` is an unreliable UDP snapshot, so the region now
rejects older per-avatar packet sequences and expires transient directional
controls after one second without a fresh update, while retaining persistent
flight mode. This prevents a lost key-release snapshot from carrying an avatar
indefinitely. A first repeat session passed, but a second fresh start exposed
alternating stop/start prediction until the region boundary. The region had
been consuming only one viewer datagram per simulation tick and only sending
avatar transforms when position changed. It now drains a bounded shared batch
of up to 256 immediately available viewer packets per tick and also transmits
velocity and rotation transitions, including the final zero-velocity update
Firestorm needs to stop extrapolating. These reconciliation changes passed a
fresh-region repeat on 2026-07-15: immediate
forward/release, repeated direction changes, and movement down toward the lower
shoreline all behaved correctly, with no continued, alternating, or visibly
mis-predicted motion.

The initial production Jolt avatar-capsule mirror passed live Firestorm acceptance on
2026-07-15. The region initialized Jolt through the engine-independent
`physics::World` boundary, created a shape-height-derived capsule for Jim
Tarber, synchronized it on the bounded fixed-step clock, and retained stable
login, terrain following, walking, stopping, jumping, and flight behavior.
No viewer-visible regression was detected. Authoritative scene identity and
persistence remain owned by Homeworldz rather than by Jolt.

Viewer terrain editing passed live Firestorm acceptance on 2026-07-15.
Firestorm's standard `ModifyLand` Raise tool changed only the affected terrain
patches, the region broadcast the result immediately, and avatar grounding
followed both a subtle default-strength edit and a stronger near-spike. The
region persisted sub-metre heights in its region-local `terrain.f32` state
rather than overwriting the packaged RAW default. After a complete viewer,
grid, and region restart, the region reported `region-state` as its terrain
source, the spike remained visible, and walking over it produced the expected
avatar-height changes.

Jolt terrain grounding passed its initial live Firestorm acceptance on 2026-07-15. The
region mirrors all 65,536 authoritative samples into a correctly oriented Jolt
heightfield at startup, replaces that collision body after viewer terrain edits,
and initially obtained the controller's support height through a targeted physics ray cast.
Jim Tarber walked over and stopped on the persisted spike, then flew and landed
nearby without a visible regression. The Homeworldz controller still owns
movement policy while Jolt supplies the collision geometry.

The production `CharacterVirtual` cutover passed live Firestorm acceptance on
2026-07-15. Persisted and newly edited static prims are mirrored into Jolt with
their scale and quaternion rotation. A vertical, appearance-height-derived,
bottom-origin capsule follows Homeworldz velocity policy while Jolt resolves its
position and grounded state. Firestorm stepped smoothly onto the 0.25 m
`Physics1`, remained stable after walking or falling onto it, landed stably on
terrain, and was blocked by the live-resized 1.5 m `Physics2`. Foot alignment on
top of `Physics1` was described as more precise than prior Second Life,
Halcyon, or OpenSimulator experience. This contact alignment is therefore a
regression constraint, not merely a visual detail. The acceptance
also covered the required fixes for Z-up capsule orientation, Jolt's
radius-based supporting plane, and clearing requested downward velocity from
the viewer-visible state while supported; no easing through platforms or
camera-wide bounce jitter remained.

Avatar motion restart persistence passed live Firestorm acceptance on
2026-07-15. The region snapshot retained Jim Tarber's local X/Y position,
airborne Z position, body rotation, velocity, and flight state. During login,
the grid obtained the saved look vector from the destination region rather than
advertising an east-facing default, and the region protected restored flight
through Firestorm's initialization packets. A controller initialization bug
that treated every restored spawn as grounded and snapped it to terrain was
also corrected. After a complete viewer, grid, and region restart, Jim returned
at viewer Z approximately `28`, still flying and facing the previous direction
above the persisted `Platform`; its edited `2 x 2 x 0.5` scale also remained.
Grounded contact is deliberately recomputed from Jolt rather than persisted as
potentially stale state.

Initial dynamic prim physics passed live Firestorm acceptance on 2026-07-15.
The viewer's `ObjectFlagUpdate` Physical and Phantom state is authenticated,
persisted in the authoritative scene, and mirrored as dynamic or excluded Jolt
collision bodies. `Dynamic1`, a wide flat box placed above two owned static
boxes (`Collision1` and `Collision2`) and the edited terrain spike, fell under
gravity, visibly deflected and rotated through the static contacts, reacted to
the terrain shape, and settled with zero velocity. The region streamed its
position and quaternion rotation to Firestorm at 10 Hz. A complete viewer,
grid, and region restart retained the settled transform and enabled Physical
state while both collision boxes remained fixed. The reusable pre-test scene
is retained locally as `var/regression-states/dynamic-prim-collision-start.json`.

Avatar contact with physical prims passed initial live Firestorm acceptance on
2026-07-15. After the character-controller maximum horizontal contact force was
derived from avatar mass and a configured maximum push acceleration, `Dynamic3`,
a 0.5 m physical cube, slid about one metre under a sustained avatar push
without being launched. `Dynamic4`, a 1 x 1 x 1 m physical cube, stayed
effectively stationary; the avatar bounced off it or walked around it at
angles. Low static objects still behaved as steps, while dynamic objects used
ordinary Jolt contact resolution.

This test also identified and removed the historical synthetic welcome prim.
That object had been injected directly into the login packet stream with a
placeholder UUID, no valid owner, no authoritative scene record, and no
physics body; it was therefore non-editable and non-colliding. Future contact
tests use only persisted scene entities. Edit suspension was subsequently made
consistent for both transition directions: on 2026-07-16, enabling Physical on
a selected non-Physical prim kept it suspended until the edit selection closed,
then activated dynamics immediately on deselection.

Primitive texture-entry persistence passed live Firestorm acceptance on
2026-07-15. All four dynamic test prims retained their distinct color tints
after a complete region and viewer restart. The region stores and restores the
opaque viewer texture entry rather than reconstructing only its default face,
which preserves both per-face texture UUIDs and tint data.

The standard Blank (`5748decc-f629-461c-9a36-a35a221fe21f`), Plywood
(`89556747-24cb-43ed-920b-47caed15465f`), Transparent
(`8dcd4a48-2d37-4909-9f78-f7a9eb4ef903`), and Media
(`8b5fec65-8d8d-9dc5-cda8-8fdf2716e361`) assets passed live Library acceptance
on 2026-07-15. After incrementing the Textures catalog version to invalidate
Firestorm's cached folder, all four appeared directly under Library → Textures.
Their region-backed asset mappings retain `Homeworldz Library` as their
importing creator provenance.

New-prim texture initialization also passed live acceptance. Because the
viewer `ObjectAdd` message does not contain a texture entry, Homeworldz assigns
the canonical Plywood texture with white tint, unit repeats, and no per-face
overrides. Two newly created boxes visibly used Plywood, and their complete
63-byte default texture entries were present in the authoritative snapshot.
Editing an existing prim's texture or tint remains independent and must not
change the default used by later prim creation.

Firestorm Build-preference application passed live acceptance on 2026-07-15.
Homeworldz honors the `ObjectAdd` material, scale, rotation, Physical, and
Create Selected fields. The last is returned transiently only to the creating
avatar, causing Firestorm to select the new object and apply its configured
follow-up object updates. A new box received the viewer's configured texture,
tint, dimensions, and Physical setting immediately and correctly. Library
textures can be selected directly for this purpose; copying them into personal
Inventory is not required.

Standard viewer protocol assets passed live acceptance on 2026-07-15. The
region served 22 fixed-UUID textures and four UI sounds from the bundled asset
set; Firestorm fetched the standard textures successfully and played its edit
interaction sounds. A subsequent legacy-scene repair replaced empty, null, and
viewer-local `d2114404-dd59-4a4d-8e6c-49359e91bbf0` default faces with Plywood
while retaining their tint and other face parameters. Nineteen historical
objects were repaired, and the resulting scene had no invalid default-face
texture UUIDs. Firestorm rendered the repaired prims correctly.

Canonical sphere creation passed live acceptance on 2026-07-15. Firestorm
rezzed a sphere with the configured Build defaults and Physical flag; Jolt used
a native sphere body, and its dynamic collision behavior was correct. The
object was named `Physical Ball` and taken into Inventory. After object assets
were extended to retain path/profile geometry and Physical/Phantom flags, a
second Take and re-rez preserved both the round shape and immediately active
physical state.

A complete region and viewer restart then retained `Physical Ball` as a
physical, non-phantom sphere with uniform `1.46018 m` dimensions. This
separately confirms scene-snapshot persistence after the inventory-asset
round-trip.

Canonical cylinder creation passed its initial live acceptance on 2026-07-15.
Firestorm rezzed the requested cylinder and the native Jolt cylinder produced
convincing physical behavior. The object was named `Physical Cylinder`, copied
in Inventory as `Physical Cylinder 2`, and re-rezzed; the copy retained its
cylinder geometry and Physical state. A complete viewer and region restart
then retained the in-world copy as a Physical cylinder.

Canonical Prism creation passed live rendering and physics acceptance on
2026-07-16. Firestorm encodes this tool preset as a square/line prim with raw
path scale `[200,100]` and shear `[0xce,0]`, rather than as an equilateral
triangle profile. Homeworldz now round-trips the complete classic prim shape
parameter block and uses a matching convex wedge; all sides, including the
sloped face, produced the expected collision. Taking `Prism1` into Inventory
and re-rezzing it preserved both the wedge shape and Physical state. A complete
viewer and region restart then retained the rezzed Prism with both properties.

Canonical Pyramid creation passed initial shape and collision acceptance on
2026-07-16: `Pyramid1` rendered with the expected apex and base, and its
five-point Jolt convex hull contacted naturally on all four sloped faces.
Mouse-drag acceptance remains pending after the initial test exposed excessive
impulse and wrong-direction movement caused by box-mass scaling and an ignored
object-local grab offset.
The corrected controller passed repeat live acceptance on 2026-07-16 with
`Pyramid1` both unrotated and noticeably rotated: it followed mouse drags in
the requested direction with controlled momentum and no launch behavior.
The edit-reactivation safety check also passed live acceptance after the same
Physical pyramid was placed roughly 80–90% below terrain: deselection raised
it smoothly above the surface without a solver launch. A repeat from well
underground resolved equally cleanly.

All seven basic build-tool shapes are now accepted on rez, and the revolved
shapes — Torus, Tube, and Ring — collide as solid cylinders over their prim
extents instead of falling through to a box; their mass model matches that
cylinder approximation. The central hole, like cut, hollow, and twist on every
shape, remains visual-only. The `ObjectShape` packet is also decoded, so
reshaping an existing prim in the build tool (including switching its basic
shape or editing cut, hollow, twist, taper, shear, or revolutions) is
authorized against owner and modify permission, applied to the authoritative
entity, persisted in the scene snapshot, echoed to all viewers, and re-mirrored
into Jolt. Previously the shape block was write-once at `ObjectAdd` and
build-tool reshapes were silently ignored. Live Firestorm acceptance for
torus/tube/ring collision and in-tool reshape remains pending.

## Cloud deployment acceptance

The first public cloud-grid login passed live Firestorm acceptance on
2026-07-16. Firestorm discovered and logged into `grid.homeworldz.com` over
HTTPS without a port suffix, the Grid authenticated Jim Tarber and selected
the registered Welcome region at `(1000, 1000)`, and the viewer completed the
direct region HTTP, UDP circuit, capability, event-queue, inventory, presence,
terrain, and appearance flows. The avatar remained a cloud for approximately
five seconds while its initial appearance resolved, then rendered normally.
Avatar movement against the cloud-hosted Jolt region was responsive and
showed no detectable control or transport lag.

This acceptance used the native Linux region build with Jolt on Ubuntu and a
separate registered Sandbox region at `(1001, 1000)`. Both region processes
continued renewing their Grid leases throughout the login.

Follow-up cloud acceptance on the same day covered interrupted first-login
recovery. Jim's six personal default wearables remained intact after an
earlier viewer failure had removed every Current Outfit link. On the next
login, the Grid detected the otherwise-impossible empty COF, verified that all
six unchanged default source items still existed, and restored only their six
links. Firestorm displayed the complete COF and rendered the clothed avatar
immediately. This recovery is deliberately disabled whenever COF contains any
entry or a default source is absent, so it cannot replace an established
avatar's chosen outfit.

Firestorm's standard private inventory setup also passed. AIS category
creation now accepts the viewer's empty `<uuid/>` server-allocation sentinel,
which created `#Firestorm` and its nested `#AO`, `#LSL Bridge`, and
`#Wearable Favorites` folders. Homeworldz provisions the viewer-required
`Calling Cards → Friends → All` hierarchy, with all three folders using type
2 as Firestorm expects. The final login produced no AIS HTTP 400 responses.
Initial wearable-cache and texture probes can still return 404 when a cache
key has not been baked; those misses are part of the rebake path and did not
delay the final rendered appearance.

Cloud world-map discovery passed live acceptance on 2026-07-16. From
Firestorm, both `Welcome` at `(1000, 1000)` and `Sandbox` at `(1001, 1000)`
were found by their complete names and by the case-insensitive prefixes `welc`
and `sand`. Homeworldz now advertises a stable grid-specific map image UUID and
the Grid serves Firestorm's levelled HTTP map-tile convention. A clean viewer
session displayed only the two registered regions rather than unrelated cached
tiles from other grids. Multi-resolution requests also returned a composite
tile containing both adjacent regions. Live visual acceptance confirmed that
the main map remained correct at every Firestorm zoom level.

Repeated-login UDP circuit replacement passed live acceptance on 2026-07-16.
After one successful cloud login, Firestorm was forcibly closed and relaunched
without restarting the Welcome Region. The Region authenticated the new login,
removed the earlier circuit and its endpoint-scoped runtime state, sent a new
RegionHandshake to the viewer's new UDP endpoint, and completed login normally.
This prevents an unacknowledged or interrupted logout from leaving the avatar
blocked indefinitely at `Waiting for region handshake`.

EventQueue long polling passed live cloud acceptance on 2026-07-16. The first
event still completed login immediately, while subsequent empty requests were
held for 20 seconds without blocking the Region's UDP loop. Idle EventQueue
responses fell from 776 per minute to 2 in a measured 45-second interval (about
a 99.7% reduction). Walking, turning, jumping, flying, and landing all remained
normal while a long poll was pending, and Region CPU remained below 1%.

Live terrain-derived world maps passed Firestorm acceptance on 2026-07-16.
Welcome and Sandbox initially displayed their matching rounded plateau terrain
at every map zoom level. To prove the tiles were not packaged placeholders, a
large X was carved into Welcome with Firestorm's terrain editor. The changed
heightfield appeared immediately on the world map while Sandbox retained the
original plateau. This verifies the complete Region terrain snapshot, Grid
shading/cache/composition, and viewer map-refresh path.

The initial task-inventory protocol passed live acceptance on 2026-07-17.
Opening the Contents tab of the newly created owned prim `Contents1` displayed
`Contents (No Elements)` immediately, without a spinner, delay, or viewer
error. The Welcome Region recorded the matching authenticated
`RequestTaskInventory` and sent the empty `ReplyTaskInventory` for the prim's
persistent object UUID.

Task-inventory copy, listing, and persistence passed live acceptance on
2026-07-17. A newly uploaded copyable texture was dragged from Jim Tarber's
personal Inventory into `Contents1`; Firestorm displayed the copied task item
immediately after the Region's serial refresh and Xfer response. Closing and
reopening Edit retained the listing. After a complete Welcome Region restart,
the item remained in Contents and its texture asset opened successfully in
Firestorm, confirming the task item UUID, asset reference, creator and owner
metadata, permission masks, and serial survived the scene-snapshot round trip.

Task-inventory multi-item transfer and removal passed live acceptance on
2026-07-17. Three copies of a personal texture were added to `Contents1`; the
viewer displayed all three after the inventory file streamed through Xfer
packets 0, 1, and 2. Removing an item from the middle and then the start of the
list updated Firestorm within milliseconds and retained the remaining items.
The first last-item removal exposed a several-second viewer delay because the
Region reset the serial to zero and omitted the inventory file. Matching
Halcyon's distinction between a never-populated inventory and a mutated empty
inventory—incrementing the serial and transferring an actual empty file—made
the final-row removal display immediately. Task-item deletion remained durable
across a Region restart and did not remove the copyable personal source item.

Task inventory also passed the object-asset round trip on 2026-07-17. A texture
was added to `Contents1`, the prim was taken into Jim Tarber's Objects folder,
and the inventory object was rezzed again. The rezzed prim retained the
texture in Contents and Firestorm could open its asset. Each rez assigns fresh
task-item UUIDs while preserving the content asset, provenance, permissions,
and per-prim inventory serial, so copyable object instances do not share task
inventory identities.

Task inventory remained independent across in-world object duplication on
2026-07-18. Shift-copying `Contents1` produced `Contents2` with the same
texture asset in its Contents. Deleting that task item from `Contents2` did
not affect the item in `Contents1`, where it remained present and openable.
This verifies that object duplication preserves content while assigning new
task-item UUIDs to every duplicated prim.

Task-item metadata mutation passed live acceptance on 2026-07-18. Renaming the
remaining texture in `Contents1` to `Contents Texture Renamed` appeared almost
immediately, followed by Firestorm's authoritative inventory refresh about one
second later. The new name survived closing and reopening Edit and a complete
Welcome Region restart. The Region received two idempotent viewer updates,
persisted each serial revision, and retained immutable asset and creator
identity fields.

Task-item Next Owner permission mutation passed live acceptance on 2026-07-18.
The uploaded source texture initially carried Firestorm's configured
`0x00082000` move-plus-transfer default, so Modify and Copy were clear rather
than being replaced by a Homeworldz default. Enabling Copy persisted after
closing and reopening the item Properties, closing and reopening object Edit,
and restarting the Welcome Region. Creator, owner, base, and asset identity
remained server-authoritative while mutable masks stayed bounded by base and
current permissions.

Copyable task-to-personal inventory movement passed live acceptance on
2026-07-18. Dragging `Contents Texture Renamed` from `Contents1` into Jim
Tarber's personal Textures folder created exactly one new personal item,
opened its texture asset, and retained the original task item. PostgreSQL
stored matching texture types, provenance, `0x0009e000` base/current masks,
zero everyone permissions, and the edited `0x0008a000` copyable Next Owner
mask. Both copies remained after a fresh Firestorm login.

No-copy personal-to-task inventory movement passed live acceptance on
2026-07-18. Dragging `No Copy Transfer Test` from Jim Tarber's Textures folder
into `Contents1` removed the personal item and created a task item with a new
UUID. The Grid transaction durably reserved the complete item before the
Region mutated its scene snapshot, and the Region finalized that reservation
only after the task item was stored. PostgreSQL retained the finalized custody
record, the source personal UUID no longer existed, and no transfer warning or
error was logged. Prepared transfers survive either service stopping between
those steps and are reconciled from the Grid when the destination Region
starts, preventing a no-copy item from being lost between inventory stores.

No-copy task-to-personal extraction passed live acceptance on 2026-07-18.
Dragging `No Copy Transfer Test` from `Contents1` back into Jim Tarber's
Textures folder removed it from the prim immediately and created exactly one
personal item. The Grid retained a finalized extraction record while
PostgreSQL stored the new personal UUID, original asset UUID, creator and owner,
`0x0009e000` base mask, `0x00096000` no-copy current mask, and literal
`0x0008a000` Next Owner mask. The Region sent both the personal-inventory
update and refreshed task-inventory file without warnings. Prepared
extractions are reconciled at Region startup: the Region snapshots removal
before the Grid atomically creates the personal item, so stopping either
service at the boundary cannot duplicate or lose the no-copy asset.

Recursive effective permissions and the no-copy object rez round trip passed
live acceptance on 2026-07-18. With `No Copy Transfer Test` inside `Contents1`,
Firestorm disabled Take Copy and rejected Shift-drag duplication. Ordinary Take
created one `Contents1 (no copy)` inventory object whose folded current mask was
`0x00086000` and folded Next Owner mask was `0x00082000`. Rezzing it removed the
no-copy inventory object and created one in-world object which retained
`No Copy Transfer Test` in Contents, still disabled Take Copy, and still
rejected Shift-drag duplication. PostgreSQL recorded the object-rez transaction
as finalized, removed the source inventory UUID exactly once, and the Welcome
Region logged exactly one completed rez with the transaction's object UUID.
Prepared object rezzes are durable: Region startup finalizes one whose object is
already in its scene snapshot or rolls it back to the original inventory folder
when no object was persisted.

Return-to-owner completed the task-inventory lifecycle acceptance on
2026-07-18. Returning the rezzed no-copy `Contents1` removed it from the scene
and created exactly one no-copy object in Jim Tarber's Objects folder. Rezzing
that returned object consumed the inventory item exactly once, retained
`No Copy Transfer Test` in Contents, disabled Take Copy, and rejected
Shift-drag duplication. The Region recorded derez destination `9`, followed by
one finalized durable object-rez transaction and one new scene object UUID.

The remaining Phase 1 inventory-content protocol is ready for live Firestorm
acceptance as of 2026-07-18. The Region accepts creator-attributed sound and
animation uploads, and it can create, fetch, and update personal notecards,
LSL source, and gestures. Direct viewer creation now supplies valid empty
notecard and gesture assets plus the conventional starter LSL source when no
separate asset transaction precedes `CreateInventoryItem`. New landmarks are
generated from the authoritative Region UUID and the avatar's current local
position. These server-created assets use fresh UUIDs, record the creating
avatar as creator, and are registered with the Grid for cross-region
replication. Firestorm visual creation, editing, playback, and relog tests are
still required before the corresponding Roadmap item is marked complete.

Object Contents editing now advertises the viewer-standard
`UpdateNotecardTaskInventory` and `UpdateScriptTask` capabilities as well.
Each request must identify an owned, modifiable object and a matching
modifiable task item. Its upload creates an immutable creator-attributed asset,
updates only that task item's asset reference, increments the task-inventory
serial, and persists the complete scene snapshot. LSL source is stored but is
reported as not compiled until the Phase 6 Homeworldz runtime is available.
Live Firestorm save-and-restart acceptance remains pending.

Dynamic physical-object synchronization now combines the previously accepted
10 Hz viewer updates with per-viewer interest and change tracking. A physical
body is sent only while its bounding radius intersects that viewer's draw
distance, only after a meaningful position, velocity, rotation, or sleep-state
change, and otherwise at a one-second heartbeat. Leaving interest clears the
per-viewer cache so re-entry forces a complete update. Earlier live tests
already confirmed smooth falling, collision, rolling, rotation, avatar pushes,
editing, and restart restoration; the new distance and transform predicates
have deterministic boundary tests.

Temporary-on-rez now has an authoritative Region lifecycle. Firestorm's
temporary flag is retained in object updates, starts or refreshes a 60-second
lease, and can be cleared before expiry. Temporary roots and their linked
children are deliberately omitted from every durable scene snapshot. At
expiry the Region removes their physics bodies and scene entities and sends a
reliable kill update to every connected viewer. Automated storage acceptance
proves a temporary object cannot reappear after restart; live viewer timing
acceptance remains pending.

Physical linksets are ready for live Firestorm acceptance. A physical root and
all collidable children now form one native Jolt compound using the children's
root-local positions, rotations, and analytic or convex prim shapes. The body
uses summed mass and mass-weighted contact settings; its authoritative root
motion continuously rebuilds the children’s derived world transforms. Linking,
delinking, whole-linkset and Edit Linked changes rebuild the compound, and
selecting a physical child suspends/restores the root body. Automated adapter
acceptance ray-casts against an offset, rotated child and verifies that the
complete linkset falls as one body. A viewer test covering collision on both
root and child, rotation while falling, editing, delink, and restart remains
pending before the Roadmap checkbox is closed.

Initial live Region-border crossing acceptance passed on 2026-07-18. Jim
Tarber crossed east from Welcome into Sandbox without invoking a teleport UI.
The Grid prepared, accepted, and activated transit
`f263cb50-b394-4d81-b470-a03738fef7d5`; Sandbox authorized the existing viewer
session and placed the avatar at local `(0.3, 127.21, 23.37)`, retaining the
recorded facing and flying state, and Welcome then retired its authority. The
same viewer immediately crossed west again. Reverse transit
`829b7629-8083-40c5-bd3b-882f95df35e7` activated in Welcome at local
`(255.7, 126.69, 23.37)` facing west and still flying, after which Sandbox
retired its authority. Both transitions retained the original session and
completed without a rollback or duplicate active Region. Broader acceptance
for appearance, held controls, velocity, camera continuity, repeated crossings,
mixed-size borders, attachments, sitting, and failure recovery remains open.

Initial 2x2 Region acceptance passed on 2026-07-18. Firestorm originally
constructed Beta as 256 by 256 metres after a Sandbox-to-Beta crossing because
Homeworldz omitted the OpenSim `RegionSizeX` and `RegionSizeY` extension fields
from `CrossedRegion`; the viewer consequently treated the internal 256 metre
line like a Region edge. Repeating both dimensions in `CrossedRegion.RegionData`
made Firestorm construct Beta as one 512 by 512 metre Region. Jim Tarber then
flew a clockwise loop around the four-quadrant centre and returned to the
northwest quadrant at `(223, 273, 23)`. Terrain, movement, and minimap position
remained continuous across both internal 256 metre lines.

The remaining 2x2 acceptance also passed on 2026-07-18. Terrain edits appeared
on the live world map immediately; map tiles remained correct at every zoom and
through idle time; persisted objects retained name, position, scale, rotation,
tint, and physics properties across a Beta Region restart and a clean viewer
relogin. The 2x2 acceptance scope is therefore complete; equivalent 4x4 live
acceptance remains open.

Initial 4x4 Gamma teleport acceptance passed on 2026-07-18. Firestorm displayed
Gamma as one 1024 by 1024 metre Region and successfully teleported Jim Tarber
into its southwest quadrant. A same-Region map teleport to the centre initially
failed because Firestorm quantized the requested destination handle to the
internal 256 m map tile and expected the simulator to map that tile back to the
containing variable Region. Homeworldz now accepts either the Region's
southwest-origin handle with full Region-local coordinates or any covered tile
handle with tile-local coordinates. After all cloud Regions received the fix,
the same map action completed normally at approximately `(514, 514)` without a
cross-Region transit.

Continuous 4x4 movement acceptance passed immediately afterward. Starting near
Gamma's centre, Jim flew east beyond local `x=768`, west across `x=512` and
`x=256`, north beyond `y=768`, and south across `y=512` and `y=256`. Movement,
terrain following, minimap position, and appearance remained stable across all
six internal tile lines, and Firestorm continued treating Gamma as one Region
without initiating a border crossing.

The remaining Gamma 4x4 acceptance passed after a clean Region restart and
viewer relog. In the far northeast quadrant, Jim made a visible terrain edit
and created the nonphysical `Gamma Persistence` prim at approximately
`(826, 786, 24)` with size `2 x 3 x 1`, Z rotation `30` degrees, and a
distinctive tint. The world map updated while the Region was live. After the
restart, the terrain data was byte-for-byte unchanged and the prim retained its
name, transform, tint, and physical state; Firestorm confirmed the terrain,
object properties, avatar appearance, Current Outfit, minimap, and world map.
Live acceptance of all three supported Region sizes is therefore complete.

Same-region location teleport also passed on 2026-07-18. The Region recognizes
its own handle, updates the avatar controller and Jolt character in place, and
returns reliable UDP `TeleportLocal` without creating a Grid transit. A local
teleport in Beta completed immediately at `(240, 140, 23.56)`. The preceding
rapid Beta-to-Sandbox-to-Beta sequence also exposed a separate EEP follow-up:
Firestorm requested an environment `settings_id` that was not present in the
asset service and briefly displayed a UUID load error. Homeworldz now packages
the standard default-day settings asset under its canonical UUID. A subsequent
trace showed that Firestorm could still process `AgentMovementComplete` before
the destination seed-capability response, selecting the default day instead of
the Region's legacy environment. The destination now gates movement completion
on that visit's seed response, with a bounded fallback if the viewer never
requests it. Two consecutive Beta-to-Sandbox-to-Beta round trips passed without
warnings or environment divergence on 2026-07-18.

The initial self-appearance completion echo passed in the same session. On a
clean last-location login, Firestorm received five wearable-cache hits and the
Region echoed the viewer's first complete nonempty `AvatarAppearance`; the
avatar resolved immediately with all six COF links worn.

Restored-platform avatar placement passed live Beta acceptance on 2026-07-18.
Jim Tarber logged out while standing on the persisted `Beta Quadrant Object`
and returned with his feet correctly supported by its top rather than falling
through and being displaced onto nearby terrain. Character creation now follows
Halcyon's mature physics-layer behavior: a capsule that genuinely overlaps
scene geometry is searched upward through progressively larger offsets until a
clear position is found, while exact surface contact and already-clear restored
positions remain unchanged. A brief viewer landing animation accompanied the
successful contact settlement and was considered appropriate.

## Falcon touch acceptance

Object touch dispatched into Falcon passed live acceptance on 2026-07-21 in the
Welcome Region. Firestorm enabled the Touch action on a prim carrying an enabled
compiled `touch_start` script, and clicking the prim produced the script's
`Touched!` chat. Getting there required three cooperating changes. The Region
now advertises the `SCRIPTED` and `HANDLE_TOUCH` object-update flags for prims
carrying enabled scripts, without which Firestorm greys out Touch and never
sends `ObjectGrab`. The initial `ObjectGrab` touch packet is decoded separately
from the physical `ObjectGrabUpdate` drag path; it dispatches `touch_start(1)`
to each enabled compiled script in the clicked prim and its linkset root through
a bounded per-script event queue, and grab-update drag motion fires no duplicate
touch. Finally, enabled task scripts are re-rezzed on Region startup and a script
rez, recompile, enable, disable, or removal re-broadcasts the object update, so a
freshly touch-enabled prim becomes touchable after a restart and without a relog.
