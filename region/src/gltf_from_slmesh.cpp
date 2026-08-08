// The `gltf` rendition: a stored type-49 mesh made readable by clients on the
// modern path (ADR 0033 M2). The inverse direction of mesh_convert.cpp, and
// deliberately hand-written rather than library-driven — cgltf reads glTF and
// does not write it, and a GLB container is a header and two chunks.

#include "homeworldz/mesh_convert.h"

#include "homeworldz/axes.h"

#include "homeworldz/slmesh.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace homeworldz::mesh {

namespace {

// Shortest round-trip decimal, so the JSON carries the float that was
// computed rather than a printf approximation of it.
std::string number(float value) {
    std::array<char, 32> buffer{};
    const auto [end, error] =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (error != std::errc{}) return "0";
    return std::string(buffer.data(), end);
}


void append_u32(std::vector<std::byte>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
}

void append_float(std::vector<std::byte>& out, float value) {
    std::array<std::byte, sizeof value> raw{};
    std::memcpy(raw.data(), &value, sizeof value);
    out.insert(out.end(), raw.begin(), raw.end());
}

void append_u16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>(value & 0xffu));
    out.push_back(static_cast<std::byte>((value >> 8) & 0xffu));
}

// glTF requires an accessor's byteOffset to be a multiple of its component
// size; padding every view to four satisfies both float and u16 attributes.
void pad_to_four(std::vector<std::byte>& out) {
    while (out.size() % 4 != 0) out.push_back(std::byte{});
}

} // namespace

