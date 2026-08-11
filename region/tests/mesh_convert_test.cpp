#include "homeworldz/avatar_joints.h"
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

        // Emitted inverse binds must be axis-aligned, because every joint in
        // avatar_skeleton.xml is: rot="0 0 0" throughout. An exporter's bone
        // orientations describe a skeleton the viewer does not have, and
        // carrying them through rotates the geometry about foreign axes as soon
        // as it is skinned — the reference body's whole arm chain came out a
        // quarter turn off while measuring correctly, because a position falls
        // out of the inversion whatever the rotation was.
        //
        // The fixture's matrices are identity, so this cannot catch a converter
        // that merely passes them through; the rotated case below is what tests
        // the discarding.
        for (const auto& matrix : parsed_rig->skin->inverse_bind)
            for (int row = 0; row < 3; ++row)
                for (int column = 0; column < 3; ++column)
                    if (std::fabs(matrix[column * 4 + row] - (row == column ? 1.0f : 0.0f)) > 1e-4f)
                        return 50;

        // A joint whose bind matrix carries a real rotation: a quarter turn,
        // which is the shape the arm chain arrived in. The rotation must be
        // discarded and the position it implies kept.
        {
            std::vector<std::uint8_t> turned_bin = bin;
            // The second joint's inverse bind, at byte 108 + 64: a rotation of
            // 90 degrees about x with a translation, column-major.
            const float turned[16] = {1, 0, 0, 0,
                                      0, 0, 1, 0,
                                      0, -1, 0, 0,
                                      0.25f, 0.5f, 0.75f, 1};
            std::memcpy(turned_bin.data() + 108 + 64, turned, sizeof turned);
            const auto rotated = homeworldz::mesh::convert_glb(glb(rigged_json, turned_bin));
            if (!rotated.ok) return 51;
            const auto parsed_rotated = homeworldz::slmesh::parse(rotated.sl_mesh);
            if (!parsed_rotated || !parsed_rotated->skin) return 52;
            const auto& matrices = parsed_rotated->skin->inverse_bind;
            if (matrices.size() < 2) return 53;
            for (const auto& matrix : matrices)
                for (int row = 0; row < 3; ++row)
                    for (int column = 0; column < 3; ++column)
                        if (std::fabs(matrix[column * 4 + row] -
                                      (row == column ? 1.0f : 0.0f)) > 1e-4f)
                            return 54;
            // And the joint still rests where the rotated matrix put it. The
            // turned matrix implies a rest at -(R^-1 t) = (-0.25, -0.75, 0.5) in
            // glTF, which on the region's axes ((x,y,z) <- (z,x,y)) is
            // (0.5, -0.25, -0.75). The emitted matrix stores its negation.
            const std::array<float, 3> rest{
                -matrices[1][12], -matrices[1][13], -matrices[1][14]};
            const std::array<float, 3> expected{0.5f, -0.25f, -0.75f};
            for (int axis = 0; axis < 3; ++axis)
                if (std::fabs(rest[axis] - expected[axis]) > 1e-3f) return 55;
        }

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

        // A joint from another skeleton keeps its geometry and loses its rig.
        //
        // This asserted a *refusal* until 2026-08-11, which was wrong in both
        // directions: the asset produced no rendition and so drew nothing, while
        // still colliding, because physics builds shapes from the prim and never
        // reads the mesh. An invisible Tyrannosaurus is what that looks like from
        // in-world. The gate had already decided the policy for an import — an
        // unrecognised skeleton is "a question rather than an offence" — and only
        // the rig is unusable, never the geometry. So: converts, carries no skin,
        // and names the joint so the creator can be told.
        std::string foreign_json = rigged_json;
        const auto foreign_at = foreign_json.find(R"({"name":"mTorso"})");
        foreign_json.replace(foreign_at, std::string(R"({"name":"mTorso"})").size(),
                             R"({"name":"CC_Base_Spine"})");
        const auto foreign = homeworldz::mesh::convert_glb(glb(foreign_json, bin));
        if (!foreign.ok) return 34;
        if (foreign.unmapped_joints.size() != 1 ||
            foreign.unmapped_joints[0] != "CC_Base_Spine")
            return 35;
        const auto parsed_foreign = homeworldz::slmesh::parse(foreign.sl_mesh);
        if (!parsed_foreign) return 36;
        // No skin at all, rather than a partial one: a rig missing a joint its
        // vertices are weighted to would skin them to nothing, and the viewer
        // collapses such a vertex to the object origin — a spike from the surface
        // to a point.
        if (parsed_foreign->skin) return 37;
        if (parsed_foreign->high.empty() || parsed_foreign->high.front().positions.empty())
            return 38;
        // A mesh that maps cleanly still keeps its rig, so this did not simply
        // stop rigging everything.
        if (!rigged.unmapped_joints.empty()) return 39;
    }

    // One part of a multi-part import, which is how a body actually arrives: a
    // Character Creator export is one mesh per GLB, so the mesh carrying the
    // tongue binds two joints and knows the rest of the skeleton only from the
    // node tree the importer emits alongside it.
    //
    // The override for such a joint must be measured against where *this rig*
    // puts its Bento parent, not where Linden rests it, because at runtime the
    // parent is placed by whichever worn mesh binds it. Measuring against Linden
    // put Caleb's teeth 31 mm through his chin and his tongue 39 mm, and
    // Ariana's tongue 115 mm below her jaw — none of it visible offline, because
    // each mesh was self-consistent and only the set was wrong (2026-08-10).
    //
    // The parent here is deliberately *not* an ancestor: Character Creator hangs
    // the tongue off the jaw where Bento hangs it off the lower teeth, so a
    // converter that walked only its own ancestry would still miss it.
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
        for (const auto& corner : {std::array<float, 3>{0, 0, 0},
                                   std::array<float, 3>{1, 0, 0},
                                   std::array<float, 3>{0, 1, 0}})
            for (const auto value : corner) put_float(value);
        for (int vertex = 0; vertex < 3; ++vertex) {
            put_ushort(0); put_ushort(0); put_ushort(0); put_ushort(0);
        }
        for (int vertex = 0; vertex < 3; ++vertex) {
            put_float(1.0f); put_float(0.0f); put_float(0.0f); put_float(0.0f);
        }
        // Positions in glTF axes, which the region reads as (x,y,z) <- (z,x,y).
        // The tongue bone rests at region (0.09, 0, 1.60) and the lower tooth it
        // must be measured against at (0.06, 0, 1.58) — the tooth bound by
        // another mesh of the same import, present here only as a node.
        const auto identity_with = [&](float x, float y, float z) {
            const float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1};
            std::string out = "[";
            for (int cell = 0; cell < 16; ++cell) {
                if (cell != 0) out += ',';
                char buffer[32];
                std::snprintf(buffer, sizeof buffer, "%g", m[cell]);
                out += buffer;
            }
            return out + "]";
        };
        // inverse(world) for the bound joint, since the geometry is in world
        // space: a translation to the negated rest.
        for (const auto value : {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f,
                                 0.f, -1.60f, -0.09f, 1.f})
            put_float(value);

        const std::string part_json =
            R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":172}],)"
            R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},)"
            R"({"buffer":0,"byteOffset":36,"byteLength":24},)"
            R"({"buffer":0,"byteOffset":60,"byteLength":48},)"
            R"({"buffer":0,"byteOffset":108,"byteLength":64}],)"
            R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",)"
            R"("min":[0,0,0],"max":[1,1,0]},)"
            R"({"bufferView":1,"componentType":5123,"count":3,"type":"VEC4"},)"
            R"({"bufferView":2,"componentType":5126,"count":3,"type":"VEC4"},)"
            R"({"bufferView":3,"componentType":5126,"count":1,"type":"MAT4"}],)"
            R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"JOINTS_0":1,"WEIGHTS_0":2}}]}],)"
            R"("nodes":[{"mesh":0,"skin":0},)"
            R"({"name":"CC_Base_Tongue01","matrix":)" + identity_with(0, 1.60f, 0.09f) + "},"
            R"({"name":"CC_Base_Teeth02","matrix":)" + identity_with(0, 1.58f, 0.06f) + "}],"
            R"("skins":[{"joints":[1],"inverseBindMatrices":3}],)"
            R"("scenes":[{"nodes":[0,1,2]}],"scene":0})";
        const auto part = homeworldz::mesh::convert_glb(glb(part_json, bin));
        if (!part.ok) return 60;
        const auto parsed = homeworldz::slmesh::parse(part.sl_mesh);
        if (!parsed || !parsed->skin) return 61;
        if (parsed->skin->joints.size() != 1) return 62;
        if (parsed->skin->joints[0] != "mFaceTongueBase") return 63;
        if (parsed->skin->alternate_inverse_bind.size() != 1) return 64;
        // Region axes throughout: (0.09, 0, 1.60) - (0.06, 0, 1.58).
        const std::array<float, 3> expected_override{0.03f, 0.0f, 0.02f};
        for (int axis = 0; axis < 3; ++axis)
            if (std::fabs(parsed->skin->alternate_inverse_bind[0][12 + axis] -
                          expected_override[axis]) > 1e-3f)
                return 65;
        const std::array<float, 3> expected_rest{-0.09f, 0.0f, -1.60f};
        for (int axis = 0; axis < 3; ++axis)
            if (std::fabs(parsed->skin->inverse_bind[0][12 + axis] - expected_rest[axis]) > 1e-3f)
                return 66;
    }

    // A bone folded onto a joint by ancestry, which is how an accessory arrives:
    // Character Creator names an earring's bone after the earring, so only its
    // ancestry says it hangs off the head.
    //
    // Such a bone must not answer for the joint's position — it is not the joint
    // under another name, it is something hanging off it. Ariana's earring rests
    // at her character origin, and answering put mHead at z=0.254 against the
    // body's 1.634: an accessory quietly competing to move the wearer's head
    // 1.43 m down (measured 2026-08-10).
    //
    // Both matrices must therefore frame the *head*: the override sends mHead
    // where this rig puts it, and the inverse bind frames the geometry on that
    // same point, so the earring rides the head instead of the head chasing the
    // earring.
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
        for (const auto& corner : {std::array<float, 3>{0, 0, 0},
                                   std::array<float, 3>{1, 0, 0},
                                   std::array<float, 3>{0, 1, 0}})
            for (const auto value : corner) put_float(value);
        for (int vertex = 0; vertex < 3; ++vertex) {
            put_ushort(0); put_ushort(0); put_ushort(0); put_ushort(0);
        }
        for (int vertex = 0; vertex < 3; ++vertex) {
            put_float(1.0f); put_float(0.0f); put_float(0.0f); put_float(0.0f);
        }
        // The head rests at region (-0.02, 0, 1.66); the earring bone hangs off
        // it at region (0.10, 0.08, 1.55), and its inverse bind is that.
        for (const auto value : {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f,
                                 -0.08f, -1.55f, -0.10f, 1.f})
            put_float(value);

        const std::string worn_json =
            R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":172}],)"
            R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},)"
            R"({"buffer":0,"byteOffset":36,"byteLength":24},)"
            R"({"buffer":0,"byteOffset":60,"byteLength":48},)"
            R"({"buffer":0,"byteOffset":108,"byteLength":64}],)"
            R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",)"
            R"("min":[0,0,0],"max":[1,1,0]},)"
            R"({"bufferView":1,"componentType":5123,"count":3,"type":"VEC4"},)"
            R"({"bufferView":2,"componentType":5126,"count":3,"type":"VEC4"},)"
            R"({"bufferView":3,"componentType":5126,"count":1,"type":"MAT4"}],)"
            R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"JOINTS_0":1,"WEIGHTS_0":2}}]}],)"
            R"("nodes":[{"mesh":0,"skin":0},)"
            // Local to the head, so the earring's world translation is the sum.
            R"({"name":"Earring_Flower_0","matrix":[1,0,0,0,0,1,0,0,0,0,1,0,0.08,-0.11,0.12,1]},)"
            R"({"name":"CC_Base_Head","matrix":[1,0,0,0,0,1,0,0,0,0,1,0,0,1.66,-0.02,1],)"
            R"("children":[1]}],)"
            R"("skins":[{"joints":[1],"inverseBindMatrices":3}],)"
            R"("scenes":[{"nodes":[0,2]}],"scene":0})";
        const auto worn = homeworldz::mesh::convert_glb(glb(worn_json, bin));
        if (!worn.ok) return 70;
        const auto parsed = homeworldz::slmesh::parse(worn.sl_mesh);
        if (!parsed || !parsed->skin) return 71;
        if (parsed->skin->joints.size() != 1) return 72;
        if (parsed->skin->joints[0] != "mHead") return 73;
        if (parsed->skin->alternate_inverse_bind.size() != 1) return 74;
        // mHead's parent is mNeck, which nothing here places, so it keeps the
        // skeleton's own position — asked of the skeleton rather than copied out
        // of it, since a number transcribed into a test is a number that can
        // stop matching.
        std::array<float, 3> neck{};
        if (!homeworldz::mesh::joint_rest("mNeck", neck[0], neck[1], neck[2])) return 75;
        const std::array<float, 3> head{-0.02f, 0.0f, 1.66f};
        for (int axis = 0; axis < 3; ++axis)
            if (std::fabs(parsed->skin->alternate_inverse_bind[0][12 + axis] -
                          (head[axis] - neck[axis])) > 1e-3f)
                return 76;
        // The head, not the earring: an inverse bind of -(0.10, 0.08, 1.55)
        // would send the geometry 1.38 m up the moment the head stopped
        // following it.
        for (int axis = 0; axis < 3; ++axis)
            if (std::fabs(parsed->skin->inverse_bind[0][12 + axis] + head[axis]) > 1e-3f)
                return 77;
    }

    // A GLB whose node tree is *not* the rest pose, which glTF permits: the
    // inverse bind matrices alone determine skinning, so an asset may leave every
    // joint node at identity and be perfectly correct. Reading joint positions
    // out of that tree would place every unbound joint at the origin and hand its
    // children metre-scale overrides.
    //
    // So the tree is consulted only where it agrees with the matrices that do
    // decide the bind pose, and this asset must convert as though it had no tree:
    // mChest's parent chain is unplaced, so the override is measured against the
    // skeleton's own mSpine4 — not against an mChest node sitting at the origin.
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
        for (const auto& corner : {std::array<float, 3>{0, 0, 0},
                                   std::array<float, 3>{1, 0, 0},
                                   std::array<float, 3>{0, 1, 0}})
            for (const auto value : corner) put_float(value);
        for (int vertex = 0; vertex < 3; ++vertex) {
            put_ushort(0); put_ushort(0); put_ushort(0); put_ushort(0);
        }
        for (int vertex = 0; vertex < 3; ++vertex) {
            put_float(1.0f); put_float(0.0f); put_float(0.0f); put_float(0.0f);
        }
        // mChest resting at region (0, 0, 1.20): glTF y is the region's z.
        for (const auto value : {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f,
                                 0.f, -1.20f, 0.f, 1.f})
            put_float(value);

        const std::string flat_json =
            R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":172}],)"
            R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},)"
            R"({"buffer":0,"byteOffset":36,"byteLength":24},)"
            R"({"buffer":0,"byteOffset":60,"byteLength":48},)"
            R"({"buffer":0,"byteOffset":108,"byteLength":64}],)"
            R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",)"
            R"("min":[0,0,0],"max":[1,1,0]},)"
            R"({"bufferView":1,"componentType":5123,"count":3,"type":"VEC4"},)"
            R"({"bufferView":2,"componentType":5126,"count":3,"type":"VEC4"},)"
            R"({"bufferView":3,"componentType":5126,"count":1,"type":"MAT4"}],)"
            R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"JOINTS_0":1,"WEIGHTS_0":2}}]}],)"
            // No transforms anywhere: both nodes sit at the origin.
            R"("nodes":[{"mesh":0,"skin":0},{"name":"mChest"},{"name":"mTorso"}],)"
            R"("skins":[{"joints":[1],"inverseBindMatrices":3}],)"
            R"("scenes":[{"nodes":[0,1,2]}],"scene":0})";
        const auto flat = homeworldz::mesh::convert_glb(glb(flat_json, bin));
        if (!flat.ok) return 80;
        const auto parsed = homeworldz::slmesh::parse(flat.sl_mesh);
        if (!parsed || !parsed->skin) return 81;
        if (parsed->skin->joints.size() != 1 || parsed->skin->joints[0] != "mChest") return 82;
        if (parsed->skin->alternate_inverse_bind.size() != 1) return 83;
        std::array<float, 3> spine4{};
        if (!homeworldz::mesh::joint_rest("mSpine4", spine4[0], spine4[1], spine4[2])) return 84;
        const std::array<float, 3> chest{0.0f, 0.0f, 1.20f};
        for (int axis = 0; axis < 3; ++axis)
            if (std::fabs(parsed->skin->alternate_inverse_bind[0][12 + axis] -
                          (chest[axis] - spine4[axis])) > 1e-3f)
                return 85;
    }

    return 0;
}
