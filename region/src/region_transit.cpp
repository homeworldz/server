#include "homeworldz/region_transit.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>

#include "homeworldz/llsd_xml.h"
#include "homeworldz/session_protocol.h"
#include <string>
#include <string_view>
#include <utility>

namespace homeworldz::region {

std::optional<std::array<float, 3>> resolve_region_teleport_position(
    int region_grid_x, int region_grid_y, int region_size_x, int region_size_y,
    std::uint64_t requested_handle, std::array<float, 3> requested_position) {
    constexpr std::uint64_t map_cell_metres = 256;
    if (region_grid_x < 0 || region_grid_y < 0 || region_size_x <= 0 || region_size_y <= 0 ||
        region_size_x % map_cell_metres != 0 || region_size_y % map_cell_metres != 0 ||
        !std::isfinite(requested_position[0]) || !std::isfinite(requested_position[1]) ||
        !std::isfinite(requested_position[2]))
        return std::nullopt;
    const auto handle_x = requested_handle >> 32;
    const auto handle_y = requested_handle & 0xffffffffULL;
    if (handle_x % map_cell_metres != 0 || handle_y % map_cell_metres != 0) return std::nullopt;
    const auto origin_x = static_cast<std::uint64_t>(region_grid_x) * map_cell_metres;
    const auto origin_y = static_cast<std::uint64_t>(region_grid_y) * map_cell_metres;
    if (handle_x < origin_x || handle_x >= origin_x + static_cast<std::uint64_t>(region_size_x) ||
        handle_y < origin_y || handle_y >= origin_y + static_cast<std::uint64_t>(region_size_y))
        return std::nullopt;

    // Firestorm normally uses the Region's southwest handle and full variable-
    // Region coordinates. Some map and SLURL paths instead quantize the handle
    // to an internal 256 m tile and send coordinates relative to that tile.
    if (handle_x != origin_x || handle_y != origin_y) {
        requested_position[0] += static_cast<float>(handle_x - origin_x);
        requested_position[1] += static_cast<float>(handle_y - origin_y);
    }
    if (requested_position[0] < 0.0F || requested_position[0] > region_size_x ||
        requested_position[1] < 0.0F || requested_position[1] > region_size_y)
        return std::nullopt;
    return requested_position;
}

std::vector<std::uint32_t> child_circuit_farewell_ids(
    std::span<const FacetOccupant> occupants, int facet,
    std::string_view recipient_session) {
    std::vector<std::uint32_t> ids;
    for (const auto& occupant : occupants) {
        // A child circuit is filled with one facet and updated with one facet,
        // so it holds one facet's ids and nothing else. Naming another's would
        // be harmless — a viewer ignores an id it never held — but it would
        // also mean this no longer describes what that circuit was shown, and
        // the next reader would have to work out which.
        if (occupant.facet != facet) continue;
        if (!occupant.session_id.empty() && occupant.session_id == recipient_session) continue;
        ids.push_back(occupant.local_id);
    }
    return ids;
}

std::optional<BorderCrossing> plan_border_crossing(
    int source_grid_x, int source_grid_y, int source_size_x, int source_size_y,
    std::array<double, 3> source_position,
    std::span<const grid::RegionNeighbor> neighbors, double destination_inset) {
    if (source_size_x <= 0 || source_size_y <= 0 || destination_inset < 0.0 ||
        !std::isfinite(source_position[0]) || !std::isfinite(source_position[1]) ||
        !std::isfinite(source_position[2]))
        return std::nullopt;
    struct CrossedBorder {
        std::string_view direction;
        double overflow;
    };
    std::array<CrossedBorder, 4> crossed{};
    std::size_t crossed_count{};
    if (source_position[0] < 0.0)
        crossed[crossed_count++] = {"west", -source_position[0]};
    if (source_position[0] > source_size_x)
        crossed[crossed_count++] = {"east", source_position[0] - source_size_x};
    if (source_position[1] < 0.0)
        crossed[crossed_count++] = {"south", -source_position[1]};
    if (source_position[1] > source_size_y)
        crossed[crossed_count++] = {"north", source_position[1] - source_size_y};
    std::sort(crossed.begin(), crossed.begin() + crossed_count,
              [](const CrossedBorder& first, const CrossedBorder& second) {
                  return first.overflow > second.overflow;
              });
    constexpr double map_cell_metres = 256.0;
    const auto global_x = source_grid_x * map_cell_metres + source_position[0];
    const auto global_y = source_grid_y * map_cell_metres + source_position[1];
    for (std::size_t border_index = 0; border_index < crossed_count; ++border_index) {
        for (const auto& neighbor : neighbors) {
            if (!neighbor.online || neighbor.direction != crossed[border_index].direction ||
                neighbor.size_x <= 0 || neighbor.size_y <= 0)
                continue;
            const auto neighbor_x = neighbor.grid_x * map_cell_metres;
            const auto neighbor_y = neighbor.grid_y * map_cell_metres;
            const auto neighbor_max_x = neighbor_x + neighbor.size_x;
            const auto neighbor_max_y = neighbor_y + neighbor.size_y;
            const bool orthogonal_match =
                (neighbor.direction == "east" || neighbor.direction == "west")
                    ? global_y >= neighbor_y && global_y <= neighbor_max_y
                    : global_x >= neighbor_x && global_x <= neighbor_max_x;
            if (!orthogonal_match) continue;
            const auto maximum_x = std::max(destination_inset, neighbor.size_x - destination_inset);
            const auto maximum_y = std::max(destination_inset, neighbor.size_y - destination_inset);
            return BorderCrossing{
                neighbor,
                {static_cast<float>(std::clamp(global_x - neighbor_x,
                                               destination_inset, maximum_x)),
                 static_cast<float>(std::clamp(global_y - neighbor_y,
                                               destination_inset, maximum_y)),
                 static_cast<float>(source_position[2])}};
        }
    }
    return std::nullopt;
}

namespace {

// The envelope is written and read here and nowhere else, so it uses the
// smallest reader that can do the job rather than a JSON library the region
// does not otherwise carry. Every field is emitted, so every field is there to
// be found; a document missing one is rejected rather than defaulted, because
// a crossing that quietly defaults an owner or a position produces a plausible
// wrong object instead of a visible failure.
std::string_view json_string_field(std::string_view document, std::string_view name) {
    const std::string marker = "\"" + std::string(name) + "\":\"";
    const auto start = document.find(marker);
    if (start == std::string_view::npos) return {};
    const auto value_start = start + marker.size();
    const auto end = document.find('"', value_start);
    if (end == std::string_view::npos) return {};
    return document.substr(value_start, end - value_start);
}

std::optional<double> json_number_field(std::string_view document, std::string_view name) {
    const std::string marker = "\"" + std::string(name) + "\":";
    const auto start = document.find(marker);
    if (start == std::string_view::npos) return std::nullopt;
    const auto rest = document.substr(start + marker.size());
    double value{};
    const auto result = std::from_chars(rest.data(), rest.data() + rest.size(), value);
    if (result.ec != std::errc{} || result.ptr == rest.data()) return std::nullopt;
    return value;
}

// A bracketed run of numbers, read as exactly `count` of them. A vector that
// arrived short is a malformed document, not a vector of zeroes.
template <std::size_t count>
std::optional<std::array<double, count>> json_number_array(
    std::string_view document, std::string_view name) {
    const std::string marker = "\"" + std::string(name) + "\":[";
    const auto start = document.find(marker);
    if (start == std::string_view::npos) return std::nullopt;
    auto rest = document.substr(start + marker.size());
    std::array<double, count> values{};
    for (std::size_t index = 0; index < count; ++index) {
        while (!rest.empty() && (rest.front() == ' ' || rest.front() == ',')) rest.remove_prefix(1);
        const auto result = std::from_chars(rest.data(), rest.data() + rest.size(), values[index]);
        if (result.ec != std::errc{} || result.ptr == rest.data()) return std::nullopt;
        rest.remove_prefix(static_cast<std::size_t>(result.ptr - rest.data()));
    }
    while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
    if (rest.empty() || rest.front() != ']') return std::nullopt;
    return values;
}

std::vector<std::string> json_string_array(std::string_view document, std::string_view name) {
    std::vector<std::string> values;
    const std::string marker = "\"" + std::string(name) + "\":[";
    const auto start = document.find(marker);
    if (start == std::string_view::npos) return values;
    auto rest = document.substr(start + marker.size());
    while (!rest.empty() && rest.front() != ']') {
        if (rest.front() != '"') {
            rest.remove_prefix(1);
            continue;
        }
        rest.remove_prefix(1);
        const auto end = rest.find('"');
        if (end == std::string_view::npos) return values;
        values.emplace_back(rest.substr(0, end));
        rest.remove_prefix(end + 1);
    }
    return values;
}

// The worn set, as an array of objects rather than the string arrays above.
//
// Returns nullopt for a malformed element, and never a partial set. Half a worn
// set is worse than none: it dresses the avatar in some of its clothes and looks
// like a successful establishment. An empty array is a different answer and a
// legitimate one — it means wearing nothing.
std::optional<std::vector<grid::WornAttachment>> json_worn_array(
    std::string_view document, std::string_view name) {
    std::vector<grid::WornAttachment> worn;
    const std::string marker = "\"" + std::string(name) + "\":[";
    const auto start = document.find(marker);
    if (start == std::string_view::npos) return std::nullopt;
    auto rest = document.substr(start + marker.size());
    while (!rest.empty() && rest.front() != ']') {
        if (rest.front() != '{') {
            rest.remove_prefix(1);
            continue;
        }
        const auto close = rest.find('}');
        if (close == std::string_view::npos) return std::nullopt;
        const auto element = rest.substr(0, close);
        const auto item = json_string_field(element, "itemId");
        const auto point = json_number_field(element, "attachmentPoint");
        // Zero means "wherever the item says", which the region resolves long
        // before worn state is stored; an unresolved point arriving here is a
        // question mistaken for an answer.
        if (item.empty() || !point || *point < 1.0 || *point > 127.0)
            return std::nullopt;
        worn.push_back({std::string(item), static_cast<std::uint8_t>(*point)});
        rest.remove_prefix(close + 1);
    }
    return worn;
}

// The nested linkset asset, lifted out whole. It is itself JSON, so it travels
// raw rather than escaped into a string: the crossing hands the destination
// the same bytes a take would have written, and the destination reads them
// with the same parser.
std::string_view json_nested_object(std::string_view document, std::string_view name) {
    const std::string marker = "\"" + std::string(name) + "\":{";
    const auto start = document.find(marker);
    if (start == std::string_view::npos) return {};
    const auto object_start = start + marker.size() - 1;
    std::size_t depth = 0;
    bool quoted_text = false;
    bool escaped = false;
    for (auto index = object_start; index < document.size(); ++index) {
        const auto character = document[index];
        if (quoted_text) {
            if (escaped) escaped = false;
            else if (character == '\\') escaped = true;
            else if (character == '"') quoted_text = false;
            continue;
        }
        if (character == '"') quoted_text = true;
        else if (character == '{') ++depth;
        else if (character == '}' && --depth == 0)
            return document.substr(object_start, index - object_start + 1);
    }
    return {};
}

std::string quoted_string(std::string_view value) {
    std::string result{'"'};
    for (const auto character : value) {
        if (character == '"' || character == '\\') result.push_back('\\');
        result.push_back(character);
    }
    result.push_back('"');
    return result;
}

// Round-trip precision, deliberately: a crossing that rounded a position would
// move the object, and one that rounded a velocity would bend its path.
std::string number(double value) {
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return std::string(buffer.data(), result.ptr);
}

template <std::size_t count>
std::string number_array(const std::array<double, count>& values) {
    std::string result{'['};
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0) result.push_back(',');
        result += number(values[index]);
    }
    result.push_back(']');
    return result;
}

} // namespace

