#include "homeworldz/mesh_acceptance.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Builds a real GLB container around the given glTF JSON and binary chunk —
// the validator must be exercised against actual container bytes, not stubs.
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

// One triangle: three float3 positions in the binary chunk.
const char* const triangle_json_head =
    R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":36}],)"
    R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],)"
    R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",)"
    R"("min":[0,0,0],"max":[1,1,0]}],)"
    R"("meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],)"
    R"("nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0)";

std::vector<std::uint8_t> triangle_bin() {
    const float positions[9] = {0, 0, 0, 1, 0, 0, 1, 1, 0};
    std::vector<std::uint8_t> bin(sizeof positions);
    std::memcpy(bin.data(), positions, sizeof positions);
    return bin;
}

} // namespace

int main() {
    using homeworldz::mesh::validate_glb;

    // A minimal valid GLB is accepted, with its triangle counted.
    const auto valid = glb(std::string(triangle_json_head) + "}", triangle_bin());
    const auto accepted = validate_glb(valid);
    if (!accepted.accepted || accepted.triangles != 1 || accepted.materials != 0)
        return 1;

    // Not a GLB at all, and truncated garbage, are refused without parsing.
    const std::string text = "not a mesh";
    if (validate_glb(std::span(reinterpret_cast<const std::byte*>(text.data()),
                               text.size())).accepted)
        return 2;

    // An unknown extension is refused, not ignored — used or required.
    const auto unknown_used = glb(std::string(triangle_json_head) +
        R"(,"extensionsUsed":["EXT_meshopt_compression"]})", triangle_bin());
    const auto refused_used = validate_glb(unknown_used);
    if (refused_used.accepted ||
        refused_used.reason.find("EXT_meshopt_compression") == std::string::npos)
        return 3;
    const auto draco = glb(std::string(triangle_json_head) +
        R"(,"extensionsUsed":["KHR_draco_mesh_compression"],)"
        R"("extensionsRequired":["KHR_draco_mesh_compression"]})", triangle_bin());
    if (validate_glb(draco).accepted) return 4;

    // A morph target sitting at zero is harmless: the base geometry is the
    // intended default and is exactly what the converter emits. A non-zero
    // default weight is not, because the shape served would be the unmorphed
    // base - a different mesh, silently. Two Library "bodies" turned out to be
    // one neutral mesh with the gender in a morph weight (2026-08-05), which is
    // what this refuses.
    {
        const char* const head =
            R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":36}],)"
            R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],)"
            R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",)"
            R"("min":[0,0,0],"max":[1,1,0]}],)";
        const char* const tail =
            R"("nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
        const auto morphed = glb(std::string(head) +
            R"("meshes":[{"weights":[0.5],"primitives":[{"attributes":{"POSITION":0},)"
            R"("targets":[{"POSITION":0}]}]}],)" + tail, triangle_bin());
        const auto refused_morph = validate_glb(morphed);
        if (refused_morph.accepted ||
            refused_morph.reason.find("morph") == std::string::npos)
            return 20;
        // And zero weights pass, so the gate is a rule about intent rather than
        // a ban on blend shapes.
        const auto at_zero = glb(std::string(head) +
            R"("meshes":[{"weights":[0.0],"primitives":[{"attributes":{"POSITION":0},)"
            R"("targets":[{"POSITION":0}]}]}],)" + tail, triangle_bin());
        if (!validate_glb(at_zero).accepted) return 21;
    }

    // An allowlisted extension passes.
    const auto allowed = glb(std::string(triangle_json_head) +
        R"(,"extensionsUsed":["KHR_materials_unlit"]})", triangle_bin());
    if (!validate_glb(allowed).accepted) return 5;

    // External buffer URIs break self-containment and are refused.
    const auto external = glb(
        R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":36,"uri":"http://example.com/x.bin"}],)"
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",)"
        R"("min":[0,0,0],"max":[1,1,0]}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],)"
        R"("nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})", {});
    const auto refused_external = validate_glb(external);
    if (refused_external.accepted ||
        refused_external.reason.find("external") == std::string::npos)
        return 6;

    // A rig whose joints are all real is refused only because rigged mesh is
    // not accepted yet. The joint node is named on purpose: an unnamed one is
    // refused earlier now, so this case must carry a real joint to reach the
    // M4 gate at all.
    const char* const rig_head =
        R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":36}],)"
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",)"
        R"("min":[0,0,0],"max":[1,1,0]}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],)"
        R"("scenes":[{"nodes":[0]}],"scene":0)";
    const auto rigged = glb(std::string(rig_head) +
        R"(,"nodes":[{"mesh":0},{"name":"mPelvis"}],"skins":[{"joints":[1]}]})",
        triangle_bin());
    // Accepted since 2026-08-08: rigged mesh is no longer refused outright, and
    // this skin's single joint is a real Bento name that no vertex moves, so
    // there is nothing left to object to.
    //
    // This assertion previously required a refusal and was correct until the
    // flag flipped. It then failed on every build for a day without anyone
    // seeing it, because this executable had no add_test and ctest never ran it
    // - which is why the CMakeLists now carries a note about that.
    const auto rigged_result = validate_glb(rigged);
    if (!rigged_result.accepted) return 7;

    // A joint that is not on the skeleton is refused by name. Whoever hears
    // this may be several tools away from the file, so the reason identifies
    // which joint rather than only that one is wrong.
    const auto bad_joint = glb(std::string(rig_head) +
        R"(,"nodes":[{"mesh":0},{"name":"Bip01_Spine"}],"skins":[{"joints":[1]}]})",
        triangle_bin());
    const auto refused_joint = validate_glb(bad_joint);
    if (refused_joint.accepted ||
        refused_joint.reason.find("Bip01_Spine") == std::string::npos)
        return 9;

    // The same bytes, arriving as an import, are accepted with the skeleton
    // recorded (ADR 0035). This is the *only* rule that differs between the two
    // origins, and it differs because the question differs: an upload chose to
    // send a rig claiming to be ours, while an import carries whatever skeleton
    // its author used and its geometry is useful before anyone can wear it.
    //
    // Both directions are asserted. Accepting the import proves the exemption
    // exists; the refusal above proves it did not leak into the upload path,
    // which is what would turn import into the side door ADR 0035 forbids.
    const auto imported_joint =
        validate_glb(bad_joint, homeworldz::mesh::Origin::Import);
    if (!imported_joint.accepted) return 60;
    if (imported_joint.unresolved_joints.size() != 1) return 61;
    if (imported_joint.unresolved_joints.front() != "Bip01_Spine") return 62;
    // A rig that *does* resolve leaves the list empty, so a caller can read
    // "not wearable" off it without asking a second question.
    if (!validate_glb(rigged, homeworldz::mesh::Origin::Import)
             .unresolved_joints.empty())
        return 63;
    // Everything else the gate protects still applies to an import, or import
    // is the side door into the asset store that ADR 0035 forbids. The
    // extension allowlist stands in for the rest: it is refused on a file that
    // is otherwise entirely valid, so the refusal cannot be a structural
    // accident, and the reason is asserted rather than only the verdict — a
    // test that checks "refused" alone passes just as happily when the fixture
    // is malformed.
    {
        const auto extended = glb(std::string(rig_head) +
            R"(,"extensionsUsed":["KHR_draco_mesh_compression"])"
            R"(,"nodes":[{"mesh":0},{"name":"mPelvis"}],"skins":[{"joints":[1]}]})",
            triangle_bin());
        const auto as_upload = validate_glb(extended, homeworldz::mesh::Origin::Upload);
        if (as_upload.accepted ||
            as_upload.reason.find("KHR_draco_mesh_compression") == std::string::npos)
            return 64;
        const auto as_import = validate_glb(extended, homeworldz::mesh::Origin::Import);
        if (as_import.accepted ||
            as_import.reason.find("KHR_draco_mesh_compression") == std::string::npos)
            return 65;
    }

    // An alias reaches the M4 gate rather than the joint check. The viewer
    // resolves `hip` onto mPelvis, and refusing it would reject what Blender
    // and Avastar emit while the published policy claimed compatibility.
    const auto aliased = glb(std::string(rig_head) +
        R"(,"nodes":[{"mesh":0},{"name":"hip"}],"skins":[{"joints":[1]}]})",
        triangle_bin());
    // Accepted, since rigged mesh is no longer refused outright: the point that
    // survives is that `hip` resolves rather than being rejected as an unknown
    // joint, which is what the published alias compatibility promises.
    const auto alias_result = validate_glb(aliased);
    if (!alias_result.accepted) return 10;

    // An unnamed joint cannot be resolved at all, so it is refused before the
    // name check has a name to report.
    const auto unnamed = glb(std::string(triangle_json_head) +
        R"(,"skins":[{"joints":[0]}]})", triangle_bin());
    const auto refused_unnamed = validate_glb(unnamed);
    if (refused_unnamed.accepted ||
        refused_unnamed.reason.find("unnamed joint") == std::string::npos)
        return 11;

    // A fifth influence arrives as JOINTS_1: glTF numbers joint and weight sets
    // four to a set, so any index above zero exceeds the limit by definition.
    // Sized so every accessor is readable. The original fixture pointed a
    // 48-byte VEC4 float accessor at a 36-byte view, so validation refused it as
    // unreadable before ever reaching the influence check — the assertion below
    // passed on the wrong refusal, and nobody saw it because this executable was
    // never registered with ctest.
    std::vector<std::uint8_t> influence_bin(108, 0);
    {
        const float positions[9] = {0, 0, 0, 1, 0, 0, 1, 1, 0};
        std::memcpy(influence_bin.data(), positions, sizeof positions);
        const std::uint16_t joints[12] = {};
        std::memcpy(influence_bin.data() + 36, joints, sizeof joints);
        float weights[12] = {};
        for (int vertex = 0; vertex < 3; ++vertex) weights[vertex * 4] = 1.0F;
        std::memcpy(influence_bin.data() + 60, weights, sizeof weights);
    }
    const auto five_influences = glb(
        R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":108}],)"
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},)"
        R"({"buffer":0,"byteOffset":36,"byteLength":24},)"
        R"({"buffer":0,"byteOffset":60,"byteLength":48}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",)"
        R"("min":[0,0,0],"max":[1,1,0]},)"
        R"({"bufferView":1,"componentType":5123,"count":3,"type":"VEC4"},)"
        R"({"bufferView":2,"componentType":5126,"count":3,"type":"VEC4"}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"JOINTS_0":1,)"
        R"("WEIGHTS_0":2,"JOINTS_1":1,"WEIGHTS_1":2}}]}],)"
        R"("nodes":[{"mesh":0,"skin":0},{"name":"mPelvis"}],"skins":[{"joints":[1]}],)"
        R"("scenes":[{"nodes":[0]}],"scene":0})", influence_bin);
    const auto refused_influences = validate_glb(five_influences);
    if (refused_influences.accepted ||
        refused_influences.reason.find("influences") == std::string::npos)
        return 12;

    // The published policy carries the same numbers the validator enforces.
    const auto policy = homeworldz::mesh::acceptance_policy_json();
    if (policy.find("\"uploadPath\":\"/session/uploads/mesh\"") == std::string::npos ||
        policy.find("\"maxTriangles\":262144") == std::string::npos ||
        policy.find("\"maxRigInfluences\":4") == std::string::npos ||
        // A rig limit published beside "rigged": false reads as a contradiction
        // unless the payload says which limits are not yet in force. It is named
        // rather than removed, because an importer should read the number instead
        // of encoding its own (client core, 2026-08-04).
        // The skeleton a re-rig must target, named because glTF binds joints by
        // node index rather than by name: a client drawing arbitrary skeletons is
        // unconstrained, a viewer uses its own and no other, so one body rigged to
        // these names serves both families (client core, 2026-08-04).
        policy.find("\"skeleton\":\"bento-avatar\"") == std::string::npos ||
        // 159, not 71: avatar_skeleton.xml defines 133 bones and 26 collision
        // volumes, and the viewer resolves a rig joint name against both. The
        // old figure described the pre-Bento skeleton and would have sent a
        // re-rig at a target half the real size.
        policy.find("\"skeletonJoints\":159") == std::string::npos ||
        policy.find("\"maxJointsPerMesh\":110") == std::string::npos ||
        // Empty since rigged acceptance was turned on 2026-08-08: the four rig
        // keys left forwardLooking together, because the array is one ternary on
        // rigged_accepted rather than four independent entries. A client reading
        // it stops hedging on those limits the moment they are enforced.
        policy.find("\"forwardLooking\":[]") == std::string::npos ||
        policy.find("\"draco\":false") == std::string::npos ||
        policy.find("\"rigged\":true") == std::string::npos ||
        policy.find("KHR_texture_transform") == std::string::npos)
        return 8;
    // The geometric check, and the distinction that decides whether it is
    // usable. Both files below carry identity inverse bind matrices, which put
    // every joint at the origin - nowhere near where the skeleton rests
    // mPelvis - so the only difference between them is whether a vertex moves
    // that joint.
    {
        // positions (36) + joints (24) + weights-one (48) + weights-zero (48)
        std::vector<std::uint8_t> bin(156, 0);
        const float positions[9] = {0, 0, 0, 1, 0, 0, 1, 1, 0};
        std::memcpy(bin.data(), positions, sizeof positions);
        const std::uint16_t joints[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        std::memcpy(bin.data() + 36, joints, sizeof joints);
        float weights_one[12] = {};
        for (int vertex = 0; vertex < 3; ++vertex) weights_one[vertex * 4] = 1.0F;
        std::memcpy(bin.data() + 60, weights_one, sizeof weights_one);
        // bytes 108..155 stay zero: the same joint, moving nothing.

        const auto skinned = [&](int weights_view) {
            return glb(
                R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":156}],)"
                R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},)"
                R"({"buffer":0,"byteOffset":36,"byteLength":24},)"
                R"({"buffer":0,"byteOffset":60,"byteLength":48},)"
                R"({"buffer":0,"byteOffset":108,"byteLength":48}],)"
                R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",)"
                R"("min":[0,0,0],"max":[1,1,0]},)"
                R"({"bufferView":1,"componentType":5123,"count":3,"type":"VEC4"},)"
                R"({"bufferView":)" + std::to_string(weights_view) +
                R"(,"componentType":5126,"count":3,"type":"VEC4"}],)"
                R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"JOINTS_0":1,)"
                R"("WEIGHTS_0":2}}]}],)"
                R"("nodes":[{"mesh":0,"skin":0},{"name":"mPelvis"}],)"
                R"("skins":[{"joints":[1]}],)"
                R"("scenes":[{"nodes":[0]}],"scene":0})", bin);
        };

        // Moved, and in the wrong place: refused, naming the skeleton.
        const auto misplaced = validate_glb(skinned(2));
        if (misplaced.accepted ||
            misplaced.reason.find("do not stand where") == std::string::npos)
            return 22;

        // Declared but moved by nothing: skipped, and the file is accepted.
        //
        // This is the regression that matters. A skin declares every armature
        // bone whatever the mesh touches, and an unused joint's bind matrix is
        // whatever the exporter wrote - the Second Life reference body has two
        // sitting 11 mm out, moving nothing. Checking the declared list refused
        // that body for joints it does not use, which is the same
        // declared-versus-used error the per-mesh budget exists to avoid, made
        // one field over and caught only because a real body was to hand.
        if (!validate_glb(skinned(3)).accepted) return 23;
    }

    return 0;
}
