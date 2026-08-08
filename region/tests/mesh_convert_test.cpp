#include "homeworldz/image.h"
#include "homeworldz/mesh_convert.h"
#include "homeworldz/slmesh.h"

#include <array>
#include <string_view>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// The same GLB builder the acceptance test uses: real container bytes.
std::vector<std::byte> glb(std::string json, const std::vector<std::uint8_t>& bin) {
    while (json.size() % 4 != 0) json.push_back(' ');
    std::vector<std::uint8_t> padded_bin = bin;
    while (!padded_bin.empty() && padded_bin.size() % 4 != 0) padded_bin.push_back(0);
    const auto append_u32 = [](std::vector<std::uint8_t>& out, std::uint32_t value) {
        out.push_back(static_cast<std::uint8_t>(value));
        out.push_back(static_cast<std::uint8_t>(value >> 8));
        out.push_back(static_cast<std::uint8_t>(value >> 16));
        out.push_back(static_cast<std::uint8_t>(value >> 24));
    };
    std::vector<std::uint8_t> out;
    out.insert(out.end(), {'g', 'l', 'T', 'F'});
    append_u32(out, 2);
    const std::uint32_t total = 12 + 8 + static_cast<std::uint32_t>(json.size()) +
        (padded_bin.empty() ? 0 : 8 + static_cast<std::uint32_t>(padded_bin.size()));
    append_u32(out, total);
    append_u32(out, static_cast<std::uint32_t>(json.size()));
    out.insert(out.end(), {'J', 'S', 'O', 'N'});
    out.insert(out.end(), json.begin(), json.end());
    if (!padded_bin.empty()) {
        append_u32(out, static_cast<std::uint32_t>(padded_bin.size()));
        out.insert(out.end(), {'B', 'I', 'N', 0});
        out.insert(out.end(), padded_bin.begin(), padded_bin.end());
    }
    std::vector<std::byte> bytes(out.size());
    std::memcpy(bytes.data(), out.data(), out.size());
    return bytes;
}

} // namespace