std::string encode_object_transit(const ObjectTransit& transit) {
    std::string children{'['};
    for (std::size_t index = 0; index < transit.child_object_ids.size(); ++index) {
        if (index != 0) children.push_back(',');
        children += quoted_string(transit.child_object_ids[index]);
    }
    children.push_back(']');
    return std::string{"{\"id\":"} + quoted_string(transit.id) +
        ",\"sourceRegionId\":" + quoted_string(transit.source_region_id) +
        ",\"destinationRegionId\":" + quoted_string(transit.destination_region_id) +
        ",\"objectId\":" + quoted_string(transit.object_id) +
        ",\"childObjectIds\":" + children +
        ",\"ownerId\":" + quoted_string(transit.owner_id) +
        ",\"creationDate\":" + std::to_string(transit.creation_date) +
        ",\"position\":" + number_array(transit.position) +
        ",\"rotation\":" + number_array(transit.rotation) +
        ",\"linearVelocity\":" + number_array(transit.linear_velocity) +
        ",\"angularVelocity\":" + number_array(transit.angular_velocity) +
        ",\"linkset\":" + transit.linkset + "}";
}

std::optional<ObjectTransit> parse_object_transit(std::string_view document) {
    ObjectTransit transit;
    transit.id = json_string_field(document, "id");
    transit.source_region_id = json_string_field(document, "sourceRegionId");
    transit.destination_region_id = json_string_field(document, "destinationRegionId");
    transit.object_id = json_string_field(document, "objectId");
    transit.child_object_ids = json_string_array(document, "childObjectIds");
    transit.owner_id = json_string_field(document, "ownerId");
    transit.linkset = json_nested_object(document, "linkset");
    const auto creation_date = json_number_field(document, "creationDate");
    const auto position = json_number_array<3>(document, "position");
    const auto rotation = json_number_array<4>(document, "rotation");
    const auto linear = json_number_array<3>(document, "linearVelocity");
    const auto angular = json_number_array<3>(document, "angularVelocity");
    if (transit.id.empty() || transit.source_region_id.empty() ||
        transit.destination_region_id.empty() || transit.object_id.empty() ||
        transit.owner_id.empty() || transit.linkset.empty() ||
        transit.source_region_id == transit.destination_region_id ||
        !creation_date || !position || !rotation || !linear || !angular)
        return std::nullopt;
    transit.creation_date = static_cast<std::uint64_t>(*creation_date);
    transit.position = *position;
    transit.rotation = *rotation;
    transit.linear_velocity = *linear;
    transit.angular_velocity = *angular;
    for (const auto& values : {transit.position, transit.linear_velocity, transit.angular_velocity})
        for (const auto value : values)
            if (!std::isfinite(value)) return std::nullopt;
    for (const auto value : transit.rotation)
        if (!std::isfinite(value)) return std::nullopt;
    return transit;
}

