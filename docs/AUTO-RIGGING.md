# Automatic rigging and rig retargeting

**Status: design sketch, not scheduled. Belongs to M5 ([ROADMAP.md](ROADMAP.md)
Phase 4) and blocks nothing before it.**

## The goal, stated as a creator's experience

A creator has a humanoid mesh — bought, generated, or modelled. Today, getting it
into a Second Life-lineage world means Blender plus Avastar or Bento Buddy: a
paid add-on, a rigging workflow, and a skill set unrelated to the thing they
actually made.

The goal of this document is to make that unnecessary for the common cases. Not
to build a general automatic rigger — to remove Blender from the path a creator
most often walks.

That distinction decides everything below. The interesting research problem
(rig an arbitrary unrigged mesh) is **not** the problem standing between creators
and Homeworldz, because the tools they actually use already rig their output.

## What already works, so we do not rebuild it

Worth stating first, because it is more than it sounds:

- A mesh rigged to Bento joint names uploads and is accepted today
  (`rigged_accepted`, 2026-08-08).
- Joint names resolve through a 153-entry alias table, so a body does not have to
  spell them the way Firestorm's own files do.
- Unused joints compact away, so a source declaring an entire armature is not
  refused for exceeding the per-mesh joint budget.
- The bind geometry is checked against the skeleton's rest pose, so a body that
  maps *by name* but not *by position* is refused rather than shipped broken
  ([rig_check.h](../region/include/homeworldz/rig_check.h)).

So a creator whose tool emits Bento-compatible names already needs no Blender.
What follows is about the creators whose tools do not.

## The cases, ranked by how tractable they actually are

### Case 1 — rigged, but to a different skeleton (**the one worth solving**)

Character Creator, Mixamo, Ready Player Me, and most AI generators emit a mesh
that is *already skinned*: it has joints, weights, and a bind pose. What it does
not have is Bento's joint names or Bento's rest pose. CC bodies name their joints
`CC_Base_*` and are refused today.

**The weights already exist and are usually good.** The problem is
correspondence, not painting. That makes this dramatically more tractable than
the general case, and it covers most of the creators we are trying to help.

What it needs:

1. **Joint correspondence.** Map source joints to Bento joints. Partly a name
   table (extending the alias mechanism), partly structural — a chain of three
   joints descending from a shoulder is an arm whatever it is called.
2. **Bind-pose reconciliation.** The source skeleton does not rest where Bento
   rests. This is the part that must *not* be solved by moving the mesh: SL
   carries per-body proportions in the joint position overrides
   (`mAlternateBindMatrix`, `pelvis_offset`), which is exactly the mechanism for
   a body that is not Linden-shaped. A retarget should **write** those, not
   flatten them.

   **A part cannot be reconciled on its own** (learned 2026-08-10). An override
   is a joint's offset *from its parent*, and the parent is placed by whichever
   worn mesh binds it — the body — not by the mesh being converted and not by
   Linden. So the teeth of a body whose jaw sits 31 mm below Linden's must be
   measured against *that* jaw: measured against Linden's they hang 31 mm through
   the chin, and a tongue hanging off the teeth in turn compounds it. A
   conversion therefore needs the whole source skeleton in front of it, even for
   a mesh that binds two joints of it, and the answer to "where does this rig put
   joint J" must be one answer used everywhere J appears. Nothing about this is
   visible in a single asset: each mesh is self-consistent and only the worn set
   is wrong, so the check has to measure the set.
3. **Extra joints folded away, and the per-mesh budget respected.** Twist bones,
   helper bones and IK targets have no Bento equivalent; their weight merges into
   the nearest mapped ancestor. This is not only tidiness — it is a hard
   constraint, and the measurement below says correspondence alone will not get a
   real body in.
4. **Refusal when correspondence fails.** The rig check already decides this, and
   it should gate the output rather than the input.

**Measured, 2026-08-08, and it contradicts the obvious guess.** A MakeHuman
export and the Linden reference body, counted three independent ways (the region
converter, and two scripts written separately here and by the client team):

```
makehuman-female.glb   1 mesh    declares 163 joints   moves 124
SLReference.glb        8 meshes  declares 133 joints   moves 21
                                 per mesh: 1, 1, 1, 2, 2, 7, 6, 12
```

`maxJointsPerMesh` is **110**, and it is per *mesh*, not per file. MakeHuman is a
single mesh moving 124, so it is over — and the reference body is not the typical
case. A rig authored outside the Bento world is likely to be far denser, so
**joint reduction is a required stage of retargeting rather than an optimisation.**

Two consequences worth having before anyone starts:

- **The name check masks it.** Names are validated before joint counts, so this
  body is refused today for binding a joint called `root` and never reaches the
  count. Fix the names and a *second* refusal appears immediately. Somebody doing
  correspondence work will reasonably suspect their retarget introduced it. It
  did not; it was always second in line.
- **Segmentation is a strategy, not just merging.** The budget is per mesh, and
  the reference body meets it by being eight meshes of 1–12 joints rather than
  one mesh of 21. Splitting a body by region is therefore as legitimate a way to
  fit the budget as folding joints away, and it loses no articulation.

