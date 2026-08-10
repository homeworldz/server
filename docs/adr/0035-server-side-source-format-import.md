# ADR 0035: Server-Side Import of Source Formats

Status: Accepted

A creator uploads the file their tool produced — `.fbx`, `.obj`, `.dae`, or an
archive containing one and its textures — and **the server converts it**. Every
client uploads the same thing and gets the same result, because the conversion
happens in one place rather than once per client.

This reverses the roadmap's earlier M5 position, which put FBX/OBJ/DAE import
**client-side**. That was written when the Homeworldz client was the only client
expected to import anything. It has two faults. It makes import a property of
which client a creator happens to run, so the same file succeeds or fails
depending on the viewer. And it puts the work where it can never reach a
third-party viewer: asking the Firestorm team to implement FBX conversion is a
large ask, while asking them to POST a file to a capability is a small one. A
server-side importer is the version a viewer can adopt.

## Decision

- **Source formats are imported server-side.** The upload capability accepts
  `.fbx`, `.obj`, `.dae` and archives (`.zip` first) in addition to GLB.
- **The upload is canonical, exactly as ADR 0033 requires.** The creator's
  original file — the FBX, or the whole archive — is the canonical blob and is
  never destroyed. glTF becomes a **derived** rendition of it, and the viewer
  renditions derive from that in turn. ADR 0033's rule is unchanged; this
  extends the set of things that rule can point at.
- **Conversion runs in the meshsmith worker**, on the conversion queue that
  already exists (ADR 0033, "Where conversion runs"). Import is unbounded CPU on
  attacker-supplied input and must not sit on any serving path — least of all
  the region's, which serves HTTP on the thread that makes synchronous grid
  calls.
- **An asset is usable when its glTF exists**, exactly as today. Import failure
  is a property of the asset, reported to the creator, not a failed upload that
  loses their file.

## An upload stops being a single file

This is the part that decides the design, and it is not the parser.

Character Creator bodies — the corpus this was written against — ship an FBX
plus a `textures/` tree of 68–80 images referenced by **relative path**. A lone
`.fbx` therefore arrives with no textures at all, producing precisely the
untextured body an import path exists to prevent. The same is true of OBJ (its
`.mtl` and maps) and DAE.

So the unit of upload becomes a **bundle**: one archive carrying a source file
and the assets it references, with relative paths resolved inside it. A bare
source file remains valid and simply imports whatever it embeds.

Accepting archives is accepting an attacker-controlled filesystem, and it is
handled as such:

- Entry paths are resolved and confined to the extraction root. Absolute paths,
  `..` segments, symlinks and links are refused rather than sanitized — a name
  that tried to escape is not a name worth guessing the intent of.
- Uncompressed size, entry count, and per-entry size are bounded before
  extraction, from the directory rather than by decompressing to find out.
- Only referenced files are imported. An archive may carry anything; what is not
  named by the source is not content and does not enter the asset store.
- Every extracted texture is a texture we already know how to validate, and goes
  through the same validation an uploaded texture does. Import is not a side
  door into the asset store.

## What the corpus turned out to be

**Measured 2026-08-10** with `homeworldz-fbx-diag` against ufbx 0.23.0, across
all five Character Creator bodies. The premise of the section above is wrong for
this corpus, and the correction matters enough to record rather than quietly fix.

**Their textures are embedded, not external.** Each body reports 30–34 distinct
texture files and every one carries its bytes inside the FBX — 19–33 MiB of
image data in a 19–41 MiB file. None resolves to a file on disk. A lone `.fbx`
from Character Creator therefore imports *with* its diffuse, normal and opacity
maps, which is the opposite of what this ADR was written expecting.

The `textures/` tree beside each body is real and holds 68–80 files, but the FBX
does not reference it: searching each binary for `textures/`, `RGBAMask`,
`SSSMap`, `_roughness` and `_metallic` finds nothing, and those maps are named by
Reallusion's `.json` sidecar instead. It is the PBR set for their own pipeline,
not the FBX's missing textures, and bundling it changes nothing an FBX reader
would see. Reading it would mean reading a vendor's material format, which is a
different piece of work from this one.

**This does not reverse the decision, and it does reorder the work.** Archives
stay in scope and the rules above stand unchanged: OBJ carries its `.mtl` and
maps beside it, DAE does the same, and plenty of FBX exporters do not embed. What
changes is that the archive layer is no longer what stands between this corpus
and a textured rigged body, so it stops being the first thing built.

Two further findings from the same run, both of which the importer has to hold:

- **Every path recorded is absolute, on the exporting machine**, including a
  named user's home directory — and ufbx surfaces it through
  `relative_filename`, whose own documentation warns it "may be absolute". A
  reader that trusts the field name opens a path that is neither ours nor
  relative to anything. Paths are classified by what they are, not by the field
  they arrived in, and an absolute one is never followed.
- **The gate's limits are per asset, and one FBX need not become one asset.**
  Taken whole, every body exceeds four ADR 0033 limits — 17–18 materials against
  8, 30–34 textures against 16, 17–18 faces against 8, 6 influences against 4.
  Taken per mesh, the worst case across all five is 6 materials, 6 faces and 13
  textures, inside every one of those. A Character Creator body is already
  authored as body, eyes, teeth, tongue, tearline and eye occlusion, which is a
  split along the lines the gate cares about. So import emits **one asset per
  mesh**. The single limit a split does not fix is influences, which is a
  property of the weights and is pruned to four and renormalized on import.

