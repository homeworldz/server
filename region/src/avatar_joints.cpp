#include "homeworldz/avatar_joints.h"

#include <algorithm>
#include <iterator>

namespace homeworldz::mesh {

bool is_riggable_joint(std::string_view name) {
    // The table is sorted, so this is a binary search rather than a scan of 407
    // entries per joint of every skin in every upload.
    return std::binary_search(std::begin(riggable_joint_names),
                              std::end(riggable_joint_names), name);
}

std::string_view canonical_joint(std::string_view name) {
    for (const auto& entry : joint_rest_positions)
        if (entry.name == name) return entry.name;
    for (const auto& entry : joint_aliases)
        if (entry.alias == name) return entry.canonical;
    // Attachment points are legal rig targets and are not bones, so they
    // resolve to themselves without a rest position.
    if (is_riggable_joint(name)) return name;
    return {};
}

bool joint_rest(std::string_view canonical, float& x, float& y, float& z) {
    for (const auto& entry : joint_rest_positions) {
        if (entry.name != canonical) continue;
        x = entry.x;
        y = entry.y;
        z = entry.z;
        return true;
    }
    return false;
}

std::string_view joint_parent(std::string_view canonical) {
    for (const auto& entry : joint_parents)
        if (entry.joint == canonical) return entry.parent;
    return {};
}

} // namespace homeworldz::mesh