### Case 2 — unrigged mesh, humanoid, roughly A- or T-posed

This is where weight transfer earns its place, and where the original sketch's
technical core was sound.

Given the reference body we now hold — verified, 21 used joints, agreeing with
the skeleton to 0.75 mm — weights can be transferred to a new mesh rather than
computed from nothing:

1. **Geodesic, not Euclidean, distance.** Measure along the mesh surface, not
   straight through space. Straight-line distance bleeds weight between surfaces
   that are near in space and far along the body — the underarm binding to the
   ribcage, adjacent fingers binding to each other. This is the single decision
   that separates a usable result from a torn one.
2. **Heat diffusion.** Treat the reference body's known weights as fixed boundary
   conditions and diffuse them across the target's vertices by solving a sparse
   Laplace system. Established technique (Baran and Popović, *Automatic Rigging
   and Animation of 3D Characters*, 2007; Maya ships a voxel variant).
3. **Clamp and renormalise.** At most four influences per vertex, summing to one
   — matching `max_rig_influences`, which the region publishes and enforces.

The reference proxy is the prerequisite, and acquiring it was the hard part. We
have it.

### Case 3 — arbitrary unrigged mesh, no reliable correspondence

**Open problem. Named here so it is not mistaken for scheduled work.**

Placing joints inside a mesh that is not posed as expected, or is not clearly
humanoid, is unsolved by any cheap heuristic. Centre-of-mass slicing along
principal axes will find a spine and will not find `mWristLeft`. The tools that
do this credibly use learned models, user-placed markers, or both.

If Case 3 is attempted, it should be as *marker-assisted*: the creator places a
handful of points and the pipeline does the rest. That is still not Blender, and
it is honest about needing a human.

## Where it runs, and what it must not do

**Derive; never mutate.** Auto-rigging belongs in the rendition pipeline, not in
the upload path.

A creator's upload is stored canonically and byte-exact
([ADR 0026](adr/0026-vault-authoritative-inventory-assets.md)). A retargeted,
Bento-rigged body is a *derived rendition* of it, exactly as the `sl-mesh` and
`png-texture` renditions are. This is not an aesthetic preference; it buys three
concrete things:

- **Nothing is lost when the algorithm is wrong.** Retargeting is heuristic and
  the first versions will be bad. The creator's file is untouched.
- **Improvements reconvert existing content for free.** The generator tag already
  re-queues every rendition an older converter produced. Improving the retargeter
  is a version bump, not a migration and not a request that creators re-upload —
  this is precisely how the axis-map correction of 2026-08-08 cost nothing.
- **The Homeworldz client can be served the original.** It has no Bento
  constraint; only viewers do. The retarget exists for the viewer path.

Two further constraints:

- **The rig check gates the output.** A retarget produces a candidate; the check
  says whether it maps. `RigOutcome::Unproven` must not be reported as success —
  a body weighted only to positionally-coincident joints has been checked by
  nothing.
- **Cost is a real design input.** Solving a sparse system per upload is heavy,
  and an upload path is reachable by anyone with an account. Whatever runs must
  be queued, bounded, and cancellable — not synchronous with the upload reply.

## What we do not know yet

Recorded honestly, because the original sketch's confident parts were the wrong
parts:

- **Whether Case 1 output is good enough to wear.** Nobody has retargeted a body
  and looked at it in a viewer. Every claim above about tractability is
  reasoning, not measurement — and the one place reasoning was substituted for
  measurement, it was wrong: this document's author predicted a MakeHuman body
  would move "far fewer than 110" joints from a sample of one. It moves 124.
- **How much of correspondence is nameable.** The alias table may cover more of
  CC/Mixamo than expected, or almost none. This is a morning's measurement
  against real files and has not been done.
- **Whether A-pose and T-pose sources can share one path**, or whether the pose
  difference has to be corrected before weight transfer means anything.
- **`max_rig_influences` remains unwitnessed.** No file has yet bound five
  weights to a vertex, so the clamping path has never run on real input.

## Relationship to the plan

M5 already carries "client-side FBX/OBJ/DAE import" and "retarget a Character
Creator rig onto the Bento skeleton". This document is the shape of that second
item, plus the observation that Case 1 is worth more than Case 2, and Case 2 more
than Case 3 — the reverse of how impressive they sound.

Nothing here blocks M4. M4's remaining work is a worn body verified in Firestorm,
attachments, and wearables.

## References

- [ROADMAP.md](ROADMAP.md) — Phase 4, M4 and M5
- [ADR 0033](adr/0033-mesh-assets.md) — mesh assets, coordinates, the acceptance gate
- [ADR 0026](adr/0026-vault-authoritative-inventory-assets.md) — canonical assets are never rewritten
- [rig_check.h](../region/include/homeworldz/rig_check.h) — what "maps to the skeleton" is measured against
- Baran and Popović, *Automatic Rigging and Animation of 3D Characters*, SIGGRAPH 2007
