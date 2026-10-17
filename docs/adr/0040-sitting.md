# ADR 0040: Sitting

Status: Accepted

This ADR records **current expectation and intent**, not a commitment, and is
expected to be revised as evidence arrives.

**Implementation state (2026-10-17).** Built and on every region (9781d3b). A
prim with no sit target seats at Halcyon's fixed default offset; placing the
avatar on the prim's top face was considered and not adopted, as Halcyon left it
commented out. Not yet built: carrying sitters across a region border with their
object, which until then is held at the edge.

## The problem

An avatar cannot sit. Right-clicking a chair does nothing, and the Firestorm
"Sit Here" and "Sit on ground" commands go unanswered. Furniture is most of what
people build, so this is the most visible gap left in the world.

The operator's rule (2026-09-23) shapes the whole design: sitting ships complete
or not at all. A sit that ignores sit targets, or that seats an avatar in the
right place but in the wrong pose, makes every piece of scripted furniture look
broken, which is worse than furniture that visibly cannot be used yet. So the
unit is everything a typical sit script and a typical seated visitor touch,
built together and switched on together.

## Decision

### The unit

1. **Wire**: `AgentRequestSit`, `AvatarSitResponse`, and the stand and
   sit-on-ground controls.
2. **Seating in the region**: choosing a seat, attaching the avatar to it, and
   every way a seat ends.
3. **Falcon**: what a sit script needs to compile and run — `key`, `!=`, `&`,
   named constants, the `changed` and `run_time_permissions` events, and the
   sit, permission, animation and camera functions.

Ground sitting and the scripted sit camera are part of the unit. Permission
requests to anyone other than the avatar sitting on the object are not: they
need the viewer's permission dialog (`ScriptQuestion`), and are their own unit.

### Wire

Firestorm sends `AgentRequestSit` (High 6) naming the clicked prim and the
click point in its local space. The region answers with `AvatarSitResponse`
(High 21) carrying the seat in the prim's frame, the camera offsets, and
`AutoPilot = false`. With autopilot off, Firestorm neither walks the avatar over
nor sends `AgentSit`; the region seats the avatar as it answers, as OpenSim does
for a seat within reach. `AgentSit` is accepted and ignored.

A seated avatar is sent in `ObjectUpdate` with its `ParentID` set to the seat's
root prim (see below). That is what makes the viewer move it with the object. The default sit animation is played
through the avatar's ordinary animation set.

Standing is the `AGENT_CONTROL_STAND_UP` flag (0x00010000) in `AgentUpdate`.
Sitting on the ground is `AGENT_CONTROL_SIT_ON_GROUND` (0x00020000): the same
seated state with no parent object and the ground-sit animation.

### Choosing a seat

- The clicked prim's sit target, if it has one and it is free.
- Otherwise the first free sit target elsewhere in the same linkset, in link
  order.
- Otherwise, if the clicked prim has no sit target, a default seat on it: the
  viewer's own default offset, <0.34, 0, 0.55> in the prim's frame, facing the
  prim's forward axis.
- Otherwise the request is refused: every target in the linkset is taken.
  Halcyon falls back to the clicked prim here and stacks a second sitter on an
  occupied seat; refusing is what Second Life does.

One avatar per prim with a sit target, as in Second Life. A sit target is used
exactly as given, with no vertical correction — Halcyon's Second Life-accurate
mode, which this follows throughout.

### What the viewer is sent

Following Halcyon, the seated avatar's parent is always the **root** prim of
the linkset, with position and rotation in the root's frame, even when the seat
is a child prim: a child seat's offset is rotated by the child's local
rotation and added to its local position. The avatar's hip offset is
subtracted from the height, because the viewer draws an avatar from its hips;
without it a seated avatar sinks into its seat.

### While seated

- The avatar's physics capsule is parked, and its world transform is the seat
  prim's transform composed with the seat offset, recomputed whenever the object
  moves, is edited, or is rotated.
- Movement controls are ignored except standing; the camera is the viewer's
  sit camera unless the prim sets `llSetCameraEyeOffset` / `llSetCameraAtOffset`.
- Neighbouring viewers, on child circuits, see the seated avatar with the same
  parent relationship.

### How a seat ends

The avatar stands, with a world position just off the object, when it presses
Stand; when a script calls `llUnSit`; when the object is deleted, taken,
returned, derezzed, or unlinked from under it; and on logout, teleport, and
region restart. An object with an avatar seated on it does not cross a region
border yet — it is contained at the edge — until the crossing that carries an
object and its sitters together lands.

### Scripts

`changed(integer change)` fires with `CHANGED_LINK` when an avatar sits on or
stands from an object, in every script in that object. The functions:

| Function | Notes |
| --- | --- |
| `llSitTarget(vector, rotation)` | exists |
| `llSetLinkSitTarget(integer link, vector, rotation)` | `LINK_THIS`, `LINK_ROOT`, or a link number |
| `llAvatarOnSitTarget()` | the avatar on this prim's sit target, or `NULL_KEY` |
| `llAvatarOnLinkSitTarget(integer link)` | the same for another prim |
| `llUnSit(key avatar)` | stands an avatar seated on this object |
| `llRequestPermissions(key, integer)` | granted at once, with `run_time_permissions`, when the key is the avatar seated on this object and the request is `PERMISSION_TRIGGER_ANIMATION` |
| `llGetPermissions()`, `llGetPermissionsKey()` | |
| `llStartAnimation(string)`, `llStopAnimation(string)` | an animation in the object's contents, or a built-in name such as `sit` |
| `llSetCameraEyeOffset(vector)`, `llSetCameraAtOffset(vector)` | stored on the prim like the sit target |

Permissions end when the avatar stands; the animations the object started stop
with them.

## Why not the alternatives

- **Sitting without sit targets first.** Rejected by the operator: it puts
  avatars in the wrong place on every scripted seat.
- **Autopilot to the seat.** Firestorm walks the avatar over and then sends
  `AgentSit`. It adds a round trip and a walk that can be blocked, for a seat the
  region could grant at once; it can be added later for distant seats.
- **The permission dialog now.** Every sit script asks only the sitter, whom
  Second Life grants without a dialog; the dialog serves attachments, vehicles
  and HUDs, which are later units.

## Relationship to other ADRs

- [ADR 0036](0036-rectangular-regions-via-facet-presentation.md): a seated
  avatar is partitioned with its seat's facet.
- [ADR 0037](0037-object-border-crossings.md): an occupied object is contained
  at a border until the seated-bundle crossing.
- [ADR 0038](0038-cross-region-child-agents.md): child circuits carry seated
  avatars with their parent relationship.
