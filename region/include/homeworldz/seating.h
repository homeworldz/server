#pragma once

// Sitting (ADR 0040): the choices and geometry that do not depend on the tick
// loop, so they can be tested without a running region.

#include "homeworldz/scene.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace homeworldz::region {

// One prim of a linkset, as seat choice sees it.
struct SeatCandidate {
    std::uint64_t id{};
    bool has_sit_target{};
    bool occupied{};
};

// The prim an avatar sits on when it clicks `clicked`, given the linkset in link
// order: the clicked prim's sit target if it is free; else the first free sit
// target in link order; else the clicked prim itself if it has no sit target
// at all (a default seat); else nothing — every target is taken. Halcyon falls
// back to the clicked prim in that last case and seats two avatars on one
// target; Second Life refuses.
std::optional<std::uint64_t> choose_seat(std::uint64_t clicked,
                                         const std::vector<SeatCandidate>& link_order);

// A linkset's link order: the root first (link 1), then its children by local
// id, which is the order they were created or linked in. The scene keeps no
// link numbers of its own; this is the one place that assigns them.
std::vector<std::uint64_t> link_order(std::uint64_t root, std::vector<std::uint64_t> children);

// The prims an LSL link number names, from the prim a script lives in:
// LINK_ROOT (1), LINK_SET (-1), LINK_ALL_OTHERS (-2), LINK_ALL_CHILDREN (-3),
// LINK_THIS (-4), or a link number. A single-prim object's only prim is link 0
// as well as LINK_ROOT. An unknown number names nothing.
std::vector<std::uint64_t> resolve_link(std::int32_t link, std::uint64_t caller,
                                        const std::vector<std::uint64_t>& order);

// Where the viewer draws a seated avatar, relative to the seat's ROOT prim: the
// viewer is always given the root as the parent, even for a child seat
// (Halcyon). `seat_local_*` is the seat prim's offset in the root's frame, both
// zero when the seat is the root.
struct SeatPose {
    scene::Vector3 position;
    scene::Vector3 rotation;
};

// The viewer's own default seat for a prim with no sit target, in its frame.
inline constexpr scene::Vector3 default_seat_offset{0.34, 0.0, 0.55};

SeatPose seat_pose(scene::Vector3 seat_local_position, scene::Vector3 seat_local_rotation,
                   bool has_sit_target, scene::Vector3 sit_target_position,
                   scene::Vector3 sit_target_rotation, double hip_offset);

// The seated avatar's world transform, for everything the region decides by
// position — which facet it is on, who is near it, where it stands up.
SeatPose seated_world(scene::Vector3 root_position, scene::Vector3 root_rotation,
                      const SeatPose& pose);

// Where a seated avatar is put when it stands: its seated position, raised to
// clear the top of the seat prim by half the avatar's height.
scene::Vector3 stand_position(scene::Vector3 seated_position, const scene::Entity& seat,
                              double avatar_height);

} // namespace homeworldz::region
