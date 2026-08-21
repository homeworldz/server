# ADR 0038: Cross-Region Child Agents

Status: Proposed

This ADR records **current expectation and intent**, not a commitment, and is
expected to be revised as evidence arrives.

**Implementation state (2026-08-21).** Nothing here is built. Neighbour
discovery exists and is trustworthy (`refresh_region_neighbors`, adjacency
validated, refreshed every 30s), and the whole child-circuit mechanism exists
for facets of one region (ADR 0036). What does not exist is any of it pointing
across a region border.

## The problem

A viewer holds a circuit to every region it can see, not just the one its
avatar stands in. Each of those neighbouring regions holds a **child agent**:
a presence that is not the avatar's home, but which knows the avatar, holds its
appearance and its attachments, and has already sent that region's world down
that circuit. Crossing the border then **promotes** the child to root. Halcyon
says so in one line — `CompleteMovement` opens with
`if (m_isChildAgent) { ... MakeRootAgent(pos); }`, logged as "Upgrading child to
root agent" — and everything else in `CompleteMovement` is bookkeeping.

Homeworldz announces only its **own facets** (ADR 0036). `region_neighbors` is
discovered and then used for nothing a viewer can see. So no neighbour ever
holds a child agent, and a crossing is not a promotion but a **cold
establishment**: in the tick the viewer is told it has arrived, the destination
creates the avatar, restores every attachment, bakes an appearance and
backfills the world. This is a thing the protocol never does, and the viewer
was never written to survive it.

Everything below was found chasing separate bugs, and all of it is this one:

| Symptom | Why |
| --- | --- |
| Attachments logged `unexpected` and not drawn after a crossing | They are news on arrival; normally the viewer knew them before crossing |
| The neighbour red on the minimap | No circuit to it, so no data from it |
| Water drawn to the border, no objects or avatars past it | Same |
| Long stall before a crossing completes | Everything a child agent would have done in advance happens at once |
| The avatar arriving as a cloud (2026-08-21) | Appearance re-established from nothing, at the worst moment |

The last of these was chased three ways in one day — seeding the arriver, then
advertising server-side bakes, then correcting the send order — and only the
third was even in the right subsystem. That is the cost of the cold path: every
symptom it produces looks like a bug in whatever subsystem happens to notice.

## Decision

Extend the facet child circuit **outward across region borders**. It is the same
mechanism: `EnableSimulator`, `EstablishAgentCommunication`, a seed capability,
and a backfill of that region's world onto a circuit whose avatar lives
somewhere else. ADR 0036 built all of it and pointed it inward.

Five parts, in dependency order. Parts 1 to 3 must land together: announcing a
neighbour whose seed does not answer stalls the viewer, which
`enqueue_facet_child_events` already knows and guards against for facets.

1. **Announce each neighbour.** For every adjacent region in `region_neighbors`,
   enqueue `EnableSimulator` and `EstablishAgentCommunication` naming *that
   region's* handle, simulator endpoint and seed. Not our own endpoint, which is
   the only thing the facet path has ever had to name.

2. **Establish the child at the neighbour.** A region-to-region call, modelled
   exactly on `POST /api/v1/transits/{id}/prepare-arrival`: service-token
   authorized, the destination decides, the source may retry. It carries the
   agent, the session, the circuit code and where the avatar is, and returns the
   seed the neighbour minted. The neighbour creates a child presence — known,
   dressed, not root.

3. **Back the child circuit with that region's world.** What
   `backfill_child_circuit` already does for a facet: terrain window, parcel
   overlay, objects and avatars of that region, in its coordinates, excluding
   the viewer's own avatar. A child is never made root by this step.

4. **Make a crossing a promotion.** `CompleteAgentMovement` at the destination
   finds a child for the session and promotes it. No create, no attachment
   restore, no re-bake, no backfill — the work is already done and the viewer
   already has it. The cold path stays as the fallback for a destination that
   holds no child, because a grid can always lose one.

5. **Tear the child down.** On leaving draw distance, on relog, on logout, and
   when the neighbour restarts and no longer knows the session.

## Consequences

**The appearance and worn set must reach the child at establishment.** That is a
grid read per neighbour per session, up to eight for a square region. It must
not happen on the sim tick; the tick already starves the viewer circuit and the
lease under long synchronous grid I/O, and this would multiply it. This ADR
therefore depends on moving that I/O off the tick, which was already queued for
its own reasons.

**State multiplies.** Every session gains a presence in every adjacent region.
A child that the neighbour has forgotten, but that this region still believes
in, is a new failure mode with no equivalent today — hence part 5, and hence
establishment being idempotent and retryable like the transit calls.

**The cold path does not go away.** It stays as the fallback, which means both
paths exist and only one is exercised in a healthy grid. The cold path must
therefore keep its own tests, or it will rot until the day it is needed.

**A region must be honest about what it cannot do.** The failure that produced
the permanent cloud on 2026-08-21 was advertising a capability the region did
not serve; a viewer treats such a bit as an instruction, not a description. Each
part above is a thing the viewer will then rely on, and must not be announced
before it works.

## What this does not decide

Server-side appearance baking as a service (`UpdateAvatarAppearance`, and with
it `RegionProtocols` bit 0) is a separate decision. A child agent removes the
crossing's dependency on it — the appearance is established before the crossing,
not during it — but it does not answer whether the region should bake on the
viewer's behalf at all. See ADR 0029.
