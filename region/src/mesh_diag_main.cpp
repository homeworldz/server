// What the upload gate says about a GLB, and what conversion makes of it,
// without uploading anything.
//
// The acceptance policy is published so creators can check their own content
// (ADR 0033), but a published document still leaves them running the file
// against a real region to find out. This answers the same questions offline
// through the identical code paths — not a reimplementation of the rules,
// which would be a second copy able to disagree with the first.
#include "homeworldz/avatar_joints.h"
#include "homeworldz/mesh_acceptance.h"
#include "homeworldz/mesh_convert.h"
#include "homeworldz/rig_check.h"
#include "homeworldz/slmesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// Where a set of overrides leaves a joint, by the viewer's own accumulation: an
// override is a local offset, so world position is the sum down the skeleton's
// hierarchy, taking an override where one exists and the skeleton's own offset
// where none does.
using Placement = std::map<std::string, std::array<float, 3>>;

std::optional<std::array<float, 3>> rebuild(std::string_view joint, const Placement& placed) {
    std::array<float, 3> built{};
    for (auto name = joint; !name.empty(); name = homeworldz::mesh::joint_parent(name)) {
        if (const auto found = placed.find(std::string(name)); found != placed.end()) {
            for (int axis = 0; axis < 3; ++axis) built[axis] += found->second[axis];
            continue;
        }
        std::array<float, 3> here{}, up{};
        const auto parent = homeworldz::mesh::joint_parent(name);
        if (!homeworldz::mesh::joint_rest(name, here[0], here[1], here[2])) return std::nullopt;
        if (!parent.empty()) homeworldz::mesh::joint_rest(parent, up[0], up[1], up[2]);
        for (int axis = 0; axis < 3; ++axis) built[axis] += here[axis] - up[axis];
    }
    return built;
}

