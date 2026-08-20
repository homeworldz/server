#pragma once

#include "homeworldz/grid_client.h"

#include <chrono>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace homeworldz::region {

// Where a thing that has left this region's extent goes next, and where it
// lands. The geometry is the same for an avatar and for a crate rolling off
// the edge — which border was crossed, which online neighbor owns the far
// side, and the same point expressed in that neighbor's coordinates — so one
// planner answers for both rather than two drifting apart.
struct BorderCrossing {
    grid::RegionNeighbor destination;
    std::array<float, 3> position{};
};

std::optional<BorderCrossing> plan_border_crossing(
    int source_grid_x, int source_grid_y, int source_size_x, int source_size_y,
    std::array<double, 3> source_position,
    std::span<const grid::RegionNeighbor> neighbors, double destination_inset = 0.3);

std::optional<std::array<float, 3>> resolve_region_teleport_position(
    int region_grid_x, int region_grid_y, int region_size_x, int region_size_y,
    std::uint64_t requested_handle, std::array<float, 3> requested_position);

class InboundTransitRegistry {
public:
    bool stage(const grid::AvatarTransit& transit, std::string_view local_region_id,
               std::chrono::steady_clock::time_point now,
               std::chrono::seconds lifetime = std::chrono::seconds(30));
    const grid::AvatarTransit* authorize(std::string_view agent_id, std::string_view session_id,
                                        std::chrono::steady_clock::time_point now);
    std::optional<grid::AvatarTransit> consume(std::string_view session_id,
                                               std::chrono::steady_clock::time_point now);
    void remove(std::string_view session_id);
    std::size_t size(std::chrono::steady_clock::time_point now);

private:
    struct Entry {
        grid::AvatarTransit transit;
        std::chrono::steady_clock::time_point expires_at;
    };

    void purge(std::chrono::steady_clock::time_point now);
    std::unordered_map<std::string, Entry> entries_;
};

// An object handed across a macro border, as it travels.
//
// The `linkset` field is the very asset a take writes — one format carries an
// object into inventory and across a border, so a crossing can never lose a
// field that a take preserves. What the asset deliberately does not hold is
// everything that makes this *this* object rather than a copy of it: its
// identity, its owner, where it is, and how it is moving. Those ride beside
// it, and a crossing that dropped any of them would arrive as a new object
// wearing the old one's shape.
struct ObjectTransit {
    std::string id;
    std::string source_region_id;
    std::string destination_region_id;
    // Preserved, never reissued. A viewer, the grid's object-rez records, and
    // anything holding a reference all name the root by this id; a crossing
    // that minted a fresh one would be a rez of a lookalike.
    std::string object_id;
    // Child ids in the same order as the linkset asset's parts after the root.
    std::vector<std::string> child_object_ids;
    std::string owner_id;
    std::uint64_t creation_date{};
    // Destination-local: the source rebases before sending, because only the
    // source knows which border was crossed.
    std::array<double, 3> position{};
    std::array<double, 4> rotation{0.0, 0.0, 0.0, 1.0};
    std::array<double, 3> linear_velocity{};
    std::array<double, 3> angular_velocity{};
    std::string linkset;
};

std::string encode_object_transit(const ObjectTransit& transit);
std::optional<ObjectTransit> parse_object_transit(std::string_view document);

// The destination half of an object crossing, which is two-phase for one
// reason: an object must never be alive in both regions at once, and must
// never be alive in neither.
//
// The source stages the object here first and only then removes its own copy;
// the removal is what authorizes activation. If the source dies in between,
// the staged entry expires having created nothing and the object is still at
// the source, which is the safe direction to fail. Arrivals are remembered
// past activation so that a retried activation — the source did not hear the
// reply — answers yes without rezzing a second object.
class InboundObjectRegistry {
public:
    bool stage(ObjectTransit transit, std::string_view local_region_id,
               std::chrono::steady_clock::time_point now,
               std::chrono::seconds lifetime = std::chrono::seconds(30));
    // The staged object, removed from staging. Empty for an unknown or expired
    // transit, and empty for one that already arrived — ask `arrived` to tell
    // those apart.
    std::optional<ObjectTransit> activate(std::string_view transit_id,
                                          std::chrono::steady_clock::time_point now);
    bool arrived(std::string_view transit_id) const;
    void note_arrival(std::string_view transit_id, std::chrono::steady_clock::time_point now,
                      std::chrono::seconds memory = std::chrono::seconds(300));
    std::size_t size(std::chrono::steady_clock::time_point now);

private:
    struct Entry {
        ObjectTransit transit;
        std::chrono::steady_clock::time_point expires_at;
    };

    void purge(std::chrono::steady_clock::time_point now);
    std::unordered_map<std::string, Entry> staged_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> arrived_;
};

class CapabilityArrivalGate {
public:
    bool mark_seed_served(std::string_view session_id, std::string_view visit_id);
    bool consume_seed(std::string_view session_id, std::string_view visit_id);
    void clear_session(std::string_view session_id);
    std::size_t size() const;

private:
    static std::string key(std::string_view session_id, std::string_view visit_id);
    std::unordered_set<std::string> served_seeds_;
};

} // namespace homeworldz::region