bool InboundObjectRegistry::stage(ObjectTransit transit, std::string_view local_region_id,
                                  std::chrono::steady_clock::time_point now,
                                  std::chrono::seconds lifetime) {
    purge(now);
    if (transit.id.empty() || transit.destination_region_id != local_region_id ||
        transit.source_region_id.empty() ||
        transit.source_region_id == transit.destination_region_id ||
        lifetime <= std::chrono::seconds::zero())
        return false;
    // An already-arrived transit does not stage again. The source is retrying
    // a call whose answer it never heard, and re-staging would let a second
    // activation rez the object a second time.
    if (arrived(transit.id)) return true;
    const auto found = staged_.find(transit.id);
    if (found != staged_.end() && found->second.transit.object_id != transit.object_id)
        return false;
    const auto id = transit.id;
    staged_.insert_or_assign(id, Entry{std::move(transit), now + lifetime});
    return true;
}

std::optional<ObjectTransit> InboundObjectRegistry::activate(
    std::string_view transit_id, std::chrono::steady_clock::time_point now) {
    purge(now);
    const auto found = staged_.find(std::string(transit_id));
    if (found == staged_.end()) return std::nullopt;
    auto transit = std::move(found->second.transit);
    staged_.erase(found);
    return transit;
}

bool InboundObjectRegistry::arrived(std::string_view transit_id) const {
    return arrived_.find(std::string(transit_id)) != arrived_.end();
}

