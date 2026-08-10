// GLB → SL-mesh conversion (ADR 0033 M1): the derivation the meshsmith
// worker runs. The canonical GLB is never rewritten; this produces the
// type-49 rendition viewers fetch.
#ifndef HOMEWORLDZ_MESH_CONVERT_H
#define HOMEWORLDZ_MESH_CONVERT_H

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace homeworldz::mesh {

struct Conversion {
    bool ok{};
    // Actionable when failed: the reason lands in the job record an operator
    // reads, and may be shown to the creator on a re-request.
    std::string error;
    std::vector<std::byte> sl_mesh;
    std::size_t faces{};
    std::size_t high_triangles{};
    std::size_t lowest_triangles{};
};

// convert_glb builds the sl-mesh rendition: one submesh per material face
// (world transforms applied, primitives sharing a material merged), a LOD
// chain generated with meshoptimizer where the source carries only one level,
// and a convex physics block from the geometry's bounding box — the
// conservative shape, until V-HACD decomposition lands (tracked in the ADR
// 0033 milestone). Assumes the acceptance gate already passed; failures here
// are conversion facts (an over-65,535-vertex face, an unreadable accessor),
// not policy.
Conversion convert_glb(std::span<const std::byte> glb);

// The world-space box the GLB's declared accessor bounds cover, under every
// node transform reachable from the scene — computed from accessor min/max
// corners without loading buffers, so the upload gate can afford it. This is
// the ONE bounds definition: the upload sets the wrapper prim's scale from
// it, and the converter normalizes geometry by it, so the prim renders at
// authored size by construction (viewers scale mesh geometry by prim scale
// over a unit domain).
struct WorldBounds {
    bool ok{};
    std::array<float, 3> center{};
    std::array<float, 3> extent{};
};
WorldBounds declared_world_bounds(std::span<const std::byte> glb);

// gltf_from_sl_mesh derives the `gltf` rendition from a stored type-49 asset:
// ADR 0033 M2's remaining half, which makes viewer-authored meshes readable by
// clients on the modern path — they never learn the legacy serialization, so
// without this every Firestorm-uploaded object is a placeholder to them.
//
// The high LOD becomes one glTF primitive per submesh (a material face).
// Geometry is emitted in the asset's own coordinates, which for a viewer-
// authored mesh is the normalized unit domain — so a renderer applies the
// object's scale over it, per ADR 0033 "Scale".
//
// COORDINATES: emitted as conformant glTF, +Y up. Both directions of this
// pipeline apply the axis map (region +Z up), so an uploaded GLB stands
// upright in-world and a derived glTF opens upright in any tool that reads
// it. Until 2026-07-30 neither direction rotated, which was consistent and
// wrong: it made every conformant export land on its side.
struct GltfConversion {
    bool ok{};
    std::string error;
    std::vector<std::byte> glb;
    std::size_t primitives{};
    std::size_t vertices{};
    std::size_t triangles{};
};
GltfConversion gltf_from_sl_mesh(std::span<const std::byte> asset);

// Textures a GLB carries, for the viewer pipeline of ADR 0033 M3. A viewer
// cannot read a GLB's embedded PNG or JPEG, so each becomes its own texture
// asset: the extracted bytes stay canonical (a format the modern client reads)
// and a `j2c-texture` rendition is derived for viewers - the same
// canonical/derived split the mesh itself uses, pointed at images.
struct SourceTexture {
    // "image/png" or "image/jpeg", as the GLB declares it.
    std::string mime;
    // The embedded bytes, verbatim: the creator's image, never re-encoded here.
    std::vector<std::byte> bytes;
};

struct TextureExtraction {
    bool ok{};
    std::string error;
    std::vector<SourceTexture> textures;
    // Per face, in the same order convert_glb emits faces, the index into
    // textures of that material's base-colour map, or -1 for an untextured
    // face. Both orders come from one shared traversal, so a face index means
    // the same thing to the converter and to the TextureEntry built from this.
    std::vector<int> face_textures;
};

// Extract the base-colour images a GLB references. Cheap enough for the upload
// path: it reads the container and copies bytes, decoding nothing.
TextureExtraction extract_textures(std::span<const std::byte> glb);

// The generator tag stored with renditions this converter produces, bumped
// when output changes so regeneration can find what it supersedes.
// 0.7: derived JPEG2000 is compressed at 20:1 rather than lossless, so every
// j2c-texture a previous generator produced is superseded and reconverts.
// Bumped to 0.8 for the material a derived glTF now states rather than leaves
// to the specification's default. Rendition regeneration re-queues whatever a
// predecessor produced, so raising this is what reissues the earlier renditions
// that drew as metal.
// Bumped to 0.9 for the corrected axis map. The old map stood a model upright
// and left its lateral axis where it found it, a 90 degree yaw; every rendition
// a predecessor produced is a model facing the wrong way and reconverts. This is
// what makes the correction free: canonical assets are stored as uploaded and
// never rewritten, so nothing a creator sent is lost or has to be re-sent.
// 0.10 sets bind_shape_matrix, which was left identity: a worn rigged mesh is
// not scaled by its prim, so nothing carried the unit-domain normalization back
// out and the viewer skinned half-unit geometry with joints metres apart.
// 0.11 emits axis-aligned inverse bind matrices. avatar_skeleton.xml gives
// every joint rot="0 0 0", so an exporter's bone orientations describe a
// skeleton the viewer does not have; carried through, they turned the reference
// body's arm chain a quarter turn while every joint still measured correctly.
// 0.12 retargets foreign rigs onto Bento and writes joint position overrides
// (AUTO-RIGGING.md Case 1). Two reasons every earlier rendition is superseded:
// a skin that named CC_Base_* was refused outright and produced nothing, and a
// skin that did map now carries an alt_inverse_bind table it did not before, so
// a body keeps its own proportions rather than taking Linden's. Bumping this is
// what re-queues them — nothing in the grid sweeps on its own, and the worker's
// --regenerate startup pass is the only caller of the endpoint that does.
inline constexpr const char* generator = "meshsmith/0.12";

} // namespace homeworldz::mesh

#endif
