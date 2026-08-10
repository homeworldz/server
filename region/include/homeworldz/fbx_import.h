// FBX → glTF import (ADR 0035): the derivation that turns a creator's source
// file into the canonical-adjacent glTF the rest of the pipeline already knows.
//
// This is the same shape as `gltf_from_sl_mesh` in mesh_convert.h and sits one
// step earlier in the chain. ADR 0033 keeps the upload canonical and never
// rewrites it, so an imported FBX stays the stored blob and the GLB produced
// here is a derived rendition of it, exactly as the type-49 asset is a derived
// rendition of a GLB.
//
// **One asset per FBX mesh**, which is the finding that decided the shape rather
// than a preference. The ADR 0033 gate's limits are per asset — 8 materials, 8
// faces, 16 textures — and every Character Creator body exceeds all three taken
// whole while the worst single mesh inside it (6 materials, 6 faces, 13
// textures) fits comfortably. A body is already authored as body, eyes, teeth,
// tongue, tearline and eye occlusion; splitting there is following the author,
// not cutting the model up to fit.
//
// What this deliberately does not do:
//
//   - **Retarget.** Joint names are carried through exactly as the FBX writes
//     them. A `CC_Base_*` rig therefore produces a valid glTF that the upload
//     gate and `convert_glb` both refuse by name, which is ADR 0035's stated
//     position: an imported static mesh is useful immediately, and wearing one
//     waits for AUTO-RIGGING.md. Refusing here as well would only move the
//     same refusal earlier and lose the geometry on the way.
//   - **Read animation.** ufbx is built here with evaluation and baking
//     compiled out; import takes the bind pose.
//   - **Follow a path out of the file.** External references are never opened.
//     Only content the FBX embeds becomes a texture; see `textures_referenced`
//     for what was named and not obtained.
#ifndef HOMEWORLDZ_FBX_IMPORT_H
#define HOMEWORLDZ_FBX_IMPORT_H

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace homeworldz::mesh {

// One FBX mesh, as the GLB it became.
struct ImportedMesh {
    // The FBX's own name for it, which becomes the creator's name for the
    // asset. Never blank: an unnamed mesh is numbered.
    std::string name;
    std::vector<std::byte> glb;
    std::size_t primitives{};
    std::size_t vertices{};
    std::size_t triangles{};
    std::size_t textures{};
    bool skinned{};
};

struct FbxImport {
    bool ok{};
    // Actionable when failed: this reason reaches the creator, who may be
    // several tools away from the file that produced it.
    std::string error;

    std::vector<ImportedMesh> meshes;

    // Joint names exactly as the FBX writes them, deduplicated and sorted.
    // Reported rather than translated — the caller decides what a name it does
    // not recognise means, and retargeting is the piece that will.
    std::vector<std::string> joints;

    // What the source said about itself, for the record an import failure or a
    // surprising result is read against.
    std::string creator;
    unsigned version{};
    // Metres per source unit before normalization. FBX writes centimetres far
    // more often than not, and a body imported a hundred times too large looks
    // like a modelling mistake rather than a unit.
    double source_unit_meters{};

    // Textures the file named, and the ones whose bytes it actually carried.
    // These differ when a reference points outside the file, which is a
    // reference this importer will not follow.
    std::size_t textures_referenced{};
    std::size_t textures_embedded{};

    // Vertices whose influence list was longer than the skeleton budget and was
    // cut to the heaviest four and renormalized. Counted rather than assumed:
    // every body in the corpus carries six, and silently dropping two is the
    // kind of change that shows up later as a limb that deforms not quite
    // right.
    std::size_t influences_pruned{};

    // Materials whose separate opacity map was composited into the alpha
    // channel of their base colour, which is the only way glTF carries opacity.
    // These are the surfaces that would otherwise render as opaque slabs:
    // eyelashes, tearlines, eye occlusion.
    std::size_t opacity_composited{};

    // Texture bindings the FBX declared that glTF has no place for — metalness
    // and roughness maps above all, which the gate's material model does not
    // carry. Counted and reported rather than dropped in silence, so a material
    // that looks wrong in-world can be told apart from one that arrived wrong.
    std::size_t bindings_dropped{};
};

// Import an FBX. Assumes nothing about the bytes: this runs in the conversion
// worker on attacker-supplied input (ADR 0035, "Conversion runs in the meshsmith
// worker"), never on a serving path.
FbxImport gltf_from_fbx(std::span<const std::byte> fbx);

} // namespace homeworldz::mesh

#endif
