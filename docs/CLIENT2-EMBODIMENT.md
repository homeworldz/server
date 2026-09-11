# Session-avatar embodiment: design

The next region-session milestone after the observer session
([CLIENT2-TRANSPORT.md](CLIENT2-TRANSPORT.md)): a client on the WebSocket
session gains an avatar — spawn, movement, scene updates, positioned chat,
departure — alongside the LLUDP viewer path, without touching the viewer
wire. Grounded in a survey of the region's avatar machinery (2026-07-27);
file:line references are to that day's tree.

## The one honest summary of the existing machinery

Everything that *models* an avatar is already transport-neutral: the scene
entity, the `AvatarController` kinematics, the physics character, the
transit registry (agent/session-keyed), appearance and baking. Everything
that *addresses* an avatar is not: `avatars` and ten sibling maps are keyed
by the UDP endpoint string, identity is re-fetched from the circuit table
rather than stored, and ~40 inline fan-out loops hold already-encoded LLUDP
bytes at the moment of sending. Embodiment is therefore not a parallel
avatar system — it is re-keying participation and inserting one delivery
seam above encoding.

## Design decisions

1. **One avatar population, keyed by participant.** `avatars` re-keys on an
   opaque participant key — LLUDP keeps `"ip:port"`, sessions use
   `"ws:<session_id>"` — and `LiveAvatar` gains `transport` (lludp |
   session) plus stored `agent_id`/`session_id`/`circuit_code`, killing the
   `circuits.identity(endpoint)` round trips. Every existing
   `for (… : avatars)` loop then already covers both kinds.
2. **The delivery sink sits above encoding.** A `deliver(participant,
   update)` seam takes the semantic update — avatar transform, object
   update, kill, chat, appearance, animation — and switches on transport:
   the LLUDP branch is today's `encode_* / circuits.send / send_udp`
   unchanged; the session branch renders a JSON envelope onto the session's
   outbound queue. First cut carries the four kinds a session avatar needs:
   transforms, object updates, kills, chat. The two existing
   `broadcast_chat` call sites dissolve into it, and session chat gains
   distance filtering for free because the session avatar finally has a
   position.
3. **Movement input narrows to what the controller reads.**
   `AvatarController::apply` takes a `MovementInput` of the seven fields it
   actually consumes; `AgentUpdate` converts trivially. The UDP sequence
   gate stays LLUDP-only — WebSocket is ordered — while the 1 s
   transient-control expiry stays for both (it protects against stalled
   clients of either kind).