void InboundObjectRegistry::note_arrival(std::string_view transit_id,
                                         std::chrono::steady_clock::time_point now,
                                         std::chrono::seconds memory) {
    if (transit_id.empty()) return;
    arrived_.insert_or_assign(std::string(transit_id), now + memory);
}

std::size_t InboundObjectRegistry::size(std::chrono::steady_clock::time_point now) {
    purge(now);
    return staged_.size();
}

void InboundObjectRegistry::purge(std::chrono::steady_clock::time_point now) {
    std::erase_if(staged_, [&](const auto& entry) { return entry.second.expires_at <= now; });
    std::erase_if(arrived_, [&](const auto& entry) { return entry.second <= now; });
}

bool InboundTransitRegistry::stage(const grid::AvatarTransit& transit,
                                   std::string_view local_region_id,
                                   std::chrono::steady_clock::time_point now,
                                   std::chrono::seconds lifetime) {
    purge(now);
    if (transit.state != "accepted" || transit.id.empty() || transit.generation == 0 ||
        transit.agent_id.empty() || transit.session_id.empty() ||
        transit.source_region_id.empty() || transit.destination_region_id != local_region_id ||
        transit.source_region_id == transit.destination_region_id || lifetime <= std::chrono::seconds::zero())
        return false;
    const auto found = entries_.find(transit.session_id);
    if (found != entries_.end() &&
        (found->second.transit.id != transit.id ||
         found->second.transit.generation != transit.generation))
        return false;
    entries_.insert_or_assign(transit.session_id, Entry{transit, now + lifetime});
    return true;
}

