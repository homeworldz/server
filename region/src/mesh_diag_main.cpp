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
        const auto overrides = parsed->skin->alternate_inverse_bind.size();
        std::cout << "  overrides: ";
        if (overrides == 0)
            std::cout << "none - this body will take the skeleton's own proportions\n";
        else if (overrides != parsed->skin->joints.size())
            std::cout << overrides << " for " << parsed->skin->joints.size()
                      << " joints - MISMATCHED, so a viewer discards all of them\n";
        else
            std::cout << overrides << ", one per joint\n";
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
