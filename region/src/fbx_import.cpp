// FBX → glTF (ADR 0035). One GLB per FBX mesh, written by hand for the reason
// gltf_from_slmesh.cpp gives: cgltf reads glTF and does not write it.

#include "homeworldz/fbx_import.h"

#include "homeworldz/image.h"
#include "homeworldz/mesh_acceptance.h"
#include "homeworldz/mesh_convert.h"

#include "fbx_load.h"
#include "glb_write.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace homeworldz::mesh {
namespace {

using glb::append_float;
using glb::append_u16;
using glb::number;
using glb::pad_to_four;

FbxImport fail(std::string reason) {
    FbxImport result;
    result.error = std::move(reason);
    return result;
}

std::string_view text(const ufbx_string& value) { return {value.data, value.length}; }

// JSON string escaping for the one place untrusted text reaches the document:
// a mesh or material name the creator chose. Control characters are dropped
// rather than encoded — they have no business in a name and a `\u` escape would
// only carry them further.
std::string json_string(std::string_view value) {
    std::string out = "\"";
    for (const char character : value) {
        if (character == '"' || character == '\\') out.push_back('\\');
        if (static_cast<unsigned char>(character) < 0x20) continue;
        out.push_back(character);
    }
    out.push_back('"');
    return out;
}

// PNG and JPEG identified by their own first bytes, not by the extension on a
// path we would not follow anyway. The gate accepts these two and nothing else
// (mesh_convert.h, extract_textures), so a third format is refused here rather
// than written into a GLB that would be refused later for a vaguer reason.
const char* sniff_image(const ufbx_blob& content) {
    const auto* bytes = static_cast<const unsigned char*>(content.data);
    if (content.size >= 8 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' &&
        bytes[3] == 'G' && bytes[4] == 0x0d && bytes[5] == 0x0a && bytes[6] == 0x1a &&
        bytes[7] == 0x0a)
        return "image/png";
    if (content.size >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff)
        return "image/jpeg";
    return nullptr;
}

std::vector<std::uint8_t> to_u8(const ufbx_blob& blob) {
    const auto* bytes = static_cast<const std::uint8_t*>(blob.data);
    return std::vector<std::uint8_t>(bytes, bytes + blob.size);
}

// Character Creator writes opacity as its own image; glTF carries opacity only
// as the alpha channel of the base colour. Carrying it across therefore means
// compositing the two, and this is the one place an import re-encodes a
// creator's image. That is sound under ADR 0033: the canonical blob is the FBX
// the creator sent and is never rewritten, and this GLB is a derived rendition
// of it, so the re-encode changes nothing anyone can lose.
//
// **White is opaque.** Measured, not assumed, and the two names disagree: FBX
// files this texture under `TransparentColor`, whose own semantics are the
// opposite, while Reallusion names the file `_Opacity`. The eyelash map settles
// it — white lashes on a black card, and the lashes are the part you can see.
// Guessing wrong here inverts every masked surface, which renders as a solid
// black rectangle where a lash should be and looks like a geometry fault.
std::optional<std::vector<std::byte>> encoded_png(const image::Image& rgba) {
    const auto encoded = image::encode_png(rgba);
    if (!encoded) return std::nullopt;
    std::vector<std::byte> out(encoded->size());
    std::memcpy(out.data(), encoded->data(), encoded->size());
    return out;
}

// What makes a mask a cutout, in two parts: it has solid pixels *and* it is
// mostly empty.
//
// The second half was missing and it cost a garment. A cutout is a shape cut out
// of nothing — a hair atlas is three quarters transparent with solid strands in
// it — and alpha-testing one is right because there is barely anything partial
// to lose. A leather vest with a feathered shoulder is the opposite: 87% solid,
// 8% clear, and the soft part is the whole of the one feature that needs to stay
// soft. Judging it on "has solid pixels" alone called it a cutout, because the
// statistic is dominated by the leather and cannot see the fringe, and the
// cutoff then chopped the feathers into flat black shards (operator screenshots,
// 2026-08-17, against the same vest imported before the change and correct).
//
// Measured, both thresholds sitting in a gap rather than at a preference:
//
//   hair atlas   11% solid, 75% clear  -> cutout
//   this vest    87% solid,  8% clear  -> not
//
// So the emptiness test is what distinguishes them, and the solidity test still
// keeps soft overlays — eye occlusion, tearlines, a soft lash map — off it.
constexpr std::uint8_t solid_alpha = 230;   // 0.9
constexpr std::uint8_t clear_alpha = 26;    // 0.1
constexpr double cutout_share = 0.02;
constexpr double empty_share = 0.30;

// How a composited mask should be drawn. Three cases, because two were not
// enough and the corpus says why.
//
// BLEND cannot be the answer for anything with solid area in it. Alpha-blended
// geometry does not write depth, so it cannot occlude — a leather vest rendered
// that way shows its own back panel through its front however opaque the texture
// says the leather is (operator, 2026-08-17). That is not a texture fault and no
// alpha value fixes it.
//
// But BLEND is still right for a mask with *no* solid area: an eye-occlusion
// shell or a tearline is a soft shadow with nothing to occlude and nothing to
// test against, and a cutoff would give it a hard edge it should not have.
//
// Between those, the cutoff differs by what the mask is. A hair atlas is mostly
// empty, its strands are already near-binary, and 0.4 is what made it read as
// hair. A mostly-solid mask with a soft feature in it — the vest's fur fringe —
// needs the low cutoff instead: the level barely changes how much survives
// (92.4% at 0.05 against 91.1% at 0.4), but taking the soft tips away is what
// chopped the fur into shards, so keep them and let them be opaque.
enum class AlphaDraw { Blend, MaskCutout, MaskSolid };

AlphaDraw alpha_draw(const image::Image& rgba) {
    const auto solid = image::opaque_fraction(rgba, solid_alpha);
    const auto clear = 1.0 - image::opaque_fraction(rgba, clear_alpha);
    if (solid <= cutout_share) return AlphaDraw::Blend;
    return clear > empty_share ? AlphaDraw::MaskCutout : AlphaDraw::MaskSolid;
}

std::optional<std::vector<std::byte>> composite_alpha(const ufbx_blob& colour,
                                                      const ufbx_blob& opacity,
                                                      AlphaDraw* draw) {
    const auto base = image::decode_png_or_jpeg(to_u8(colour));
    const auto mask = image::decode_png_or_jpeg(to_u8(opacity));
    if (!base || !mask || base->empty() || mask->empty()) return std::nullopt;

    auto rgba = image::to_rgba(*base);
    if (!image::write_alpha_from_luminance(rgba, *mask)) return std::nullopt;
    if (draw != nullptr) *draw = alpha_draw(rgba);
    return encoded_png(rgba);
}

// The same, for a material that has an opacity map and *no* colour map: the
// colour is then a single value on the material, and the surface is that value
// everywhere with the mask deciding where it shows.
//
// Character Creator's eye occlusion is exactly this — a shell over the eyeball
// that darkens the socket, with its shape entirely in the mask — and so is a
// tear line on some exports. Treating such a material as textureless dropped
// the mask with the absent colour, published the part with no texture at all,
// and left the face on the blank fallback: two opaque white shells over the
// eyes, which is what it looked like in-world (2026-08-11).
//
// The factor is written straight into an sRGB-tagged image without conversion.
// For the materials this exists to serve the point is moot — Reallusion sets
// them black or white, where every colour space agrees — and inventing a
// transfer function for the mid-greys would be a guess about the exporter that
// nothing here can check.
std::optional<std::vector<std::byte>> composite_factor_alpha(const ufbx_vec4& colour,
                                                             const ufbx_blob& opacity,
                                                             AlphaDraw* draw) {
    const auto mask = image::decode_png_or_jpeg(to_u8(opacity));
    if (!mask || mask->empty()) return std::nullopt;

    const auto channel = [](ufbx_real value) {
        const auto scaled = value * 255.0;
        if (!(scaled > 0.0)) return static_cast<std::uint8_t>(0);
        if (scaled > 255.0) return static_cast<std::uint8_t>(255);
        return static_cast<std::uint8_t>(scaled + 0.5);
    };
    const auto rgba = image::solid_with_alpha(
        {channel(colour.x), channel(colour.y), channel(colour.z)}, *mask);
    if (rgba.empty()) return std::nullopt;
    if (draw != nullptr) *draw = alpha_draw(rgba);
    return encoded_png(rgba);
}

// ufbx stores a 3x4 column-major affine; glTF wants a 4x4 column-major one.
std::array<float, 16> to_gltf_matrix(const ufbx_matrix& matrix) {
    std::array<float, 16> out{};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 3; ++row)
            out[column * 4 + row] = static_cast<float>(matrix.cols[column].v[row]);
        out[column * 4 + 3] = column == 3 ? 1.0f : 0.0f;
    }
    return out;
}

