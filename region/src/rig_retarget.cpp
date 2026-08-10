#include "homeworldz/rig_retarget.h"

#include "homeworldz/avatar_joints.h"

#include <algorithm>
#include <set>

namespace homeworldz::mesh {

std::string_view retarget_joint(std::string_view source) {
    // A rig that already speaks Bento is not retargeted at all — it resolves
    // through the skeleton's own alias table, which accepts every spelling a
    // viewer would. Checked first so this never re-maps a name that was already
    // correct.
    if (const auto canonical = canonical_joint(source); !canonical.empty()) return canonical;
    for (const auto& entry : character_creator_to_bento)
        if (entry.source == source) return entry.target;
    return {};
}

RetargetFinding describe_retarget(const std::vector<std::string>& sources) {
    RetargetFinding finding;
    std::set<std::string_view> targets;
    for (const auto& source : sources) {
        const auto target = retarget_joint(source);
        if (target.empty()) {
            finding.unmapped.push_back(source);
            continue;
        }
        ++finding.mapped;
        // A target already claimed means this source's weights will be added to
        // another's rather than standing alone. Counted because it is the part
        // of a retarget that is lossy, and the number belongs in the report
        // rather than in someone's head.
        if (!targets.insert(target).second) ++finding.merged;
    }
    finding.targets = static_cast<std::uint32_t>(targets.size());
    return finding;
}

} // namespace homeworldz::mesh
