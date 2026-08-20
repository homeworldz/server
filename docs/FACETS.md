# Facet child circuits: implementation plan

The working companion to [ADR 0036](adr/0036-rectangular-regions-via-facet-presentation.md)
for its next milestone: *"the viewer sees into adjacent facets through the same
child circuits real crossings already exercise. Every facet is a permanently
live neighbor served from the same memory."* Written at the end of the first
live acceptance day (2026-08-19) for the session that builds it.

## Status: built 2026-08-19; crossings proven live 2026-08-20

Sandbox ↔ Sandbox 2 crossings accepted in Firestorm after five same-day
fixes (below): fast hand-off, correct facet in the title bar, attachments
intact, movement immediately live after promotion. Still to verify: a
sibling facet's objects/avatars visible before crossing, a second avatar
crossing while watched, clean teardown at relog/logout, and a
symmetric-NAT login (Ed Ashford's case, fixed in e20a8a8).

All eight steps below are implemented and compile; the eviction rule is
unit-tested (`circuit_registry_facet_children`, viewer-protocol tests). Not
yet proven in a viewer. Decisions made while building, where the plan left
room:

- Removal kills (derez, return, expiry, departure) fan out to **every**
  circuit a viewer holds (`for_each_viewer_circuit`) rather than tracking
  which facet knew the object — a kill for an id a circuit never carried is
  ignored by the viewer. The precise old-facet kill is reserved for the
  partition sweep, where a same-tick update on the new facet makes ordering
  matter.
- The partition change (step 6) is a 100 ms sweep over scene roots
  (`entity_last_facets`, beside the dynamic-transform cache), not a hook in
  every mover: it catches physics drift, edit drags, and avatars with one
  mechanism. An avatar's own session is excluded from both halves — its move
  is the ceremony's.
- Chat, avatar animations, and AvatarAppearance broadcasts stay on primary
  circuits: they are agent-UUID-scoped, and the viewer resolves them
  region-independently. Watch animations live; partition them if a sibling
  facet's avatar animates wrongly.
- A crossing into a facet whose circuit is alive sends **CrossedRegion
  alone**, carrying the seed that facet was established with
  (`session_facet_seeds`): a matching seed is a viewer-side no-op, while
  re-enabling a live region with a fresh seed rebuilds its capabilities and
  event poll in place — three rapid crossings of that crashed Firestorm in
  `process_enable_simulator` (2026-08-20, found on the first live day). The
  full three-event ceremony survives only for a viewer whose child circuit
  never came up or has died.

Live checks for the first session: stand in Sandbox and watch a prim rezzed
in Sandbox 2 (child backfill); walk across and back (promotion via
CompleteAgentMovement, old facet stays live — no DisableSimulator); a second
avatar crossing while watched (partition sweep kill+update, no double-draw,
no gap); relog and logout (child teardown, no dead neighbors on the minimap).

## Where the shipped v1 stands

A rectangular region runs as one macro simulation and serves one square facet
per viewer: the facet whose UDP socket the viewer's **single** circuit is on.
Packets are keyed by composite endpoint — `ip:port` for facet 0, `ip:port/f<i>`
beyond — so the circuit registry and every endpoint-keyed map distinguish
facets without knowing they exist (`facet_endpoint_key` / `endpoint_facet` /
`endpoint_transport`, file scope in `region/src/main.cpp` beside `send_udp`,
which routes a suffixed key to its facet's socket).

Crossing an internal line is a real ceremony with no handoff: the movement tick
sends EnableSimulator → EstablishAgentCommunication → CrossedRegion (one shared
seed visit id) for the sibling facet; the viewer connects to the new socket;
`CircuitRegistry::receive`'s same-identity eviction fires; the replaced-circuit
handler recognizes same-session/same-transport/different-key as an internal
move and calls `migrate_viewer_endpoint` instead of tearing down; the old
endpoint gets a bodyless DisableSimulator so the viewer drops it immediately.
CompleteAgentMovement on an existing avatar answers with facet-local position,
re-sends AvatarAppearance, and the shared backfill re-sends terrain window,
objects, and avatars on the new circuit.

Proven live in Firestorm: smooth two-way Sandbox ↔ Sandbox 2 crossings with
mesh attachments intact. The remaining visible seams: **a facet is blind until
you cross into it** (no updates from sibling facets), and the old facet's
retirement is a workaround for not keeping it alive at all.

## The target

Every viewer on a rectangular region holds one circuit **per facet**: a primary
(where the avatar stands) plus standing child circuits to every sibling. Each
circuit carries only its own facet's world — objects, avatars, terrain — in
facet-local coordinates. Crossing a line stops being an event that builds a
connection and becomes CrossedRegion promoting a circuit that already exists.

## Steps, in build order

1. **Share the endpoint-facet helpers.** `endpoint_facet` / `endpoint_transport`
   move from main.cpp file scope to a header (`homeworldz/viewer_protocol.h` is
   the natural home; `CircuitRegistry` needs them next).

2. **Relax circuit eviction to same-facet-only**
   (`region/src/viewer_protocol.cpp`, `CircuitRegistry::receive`, the loop that
   erases same circuit_code/session/agent entries). New rule: evict a matching
   identity only when `endpoint_transport` differs (a genuine relogin from a
   new address) **or** the facet is the same (a reconnect). Same transport +
   different facet coexists — that is a child circuit. Unit-test in
   viewer-protocol tests: same session on `/f1` does not evict facet 0; a new
   transport evicts everything.

3. **Establish children at arrival.** After the arrival backfill in the
   CompleteAgentMovement handler (main.cpp, the block that already knows
   `arrival_facet`), enqueue EnableSimulator + EstablishAgentCommunication for
   every sibling facet, exactly as the crossing ceremony builds them (hoist
   that ceremony's event construction into a helper — it is written once there
   today). The viewer then opens UseCircuitCode to every facet socket; step 2
   lets them all live. RegionHandshake per child already works (it is sent on
   any authorized UseCircuitCode, named per facet).

4. **Backfill a child circuit at its handshake.** When UseCircuitCode arrives
   on a facet whose composite key has **no** avatars entry but whose session
   has an avatar under another key, it is a child being established: send that
   facet's terrain window, its parcel overlay, and step 5's object/avatar set
   for that facet. (Today this state was unreachable — eviction forbade it.)

5. **Partition updates by the facet containing the object** (the ADR's rule).
   `object_update_for(recipient_endpoint, object)` currently rebases everything
   to the recipient's facet. Replace the recipient-selection at the broadcast
   sites: compute the **object's** facet from its root macro position
   (`facet_of_position`), rebase to that facet, and send on each viewer's
   circuit *for that facet* — primary or child — skipping viewers without one.
   A per-viewer send becomes: `facet_endpoint_key(endpoint_transport(primary),
   object_facet)` if `circuits.identity()` knows it. Avatars partition the same
   way by avatar position. Sites: `broadcast_object_update`, the dynamic-sync
   tick, the avatar-transform tick, the arrival loops, KillObject broadcasts,
   and the incremental terrain flush (already per-facet; widen its recipient
   test from primary facet to any circuit on that facet).

6. **Emit the partition change.** Track each entity's last facet
   (`std::unordered_map<EntityId, int>`, beside `sent_dynamic_transforms`).
   When a root entity's facet changes: KillObject on every viewer's old-facet
   circuit, ObjectUpdate on the new — the ADR's "atomic server-side" line
   crossing for objects. Same for avatars (this replaces nothing: the avatar's
   own primary swap stays the ceremony).

7. **Simplify the crossing.** With children standing, the internal ceremony
   sends **CrossedRegion only** (EnableSimulator/establish are redundant but
   harmless; drop them once proven). The primary swap trigger moves from the
   circuit-replacement hook (which stops firing — nothing is evicted) to the
   CompleteAgentMovement handler: an avatar found under a different facet key
   for the same session migrates there. Keep `migrate_viewer_endpoint`; keep
   the replacement hook as a fallback. DisableSimulator retreats to real
   departures: `clear_viewer_endpoint` must also remove the viewer's child
   circuits (`circuits.remove` per facet key) and disable each.

8. **Liveness and teardown.** Pings: `StartPingCheck` is answered per circuit
   already; `avatar.last_pong` keys off the primary — child circuits need no
   own liveness (they die with the primary in step 7's teardown). Audit
   `clear_viewer_endpoint` and the logout/timeout paths for child cleanup.

## Gotchas known in advance

- **Squares must be untouched**: facet_count == 1 short-circuits every step;
  keep it that way (facet 0 = bare key = today's behavior, byte-identical).
- The viewer namespaces objects per region: the same entity must never be live
  on two of a viewer's circuits at once, or it draws twice — step 6's kill
  must precede step 5's update on a facet change (one tick is fine, same loop).
- Attachments ride their avatar: partition by the **wearer's** facet, not the
  attachment entity's own position (state byte + parent, see
  `static_object_from_entity`).
- Chat still broadcasts region-wide with per-recipient rebased positions —
  range tests are macro; do not partition chat.
- The event queue stays one per session (HTTP, session-keyed); nothing there
  changes.
- Fan-out cost is the ADR's stated price: N circuits per viewer per rectangle.
  Nova and every square pay nothing.

## Also open (separate from child circuits, same punch list)

- Relog into a facet can race the spawn position and show "Unknown": extend
  the region's `/api/v1/agents/{id}/start-state` with `position` and use it for
  the grid's login facet pick (`resolveViewerLogin` currently uses the
  locations store, which can disagree with the region's persisted spawn).
- Inter-region (real border) arrivals can rez invisible until a Wear/rebake —
  appearance seeding on transit arrival; check whether it predates facets.
- Rectangle-only input seams from the implementation day: inbound rebasing of
  rez rays, MultipleObjectUpdate, ModifyLand, parcel-request rects; per-facet
  chat positions; within-macro teleports across a facet line (TeleportLocal
  must become the facet ceremony); the session transport's terrain descriptor
  is still a square `width`.

## Live layout for testing (cloud grid, 2026-08-19)

Welcome 1x1 @1000,1000 · Beta 1x1 @1000,1001 (wiped) · Sandbox 1x2 @1001,1000
("Sandbox 2" north, 22 m island, 8 m edge falloff) · Gamma 2x1 @1002,1000
("Gamma 2" east) · Homeworldz Strait 4x2 @1000,998 (all-water ocean) · Delta
4x2 @1000,996. The natural test: stand in Sandbox and watch someone (or a
rezzed prim) in Sandbox 2 before crossing.