// At most four influences per vertex, heaviest first, renormalized.
//
// ufbx sorts a vertex's weights by decreasing weight, so the first four *are*
// the heaviest four and this is a truncation rather than a choice. The
// renormalization is the part that matters: dropping two influences without it
// leaves the vertex weighted to less than one and the skin shrinks toward the
// origin wherever it happened.
struct Influence {
    std::uint16_t joint{};
    float weight{};
};

std::array<Influence, 4> prune(const ufbx_skin_deformer* skin, std::uint32_t vertex,
                               bool& was_pruned) {
    std::array<Influence, 4> out{};
    if (skin == nullptr || vertex >= skin->vertices.count) return out;
    const auto& entry = skin->vertices.data[vertex];
    const auto taken = (std::min<std::size_t>)(entry.num_weights, 4);
    if (entry.num_weights > 4) was_pruned = true;
    float total = 0.0f;
    for (std::size_t at = 0; at < taken; ++at) {
        const auto& weight = skin->weights.data[entry.weight_begin + at];
        out[at].joint = static_cast<std::uint16_t>(weight.cluster_index);
        out[at].weight = static_cast<float>(weight.weight);
        total += out[at].weight;
    }
    if (total > 0.0f)
        for (auto& influence : out) influence.weight /= total;
    return out;
}

// The vertex streams a primitive is built from, one entry per triangle corner
// before deduplication. Kept as parallel arrays because that is the shape
// ufbx_generate_indices takes: it compacts each stream in place and hands back
// the index buffer.
struct Streams {
    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 3>> normals;
    std::vector<std::array<float, 2>> texcoords;
    std::vector<std::array<std::uint16_t, 4>> joints;
    std::vector<std::array<float, 4>> weights;
};

} // namespace

bool looks_like_fbx(std::span<const std::byte> content) {
    // The binary magic Autodesk has written since FBX 6, and the comment line
    // an ASCII export opens with. Both are what ufbx itself detects on; this is
    // the same question asked earlier and more cheaply.
    constexpr std::string_view binary_magic = "Kaydara FBX Binary";
    constexpr std::string_view ascii_magic = "; FBX";
    const auto begins_with = [&](std::string_view prefix) {
        if (content.size() < prefix.size()) return false;
        return std::memcmp(content.data(), prefix.data(), prefix.size()) == 0;
    };
    return begins_with(binary_magic) || begins_with(ascii_magic);
}