GltfConversion gltf_from_sl_mesh(std::span<const std::byte> asset) {
    GltfConversion result;
    const auto parsed = slmesh::parse(asset);
    if (!parsed) {
        result.error = "the asset is not a readable SL mesh";
        return result;
    }
    if (parsed->high.empty()) {
        result.error = "the mesh carries no faces";
        return result;
    }

    std::vector<std::byte> binary;
    std::string views;
    std::string accessors;
    std::string primitives;
    std::size_t view_count = 0;
    std::size_t accessor_count = 0;

    const auto add_view = [&](std::size_t offset, std::size_t length, int target) {
        if (!views.empty()) views += ',';
        views += "{\"buffer\":0,\"byteOffset\":" + std::to_string(offset) +
                 ",\"byteLength\":" + std::to_string(length) +
                 ",\"target\":" + std::to_string(target) + "}";
        return view_count++;
    };

    for (const auto& submesh : parsed->high) {
        if (submesh.positions.empty() || submesh.indices.empty()) {
            result.error = "a submesh carries no geometry";
            return result;
        }
        if (submesh.positions.size() > (std::numeric_limits<std::uint16_t>::max)()) {
            result.error = "a submesh exceeds the 16-bit index range";
            return result;
        }
        const bool has_normals = submesh.normals.size() == submesh.positions.size();
        const bool has_texcoords = submesh.texcoords.size() == submesh.positions.size();

        // Positions, with the min/max the spec requires on POSITION.
        std::array<float, 3> low{submesh.positions[0]};
        std::array<float, 3> high{submesh.positions[0]};
        pad_to_four(binary);
        const auto position_offset = binary.size();
        {
            auto first = submesh.positions[0];
            to_gltf_axes(first);
            low = first;
            high = first;
        }
        for (const auto& stored_position : submesh.positions) {
            auto position = stored_position;
            to_gltf_axes(position);
            for (int axis = 0; axis < 3; ++axis) {
                low[axis] = (std::min)(low[axis], position[axis]);
                high[axis] = (std::max)(high[axis], position[axis]);
                append_float(binary, position[axis]);
            }
        }
        const auto position_view =
            add_view(position_offset, binary.size() - position_offset, 34962);
        if (!accessors.empty()) accessors += ',';
        accessors += "{\"bufferView\":" + std::to_string(position_view) +
            ",\"componentType\":5126,\"count\":" + std::to_string(submesh.positions.size()) +
            ",\"type\":\"VEC3\",\"min\":[" + number(low[0]) + "," + number(low[1]) + "," +
            number(low[2]) + "],\"max\":[" + number(high[0]) + "," + number(high[1]) + "," +
            number(high[2]) + "]}";
        const auto position_accessor = accessor_count++;

        std::string attributes = "\"POSITION\":" + std::to_string(position_accessor);
        if (has_normals) {
            pad_to_four(binary);
            const auto offset = binary.size();
            for (const auto& stored_normal : submesh.normals) {
                auto normal = stored_normal;
                to_gltf_axes(normal);
                for (int axis = 0; axis < 3; ++axis) append_float(binary, normal[axis]);
            }
            const auto view = add_view(offset, binary.size() - offset, 34962);
            accessors += ",{\"bufferView\":" + std::to_string(view) +
                ",\"componentType\":5126,\"count\":" +
                std::to_string(submesh.normals.size()) + ",\"type\":\"VEC3\"}";
            attributes += ",\"NORMAL\":" + std::to_string(accessor_count++);
        }
        if (has_texcoords) {
            pad_to_four(binary);
            const auto offset = binary.size();
            for (const auto& texcoord : submesh.texcoords) {
                append_float(binary, texcoord[0]);
                append_float(binary, texcoord[1]);
            }
            const auto view = add_view(offset, binary.size() - offset, 34962);
            accessors += ",{\"bufferView\":" + std::to_string(view) +
                ",\"componentType\":5126,\"count\":" +
                std::to_string(submesh.texcoords.size()) + ",\"type\":\"VEC2\"}";
            attributes += ",\"TEXCOORD_0\":" + std::to_string(accessor_count++);
        }

        pad_to_four(binary);
        const auto index_offset = binary.size();
        for (const auto index : submesh.indices) append_u16(binary, index);
        const auto index_view = add_view(index_offset, binary.size() - index_offset, 34963);
        accessors += ",{\"bufferView\":" + std::to_string(index_view) +
            ",\"componentType\":5123,\"count\":" + std::to_string(submesh.indices.size()) +
            ",\"type\":\"SCALAR\"}";
        const auto index_accessor = accessor_count++;

        if (!primitives.empty()) primitives += ',';
        primitives += "{\"attributes\":{" + attributes + "},\"indices\":" +
                      std::to_string(index_accessor) + ",\"mode\":4,\"material\":0}";

        result.primitives += 1;
        result.vertices += submesh.positions.size();
        result.triangles += submesh.indices.size() / 3;
    }
    pad_to_four(binary);

    // State the material rather than leaving the primitive without one, because
    // what a renderer does with a material-less primitive is not settled in
    // practice. glTF says such a primitive takes the specification's default
    // material, which is metallicFactor 1.0 — a fully rough metal with no
    // diffuse response. Godot does not do that: it honours per-field defaults
    // inside a material that is declared, but substitutes its own non-metallic
    // StandardMaterial3D when none is declared at all, so absence of a material
    // is not equivalent there to a material of defaults. Both readings are
    // defensible and they disagree, which is the whole argument for saying it.
    //
    // Measured 2026-08-07 in the Homeworldz client (Godot): renditions carrying
    // no material imported at metallic 0.00. The spec-derived prediction of
    // metal was wrong for that renderer and right for others; relying on either
    // is the bug.
    //
    // The content is a mesh a person uploaded through a viewer, which is not
    // metal. Saying so explicitly makes the result the same everywhere instead
    // of resting on whose default wins.
    //
    // No baseColorTexture: a type-49 mesh carries geometry, and its faces are
    // textured by the region's own texture entry rather than by anything in the
    // asset. Naming an image here would invent one.
    constexpr const char* default_material =
        "{\"pbrMetallicRoughness\":{\"metallicFactor\":0,\"roughnessFactor\":1}}";
    std::string document =
        "{\"asset\":{\"version\":\"2.0\",\"generator\":\"" + std::string(generator) +
        "\"},\"buffers\":[{\"byteLength\":" + std::to_string(binary.size()) +
        "}],\"bufferViews\":[" + views + "],\"accessors\":[" + accessors +
        "],\"materials\":[" + std::string(default_material) +
        "],\"meshes\":[{\"primitives\":[" + primitives +
        "]}],\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
    while (document.size() % 4 != 0) document.push_back(' ');

    const auto total = 12 + 8 + document.size() + (binary.empty() ? 0 : 8 + binary.size());
    result.glb.reserve(total);
    const char magic[4] = {'g', 'l', 'T', 'F'};
    for (const char character : magic)
        result.glb.push_back(static_cast<std::byte>(character));
    append_u32(result.glb, 2);
    append_u32(result.glb, static_cast<std::uint32_t>(total));
    append_u32(result.glb, static_cast<std::uint32_t>(document.size()));
    for (const char character : {'J', 'S', 'O', 'N'})
        result.glb.push_back(static_cast<std::byte>(character));
    for (const char character : document)
        result.glb.push_back(static_cast<std::byte>(character));
    if (!binary.empty()) {
        append_u32(result.glb, static_cast<std::uint32_t>(binary.size()));
        for (const char character : {'B', 'I', 'N', '\0'})
            result.glb.push_back(static_cast<std::byte>(character));
        result.glb.insert(result.glb.end(), binary.begin(), binary.end());
    }
    result.ok = true;
    return result;
}

} // namespace homeworldz::mesh
