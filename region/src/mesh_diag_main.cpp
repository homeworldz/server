// What the upload gate says about a GLB, and what conversion makes of it,
// without uploading anything.
//
// The acceptance policy is published so creators can check their own content
// (ADR 0033), but a published document still leaves them running the file
// against a real region to find out. This answers the same questions offline
// through the identical code paths — not a reimplementation of the rules,
// which would be a second copy able to disagree with the first.
#include "homeworldz/mesh_acceptance.h"
#include "homeworldz/mesh_convert.h"
#include "homeworldz/rig_check.h"
#include "homeworldz/slmesh.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: homeworldz-mesh-diag <file.glb> [more.glb ...]\n";
        return 2;
    }
    int refused = 0;
    for (int index = 1; index < argc; ++index) {
        std::ifstream input(argv[index], std::ios::binary);
        if (!input) {
            std::cout << argv[index] << ": cannot be opened\n";
            refused = 1;
            continue;
        }
        const std::vector<char> raw{std::istreambuf_iterator<char>(input),
                                    std::istreambuf_iterator<char>()};
        std::vector<std::byte> bytes(raw.size());
        for (std::size_t at = 0; at < raw.size(); ++at)
            bytes[at] = static_cast<std::byte>(raw[at]);

        const auto result = homeworldz::mesh::validate_glb(bytes);
        std::cout << argv[index] << ": " << (result.accepted ? "accepted" : "REFUSED");
        if (!result.accepted) {
            std::cout << " - " << result.reason;
            refused = 1;
        } else {
            std::cout << " (" << result.triangles << " triangles, " << result.materials
                      << " materials, " << result.textures << " textures)";
        }
        std::cout << '\n';

        // Acceptance and conversion are different claims and a creator wants
        // both. A file can satisfy every published limit and still fail to
        // convert: the gate reads structure, the converter reads every vertex.
        const auto converted = homeworldz::mesh::convert_glb(bytes);
        std::cout << "  convert: " << (converted.ok ? "ok" : "FAILED");
        if (!converted.ok) {
            std::cout << " - " << converted.error;
            refused = 1;
            std::cout << '\n';
            continue;
        }
        std::cout << " (" << converted.faces << " face(s), " << converted.high_triangles
                  << " triangles, " << converted.sl_mesh.size() << " bytes)\n";

        // Read the asset back rather than trust what was written. The joint
        // table is the one part of a rig nothing downstream can check: a wrong
        // mapping given its target's inverse bind matrix produces a correct
        // bind pose, so no render disagrees with it.
        const auto parsed = homeworldz::slmesh::parse(converted.sl_mesh);
        if (!parsed) {
            std::cout << "  skin: the converted asset does not parse back\n";
            refused = 1;
            continue;
        }
        if (!parsed->skin) continue;
        // Joint position overrides, which are what let a body keep its own
        // proportions instead of taking Linden's. Reported because their
        // absence is invisible otherwise — the asset parses, renders and
        // animates either way, and only the shape is wrong.
        //
        // The count matters as much as the presence: Firestorm ignores *every*
        // override unless there is exactly one per joint
        // (llvoavatar.cpp, addAttachmentOverridesForObject), so a partial table
        // is silently no table at all.
        // The two transforms a rigged mesh is skinned through, and the vertex
        // range they act on. A rigged mesh is not scaled by its prim, so the
        // normalization the converter applies has to be carried back out by
        // bind_shape; if these disagree the body still parses and still reports
        // sane joints, and explodes the moment anything skins it.
        const auto& shape = parsed->skin->bind_shape;
        std::cout << "  bind shape: scale (" << shape[0] << ", " << shape[5] << ", " << shape[10]
                  << ") offset (" << shape[12] << ", " << shape[13] << ", " << shape[14] << ")\n";
        if (!parsed->high.empty() && !parsed->high[0].positions.empty()) {
            std::array<float, 3> low = parsed->high[0].positions[0];
            std::array<float, 3> high = low;
            for (const auto& submesh : parsed->high)
                for (const auto& position : submesh.positions)
                    for (int axis = 0; axis < 3; ++axis) {
                        low[axis] = (std::min)(low[axis], position[axis]);
                        high[axis] = (std::max)(high[axis], position[axis]);
                    }
            std::cout << "  vertices:   x " << low[0] << ".." << high[0] << "  y " << low[1] << ".."
                      << high[1] << "  z " << low[2] << ".." << high[2] << '\n';
        }
        // Vertices carrying no usable influence. Retargeting drops influences
        // whose source joint has no Bento correspondence, and a vertex weighted
        // *only* to dropped joints keeps its position but loses every joint. The
        // asset still parses, still reports a sane joint list and still passes
        // the rig check; the viewer skins that vertex to nothing and collapses
        // it to the object origin, which draws as a spike from the body surface
        // to a point. Counted because no other check here can see it.
        std::size_t unweighted = 0, total_vertices = 0;
        for (const auto& submesh : parsed->high) {
            total_vertices += submesh.positions.size();
            for (std::size_t at = 0; at < submesh.positions.size(); ++at) {
                float carried = 0.0f;
                if (at < submesh.influences.size())
                    for (const auto& influence : submesh.influences[at]) carried += influence.weight;
                if (carried <= 0.0f) ++unweighted;
            }
        }
        if (unweighted > 0) {
            std::cout << "  UNWEIGHTED: " << unweighted << " of " << total_vertices
                      << " vertices carry no joint - each collapses to the origin\n";
            refused = 1;
        }
        const auto overrides = parsed->skin->alternate_inverse_bind.size();
        std::cout << "  overrides: ";
        if (overrides == 0)
            std::cout << "none - this body will take the skeleton's own proportions\n";
        else if (overrides != parsed->skin->joints.size())
            std::cout << overrides << " for " << parsed->skin->joints.size()
                      << " joints - MISMATCHED, so a viewer discards all of them\n";
        else {
            // The largest override, which says which convention these are in.
            // A joint position override is relative to its parent, so a body's
            // biggest is a limb segment - tens of centimetres. A figure near
            // the joint's height above the ground means world positions got
            // written, and every joint will inherit its ancestors' error.
            float worst = 0.0f;
            std::string worst_joint;
            for (std::size_t at = 0; at < overrides; ++at) {
                const auto& m = parsed->skin->alternate_inverse_bind[at];
                const auto length = std::sqrt(m[12] * m[12] + m[13] * m[13] + m[14] * m[14]);
                if (length > worst) {
                    worst = length;
                    worst_joint = parsed->skin->joints[at];
                }
            }
            std::cout << overrides << ", one per joint; largest " << worst * 1000.0f << " mm ("
                      << worst_joint << ")\n";
        }
        std::cout << "  skin: " << parsed->skin->joints.size() << " joint(s):";
        for (std::size_t at = 0; at < parsed->skin->joints.size(); ++at) {
            if (at == 12) {
                std::cout << " ...";
                break;
            }
            std::cout << ' ' << parsed->skin->joints[at];
        }
        std::cout << '\n';
        // The joint *names* survived the round trip; whether the skeleton those
        // names describe stands where Bento rests it is a separate question, and
        // the matrices cannot answer it themselves.
        std::cout << "  "
                  << homeworldz::mesh::describe(homeworldz::mesh::check_rig(
                         parsed->skin->joints, parsed->skin->inverse_bind));
    }
    return refused;
}
