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
