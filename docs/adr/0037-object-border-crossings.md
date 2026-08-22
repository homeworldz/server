# ADR 0037: Object Border Crossings

Status: Accepted

This ADR records **current expectation and intent**, not a commitment, and is
expected to be revised as evidence arrives.

**Revision (2026-08-22).** A non-physical object dragged over a border now
crosses instead of being refused. The refusal was not a neutral simplification:
the viewer predicts the move it asked for, so a silent refusal *looked* like a
success and reverted at the next relog — and once the object was believed to be
next door, the viewer addressed its edits there, where local ids mean something
else entirely. One such stray edit resolved to an avatar's own scene entity,
which every ownership and permission check accepts. Edits are now refused for
anything that is not a rezzed object, and a root leaving the extent is handed to
the neighbour that owns the far side by the same two-phase crossing a physical
object uses.

Not yet enforced: parcel and region entry permission, which the table below now
names as the one legitimate refusal.

**Implementation state (2026-08-20).** The disposition rules below are in the
region, and so is the crossing itself: the source detects a physical root
outside the extent, serializes it as a take-format linkset with a motion
envelope, stages it at the neighbor, removes its own copy, and activates.
The representation, the two-phase registry, and the idempotence rules are
unit-tested (`region-transit`). Not yet proven: a crossing between two live
regions, which needs the two-region harness the avatar handoff was proven
with. Vehicles are out of scope by construction — see the last section.

## The problem

Something moving reaches the edge of the region and keeps going. Until now
there was exactly one answer, written when there were no neighbors to hand
anything to: clamp the body inside the extent and cancel the velocity still
pointing outward. That answer is right only when nothing can accept the
object, and it is now the minority case.

The avatar half of this was solved first, because avatars are the thing a user
notices. An avatar crossing is a grid-mediated transit — prepare, accept,
activate, roll back — with a viewer session that has to be told to reconnect
somewhere else. An object has no session, no viewer to redirect, and no grid
record that has to move. It has something the avatar does not: a body in a
physics world, mid-flight, whose motion is the whole point of the crossing.

Two questions had to be answered together. What happens to each kind of moving
thing at the border, and how does one region hand an object to another without
the object existing twice or ceasing to exist.

## Every moving entity's disposition

Left undecided, each of these acquires an accidental answer — usually
whichever one the containment clamp happens to produce.

| Entity | At a border |
| --- | --- |
| Avatar (viewer) | Grid transit handoff, unchanged (ADR pre-dates this one) |
| Avatar (session client) | Re-entry: the client is told where to continue |
| Physical root object | Crossed to the neighbor that owns the far side |
| Child prim | Never alone; travels as part of its root's linkset |
| Attachment | Never alone; travels with its wearer, and is rebuilt at the destination from the grid's worn list |
| Temporary-on-rez object | Contained. Its remaining lifetime is not a thing the crossing carries, and sixty seconds of borrowed existence is not worth a protocol |
| Non-physical object, dragged over the line | Crossed, by the same handoff a physical one uses. Revised 2026-08-22: this row said the edit was refused where it was validated, and that was wrong — editing an object across a border is ordinary on every grid, and the only thing entitled to refuse it is a parcel or region that disallows entry |
| Anything, no eligible neighbor | Contained: origin clamped to the extent, outward velocity cancelled |

The attachment row is the one that looks like a gap and is not. A worn object
is already restored at every establishment from the grid's worn list, which is
the same path that dresses an avatar at login and after a teleport. A crossing
that also shipped attachments would be a second mechanism producing the same
prims, and the two would disagree the first time they diverged.

## The crossing itself

**One format.** An object travels as the very asset a take writes. This is not
convenience: the take format is the one representation already required to
preserve everything about an object across a round trip, and it is exercised
constantly. A second, crossing-only format would be a second place to forget a
field, and the failure it produces — an object that arrives subtly diminished —
is one nobody notices until much later.

What the asset deliberately does not hold is everything that distinguishes
*this* object from a copy of it: its identity, its owner, when it was created,
where it is, and how it is moving. Those ride beside it in the envelope. Two
consequences follow, and both are load-bearing:

- **Ids are preserved, never reissued.** The root's object id is what a viewer,
  a grid object-rez record, and anything holding a reference all name it by.
  Task inventory item ids are preserved too, which required suppressing the
  reissue that a rez performs — correct for a rez, which makes a new object,
  and wrong for a crossing, which moves an existing one.
- **Motion is carried explicitly.** The body's quaternion and angular velocity
  are the two things the object asset cannot say, and they are exactly what
  makes an arriving object continue its arc instead of landing dead at the
  border.

**Region to region, not through the grid.** What the destination has to trust
is the source region, which the shared service token already establishes. No
viewer session is moving that the grid would have to authorize, and no grid
record changes hands. Routing an object through the grid would add a table, a
state machine, and a second thing to reconcile after a restart, in exchange for
nothing the token does not already provide.

**Stage, remove, activate.** The order is chosen so that the two ways this can
fail are not equally bad:

1. The source stages the object at the destination, which creates nothing.
2. The source removes its own copy, persists, and tells viewers.
3. The source activates, and only then does the object exist at the
   destination.

Removing before activating means the object is briefly *nowhere* rather than
briefly in two places. Briefly nowhere is recoverable, and is recovered: the
source builds a recovery document before it removes anything, and rebuilds the
object here if activation fails. Two live copies of one object, colliding with
the world in two regions at once, is not recoverable — something has to decide
which one is real, and nothing is in a position to.

The remaining failures fall the safe way. A source that dies between staging
and activation leaves a staged entry that expires having created nothing, and
the object is still in the source's last snapshot. A destination that never
answers an activation it performed will be asked again: arrivals are remembered
past activation, so the retry is answered yes rather than rezzing a second
object.

## Why vehicles are not here

A vehicle is a scripted object driven by the Second Life vehicle parameter
model. Neither the parameter model nor the scripting it needs exists yet, so a
vehicle crossing has nothing to preserve that this ADR does not already
preserve, and cannot be tested against anything. It is listed at the end of the
scripting phase, where the things it depends on are built.

The same reasoning applies in reverse to what *is* here. A physical object
carrying its motion across a border, an object with attachments, and eventually
an object with avatars seated on it are the crossings a vehicle is later
assembled from. Building them without vehicles is not a substitute for vehicle
crossings; it is the part of vehicle crossings that does not need scripts.

## Consequences

- A physical object rolling off the edge arrives on the neighbor still rolling,
  with its identity, contents, and permissions intact.
- Two regions must share a service token to hand objects to each other. They
  already do, for asset replication and avatar arrival preparation.
- A crossing whose neighbor refuses or cannot be reached is retried, not
  hammered: the object is contained against the edge and tries again shortly.
- A seated-avatar bundle is unbuildable until seating exists (Phase 3). The
  crossing is shaped to take it — an object and everything riding on it move as
  one document — but there is nothing to seat yet.
