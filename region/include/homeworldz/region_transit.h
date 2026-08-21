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

// A child agent (ADR 0038): a session this region knows whose avatar lives in a
// neighbour. It exists so that a crossing can *promote* it rather than build an
// avatar from nothing in the tick the viewer is told it has arrived — which is
// what a cold arrival does, and what no viewer was written to survive.
//
// `seed` is the capability seed this region minted for the child's own circuit.
// It is the field that makes establishment idempotent rather than repeatable:
// see establish().
struct ChildAgent {
    std::string agent_id;
    std::string session_id;
    std::uint32_t circuit_code{};
    // The region the avatar is actually in, so a child can tell a promotion it
    // expected from one it did not.
    std::string home_region_id;
    std::string seed;
    // Last known, in this region's coordinates. A child is told where the
    // avatar is so it can decide what of its world is worth sending.
    std::array<float, 3> position{};
    // What the avatar is wearing, carried by the source rather than read here.
    // The source already holds it; a destination that fetched it would do one
    // grid read per neighbour per session, and those reads would land on the
    // sim tick, which already starves the viewer circuit under synchronous grid
    // I/O. Empty is a real answer — wearing nothing — which is why the parser
    // refuses a malformed set rather than returning a short one.
    std::vector<grid::WornAttachment> worn;
    // The appearance metadata, carried for the same reason the worn set is, and
    // stopping where Halcyon's JSON representation stops: it packs
    // `texture_entry`, `visual_params` and wearables into the interregion
    // message and leaves serialized attachment blobs to its protobuf path. The
    // texture entry is a list of asset ids and the visual params are a couple of
    // hundred bytes, so this carries what an appearance *is* and never the
    // assets it names — those the destination fetches like any other.
    //
    // Optional, but all-or-nothing: an avatar the source has not established an
    // appearance for yet sends none, and half an appearance is the same class of
    // fault as half a worn set.
    std::vector<std::byte> texture_entry;
    std::vector<std::uint8_t> visual_params;
    // The wire's CofVersion, named for what it is. A viewer discards an
    // appearance for its own avatar whose version does not exceed the highest it
    // has seen, so a promotion has to continue this number rather than restart
    // it — the mistake that made a crossing drop the seeded appearance.
    std::uint32_t cof_version{};
    // 0 = legacy (the viewer composites locally), 1 = server-side bake.
    std::uint8_t appearance_version{};

    bool has_appearance() const {
        return !texture_entry.empty() && !visual_params.empty();
    }
};

// The establishment call, region to region (ADR 0038). The source describes the
// visitor; the destination answers with the seed it minted.
//
// `seed` is deliberately not part of the request, and the parser drops one if it
// is sent: the seed belongs to the region that will serve it, and a source that
// could name it could name a capability path on a region it does not run.
std::string encode_child_agent_request(const ChildAgent& agent);
std::optional<ChildAgent> parse_child_agent_request(std::string_view document);
std::string encode_child_agent_acceptance(std::string_view seed);
// The seed, or empty when the answer did not carry one — which a source must
// treat as a refusal, because announcing a neighbour whose seed does not answer
// stalls the viewer.
std::string parse_child_agent_acceptance(std::string_view document);

class ChildAgentRegistry {
public:
    // Establish, or refresh what is already there, and return the child as this
    // region now holds it.
    //
    // Idempotent by session, and deliberately keeps the seed it first minted: a
    // source that retries — which it may, the call is a region-to-region POST —
    // must not be handed a second seed, because the viewer already opened a
    // circuit against the first and a replacement strands it. A neighbour that
    // restarted has no entry, mints a new seed, and answers with that; the
    // source cannot tell the difference and does not need to.
    const ChildAgent& establish(ChildAgent agent,
                                std::chrono::steady_clock::time_point now,
                                std::chrono::seconds lifetime = std::chrono::seconds(300));
    const ChildAgent* find(std::string_view session_id,
                           std::chrono::steady_clock::time_point now);
    // The crossing arrived: this session is not a child here any more. Returns
    // what was held, once. A second promotion of one session finds nothing,
    // because an avatar cannot arrive twice and a retry that seemed to work
    // would leave a child and a root for the same session.
    std::optional<ChildAgent> promote(std::string_view session_id,
                                      std::chrono::steady_clock::time_point now);
    void remove(std::string_view session_id);
    std::size_t size(std::chrono::steady_clock::time_point now);

private:
    struct Entry {
        ChildAgent agent;
        std::chrono::steady_clock::time_point expires_at;
    };

    void purge(std::chrono::steady_clock::time_point now);
    std::unordered_map<std::string, Entry> entries_;
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