**What the import produces, measured the same day.** `CC3_Base_Plus` becomes six
GLBs carrying all 30 of its textures, 132 influence lists pruned to four, and
five texture bindings glTF has no place for. Posed through its own joints, the
body measures **0.31 x 1.54 x 1.79 m** — depth, arm span, height — with the eyes
9.6 cm apart and the teeth 6 cm across. That is the check worth running: FBX
writes centimetres and a Y-up to Z-up map has two ways to be wrong, and a body a
hundred times too large or lying on its side parses exactly as cleanly as a
correct one.

Note the bind-space geometry measures 1.79 x 1.54 x 0.31 — the same body on its
side. That is not a fault. glTF ignores a skinned node's own transform and poses
the vertices through the joints, so bind-space bounds are the wrong thing to
look at for a rigged asset, and reading them as the model's size is how an
importer gets declared wrong while being right.

Every one of the six is refused by the upload gate, naming the joint: `a skin
binds joint "CC_Base_Pelvis", which is not a joint of the bento-avatar
skeleton`. That is this ADR's stated position holding, not a defect — the
geometry, materials and textures survive the trip, and wearing the result waits
on retargeting.

## Where import actually runs

**Amended 2026-08-10.** The decision above says conversion runs in the meshsmith
worker. It runs in the **region's publish worker** instead — a thread the region
gained for a different reason, and one that satisfies this ADR's stated
objection.

The reason given was that "import is unbounded CPU on attacker-supplied input
and must not sit on any serving path — least of all the region's, which serves
HTTP on the thread that makes synchronous grid calls." That was accurate and
understated. The region runs *one* loop, servicing HTTP, the viewer's UDP socket
and the simulation together, and the mesh upload handler was already the worst
thing on it: `publish_glb` makes 7 + 3T blocking grid round trips for T
textures, fifty-five at the gate's limit, so an upload could stop physics for as
long as the grid took. That was true before this ADR and had nothing to do with
import.

Fixing it gave the region a worker thread that is not a serving path. With that
in place the objection is met, and the reason to prefer meshsmith goes with it.

What decided it against meshsmith is the shape of a rendition. **A rendition is
one blob per (asset, kind)**, and an import produces N — one asset per mesh, for
the reasons in the section above. meshsmith could therefore only return a single
combined glTF, which the region would have to split back apart, costing a
glTF-to-glTF splitter that re-derives what `gltf_from_fbx` already does and a
second parse of the same geometry, for no result a creator could tell apart.

ADR 0028 is not weakened by this. Its concern is the region's *authority*, not
its CPU, and nothing here grants any: the region already validates uploads,
stores canonical blobs and creates inventory on the creator's own credential.
Import adds no power it did not have.

The upload's own contract follows from the same split:

- A source upload is answered **202** with its asset id once the file is stored.
  201 would name an inventory item, and there is not one yet.
- The parts arrive afterwards as items in inventory. Import failure is reported
  against the stored asset, exactly as this ADR requires — the source is kept
  either way.

## The parser

**ufbx** (MIT, single-file C) for FBX. It builds with the region's stack, adds no
package, carries no redistribution terms, and reads the binary FBX versions the
tools in question emit.

Not the **Autodesk FBX SDK**: proprietary, with redistribution terms that reach
into a server we ship. Not **assimp**: a large dependency and a large attack
surface for one format, when the formats after FBX are OBJ and DAE, which are
text and small.

## What this does not solve

Import produces geometry, materials and textures. It does not make an imported
body wearable: Character Creator rigs name their joints `CC_Base_*`, and
`rig_check` refuses them today by design, since a body that maps by name but not
by position is refused rather than shipped broken. Retargeting is a separate
piece of work with its own design ([AUTO-RIGGING.md](../AUTO-RIGGING.md)) and
does not gate this one — an imported static mesh is useful immediately, and an
imported body becomes wearable when retargeting lands.

## Consequences

- A viewer gains import by uploading a file, which is the smallest ask that
  could be made of a third-party viewer team, and the reason for this ADR.
- The asset store gains bundles as a stored form. Their durability follows
  ADR 0026 unchanged: the canonical blob is the archive, and completeness is
  transitive over what it produced.
- Import failures become a reportable asset state rather than an upload error,
  so a creator who uploads a file we cannot yet read keeps the file.
- The roadmap's M5 line moves from "client-side FBX/OBJ/DAE import" to
  server-side, and the documented Daz export path becomes one input among
  several rather than the way in.

## References

- [ADR 0033](0033-mesh-pipeline-gltf-canonical.md) — canonical glTF, derived
  renditions, and the conversion worker this runs in.
- [ADR 0026](0026-vault-authoritative-inventory-assets.md) — canonical blobs and transitive
  completeness.
- [ADR 0028](0028-untrusted-region-trust-model.md) — why conversion is grid-side.
- [AUTO-RIGGING.md](../AUTO-RIGGING.md) — the retargeting this deliberately
  leaves alone.