const grid::AvatarTransit* InboundTransitRegistry::authorize(
    std::string_view agent_id, std::string_view session_id,
    std::chrono::steady_clock::time_point now) {
    purge(now);
    const auto found = entries_.find(std::string(session_id));
    if (found == entries_.end() || found->second.transit.agent_id != agent_id) return nullptr;
    return &found->second.transit;
}

std::optional<grid::AvatarTransit> InboundTransitRegistry::consume(
    std::string_view session_id, std::chrono::steady_clock::time_point now) {
    purge(now);
    const auto found = entries_.find(std::string(session_id));
    if (found == entries_.end()) return std::nullopt;
    auto transit = std::move(found->second.transit);
    entries_.erase(found);
    return transit;
}

void InboundTransitRegistry::remove(std::string_view session_id) {
    entries_.erase(std::string(session_id));
}

std::size_t InboundTransitRegistry::size(std::chrono::steady_clock::time_point now) {
    purge(now);
    return entries_.size();
}

void InboundTransitRegistry::purge(std::chrono::steady_clock::time_point now) {
    std::erase_if(entries_, [&](const auto& entry) { return entry.second.expires_at <= now; });
}

std::string encode_child_agent_request(const ChildAgent& agent) {
    const std::array<double, 3> position{
        static_cast<double>(agent.position[0]),
        static_cast<double>(agent.position[1]),
        static_cast<double>(agent.position[2])};
    std::string worn{'['};
    for (std::size_t index = 0; index < agent.worn.size(); ++index) {
        if (index != 0) worn.push_back(',');
        worn += "{\"itemId\":" + quoted_string(agent.worn[index].item_id) +
            ",\"attachmentPoint\":" +
            std::to_string(static_cast<unsigned>(agent.worn[index].attachment_point)) + "}";
    }
    worn.push_back(']');
    // All-or-nothing: see has_appearance in the header.
    std::string appearance;
    if (agent.has_appearance()) {
        std::vector<std::byte> params;
        params.reserve(agent.visual_params.size());
        for (const auto value : agent.visual_params)
            params.push_back(static_cast<std::byte>(value));
        appearance = ",\"textureEntry\":" + quoted_string(session::base64(agent.texture_entry)) +
            ",\"visualParams\":" + quoted_string(session::base64(params)) +
            ",\"cofVersion\":" + std::to_string(agent.cof_version) +
            ",\"appearanceVersion\":" +
            std::to_string(static_cast<unsigned>(agent.appearance_version));
    }
    return std::string{"{\"agentId\":"} + quoted_string(agent.agent_id) +
        ",\"sessionId\":" + quoted_string(agent.session_id) +
        ",\"circuitCode\":" + std::to_string(agent.circuit_code) +
        ",\"homeRegionId\":" + quoted_string(agent.home_region_id) +
        ",\"position\":" + number_array(position) +
        ",\"worn\":" + worn + appearance + "}";
}