4. **Spawn and initial scene are extracted, not duplicated.** The spawn
   body inside `CompleteAgentMovement` (find-or-create entity by agent id,
   arrival/persisted/initial position, controller + Jolt character,
   presence) lifts into `spawn_avatar(...)`; the join backfill lifts into a
   per-kind `send_initial_scene(...)` — a session wants existing avatars,
   appearances, and objects, but not 16×16 terrain patch packets. The
   "later" arrived 2026-07-29 as the capability fetch: `GET /session/terrain`
   (region ticket bearer) returns the heightmap the region collides against
   as float32 little-endian meters, and the hello's `terrain` block states
   width, spacing, and the interpolation rule (each 1 m cell is two planar
   triangles along the (x, y+1)–(x+1, y) diagonal — Jolt's heightfield
   triangulation, which region movement samples by raycast). The hello's
   `avatar` block publishes the capsule contract (radius 0.3, support at
   ground + height/2, grounded tolerance 0.05) and the spawned reply carries
   the avatar's own computed height and hip offset. The support rule's
   honest domain, measured live 2026-07-29 (server-side samples, then the
   client core's cleaner sweeps): with Jolt active (every cloud region) z
   is the Jolt capsule's rest position, which the next section gives exactly.
   (An earlier revision of this document reported the opposite sign here —
   avatars resting *below* the flat arithmetic by −0.107 m at 11° and −0.331 m
   at 39°. Those measurements were real and their cause was a collision
   heightfield built with a stretched spacing, so the physics read ground up
   to a metre horizontally from the ground clients were shown; see ADR-less
   commit "The collision surface was stretched". Corrected 2026-07-30, and the
   figures are kept here only so anyone who read the old claim finds out it
   was retracted rather than quietly edited away.) The walkable limit —
   region-owned rather than Jolt's silent default,
   `character_walkable_slope_degrees` in physics.h (default 65°, Halcyon's
   MAX_WALKABLE_SLOPE, adopted 2026-07-29; per-region override
   `region.walkable_slope_degrees`), published in the hello avatar block as
   `walkableSlopeDegrees` — is strictly the
   **Changed 2026-08-08: `walkableSlopeDegrees` now bounds resting as well as
   traversal.** It is Jolt's `CharacterVirtual` max slope angle. Ground past it
   is Jolt's `OnSteepGround`, which the region no longer counts as grounded, so
   gravity applies and the avatar descends under the existing fall path. The
   hello says so with `slopeLimitGoverns: "traversal-and-resting"` and
   `restingBoundedBySlope: true`.

   The framing matters and is the client core's: the resting bound was never
   absent, it was **infinite**. This moves it to the same angle rather than
   adding a limit where there was none, and one angle covers both because Jolt
   derives both from `mMaxSlopeAngle` — a second published number would describe
   a distinction the engine does not make.

   *Previously, and corrected 2026-08-05:* the limit bounded traversal only.
   `grounded` was `IsSupported()`, true for `OnSteepGround` as well as
   `OnGround`, so an avatar rested motionless on ground far steeper than it
   could walk. This section had called the number the "grounded-versus-sliding
   boundary", describing behaviour the region did not have; the client core
   built a resting prediction on that wording and three fixture faces exposed
   it — resting repeatably on 70°, unable to walk up 65°. The wording is now
   true again, by changing the behaviour rather than the description.
   It is also still NOT an exactness threshold; the two
   differ by an order of magnitude and conflating them was a bug in both
   ends' first drafts.

   **The resting height, and what `groundedTolerance` is measured from.**
   Established by designed measurement on the operator's test slope
   (2026-07-30), ten points across 10-33 degrees agreeing within 6 mm - a
   residual now known to sit inside Jolt's own height quantization, which is
   `block relief / 255` and therefore scales with local roughness rather than
   being a fixed floor:

       resting_z = ground + height * supportOffsetFactor
                          + capsuleRadius * (sqrt(1 + gradient^2) - 1)

   The third term is a hemisphere resting on an incline: its centre is a
   perpendicular distance `capsuleRadius` from the surface, so it sits
   `capsuleRadius * sec` above the ground vertically beneath it rather than
   `capsuleRadius`. It is zero on the flat and derives entirely from constants
   already published, so it is not a new field - but it **must** be included
   before `groundedTolerance` is applied. The tolerance is 0.05 m above
   `resting_z`, not above the flat arithmetic. Applied to the flat arithmetic
   it calls a *standing* avatar airborne from about 31 degrees upward, and
   0.41 m adrift at the 65 degrees a region will still stand an avatar on -
   which is exactly the fault it caused in the client core's predictor before
   the law was known. The region does not have that fault where Jolt is
   present, because `grounded` comes from Jolt's own contact state rather than
   from this arithmetic; the non-Jolt fallback is flat-only by construction,
   having a scalar ground height and no gradient to work from.

   **Domain, measured rather than assumed** (client core on the operator's
   second test hill, 2026-07-30). The correction is exact to about 43 degrees
   and then progressively over-predicts - the region rests an avatar *lower*
   than the geometry says:

       18.8 deg  +0.001    35.9 deg  +0.003    48.7 deg  -0.014   (9% of the term)
       28.9 deg  -0.002    43.4 deg  +0.002    64.4 deg  -0.087  (22% of the term)

   The onset between 43 and 64 is unmeasured, and no mechanism is claimed:
   two were offered for deviations on this surface and both were wrong. The
   correction still earns its place, because at 64 degrees the flat rule is
   0.39 m out and this is 0.087 m out, and it is exact across the range real
   terrain mostly occupies. Use it with the domain attached rather than
   widening the claim - "it is geometry so it holds everywhere" has now been
   true up to a point three times running.

   The flat arithmetic is also the non-Jolt fallback's clamp. Session z is the
   capsule center everywhere — the transform
   envelope briefly carried the hip-shifted viewer convention, found by
   the client core's ground comparison and fixed the same night. A fetch is a snapshot,
   so in-world terrain editing announces itself to connected sessions with a
   `terrainChanged` event naming the dirty 16 m patches (the event is itself
   named in the hello's terrain block as `changedEvent`). Notification is
   deliberately **lossy and detectable** rather than reliable (client core
   measurement, 2026-07-30): a monotonic per-region `revision` rides the hello,
   every `terrainChanged`, and the heightmap's `ETag`, so events may be
   coalesced or dropped under load and a client still knows it is behind. The
   region emits at most one event per 250 ms carrying the union of everything
   dirtied since the last, because one event per brush tick cost each client a
   whole-heightmap fetch - 4 MB on a 1024 region - and queued fetches faster
   than they completed. `If-None-Match` answers 304 for a current client, and
   the heightmap honours `Range` (`Accept-Ranges: bytes`), which matters
   because the map is row-major: dirty rows are contiguous, so 16 rows of a
   1024 region is 64 KB rather than 4 MB. Reconnects and crossings should
   compare the revision rather than assume nothing was missed - that is where
   a lost edit used to survive unnoticed. Canonical asset
   bytes reach a session through `GET /session/assets/{id}` on the same
   ticket, announced in the hello as `assets.base` (a base to append an id to, unlike
   `terrain.path`, which is complete); the reply's Content-Type
   names the format actually stored, because a mesh uploaded through the
   session path is glTF while one uploaded by a viewer is Second Life mesh
   (ADR 0033 stores each verbatim). Scene objects carry an optional
   `geometry` block (`assetId` plus `kind`, mesh or sculptMap) so a client
   can draw what an object is rather than a placeholder box - absent for
   prims, so a reader written before it keeps working.
   **Asset caching (2026-07-31).** Every asset response carries an `ETag` that
   is the digest of exactly what was served, and honours `If-None-Match` with a
   bodiless `304`. Freshness is two answers, not one, and the difference is
   invisible from outside — which is why it is published rather than left to be
   inferred. A **canonical** representation is `immutable`: canonical bytes are
   never rewritten (ADR 0026), so a cached copy is good for the life of the id
   and a returning client needs no round trip at all. A **derived** one — where
   the stored asset is Second Life mesh and the glTF rendition stands in for it
   — is `no-cache` and must be revalidated, because a rendition is regenerated
   when the converter's generator changes, and until one exists the route
   answers with the legacy bytes, so two requests seconds apart can differ in
   body *and* `Content-Type`. Store either; trust only the canonical one
   **Textures convert both ways too (2026-07-31).** A texture a viewer uploaded
   is canonically JPEG2000, which this client refuses by rule, so `png-texture`
   derives a lossless PNG from it and the asset route serves that instead —
   exactly as it serves glTF for a canonical Second Life mesh. So a texture
   created in Firestorm is readable here without exception, and the
   `Content-Type` names what was actually served rather than what the id
   nominally holds. Being derived, it is `no-cache`; only canonical bytes are
   immutable. The conversion recovers the pixels the viewer stored, not the
   detail its uploader already discarded — Firestorm downsizes anything over
   1024 and encodes lossily on the way in, so uploading through this client
   preserves more.
   The hello also
   states the region's water as `water: {height}` — a height, not a
   surface, because the plane is flat and region-wide and everything about
   how it is drawn is the client's business (client core, 2026-07-30).
   Viewers had this from the start in `RegionHandshake`; session clients
   were told nothing, so a client comparing itself to Firestorm was
   comparing a guessed water line against a published one. It is now
   `region.water_height` on both paths, defaulting to the 20 m the
   handshake had always hard-coded.
   **The ground's surface (2026-07-31).** `terrain.layers` names the four
   textures and the elevation band that selects between them: `assets` lowest
   to highest, and `lowHeight` and `highHeight` per corner in the order
   south-west, north-west, south-east, north-east, interpolated bilinearly
   across the region.
   **They are `startHeight` and `heightRange` — a start and a span, not two
   absolute bounds — and this section said otherwise for a day.** The viewer
   computes a composition value and blends the four layers by it
   (`llvlcomposition.cpp`):

   ```
   t = clamp((height + noise − startHeight) * 4 / heightRange, 0, 3)
   ```

   Layer 1 is pure at `t = 0`, layer 4 at `t = 3`, linear between neighbours, so
   the boundaries sit at half-integer `t`:

   | boundary | height |
   | --- | --- |
   | 1 │ 2 | `start + 0.125 × range` |
   | 2 │ 3 | `start + 0.375 × range` |
   | 3 │ 4 | `start + 0.625 × range` |
   | layer 4 pure | `start + 0.75 × range` and above |

   **Retraction of a retraction, with the reason, because a reader deserves to
   know which way to trust this file.** An earlier revision published
   `startHeight`/`heightRange` with a four-way split — correct. On 2026-08-04 that
   was retracted in favour of `lowHeight`/`highHeight` as absolute ends, on the
   strength of the viewer's Region/Estate dialog, which labels the two spinners Low
   and High and states that "the LOW value is the MAXIMUM height of Texture #1, and
   the HIGH value is the MINIMUM height of Texture #4". That text describes a model
   the viewer's own renderer does not implement. The widgets are named
   `height_start_spin_N` and `height_range_spin_N`, are filled from
   `getStartHeight()`/`getHeightRange()`, and the arithmetic above is what an
   operator sees. It was believed over the source, which was vendored in the server
   repository the whole time.
   An operator's screenshot caught it: sand at 22 m on a region set to 20 and 60.
   `t` there is 0.13 — layer 1 almost pure — so the numbers were behaving exactly
   as documented above and the published rule was wrong. **A client that implemented
   the retracted rule renders boundaries a viewer does not**, which is the
   divergence both sides have been working to avoid.
   To place boundaries at 20 / 40 / 60 an operator sets start 10 and range 80, not
   20 and 60. Worth knowing when comparing a render against a viewer's.
   A consequence worth stating: with the default start of 10 m and range of 60 m,
   the 1│2 boundary sits at 17.5 m — below the default water height of 20 m — so
   layer 1 has little dry ground to appear on until an operator raises the start.
   They are ordinary assets, fetched from `assets.base`, canonical PNG since
   the layers were re-sourced — so a client that refuses JPEG2000 can read the
   ground it stands on, and a cache can hold them for the life of the id.
   `gridWide` **was `true` and is now per region (2026-08-04).** It said these
   were compile-time constants shared by every region; they are now operator
   state, set from the viewer's own Region/Estate → Terrain tab and persisted
   per region. The field survives with its meaning intact — it reports whether
   *this* region still holds the shipped defaults — so a client that read the
   fact rather than assuming grid uniformity needs no change, which is the whole
   reason it was published while it was still trivially true.
   Read `assets`, `startHeight`, and `heightRange` from the hello per region and
   per connect. A client that cached them once for the grid will now be wrong on
   any region an operator has changed.
   **A connected client is told:** `changedEvent: "terrainLayersChanged"` names
   the event, discovered from the block itself rather than from this document,
   exactly as `terrain` names `terrainChanged`. It carries `layers` — the same
   block, from the same function — so handling it is re-reading what was already
   parsed once at greeting. No revision and no refetch: the block is the whole
   state.
   **Correction (2026-08-04): an earlier revision of this section claimed a viewer
   *cannot* be told, and called this the one place the session path is better than
   the legacy one. Both were wrong.** The reasoning given was that `RegionHandshake`
   is the only message carrying terrain and that re-sending it mid-session restarts
   more region state than a texture change warrants. Reading the viewer instead of
   reasoning about it settles it the other way: `unpackRegionHandshake` diffs the
   composition, calls `dirtyAllPatches()` when it changed, and refreshes the
   Region/Estate floater — it is written for the repeat. Firestorm's own comment on
   the PBR path spells the exchange out: "viewer: POST ModifyRegion / simulator:
   RegionHandshake / viewer: GET ModifyRegion". The region now re-handshakes every
   connected viewer on commit. The symptom that exposed it was an operator changing
   the elevations, reopening the form, and seeing the old values.
   So the two transports are equal here, which is the intended state. What remains
   true is the reason the event exists at all: a session client has no
   `RegionHandshake`, so it needs its own message to be told the same thing.
   **`water` now changes too, and names its own event.** It was fixed for the life
   of a region process, which is why the greeting alone used to suffice; the
   Region/Estate form can now set it per region, so the block carries
   `changedEvent: "waterChanged"` and that event carries the same `water` block from
   the same function. The note that said water needed no event is superseded.
   **No blend width is published, and nothing is missing.** `selection`
   determines the crossfade: neighbouring layers blend linearly between their
   peaks, so a transition is `heightRange / 4` wide and follows from the rule.
   A `blendMetres` field was published here until 2026-08-05 and is **retired**.
   It came from a region setting invented when the layer model was believed to be
   two absolute bounds, where a crossfade genuinely was a separate quantity. Under
   the real arithmetic it contradicts the rule beside it — it said 2 m on a region
   whose rule gives 15 — and a client applying both draws to neither. The client
   core found it within minutes of the corrected rule shipping.
   **Which strings in this block are a contract and which are prose.** `selection`
   is a contract: its wording changes only when the meaning changes, *including on
   a pure rename*, so a client comparing it whole refuses rather than silently
   drawing to a superseded rule. That is a commitment this side keeps — it is what
   made the 2026-08-05 correction cost minutes instead of another screenshot, and
   it is why the client core compares it rather than parsing it. `boundaryNoise` is
   prose and may be reworded freely; it is informational, and a client that refused
   over its exact text would stop painting the ground over a sentence.
   The viewer's **noise term** is published, as `boundaryNoise` in the same block.
   `llvlcomposition.cpp` adds a two-octave turbulence sum to the height before
   computing `t`, so a viewer perturbs every boundary by a few metres in a pattern
   no one has reproduced outside Linden. **This region adds none**, and the field
   says so, so it cannot be read as something to reproduce.
   It is on the wire rather than only here because of where it gets used: a client
   that reads it can print it on the run that produced a render, which is where
   somebody comparing a screenshot against Firestorm is actually looking (client
   core's argument, 2026-08-05). It converts "the two will differ and nobody knows
   why" into a bounded, explained difference — the boundaries agree exactly and
   only the wobble along them does not — and it pre-empts a side-by-side being read
   as a rendering fault on one side.
5. **Departure splits into what the avatar owns and what the viewer owns.**
   `retire_avatar(key)` (kill broadcast, physics removal, avatar-keyed maps)
   serves both transports; `clear_viewer_transport(endpoint)` (texture
   queues, xfers, event responses) stays LLUDP-only. Session liveness is
   socket close plus protocol ping — no LLUDP ping/pong; the grid-session
   revalidation is already session-id-based and applies unchanged.
6. **Draw distance is explicit or defaulted, never zero.** `draw_distance
   = 0` means "no filter" to the dynamic-object interest check, so an
   embodied session that never reports one would receive every dynamic
   object in the region. The session's spawn message carries an optional
   `drawDistance`; absent, the server applies a conservative default
   (128 m) rather than infinity.
7. **Appearance names one body asset per avatar. Decided 2026-08-05, not yet
   implemented.** The client core asked which shape to build toward — a body asset
   per avatar, a set of wearables to compose, or both — because it decides whether
   they fetch one mesh or assemble several. The operator's answer: **one body asset
   per avatar, and wearing a body replaces any body already worn.**
   So a client fetches **one** mesh and composes nothing. There is no wearable list
   to resolve, no layering of bodies, and no client-side assembly order to get
   wrong. Replacement rather than addition also means there is no state where an
   avatar has two bodies, which is a state worth not being able to represent.
   **Why this shape rather than a wearable set**, since the alternative is what the
   legacy family does. This project publishes the resolved fact rather than the
   parts, and has done so consistently enough that the exceptions are the notable
   ones: `motion` is a state and not a clip, water is a height and not a surface,
   the walkable limit is a number and not a slope field. Appearance as a wearable
   list would be publishing an implementation of an avatar's look and asking every
   client to arrive at the same answer from it — which is the class of thing that
   diverges silently, as the terrain layer rule did twice in a day. The pieces
   already lean this way too: `spawned` carries a per-avatar height and
   `hipOffset`, and the bake path already composes textures server-side rather
   than shipping wearables for a client to combine.
   **What is decided is the shape, not the wire.** Still open, and deliberately not
   guessed at here: the envelope and field names, whether the resolved baked
   textures ride in the same block as the body reference or a neighbouring one,
   attachment points, and how a body is *chosen* at all — there is no inventory,
   library, wear or attach surface in the session protocol yet, which is the client
   core's second blocker and remains open. A body asset that nothing can be told to
   wear is not yet useful, so the two land together or not at all.
   **A consequence for the bodies already in the Library.** "Male" and "Female" are
   static bind-pose previews rigged to MakeHuman's skeleton, so they are not
   candidates for this: a body worn by an avatar has to be weighted to the viewer's
   own 71 named joints to serve both client families from one asset, and rigged
   upload is refused until M4. The first body this contract can name is one that
   has been re-rigged.

8. **Appearance first cut: the server bake.** A session avatar seeds the
   default-outfit server bake exactly as an appearance-less viewer does, so
   viewers see something sane. Sessions do not receive `AvatarAppearance`
   blobs in the first cut (texture-entry byte blobs are a legacy shape);
   what a session client renders for other avatars is deferred to the
   asset-format work (ROADMAP Phase 10).
   **Motion is published, as a name (2026-08-04).** The client core named three
   blockers behind drawing avatars as anything but capsules, and observed that
   the third — "nothing publishes animation, not one field" — was the one
   already computed server-side and simply never given an envelope. It has one
   now. `motion` is one of `stand`, `walk`, `run`, `jump`, `fall`, `fly`,
   `hover`, `hoverUp`, `hoverDown`, `land`.
   A **name, not the animation UUID**, for the same reason water is a height and
   not a surface: the id names Linden-authored content that ships inside the
   viewer, so it identifies an animation only to something that already has it,
   and is useless to a client built from nothing. The name says what the avatar
   is doing and leaves how to draw it entirely to the client.
   It arrives two ways, the same fields either way. The `avatar` announcement
   carries the current value, so a client that arrives mid-stride is not left
   with an avatar standing still; a `motion` envelope (`id`, `motion`, `clips`)
   follows on every change. On change and not on every transform — transforms run
   at frame rate and this does not — and behind the same interest filter, so you
   are never told about an avatar you were not told exists.
   **`clips` is the custom case, and it was already diverging.** A viewer can
   start any animation by asset id through `AgentAnimation` — gestures, anything
   a script plays — and the region broadcasts those to other viewers. Session
   clients were told nothing, so an avatar mid-gesture read as animated to one
   client family and idle to the other. `clips` is the array of animation asset
   ids playing that `motion` does not describe; the movement animations are
   filtered out of it, so no fact arrives twice.
   **Do not fetch them yet.** They are legacy animation assets with no modern
   rendition — the same wall as JPEG2000, and the animation-format decision
   (canonical glTF, legacy derived) has not been made. Publishing them anyway is
   the deliberate choice, on the client core's own principle that "not advertised"
   must read differently from "advertised and broken": a client that knows a clip
   is playing and cannot draw it is better placed than one that believes the
   avatar is standing. A gesture played while standing still emits a `motion`
   envelope of its own, since otherwise it would wait for a movement change that
   might never come.
   The shape — a short state name always present, plus an optional asset id for
   creator content — is the client core's proposal (2026-08-04) and mirrors how
   an object names both a kind and an asset. Their argument for state over clips
   is recorded because it is stronger than convenience: **state is the fact and a
   clip is a rendering of it.** A client that thinks you are standing while the
   region has you sitting is wrong about the world, not about the picture, which
   puts motion in the same category as the walkable limit, the terrain selection
   rule and the water height. It also degrades honestly — an unknown state name
   still draws a capsule and says so, where an undecodable clip is geometry that
   cannot move, which is the T-pose failure again — and it earns its place even
   in a client that draws capsules forever, since prediction is better with the
   region's own view of what the body is doing.
   This does **not** answer the other two blockers, and is not meant to: there is
   still no canonical glTF body (the legacy one is viewer-licensed content, the
   same wall the terrain layers hit) and appearance is still unpublished. The
   capsule is still the honest amount. What changes is that a capsule can now
   report what it is doing, and the state will be there when a body arrives.
9. **Crossings cost one new message.** The transit machinery needs no
   change; the session equivalent of `EnableSimulator`/`CrossedRegion` is a
   single `crossing` envelope carrying `{regionHandle, sessionURL of the
   neighbor, position, lookAt}` — and the neighbor's session URL is already
   in the lease store. Deferred to the second cut, after single-region
   embodiment stands.

## Wire messages (session envelope kinds)

Field-level contract, agreed with the client core 2026-07-27. Conventions
as everywhere on this surface: the `{type, version, correlationId?,
payload}` envelope, camelCase keys, vectors as 3-element JSON arrays,
errors as `{code, message, field?}`. Optional fields are omitted, never
sent as zero values. Fields are only ever added to these payloads, never
repurposed.

Client → server, after auth:

- `spawn` `{drawDistance?}` — requests embodiment; answered by `spawned`
  or an `error`. An observer session stays legal: no spawn, no scene
  traffic, chat region-wide as today.
- `move` `{controls, bodyRotation: [x,y,z], camera?: {center, at, left,
  up (each [x,y,z])}, drawDistance?}` — `controls` is the `control_flags`
  bitfield unchanged. Applied at most once per tick, last-write-wins
  within a tick. **Draw distance is session state, not per-move input**:
  the server keeps the last value (from `spawn` or a `move` that carried
  one) and re-fills it into every `MovementInput`, so a `move` without it
  can never write the zero that means "no filter". Values clamp to
  [16, 512]; a session that never sends one gets 128. A `move` without
  `camera` keeps the previous camera.
- `say` `{message}` — public chat at the avatar's position, normal radius,
  same 1-2048 character rule as instant messages.
- `leave` — payload omitted entirely; retires the avatar, keeps the
  session open (back to observer).

Server → client:

- `spawned` `{entityId, position: [x,y,z], lookAt: [x,y,z]}` — `entityId`
  is the scene entity id as a decimal string, the same id `transform` and
  `kill` use.
- Initial scene after `spawned`: one `avatar` per present avatar, one
  `object` per entity, then live traffic.
- `avatar` `{id, userId, position: [x,y,z], rotation: [x,y,z]}` — display
  names arrive with the name-resolution work, additively.
- `object` `{id, objectId, ownerId, name, position: [x,y,z],
  rotation: [x,y,z,w], scale: [x,y,z]}` — enough to place a box; prim
  shape and materials arrive with the Phase 10 asset work, additively.
- `transform` `{id, position: [x,y,z], velocity: [x,y,z], rotation}` —
  interest-filtered by draw distance. **`rotation` length discriminates
  the form**: 3 elements is an avatar's body rotation (the same triplet
  the LLUDP path carries today), 4 is an object's quaternion `[x,y,z,w]`.
- `kill` `{ids: [...]}` — entity ids leaving the scene.
- `chat` `{from, message}` today; grows additively to `{from, fromId?,
  position?, message}` when embodied chat lands — existing readers keep
  working.
- `hello` (the auth reply) carries, additively since 2026-07-28,
  `movement: {walkSpeed, runSpeed, flySpeed, jumpVelocity, gravity}` and
  `interestSweepMs`. The movement block is the region's authoritative
  movement model for client-side prediction of the client's own avatar —
  the same constants the server controller computes with, published so no
  client hard-codes an observation. Semantics a predictor needs beyond the
  numbers: diagonal input is normalized (never faster than straight);
  horizontal velocity is control-driven while flying or grounded, but an
  airborne non-flying avatar is ballistic with directional input still
  steering; flight uses `flySpeed` and the walk/run gait does not reach it,
  as in Second Life, so a client must not fold its run state into flight even
  though the two numbers are equal today; avatar capsule height
  is per-avatar (from its shape), not a constant. `interestSweepMs` is the
  avatar-interest sweep period — the floor on remote-transform staleness,
  which is what an extrapolation cap should be derived from.
- `crossing` (second cut) as above.

JSON first, per the encoding decision; the first-byte rule leaves room for
a packed transform encoding if Phase 2 measurement demands it.

## E2: a session crossing is a re-entry, not a handoff

The design left "one crossing envelope" unresolved on the question that
actually decides the shape: **what credential does the client present at the
destination?** Its region ticket names the region it was minted for, so it is
refused next door — correctly, that is the audience and region claim doing
their job.

Two candidate answers. The **handoff** model mirrors viewers: the source
region asks the grid to mint a ticket for the neighbor and hands it over with
the crossing, keeping one grid session across the border. The **re-entry**
model has the client run world entry again for the named destination,
receiving a fresh ticket and session by the path it already used to arrive.

**Decided: re-entry.** The handoff model would let a region obtain a
credential for a *different* region — a new trust surface on machines
[ADR 0028](adr/0028-untrusted-region-trust-model.md) explicitly does not
trust — and it would need the destination to accept an avatar staged against
a session id the client is about to replace. Re-entry needs no new endpoint,
no new trust, and no change to the viewers' transit machinery, which stays
exactly as it is. What the client already holds makes it cheap: world entry
accepts `Region/x/y/z`, so the arrival point survives the crossing exactly.

The honest cost: **a crossing is not atomic for sessions.** The avatar is
retired here before it exists there, so there is a brief gap — a REST round
trip plus a socket open — where it is in neither region, and other
participants see it leave and arrive rather than slide across. Viewers keep
the seamless path. This is acceptable because the grid channel, not the
region session, is what must survive a crossing, and it does: instant
messages and notices are unaffected by a region change, which is the whole
reason that channel is anchored to the grid.

The envelope, sent to the crossing session and then the avatar is retired
in the same tick (so it never wanders outside the region):

```json
{ "region": "Welcome", "sessionURL": "wss://…/session/welcome",
  "start": "Welcome/250/128/23", "position": [250, 128, 23], "lookAt": [1, 0, 0] }
```

`start` is pre-formatted for handing straight back to world entry, so no
float formatting can drift between the two sides. A client that ignores the
crossing simply stops moving: containment applies toward any neighbor that
serves no session, and its last position is persisted, so `start=last`
recovers it.

## Interest: sessions now, viewers on a Firestorm pass

A session holds an **interest set** — the avatars it currently knows about.
Avatar `transform` frames flow only for pairs already in that set, and a
100 ms sweep emits the boundary events: an `avatar` message when a body comes
into range, a `kill` when it leaves.

**The sweep evaluates every observer-subject pair rather than reacting to the
subject's motion**, because interest changes when *either* party moves. A
subject-driven design looks correct in every test where the moving avatar is
the one being watched, and is silently wrong the moment an observer walks
away from someone standing still — that observer is never told, and its
client holds a body frozen at a stale position forever. Verified in exactly
that direction.

Rules worth stating: **self is always in interest** (an avatar's own
transforms are its authoritative position), spawn seeds the set from what is
genuinely in range rather than announcing the whole region, and retirement
forgets the avatar in every other session's set so a later arrival is
announced afresh rather than assumed known. Draw distance is the session
state described above, so it can never be the zero that means "no filter".

**Objects follow the same discipline**, and for the same reason: a client
keeps what it was told about. Every path that kills an object for viewers —
link-set removal, derez, temporary expiry, auto-return — kills it for
sessions too, a dynamic object leaving interest is said out loud rather than
having its transforms silently stop, and first sight of one is an `object`
introduction rather than a `transform` for an id the client never learned.
So for both kinds: **a `kill` means gone or out of view, and either way
remove it** — if it matters again, an introduction follows.

**Viewers remain region-wide, deliberately.** The same narrowing for the
LLUDP path changes what a legacy viewer sees — bodies would need killing and
re-rezzing at the boundary — and that is a visible behavior change on the
compatibility-critical path with no automated acceptance test behind it. It
wants a manual Firestorm regression pass before it ships, and is left
unchecked on the roadmap for that reason rather than forgotten.

## A quiet client is not a dead client

Established by the client core's browser work, 2026-07-27, and binding on
anything added later. **Browsers throttle hidden pages** — a tab hidden long
enough runs timers roughly once a minute ("intensive throttling"), and may be
frozen outright. A backgrounded client therefore stops sending keepalives,
while its socket stays open and reads normally.

Two consequences the server side must honor:

- **No idle or pong timeout below about 120 seconds** on the region session
  or the grid channel. A tighter one disconnects users whose only fault is
  looking at another tab. The session has no idle timeout today, and the
  auth deadline (10 seconds, before any traffic) is unaffected.
- **A stalled reader must not grow the region's memory.** Each session's
  outbox is bounded: past a soft limit, frames a later one supersedes
  (transforms) are dropped first; past a hard limit the oldest go
  regardless, logged as `session outbox trimmed`. A client that was stalled
  long enough to lose frames resynchronizes by spawning again — the initial
  scene is the resync mechanism. Durable traffic is deliberately not here:
  instant messages live on the grid channel with store-and-forward.

## Milestones

- **E1 — embodied presence:** spawn/move/leave, transforms and kills both
  ways, positioned chat, draw-distance interest. Viewers see session
  avatars; session clients see the scene move.
- **E2 — crossings:** the `crossing` envelope, transit parity with viewers.
- **E3 — appearance/assets:** what session clients render for avatars,
  with the Phase 10 asset formats.

Out of scope throughout: capabilities/EventQueueGet, UDP texture transfer,
inventory xfer — a session client fetches assets over HTTP.