FbxImport gltf_from_fbx(std::span<const std::byte> fbx) {
    auto options = fbx_load_options();
    ufbx_error error{};
    ufbx_scene* scene =
        ufbx_load_memory(fbx.data(), fbx.size(), &options, &error);
    if (scene == nullptr) {
        char described[512];
        ufbx_format_error(described, sizeof described, &error);
        return fail(std::string("the FBX does not read: ") + described);
    }
    struct Free {
        ufbx_scene* scene;
        ~Free() { ufbx_free_scene(scene); }
    } freer{scene};

    FbxImport result;
    result.creator = std::string(text(scene->metadata.creator));
    result.version = scene->metadata.version;
    result.source_unit_meters = static_cast<double>(scene->settings.original_unit_meters);
    result.textures_referenced = scene->texture_files.count;
    for (std::size_t at = 0; at < scene->texture_files.count; ++at)
        if (scene->texture_files.data[at].content.size > 0) ++result.textures_embedded;

    std::set<std::string> joint_names;
    for (std::size_t at = 0; at < scene->skin_clusters.count; ++at)
        if (const auto* bone = scene->skin_clusters.data[at]->bone_node)
            joint_names.insert(std::string(text(bone->name)));
    result.joints.assign(joint_names.begin(), joint_names.end());

    if (scene->meshes.count == 0) return fail("the FBX carries no meshes");

    for (std::size_t mesh_index = 0; mesh_index < scene->meshes.count; ++mesh_index) {
        const ufbx_mesh* mesh = scene->meshes.data[mesh_index];
        if (!mesh->vertex_position.exists || mesh->num_faces == 0) continue;
        // A mesh with no node instance is geometry nothing places. Skipping it
        // is not a loss: there is no transform for it and no scene it belongs
        // to, so any position we invented for it would be invented.
        if (mesh->instances.count == 0) continue;
        const ufbx_node* node = mesh->instances.data[0];

        // One skin. FBX permits several deformers on a mesh and glTF binds a
        // primitive to exactly one skin, so a second would have to be merged or
        // picked and both are guesses about what the author meant.
        const ufbx_skin_deformer* skin =
            mesh->skin_deformers.count > 0 ? mesh->skin_deformers.data[0] : nullptr;

        // Geometry is baked to world space, skinned or not, so that bind space
        // *is* world space and the file is conventional glTF.
        //
        // The first version left skinned geometry in its own space and used
        // ufbx's `geometry_to_bone` as the inverse bind. That skins correctly —
        // the two are in the same space and compose to world — but it hides the
        // joint positions somewhere no reader can use them: the bind matrices
        // then describe a body lying on its side in geometry coordinates, and
        // anything asking "where does this joint rest" gets the height in X.
        // The retarget needs exactly that answer to write joint position
        // overrides, and got 1.63 m of head height on the X axis.
        //
        // With positions in world space the inverse bind is simply the inverse
        // of the joint's own world transform, which is what glTF means by one:
        // jointWorld * inverseBind is identity at rest, so the bind pose is the
        // geometry as written.
        const ufbx_matrix to_world = node->geometry_to_world;
        const ufbx_matrix normal_matrix = ufbx_matrix_for_normals(&to_world);
        constexpr bool bake_transform = true;

        ImportedMesh imported;
        imported.name = std::string(text(mesh->name));
        if (imported.name.empty()) imported.name = "mesh " + std::to_string(mesh_index);
        imported.skinned = skin != nullptr;

        std::vector<std::byte> binary;
        std::string views, accessors, primitives;
        std::size_t view_count = 0, accessor_count = 0;

        const auto add_view = [&](std::size_t offset, std::size_t length, int target) {
            if (!views.empty()) views += ',';
            views += "{\"buffer\":0,\"byteOffset\":" + std::to_string(offset) +
                     ",\"byteLength\":" + std::to_string(length);
            if (target != 0) views += ",\"target\":" + std::to_string(target);
            views += "}";
            return view_count++;
        };

        // Images this mesh's own materials use, in first-encounter order, so an
        // asset carries only what it draws with. A map shared by two materials
        // is embedded once.
        // An image bound for the GLB: either the creator's bytes untouched, or
        // a colour map with an opacity map composited into its alpha.
        struct Embedded {
            std::vector<std::byte> bytes;
            const char* mime{};
            // Whether the mask composited into this image is a cutout rather
            // than a soft overlay, which decides MASK against BLEND below.
            AlphaDraw draw{AlphaDraw::Blend};
        };
        // Keyed on the *pair*, because a colour map composited with two
        // different opacity maps is two different images while the same pair
        // reached from two materials is one.
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::size_t> image_slot;
        std::vector<Embedded> image_files;

        // The file a texture carries, or null when it named one it did not
        // carry — a reference this importer will not follow off disk.
        const auto carried = [&](const ufbx_texture* texture) -> const ufbx_texture_file* {
            if (texture == nullptr || !texture->has_file) return nullptr;
            const auto& file = scene->texture_files.data[texture->file_index];
            if (file.content.size == 0) return nullptr;
            if (sniff_image(file.content) == nullptr) return nullptr;
            return &file;
        };

        constexpr std::uint32_t no_opacity = std::numeric_limits<std::uint32_t>::max();
        const auto slot_for = [&](const ufbx_texture* texture,
                                  const ufbx_texture* opacity) -> long long {
            const auto* file = carried(texture);
            if (file == nullptr) return -1;
            const auto* mask = carried(opacity);
            const std::pair<std::uint32_t, std::uint32_t> key{
                texture->file_index, mask != nullptr ? opacity->file_index : no_opacity};
            if (const auto found = image_slot.find(key); found != image_slot.end())
                return static_cast<long long>(found->second);

            Embedded embedded;
            if (mask != nullptr) {
                if (auto merged = composite_alpha(file->content, mask->content,
                                                  &embedded.draw)) {
                    embedded.bytes = std::move(*merged);
                    embedded.mime = "image/png";  // composite_alpha encodes PNG
                } else {
                    // Either image being unreadable is worth reporting rather
                    // than half-applying: the colour still goes in, and the
                    // count says the mask did not.
                    ++result.bindings_dropped;
                }
            }
            if (embedded.bytes.empty()) {
                const auto* bytes = static_cast<const std::byte*>(file->content.data);
                embedded.bytes.assign(bytes, bytes + file->content.size);
                embedded.mime = sniff_image(file->content);
            }
            const auto slot = image_files.size();
            image_slot.emplace(key, slot);
            image_files.push_back(std::move(embedded));
            return static_cast<long long>(slot);
        };

        // A mask with no colour map to carry it: the colour comes from the
        // material instead. Keyed on the pair the image is made of, like
        // slot_for, so two materials sharing a mask and a colour share one
        // image — Reallusion gives the left and right eye occlusion the same
        // mask, and their colours match too.
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::size_t> factor_slot;
        const auto slot_for_factor = [&](const ufbx_vec4& colour,
                                         const ufbx_texture* opacity) -> long long {
            const auto* mask = carried(opacity);
            if (mask == nullptr) return -1;
            const auto packed = static_cast<std::uint32_t>(
                (static_cast<std::uint32_t>((std::clamp)(colour.x, 0.0, 1.0) * 255.0 + 0.5) << 16) |
                (static_cast<std::uint32_t>((std::clamp)(colour.y, 0.0, 1.0) * 255.0 + 0.5) << 8) |
                static_cast<std::uint32_t>((std::clamp)(colour.z, 0.0, 1.0) * 255.0 + 0.5));
            const std::pair<std::uint32_t, std::uint32_t> key{opacity->file_index, packed};
            if (const auto found = factor_slot.find(key); found != factor_slot.end())
                return static_cast<long long>(found->second);
            Embedded embedded;
            auto filled = composite_factor_alpha(colour, mask->content, &embedded.draw);
            if (!filled) {
                // The mask was unreadable, so there is nothing to publish and
                // the count says a binding was lost. Better than a texture that
                // states a colour the creator never asked for.
                ++result.bindings_dropped;
                return -1;
            }
            embedded.bytes = std::move(*filled);
            embedded.mime = "image/png";  // composite_factor_alpha encodes PNG
            const auto slot = image_files.size();
            factor_slot.emplace(key, slot);
            image_files.push_back(std::move(embedded));
            return static_cast<long long>(slot);
        };

        std::string materials;
        std::size_t material_count = 0;

        const auto part_count = (std::max<std::size_t>)(mesh->material_parts.count, 1);
        for (std::size_t part_index = 0; part_index < part_count; ++part_index) {
            const ufbx_mesh_part* part = mesh->material_parts.count > 0
                                             ? &mesh->material_parts.data[part_index]
                                             : nullptr;
            const std::size_t face_total = part != nullptr ? part->num_faces : mesh->num_faces;
            if (face_total == 0) continue;

            // Triangulate every face of this part into corner indices. The
            // capacity ufbx asks for is the whole mesh's worst face, which is
            // the only bound that is always safe.
            std::vector<std::uint32_t> corners;
            std::vector<std::uint32_t> scratch(
                (std::max<std::size_t>)(mesh->max_face_triangles, 1) * 3);
            for (std::size_t at = 0; at < face_total; ++at) {
                const auto face_index =
                    part != nullptr ? part->face_indices.data[at] : static_cast<std::uint32_t>(at);
                const auto face = mesh->faces.data[face_index];
                if (face.num_indices < 3) continue;  // a point or a line is not a surface
                const auto triangles =
                    ufbx_triangulate_face(scratch.data(), scratch.size(), mesh, face);
                corners.insert(corners.end(), scratch.begin(),
                               scratch.begin() + static_cast<std::size_t>(triangles) * 3);
            }
            if (corners.size() < 3) continue;

            const bool has_normals = mesh->vertex_normal.exists;
            const bool has_texcoords = mesh->vertex_uv.exists;

            Streams streams;
            streams.positions.reserve(corners.size());
            for (const auto corner : corners) {
                auto position = ufbx_get_vertex_vec3(&mesh->vertex_position, corner);
                if (bake_transform) position = ufbx_transform_position(&to_world, position);
                streams.positions.push_back({static_cast<float>(position.x),
                                             static_cast<float>(position.y),
                                             static_cast<float>(position.z)});
                if (has_normals) {
                    auto normal = ufbx_get_vertex_vec3(&mesh->vertex_normal, corner);
                    if (bake_transform)
                        normal = ufbx_transform_direction(&normal_matrix, normal);
                    streams.normals.push_back({static_cast<float>(normal.x),
                                               static_cast<float>(normal.y),
                                               static_cast<float>(normal.z)});
                }
                if (has_texcoords) {
                    const auto uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, corner);
                    // Taken as ufbx reports them, with no V flip.
                    //
                    // This flipped V until 2026-08-10, on the stated grounds
                    // that FBX puts the UV origin at the bottom left and glTF at
                    // the top left. That was assumed rather than measured, and
                    // it is wrong here: an imported body wore its skin upside
                    // down, nipples at the hips and a face across the abdomen.
                    //
                    // What settles it is the other path. An uploaded GLB reaches
                    // a viewer through the same glTF -> sl-mesh conversion,
                    // which flips nothing, and its textures have been right
                    // since M1. So whatever the two specifications say, zero
                    // flips is what renders correctly downstream, and an import
                    // has to arrive at the same place an upload does. Flipping
                    // here also left the canonical glTF disagreeing with every
                    // other glTF we store.
                    streams.texcoords.push_back(
                        {static_cast<float>(uv.x), static_cast<float>(uv.y)});
                }
                if (skin != nullptr) {
                    const auto vertex = mesh->vertex_indices.data[corner];
                    bool pruned = false;
                    const auto influences = prune(skin, vertex, pruned);
                    if (pruned) ++result.influences_pruned;
                    std::array<std::uint16_t, 4> joints{};
                    std::array<float, 4> weights{};
                    for (int at = 0; at < 4; ++at) {
                        joints[at] = influences[at].joint;
                        weights[at] = influences[at].weight;
                    }
                    streams.joints.push_back(joints);
                    streams.weights.push_back(weights);
                }
            }

            std::vector<ufbx_vertex_stream> vertex_streams;
            const auto add_stream = [&](void* data, std::size_t count, std::size_t size) {
                vertex_streams.push_back({data, count, size});
            };
            add_stream(streams.positions.data(), streams.positions.size(),
                       sizeof(streams.positions[0]));
            if (has_normals)
                add_stream(streams.normals.data(), streams.normals.size(),
                           sizeof(streams.normals[0]));
            if (has_texcoords)
                add_stream(streams.texcoords.data(), streams.texcoords.size(),
                           sizeof(streams.texcoords[0]));
            if (skin != nullptr) {
                add_stream(streams.joints.data(), streams.joints.size(),
                           sizeof(streams.joints[0]));
                add_stream(streams.weights.data(), streams.weights.size(),
                           sizeof(streams.weights[0]));
            }

            std::vector<std::uint32_t> indices(corners.size());
            ufbx_error index_error{};
            const auto unique =
                ufbx_generate_indices(vertex_streams.data(), vertex_streams.size(),
                                      indices.data(), indices.size(), nullptr, &index_error);
            if (unique == 0) {
                char described[512];
                ufbx_format_error(described, sizeof described, &index_error);
                return fail("mesh \"" + imported.name +
                            "\" could not be indexed: " + described);
            }

            // Positions, with the min/max the specification requires on
            // POSITION and which declared_world_bounds later reads.
            pad_to_four(binary);
            const auto position_offset = binary.size();
            std::array<float, 3> low = streams.positions[0];
            std::array<float, 3> high = streams.positions[0];
            for (std::size_t at = 0; at < unique; ++at)
                for (int axis = 0; axis < 3; ++axis) {
                    const float value = streams.positions[at][axis];
                    low[axis] = (std::min)(low[axis], value);
                    high[axis] = (std::max)(high[axis], value);
                    append_float(binary, value);
                }
            const auto position_view =
                add_view(position_offset, binary.size() - position_offset, 34962);
            if (!accessors.empty()) accessors += ',';
            accessors += "{\"bufferView\":" + std::to_string(position_view) +
                ",\"componentType\":5126,\"count\":" + std::to_string(unique) +
                ",\"type\":\"VEC3\",\"min\":[" + number(low[0]) + ',' + number(low[1]) + ',' +
                number(low[2]) + "],\"max\":[" + number(high[0]) + ',' + number(high[1]) + ',' +
                number(high[2]) + "]}";
            std::string attributes = "\"POSITION\":" + std::to_string(accessor_count++);

            if (has_normals) {
                pad_to_four(binary);
                const auto offset = binary.size();
                for (std::size_t at = 0; at < unique; ++at)
                    for (int axis = 0; axis < 3; ++axis) append_float(binary, streams.normals[at][axis]);
                const auto view = add_view(offset, binary.size() - offset, 34962);
                accessors += ",{\"bufferView\":" + std::to_string(view) +
                    ",\"componentType\":5126,\"count\":" + std::to_string(unique) +
                    ",\"type\":\"VEC3\"}";
                attributes += ",\"NORMAL\":" + std::to_string(accessor_count++);
            }
            if (has_texcoords) {
                pad_to_four(binary);
                const auto offset = binary.size();
                for (std::size_t at = 0; at < unique; ++at) {
                    append_float(binary, streams.texcoords[at][0]);
                    append_float(binary, streams.texcoords[at][1]);
                }
                const auto view = add_view(offset, binary.size() - offset, 34962);
                accessors += ",{\"bufferView\":" + std::to_string(view) +
                    ",\"componentType\":5126,\"count\":" + std::to_string(unique) +
                    ",\"type\":\"VEC2\"}";
                attributes += ",\"TEXCOORD_0\":" + std::to_string(accessor_count++);
            }
            if (skin != nullptr) {
                pad_to_four(binary);
                const auto joint_offset = binary.size();
                for (std::size_t at = 0; at < unique; ++at)
                    for (int slot = 0; slot < 4; ++slot) append_u16(binary, streams.joints[at][slot]);
                const auto joint_view = add_view(joint_offset, binary.size() - joint_offset, 34962);
                accessors += ",{\"bufferView\":" + std::to_string(joint_view) +
                    ",\"componentType\":5123,\"count\":" + std::to_string(unique) +
                    ",\"type\":\"VEC4\"}";
                attributes += ",\"JOINTS_0\":" + std::to_string(accessor_count++);

                pad_to_four(binary);
                const auto weight_offset = binary.size();
                for (std::size_t at = 0; at < unique; ++at)
                    for (int slot = 0; slot < 4; ++slot) append_float(binary, streams.weights[at][slot]);
                const auto weight_view =
                    add_view(weight_offset, binary.size() - weight_offset, 34962);
                accessors += ",{\"bufferView\":" + std::to_string(weight_view) +
                    ",\"componentType\":5126,\"count\":" + std::to_string(unique) +
                    ",\"type\":\"VEC4\"}";
                attributes += ",\"WEIGHTS_0\":" + std::to_string(accessor_count++);
            }

            pad_to_four(binary);
            const auto index_offset = binary.size();
            const bool wide = unique > (std::numeric_limits<std::uint16_t>::max)();
            for (const auto index : indices)
                if (wide) glb::append_u32(binary, index);
                else append_u16(binary, static_cast<std::uint16_t>(index));
            const auto index_view = add_view(index_offset, binary.size() - index_offset, 34963);
            accessors += ",{\"bufferView\":" + std::to_string(index_view) +
                ",\"componentType\":" + (wide ? "5125" : "5123") +
                ",\"count\":" + std::to_string(indices.size()) + ",\"type\":\"SCALAR\"}";
            const auto index_accessor = accessor_count++;

            // The material for this part, with the maps glTF has a place for.
            const ufbx_material* material =
                part != nullptr && part_index < mesh->materials.count
                    ? mesh->materials.data[part_index]
                    : nullptr;
            long long base_colour = -1, normal_map = -1;
            bool masked = false;
            // A surface made transparent by the material's own opacity value
            // rather than by a mask. It needs the same BLEND that a mask gets.
            bool transparent_factor = false;
            std::string name = "part " + std::to_string(part_index);
            if (material != nullptr) {
                name = std::string(text(material->name));
                // ufbx's own PBR maps rather than raw FBX property names.
                // It normalizes across shading models — these materials are
                // `phong`, where the colour arrives as `DiffuseColor` — so
                // reading the FBX names directly would be a second, worse copy
                // of a table ufbx already maintains.
                //
                // Opacity is the exception, and deliberately so. For the FBX
                // shaders ufbx maps `TransparentColor` to `transmission_color`,
                // not to `opacity`, because that is what the slot means in the
                // format. Reallusion puts an opacity map there anyway. Both
                // are consulted, opacity first, so a file that means what it
                // says still works.
                const ufbx_texture* opacity = material->pbr.opacity.texture;
                if (opacity == nullptr) opacity = material->pbr.transmission_color.texture;

                base_colour = slot_for(material->pbr.base_color.texture, opacity);
                // No colour map, but a mask: the material's own colour is the
                // surface, and the mask is the whole of its shape. Without this
                // the mask went out with the missing colour and the part
                // published untextured, which the viewer draws as opaque white.
                if (base_colour < 0)
                    base_colour = slot_for_factor(material->pbr.base_color.value_vec4, opacity);
                normal_map = slot_for(material->pbr.normal_map.texture, nullptr);
                masked = opacity != nullptr && carried(opacity) != nullptr && base_colour >= 0;

                // Anything else the material bound and glTF has no place for —
                // metalness and roughness maps above all, which the gate's
                // material model does not carry. Counted rather than dropped in
                // silence.
                for (std::size_t at = 0; at < material->textures.count; ++at) {
                    const auto* bound = material->textures.data[at].texture;
                    if (bound == nullptr) continue;
                    if (bound == material->pbr.base_color.texture ||
                        bound == material->pbr.normal_map.texture || bound == opacity)
                        continue;
                    ++result.bindings_dropped;
                }
            }
            if (!materials.empty()) materials += ',';
            materials += "{\"name\":" + json_string(name) +
                ",\"pbrMetallicRoughness\":{\"metallicFactor\":0,\"roughnessFactor\":1";
            if (base_colour >= 0) {
                materials += ",\"baseColorTexture\":{\"index\":" + std::to_string(base_colour) + "}";
            } else if (material != nullptr) {
                // No map of any kind, so the material's own colour is the whole
                // surface. Without this the material said nothing at all and
                // glTF's default — opaque white — stood in for it, which is how
                // a tinted or semitransparent surface with no texture came out
                // as a solid white slab.
                //
                // Opacity is read from its own slot rather than from the
                // colour's alpha, because FBX carries it separately and a phong
                // material's diffuse alpha is routinely 1 while the surface is
                // half transparent.
                const auto& colour = material->pbr.base_color.value_vec4;
                const double alpha = material->pbr.opacity.has_value
                                         ? (std::clamp)(material->pbr.opacity.value_real, 0.0, 1.0)
                                         : 1.0;
                materials += ",\"baseColorFactor\":[" +
                    number((std::clamp)(colour.x, 0.0, 1.0)) + ',' +
                    number((std::clamp)(colour.y, 0.0, 1.0)) + ',' +
                    number((std::clamp)(colour.z, 0.0, 1.0)) + ',' + number(alpha) + ']';
                if (alpha < 1.0) transparent_factor = true;
            }
            materials += "}";
            if (normal_map >= 0)
                materials += ",\"normalTexture\":{\"index\":" + std::to_string(normal_map) + "}";
            // MASK for a cutout, BLEND for a soft overlay, decided by measuring
            // the composited alpha rather than by the material's name.
            //
            // This was BLEND for everything, on the reasoning that a lash map is
            // soft-edged and a cutoff would leave it jagged. That is true of
            // *some* lash maps and false of others, which is exactly why the
            // name cannot decide it: in the reference corpus Alika's eyelash map
            // is a crisper cutout (17.1% of it fully opaque) than the hair that
            // needed this (10.8%), while Talking-Kevin's eyelash map is soft
            // (0.4%) and sits with eye occlusion, tearlines and scalp caps at or
            // below 0.4%. The two populations are ten times apart with nothing
            // between them.
            //
            // It matters because blending is what made hair look thin: a soft
            // mask blended over itself never accumulates to solid, so strands
            // that are 60% opaque in the texture read as haze. A cutoff snaps
            // them to solid and doubles the covered area, and on a mask that is
            // already a hard cutout it changes nothing at all — which is why it
            // is safe on Alika's lashes and wrong on Talking-Kevin's.
            const auto draw = (masked && base_colour >= 0 &&
                               static_cast<std::size_t>(base_colour) < image_files.size())
                                  ? image_files[static_cast<std::size_t>(base_colour)].draw
                                  : AlphaDraw::Blend;
            if (masked) {
                switch (draw) {
                    case AlphaDraw::MaskCutout:
                        materials += ",\"alphaMode\":\"MASK\",\"alphaCutoff\":0.4";
                        break;
                    case AlphaDraw::MaskSolid:
                        // Low, so the soft edge survives as opaque rather than
                        // being cut away: what shredded the fur was losing the
                        // tips, not testing them.
                        materials += ",\"alphaMode\":\"MASK\",\"alphaCutoff\":0.05";
                        break;
                    case AlphaDraw::Blend:
                        materials += ",\"alphaMode\":\"BLEND\"";
                        break;
                }
                ++result.opacity_composited;
                if (draw != AlphaDraw::Blend) ++result.masks_cut_out;
            } else if (transparent_factor) {
                materials += ",\"alphaMode\":\"BLEND\"";
            }
            materials += ",\"doubleSided\":true}";

            if (!primitives.empty()) primitives += ',';
            primitives += "{\"attributes\":{" + attributes + "},\"indices\":" +
                std::to_string(index_accessor) + ",\"mode\":4,\"material\":" +
                std::to_string(material_count++) + "}";
            imported.primitives += 1;
            imported.vertices += unique;
            imported.triangles += indices.size() / 3;
        }

        if (imported.primitives == 0) continue;

        // The skin: joints as a flat list of nodes each carrying its own world
        // transform as its local matrix.
        //
        // The real bone hierarchy is deliberately not rebuilt. glTF composes a
        // joint's global transform down the node tree, so a flat list of world
        // transforms produces exactly the same bind result with nothing to get
        // wrong. It gives up animation, which this import does not read anyway,
        // and its inverse bind matrix — but the *retarget* does walk the tree,
        // to fold a joint nothing corresponds to into the nearest one that does.
        std::string skin_json, joint_nodes, joint_list, joint_roots;
        std::size_t joint_count = 0;
        if (skin != nullptr) {
            pad_to_four(binary);
            const auto bind_offset = binary.size();
            for (std::size_t at = 0; at < skin->clusters.count; ++at) {
                // inverse(joint world), because the geometry is in world space.
                // Not `geometry_to_bone`: that maps out of a space nothing else
                // in this file is written in.
                const auto* bone = skin->clusters.data[at]->bone_node;
                const ufbx_matrix world = bone != nullptr ? bone->node_to_world
                                                          : ufbx_identity_matrix;
                const ufbx_matrix inverse = ufbx_matrix_invert(&world);
                for (const auto value : to_gltf_matrix(inverse)) append_float(binary, value);
            }
            const auto bind_view = add_view(bind_offset, binary.size() - bind_offset, 0);
            accessors += ",{\"bufferView\":" + std::to_string(bind_view) +
                ",\"componentType\":5126,\"count\":" + std::to_string(skin->clusters.count) +
                ",\"type\":\"MAT4\"}";
            const auto bind_accessor = accessor_count++;

            // Emitted as a tree rather than a flat list, which is both what
            // glTF means by a skeleton and what a retarget needs.
            //
            // The first version emitted every joint as a root carrying its world
            // transform. That binds identically — glTF composes down the tree,
            // and a one-level tree composes to the same thing — and it throws
            // away ancestry. Ancestry is what folds a joint nothing corresponds
            // to into the nearest one that does: Character Creator rigs an
            // accessory to a bone named after itself, so an earring binds
            // `Earring_Flower_0` and the only way to know that is the head is to
            // walk up from it. Without the tree that mesh simply fails to
            // convert, and the set of accessory names is unbounded.
            // The nodes to emit: every joint this skin binds, then every
            // ancestor of one, whether or not the skin binds it.
            //
            // The ancestors are the point. A mesh may bind exactly one joint —
            // an earring binds `Earring_Flower_0` and nothing else — and its
            // ancestry then lies entirely outside its own skin. Emitting only
            // the skin's joints leaves that mesh a lone root with nothing above
            // it, and no reader can discover that the bone hangs off the head.
            // So the chain is carried, which is also simply the truth about the
            // skeleton.
            std::vector<const ufbx_node*> emitted;
            std::map<const ufbx_node*, std::size_t> index_of_bone;
            for (std::size_t at = 0; at < skin->clusters.count; ++at) {
                const auto* bone = skin->clusters.data[at]->bone_node;
                index_of_bone.emplace(bone, emitted.size());
                emitted.push_back(bone);  // null is tolerated; it emits as identity
            }
            for (std::size_t at = 0; at < skin->clusters.count; ++at)
                for (const ufbx_node* up = emitted[at] != nullptr ? emitted[at]->parent : nullptr;
                     up != nullptr; up = up->parent) {
                    if (index_of_bone.count(up) != 0) break;  // and everything above it
                    index_of_bone.emplace(up, emitted.size());
                    emitted.push_back(up);
                }
            // Then the rest of the rig: everything hanging off what is already
            // emitted, which — the ancestors having reached the skeleton's root —
            // is the whole skeleton, bound by this mesh or not.
            //
            // The ancestors alone are not enough, because the two hierarchies
            // disagree about more than names. Character Creator hangs the tongue
            // off the jaw where Bento hangs it off the lower teeth, so the tongue
            // mesh's ancestry never mentions a tooth: it could not discover where
            // its own Bento parent goes, assumed Linden's offset for it, and
            // landed 27 mm out from the teeth the *teeth* mesh had moved. A part
            // has to be able to ask about any joint the import places, not only
            // the ones above itself.
            //
            // Cost is a few dozen named nodes with a matrix each, against
            // megabytes of texture in the same file.
            for (std::size_t at = 0; at < emitted.size(); ++at) {
                const auto* here = emitted[at];
                if (here == nullptr) continue;
                for (std::size_t child = 0; child < here->children.count; ++child) {
                    const auto* below = here->children.data[child];
                    // Bones and the null helpers between them. A mesh, camera or
                    // light under a bone is content hanging off the rig, not part
                    // of it, and emitting it would put geometry in the skeleton.
                    if (below == nullptr || below->mesh != nullptr || below->camera != nullptr ||
                        below->light != nullptr)
                        continue;
                    if (index_of_bone.count(below) != 0) continue;
                    index_of_bone.emplace(below, emitted.size());
                    emitted.push_back(below);
                }
            }

            std::vector<std::string> children(emitted.size());
            std::string roots;
            for (std::size_t at = 0; at < emitted.size(); ++at) {
                const auto* parent = emitted[at] != nullptr ? emitted[at]->parent : nullptr;
                const auto found = parent != nullptr ? index_of_bone.find(parent)
                                                     : index_of_bone.end();
                // Node 0 is the mesh; these follow it.
                const auto node_index = std::to_string(1 + at);
                if (found == index_of_bone.end()) {
                    if (!roots.empty()) roots += ',';
                    roots += node_index;
                } else {
                    auto& list = children[found->second];
                    if (!list.empty()) list += ',';
                    list += node_index;
                }
            }

            for (std::size_t at = 0; at < emitted.size(); ++at) {
                const auto* bone = emitted[at];
                const auto* parent = bone != nullptr ? bone->parent : nullptr;
                const bool parented = parent != nullptr && index_of_bone.count(parent) != 0;
                if (!joint_nodes.empty()) joint_nodes += ',';
                // Only the skin's own joints go in the joints array; the
                // ancestors are there to be walked, not to be bound.
                if (at < skin->clusters.count) {
                    if (!joint_list.empty()) joint_list += ',';
                    joint_list += std::to_string(1 + at);
                }
                // Local to its parent, since glTF accumulates down the tree: a
                // world transform would be applied once per ancestor.
                const ufbx_matrix world =
                    bone != nullptr ? bone->node_to_world : ufbx_identity_matrix;
                ufbx_matrix local = world;
                if (parented) {
                    const ufbx_matrix inverse = ufbx_matrix_invert(&parent->node_to_world);
                    local = ufbx_matrix_mul(&inverse, &world);
                }
                joint_nodes += "{\"name\":" +
                    json_string(bone != nullptr ? text(bone->name) : std::string_view{}) +
                    ",\"matrix\":[";
                const auto matrix = to_gltf_matrix(local);
                for (std::size_t element = 0; element < matrix.size(); ++element) {
                    if (element != 0) joint_nodes += ',';
                    joint_nodes += number(matrix[element]);
                }
                joint_nodes += "]";
                if (!children[at].empty()) joint_nodes += ",\"children\":[" + children[at] + "]";
                joint_nodes += "}";
                ++joint_count;
            }
            joint_roots = roots;
            skin_json = ",\"skins\":[{\"inverseBindMatrices\":" + std::to_string(bind_accessor) +
                        ",\"joints\":[" + joint_list + "]}]";
        }

        // Bring this part's images within what the gate and the viewer will
        // take, before they are embedded.
        //
        // The creator's bytes are still preferred verbatim — an import that
        // re-encodes changes the file it claims to have imported — but "verbatim
        // or nothing" was costing whole characters. One export's four 4096²
        // PNG normal maps came to 100.7 MiB on a body whose geometry is
        // trivial, so the part measured 133.9 MiB against a 32 MiB limit and
        // the import produced nothing at all.
        //
        // Two rules, in order:
        //
        //   1. **Nothing wider than 2048.** That is the viewer's own ceiling
        //      (`MAX_IMAGE_SIZE_DEFAULT` in llgltexture.h), the size every
        //      Firestorm upload path scales to, so a 4096 map is detail no
        //      viewer in this pipeline was ever going to draw.
        //   2. **Then halve the largest until the set fits `max_image_bytes`.**
        //      A cap alone is not enough: four 2048 normal maps still come to
        //      about 25 MiB. This is what makes the outcome a guarantee rather
        //      than a size that happened to be lucky, and it takes from the
        //      biggest image each round, so one heavy map is reduced before a
        //      small one is touched at all.
        //
        // Re-encoding follows the alpha, not the source format: PNG only where
        // there is alpha to keep, JPEG otherwise. Re-encoding an opaque skin
        // JPEG as PNG can come out *larger* than the original, which would make
        // a downscale into an increase.
        const auto shrink = [&](Embedded& file, std::uint32_t limit) -> bool {
            const std::vector<std::uint8_t> source(
                reinterpret_cast<const std::uint8_t*>(file.bytes.data()),
                reinterpret_cast<const std::uint8_t*>(file.bytes.data()) + file.bytes.size());
            auto decoded = image::decode_png_or_jpeg(source);
            if (!decoded) return false;
            const auto widest = (std::max)(decoded->width, decoded->height);
            if (widest <= limit) return false;
            // Halving rather than scaling straight to the limit: powers of two
            // are what the texture pipeline wants, and an exact quartering of
            // the pixels is what the box filter is cleanest at.
            std::uint32_t width = decoded->width, height = decoded->height;
            while ((std::max)(width, height) > limit && width > 1 && height > 1) {
                width = (std::max)(1u, width / 2);
                height = (std::max)(1u, height / 2);
            }
            const auto scaled = image::resize_box(*decoded, width, height);
            if (scaled.empty()) return false;
            const bool keeps_alpha = scaled.channels == 2 || scaled.channels == 4;
            auto encoded = keeps_alpha ? image::encode_png(scaled)
                                       : image::encode_jpeg(scaled, 90);
            if (!encoded) return false;
            file.bytes.assign(reinterpret_cast<const std::byte*>(encoded->data()),
                              reinterpret_cast<const std::byte*>(encoded->data()) +
                                  encoded->size());
            file.mime = keeps_alpha ? "image/png" : "image/jpeg";
            ++result.textures_downscaled;
            return true;
        };

        constexpr std::uint32_t viewer_texture_limit = 2048;
        for (auto& file : image_files) static_cast<void>(shrink(file, viewer_texture_limit));

        const auto total_image_bytes = [&] {
            std::size_t total = 0;
            for (const auto& file : image_files) total += file.bytes.size();
            return total;
        };
        std::uint32_t budget_limit = viewer_texture_limit;
        while (total_image_bytes() > max_image_bytes && budget_limit > 64) {
            budget_limit /= 2;
            // Every image over the new limit comes down, not just the largest:
            // taking one at a time converges too slowly when a body carries four
            // maps of the same size, and leaves them at mismatched resolutions.
            bool reduced = false;
            for (auto& file : image_files) reduced = shrink(file, budget_limit) || reduced;
            if (!reduced) break;
        }

        // Images, embedded as buffer views.
        std::string images, textures;
        for (std::size_t at = 0; at < image_files.size(); ++at) {
            const auto& file = image_files[at];
            pad_to_four(binary);
            const auto offset = binary.size();
            binary.insert(binary.end(), file.bytes.begin(), file.bytes.end());
            const auto view = add_view(offset, file.bytes.size(), 0);
            if (!images.empty()) images += ',';
            images += "{\"bufferView\":" + std::to_string(view) + ",\"mimeType\":\"" +
                      file.mime + "\"}";
            if (!textures.empty()) textures += ',';
            textures += "{\"source\":" + std::to_string(at) + "}";
        }
        imported.textures = image_files.size();

        std::string document =
            "{\"asset\":{\"version\":\"2.0\",\"generator\":\"" + std::string(generator) +
            "\"},\"buffers\":[{\"byteLength\":" + std::to_string(binary.size()) +
            "}],\"bufferViews\":[" + views + "],\"accessors\":[" + accessors +
            "],\"materials\":[" + materials + "]";
        if (!images.empty())
            document += ",\"images\":[" + images + "],\"textures\":[" + textures + "]";
        document += ",\"meshes\":[{\"name\":" + json_string(imported.name) +
                    ",\"primitives\":[" + primitives + "]}]" + skin_json +
                    ",\"nodes\":[{\"name\":" + json_string(imported.name) + ",\"mesh\":0";
        if (skin != nullptr) document += ",\"skin\":0";
        document += "}";
        if (!joint_nodes.empty()) document += ',' + joint_nodes;
        // The mesh and the skeleton's roots only. Listing every joint would make
        // each one a scene root as well as somebody's child, and glTF allows a
        // node to appear in the hierarchy exactly once.
        document += "],\"scenes\":[{\"nodes\":[0";
        if (!joint_roots.empty()) document += ',' + joint_roots;
        document += "]}],\"scene\":0}";

        imported.glb = glb::wrap(std::move(document), std::move(binary));
        result.meshes.push_back(std::move(imported));
    }

    if (result.meshes.empty()) return fail("the FBX carries no mesh with placed geometry");
    result.ok = true;
    return result;
}

} // namespace homeworldz::mesh
