#include "homeworldz/seating.h"

#include <algorithm>
#include <cmath>

namespace homeworldz::region {

std::optional<std::uint64_t> choose_seat(std::uint64_t clicked,
                                         const std::vector<SeatCandidate>& link_order) {
    const auto clicked_part = std::find_if(link_order.begin(), link_order.end(),
        [&](const SeatCandidate& part) { return part.id == clicked; });
    if (clicked_part == link_order.end()) return std::nullopt;
    if (clicked_part->has_sit_target && !clicked_part->occupied) return clicked;
    for (const auto& part : link_order)
        if (part.has_sit_target && !part.occupied) return part.id;
    if (!clicked_part->has_sit_target && !clicked_part->occupied) return clicked;
    return std::nullopt;
}

std::vector<std::uint64_t> link_order(std::uint64_t root, std::vector<std::uint64_t> children) {
    std::sort(children.begin(), children.end());
    children.erase(std::remove(children.begin(), children.end(), root), children.end());
    std::vector<std::uint64_t> order{root};
    order.insert(order.end(), children.begin(), children.end());
    return order;
}

std::vector<std::uint64_t> resolve_link(std::int32_t link, std::uint64_t caller,
                                        const std::vector<std::uint64_t>& order) {
    if (order.empty()) return {};
    switch (link) {
    case -1: return order;                                   // LINK_SET
    case -2: {                                               // LINK_ALL_OTHERS
        std::vector<std::uint64_t> others;
        for (const auto id : order)
            if (id != caller) others.push_back(id);
        return others;
    }
    case -3: return {order.begin() + 1, order.end()};        // LINK_ALL_CHILDREN
    case -4: return {caller};                                // LINK_THIS
    default: break;
    }
    if (link == 0 && order.size() == 1) return {order.front()};
    if (link >= 1 && static_cast<std::size_t>(link) <= order.size())
        return {order[static_cast<std::size_t>(link) - 1]};
    return {};
}

SeatPose seat_pose(scene::Vector3 seat_local_position, scene::Vector3 seat_local_rotation,
                   bool has_sit_target, scene::Vector3 sit_target_position,
                   scene::Vector3 sit_target_rotation, double hip_offset) {
    // In the seat prim's own frame first, then carried into the root's.
    scene::Vector3 position = has_sit_target ? sit_target_position : default_seat_offset;
    // The viewer draws an avatar from its hips; the offset (normally negative)
    // is what keeps a seated one from sinking into its seat (Halcyon).
    position.z -= hip_offset;
    const scene::Vector3 rotation =
        has_sit_target ? sit_target_rotation : scene::Vector3{0.0, 0.0, 0.0};
    const auto rotated = scene::rotate_vector(position, seat_local_rotation);
    return {{rotated.x + seat_local_position.x, rotated.y + seat_local_position.y,
             rotated.z + seat_local_position.z},
            scene::multiply_rotations(seat_local_rotation, rotation)};
}

SeatPose seated_world(scene::Vector3 root_position, scene::Vector3 root_rotation,
                      const SeatPose& pose) {
    const auto offset = scene::rotate_vector(pose.position, root_rotation);
    return {{root_position.x + offset.x, root_position.y + offset.y, root_position.z + offset.z},
            scene::multiply_rotations(root_rotation, pose.rotation)};
}

scene::Vector3 stand_position(scene::Vector3 seated_position, const scene::Entity& seat,
                              double avatar_height) {
    // The seat's vertical extent, whichever way it is turned: its scale carried
    // through its rotation, as Halcyon does with the bounding box.
    const auto extent = scene::rotate_vector(seat.scale, seat.rotation);
    const auto top = seat.position.z + std::abs(extent.z) * 0.5;
    scene::Vector3 standing = seated_position;
    standing.z = (std::max)(seated_position.z, top + avatar_height * 0.5);
    return standing;
}

} // namespace homeworldz::region