int main() {
    // A quad (two triangles, four vertices, indexed) under a node that
    // translates it by +10 on x: the conversion must apply the transform.
    const float positions[12] = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    const std::uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
    std::vector<std::uint8_t> bin(sizeof positions + sizeof indices);
    std::memcpy(bin.data(), positions, sizeof positions);
    std::memcpy(bin.data() + sizeof positions, indices, sizeof indices);
    const std::string json =
        R"({"asset":{"version":"2.0"},)"
        R"("buffers":[{"byteLength":60}],)"
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":48},)"
        R"({"buffer":0,"byteOffset":48,"byteLength":12}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3",)"
        R"("min":[0,0,0],"max":[1,1,0]},)"
        R"({"bufferView":1,"componentType":5123,"count":6,"type":"SCALAR"}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],)"
        R"("nodes":[{"mesh":0,"translation":[10,0,0]}],)"
        R"("scenes":[{"nodes":[0]}],"scene":0})";

    const auto conversion = homeworldz::mesh::convert_glb(glb(json, bin));
    if (!conversion.ok) return 1;
    if (conversion.faces != 1 || conversion.high_triangles != 2) return 2;

    // The wrapper prim's scale comes from the declared world bounds, in region
    // axes. The source quad lies in glTF's XY plane, so its thin axis is glTF Z,
    // and it is translated +10 along glTF X. Under `(x, y, z)_glTF ->
    // (z, x, y)_region` that lands thin in region X, offset along region Y, and
    // one metre tall in region Z. A square quad cannot show any of this; only
    // the thin axis can (the cube-proves-size / triangle-proves-orientation
    // lesson, 2026-07-30).
    //
    // **This assertion discriminates the yaw, and it is the reason to run it.**
    // It was written against the old `(x, -z, y)` map and disagreed the moment
    // that map was corrected on 2026-08-08 — but this executable had no
    // add_test, so it had never been run and said nothing. The one fixture in
    // the tree that could have caught the axis fault was sitting unexecuted
    // while the fault shipped and was found by measuring a skeleton instead.
    const auto bounds = homeworldz::mesh::declared_world_bounds(glb(json, bin));
    if (!bounds.ok || std::fabs(bounds.center[1] - 10.5f) > 0.001f ||
        std::fabs(bounds.extent[1] - 1.0f) > 0.001f ||
        std::fabs(bounds.extent[2] - 1.0f) > 0.001f)
        return 10;
    if (bounds.extent[0] > 0.01f) return 10;  // thin axis is region X (forward)

    // The output is a well-formed type-49 asset whose geometry is normalized
    // by those same bounds — the unit domain the prim scale stretches back to
    // authored size. The far corner lands at (+0.5, +0.5) in the two axes the
    // quad spans, which after the `(z, x, y)` axis map are region Y and Z — the
    // quad's own plane is glTF XY, and glTF Z, its thin axis, becomes region X.
    const auto parsed = homeworldz::slmesh::parse(conversion.sl_mesh);
    if (!parsed || parsed->high.size() != 1) return 3;
    const auto& face = parsed->high.front();
    if (face.positions.size() != 4 || face.indices.size() != 6) return 4;
    bool found_corner = false;
    for (const auto& position : face.positions) {
        if (std::fabs(position[1]) > 0.501f || std::fabs(position[2]) > 0.501f) return 5;
        if (std::fabs(position[1] - 0.5f) < 0.001f && std::fabs(position[2] - 0.5f) < 0.001f)
            found_corner = true;
    }
    if (!found_corner) return 5;

    // The source has no normals, so the converter computed them: a quad lying in
    // glTF's XY plane faces glTF +Z, which under `(z, x, y)` is region X — the
    // forward axis. This is the same fact as the thin-extent assertion above,
    // read off a different quantity, and it moved for the same reason.
    if (face.normals.size() != 4 || std::fabs(std::fabs(face.normals[0][0]) - 1.0f) > 0.01f)
        return 11;
    // No texcoords in the source either, so the converter synthesized them,
    // and they vary per vertex — constant UVs are what NaN a viewer's
    // tangent math.
    if (face.texcoords.size() != 4) return 12;
    bool uv_varies = false;
    for (std::size_t vertex = 1; vertex < 4; ++vertex)
        if (std::fabs(face.texcoords[vertex][0] - face.texcoords[0][0]) > 0.01f ||
            std::fabs(face.texcoords[vertex][1] - face.texcoords[0][1]) > 0.01f)
            uv_varies = true;
    if (!uv_varies) return 13;

    // Every level is present and non-empty; the physics hull is the unit box.
    if (parsed->medium.empty() || parsed->low.empty() || parsed->lowest.empty()) return 6;
    if (parsed->physics_hull.size() != 8) return 7;
    float max_x = -1e9f;
    for (const auto& vertex : parsed->physics_hull) max_x = (std::max)(max_x, vertex[0]);
    if (std::fabs(max_x - 0.5f) > 0.001f) return 8;

    // Geometry-free input fails with a reason, never with bytes.
    const auto empty = homeworldz::mesh::convert_glb(glb(
        R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[]}],"scene":0})", {}));
    if (empty.ok || empty.error.empty()) return 9;

    // Texture extraction (ADR 0033 M3), and the property that matters most
    // about it: face N means the same face to the converter and to the
    // TextureEntry built from this. Two materials, the first textured and the
    // second not, so a mismatch in ordering shows as the wrong face being
    // textured rather than as a count that happens to agree.
    {
        // A 1x1 PNG, hand-assembled: signature, IHDR, IDAT (a zlib stored
        // block), IEND. Real bytes so the extractor's mime and size checks run
        // against something a decoder would accept.
        // A valid 1x1 RGBA PNG (generated, CRCs and zlib stream real), so the
        // decode assertion below tests stb rather than tolerating a broken
        // fixture - the first version of this array parsed as a texture and
        // failed to decode, which is the fixture lying about being evidence.
        const std::vector<std::uint8_t> png{
            0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,
            0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,
            0x08,0x06,0x00,0x00,0x00,0x1f,0x15,0xc4,0x89,0x00,0x00,0x00,
            0x0d,0x49,0x44,0x41,0x54,0x78,0x9c,0x63,0xf8,0xcf,0xc0,0xf0,
            0x1f,0x00,0x05,0x00,0x01,0xff,0x89,0x99,0x3d,0x1d,0x00,0x00,
            0x00,0x00,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82};
        const float positions[12] = {0,0,0, 1,0,0, 1,1,0, 0,1,0};
        const std::uint16_t indices[6] = {0,1,2, 0,2,3};
        std::vector<std::uint8_t> bin(sizeof positions + sizeof indices + png.size());
        std::memcpy(bin.data(), positions, sizeof positions);
        std::memcpy(bin.data() + sizeof positions, indices, sizeof indices);
        std::memcpy(bin.data() + sizeof positions + sizeof indices, png.data(), png.size());
        const auto image_offset = sizeof positions + sizeof indices;
        const std::string textured_json =
            std::string(R"({"asset":{"version":"2.0"},)") +
            R"("buffers":[{"byteLength":)" + std::to_string(bin.size()) + R"(}],)" +
            R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":48},)" +
            R"({"buffer":0,"byteOffset":48,"byteLength":12},)" +
            R"({"buffer":0,"byteOffset":)" + std::to_string(image_offset) +
            R"(,"byteLength":)" + std::to_string(png.size()) + R"(}],)" +
            R"("accessors":[{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3",)" +
            R"("min":[0,0,0],"max":[1,1,0]},)" +
            R"({"bufferView":1,"componentType":5123,"count":6,"type":"SCALAR"}],)" +
            R"("images":[{"bufferView":2,"mimeType":"image/png"}],)" +
            R"("samplers":[{}],"textures":[{"source":0,"sampler":0}],)" +
            R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}},{}],)" +
            R"("meshes":[{"primitives":[)" +
            R"({"attributes":{"POSITION":0},"indices":1,"material":0},)" +
            R"({"attributes":{"POSITION":0},"indices":1,"material":1}]}],)" +
            R"("nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
        const auto textured = glb(textured_json, bin);
        const auto extraction = homeworldz::mesh::extract_textures(textured);
        if (!extraction.ok) return 20;
        if (extraction.textures.size() != 1) return 21;
        if (extraction.textures[0].mime != "image/png") return 22;
        if (extraction.textures[0].bytes.size() != png.size()) return 23;
        // Face 0 is the textured material, face 1 is the bare one.
        if (extraction.face_textures.size() != 2) return 24;
        if (extraction.face_textures[0] != 0 || extraction.face_textures[1] != -1) return 25;
        // And the converter agrees about how many faces there are, in that
        // order - the shared traversal is the whole point.
        const auto textured_conversion = homeworldz::mesh::convert_glb(textured);
        if (!textured_conversion.ok) return 26;
        if (textured_conversion.faces != extraction.face_textures.size()) return 27;
        // The embedded PNG decodes, so what the viewer pipeline will re-encode
        // as JPEG2000 is a real image rather than bytes that merely survived.
        const auto decoded = homeworldz::image::decode_png_or_jpeg(png);
        if (!decoded || decoded->width != 1 || decoded->height != 1) return 28;
    }

    // A rigged mesh, built here rather than loaded, because a fixture in the
    // repository is a binary nobody can review and this one has to be exactly
    // right: three vertices bound to two real Bento joints, weights summing to
    // one, and an identity inverse bind matrix each.
    //
    // It is a fixture I authored, which tests this converter against my own
    // reading of the format and no one else's. That is worth stating rather
    // than discovering: it is the first fixture and deliberately not the last.
    {
        std::vector<std::uint8_t> bin;
        const auto put_float = [&](float value) {
            std::uint8_t raw[4];
            std::memcpy(raw, &value, 4);
            bin.insert(bin.end(), raw, raw + 4);
        };
        const auto put_ushort = [&](std::uint16_t value) {
            bin.push_back(static_cast<std::uint8_t>(value));
            bin.push_back(static_cast<std::uint8_t>(value >> 8));
        };
        // positions: 36 bytes
        for (const auto& corner : {std::array<float, 3>{0, 0, 0},
                                   std::array<float, 3>{1, 0, 0},
                                   std::array<float, 3>{0, 1, 0}})
            for (const auto value : corner) put_float(value);
        // joints: 24 bytes, every vertex bound to slots 0 and 1
        for (int vertex = 0; vertex < 3; ++vertex) {
            put_ushort(0); put_ushort(1); put_ushort(0); put_ushort(0);
        }
        // weights: 48 bytes. The trailing pair is zero, which is padding
        // rather than a binding — the converter must drop it rather than spend
        // two of the format's four slots on nothing.
        for (int vertex = 0; vertex < 3; ++vertex) {
            put_float(0.75f); put_float(0.25f); put_float(0.0f); put_float(0.0f);
        }
        // inverse bind matrices: two identities, 128 bytes
        for (int joint = 0; joint < 2; ++joint)
            for (int cell = 0; cell < 16; ++cell) put_float(cell % 5 == 0 ? 1.0f : 0.0f);

        const std::string rigged_json =
            R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":236}],)"
            R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},)"
            R"({"buffer":0,"byteOffset":36,"byteLength":24},)"
            R"({"buffer":0,"byteOffset":60,"byteLength":48},)"
            R"({"buffer":0,"byteOffset":108,"byteLength":128}],)"
            R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",)"
            R"("min":[0,0,0],"max":[1,1,0]},)"
            R"({"bufferView":1,"componentType":5123,"count":3,"type":"VEC4"},)"
            R"({"bufferView":2,"componentType":5126,"count":3,"type":"VEC4"},)"
            R"({"bufferView":3,"componentType":5126,"count":2,"type":"MAT4"}],)"
            R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"JOINTS_0":1,"WEIGHTS_0":2}}]}],)"
            R"("nodes":[{"mesh":0,"skin":0},{"name":"mPelvis"},{"name":"mTorso"}],)"
            R"("skins":[{"joints":[1,2],"inverseBindMatrices":3}],)"
            R"("scenes":[{"nodes":[0]}],"scene":0})";
        const auto rigged = homeworldz::mesh::convert_glb(glb(rigged_json, bin));
        if (!rigged.ok) return 30;
        // Read the asset back rather than searching its bytes. The skin block is
        // zlib-compressed like every other block, so looking for "mPelvis" in
        // the blob tests whether deflate happened to leave that string
        // uncompressed — it passed for a while by exactly that luck, and would
        // have gone on passing had the joint table been empty.
        const auto parsed_rig = homeworldz::slmesh::parse(rigged.sl_mesh);
        // One code per condition: four conditions behind one exit status is how
        // an earlier failure in this tree got attributed to the wrong assertion.
        if (!parsed_rig) return 40;
        if (!parsed_rig->skin) return 41;
        const auto& joints = parsed_rig->skin->joints;
        if (std::find(joints.begin(), joints.end(), "mPelvis") == joints.end()) return 42;
        if (std::find(joints.begin(), joints.end(), "mTorso") == joints.end()) return 43;
        // And the weights reached the level data, not just the joint table.
        if (parsed_rig->high.empty()) return 44;
        if (parsed_rig->high.front().influences.empty()) return 45;

        // bind_shape_matrix must carry the stored geometry back to the space the
        // inverse bind matrices are written in. Stored positions are normalized
        // to about half a unit; the joints are metres apart. Identity here skins
        // one against the other, which is a body drawn as stretched spikes that
        // still animates (in-world, 2026-08-08).
        //
        // Asserted as a round trip rather than by reading the matrix: put every
        // stored vertex through bind_shape and the bounding box must come back
        // to what the GLB declared, on the region's axes. That fails for a wrong
        // matrix whatever shape the wrongness takes, including the identity this
        // shipped with.
        {
            const auto& submesh = parsed_rig->high.front();
            if (submesh.positions.empty()) return 47;
            const auto& bind_shape = parsed_rig->skin->bind_shape;
            std::array<float, 3> low{std::numeric_limits<float>::max(),
                                     std::numeric_limits<float>::max(),
                                     std::numeric_limits<float>::max()};
            std::array<float, 3> high{std::numeric_limits<float>::lowest(),
                                      std::numeric_limits<float>::lowest(),
                                      std::numeric_limits<float>::lowest()};
            for (const auto& position : submesh.positions)
                for (int axis = 0; axis < 3; ++axis) {
                    const auto value = position[axis] * bind_shape[axis * 4 + axis] +
                                       bind_shape[12 + axis];
                    low[axis] = (std::min)(low[axis], value);
                    high[axis] = (std::max)(high[axis], value);
                }
            // The GLB declares min [0,0,0] max [1,1,0]; on the region's axes
            // (x,y,z) <- (z,x,y) that is [0,0,0] to [0,1,1].
            const std::array<float, 3> expected_low{0.0f, 0.0f, 0.0f};
            const std::array<float, 3> expected_high{0.0f, 1.0f, 1.0f};
            for (int axis = 0; axis < 3; ++axis) {
                if (std::fabs(low[axis] - expected_low[axis]) > 0.01f) return 48;
                if (std::fabs(high[axis] - expected_high[axis]) > 0.01f) return 49;
            }
        }

        // An alias resolves to its canonical joint rather than being carried
        // through: what a viewer resolves and what the asset says should not
        // differ, or two readers of the same file disagree about the skeleton.
        std::string aliased_json = rigged_json;
        const auto at = aliased_json.find(R"({"name":"mPelvis"})");
        aliased_json.replace(at, std::string(R"({"name":"mPelvis"})").size(),
                             R"({"name":"hip"})");
        const auto aliased = homeworldz::mesh::convert_glb(glb(aliased_json, bin));
        if (!aliased.ok) return 32;
        // Parsed, not searched. This assertion used to scan the serialized bytes
        // for "mPelvis", which sits inside a zlib-compressed block: it passed
        // only while that block happened to be stored uncompressed, and has been
        // failing silently since compression was fixed - unseen because this
        // executable had no add_test.
        const auto parsed_alias = homeworldz::slmesh::parse(aliased.sl_mesh);
        if (!parsed_alias || !parsed_alias->skin) return 46;
        const auto& alias_joints = parsed_alias->skin->joints;
        if (std::find(alias_joints.begin(), alias_joints.end(), "mPelvis") == alias_joints.end())
            return 47;
        // The alias is resolved on the way in, so the canonical name is what the
        // asset carries and "hip" appears nowhere in it.
        if (std::find(alias_joints.begin(), alias_joints.end(), "hip") != alias_joints.end())
            return 48;

        // A joint from another skeleton is refused by name, not silently
        // dropped or mapped to something nearby.
        std::string foreign_json = rigged_json;
        const auto foreign_at = foreign_json.find(R"({"name":"mTorso"})");
        foreign_json.replace(foreign_at, std::string(R"({"name":"mTorso"})").size(),
                             R"({"name":"CC_Base_Spine"})");
        const auto foreign = homeworldz::mesh::convert_glb(glb(foreign_json, bin));
        if (foreign.ok || foreign.error.find("CC_Base_Spine") == std::string::npos) return 34;
    }

    return 0;
}
