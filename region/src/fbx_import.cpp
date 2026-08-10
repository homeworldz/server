// FBX → glTF (ADR 0035). One GLB per FBX mesh, written by hand for the reason
// gltf_from_slmesh.cpp gives: cgltf reads glTF and does not write it.

#include "homeworldz/fbx_import.h"

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

        // Geometry stays in its own space when skinned, because that is the
        // space the cluster matrices map out of: skinning composes the joint's
        // world transform with `geometry_to_bone`, and baking a world transform
        // into the positions would apply it twice. Unskinned geometry has no
        // such composition to ride on, so its node transform is baked in.
        const ufbx_matrix to_world = node->geometry_to_world;
        const ufbx_matrix normal_matrix = ufbx_matrix_for_normals(&to_world);
        const bool bake_transform = skin == nullptr;

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
        std::map<std::uint32_t, std::size_t> image_slot;
        std::vector<const ufbx_texture_file*> image_files;
        const auto slot_for = [&](const ufbx_texture* texture) -> long long {
            if (texture == nullptr || !texture->has_file) return -1;
            const auto& file = scene->texture_files.data[texture->file_index];
            if (file.content.size == 0) return -1;      // named, not carried
            if (sniff_image(file.content) == nullptr) return -1;
            if (const auto found = image_slot.find(texture->file_index);
                found != image_slot.end())
                return static_cast<long long>(found->second);
            const auto slot = image_files.size();
            image_slot.emplace(texture->file_index, slot);
            image_files.push_back(&file);
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
                    // FBX puts the UV origin at the bottom left and glTF at the
                    // top left, so V is flipped. Getting this wrong produces a
                    // texture that is present, plausible and upside down.
                    streams.texcoords.push_back(
                        {static_cast<float>(uv.x), 1.0f - static_cast<float>(uv.y)});
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
            std::string name = "part " + std::to_string(part_index);
            if (material != nullptr) {
                name = std::string(text(material->name));
                for (std::size_t at = 0; at < material->textures.count; ++at) {
                    const auto& binding = material->textures.data[at];
                    const auto property = text(binding.material_prop);
                    if (property == "DiffuseColor") {
                        base_colour = slot_for(binding.texture);
                    } else if (property == "NormalMap" || property == "Bump") {
                        normal_map = slot_for(binding.texture);
                    } else {
                        // TransparentColor above all: Character Creator writes
                        // opacity as its own image and glTF carries it only as
                        // the alpha of the base colour, which would need the
                        // two composited. Counted, not silently dropped - the
                        // visible result is a lash or a tearline drawn as an
                        // opaque slab.
                        ++result.bindings_dropped;
                    }
                }
            }
            if (!materials.empty()) materials += ',';
            materials += "{\"name\":" + json_string(name) +
                ",\"pbrMetallicRoughness\":{\"metallicFactor\":0,\"roughnessFactor\":1";
            if (base_colour >= 0)
                materials += ",\"baseColorTexture\":{\"index\":" + std::to_string(base_colour) + "}";
            materials += "}";
            if (normal_map >= 0)
                materials += ",\"normalTexture\":{\"index\":" + std::to_string(normal_map) + "}";
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
        // and it gives up nothing else: `convert_glb` reads a joint's name and
        // its inverse bind matrix and never walks the tree.
        std::string skin_json, joint_nodes, joint_list;
        std::size_t joint_count = 0;
        if (skin != nullptr) {
            pad_to_four(binary);
            const auto bind_offset = binary.size();
            for (std::size_t at = 0; at < skin->clusters.count; ++at) {
                const auto& matrix = to_gltf_matrix(skin->clusters.data[at]->geometry_to_bone);
                for (const auto value : matrix) append_float(binary, value);
            }
            const auto bind_view = add_view(bind_offset, binary.size() - bind_offset, 0);
            accessors += ",{\"bufferView\":" + std::to_string(bind_view) +
                ",\"componentType\":5126,\"count\":" + std::to_string(skin->clusters.count) +
                ",\"type\":\"MAT4\"}";
            const auto bind_accessor = accessor_count++;

            for (std::size_t at = 0; at < skin->clusters.count; ++at) {
                const ufbx_skin_cluster* cluster = skin->clusters.data[at];
                const auto* bone = cluster->bone_node;
                if (!joint_nodes.empty()) joint_nodes += ',';
                if (!joint_list.empty()) joint_list += ',';
                // Node 0 is the mesh; joints follow it.
                joint_list += std::to_string(1 + at);
                joint_nodes += "{\"name\":" +
                    json_string(bone != nullptr ? text(bone->name) : std::string_view{}) +
                    ",\"matrix\":[";
                const auto matrix = to_gltf_matrix(
                    bone != nullptr ? bone->node_to_world : ufbx_identity_matrix);
                for (std::size_t element = 0; element < matrix.size(); ++element) {
                    if (element != 0) joint_nodes += ',';
                    joint_nodes += number(matrix[element]);
                }
                joint_nodes += "]}";
                ++joint_count;
            }
            skin_json = ",\"skins\":[{\"inverseBindMatrices\":" + std::to_string(bind_accessor) +
                        ",\"joints\":[" + joint_list + "]}]";
        }

        // Images, embedded as buffer views. The creator's bytes, verbatim: an
        // import re-encoding them would change the file the creator sent while
        // claiming to have imported it.
        std::string images, textures;
        for (std::size_t at = 0; at < image_files.size(); ++at) {
            const auto& file = *image_files[at];
            pad_to_four(binary);
            const auto offset = binary.size();
            const auto* bytes = static_cast<const std::byte*>(file.content.data);
            binary.insert(binary.end(), bytes, bytes + file.content.size);
            const auto view = add_view(offset, file.content.size, 0);
            if (!images.empty()) images += ',';
            images += "{\"bufferView\":" + std::to_string(view) + ",\"mimeType\":\"" +
                      sniff_image(file.content) + "\"}";
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
        document += "],\"scenes\":[{\"nodes\":[0";
        for (std::size_t at = 0; at < joint_count; ++at) document += ',' + std::to_string(1 + at);
        document += "]}],\"scene\":0}";

        imported.glb = glb::wrap(std::move(document), std::move(binary));
        result.meshes.push_back(std::move(imported));
    }

    if (result.meshes.empty()) return fail("the FBX carries no mesh with placed geometry");
    result.ok = true;
    return result;
}

} // namespace homeworldz::mesh