std::optional<ChildAgent> parse_child_agent_request(std::string_view document,
                                                   std::string* reason) {
    const auto refuse = [&](const char* because) {
        if (reason) *reason = because;
        return std::optional<ChildAgent>{};
    };
    ChildAgent agent;
    agent.agent_id = json_string_field(document, "agentId");
    agent.session_id = json_string_field(document, "sessionId");
    agent.home_region_id = json_string_field(document, "homeRegionId");
    const auto circuit_code = json_number_field(document, "circuitCode");
    const auto position = json_number_array<3>(document, "position");
    auto worn = json_worn_array(document, "worn");
    if (agent.agent_id.empty() || agent.session_id.empty() || agent.home_region_id.empty())
        return refuse("identity");
    // Absent or unreadable, not zero: zero is a real answer, see below.
    if (!circuit_code) return refuse("circuit_code");
    if (!position) return refuse("position");
    if (!worn) return refuse("worn");
    agent.worn = std::move(*worn);
    // The appearance is optional as a whole and indivisible within itself. One
    // half of it is not a smaller appearance, it is a broken one, and it would
    // reach a viewer as an avatar wearing part of itself.
    const auto texture_entry = json_string_field(document, "textureEntry");
    const auto visual_params = json_string_field(document, "visualParams");
    if (texture_entry.empty() != visual_params.empty()) return refuse("appearance_half");
    if (!texture_entry.empty()) {
        const auto entry = llsd::decode_base64(texture_entry);
        const auto params = llsd::decode_base64(visual_params);
        const auto cof_version = json_number_field(document, "cofVersion");
        const auto appearance_version = json_number_field(document, "appearanceVersion");
        if (!entry || !params || entry->empty() || params->empty())
            return refuse("appearance_unreadable");
        if (!cof_version || !appearance_version) return refuse("appearance_versions");
        // The bounds encode_avatar_appearance itself enforces. Refused here
        // rather than there, where the only outcome available is an empty
        // message that nobody can trace back to this.
        if (entry->size() > 65535 || params->size() > 255) return refuse("appearance_oversized");
        if (*cof_version < 0.0 ||
            *cof_version > static_cast<double>((std::numeric_limits<std::uint32_t>::max)()))
            return refuse("cof_version");
        if (*appearance_version < 0.0 || *appearance_version > 1.0)
            return refuse("appearance_version");
        agent.texture_entry = *entry;
        agent.visual_params.reserve(params->size());
        for (const auto value : *params)
            agent.visual_params.push_back(static_cast<std::uint8_t>(value));
        agent.cof_version = static_cast<std::uint32_t>(*cof_version);
        agent.appearance_version = static_cast<std::uint8_t>(*appearance_version);
    }
    // Zero means "this avatar has no viewer circuit", which is what every
    // avatar on the client session transport has: circuit_code is an LLUDP
    // concept and those sessions never acquire one. It was refused here as
    // malformed, on the reasoning that an absent field becomes zero once it is
    // a number — but an absent field is already refused above, as unreadable
    // rather than as zero. So the check cost nothing against a viewer and
    // rejected every offer a client-only region ever made: two such regions
    // refused each other's child agents 400 in both directions, always,
    // reported 2026-08-24 and reproduced from the client's own logs.
    if (*circuit_code < 0.0 ||
        *circuit_code > static_cast<double>((std::numeric_limits<std::uint32_t>::max)()))
        return refuse("circuit_code_range");
    for (const auto value : *position)
        if (!std::isfinite(value)) return refuse("position_not_finite");
    agent.circuit_code = static_cast<std::uint32_t>(*circuit_code);
    agent.position = {
        static_cast<float>((*position)[0]),
        static_cast<float>((*position)[1]),
        static_cast<float>((*position)[2])};
    // Whatever the request said about a seed is not carried: see the header.
    agent.seed.clear();
    return agent;
}