// This mesh's own overrides, as that map.
Placement own_placement(const homeworldz::slmesh::Skin& skin) {
    Placement placed;
    for (std::size_t at = 0; at < skin.alternate_inverse_bind.size() && at < skin.joints.size();
         ++at) {
        const auto& m = skin.alternate_inverse_bind[at];
        placed.emplace(skin.joints[at], std::array<float, 3>{m[12], m[13], m[14]});
    }
    return placed;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: homeworldz-mesh-diag <file.glb> [more.glb ...]\n";
        return 2;
    }
    int refused = 0;
    // Every file's overrides together, because that is the skeleton the viewer
    // builds: overrides are applied per joint across every mesh a wearer has on,
    // so a part binding two joints is measured against the *body's* placement of
    // everything above them, not Linden's.
    //
    // Measuring each file alone reported the teeth of an imported body as
    // landing exactly where they should while they hung 31 mm through its chin,
    // because both the check and the converter assumed Linden's jaw and the body
    // moved it. A pass is a claim about what was measured; this measures the set
    // the parts are worn as.
    const bool as_set = argc > 2;
    Placement shared;
    std::set<std::string> contested;
    std::vector<std::pair<std::string, homeworldz::slmesh::Skin>> skins;
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

        // Both verdicts, because there are two ways for a GLB to arrive and they
        // differ on exactly one thing — the rig. A part produced by importing a
        // Character Creator file binds `CC_Base_*`, which is a refusal for an
        // upload (the creator sent a rig claiming to be ours) and an unanswered
        // question for an import (ADR 0035).
        //
        // Reporting only the upload verdict called such a part REFUSED and exited
        // 1, so a perfectly good imported asset read as broken — and this is the
        // tool reached for to find out whether it *is* broken. The tool cannot
        // know which door a file came through, so it says what each would make of
        // it and fails only when neither would take it.
        const auto as_upload =
            homeworldz::mesh::validate_glb(bytes, homeworldz::mesh::Origin::Upload);
        const auto as_import =
            homeworldz::mesh::validate_glb(bytes, homeworldz::mesh::Origin::Import);
        const auto& result = as_upload.accepted ? as_upload : as_import;
        std::cout << argv[index] << ": ";
        if (as_upload.accepted == as_import.accepted) {
            std::cout << (as_upload.accepted ? "accepted" : "REFUSED");
            if (!as_upload.accepted) {
                std::cout << " - " << as_upload.reason;
                refused = 1;
            }
        } else {
            // Only ever this way round: Import is Upload's rules minus the rig
            // refusal, so it cannot refuse what Upload accepts.
            std::cout << "accepted as an import, REFUSED as an upload - " << as_upload.reason;
        }
        if (result.accepted) {
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
        // What each joint actually ends up carrying, summed over every vertex of
        // the highest level.
        //
        // The point is the *total*: a correctly skinned vertex's weights sum to
        // one, so the grand total must equal the vertex count. Anything less is
        // weight that went missing between the glTF and here — and missing
        // weight does not fail, parse badly or look wrong in the bind pose. It
        // shows up only once something animates, as vertices that lag behind
        // their neighbours, which is a fan of long thin triangles rather than
        // anything a reader would call a rig fault.
        //
        // Per joint as well as in total, because Character Creator folds a
        // limb's twist bones onto the limb: `UpperarmTwist01` and its siblings
        // all become mShoulderLeft, and the weight they carried has to arrive
        // there rather than be dropped on the way.
        {
            std::map<std::string, double> carried;
            double total = 0.0;
            std::size_t vertices = 0;
            for (const auto& submesh : parsed->high) {
                vertices += submesh.positions.size();
                for (const auto& vertex : submesh.influences)
                    for (const auto& influence : vertex) {
                        if (influence.joint >= parsed->skin->joints.size()) continue;
                        carried[parsed->skin->joints[influence.joint]] += influence.weight;
                        total += influence.weight;
                    }
            }
            const auto shortfall = static_cast<double>(vertices) - total;
            std::cout << "  weights:    " << total << " over " << vertices << " vertices";
            if (std::fabs(shortfall) > static_cast<double>(vertices) * 0.005)
                std::cout << "  ** " << shortfall << " MISSING **";
            std::cout << '\n';
            std::vector<std::pair<std::string, double>> ranked(carried.begin(), carried.end());
            std::sort(ranked.begin(), ranked.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            for (std::size_t at = 0; at < ranked.size() && at < 6; ++at)
                std::cout << "     " << std::setw(22) << std::left << ranked[at].first
                          << std::right << std::setw(10) << ranked[at].second << '\n';
        }

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
            // What the viewer will actually build from them. An override is a
            // local offset, so world position is the sum down the skeleton's
            // hierarchy, taking this mesh's value where it has one and the
            // skeleton's own where it does not. That total has to match the
            // joint's world position, which is what the inverse bind encodes
            // and what rig_check measures.
            //
            // This is the check that was missing. Both convention errors — world
            // positions written as local, then local measured against the source
            // rig's parent rather than the skeleton's — were invisible to
            // everything else here, because the inverse bind is the override's
            // exact inverse and so the bind pose is identity either way. Each
            // was found by wearing one.
            const auto own = own_placement(*parsed->skin);
            std::size_t disagreed = 0;
            float worst_gap = 0.0f;
            std::string worst_gap_joint;
            for (std::size_t at = 0; at < overrides; ++at) {
                const auto built = rebuild(parsed->skin->joints[at], own);
                if (!built) continue;
                const auto& inverse = parsed->skin->inverse_bind[at];
                float gap = 0.0f;
                for (int axis = 0; axis < 3; ++axis) {
                    const auto difference = (*built)[axis] + inverse[12 + axis];
                    gap += difference * difference;
                }
                gap = std::sqrt(gap);
                if (gap > 0.001f) {
                    ++disagreed;
                    if (gap > worst_gap) {
                        worst_gap = gap;
                        worst_gap_joint = parsed->skin->joints[at];
                    }
                }
            }
            std::cout << "  rebuilt:    "
                      << (disagreed == 0
                              ? "every joint lands where the mesh says it should"
                              : std::to_string(disagreed) + " of " +
                                    std::to_string(overrides) +
                                    " joints land elsewhere, worst " + worst_gap_joint + " at " +
                                    std::to_string(worst_gap * 1000.0f) + " mm")
                      // Worn alone, which for a part of a larger import is not
                      // how it is worn at all. The set below is the verdict.
                      << (as_set ? " (measured on its own)\n" : "\n");
            if (disagreed != 0 && !as_set) refused = 1;
            for (const auto& [joint, offset] : own) {
                const auto found = shared.find(joint);
                if (found == shared.end()) {
                    shared.emplace(joint, offset);
                    continue;
                }
                for (int axis = 0; axis < 3; ++axis)
                    if (std::fabs(found->second[axis] - offset[axis]) > 0.0001f)
                        contested.insert(joint);
            }
            skins.emplace_back(argv[index], *parsed->skin);
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
    // The files as one worn set: every override applied to one skeleton, which is
    // what a wearer gets. Only meaningful for parts of the same import — given
    // unrelated bodies it reports exactly what it should, that two of them are
    // competing to place the same joints.
    if (as_set && !skins.empty()) {
        std::cout << "as one set: " << skins.size() << " rigged file(s), " << shared.size()
                  << " joint(s) placed\n";
        for (const auto& joint : contested)
            std::cout << "  CONTESTED   " << joint
                      << " - two meshes place it differently; the viewer takes one of them\n";
        if (!contested.empty()) refused = 1;
        std::size_t disagreed = 0;
        for (const auto& [name, skin] : skins) {
            for (std::size_t at = 0; at < skin.alternate_inverse_bind.size(); ++at) {
                const auto built = rebuild(skin.joints[at], shared);
                if (!built) continue;
                float gap = 0.0f;
                for (int axis = 0; axis < 3; ++axis) {
                    const auto difference = (*built)[axis] + skin.inverse_bind[at][12 + axis];
                    gap += difference * difference;
                }
                gap = std::sqrt(gap);
                if (gap <= 0.001f) continue;
                ++disagreed;
                std::cout << "  ELSEWHERE   " << skin.joints[at] << " in " << name << " lands "
                          << gap * 1000.0f << " mm from where that mesh is skinned for\n";
            }
        }
        if (disagreed == 0)
            std::cout << "  every joint lands where the mesh skinned to it says it should\n";
        else
            refused = 1;
    }
    return refused;
}