std::string encode_child_agent_acceptance(std::string_view seed) {
    return std::string{"{\"seed\":"} + quoted_string(std::string(seed)) + "}";
}

std::string parse_child_agent_acceptance(std::string_view document) {
    return std::string{json_string_field(document, "seed")};
}

const ChildAgent& ChildAgentRegistry::establish(
    ChildAgent agent, std::chrono::steady_clock::time_point now,
    std::chrono::seconds lifetime) {
    purge(now);
    const auto session_id = agent.session_id;
    const auto existing = entries_.find(session_id);
    if (existing != entries_.end()) {
        // Everything the source told us may have moved on; the seed may not.
        const auto minted = existing->second.agent.seed;
        existing->second.agent = std::move(agent);
        existing->second.agent.seed = minted;
        existing->second.expires_at = now + lifetime;
        return existing->second.agent;
    }
    auto& entry = entries_[session_id];
    entry.agent = std::move(agent);
    entry.expires_at = now + lifetime;
    return entry.agent;
}

bool ChildAgentRegistry::renew(std::string_view session_id,
                               std::chrono::steady_clock::time_point now,
                               std::chrono::seconds lifetime) {
    if (session_id.empty()) return false;
    purge(now);
    const auto found = entries_.find(std::string(session_id));
    if (found == entries_.end()) return false;
    found->second.expires_at = now + lifetime;
    return true;
}

const ChildAgent* ChildAgentRegistry::find(
    std::string_view session_id, std::chrono::steady_clock::time_point now) {
    if (session_id.empty()) return nullptr;
    purge(now);
    const auto found = entries_.find(std::string(session_id));
    return found == entries_.end() ? nullptr : &found->second.agent;
}

std::optional<ChildAgent> ChildAgentRegistry::promote(
    std::string_view session_id, std::chrono::steady_clock::time_point now) {
    if (session_id.empty()) return std::nullopt;
    purge(now);
    const auto found = entries_.find(std::string(session_id));
    if (found == entries_.end()) return std::nullopt;
    auto promoted = found->second.agent;
    entries_.erase(found);
    return promoted;
}

void ChildAgentRegistry::remove(std::string_view session_id) {
    if (session_id.empty()) return;
    entries_.erase(std::string(session_id));
}

std::vector<ChildAgent> ChildAgentRegistry::live(std::chrono::steady_clock::time_point now) {
    purge(now);
    std::vector<ChildAgent> children;
    children.reserve(entries_.size());
    for (const auto& [session_id, entry] : entries_) {
        static_cast<void>(session_id);
        children.push_back(entry.agent);
    }
    return children;
}

std::size_t ChildAgentRegistry::size(std::chrono::steady_clock::time_point now) {
    purge(now);
    return entries_.size();
}

void ChildAgentRegistry::purge(std::chrono::steady_clock::time_point now) {
    std::erase_if(entries_, [&](const auto& entry) {
        return entry.second.expires_at <= now;
    });
}

bool CapabilityArrivalGate::mark_seed_served(
    std::string_view session_id, std::string_view visit_id) {
    if (session_id.empty() || visit_id.empty()) return false;
    return served_seeds_.insert(key(session_id, visit_id)).second;
}

bool CapabilityArrivalGate::consume_seed(
    std::string_view session_id, std::string_view visit_id) {
    if (session_id.empty() || visit_id.empty()) return false;
    return served_seeds_.erase(key(session_id, visit_id)) != 0;
}

void CapabilityArrivalGate::clear_session(std::string_view session_id) {
    if (session_id.empty()) return;
    const auto prefix = std::string(session_id) + '|';
    std::erase_if(served_seeds_, [&](const std::string& value) {
        return value.starts_with(prefix);
    });
}

std::size_t CapabilityArrivalGate::size() const {
    return served_seeds_.size();
}

std::string CapabilityArrivalGate::key(
    std::string_view session_id, std::string_view visit_id) {
    return std::string(session_id) + '|' + std::string(visit_id);
}

} // namespace homeworldz::region
