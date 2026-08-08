#include <array>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <charconv>
#include <cctype>
#include <csignal>
#include <chrono>
#include <thread>
#include <cmath>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <set>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <iomanip>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "homeworldz/api_models.h"
#include "homeworldz/appearance_bake.h"
#include "homeworldz/avatar_controller.h"
#include "homeworldz/capability_paths.h"
#include "homeworldz/llsd_binary.h"
#include "homeworldz/render_material.h"
#include "homeworldz/texture_entry.h"
#include "homeworldz/falcon_runtime.h"
#include "homeworldz/grid_client.h"
#include "homeworldz/session_server.h"
#include "homeworldz/terrain_layers.h"
#include "homeworldz/http_response.h"
#include "homeworldz/inventory_asset.h"
#include "homeworldz/object_asset.h"
#include "homeworldz/parcel.h"
#include "homeworldz/physics_adapters.h"
#include "homeworldz/physics_scene.h"
#include "homeworldz/region_config.h"
#include "homeworldz/region_storage.h"
#include "homeworldz/mesh_acceptance.h"
#include "homeworldz/mesh_convert.h"
#include "homeworldz/mesh_model_upload.h"
#include "homeworldz/slmesh.h"
#include "homeworldz/region_transit.h"
#include "homeworldz/scene.h"
#include "homeworldz/sha256.h"
#include "homeworldz/simulation_loop.h"
#include "homeworldz/terrain_edit.h"
#include "homeworldz/viewer_capabilities.h"
#include "homeworldz/viewer_protocol.h"
#include "homeworldz/visual_params.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_handle = SOCKET;
using socket_length = int;
constexpr socket_handle invalid_socket = INVALID_SOCKET;
static void close_socket(socket_handle socket) { closesocket(socket); }
static void set_socket_deadline(socket_handle socket, int milliseconds) {
    const DWORD timeout = static_cast<DWORD>(milliseconds);
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
}
static void set_socket_blocking_mode(socket_handle socket, bool blocking) {
    u_long mode = blocking ? 0u : 1u;
    ioctlsocket(socket, FIONBIO, &mode);
}
static bool socket_would_block() { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
using socket_handle = int;
using socket_length = socklen_t;
constexpr socket_handle invalid_socket = -1;
static void close_socket(socket_handle socket) { close(socket); }
static void set_socket_deadline(socket_handle socket, int milliseconds) {
    timeval timeout{milliseconds / 1000, (milliseconds % 1000) * 1000};
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}
static void set_socket_blocking_mode(socket_handle socket, bool blocking) {
    const int flags = fcntl(socket, F_GETFL, 0);
    if (flags < 0) return;
    fcntl(socket, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
}
static bool socket_would_block() {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}
#endif

namespace {
std::atomic_bool running{true};
// Applied to every accepted HTTP connection. It bounds a single blocking recv
// or send, not a whole transfer, so a live mesh upload keeps its time while a
// silent peer is dropped rather than stalling the region's only loop.
constexpr int http_client_timeout_ms = 15000;
// A ceiling on connections held mid-request. Reached only by a peer opening
// sockets faster than they time out; past it the listen queue does the refusing,
// which is the right place for it — unlike the old behaviour, where the queue
// filled because nothing was draining it.
constexpr std::size_t maximum_incoming_http = 256;
constexpr std::string_view system_creator_id = "00000000-0000-0000-0000-000000000002";
constexpr std::string_view default_map_tile_asset_id = "00000000-0000-1111-9999-000000000100";
homeworldz::config::RegionSettings configured_values;

// AvatarTransport names which wire drives a participant: the legacy LLUDP
// circuit, or the WebSocket region session (docs/CLIENT2-EMBODIMENT.md). The
// avatars map is keyed by participant — "ip:port" for LLUDP so existing send
// sites keep working verbatim, "ws:<session_id>" for sessions.
enum class AvatarTransport { lludp, session };

struct LiveAvatar {
    LiveAvatar(homeworldz::viewer::AvatarController initial_controller,
               homeworldz::scene::EntityId initial_entity_id, std::string initial_user_id,
               std::chrono::steady_clock::time_point initial_next_ping,
               std::chrono::steady_clock::time_point initial_next_presence,
               std::chrono::steady_clock::time_point initial_next_transform,
               homeworldz::scene::Vector3 initial_sent_position)
        : controller(std::move(initial_controller)), entity_id(initial_entity_id),
          user_id(std::move(initial_user_id)), next_ping(initial_next_ping),
          next_presence(initial_next_presence), next_transform(initial_next_transform),
          last_sent_position(initial_sent_position) {}

    homeworldz::viewer::AvatarController controller;
    homeworldz::scene::EntityId entity_id{};
    AvatarTransport transport{AvatarTransport::lludp};
    std::string user_id;
    std::string session_id;              // formatted UUID, for Event Queue delivery
    std::uint32_t circuit_code{};        // LLUDP only; stored so identity needs no circuit lookup
    std::int32_t last_agent_parcel{};    // local id of the parcel last reported to this viewer
    std::chrono::steady_clock::time_point next_ping{};
    std::chrono::steady_clock::time_point next_presence{};
    std::chrono::steady_clock::time_point next_transform{};
    std::chrono::steady_clock::time_point last_pong{};  // last CompletePingCheck reply from this viewer
    homeworldz::scene::Vector3 last_sent_position{};
    homeworldz::scene::Vector3 last_sent_velocity{};
    std::array<float, 3> last_sent_rotation{};
    std::uint8_t ping_id{};
    std::chrono::steady_clock::time_point last_agent_update{};
    std::uint32_t last_agent_update_sequence{};
    bool has_agent_update{};
    homeworldz::physics::CharacterId physics_character{};
    std::chrono::steady_clock::time_point restored_flying_until{};
    std::string outbound_transit_id;
    std::chrono::steady_clock::time_point outbound_transit_expires{};
    std::chrono::steady_clock::time_point next_crossing_attempt{};
};

bool sequence_is_newer(std::uint32_t candidate, std::uint32_t current) {
    return static_cast<std::int32_t>(candidate - current) > 0;
}

struct QueuedTexturePacket {
    std::string asset_id;
    std::vector<std::byte> payload;
    bool last{};
};

struct PendingInventoryUpload {
    std::string session_id;
    std::string agent_id;
    std::string item_id;
    std::string asset_id;
    homeworldz::viewer::NewFileInventoryUpload request;
};

// A mesh model upload between its two POSTs (ADR 0033 M2): the fee request
// carried the metadata, the upload to the minted URL carries only the
// resources, so the metadata waits here keyed by the URL's token.
struct PendingMeshModelUpload {
    std::string session_id;
    std::string agent_id;
    homeworldz::mesh_model::Metadata metadata;
};

struct PendingInventoryAssetUpdate {
    std::string session_id;
    std::string agent_id;
    std::string item_id;
    std::string asset_id;
    std::int8_t asset_type{};
    std::int8_t inventory_type{};
    std::string task_id;
    bool script_running{};
};

struct PendingInventoryAssetUpload {
    std::string asset_id;
    std::int8_t asset_type{};
};

struct PendingInventoryAssetXfer {
    std::string transaction_id;
    std::string asset_id;
    homeworldz::viewer::Uuid asset_uuid{};
    std::int8_t asset_type{};
    std::size_t expected_size{};
    std::size_t packet_size{1000};
    std::vector<std::byte> data;
    std::unordered_set<std::uint32_t> received_packets;
};

struct PendingTaskInventoryXfer {
    std::vector<std::byte> data;
    std::size_t offset{};
    std::uint32_t next_packet{1};
    std::uint32_t awaiting_confirmation{};
};

struct SentDynamicTransform {
    homeworldz::physics::BodyState state;
    std::chrono::steady_clock::time_point sent_at;
};

std::string task_inventory_type_name(std::int8_t type) {
    switch (type) {
    case 0: return "texture";
    case 1: return "sound";
    case 2: return "callcard";
    case 3: return "landmark";
    case 5: return "clothing";
    case 6: return "object";
    case 7: return "notecard";
    case 10: return "lsltext";
    case 13: return "bodypart";
    case 20: return "animation";
    case 21: return "gesture";
    default: return "unknown";
    }
}

std::string task_inventory_inv_type_name(std::int8_t type) {
    switch (type) {
    case 0: return "texture";
    case 1: return "sound";
    case 2: return "callcard";
    case 3: return "landmark";
    case 6: return "object";
    case 7: return "notecard";
    case 10: return "lsl";
    case 15: return "snapshot";
    case 17: return "attachment";
    case 18: return "wearable";
    case 19: return "animation";
    case 20: return "gesture";
    default: return "unknown";
    }
}

std::string permission_hex(std::uint32_t value) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(8) << value;
    return output.str();
}

std::string task_inventory_field(std::string_view value) {
    std::string result(value);
    for (auto& character : result)
        if (character == '|' || static_cast<unsigned char>(character) < 0x20)
            character = ' ';
    return result;
}

std::vector<std::byte> task_inventory_file(const homeworldz::scene::Entity& entity) {
    std::string text = "\tinv_object\t0\n\t{\n\t\tobj_id\t" + entity.object_id +
        "\n\t\tparent_id\t00000000-0000-0000-0000-000000000000\n"
        "\t\ttype\tcategory\n\t\tname\tContents|\n";
    for (const auto& item : entity.task_inventory) {
        text += "\t}\n\tinv_item\t0\n\t{\n\t\titem_id\t" + item.item_id +
            "\n\t\tparent_id\t" + entity.object_id +
            "\n\tpermissions 0\n\t{\n\t\tbase_mask\t" + permission_hex(item.base_permissions) +
            "\n\t\towner_mask\t" + permission_hex(item.current_permissions) +
            "\n\t\tgroup_mask\t" + permission_hex(item.group_permissions) +
            "\n\t\teveryone_mask\t" + permission_hex(item.everyone_permissions) +
            "\n\t\tnext_owner_mask\t" + permission_hex(item.next_permissions) +
            "\n\t\tcreator_id\t" + item.creator_id +
            "\n\t\towner_id\t" + item.owner_id +
            "\n\t\tlast_owner_id\t" + item.last_owner_id +
            "\n\t\tgroup_id\t" + item.group_id +
            "\n\t}\n\t\tasset_id\t" + item.asset_id +
            "\n\t\ttype\t" + task_inventory_type_name(item.asset_type) +
            "\n\t\tinv_type\t" + task_inventory_inv_type_name(item.inventory_type) +
            "\n\t\tflags\t" + permission_hex(item.flags) +
            "\n\tsale_info\t0\n\t{\n\t\tsale_type\tnot\n\t\tsale_price\t0\n\t}\n"
            "\t\tname\t" + task_inventory_field(item.name) +
            "|\n\t\tdesc\t" + task_inventory_field(item.description) +
            "|\n\t\tcreation_date\t" + std::to_string(item.creation_date) + "\n";
    }
    text += "\t}";
    text.push_back('\0');
    return std::vector<std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                  reinterpret_cast<const std::byte*>(text.data() + text.size()));
}

struct PendingEventResponse {
    socket_handle client{invalid_socket};
    std::string request;
    std::string session_id;
    std::chrono::steady_clock::time_point deadline{};
};

struct PendingAgentMovementComplete {
    std::string endpoint;
    std::string session_id;
    std::string visit_id;
    std::vector<std::byte> payload;
    std::chrono::steady_clock::time_point deadline{};
};

void stop(int) { running = false; }

std::string configured_value(std::string_view name, std::string fallback = {}) {
    const auto configured = configured_values.find(std::string(name));
    if (configured != configured_values.end()) return configured->second;
    return fallback;
}

std::unique_ptr<homeworldz::terrain::Heightmap> load_raw_heightmap(
    const std::filesystem::path& path, std::size_t width) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return {};
    const auto byte_count = static_cast<std::size_t>(input.tellg());

    // A four-byte-per-sample file is the exact heightfield /map/terrain.raw
    // emits, and the same layout OpenSimulator calls RAW32 and reads from a
    // .r32 file, so ground can move between the two.
    // The one-byte form below stores whole metres only, which
    // cannot express a graded slope: the 65-degree test face on Gamma rises
    // 2.145 m per metre, and rounding that to integers turns one constant angle
    // into an alternating staircase of two wrong ones.
    if (byte_count == width * width * sizeof(float)) {
        input.seekg(0);
        auto result = std::make_unique<homeworldz::terrain::Heightmap>(width);
        std::vector<float> samples(width * width);
        input.read(reinterpret_cast<char*>(samples.data()),
                   static_cast<std::streamsize>(byte_count));
        if (!input) return {};
        for (std::size_t index = 0; index < samples.size(); ++index) {
            (*result)[index] = samples[index];
        }
        return result;
    }

    const auto source_width = byte_count == width * width ? width :
        (byte_count == 256 * 256 ? std::size_t{256} : std::size_t{});
    if (source_width == 0) return {};
    input.seekg(0);
    std::vector<unsigned char> source(byte_count);
    input.read(reinterpret_cast<char*>(source.data()), source.size());
    if (!input) return {};
    auto result = std::make_unique<homeworldz::terrain::Heightmap>(width);
    for (std::size_t y = 0; y < width; ++y) {
        const auto source_y = y * (source_width - 1) / (width - 1);
        for (std::size_t x = 0; x < width; ++x) {
            const auto source_x = x * (source_width - 1) / (width - 1);
            (*result)[y * width + x] = static_cast<float>(source[source_y * source_width + source_x]);
        }
    }
    return result;
}

std::string encode_heightmap(const homeworldz::terrain::Heightmap& heightmap) {
    std::string output;
    output.reserve(heightmap.size() * sizeof(float));
    for (const auto height : heightmap) {
        const auto bits = std::bit_cast<std::uint32_t>(height);
        output.push_back(static_cast<char>(bits));
        output.push_back(static_cast<char>(bits >> 8));
        output.push_back(static_cast<char>(bits >> 16));
        output.push_back(static_cast<char>(bits >> 24));
    }
    return output;
}

homeworldz::scene::Vector3 default_spawn(const homeworldz::terrain::Heightmap& heightmap) {
    const auto width = heightmap.width();
    const auto center = width / 2;
    std::size_t selected_x = center;
    std::size_t selected_y = center;
    std::size_t selected_distance = (std::numeric_limits<std::size_t>::max)();
    for (std::size_t y = 16; y < width - 16; ++y) {
        for (std::size_t x = 16; x < width - 16; ++x) {
            const auto height = heightmap[y * width + x];
            if (height < 24.0F) continue;
            const auto dx = static_cast<std::int64_t>(x) - static_cast<std::int64_t>(center);
            const auto dy = static_cast<std::int64_t>(y) - static_cast<std::int64_t>(center);
            const auto distance = static_cast<std::size_t>(dx * dx + dy * dy);
            if (distance < selected_distance) {
                selected_x = x;
                selected_y = y;
                selected_distance = distance;
            }
        }
    }
    const auto height = heightmap[selected_y * width + selected_x];
    return {static_cast<double>(selected_x), static_cast<double>(selected_y), height + 1.0};
}

double ground_height(const homeworldz::terrain::Heightmap& heightmap,
                      const homeworldz::scene::Vector3& position) {
    const auto maximum = static_cast<int>(heightmap.width() - 1);
    const auto x = std::clamp(static_cast<int>(position.x), 0, maximum);
    const auto y = std::clamp(static_cast<int>(position.y), 0, maximum);
    return heightmap[static_cast<std::size_t>(y) * heightmap.width() + x];
}

int configured_int(std::string_view name, int fallback, int minimum, int maximum) {
    const auto value = configured_value(name);
    if (value.empty()) return fallback;
    int parsed{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size() &&
        parsed >= minimum && parsed <= maximum ? parsed : fallback;
}

int configured_port() {
    return configured_int("region.http_port", 42001, 1, 65535);
}

// The arrival greeting (region.welcome_message), delivered privately to each
// avatar as it enters — the llOwnerSay shape on the viewer path, a chat
// envelope on the session path. {region} and {user} resolve to the region
// name and the avatar's display name.
//
// Silent by default, deliberately: this fires on every entry including the
// border crossings people make constantly, so the arrival greeting proper is
// the grid's ([grid] welcome_message, once per login). A region speaks here
// only when its operator has something region-specific worth repeating —
// "You are in Sandbox, the build area" — which is the minority case (client
// core observation, and the operator's own expectation, 2026-07-29).
std::string welcome_chat_message(std::string_view display_name, std::string_view region) {
    auto message = configured_value("region.welcome_message");
    if (message.empty()) return {};
    const auto substitute = [&](std::string_view placeholder, std::string_view value) {
        for (auto at = message.find(placeholder); at != std::string::npos;
             at = message.find(placeholder, at + value.size()))
            message.replace(at, placeholder.size(), value);
    };
    substitute("{region}", region);
    substitute("{user}", display_name);
    return message;
}

int configured_viewer_port() {
    return configured_int("region.viewer_port", 42002, 1, 65535);
}

bool configured_bind_address(sockaddr_in& address, std::string_view setting_name, int port) {
    address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(port));
    const auto host = configured_value(setting_name, "127.0.0.1");
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) == 1) return true;
    std::cerr << "{\"level\":\"error\",\"message\":\"invalid IPv4 bind address\",\"setting\":"
              << homeworldz::api::json_string(setting_name) << ",\"address\":"
              << homeworldz::api::json_string(host) << "}" << std::endl;
    return false;
}

std::string udp_endpoint(const sockaddr_in& address) {
    std::array<char, INET_ADDRSTRLEN> ip{};
    if (!inet_ntop(AF_INET, &address.sin_addr, ip.data(), ip.size())) return {};
    return std::string(ip.data()) + ':' + std::to_string(ntohs(address.sin_port));
}

bool send_udp(socket_handle socket, std::string_view endpoint, std::span<const std::byte> bytes) {
    const auto colon = endpoint.rfind(':');
    if (colon == std::string_view::npos) return false;
    unsigned port{};
    const auto port_text = endpoint.substr(colon + 1);
    const auto parsed = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (parsed.ec != std::errc{} || parsed.ptr != port_text.data() + port_text.size() || port > 65535) return false;
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(static_cast<unsigned short>(port));
    const std::string ip(endpoint.substr(0, colon));
    if (inet_pton(AF_INET, ip.c_str(), &destination.sin_addr) != 1) return false;
    return sendto(socket, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
                  reinterpret_cast<const sockaddr*>(&destination), sizeof(destination)) == static_cast<int>(bytes.size());
}

// How far an accumulated request has got. Reading is incremental because the
// region has exactly one loop: it renews the lease, services viewer UDP, and
// serves HTTP, so any blocking read here is a stall in all three. Deciding
// completeness from the buffer alone is what lets the read be spread across
// loop iterations instead of held open on the socket.
enum class HttpRequestState { need_more, complete, invalid };

// Consumes a request buffer, truncating it to exactly one request when whole.
// Same limits and same mesh-route exceptions the blocking reader applied; the
// difference is only that this returns rather than waits.
HttpRequestState http_request_state(std::string& request) {
    constexpr std::size_t maximum_header_size = 64 * 1024;
    constexpr std::size_t maximum_body_size = 1024 * 1024;
    const std::size_t maximum_mesh_body = homeworldz::mesh::max_glb_bytes + 64 * 1024;
    const auto header_end = request.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return request.size() > maximum_header_size ? HttpRequestState::invalid
                                                    : HttpRequestState::need_more;
    }
    const std::string_view head(request.data(), header_end + 4);
    std::size_t body_limit = maximum_body_size;
    if (head.starts_with("POST " + std::string(homeworldz::mesh::upload_path)))
        body_limit = maximum_mesh_body;
    if (head.starts_with("POST /caps/upload-file/") ||
        head.starts_with("POST /caps/upload-model-data/"))
        body_limit = (maximum_mesh_body * 4) / 3 + 64 * 1024;
    const auto content_length = homeworldz::http::request_content_length(head);
    if (!content_length || *content_length > body_limit) return HttpRequestState::invalid;
    const auto expected = header_end + 4 + *content_length;
    if (request.size() < expected) return HttpRequestState::need_more;
    request.resize(expected);
    return HttpRequestState::complete;
}

std::optional<std::string> receive_http_request(socket_handle client) {
    constexpr std::size_t maximum_header_size = 64 * 1024;
    constexpr std::size_t maximum_body_size = 1024 * 1024;
    // The mesh upload route alone accepts a body up to the published GLB cap
    // (plus header slack); every other route keeps the tight general bound.
    const std::size_t maximum_mesh_body =
        homeworldz::mesh::max_glb_bytes + 64 * 1024;
    std::string request;
    std::array<char, 4096> buffer{};
    std::optional<std::size_t> expected_size;
    std::size_t body_limit = maximum_body_size;
    while (request.size() <= maximum_header_size + body_limit) {
        const auto received = recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (received <= 0) return std::nullopt;
        request.append(buffer.data(), static_cast<std::size_t>(received));
        if (!expected_size) {
            const auto header_end = request.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                if (request.size() > maximum_header_size) return std::nullopt;
                continue;
            }
            const std::string_view head(request.data(), header_end + 4);
            if (head.starts_with("POST " + std::string(homeworldz::mesh::upload_path)))
                body_limit = maximum_mesh_body;
            // The viewer model upload carries whole type-49 payloads in
            // LLSD XML, whose base64 adds a third again over the raw cap.
            if (head.starts_with("POST /caps/upload-file/") ||
                head.starts_with("POST /caps/upload-model-data/"))
                body_limit = (maximum_mesh_body * 4) / 3 + 64 * 1024;
            const auto content_length = homeworldz::http::request_content_length(head);
            if (!content_length || *content_length > body_limit) return std::nullopt;
            expected_size = header_end + 4 + *content_length;
        }
        if (request.size() >= *expected_size) {
            request.resize(*expected_size);
            return request;
        }
    }
    return std::nullopt;
}

bool send_all(socket_handle client, std::string_view content) {
    std::size_t sent = 0;
    while (sent < content.size()) {
        const auto count = send(client, content.data() + sent, static_cast<int>(content.size() - sent), 0);
        if (count <= 0) return false;
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

void finish_http_response(socket_handle client) {
#ifdef _WIN32
    shutdown(client, SD_SEND);
#else
    shutdown(client, SHUT_WR);
#endif
}

struct InternalAssetRequest {
    std::string asset_id;
    bool replicate{};
};

std::optional<InternalAssetRequest> internal_asset_request(std::string_view path) {
    constexpr std::string_view prefix = "/api/v1/assets/";
    if (!path.starts_with(prefix)) return std::nullopt;
    auto asset_id = path.substr(prefix.size());
    constexpr std::string_view replicate_suffix = "/replicate";
    bool replicate = false;
    if (asset_id.ends_with(replicate_suffix)) {
        asset_id.remove_suffix(replicate_suffix.size());
        replicate = true;
    }
    if (asset_id.empty() || asset_id.find('/') != std::string_view::npos ||
        !homeworldz::viewer::parse_uuid(asset_id)) return std::nullopt;
    return InternalAssetRequest{std::string(asset_id), replicate};
}

std::optional<std::pair<std::string, std::string>> baked_upload_data_request(std::string_view path) {
    constexpr std::string_view prefix = "/caps/upload-baked-data/";
    if (!path.starts_with(prefix)) return std::nullopt;
    const auto separator = path.find('/', prefix.size());
    if (separator == std::string_view::npos) return std::nullopt;
    const auto session = path.substr(prefix.size(), separator - prefix.size());
    const auto token = path.substr(separator + 1);
    if (session.empty() || token.empty() || token.find('/') != std::string_view::npos) return std::nullopt;
    return std::pair{std::string(session), std::string(token)};
}

std::optional<std::pair<std::string, std::string>> file_upload_data_request(std::string_view path) {
    constexpr std::string_view prefix = "/caps/upload-file-data/";
    if (!path.starts_with(prefix)) return std::nullopt;
    const auto separator = path.find('/', prefix.size());
    if (separator == std::string_view::npos) return std::nullopt;
    const auto session = path.substr(prefix.size(), separator - prefix.size());
    const auto token = path.substr(separator + 1);
    if (session.empty() || token.empty() || token.find('/') != std::string_view::npos) return std::nullopt;
    return std::pair{std::string(session), std::string(token)};
}

// GET /session/assets/{uuid}: canonical bytes for a session client, keyed by
// the asset id an upload reply or an object's geometry block named.
std::optional<std::string> session_asset_request(std::string_view path) {
    constexpr std::string_view prefix = "/session/assets/";
    if (!path.starts_with(prefix)) return std::nullopt;
    const auto asset = path.substr(prefix.size());
    if (!homeworldz::viewer::parse_uuid(asset)) return std::nullopt;
    return std::string(asset);
}

std::optional<std::pair<std::string, std::string>> model_upload_data_request(std::string_view path) {
    constexpr std::string_view prefix = "/caps/upload-model-data/";
    if (!path.starts_with(prefix)) return std::nullopt;
    const auto separator = path.find('/', prefix.size());
    if (separator == std::string_view::npos) return std::nullopt;
    const auto session = path.substr(prefix.size(), separator - prefix.size());
    const auto token = path.substr(separator + 1);
    if (session.empty() || token.empty() || token.find('/') != std::string_view::npos) return std::nullopt;
    return std::pair{std::string(session), std::string(token)};
}

std::optional<std::pair<std::string, std::string>> inventory_asset_update_data_request(
    std::string_view path) {
    constexpr std::string_view prefix = "/caps/update-inventory-asset-data/";
    if (!path.starts_with(prefix)) return std::nullopt;
    const auto separator = path.find('/', prefix.size());
    if (separator == std::string_view::npos) return std::nullopt;
    const auto session = path.substr(prefix.size(), separator - prefix.size());
    const auto token = path.substr(separator + 1);
    if (session.empty() || token.empty() || token.find('/') != std::string_view::npos)
        return std::nullopt;
    return std::pair{std::string(session), std::string(token)};
}

std::string_view http_request_body(std::string_view request) {
    const auto separator = request.find("\r\n\r\n");
    return separator == std::string_view::npos ? std::string_view{} : request.substr(separator + 4);
}

// EstablishAgentCommunication's sim-ip-and-port is parsed by the viewer with
// inet_addr, which does not resolve names. The field only ever held a literal
// address while region endpoints were loopback; once a region is configured by
// hostname, passing the authority through unresolved emits a value the viewer
// silently rejects. Resolve it here, the way simulator_event_endpoint below
// already does for the same reason.
std::string simulator_endpoint(std::string_view public_endpoint, int viewer_port) {
    auto authority = public_endpoint;
    const auto scheme = authority.find("://");
    if (scheme != std::string_view::npos) authority.remove_prefix(scheme + 3);
    const auto slash = authority.find('/');
    if (slash != std::string_view::npos) authority = authority.substr(0, slash);
    const auto colon = authority.rfind(':');
    if (colon != std::string_view::npos) authority = authority.substr(0, colon);
    const auto host = std::string(authority);
    if (host.empty()) return {};
    in_addr literal{};
    if (inet_pton(AF_INET, host.c_str(), &literal) == 1)
        return host + ':' + std::to_string(viewer_port);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* addresses{};
    if (getaddrinfo(host.c_str(), nullptr, &hints, &addresses) != 0) return {};
    std::string resolved;
    for (auto* address = addresses; address; address = address->ai_next) {
        if (address->ai_family != AF_INET || address->ai_addrlen < sizeof(sockaddr_in)) continue;
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address->ai_addr);
        std::array<char, INET_ADDRSTRLEN> text{};
        if (inet_ntop(AF_INET, &ipv4->sin_addr, text.data(), text.size()))
            resolved = std::string(text.data()) + ':' + std::to_string(viewer_port);
        break;
    }
    freeaddrinfo(addresses);
    return resolved;
}

// The wearable assets this region ships, in wearable-type order. A viewer that
// asks to create a wearable sends no body with the request, and a wearable body
// is an LLWearable document that cannot be generated from nothing the way a
// notecard or script stub can. These are the same six the default-outfit bake
// uses; seeding a new item from the matching one gives the viewer something
// valid to edit and upload over. Types beyond these have no shipped default.
std::string_view shipped_default_wearable_asset(std::uint32_t wearable_type) {
    switch (wearable_type) {
        case 0: return "66c41e39-38f9-f75a-024e-585989bfab73";  // Shape
        case 1: return "00000000-0000-1111-9999-000000000202";  // Skin
        case 2: return "d342e6c0-b9d2-11dc-95ff-0800200c9a66";  // Hair
        case 3: return "4bb6fa4d-1cd2-498a-a84c-95c1a0e745a7";  // Eyes
        case 4: return "00000000-38f9-1111-024e-222222111110";  // Shirt
        case 5: return "00000000-38f9-1111-024e-222222111120";  // Pants
        default: return {};
    }
}

std::optional<homeworldz::viewer::SimulatorEventEndpoint> simulator_event_endpoint(
    std::string_view public_endpoint, int viewer_port) {
    auto authority = public_endpoint;
    const auto scheme = authority.find("://");
    if (scheme != std::string_view::npos) authority.remove_prefix(scheme + 3);
    const auto slash = authority.find('/');
    if (slash != std::string_view::npos) authority = authority.substr(0, slash);
    const auto colon = authority.rfind(':');
    if (colon != std::string_view::npos) authority = authority.substr(0, colon);
    const auto host = std::string(authority);
    if (host.empty() || viewer_port < 1 || viewer_port > 65535) return std::nullopt;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* addresses{};
    if (getaddrinfo(host.c_str(), nullptr, &hints, &addresses) != 0) return std::nullopt;
    std::optional<homeworldz::viewer::SimulatorEventEndpoint> result;
    for (auto* address = addresses; address; address = address->ai_next) {
        if (address->ai_family != AF_INET || address->ai_addrlen < sizeof(sockaddr_in)) continue;
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address->ai_addr);
        const auto bytes = reinterpret_cast<const std::uint8_t*>(&ipv4->sin_addr.s_addr);
        result = homeworldz::viewer::SimulatorEventEndpoint{
            {bytes[0], bytes[1], bytes[2], bytes[3]}, static_cast<std::uint16_t>(viewer_port)};
        break;
    }
    freeaddrinfo(addresses);
    return result;
}

void apply_material_contact_defaults(homeworldz::scene::Entity& entity) {
    const auto material = homeworldz::physics::material_properties(entity.material);
    entity.physics_friction = material.friction;
    entity.physics_restitution = material.restitution;
}

// The RenderMaterials envelope: a viewer's request and the region's reply are
// both LLSD XML carrying one "Zipped" binary member, inside which is
// zlib-deflated LLSD binary. Two layers, because the outer one is what the
// capability framework speaks and the inner one is what the materials protocol
// speaks.
std::optional<std::vector<std::byte>> zipped_member(std::string_view body) {
    const auto document = homeworldz::llsd::parse_xml(body);
    if (!document) return std::nullopt;
    const auto* zipped = document->find("Zipped");
    if (zipped == nullptr || zipped->type != homeworldz::llsd::Value::Type::binary)
        return std::nullopt;
    return zipped->binary;
}

std::string zipped_llsd_reply(const homeworldz::llsd::Value& value) {
    const auto deflated = homeworldz::llsd::deflate_bytes(homeworldz::llsd::to_binary(value));
    return "<?xml version=\"1.0\"?><llsd><map><key>Zipped</key>"
           "<binary encoding=\"base64\">" +
           homeworldz::session::base64(deflated) + "</binary></map></llsd>";
}

// A stored material's id is kept as text; the wire wants the sixteen bytes.
std::vector<std::byte> material_id_bytes(std::string_view text) {
    std::vector<std::byte> out;
    int high = -1;
    for (const auto character : text) {
        if (character == '-') continue;
        int digit = -1;
        if (character >= '0' && character <= '9') digit = character - '0';
        else if (character >= 'a' && character <= 'f') digit = character - 'a' + 10;
        else if (character >= 'A' && character <= 'F') digit = character - 'A' + 10;
        else return {};
        if (high < 0) high = digit;
        else {
            out.push_back(static_cast<std::byte>((high << 4) | digit));
            high = -1;
        }
    }
    return out.size() == 16 ? out : std::vector<std::byte>{};
}

const std::vector<std::byte>& default_prim_texture_entry() {
    static const auto entry = [] {
        const auto plywood = homeworldz::viewer::parse_uuid("89556747-24cb-43ed-920b-47caed15465f");
        if (!plywood) throw std::logic_error("default plywood texture UUID is invalid");
        return homeworldz::viewer::default_texture_entry(*plywood);
    }();
    return entry;
}

// What an *uploaded model* wears when it brought no textures of its own.
//
// A newly rezzed prim is plywood, which is right: it is the Second Life
// convention, it is unmistakably a placeholder, and a creator who rezzes a box
// expects it. An uploaded model is a different thing. Its creator made geometry
// and is looking at whether the geometry arrived, and plywood's loud grain reads
// as a texture that went wrong rather than as one that was never there - it
// obscures the surface being inspected and invites the question "why is my model
// wood?". Blank shows the shape.
//
// IMG_WHITE rather than a null id: the viewer names this UUID as a real asset
// ("dataserver" in indra_constants.cpp), and this grid serves it - checked, not
// assumed, because plywood is also a dataserver asset and renders here largely
// because viewers cache it from Second Life. A texture that resolves only for
// people who have been to another grid is not a default.
const std::vector<std::byte>& blank_prim_texture_entry() {
    static const auto entry = [] {
        const auto blank = homeworldz::viewer::parse_uuid("5748decc-f629-461c-9a36-a35a221fe21f");
        if (!blank) throw std::logic_error("blank texture UUID is invalid");
        return homeworldz::viewer::default_texture_entry(*blank);
    }();
    return entry;
}

// The same texture as a bare id, for the per-face default of a model that
// textured some faces and not others.
homeworldz::viewer::Uuid blank_texture_id() {
    static const auto id =
        homeworldz::viewer::parse_uuid("5748decc-f629-461c-9a36-a35a221fe21f").value();
    return id;
}

void apply_extra_physics(
    homeworldz::scene::Entity& entity, const homeworldz::viewer::ObjectFlagUpdate& update) {
    entity.physics_shape_type = std::min<std::uint8_t>(update.physics_shape_type, 0x02);
    entity.physics_density = std::clamp(static_cast<double>(update.density), 1.0, 22587.0);
    entity.physics_friction = std::clamp(static_cast<double>(update.friction), 0.0, 255.0);
    entity.physics_restitution = std::clamp(static_cast<double>(update.restitution), 0.0, 1.0);
    entity.physics_gravity_multiplier =
        std::clamp(static_cast<double>(update.gravity_multiplier), -1.0, 28.0);
}

// The Extra Physics values to report to a viewer for one entity. The region is
// the only source the viewer has for these.
homeworldz::viewer::ObjectPhysicsProperties physics_properties_of(
    const homeworldz::scene::Entity& entity) {
    // Scene entity ids are 64-bit; the viewer addresses objects by a 32-bit
    // LocalID, the same narrowing every other object update performs.
    return {static_cast<std::uint32_t>(entity.id), entity.physics_shape_type,
            entity.physics_density, entity.physics_friction, entity.physics_restitution,
            entity.physics_gravity_multiplier};
}

std::optional<homeworldz::viewer::StaticObject> static_object_from_entity(
    const homeworldz::scene::Scene& scene, const homeworldz::scene::Entity& entity,
    std::string_view recipient_id, const homeworldz::script::FalconRuntime& falcon) {
    constexpr std::uint32_t object_scripted = 0x00000040;
    constexpr std::uint32_t object_handle_touch = 0x00000080;
    constexpr std::uint32_t object_modify = 0x00000004;
    constexpr std::uint32_t object_copy = 0x00000008;
    constexpr std::uint32_t object_any_owner = 0x00000010;
    constexpr std::uint32_t object_you_owner = 0x00000020;
    constexpr std::uint32_t object_move = 0x00000100;
    constexpr std::uint32_t object_transfer = 0x00020000;
    constexpr std::uint32_t object_owner_modify = 0x10000000;
    constexpr std::uint32_t object_physics = 0x00000001;
    constexpr std::uint32_t object_phantom = 0x00000400;
    constexpr std::uint32_t object_temporary = 0x40000000;
    if (entity.object_id.empty() || entity.id > (std::numeric_limits<std::uint32_t>::max)())
        return std::nullopt;
    const auto object_id = homeworldz::viewer::parse_uuid(entity.object_id);
    const auto owner_id = homeworldz::viewer::parse_uuid(entity.owner_id);
    if (!object_id || !owner_id) return std::nullopt;
    homeworldz::viewer::StaticObject object;
    object.local_id = static_cast<std::uint32_t>(entity.id);
    object.parent_local_id = static_cast<std::uint32_t>(entity.parent_id);
    // A worn prim is drawn on its wearer's attachment joint only if the State
    // byte says which joint. Parenting alone puts it at the avatar's origin.
    object.state = homeworldz::viewer::attachment_state(entity.attachment_point);
    // And which item it is, or the viewer cannot tell two worn objects apart.
    if (entity.attachment_point != 0)
        if (const auto worn_item = homeworldz::viewer::parse_uuid(entity.attachment_item_id))
            object.attachment_item_id = *worn_item;
    object.id = *object_id;
    object.owner_id = *owner_id;
    const bool is_owner = entity.owner_id == recipient_id;
    const auto folded = homeworldz::scene::effective_permissions(scene, entity);
    const auto permissions = is_owner ? folded.owner : entity.everyone_permissions & folded.owner;
    object.update_flags = object_any_owner;
    if (entity.physical) object.update_flags |= object_physics;
    if (entity.phantom) object.update_flags |= object_phantom;
    if (entity.temporary) object.update_flags |= object_temporary;
    if ((permissions & homeworldz::scene::permission_modify) != 0) object.update_flags |= object_modify;
    if ((permissions & homeworldz::scene::permission_copy) != 0) object.update_flags |= object_copy;
    if ((permissions & homeworldz::scene::permission_move) != 0) object.update_flags |= object_move;
    if ((permissions & homeworldz::scene::permission_transfer) != 0) object.update_flags |= object_transfer;
    if (is_owner) object.update_flags |= object_you_owner | object_owner_modify;
    const auto script_status = falcon.object_script_status(entity.object_id);
    if (script_status.scripted) object.update_flags |= object_scripted;
    if (script_status.handles_touch) object.update_flags |= object_handle_touch;
    object.material = entity.material;
    const auto& protocol_position = entity.parent_id == 0 ? entity.position : entity.local_position;
    const auto& protocol_rotation = entity.parent_id == 0 ? entity.rotation : entity.local_rotation;
    object.position = {static_cast<float>(protocol_position.x), static_cast<float>(protocol_position.y),
                       static_cast<float>(protocol_position.z)};
    object.velocity = {static_cast<float>(entity.velocity.x), static_cast<float>(entity.velocity.y),
                       static_cast<float>(entity.velocity.z)};
    object.rotation = {static_cast<float>(protocol_rotation.x), static_cast<float>(protocol_rotation.y),
                       static_cast<float>(protocol_rotation.z)};
    object.scale = {static_cast<float>(entity.scale.x), static_cast<float>(entity.scale.y),
                    static_cast<float>(entity.scale.z)};
    object.texture_entry = entity.texture_entry;
    if (!entity.sculpt_id.empty()) {
        if (const auto sculpt = homeworldz::viewer::parse_uuid(entity.sculpt_id)) {
            object.sculpt_id = *sculpt;
            object.sculpt_type = entity.sculpt_type;
        }
    }
    object.path_curve = entity.path_curve;
    object.profile_curve = entity.profile_curve;
    object.path_begin = entity.path_begin;
    object.path_end = entity.path_end;
    object.path_scale_x = entity.path_scale_x;
    object.path_scale_y = entity.path_scale_y;
    object.path_shear_x = entity.path_shear_x;
    object.path_shear_y = entity.path_shear_y;
    object.path_twist = entity.path_twist;
    object.path_twist_begin = entity.path_twist_begin;
    object.path_radius_offset = entity.path_radius_offset;
    object.path_taper_x = entity.path_taper_x;
    object.path_taper_y = entity.path_taper_y;
    object.path_revolutions = entity.path_revolutions;
    object.path_skew = entity.path_skew;
    object.profile_begin = entity.profile_begin;
    object.profile_end = entity.profile_end;
    object.profile_hollow = entity.profile_hollow;
    return object;
}

void regenerate_task_inventory_item_ids(homeworldz::scene::Entity& entity) {
    for (auto& item : entity.task_inventory)
        item.item_id = homeworldz::viewer::random_uuid();
}

void apply_object_asset(
    homeworldz::scene::Entity& entity, const homeworldz::asset::ObjectAsset& asset) {
    entity.scale = asset.scale;
    entity.rotation = asset.rotation;
    entity.material = asset.material;
    entity.physics_shape_type = asset.physics_shape_type;
    entity.physics_density = asset.physics_density;
    entity.physics_friction = asset.physics_friction;
    entity.physics_restitution = asset.physics_restitution;
    entity.physics_gravity_multiplier = asset.physics_gravity_multiplier;
    entity.texture_entry = asset.texture_entry;
    entity.path_curve = asset.path_curve;
    entity.profile_curve = asset.profile_curve;
    entity.path_begin = asset.path_begin;
    entity.path_end = asset.path_end;
    entity.path_scale_x = asset.path_scale_x;
    entity.path_scale_y = asset.path_scale_y;
    entity.path_shear_x = asset.path_shear_x;
    entity.path_shear_y = asset.path_shear_y;
    entity.path_twist = asset.path_twist;
    entity.path_twist_begin = asset.path_twist_begin;
    entity.path_radius_offset = asset.path_radius_offset;
    entity.path_taper_x = asset.path_taper_x;
    entity.path_taper_y = asset.path_taper_y;
    entity.path_revolutions = asset.path_revolutions;
    entity.path_skew = asset.path_skew;
    entity.profile_begin = asset.profile_begin;
    entity.profile_end = asset.profile_end;
    entity.profile_hollow = asset.profile_hollow;
    entity.physical = asset.physical;
    entity.phantom = asset.phantom;
    entity.task_inventory_serial = asset.task_inventory_serial;
    entity.task_inventory = asset.task_inventory;
    entity.sculpt_id = asset.sculpt_id;
    entity.sculpt_type = asset.sculpt_type;
    regenerate_task_inventory_item_ids(entity);
}

std::optional<homeworldz::viewer::ObjectProperties> object_properties_from_entity(
    const homeworldz::scene::Scene& scene, const homeworldz::scene::Entity& entity) {
    const auto object_id = homeworldz::viewer::parse_uuid(entity.object_id);
    const auto creator_id = homeworldz::viewer::parse_uuid(entity.creator_id);
    const auto owner_id = homeworldz::viewer::parse_uuid(entity.owner_id);
    if (!object_id || !creator_id || !owner_id) return std::nullopt;
    homeworldz::viewer::ObjectProperties properties;
    properties.object_id = *object_id;
    properties.creator_id = *creator_id;
    properties.owner_id = *owner_id;
    properties.base_permissions = entity.base_permissions;
    properties.owner_permissions = entity.owner_permissions;
    properties.group_permissions = entity.group_permissions;
    properties.everyone_permissions = entity.everyone_permissions;
    properties.next_owner_permissions = entity.next_owner_permissions;
    const auto folded = homeworldz::scene::effective_permissions(scene, entity);
    properties.folded_owner_permissions = folded.owner;
    properties.folded_next_owner_permissions = folded.next_owner;
    properties.creation_date = entity.creation_date;
    properties.name = entity.name;
    properties.description = entity.description;
    return properties;
}

std::pair<std::string, std::string> legacy_avatar_name(std::string_view username) {
    const auto separator = username.find('.');
    auto first = std::string(username.substr(0, separator));
    auto last = separator == std::string_view::npos
        ? std::string("Resident") : std::string(username.substr(separator + 1));
    const auto capitalize = [](std::string& value) {
        if (!value.empty())
            value.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(value.front())));
    };
    capitalize(first);
    capitalize(last);
    return {std::move(first), std::move(last)};
}

std::string runtime_version() {
    std::ifstream input("VERSION");
    std::string value;
    if (input && std::getline(input, value)) {
        const auto first = value.find_first_not_of(" \t\r\n");
        const auto last = value.find_last_not_of(" \t\r\n");
        if (first != std::string::npos) value = value.substr(first, last - first + 1);
        const bool valid = !value.empty() && value.size() <= 64 &&
            std::all_of(value.begin(), value.end(), [](unsigned char character) {
                return std::isalnum(character) || character == '.' || character == '-' ||
                       character == '_';
            });
        if (valid) return value;
    }
    return HOMEWORLDZ_VERSION;
}
} // namespace

int main(int argc, char* argv[]) {
    std::filesystem::path config_path = "config/region.ini";
    std::string provisioned_region_id;
	std::string provisioned_region_name;
    std::string region_access_key;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--config" && index + 1 < argc) {
            config_path = argv[++index];
        } else if (argument == "--region-id" && index + 1 < argc) {
            provisioned_region_id = argv[++index];
		} else if (argument == "--region-name" && index + 1 < argc) {
			provisioned_region_name = argv[++index];
        } else if (argument == "--access-key" && index + 1 < argc) {
            region_access_key = argv[++index];
        } else if (argument == "--help") {
            std::cout << "Usage: homeworldz-region [--config path-to-region.ini] "
						 "(--region-id uuid | --region-name name) --access-key key" << std::endl;
            return 0;
        } else {
            std::cerr << "Unknown or incomplete argument: " << argument << std::endl;
            return 2;
        }
    }
	if (provisioned_region_id.empty() == provisioned_region_name.empty() || region_access_key.empty()) {
		std::cerr << "Exactly one of --region-id or --region-name, plus --access-key, is required." << std::endl;
        return 2;
    }
	if (provisioned_region_id.empty()) provisioned_region_id = provisioned_region_name;
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
#endif
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    const auto region_version = runtime_version();

    try {
        configured_values = homeworldz::config::load_region_ini(config_path);
        if (!configured_values.empty()) {
            std::cout << "{\"level\":\"info\",\"message\":\"region configuration loaded\",\"path\":"
                      << homeworldz::api::json_string(config_path.string()) << ",\"settings\":"
                      << configured_values.size() << "}" << std::endl;
        }
    } catch (const std::exception& error) {
        std::cerr << "{\"level\":\"error\",\"message\":\"load region configuration failed\",\"error\":"
                  << homeworldz::api::json_string(error.what()) << "}" << std::endl;
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    std::string region_name;
    std::string region_owner_id;
    std::optional<homeworldz::grid::Estate> region_estate;
    int region_maturity{};
    int region_grid_x{};
    int region_grid_y{};
	int region_size_x{256};
	int region_size_y{256};
    auto region_public_endpoint = configured_value(
        "region.public_endpoint", "http://localhost:" + std::to_string(configured_port()));
    auto grid_public_endpoint = configured_value(
        "grid.public_url", configured_value("grid.url", "http://localhost:42000"));
	auto region_viewer_port = configured_viewer_port();
    const auto region_data_path = std::filesystem::path(
        configured_value("region.data_path", "var/region"));
    const auto terrain_state_path = region_data_path / "terrain.f32";
    // The baseline the terrain edit limits are measured from, and what the revert
    // brush returns to. Persisted separately from the current heightmap because it
    // is a different fact with a different lifetime: the ground changes constantly
    // and the baseline changes only when an operator bakes it.
    //
    // Before this file existed the baseline was rebuilt from the packaged RAW on
    // every start, so the limits were anchored to the shipped shape forever and a
    // region sculpted away from it had a window nowhere near its own ground
    // (found while the operator tested the limits, 2026-08-04).
    const auto revert_state_path = region_data_path / "revert.f32";
    // The terrain revision (client core request, 2026-07-30): monotonic per
    // region, incremented on every applied edit, persisted beside the terrain
    // it describes. Its value is that change notification can then be *lossy*
    // and still correct - a client may miss any number of events, coalesced or
    // dropped under load, and still detect that it is behind and reconcile in
    // one fetch. Detectable incompleteness beats a delivery guarantee, and it
    // is what the Second Life family lacks: there, a burst over the per-frame
    // budget is simply gone, so it presents as terrain that never finishes
    // rather than as an error.
    const auto terrain_revision_path = region_data_path / "terrain.rev";
    std::uint64_t terrain_revision = 0;
    if (std::ifstream stored(terrain_revision_path); stored) {
        std::uint64_t value = 0;
        if (stored >> value) terrain_revision = value;
    }
    const auto persist_terrain_revision = [&] {
        std::ofstream out(terrain_revision_path, std::ios::trunc);
        if (out) out << terrain_revision << std::endl;
    };
    // Retire an avatar whose viewer stops answering pings for this long (a lost
    // connection: crash, force-kill, or sustained packet loss) so its KillObject
    // broadcasts promptly rather than waiting on the grid session TTL. The region
    // pings every 5s; this counts missed replies, so an idle-but-connected viewer
    // is never affected. Kept well above transient outages; raise via
    // region.connection_timeout_seconds if your users see longer blips.
    const auto connection_timeout =
        std::chrono::seconds(configured_int("region.connection_timeout_seconds", 60, 15, 3600));
    // The walkable slope limit, overridable per region and published in the
    // session hello. Every character this region creates uses the same value
    // the hello announces — one configured number, no drift.
    // Read once at startup like the other feel constants; an operator tuning it
    // restarts the region, which is the same cost as any other ini change.
    const auto terrain_smooth_strength = static_cast<float>(std::clamp(
        configured_int("region.smooth_strength_percent",
                       static_cast<int>(homeworldz::terrain::default_smooth_strength * 100.0F),
                       1, 100), 1, 100)) / 100.0F;
    const auto walkable_slope_degrees = static_cast<double>(configured_int(
        "region.walkable_slope_degrees",
        static_cast<int>(homeworldz::physics::character_walkable_slope_degrees), 10, 89));
    // The region's water plane. Viewers have always been told a height in
    // RegionHandshake and session clients were told nothing at all, so both
    // now read this one number; the 20 m default is the value the handshake
    // struct has carried since it was written, kept so no region moves its
    // water by being upgraded.
    const auto water_height = static_cast<double>(
        configured_int("region.water_height", 20, 0, 4096));
    // Where the viewer's About box sends someone who clicks through to the
    // server's release notes. Configurable because a grid that is not this one
    // should not be advertising this one's pages.
    const auto release_notes_url = configured_value(
        "region.release_notes_url", "https://homeworldz.com/roadmaps/server");
    // This region's live terrain layers: the shipped defaults until an operator
    // changes them from the viewer's Region/Estate -> Terrain tab, then whatever
    // was persisted. Read by both publish paths, so a viewer and a session
    // client are always told the same thing.
    // The Region/Estate form's own settings. Seeded from configuration, then
    // replaced by anything an operator has persisted. `water_height` above is now
    // the *default* rather than the value: it is what a region starts at, and the
    // form can change it for this region only.
    homeworldz::storage::RegionStorage::RegionSettings region_settings{
        water_height,
        homeworldz::terrain::default_terrain_raise_limit,
        homeworldz::terrain::default_terrain_lower_limit,
        true, false, 0.0};
    homeworldz::terrain::Settings terrain_layers;
    // Sends RegionHandshake. Used at login and again whenever the terrain or
    // water changes, because that is the only message carrying either and the
    // viewer is built for the repeat: `unpackRegionHandshake` tracks whether the
    // composition changed, calls `dirtyAllPatches()` when it did, and refreshes
    // the Region/Estate floater. Firestorm's own comment on the PBR path spells
    // the exchange out - "viewer: POST ModifyRegion / simulator: RegionHandshake
    // / viewer: GET ModifyRegion". An earlier note here claimed re-sending
    // restarts more viewer state than a texture change warrants; that was wrong,
    // and the symptom was an operator reopening the form to stale values
    // (2026-08-04).
    std::function<bool(const std::string&, const homeworldz::viewer::Uuid&)> send_region_handshake;
    // What the viewer has staged but not yet committed. Seeded from the live
    // values at startup so a commit that follows only one of the two staging
    // messages carries the current setting for the other, not a default.
    homeworldz::terrain::Settings pending_terrain_layers;
    std::unique_ptr<homeworldz::grid::RegistrationLifecycle> registration;
    std::unique_ptr<homeworldz::session::Server> session_server;
    std::unique_ptr<homeworldz::grid::Client> viewer_grid;
    // Access-key-authenticated client for estate updates via the region-runtime API.
    std::unique_ptr<homeworldz::grid::Client> estate_client;
    std::unique_ptr<homeworldz::grid::ViewerSessionCache> viewer_sessions;
    std::vector<homeworldz::grid::RegionNeighbor> region_neighbors;
    // The world map is grid-wide, so it cannot be built from the neighbor
    // list. Cached rather than fetched per request: a viewer panning the map
    // asks repeatedly, while the answer changes only when a region is placed
    // or its lease turns over. A failed refresh keeps the previous list —
    // stale placements draw a better map than none.
    std::vector<homeworldz::grid::RegionPlacement> grid_topology;
    // Viewer-served sl-mesh renditions, fetch-through cached from the grid.
    std::unordered_map<std::string, std::string> mesh_rendition_cache;
    auto next_topology_refresh = std::chrono::steady_clock::time_point{};
    const auto refresh_grid_topology = [&]() {
        const auto moment = std::chrono::steady_clock::now();
        if (moment < next_topology_refresh) return;
        next_topology_refresh = moment + std::chrono::seconds(30);
        try {
            if (auto discovered = viewer_grid ? viewer_grid->find_grid_topology() : std::nullopt)
                grid_topology = std::move(*discovered);
            else
                std::cerr << "{\"level\":\"warning\",\"message\":\"grid topology discovery failed\"}"
                          << std::endl;
        } catch (const std::exception& error) {
            std::cerr << "{\"level\":\"warning\",\"message\":\"grid topology discovery failed\",\"error\":"
                      << homeworldz::api::json_string(error.what()) << "}" << std::endl;
        }
    };
    auto next_neighbor_refresh = std::chrono::steady_clock::time_point{};
    const auto refresh_region_neighbors = [&](bool required) {
        const auto retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        try {
            const auto discovered = viewer_grid ?
                viewer_grid->find_region_neighbors(provisioned_region_id) : std::nullopt;
            if (!discovered) {
                std::cerr << "{\"level\":\"" << (required ? "error" : "warning")
                          << "\",\"message\":\"region neighbor discovery failed\"}" << std::endl;
                next_neighbor_refresh = retry_at;
                return false;
            }
            for (const auto& neighbor : *discovered) {
				const auto source_size = region_size_x / 256;
				const auto neighbor_size = neighbor.size_x / 256;
				const auto overlaps = [](int first, int first_size, int second, int second_size) {
					return first < second + second_size && second < first + first_size;
				};
				bool adjacent{};
				if (neighbor.direction == "north")
					adjacent = neighbor.grid_y == region_grid_y + source_size &&
						overlaps(region_grid_x, source_size, neighbor.grid_x, neighbor_size);
				else if (neighbor.direction == "east")
					adjacent = neighbor.grid_x == region_grid_x + source_size &&
						overlaps(region_grid_y, source_size, neighbor.grid_y, neighbor_size);
				else if (neighbor.direction == "south")
					adjacent = neighbor.grid_y + neighbor_size == region_grid_y &&
						overlaps(region_grid_x, source_size, neighbor.grid_x, neighbor_size);
				else if (neighbor.direction == "west")
					adjacent = neighbor.grid_x + neighbor_size == region_grid_x &&
						overlaps(region_grid_y, source_size, neighbor.grid_y, neighbor_size);
				if (!adjacent) {
                    std::cerr << "{\"level\":\"" << (required ? "error" : "warning")
                              << "\",\"message\":\"invalid region neighbor topology\"}" << std::endl;
                    next_neighbor_refresh = retry_at;
                    return false;
                }
            }
            const bool changed = region_neighbors != *discovered;
            region_neighbors = *discovered;
            if (required || changed) {
                std::cout << "{\"level\":\"info\",\"message\":\"region neighbors discovered\",\"count\":"
                          << region_neighbors.size() << ",\"borders\":[";
                for (std::size_t index = 0; index < region_neighbors.size(); ++index) {
                    if (index != 0) std::cout << ',';
                    std::cout << homeworldz::api::json_string(region_neighbors[index].direction);
                }
                std::cout << "]}" << std::endl;
            }
            next_neighbor_refresh = retry_at;
            return true;
        } catch (const std::exception& error) {
            std::cerr << "{\"level\":\"" << (required ? "error" : "warning")
                      << "\",\"message\":\"region neighbor discovery failed\",\"error\":"
                      << homeworldz::api::json_string(error.what()) << "}" << std::endl;
            next_neighbor_refresh = retry_at;
            return false;
        }
    };
    const auto service_token = configured_value("grid.service_token");
    if (service_token.empty()) {
        std::cerr << "{\"level\":\"error\",\"message\":\"grid service token is required\"}" << std::endl;
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }
    {
        try {
            homeworldz::grid::RegionSettings settings{
                {}, 0, 0,
                region_public_endpoint,
                region_viewer_port,
                configured_int("region.lease_seconds", 60, 10, 300)};
            // The session transport is served when a port is configured, and
            // its public URL is reported to the grid so world entry can hand
            // it to clients (docs/CLIENT2-TRANSPORT.md). The URL is explicit
            // configuration: only the operator knows where TLS terminates.
            if (configured_int("region.session_port", 0, 0, 65535) > 0)
                settings.session_endpoint = configured_value("region.session_public_url");
            const auto grid_url = configured_value("grid.url", "http://localhost:42000");
            auto registration_transport = homeworldz::grid::socket_transport(grid_url, region_access_key);
            homeworldz::grid::Client registration_client(registration_transport);
            std::string registration_refusal;
            const auto provisioned = registration_client.register_provisioned_region(
                provisioned_region_id, settings, &registration_refusal);
            if (!provisioned) {
                std::cerr << "{\"level\":\"error\",\"message\":\"region registration failed\"";
                if (!registration_refusal.empty())
                    std::cerr << ",\"reason\":"
                              << homeworldz::api::json_string(registration_refusal);
                std::cerr << '}' << std::endl;
#ifdef _WIN32
                WSACleanup();
#endif
                return 1;
            }
            region_name = provisioned->name;
            region_owner_id = provisioned->owner_id;
            region_estate = provisioned->estate;
            region_maturity = provisioned->maturity;
            region_grid_x = provisioned->grid_x;
            region_grid_y = provisioned->grid_y;
			region_size_x = provisioned->size_x;
			region_size_y = provisioned->size_y;
			region_public_endpoint = provisioned->public_endpoint;
			region_viewer_port = provisioned->viewer_port;
			grid_public_endpoint = provisioned->grid_public_url;
			settings.public_endpoint = region_public_endpoint;
			settings.viewer_port = region_viewer_port;
			provisioned_region_id = provisioned->id;
            auto viewer_transport = homeworldz::grid::socket_transport(grid_url, service_token);
            viewer_grid = std::make_unique<homeworldz::grid::Client>(std::move(viewer_transport));
            estate_client = std::make_unique<homeworldz::grid::Client>(
                homeworldz::grid::socket_transport(grid_url, region_access_key));
            if (!refresh_region_neighbors(true)) {
#ifdef _WIN32
                WSACleanup();
#endif
                return 1;
            }
            viewer_sessions = std::make_unique<homeworldz::grid::ViewerSessionCache>(*viewer_grid);
            registration = std::make_unique<homeworldz::grid::RegistrationLifecycle>(
                std::move(registration_client), std::move(settings), provisioned->id);
            if (!registration->start(std::chrono::steady_clock::now())) {
                std::cerr << "{\"level\":\"error\",\"message\":\"region registration failed\"}" << std::endl;
#ifdef _WIN32
                WSACleanup();
#endif
                return 1;
            }
            if (const auto session_port = configured_int("region.session_port", 0, 0, 65535);
                session_port > 0) {
                // The validator asks the grid on its own connection; the
                // ticket-signing secret never reaches the region.
                auto validation_client = std::make_shared<homeworldz::grid::Client>(
                    homeworldz::grid::socket_transport(grid_url, region_access_key));
                const auto session_region_id = provisioned->id;
                session_server = homeworldz::session::Server::start({session_port, region_name,
                    [validation_client, session_region_id](const std::string& token)
                        -> std::optional<homeworldz::session::SessionIdentity> {
                        const auto resolved =
                            validation_client->validate_region_ticket(session_region_id, token);
                        if (!resolved) return std::nullopt;
                        return homeworldz::session::SessionIdentity{resolved->user_id,
                            resolved->userid, resolved->display_name, resolved->session_id,
                            resolved->arrival};
                    },
                    static_cast<std::size_t>(region_size_x),
                    walkable_slope_degrees,
                    water_height,
                    [&terrain_layers] { return terrain_layers; },
                    [&terrain_revision] { return terrain_revision; }});
                if (!session_server) {
                    std::cerr << "{\"level\":\"error\",\"message\":\"region session listener failed\",\"port\":"
                              << session_port << "}" << std::endl;
#ifdef _WIN32
                    WSACleanup();
#endif
                    return 1;
                }
                std::cout << "{\"level\":\"info\",\"message\":\"region session listening\",\"port\":"
                          << session_port << "}" << std::endl;
            }
        } catch (const std::exception& error) {
            std::cerr << "{\"level\":\"error\",\"message\":\"region registration failed\",\"error\":"
                      << homeworldz::api::json_string(error.what()) << "}" << std::endl;
#ifdef _WIN32
            WSACleanup();
#endif
            return 1;
        }
    }

    const auto terrain_width = static_cast<std::size_t>(region_size_x);
    const auto default_heightmap = load_raw_heightmap(configured_value(
        "region.terrain_path", "assets/region/terrain/plateau-square.raw"), terrain_width);
    auto revert_heightmap = homeworldz::terrain::load_state(revert_state_path, terrain_width);
    const bool loaded_baked_baseline = static_cast<bool>(revert_heightmap);
    if (!revert_heightmap) {
        revert_heightmap = default_heightmap ?
            std::make_unique<homeworldz::terrain::Heightmap>(*default_heightmap) :
            std::make_unique<homeworldz::terrain::Heightmap>(terrain_width);
        if (!default_heightmap) revert_heightmap->fill(25.0F);
    }
    auto terrain_heightmap = homeworldz::terrain::load_state(terrain_state_path, terrain_width);
    const bool loaded_region_state = static_cast<bool>(terrain_heightmap);
    if (!terrain_heightmap)
        terrain_heightmap = std::make_unique<homeworldz::terrain::Heightmap>(*revert_heightmap);
    std::cout << "{\"level\":\"info\",\"message\":\"terrain heightmap loaded\",\"source\":\""
              << (loaded_region_state ? "region-state" :
                  (default_heightmap ? "packaged-default" : "flat-fallback"))
              << "\",\"width\":" << terrain_width << "}" << std::endl;
    std::cout << "{\"level\":\"info\",\"message\":\"terrain baseline loaded\",\"source\":\""
              << (loaded_baked_baseline ? "baked" :
                  (default_heightmap ? "packaged-default" : "flat-fallback"))
              << "\"}" << std::endl;
    const auto initial_spawn = default_spawn(*terrain_heightmap);

    homeworldz::scene::Scene scene;
    // Legacy Blinn-Phong material definitions by id, in the LLSD binary the
    // capability serves. Populated from storage below and by registrations.
    std::map<std::string, std::vector<std::byte>> render_material_cache;
    std::unique_ptr<homeworldz::storage::RegionStorage> storage;
    try {
        storage = std::make_unique<homeworldz::storage::RegionStorage>(
            region_data_path);
        // Material definitions a viewer registered previously. Held in memory
        // because the capability answers them on the request path, and reloaded
        // here so an assignment survives a restart - a material that lived only
        // until the region bounced would be the same silent loss with a longer
        // fuse.
        for (auto& [id, definition] : storage->load_render_materials())
            render_material_cache.emplace(id, std::move(definition));
        if (const auto stored = storage->load_region_settings()) {
            region_settings = *stored;
            std::cout << "{\"level\":\"info\",\"message\":\"region settings loaded\""
                      << ",\"water\":" << region_settings.water_height
                      << ",\"terrainRaise\":" << region_settings.terrain_raise
                      << ",\"terrainLower\":" << region_settings.terrain_lower
                      << "}" << std::endl;
        }
        // Terrain layers an operator set previously. Absent means untouched, which
        // is deliberately distinct from "set to the defaults": only the second is
        // a decision, and only the first should follow a change of defaults.
        if (storage->load_terrain_settings(terrain_layers.assets, terrain_layers.start,
                                          terrain_layers.range)) {
            pending_terrain_layers = terrain_layers;
            std::cout << "{\"level\":\"info\",\"message\":\"terrain layers loaded\""
                      << ",\"startHeight\":" << terrain_layers.start[0]
                      << ",\"heightRange\":" << terrain_layers.range[0] << "}" << std::endl;
        }
        if (!render_material_cache.empty())
            std::cout << "{\"level\":\"info\",\"message\":\"render materials loaded\",\"count\":"
                      << render_material_cache.size() << "}" << std::endl;
        const auto imported_assets = storage->import_asset_directory(
            configured_value("region.asset_path", "assets/region"), system_creator_id);
        if (imported_assets != 0) {
            std::cout << "{\"level\":\"info\",\"message\":\"region assets imported\",\"count\":"
                      << imported_assets << "}" << std::endl;
        }
        if (viewer_grid) {
            const auto assets = storage->list_assets();
            for (const auto& asset : assets) {
                if (!viewer_grid->register_asset(asset.viewer_id, asset.creator_id, asset.sha256,
                                                 asset.size, region_public_endpoint, true)) {
                    const auto authoritative = viewer_grid->find_asset(asset.viewer_id);
                    if (!authoritative || authoritative->sha256 != asset.sha256 ||
                        authoritative->size != asset.size)
                        throw std::runtime_error("register local asset origin failed");
                    const auto reconciled = storage->reconcile_asset_creator(
                        asset.viewer_id, authoritative->creator_id, asset.sha256, asset.size);
                    if (!viewer_grid->register_asset(
                            reconciled.viewer_id, reconciled.creator_id, reconciled.sha256,
                            reconciled.size, region_public_endpoint, true))
                        throw std::runtime_error("register reconciled local asset origin failed");
                    std::cout << "{\"level\":\"warning\",\"message\":\"local asset provenance reconciled\","
                                 "\"assetId\":" << homeworldz::api::json_string(asset.viewer_id)
                              << ",\"creatorId\":"
                              << homeworldz::api::json_string(authoritative->creator_id) << "}"
                              << std::endl;
                }
            }
            if (!assets.empty()) {
                std::cout << "{\"level\":\"info\",\"message\":\"region asset origins registered\",\"count\":"
                          << assets.size() << "}" << std::endl;
                // Write the bundled assets through to the vault (idempotent).
                // Anything a commit's closure can reach — the default prim
                // texture on every wrapper, the default wearables — must
                // already be vault-held, because the durability fetch-back
                // and this region's single thread cannot meet (ADR 0026).
                std::size_t vaulted = 0;
                for (const auto& asset : assets) {
                    try {
                        const auto bytes = storage->read_asset(asset.viewer_id);
                        if (viewer_grid->store_vault_asset(asset.viewer_id,
                                std::span(bytes.data(), bytes.size())))
                            ++vaulted;
                    } catch (const std::exception&) {
                    }
                }
                std::cout << "{\"level\":\"info\",\"message\":\"bundled assets vaulted\",\"count\":"
                          << vaulted << "}" << std::endl;
            }
        }
        if (storage->load_snapshot(scene)) {
            std::cout << "{\"level\":\"info\",\"message\":\"scene snapshot restored\",\"revision\":"
                      << scene.revision() << ",\"entities\":" << scene.size() << "}" << std::endl;
            std::size_t repaired_texture_entries = 0;
            for (const auto& [entity_id, restored_entity] : scene.entities()) {
                if (restored_entity.object_id.empty()) continue;
                auto* entity = scene.find(entity_id);
                if (entity && homeworldz::viewer::normalize_primitive_texture_entry(
                                  entity->texture_entry, default_prim_texture_entry()))
                    ++repaired_texture_entries;
            }
            if (repaired_texture_entries != 0) {
                storage->save_snapshot(scene);
                std::cout << "{\"level\":\"info\",\"message\":\"legacy primitive textures repaired\",\"count\":"
                          << repaired_texture_entries << "}" << std::endl;
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "{\"level\":\"error\",\"message\":\"open region storage failed\",\"error\":"
                  << homeworldz::api::json_string(error.what()) << "}" << std::endl;
        if (registration) registration->stop();
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    // Parcels: restore persisted land, or create a single region-wide parcel owned
    // by the region owner. The owner comes from the authoritative grid record; when
    // the record has no owner (older provisioning) the default parcel is public land.
    std::optional<homeworldz::parcel::ParcelSet> parcels;
    try {
        if (auto restored = storage->load_parcels()) {
            parcels.emplace(region_size_x, std::move(*restored));
            std::cout << "{\"level\":\"info\",\"message\":\"parcels restored\",\"count\":"
                      << parcels->parcels().size() << "}" << std::endl;
        } else {
            const auto claim_date = static_cast<std::int32_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            parcels.emplace(region_size_x, homeworldz::viewer::random_uuid(), region_owner_id,
                            claim_date);
            storage->save_parcels(parcels->parcels());
            std::cout << "{\"level\":\"info\",\"message\":\"default parcel created\",\"owner\":"
                      << homeworldz::api::json_string(region_owner_id) << "}" << std::endl;
        }
    } catch (const std::exception& error) {
        std::cerr << "{\"level\":\"error\",\"message\":\"open region parcels failed\",\"error\":"
                  << homeworldz::api::json_string(error.what()) << "}" << std::endl;
        if (registration) registration->stop();
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }
    // Pulls an asset closure into the local blob store (ADR 0026,
    // "Completeness is transitive"). Assigned below once the asset-fetch
    // machinery exists; a safe no-op until then. Declared this early because
    // the contents-transfer application, defined next, is the first path that
    // must materialize what it admits.
    std::function<void(std::vector<std::pair<std::string, int>>, std::string_view)>
        materialize_asset_closure = [](std::vector<std::pair<std::string, int>>,
                                       std::string_view) {};
    const auto apply_task_inventory_transfer = [&](const homeworldz::grid::TaskInventoryTransfer& transfer) {
        homeworldz::scene::Entity* target = nullptr;
        for (const auto& [entity_id, entity] : scene.entities()) {
            if (entity.object_id == transfer.object_id) {
                target = scene.find(entity_id);
                break;
            }
        }
        if (!target || target->owner_id != transfer.user_id) return false;
        const auto existing = std::find_if(
            target->task_inventory.begin(), target->task_inventory.end(),
            [&](const auto& item) { return item.item_id == transfer.task_item_id; });
        if (existing == target->task_inventory.end()) {
            const auto previous_serial = target->task_inventory_serial;
            const auto created = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            const auto& item = transfer.item;
            target->task_inventory.push_back({
                transfer.task_item_id, item.asset_id, item.creator_id, transfer.user_id,
                item.owner_id, "00000000-0000-0000-0000-000000000000", item.name,
                item.description, static_cast<std::int8_t>(item.asset_type),
                static_cast<std::int8_t>(item.inventory_type), item.flags,
                item.base_permissions, item.current_permissions, 0,
                item.everyone_permissions, item.next_permissions,
                static_cast<std::uint8_t>(item.sale_type), item.sale_price, created});
            target->task_inventory_serial = previous_serial == 65535
                ? 1 : static_cast<std::uint16_t>(previous_serial + 1);
            try {
                storage->save_snapshot(scene);
            } catch (...) {
                target->task_inventory.pop_back();
                target->task_inventory_serial = previous_serial;
                throw;
            }
            // The item's bytes must live here now, not wherever it came from:
            // this region's backup has to be able to reconstruct its own
            // contents (ADR 0026, region completeness).
            materialize_asset_closure({{item.asset_id, item.asset_type}}, "contents-add");
        }
        return viewer_grid && viewer_grid->finalize_task_inventory_transfer(
            transfer.id, provisioned_region_id);
    };
    const auto apply_task_inventory_extraction = [&](const homeworldz::grid::TaskInventoryExtraction& extraction)
        -> std::optional<homeworldz::grid::TaskInventoryExtraction> {
        homeworldz::scene::Entity* target = nullptr;
        for (const auto& [entity_id, entity] : scene.entities()) {
            if (entity.object_id == extraction.object_id) {
                target = scene.find(entity_id);
                break;
            }
        }
        if (!target || target->owner_id != extraction.user_id) return std::nullopt;
        const auto found = std::find_if(
            target->task_inventory.begin(), target->task_inventory.end(),
            [&](const auto& item) { return item.item_id == extraction.source_task_item_id; });
        if (found != target->task_inventory.end()) {
            if (found->owner_id != extraction.user_id ||
                found->asset_id != extraction.item.asset_id ||
                (found->current_permissions & homeworldz::scene::permission_copy) != 0)
                return std::nullopt;
            const auto index = static_cast<std::size_t>(
                std::distance(target->task_inventory.begin(), found));
            const auto removed = *found;
            const auto previous_serial = target->task_inventory_serial;
            target->task_inventory.erase(found);
            target->task_inventory_serial = previous_serial == 65535
                ? 1 : static_cast<std::uint16_t>(previous_serial + 1);
            try {
                storage->save_snapshot(scene);
            } catch (...) {
                target->task_inventory.insert(
                    target->task_inventory.begin() + static_cast<std::ptrdiff_t>(index), removed);
                target->task_inventory_serial = previous_serial;
                throw;
            }
        }
        return viewer_grid
            ? viewer_grid->finalize_task_inventory_extraction(extraction.id, provisioned_region_id)
            : std::nullopt;
    };
    if (viewer_grid) {
        try {
            const auto pending = viewer_grid->pending_task_inventory_transfers(provisioned_region_id);
            if (!pending) throw std::runtime_error("load pending task inventory transfers");
            std::size_t reconciled = 0;
            for (const auto& transfer : *pending)
                if (apply_task_inventory_transfer(transfer)) ++reconciled;
            if (!pending->empty()) {
                std::cout << "{\"level\":"
                          << (reconciled == pending->size() ? "\"info\"" : "\"warning\"")
                          << ",\"message\":\"pending task inventory transfers reconciled\",\"completed\":"
                          << reconciled << ",\"pending\":" << pending->size() << "}"
                          << std::endl;
            }
        } catch (const std::exception& error) {
            std::cerr << "{\"level\":\"warning\",\"message\":\"pending task inventory transfer reconciliation failed\",\"error\":"
                      << homeworldz::api::json_string(error.what()) << "}" << std::endl;
        }
        try {
            const auto pending = viewer_grid->pending_task_inventory_extractions(provisioned_region_id);
            if (!pending) throw std::runtime_error("load pending task inventory extractions");
            std::size_t reconciled = 0;
            for (const auto& extraction : *pending)
                if (apply_task_inventory_extraction(extraction)) ++reconciled;
            if (!pending->empty()) {
                std::cout << "{\"level\":"
                          << (reconciled == pending->size() ? "\"info\"" : "\"warning\"")
                          << ",\"message\":\"pending task inventory extractions reconciled\",\"completed\":"
                          << reconciled << ",\"pending\":" << pending->size() << "}"
                          << std::endl;
            }
        } catch (const std::exception& error) {
            std::cerr << "{\"level\":\"warning\",\"message\":\"pending task inventory extraction reconciliation failed\",\"error\":"
                      << homeworldz::api::json_string(error.what()) << "}" << std::endl;
        }
        try {
            const auto pending = viewer_grid->pending_object_rezzes(provisioned_region_id);
            if (!pending) throw std::runtime_error("load pending object rezzes");
            std::size_t reconciled = 0;
            for (const auto& rez : *pending) {
                const auto exists = std::any_of(scene.entities().begin(), scene.entities().end(),
                    [&](const auto& entry) { return entry.second.object_id == rez.object_id; });
                const auto completed = exists
                    ? viewer_grid->finalize_object_rez(rez.id, provisioned_region_id)
                    : viewer_grid->rollback_object_rez(rez.id, provisioned_region_id);
                if (completed) ++reconciled;
            }
            if (!pending->empty()) {
                std::cout << "{\"level\":"
                          << (reconciled == pending->size() ? "\"info\"" : "\"warning\"")
                          << ",\"message\":\"pending object rezzes reconciled\",\"completed\":"
                          << reconciled << ",\"pending\":" << pending->size() << "}"
                          << std::endl;
            }
        } catch (const std::exception& error) {
            std::cerr << "{\"level\":\"warning\",\"message\":\"pending object rez reconciliation failed\",\"error\":"
                      << homeworldz::api::json_string(error.what()) << "}" << std::endl;
        }
    }
    const auto read_federated_asset = [&](std::string_view asset_id) -> std::vector<std::byte> {
        try {
            return storage->read_asset(asset_id);
        } catch (const std::exception&) {
            if (!viewer_grid) throw;
        }
        const auto metadata = viewer_grid->find_asset(asset_id);
        if (!metadata) throw std::runtime_error("asset federation metadata was not found");
        const auto normalized_region_endpoint = [&] {
            auto endpoint = region_public_endpoint;
            while (!endpoint.empty() && endpoint.back() == '/') endpoint.pop_back();
            return endpoint;
        }();
        for (const auto& location : metadata->locations) {
            auto endpoint = location.endpoint;
            while (!endpoint.empty() && endpoint.back() == '/') endpoint.pop_back();
            if (endpoint == normalized_region_endpoint) continue;
            homeworldz::grid::HttpResponse response;
            try {
                response = homeworldz::grid::fetch_asset_from(
                    endpoint, service_token, metadata->asset_id);
            } catch (const std::exception&) {
                continue;
            }
            if (response.status_code != 200 || response.body.size() != metadata->size) continue;
            const auto content = std::span(
                reinterpret_cast<const std::byte*>(response.body.data()), response.body.size());
            if (homeworldz::crypto::sha256_hex(content) != metadata->sha256) continue;
            const auto stored = storage->store_asset(
                metadata->asset_id, metadata->creator_id, content);
            if (!viewer_grid->register_asset(
                    stored.viewer_id, stored.creator_id, stored.sha256, stored.size,
                    region_public_endpoint, false))
                throw std::runtime_error("register replicated asset failed");
            std::cout << "{\"level\":\"info\",\"message\":\"region asset replicated\",\"assetId\":"
                      << homeworldz::api::json_string(stored.viewer_id) << ",\"source\":"
                      << homeworldz::api::json_string(endpoint) << "}" << std::endl;
            return {content.begin(), content.end()};
        }
        // The vault is the location of last resort and, for anything inventory
        // references, the one that is always there (ADR 0026): peer regions are
        // an optimization, and a region being unreachable — or gone for good —
        // must not be the reason a user's own content cannot be read. The bytes
        // are verified exactly as a peer's are; the vault is trusted storage,
        // not a trusted source.
        if (const auto vaulted = viewer_grid->fetch_vault_asset(metadata->asset_id)) {
            const auto content = std::span(
                reinterpret_cast<const std::byte*>(vaulted->data()), vaulted->size());
            if (vaulted->size() == metadata->size &&
                homeworldz::crypto::sha256_hex(content) == metadata->sha256) {
                const auto stored = storage->store_asset(
                    metadata->asset_id, metadata->creator_id, content);
                if (!viewer_grid->register_asset(
                        stored.viewer_id, stored.creator_id, stored.sha256, stored.size,
                        region_public_endpoint, false))
                    throw std::runtime_error("register vault-restored asset failed");
                std::cout << "{\"level\":\"info\",\"message\":\"region asset restored from vault\",\"assetId\":"
                          << homeworldz::api::json_string(stored.viewer_id) << "}" << std::endl;
                return {content.begin(), content.end()};
            }
            std::cerr << "{\"level\":\"error\",\"message\":\"vault asset failed verification\",\"assetId\":"
                      << homeworldz::api::json_string(metadata->asset_id) << "}" << std::endl;
        }
        throw std::runtime_error("no verified asset replica was available");
    };

    // --- Server-side appearance baking (ADR 0029) --------------------------
    // The default outfit is identical for every default avatar, so its bake is
    // computed once (lazily) and reused. Baking is inline; the one-time first
    // call fetches the bundled default wearables/textures, composites the bake
    // slots, and stores the results as content-addressed assets.
    // Unbaked bake slots fall back to this face. It must be IMG_DEFAULT_AVATAR
    // — the viewer's "no bake here, never rendered" sentinel — so slots we do
    // not produce (e.g. the skirt when no skirt is worn) render nothing and do
    // not block the avatar from being treated as fully baked. (IMG_WHITE draws
    // a solid mesh; IMG_INVISIBLE is treated as an unfinished bake -> cloud.)
    const auto server_bake_default_face =
        homeworldz::viewer::parse_uuid("c228d1cf-4b5d-4ba8-84f4-899a0796aa97").value();
    const auto fetch_asset_bytes =
        [&](const homeworldz::viewer::Uuid& id) -> std::optional<std::vector<std::byte>> {
        try {
            auto bytes = read_federated_asset(homeworldz::viewer::format_uuid(id));
            if (bytes.empty()) return std::nullopt;
            return bytes;
        } catch (const std::exception&) {
            return std::nullopt;
        }
    };
    // Region completeness (ADR 0026, "Completeness is transitive"): content
    // standing in the scene is only region-durable when its whole reference
    // closure is in the local blob store — face textures, task inventory
    // assets, nested objects and their closures — so a backup of this
    // region's own storage reconstructs it without reaching any other
    // server. read_federated_asset stores what it fetches, so materializing
    // is walking the closure and reading each member once; already-local
    // members cost a local read. Lenient by design: the content already
    // stands in the scene, so a missing member is reported, never a refusal
    // — refusal lives grid-side, at the inventory commit.
    materialize_asset_closure = [&](std::vector<std::pair<std::string, int>> seeds,
                                    std::string_view reason) {
        constexpr std::size_t closure_bound = 10000;
        std::vector<std::pair<std::string, int>> queue;
        std::unordered_set<std::string> seen;
        const auto push = [&](const std::string& id, int type) {
            if (id.empty() || id == "00000000-0000-0000-0000-000000000000") return;
            if (!seen.insert(id).second) return;
            queue.emplace_back(id, type);
        };
        for (const auto& [id, type] : seeds) push(id, type);
        std::size_t unavailable = 0;
        for (std::size_t index = 0; index < queue.size() && index < closure_bound; ++index) {
            const auto [id, type] = queue[index];
            std::vector<std::byte> content;
            try {
                content = read_federated_asset(id);
            } catch (const std::exception&) {
                // Includes external references (stock viewer textures the grid
                // never registered) alongside genuine losses; the grid-side
                // walk is where the two are told apart.
                ++unavailable;
                continue;
            }
            if (type == 6) {
                if (const auto linkset = homeworldz::asset::parse_linkset_asset(content)) {
                    const auto walk_part = [&](const homeworldz::asset::ObjectAsset& part) {
                        for (auto& texture : homeworldz::asset::texture_entry_texture_ids(
                                 std::span(part.texture_entry)))
                            push(texture, 0);
                        for (const auto& item : part.task_inventory)
                            push(item.asset_id, item.asset_type);
                        // A mesh or sculpted prim's shaping asset (type 49).
                        push(part.sculpt_id, 49);
                    };
                    walk_part(linkset->root);
                    for (const auto& child : linkset->children) walk_part(child);
                }
            } else if (type == 5 || type == 13) {
                const std::string text(
                    reinterpret_cast<const char*>(content.data()), content.size());
                if (const auto wearable = homeworldz::viewer::parse_wearable(text))
                    for (const auto& [slot, texture] : wearable->textures) {
                        static_cast<void>(slot);
                        push(homeworldz::viewer::format_uuid(texture), 0);
                    }
            }
        }
        if (unavailable != 0)
            std::cout << "{\"level\":\"warning\",\"message\":\"scene closure members unavailable\","
                         "\"reason\":" << homeworldz::api::json_string(reason)
                      << ",\"walked\":" << queue.size()
                      << ",\"unavailable\":" << unavailable << "}" << std::endl;
    };
    // The whole-scene sweep: every entity already standing when the region
    // starts gets its closure materialized, which both adopts scenes that
    // predate this rule and self-heals any arrival path that misses it — the
    // next restart repairs the gap.
    if (scene.size() != 0) {
        std::vector<std::pair<std::string, int>> seeds;
        for (const auto& [entity_id, entity] : scene.entities()) {
            static_cast<void>(entity_id);
            for (auto& texture : homeworldz::asset::texture_entry_texture_ids(
                     std::span(entity.texture_entry)))
                seeds.emplace_back(std::move(texture), 0);
            for (const auto& item : entity.task_inventory)
                seeds.emplace_back(item.asset_id, item.asset_type);
        }
        if (!seeds.empty()) {
            const auto seed_count = seeds.size();
            materialize_asset_closure(std::move(seeds), "startup sweep");
            std::cout << "{\"level\":\"info\",\"message\":\"scene closure sweep completed\","
                         "\"seeds\":" << seed_count << "}" << std::endl;
        }
    }
    // Clothing alpha masks (TGA) are bundled region files, read straight from
    // the default-avatar asset directory by name (they are not UUID-named
    // assets, so they are not imported into the content-addressed store).
    const std::filesystem::path mask_directory =
        std::filesystem::path(configured_value("region.asset_path", "assets/region")) /
        "default-avatar";
    const auto fetch_mask_bytes =
        [mask_directory](std::string_view name) -> std::optional<std::vector<std::uint8_t>> {
        std::ifstream input(mask_directory / (std::string(name) + ".tga"),
                            std::ios::binary | std::ios::ate);
        if (!input) return std::nullopt;
        const auto length = static_cast<std::streamsize>(input.tellg());
        if (length <= 0) return std::nullopt;
        input.seekg(0);
        std::vector<std::uint8_t> out(static_cast<std::size_t>(length));
        input.read(reinterpret_cast<char*>(out.data()), length);
        if (!input) return std::nullopt;
        return out;
    };
    std::optional<homeworldz::viewer::OutfitBake> default_outfit_bake;
    std::vector<std::uint8_t> default_outfit_visual_params;
    bool default_outfit_bake_attempted = false;
    const auto ensure_default_outfit_bake = [&]() -> const homeworldz::viewer::OutfitBake* {
        if (default_outfit_bake) return &*default_outfit_bake;
        if (default_outfit_bake_attempted) return nullptr;
        default_outfit_bake_attempted = true;
        static const char* const default_wearable_asset_ids[] = {
            "66c41e39-38f9-f75a-024e-585989bfab73",  // Default Shape
            "00000000-0000-1111-9999-000000000202",  // Default Skin
            "d342e6c0-b9d2-11dc-95ff-0800200c9a66",  // Default Hair
            "4bb6fa4d-1cd2-498a-a84c-95c1a0e745a7",  // Default Eyes
            "00000000-38f9-1111-024e-222222111110",  // Default Shirt
            "00000000-38f9-1111-024e-222222111120",  // Default Pants
        };
        std::vector<homeworldz::viewer::Uuid> wearable_ids;
        for (const char* id : default_wearable_asset_ids)
            if (const auto parsed = homeworldz::viewer::parse_uuid(id))
                wearable_ids.push_back(*parsed);
        auto baked = homeworldz::viewer::bake_worn_outfit(
            wearable_ids, server_bake_default_face, fetch_asset_bytes, fetch_mask_bytes);
        if (!baked) {
            std::cerr << "{\"level\":\"warning\",\"message\":\"server default-outfit bake failed\"}"
                      << std::endl;
            return nullptr;
        }
        const std::string system_creator = "00000000-0000-0000-0000-000000000000";
        for (const auto& asset : baked->assets) {
            try {
                const auto content =
                    std::span<const std::byte>(asset.j2c.data(), asset.j2c.size());
                const auto record = storage->store_asset(
                    homeworldz::viewer::format_uuid(asset.id), system_creator, content);
                if (viewer_grid)
                    static_cast<void>(viewer_grid->register_asset(
                        record.viewer_id, record.creator_id, record.sha256, record.size,
                        region_public_endpoint, true));
            } catch (const std::exception& error) {
                std::cerr << "{\"level\":\"warning\",\"message\":\"store baked texture failed\",\"error\":"
                          << homeworldz::api::json_string(error.what()) << "}" << std::endl;
            }
        }
        // appearance_version 1 marks this as a server-side bake so the viewer
        // uses the baked textures directly instead of compositing locally.
        default_outfit_visual_params = homeworldz::viewer::build_visual_params(baked->worn, 1);
        default_outfit_bake = std::move(*baked);
        std::cout << "{\"level\":\"info\",\"message\":\"server default-outfit bake ready\",\"slots\":"
                  << default_outfit_bake->assets.size() << ",\"visualParams\":"
                  << default_outfit_visual_params.size() << "}" << std::endl;
        return &*default_outfit_bake;
    };

    homeworldz::simulation::FixedStepLoop simulation(scene);
#ifdef HOMEWORLDZ_JOLT_AVAILABLE
    auto physics_world = homeworldz::physics::make_jolt_world();
    std::cout << "{\"level\":\"info\",\"message\":\"production physics initialized\","
                 "\"backend\":\"jolt\"}" << std::endl;
#else
    std::unique_ptr<homeworldz::physics::World> physics_world;
    std::cout << "{\"level\":\"warning\",\"message\":\"production physics unavailable\"}" << std::endl;
#endif
    homeworldz::physics::BodyId physics_terrain{};
    const auto synchronize_physics_terrain = [&]() {
        if (!physics_world) return false;
        try {
            // The heightmap has one sample per metre, so the collision surface
            // must too: sample i belongs at x = i exactly.
            //
            // This used to stretch the field instead - spacing =
            // width / (count - 1) - to cover the last metre between the final
            // sample and the region border. That reasoning was sound and the
            // remedy was not: a spacing of 1.000977 on a 1024 region displaces
            // sample i by i * 0.000977, so the ground physics stands on is
            // read up to a metre away from the ground everyone is shown. On
            // flat terrain nothing shows; on a slope an avatar rests above or
            // below the visible surface in proportion to the gradient, which
            // is exactly what the client core measured on the operator's test
            // slope (0.637 m per unit gradient near x = 640, and a spread
            // across the sweep that tracks x rather than the angle).
            //
            // Instead the field is padded by two samples with the edge value
            // repeated: alignment stays exact, the surface extends past the
            // border rather than short of it, and Jolt's requirement that the
            // sample count divide by its block size is satisfied (an odd
            // count is rejected, so one extra sample would not do).
            const auto terrain_samples = terrain_heightmap->width();
            const auto padded = terrain_samples + 2;
            homeworldz::physics::HeightFieldDefinition definition;
            definition.samples.resize(static_cast<std::size_t>(padded) * padded);
            for (std::size_t y = 0; y < padded; ++y) {
                const auto source_y = (std::min)(y, terrain_samples - 1);
                for (std::size_t x = 0; x < padded; ++x) {
                    const auto source_x = (std::min)(x, terrain_samples - 1);
                    definition.samples[y * padded + x] =
                        (*terrain_heightmap)[source_y * terrain_samples + source_x];
                }
            }
            definition.sample_count = static_cast<std::uint32_t>(padded);
            definition.spacing = 1.0;
            const auto replacement = physics_world->create_heightfield(definition);
            if (replacement == 0) return false;
            if (physics_terrain != 0) physics_world->remove_body(physics_terrain);
            physics_terrain = replacement;
            return true;
        } catch (const std::exception& error) {
            std::cerr << "{\"level\":\"error\",\"message\":\"physics terrain synchronization failed\","
                         "\"error\":" << homeworldz::api::json_string(error.what()) << "}" << std::endl;
            return false;
        }
    };
    const auto physics_terrain_ready = synchronize_physics_terrain();
    // The collision surface must be the surface everyone is shown. Nothing
    // enforced that, and a spacing that displaced samples in proportion to
    // their coordinate went unnoticed for as long as the only terrain was
    // flat - where any horizontal misalignment reads identically to none.
    // Checked at integer coordinates, where the heightfield's triangles pass
    // exactly through the samples, so a correct field agrees to float
    // precision and a displaced one cannot.
    if (physics_terrain_ready && physics_world && physics_terrain != 0) {
        // How far a sample may legitimately sit from the heightmap is not one
        // number. Jolt stores each sample as 8 bits within the range of its own
        // 2x2 block, and measures that range over a 3x3 span — one sample of
        // border, "just like we do while building the hierarchical grids"
        // (Jolt's HeightFieldShape.cpp). So a block beside a cliff inherits the
        // cliff's range and its samples turn coarse, while flat ground stays
        // exact. A flat 1 cm tolerance got this backwards at both ends: it
        // called Gamma's brush-carved walls a misalignment (2026-07-31, worst
        // 0.0135 m at a mild point two metres from a 24 m wall) while being
        // slack enough on level ground to miss a real displacement many times
        // over. The bound is now each sample's own quantization step, which is
        // the tightest statement true everywhere — and it tightens to
        // millimetres exactly where the old constant was blindest.
        //
        // The block grid belongs to the padded field, and the heightfield is
        // built from row-reversed samples, so a world row's block neighbours
        // are found back through that reversal rather than by assuming world
        // coordinates land on block boundaries.
        constexpr std::size_t block_size = 2;         // Jolt's mBlockSize default
        constexpr double quantization_steps = 255.0;  // Jolt's mBitsPerSample default
        // Float slack for a ray that falls 4 km before it arrives; well above
        // the observed noise on ground whose block has no relief to quantize.
        constexpr double float_slack = 0.002;
        const auto padded = terrain_width + 2;
        const auto sample_bound = [&](std::size_t x, std::size_t y) {
            const auto column_start = (x / block_size) * block_size;
            const auto reversed_start = ((padded - 1 - y) / block_size) * block_size;
            float low = (std::numeric_limits<float>::max)();
            float high = (std::numeric_limits<float>::lowest)();
            for (std::size_t across = 0; across <= block_size; ++across)
                for (std::size_t down = 0; down <= block_size; ++down) {
                    const auto reversed = reversed_start + down;
                    if (reversed >= padded) continue;
                    // Padding repeats the edge sample, so clamping reads the
                    // same value the padded field holds.
                    const auto column = (std::min)(column_start + across, terrain_width - 1);
                    const auto row = (std::min)(padded - 1 - reversed, terrain_width - 1);
                    const auto value = (*terrain_heightmap)[row * terrain_width + column];
                    low = (std::min)(low, value);
                    high = (std::max)(high, value);
                }
            return static_cast<double>(high - low) / quantization_steps + float_slack;
        };
        // Below any real ratio so the first sample always records: on ground
        // flat enough that nothing deviates at all, a zero here would be
        // reported as the bound and read as "no allowance" rather than "no
        // deviation".
        double worst = 0.0, worst_bound = 0.0, worst_ratio = -1.0;
        int worst_x = 0, worst_y = 0;
        const auto step = (std::max<std::size_t>)(1, terrain_width / 16);
        for (std::size_t y = 0; y + 1 < terrain_width; y += step)
            for (std::size_t x = 0; x + 1 < terrain_width; x += step) {
                const auto hit = physics_world->ray_cast_body(
                    physics_terrain, {static_cast<double>(x), static_cast<double>(y), 4096.0},
                    {0, 0, -1}, 8192.0);
                if (!hit) continue;
                const auto expected = (*terrain_heightmap)[y * terrain_width + x];
                const auto difference = std::abs(hit->point.z - expected);
                const auto bound = sample_bound(x, y);
                // Ranked by how much of its own allowance a sample used, not by
                // metres: the largest deviation on a cliff is routinely more
                // correct than a small one on a plain.
                const auto ratio = difference / bound;
                if (ratio > worst_ratio) {
                    worst_ratio = ratio;
                    worst = difference;
                    worst_bound = bound;
                    worst_x = static_cast<int>(x);
                    worst_y = static_cast<int>(y);
                }
            }
        const bool aligned = worst_ratio <= 1.0;
        std::cout << "{\"level\":" << (aligned ? "\"info\"" : "\"error\"")
                  << ",\"message\":\"terrain collision alignment checked\""
                     ",\"worstDeviation\":" << worst
                  << ",\"quantizationBound\":" << worst_bound
                  << ",\"boundUsed\":" << worst_ratio
                  << ",\"atX\":" << worst_x << ",\"atY\":" << worst_y
                  << ",\"aligned\":" << (aligned ? "true" : "false") << "}" << std::endl;
    }
    std::cout << "{\"level\":" << (physics_terrain_ready ? "\"info\"" : "\"warning\"")
              << ",\"message\":\"physics terrain initialized\",\"samples\":"
              << terrain_heightmap->size() << ",\"synchronized\":"
              << (physics_terrain_ready ? "true" : "false") << "}" << std::endl;
    std::unique_ptr<homeworldz::physics::StaticSceneMirror> physics_scene;
    std::unordered_map<homeworldz::scene::EntityId, std::size_t> physics_edit_suspended;
    std::unordered_map<std::string,
        std::unordered_map<homeworldz::scene::EntityId, homeworldz::scene::EntityId>>
        physics_edit_selections;
    if (physics_world) {
        try {
            physics_scene = std::make_unique<homeworldz::physics::StaticSceneMirror>(*physics_world);
            physics_scene->synchronize(scene);
            std::cout << "{\"level\":\"info\",\"message\":\"static scene physics synchronized\","
                         "\"bodies\":" << physics_scene->size() << "}" << std::endl;
        } catch (const std::exception& error) {
            physics_scene.reset();
            std::cerr << "{\"level\":\"error\",\"message\":\"static scene physics synchronization failed\","
                         "\"error\":" << homeworldz::api::json_string(error.what()) << "}" << std::endl;
        }
    }
    const auto synchronize_physics_object = [&](const homeworldz::scene::Entity& entity) {
        if (!physics_scene) return;
        try {
            const auto root_id = entity.parent_id == 0 ? entity.id : entity.parent_id;
            if (!physics_scene->synchronize_linkset(
                    scene, root_id, physics_edit_suspended.contains(root_id)))
                std::cerr << "{\"level\":\"warning\",\"message\":\"object physics synchronization rejected\","
                             "\"entityId\":" << root_id << "}" << std::endl;
        } catch (const std::exception& error) {
            std::cerr << "{\"level\":\"error\",\"message\":\"static object physics synchronization failed\","
                         "\"entityId\":" << entity.id << ",\"error\":"
                      << homeworldz::api::json_string(error.what()) << "}" << std::endl;
        }
    };
    const auto remove_physics_object = [&](homeworldz::scene::EntityId entity_id) {
        physics_edit_suspended.erase(entity_id);
        for (auto& [selection_endpoint, selected] : physics_edit_selections) {
            static_cast<void>(selection_endpoint);
            std::erase_if(selected, [&](const auto& entry) {
                return entry.first == entity_id || entry.second == entity_id;
            });
        }
        if (physics_scene) physics_scene->remove(entity_id);
    };
    const auto character_definition = [&](homeworldz::scene::EntityId entity,
                                          const homeworldz::scene::Vector3& position,
                                          double height) {
        homeworldz::physics::CharacterDefinition definition;
        definition.entity_id = entity;
        definition.position = position;
        definition.radius = homeworldz::viewer::avatar_capsule_radius;
        definition.height = height;
        definition.walkable_slope_degrees = walkable_slope_degrees;
        return definition;
    };
    const auto collision_ground_height = [&](const homeworldz::scene::Vector3& position) {
        if (physics_world && physics_terrain != 0) {
            constexpr double ray_origin_height = 4096.0;
            constexpr double ray_distance = 8192.0;
            const auto hit = physics_world->ray_cast_body(
                physics_terrain, {position.x, position.y, ray_origin_height}, {0, 0, -1}, ray_distance);
            if (hit) return hit->point.z;
        }
        return ground_height(*terrain_heightmap, position);
    };
    const auto raise_physical_object_above_terrain = [&](homeworldz::scene::Entity& entity) {
        if (!entity.physical) return false;
        const auto squared = entity.rotation.x * entity.rotation.x +
                             entity.rotation.y * entity.rotation.y +
                             entity.rotation.z * entity.rotation.z;
        const std::array<double, 4> rotation{
            entity.rotation.x, entity.rotation.y, entity.rotation.z,
            std::sqrt((std::max)(0.0, 1.0 - squared))};
        const auto half_extents =
            homeworldz::physics::rotated_box_half_extents(entity.scale, rotation);
        const auto terrain_maximum = static_cast<int>(terrain_width - 1);
        const auto minimum_x = std::clamp(
            static_cast<int>(std::floor(entity.position.x - half_extents.x)), 0, terrain_maximum);
        const auto maximum_x = std::clamp(
            static_cast<int>(std::ceil(entity.position.x + half_extents.x)), 0, terrain_maximum);
        const auto minimum_y = std::clamp(
            static_cast<int>(std::floor(entity.position.y - half_extents.y)), 0, terrain_maximum);
        const auto maximum_y = std::clamp(
            static_cast<int>(std::ceil(entity.position.y + half_extents.y)), 0, terrain_maximum);
        double maximum_ground = -std::numeric_limits<double>::infinity();
        for (int y = minimum_y; y <= maximum_y; ++y)
            for (int x = minimum_x; x <= maximum_x; ++x)
                maximum_ground = (std::max)(maximum_ground,
                    static_cast<double>((*terrain_heightmap)[
                        static_cast<std::size_t>(y) * terrain_width + x]));
        constexpr double terrain_clearance = 0.01;
        const auto required_origin_z = maximum_ground + half_extents.z + terrain_clearance;
        if (entity.position.z >= required_origin_z) return false;
        entity.position.z = required_origin_z;
        entity.velocity = {};
        return true;
    };
    bool recovered_escaped_objects{};
    std::vector<homeworldz::scene::EntityId> persisted_entity_ids;
    persisted_entity_ids.reserve(scene.size());
    for (const auto& [entity_id, entity] : scene.entities()) {
        static_cast<void>(entity);
        persisted_entity_ids.push_back(entity_id);
    }
    for (const auto entity_id : persisted_entity_ids) {
        auto* entity = scene.find(entity_id);
        if (!entity || !entity->physical) continue;
        const auto original_x = entity->position.x;
        const auto original_y = entity->position.y;
        entity->position.x = std::clamp(entity->position.x, 0.0, static_cast<double>(region_size_x));
        entity->position.y = std::clamp(entity->position.y, 0.0, static_cast<double>(region_size_y));
        const bool escaped = entity->position.x != original_x || entity->position.y != original_y;
        if (escaped) {
            entity->velocity.x = 0.0;
            entity->velocity.y = 0.0;
        }
        const bool raised = (escaped || entity->position.z < -64.0) &&
            raise_physical_object_above_terrain(*entity);
        if (!escaped && !raised) continue;
        recovered_escaped_objects = true;
        synchronize_physics_object(*entity);
        std::cout << "{\"level\":\"warning\",\"message\":\"escaped physical object recovered\",\"entityId\":"
                  << entity->id << ",\"position\":[" << entity->position.x << ','
                  << entity->position.y << ',' << entity->position.z << "]}" << std::endl;
    }
    if (recovered_escaped_objects) {
        try {
            storage->save_snapshot(scene);
        } catch (const std::exception& error) {
            std::cerr << "{\"level\":\"error\",\"message\":\"escaped object recovery persistence failed\",\"error\":"
                      << homeworldz::api::json_string(error.what()) << "}" << std::endl;
            return 1;
        }
    }
    auto previous_tick = std::chrono::steady_clock::now();
    auto next_snapshot = previous_tick + std::chrono::seconds(30);
    auto next_parcel_sweep = previous_tick + std::chrono::seconds(30);
    // Monotonic sequence id for "agent parcel" ParcelProperties updates. The viewer
    // uses a positive, increasing sequence to know which parcel the avatar stands in
    // (distinct from the negative sentinels used for explicit About Land selection).
    std::uint32_t agent_parcel_sequence = 0;
    // When each auto-return-eligible object was first seen on its parcel.
    std::unordered_map<homeworldz::scene::EntityId, std::chrono::steady_clock::time_point>
        object_clean_since;
    auto next_dynamic_sync = previous_tick;

    const auto server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == invalid_socket) return 1;
    int reuse = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in address{};
    if (!configured_bind_address(address, "region.bind_address", configured_port()) ||
        bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(server, 16) != 0) {
        close_socket(server);
        return 1;
    }
    // The listening socket is non-blocking because the loop drains the whole
    // accept backlog each pass: a blocking accept returns nothing to take once
    // the queue empties and waits there forever, which stops the region on its
    // first connection. Caught by the stall test, not by the compiler.
    set_socket_blocking_mode(server, false);

    const auto viewer_server = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (viewer_server == invalid_socket) {
        close_socket(server);
        return 1;
    }
    sockaddr_in viewer_address{};
    if (!configured_bind_address(viewer_address, "region.viewer_bind_address", region_viewer_port) ||
        bind(viewer_server, reinterpret_cast<sockaddr*>(&viewer_address), sizeof(viewer_address)) != 0) {
        close_socket(viewer_server);
        close_socket(server);
        return 1;
    }
    homeworldz::region::InboundTransitRegistry inbound_transits;
    homeworldz::region::CapabilityArrivalGate capability_arrival_gate;
    homeworldz::viewer::CircuitRegistry circuits([&](const homeworldz::viewer::UseCircuitCode& request) {
        const auto reject = [&](std::string_view reason) {
            std::cout << "{\"level\":\"warn\",\"message\":\"viewer circuit rejected\",\"reason\":"
                      << homeworldz::api::json_string(reason)
                      << ",\"circuitCode\":" << request.circuit_code
                      << ",\"sessionId\":"
                      << homeworldz::api::json_string(homeworldz::viewer::format_uuid(request.session_id))
                      << ",\"agentId\":"
                      << homeworldz::api::json_string(homeworldz::viewer::format_uuid(request.agent_id))
                      << "}" << std::endl;
            return false;
        };
        if (!registration || !viewer_sessions) return reject("region_not_registered");
        std::optional<homeworldz::grid::ViewerSession> session;
        try {
            session = viewer_sessions->validate(homeworldz::viewer::format_uuid(request.session_id));
        } catch (const std::exception& error) {
            std::cout << "{\"level\":\"error\",\"message\":\"viewer session validation failed\",\"error\":"
                      << homeworldz::api::json_string(error.what()) << "}" << std::endl;
            return reject("session_validation_error");
        }
        if (!session) return reject("session_not_found");
        if (session->circuit_code != request.circuit_code) return reject("circuit_code_mismatch");
        const auto agent = homeworldz::viewer::parse_uuid(session->agent_id);
        if (!agent) return reject("invalid_session_agent");
        if (*agent != request.agent_id) return reject("agent_id_mismatch");
        if (session->destination_region_id != registration->region_id()) {
            const auto transit = inbound_transits.authorize(
                session->agent_id, session->session_id, std::chrono::steady_clock::now());
            if (!transit || transit->destination_region_id != registration->region_id())
                return reject("destination_region_mismatch");
        }
        std::cout << "{\"level\":\"info\",\"message\":\"viewer circuit authorized\",\"circuitCode\":"
                  << request.circuit_code << ",\"sessionId\":"
                  << homeworldz::api::json_string(homeworldz::viewer::format_uuid(request.session_id))
                  << "}" << std::endl;
        return true;
    });
    std::unordered_set<std::string> handshake_replies;
    std::unordered_set<std::string> established_events;
    std::unordered_set<std::string> parcel_overlay_sent;
    std::unordered_map<std::string, LiveAvatar> avatars;
    std::unordered_map<std::string, homeworldz::viewer::AvatarGeometry> avatar_geometries;
    std::unordered_map<std::string, homeworldz::viewer::AgentSetAppearance> avatar_appearances;
    std::unordered_map<std::string, std::vector<homeworldz::viewer::AvatarAnimationEntry>> avatar_animations;
    std::unordered_map<std::string, std::int32_t> next_animation_sequences;
    std::unordered_map<std::string, homeworldz::viewer::MovementAnimation> movement_animations;
    std::unordered_map<std::string, homeworldz::viewer::UuidName> resolved_avatar_names;
    std::unordered_map<std::string, std::deque<QueuedTexturePacket>> texture_packets;
    std::unordered_set<std::string> active_texture_transfers;
    std::unordered_map<std::string, PendingInventoryUpload> pending_inventory_uploads;
    std::unordered_map<std::string, PendingMeshModelUpload> pending_mesh_model_uploads;
    // Dirty terrain patches awaiting one coalesced session event, keyed
    // (y << 8) | x so a brush passing over the same patch repeatedly counts
    // once. Flushed on the tick below at a bounded rate.
    std::set<std::uint32_t> pending_terrain_patches;
    std::chrono::steady_clock::time_point next_terrain_notice{};
    // When the last change was announced, so a client refetching from behind
    // can be told apart from one that was simply away: those are different
    // facts and a log line that renders them the same has stopped being a
    // check (client core, 2026-07-30).
    std::chrono::steady_clock::time_point last_terrain_notice_at{};
    // Terrain edited in memory but not yet mirrored into physics, persisted, or
    // shown to anyone. Each of those is region-scale work that must not run per
    // brush tick; they run at their own cadences below.
    std::set<std::uint32_t> pending_viewer_terrain_patches;
    std::chrono::steady_clock::time_point next_viewer_terrain_notice{};
    bool terrain_dirty = false;
    std::chrono::steady_clock::time_point next_terrain_physics{};
    std::chrono::steady_clock::time_point next_terrain_persist{};
    std::unordered_map<std::string, PendingInventoryAssetUpdate> pending_inventory_asset_updates;
    std::unordered_map<std::string, PendingInventoryAssetUpload> pending_inventory_asset_uploads;
    std::unordered_map<std::string, PendingInventoryAssetXfer> pending_inventory_asset_xfers;
    std::unordered_map<std::string, std::vector<std::byte>> pending_task_inventory_files;
    std::unordered_map<std::string, PendingTaskInventoryXfer> pending_task_inventory_xfers;
    std::unordered_map<std::string,
        std::unordered_map<homeworldz::scene::EntityId, SentDynamicTransform>>
        sent_dynamic_transforms;
    std::unordered_map<homeworldz::scene::EntityId, std::chrono::steady_clock::time_point>
        temporary_expirations;
    std::unordered_map<std::string, std::deque<std::string>> queued_viewer_events;
    std::vector<PendingEventResponse> pending_event_responses;
    std::vector<PendingAgentMovementComplete> pending_agent_movement_completes;
    std::uint64_t event_id{};
    std::uint64_t next_inventory_asset_xfer{1};
    homeworldz::script::FalconRuntime falcon([&](homeworldz::script::FalconHostMessage message) {
        if (message.text.empty() || message.text.size() > 1023) return;
        const auto object_id = homeworldz::viewer::parse_uuid(message.identity.object_id);
        const auto owner_id = homeworldz::viewer::parse_uuid(message.identity.owner_id);
        if (!object_id || !owner_id) return;
        const homeworldz::scene::Entity* speaker = nullptr;
        for (const auto& [entity_id, candidate] : scene.entities()) {
            static_cast<void>(entity_id);
            if (candidate.object_id == message.identity.object_id) {
                speaker = &candidate;
                break;
            }
        }
        if (!speaker) return;
        if (!message.owner_only && message.channel != 0) return;

        homeworldz::viewer::ChatFromSimulator chat;
        chat.from_name = speaker->name.empty() ? "Object" : speaker->name;
        chat.source_id = *object_id;
        chat.owner_id = *owner_id;
        chat.source_type = 0x02;
        chat.chat_type = message.owner_only ? 0x08 : 0x01;
        chat.audible = 0x01;
        chat.position = {static_cast<float>(speaker->position.x),
                         static_cast<float>(speaker->position.y),
                         static_cast<float>(speaker->position.z)};
        chat.message = std::move(message.text);
        // Public script chat also reaches authenticated region sessions; the
        // session has no position yet, so it hears the whole region.
        if (session_server && !message.owner_only)
            session_server->broadcast_chat(chat.from_name, chat.message);
        const auto payload = homeworldz::viewer::encode_chat_from_simulator(chat);
        const auto sent_at = std::chrono::steady_clock::now();
        for (const auto& [recipient_endpoint, recipient] : avatars) {
            if (message.owner_only) {
                if (recipient.user_id != message.identity.owner_id) continue;
            } else {
                const auto& target = recipient.controller.state().position;
                const auto dx = target.x - speaker->position.x;
                const auto dy = target.y - speaker->position.y;
                const auto dz = target.z - speaker->position.z;
                if (dx * dx + dy * dy + dz * dz > 20.0 * 20.0) continue;
            }
            if (const auto outgoing = circuits.send(
                    recipient_endpoint, payload, true, sent_at))
                static_cast<void>(send_udp(viewer_server, recipient_endpoint, *outgoing));
        }
    });
    const auto rez_task_script = [&](const homeworldz::scene::Entity& entity,
                                     const homeworldz::scene::TaskInventoryItem& item,
                                     bool enabled) {
        // Parcel script policy: a script only runs when its parcel allows scripts
        // for the object owner (AllowOtherScripts, or owner/region-owner). The item
        // stays in contents; it just does not execute where scripts are disallowed.
        bool effective_enabled = enabled;
        if (enabled && parcels) {
            if (const auto* parcel = parcels->parcel_at(
                    static_cast<float>(entity.position.x), static_cast<float>(entity.position.y)))
                effective_enabled =
                    homeworldz::parcel::can_run_scripts(*parcel, entity.owner_id, region_owner_id);
        }
        try {
            const auto asset = read_federated_asset(item.asset_id);
            const auto source = std::string(
                reinterpret_cast<const char*>(asset.data()), asset.size());
            return falcon.rez(
                {item.asset_id, item.item_id, entity.object_id, entity.owner_id},
                source, effective_enabled);
        } catch (const std::exception& error) {
            return homeworldz::script::FalconRezResult{false, false, error.what()};
        }
    };
    // Re-send an entity's ObjectUpdate to every nearby viewer. A script rez,
    // recompile, enable, disable, or removal changes the SCRIPTED / HANDLE_TOUCH
    // flags, and the viewer only learns the new flags from a fresh update; without
    // this it keeps showing Touch as disabled on a freshly touch-enabled prim.
    // --- Region-session embodiment (docs/CLIENT2-EMBODIMENT.md, milestone E1).
    // Session participants live in the same avatars map under "ws:<session>"
    // keys; every UDP loop is inert for them (no circuit), and these helpers
    // are the session-facing halves of the fan-outs that matter for E1.
    std::unordered_map<std::string, double> session_draw_distances;
    // Which avatars each embodied session currently knows about. Enter and
    // leave are events the interest sweep emits — because interest changes
    // when either party moves, not only when the subject does — and avatar
    // transforms flow only for pairs already in interest.
    std::unordered_map<std::string, std::unordered_set<homeworldz::scene::EntityId>>
        session_avatar_interest;
    auto next_session_interest_sweep = previous_tick;
    const auto session_interested = [](const LiveAvatar& observer, const LiveAvatar& subject) {
        // An avatar is about a metre wide; grant that so a body does not pop
        // exactly on the boundary. A zero draw distance would mean no filter,
        // which a session can never have (it is clamped at spawn).
        constexpr double avatar_radius = 1.0;
        const auto draw = static_cast<double>(observer.controller.state().draw_distance);
        if (draw <= 0.0) return true;
        const auto& from = observer.controller.state().position;
        const auto& to = subject.controller.state().position;
        const auto dx = from.x - to.x, dy = from.y - to.y, dz = from.z - to.z;
        const auto reach = draw + avatar_radius;
        return dx * dx + dy * dy + dz * dz <= reach * reach;
    };
    const auto session_vec3 = [](double x, double y, double z) {
        return "[" + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z) + "]";
    };
    const auto session_quat_w = [](double x, double y, double z) {
        return std::sqrt((std::max)(0.0, 1.0 - x * x - y * y - z * z));
    };
    const auto deliver_to_embodied = [&](const std::string& envelope, bool droppable = false) {
        if (!session_server) return;
        for (const auto& [key, participant] : avatars) {
            static_cast<void>(key);
            if (participant.transport == AvatarTransport::session)
                session_server->send_to(participant.session_id, envelope, droppable);
        }
    };
    // An entity with no object id is not a rezzed object — an avatar's own
    // scene entity is the common case, and it persists after its owner
    // leaves. The viewer path filters these out inside
    // static_object_from_entity; session delivery must filter them too, or a
    // client is told an offline avatar's body is an object (and sees its
    // stale, possibly out-of-bounds, last position).
    const auto session_object_visible = [](const homeworldz::scene::Entity& entity) {
        return !entity.object_id.empty() && !entity.owner_id.empty();
    };
    const auto session_object_envelope = [&](const homeworldz::scene::Entity& entity) {
        return homeworldz::session::encode_envelope("object", {},
            "{\"id\":\"" + std::to_string(entity.id) + "\"" +
            ",\"objectId\":" + homeworldz::session::json_string(entity.object_id) +
            ",\"ownerId\":" + homeworldz::session::json_string(entity.owner_id) +
            ",\"name\":" + homeworldz::session::json_string(entity.name) +
            ",\"position\":" + session_vec3(entity.position.x, entity.position.y, entity.position.z) +
            ",\"rotation\":[" + std::to_string(entity.rotation.x) + "," +
                std::to_string(entity.rotation.y) + "," + std::to_string(entity.rotation.z) + "," +
                std::to_string(session_quat_w(entity.rotation.x, entity.rotation.y, entity.rotation.z)) + "]" +
            ",\"scale\":" + session_vec3(entity.scale.x, entity.scale.y, entity.scale.z) +
            // What geometry this part uses, when it is not a prim shape. Without
            // it a client receiving scene traffic can only draw placeholder
            // boxes — it knows where every object is and never what any of them
            // looks like (client core, 2026-07-29). Absent for prims, so a
            // reader written before this field keeps working; the bytes come
            // from /session/assets/{assetId}, whose Content-Type names the
            // format (a session-uploaded mesh is glTF, a viewer-uploaded one is
            // Second Life mesh).
            (entity.sculpt_id.empty() ? std::string{} :
                ",\"geometry\":{\"assetId\":" +
                    homeworldz::session::json_string(entity.sculpt_id) +
                    ",\"kind\":" + homeworldz::session::json_string(
                        entity.sculpt_type == 5 ? "mesh" : "sculptMap") + "}") +
            "}");
    };
    // Every animation active on this avatar, as formatted ids. The decision about
    // which of them are clips and which are already named by `motion` belongs to
    // motion_fields_json, so it cannot be made differently in two places or
    // forgotten in one.
    const auto session_motion_fields = [&](const LiveAvatar& participant, const std::string& key) {
        std::vector<std::string> playing;
        if (const auto found = avatar_animations.find(key); found != avatar_animations.end()) {
            playing.reserve(found->second.size());
            for (const auto& entry : found->second)
                playing.push_back(homeworldz::viewer::format_uuid(entry.animation_id));
        }
        return homeworldz::viewer::motion_fields_json(
            participant.controller.movement_animation(), playing);
    };
    const auto session_motion_envelope = [&](const LiveAvatar& participant, const std::string& key) {
        return homeworldz::session::encode_envelope("motion", {},
            "{\"id\":\"" + std::to_string(participant.entity_id) + "\"," +
            session_motion_fields(participant, key) + "}");
    };
    const auto session_avatar_envelope = [&](const LiveAvatar& participant, const std::string& key) {
        const auto& state = participant.controller.state();
        return homeworldz::session::encode_envelope("avatar", {},
            "{\"id\":\"" + std::to_string(participant.entity_id) + "\"" +
            ",\"userId\":" + homeworldz::session::json_string(participant.user_id) +
            ",\"position\":" + session_vec3(state.position.x, state.position.y, state.position.z) +
            ",\"rotation\":[" + std::to_string(state.rotation[0]) + "," +
                std::to_string(state.rotation[1]) + "," + std::to_string(state.rotation[2]) + "]" +
            // What this avatar is doing right now, so a client that arrives
            // mid-stride is not left standing until the next change. `motion`
            // envelopes carry the same fields from then on - one thing to parse,
            // whether it came as initial state or as an update.
            "," + session_motion_fields(participant, key) + "}");
    };
    const auto session_kill_envelope = [](homeworldz::scene::EntityId entity_id) {
        return homeworldz::session::encode_envelope("kill", {},
            "{\"ids\":[\"" + std::to_string(entity_id) + "\"]}");
    };
    // Objects leave a scene as often as avatars do — deleted, derezzed,
    // returned, expired — and a session client keeps what it was told about,
    // so every kill the viewers get must reach sessions too.
    const auto session_kill_many = [](const auto& local_ids) {
        std::string list;
        for (const auto id : local_ids) {
            if (!list.empty()) list.push_back(',');
            list += "\"" + std::to_string(id) + "\"";
        }
        return homeworldz::session::encode_envelope("kill", {}, "{\"ids\":[" + list + "]}");
    };
    // An attachment is an ordinary scene entity parented to its wearer's avatar
    // entity, with a non-zero attachment_point. Taking one off has to reach both
    // viewers and sessions, and it happens from three places: wearing something
    // that displaces it, an explicit detach, and the wearer leaving. That last
    // one is not housekeeping — an attachment that outlives its wearer is a prim
    // parented to an entity that no longer exists.
    const auto remove_attachment_linkset = [&](homeworldz::scene::EntityId root_id,
                                               std::chrono::steady_clock::time_point when) {
        std::vector<homeworldz::scene::EntityId> part_ids;
        for (const auto& [candidate_id, candidate] : scene.entities())
            if (candidate.parent_id == root_id) part_ids.push_back(candidate_id);
        part_ids.push_back(root_id);
        std::vector<std::uint32_t> killed;
        for (const auto part : part_ids)
            if (scene.remove(part)) killed.push_back(static_cast<std::uint32_t>(part));
        if (killed.empty()) return killed;
        const auto kill = homeworldz::viewer::encode_kill_object(killed);
        for (const auto& [recipient_endpoint, recipient] : avatars) {
            static_cast<void>(recipient);
            if (const auto outgoing = circuits.send(recipient_endpoint, kill, true, when))
                static_cast<void>(send_udp(viewer_server, recipient_endpoint, *outgoing));
        }
        deliver_to_embodied(session_kill_many(killed));
        return killed;
    };
    const auto remove_avatar_attachments = [&](homeworldz::scene::EntityId wearer_id,
                                               std::chrono::steady_clock::time_point when) {
        std::vector<homeworldz::scene::EntityId> roots;
        for (const auto& [candidate_id, candidate] : scene.entities())
            if (candidate.attachment_point != 0 && candidate.parent_id == wearer_id)
                roots.push_back(candidate_id);
        std::size_t removed = 0;
        for (const auto root_id : roots)
            if (!remove_attachment_linkset(root_id, when).empty()) ++removed;
        return removed;
    };
    // retire_session_avatar removes an embodied session's avatar: kill to
    // viewers and other sessions, physics teardown, map erasure. The session
    // itself stays open (back to observer) unless the socket already closed.
    const auto retire_session_avatar = [&](const std::string& participant_key) {
        const auto found = avatars.find(participant_key);
        if (found == avatars.end()) return;
        const auto entity_id = found->second.entity_id;
        const std::array<std::uint32_t, 1> kill_ids{static_cast<std::uint32_t>(entity_id)};
        const auto kill = homeworldz::viewer::encode_kill_object(kill_ids);
        const auto kill_now = std::chrono::steady_clock::now();
        // What this avatar was wearing goes with it.
        static_cast<void>(remove_avatar_attachments(entity_id, kill_now));
        for (const auto& [recipient_endpoint, recipient] : avatars) {
            static_cast<void>(recipient);
            if (recipient_endpoint == participant_key) continue;
            if (const auto outgoing = circuits.send(recipient_endpoint, kill, true, kill_now, true))
                static_cast<void>(send_udp(viewer_server, recipient_endpoint, *outgoing));
        }
        if (physics_world && found->second.physics_character != 0)
            physics_world->remove_character(found->second.physics_character);
        if (viewer_grid && registration) {
            // Persist where the avatar stood, so re-entry with start=last
            // lands sensibly — including after a crossing the client never
            // completes.
            const auto& state = found->second.controller.state();
            static_cast<void>(viewer_grid->update_last_location(
                found->second.user_id, registration->region_id(),
                {static_cast<float>(state.position.x), static_cast<float>(state.position.y),
                 static_cast<float>(state.position.z)},
                state.rotation, state.flying));
        }
        if (viewer_grid)
            static_cast<void>(viewer_grid->clear_presence(found->second.user_id));
        session_draw_distances.erase(found->second.session_id);
        sent_dynamic_transforms.erase(participant_key);
        session_avatar_interest.erase(participant_key);
        // Forget this avatar in every other session's view, so a later
        // arrival is announced afresh rather than assumed known.
        for (auto& [other_key, known] : session_avatar_interest) {
            static_cast<void>(other_key);
            known.erase(entity_id);
        }
        avatar_appearances.erase(participant_key);
        avatar_geometries.erase(participant_key);
        avatar_animations.erase(participant_key);
        next_animation_sequences.erase(participant_key);
        movement_animations.erase(participant_key);
        avatars.erase(found);
        deliver_to_embodied(session_kill_envelope(entity_id));
        std::cout << "{\"level\":\"info\",\"message\":\"session avatar retired\",\"localId\":"
                  << static_cast<std::uint64_t>(entity_id) << "}" << std::endl;
    };
    const auto broadcast_object_update = [&](const homeworldz::scene::Entity& entity,
                                             std::chrono::steady_clock::time_point when) {
        const auto region_handle =
            (static_cast<std::uint64_t>(region_grid_x * 256) << 32) |
            static_cast<std::uint32_t>(region_grid_y * 256);
        for (const auto& [recipient_endpoint, recipient] : avatars) {
            const auto object =
                static_object_from_entity(scene, entity, recipient.user_id, falcon);
            if (!object) continue;
            if (const auto sent = circuits.send(
                    recipient_endpoint,
                    homeworldz::viewer::encode_static_object_update(region_handle, *object),
                    true, when, true))
                static_cast<void>(send_udp(viewer_server, recipient_endpoint, *sent));
        }
        if (session_object_visible(entity)) deliver_to_embodied(session_object_envelope(entity));
    };
    // The outcome of one wear. `refused` is empty only when the object is on,
    // and `recorded` is separate from `worn` because an attachment the grid did
    // not record is worn now and gone at the next login — a difference the log
    // has to be able to state.
    struct WearOutcome {
        bool worn{};
        bool recorded{};
        std::uint8_t point{};
        std::size_t prims{};
        std::string refused;
        // The refusal is an inability, not an answer: the grid could not be
        // asked, so nothing has been decided about the item. Reported as a
        // check that could not be run rather than as a wear that was refused —
        // a probe that convicts on silence spends the credibility it needs for
        // the times it is right (client core, 2026-08-08).
        bool inconclusive{};
    };
    // wear_attachment rezzes one inventory item onto a wearer's avatar. Two
    // callers: the viewer's Wear, and an avatar arriving with worn items the
    // grid remembers. `requested_point` is the viewer's point with
    // ATTACHMENT_ADD stripped, or 0 to mean "wherever the item says".
    //
    // An arrival passes keep_others=true and record=false: the grid's list is
    // already the record, and a second item legitimately sharing a point must
    // not evict the first one as it is rezzed back.
    const auto wear_attachment = [&](const std::string& user_id,
                                     homeworldz::scene::EntityId wearer_id,
                                     const std::string& item_id, std::uint8_t requested_point,
                                     bool keep_others, bool record,
                                     std::chrono::steady_clock::time_point when) -> WearOutcome {
        // indra's ATTACHMENT_RIGHT_HAND: where an item with no remembered point
        // goes, matching what a viewer expects from a plain Wear.
        constexpr std::uint8_t attachment_point_right_hand = 5;
        constexpr std::uint8_t attachment_point_mask =
            static_cast<std::uint8_t>(~homeworldz::viewer::attachment_add);
        WearOutcome outcome;
        outcome.point = requested_point & attachment_point_mask;
        std::vector<homeworldz::scene::EntityId> entity_ids;
        try {
            const auto lookup = viewer_grid
                ? viewer_grid->lookup_inventory_item(user_id, item_id)
                : homeworldz::grid::InventoryItemLookup{};
            const auto& item = lookup.item;
            if (!scene.find(wearer_id)) outcome.refused = "wearer has no avatar here";
            else if (lookup.outcome == homeworldz::grid::InventoryLookup::unavailable) {
                outcome.refused = "inventory could not be checked";
                outcome.inconclusive = true;
            }
            else if (!item) outcome.refused = "inventory item not found";
            else if (item->asset_type != 6 || item->inventory_type != 6)
                outcome.refused = "inventory item is not an object";
            else {
                // A plain Wear sends point 0 and leaves the choice to the
                // region. indra keeps the point an item was last worn on in the
                // low byte of its inventory flags.
                if (outcome.point == 0)
                    outcome.point = static_cast<std::uint8_t>(item->flags & 0xffU) &
                                    attachment_point_mask;
                if (outcome.point == 0) outcome.point = attachment_point_right_hand;
                // Wearing replaces the point's occupant, and re-wearing an item
                // already on moves it rather than doubling it. "Attach To >"
                // sets ATTACHMENT_ADD to keep the rest.
                std::vector<std::pair<homeworldz::scene::EntityId, std::string>> displaced;
                for (const auto& [candidate_id, candidate] : scene.entities()) {
                    if (candidate.attachment_point == 0 || candidate.parent_id != wearer_id)
                        continue;
                    if (candidate.attachment_item_id == item_id ||
                        (!keep_others && candidate.attachment_point == outcome.point))
                        displaced.emplace_back(candidate_id, candidate.attachment_item_id);
                }
                for (const auto& [displaced_id, displaced_item] : displaced) {
                    static_cast<void>(remove_attachment_linkset(displaced_id, when));
                    // Displaced means taken off, and the grid has to agree or
                    // the next login rezzes back something the wearer replaced.
                    if (viewer_grid && !displaced_item.empty() && displaced_item != item_id)
                        static_cast<void>(viewer_grid->set_attachment_worn(
                            user_id, displaced_item, 0, false));
                }
                const auto content = read_federated_asset(item->asset_id);
                const auto linkset = homeworldz::asset::parse_linkset_asset(content);
                if (!linkset) outcome.refused = "object asset did not parse";
                else {
                    const auto& asset = linkset->root;
                    const auto* wearer_entity = scene.find(wearer_id);
                    if (!wearer_entity) throw std::runtime_error("wearer entity");
                    const auto root_id = scene.create(item->name, wearer_entity->position);
                    entity_ids.push_back(root_id);
                    auto* entity = scene.find(root_id);
                    if (!entity) throw std::runtime_error("create attachment root");
                    entity->object_id = homeworldz::viewer::random_uuid();
                    entity->owner_id = user_id;
                    entity->creator_id = item->creator_id;
                    apply_object_asset(*entity, asset);
                    entity->description = item->description.empty()
                        ? asset.description : item->description;
                    entity->base_permissions = item->base_permissions;
                    entity->owner_permissions = item->current_permissions;
                    entity->everyone_permissions = item->everyone_permissions;
                    entity->next_owner_permissions = item->next_permissions;
                    entity->creation_date = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count());
                    entity->parent_id = wearer_id;
                    entity->attachment_point = outcome.point;
                    entity->attachment_item_id = item_id;
                    // A physical prim that is worn would be simulated while
                    // parented to an avatar the physics world does not know as a
                    // parent. Worn means carried.
                    entity->physical = false;
                    // Worn at the joint itself: the offset an object was last
                    // taken off at is not stored anywhere yet, so there is
                    // nothing truthful to restore.
                    entity->local_position = {};
                    entity->local_rotation = {};
                    for (const auto& child_asset : linkset->children) {
                        const auto child_id = scene.create(
                            child_asset.name.empty() ? "Primitive" : child_asset.name);
                        entity_ids.push_back(child_id);
                        auto* child = scene.find(child_id);
                        if (!child) throw std::runtime_error("create attachment child");
                        child->object_id = homeworldz::viewer::random_uuid();
                        child->owner_id = user_id;
                        child->creator_id = child_asset.creator_id.empty()
                            ? item->creator_id : child_asset.creator_id;
                        apply_object_asset(*child, child_asset);
                        child->description = child_asset.description;
                        child->base_permissions =
                            child_asset.base_permissions & item->base_permissions;
                        child->owner_permissions =
                            child_asset.owner_permissions & item->current_permissions;
                        child->group_permissions = child_asset.group_permissions;
                        child->everyone_permissions =
                            child_asset.everyone_permissions & item->everyone_permissions;
                        child->next_owner_permissions =
                            child_asset.next_owner_permissions & item->next_permissions;
                        child->creation_date = entity->creation_date;
                        child->physical = false;
                        // The child's parent is the root prim, not the avatar:
                        // only the root carries the point, or a viewer would
                        // draw every prim on the joint.
                        child->parent_id = root_id;
                        child->local_position = child_asset.local_position;
                        child->local_rotation = child_asset.local_rotation;
                        homeworldz::scene::update_linked_world_transform(*child, *entity);
                    }
                    outcome.worn = true;
                    // The worn object stands in this region, so its closure must
                    // be servable from here (ADR 0026).
                    materialize_asset_closure({{item->asset_id, 6}}, "attach");
                }
            }
        } catch (const std::exception& error) {
            for (auto entity = entity_ids.rbegin(); entity != entity_ids.rend(); ++entity)
                scene.remove(*entity);
            entity_ids.clear();
            outcome.worn = false;
            outcome.refused = error.what();
        }
        outcome.prims = entity_ids.size();
        if (outcome.worn) {
            for (const auto entity_id : entity_ids)
                if (const auto* entity = scene.find(entity_id))
                    broadcast_object_update(*entity, when);
            outcome.recorded = !record || (viewer_grid && viewer_grid->set_attachment_worn(
                user_id, item_id, outcome.point, true));
        }
        return outcome;
    };
    // An arriving avatar rezzes back what the grid says it is wearing. This is
    // the whole point of recording it: attachments are region-local, and the
    // region someone returns to is usually not the one they left.
    //
    // A grid that cannot answer leaves the avatar as it is. An empty wardrobe
    // and an unanswered question look identical afterwards, and only one of
    // them is a reason to arrive wearing nothing.
    const auto restore_attachments = [&](const std::string& user_id,
                                         homeworldz::scene::EntityId wearer_id,
                                         std::chrono::steady_clock::time_point when) {
        if (!viewer_grid) return;
        const auto worn = viewer_grid->worn_attachments(user_id);
        if (!worn) {
            std::cout << "{\"level\":\"warn\",\"message\":\"worn attachments unavailable\",\"userId\":"
                      << homeworldz::api::json_string(user_id) << "}" << std::endl;
            return;
        }
        if (worn->empty()) return;
        std::size_t restored = 0;
        std::size_t inconclusive = 0;
        for (const auto& item : *worn) {
            // keep_others: the grid's list may legitimately hold two items on
            // one point, and rezzing the second must not evict the first.
            // record=false: this list is the record.
            const auto outcome = wear_attachment(user_id, wearer_id, item.item_id,
                                                 item.attachment_point, true, false, when);
            if (outcome.worn) {
                ++restored;
                continue;
            }
            if (outcome.inconclusive) ++inconclusive;
            // "Could not be run" is not "refused". The distinction is the whole
            // value of saying anything here: a wearer whose grid hiccuped has
            // not lost the item, and the log must not read as though they had.
            std::cout << "{\"level\":\"warn\",\"message\":"
                      << (outcome.inconclusive
                              ? "\"worn attachment could not be checked\""
                              : "\"worn attachment not restored\"")
                      << ",\"userId\":"
                      << homeworldz::api::json_string(user_id) << ",\"itemId\":"
                      << homeworldz::api::json_string(item.item_id) << ",\"attachmentPoint\":"
                      << static_cast<unsigned>(item.attachment_point) << ",\"reason\":"
                      << homeworldz::api::json_string(outcome.refused) << "}" << std::endl;
        }
        std::cout << "{\"level\":" << (restored == worn->size() ? "\"info\"" : "\"warn\"")
                  << ",\"message\":\"worn attachments restored\",\"userId\":"
                  << homeworldz::api::json_string(user_id) << ",\"restored\":" << restored
                  << ",\"inconclusive\":" << inconclusive
                  << ",\"worn\":" << worn->size() << "}" << std::endl;
    };
    // Restore enabled task scripts after a Region restart. VM state is not yet
    // persisted, so each restored script starts fresh and re-runs state_entry;
    // this is enough to re-establish SCRIPTED / HANDLE_TOUCH advertising and live
    // event handling so touch works after a restart. Persisting and resuming VM
    // state across restarts is separate future work.
    {
        std::size_t restored_scripts = 0;
        for (const auto& [entity_id, entity] : scene.entities()) {
            static_cast<void>(entity_id);
            for (const auto& item : entity.task_inventory) {
                if (item.asset_type != 10) continue; // LSL script asset type
                const auto result = rez_task_script(entity, item, true);
                if (result.compiled) {
                    ++restored_scripts;
                } else {
                    std::cerr << "{\"level\":\"warning\",\"message\":\"task script restore failed\",\"objectId\":"
                              << homeworldz::api::json_string(entity.object_id)
                              << ",\"itemId\":" << homeworldz::api::json_string(item.item_id)
                              << ",\"diagnostic\":"
                              << homeworldz::api::json_string(result.diagnostic) << "}"
                              << std::endl;
                }
            }
        }
        if (restored_scripts != 0)
            std::cout << "{\"level\":\"info\",\"message\":\"task scripts restored\",\"count\":"
                      << restored_scripts << "}" << std::endl;
    }
    const auto clear_viewer_endpoint = [&](const std::string& endpoint, const std::string& session_id) {
        // Tell the remaining viewers to remove this avatar's rezzed
        // representation. Every avatar-removal path (logout, disconnect/timeout,
        // duplicate-login replacement, and teleport/crossing source-retirement
        // via departed_avatars) funnels through here, and it runs only once the
        // avatar has actually left this region — so it is safe for crossings and
        // teleports: a rolled-back crossing never reaches this point, and the
        // destination region independently rezzes the avatar for viewers there.
        if (const auto departing = avatars.find(endpoint); departing != avatars.end()) {
            const std::array<std::uint32_t, 1> kill_ids{
                static_cast<std::uint32_t>(departing->second.entity_id)};
            const auto kill = homeworldz::viewer::encode_kill_object(kill_ids);
            const auto kill_now = std::chrono::steady_clock::now();
            // What this avatar was wearing goes with it.
            static_cast<void>(remove_avatar_attachments(departing->second.entity_id, kill_now));
            std::size_t kill_recipients = 0;
            for (const auto& [recipient_endpoint, recipient] : avatars) {
                static_cast<void>(recipient);
                if (recipient_endpoint == endpoint) continue;
                if (const auto outgoing = circuits.send(
                        recipient_endpoint, kill, true, kill_now, true)) {
                    static_cast<void>(send_udp(viewer_server, recipient_endpoint, *outgoing));
                    ++kill_recipients;
                }
            }
            std::cout << "{\"level\":\"info\",\"message\":\"avatar departure kill broadcast\","
                         "\"localId\":" << kill_ids[0] << ",\"recipients\":" << kill_recipients
                      << "}" << std::endl;
            deliver_to_embodied(session_kill_envelope(departing->second.entity_id));
        }
        if (const auto live = avatars.find(endpoint); live != avatars.end() &&
            physics_world && live->second.physics_character != 0)
            physics_world->remove_character(live->second.physics_character);
        if (const auto selected = physics_edit_selections.find(endpoint);
            selected != physics_edit_selections.end()) {
            for (const auto& [selected_id, root_id] : selected->second) {
                static_cast<void>(selected_id);
                const auto suspended = physics_edit_suspended.find(root_id);
                if (suspended == physics_edit_suspended.end()) continue;
                if (--suspended->second == 0) {
                    physics_edit_suspended.erase(suspended);
                    if (const auto* entity = scene.find(root_id)) synchronize_physics_object(*entity);
                }
            }
            physics_edit_selections.erase(selected);
        }
        avatars.erase(endpoint);
        avatar_geometries.erase(endpoint);
        avatar_appearances.erase(endpoint);
        avatar_animations.erase(endpoint);
        next_animation_sequences.erase(endpoint);
        movement_animations.erase(endpoint);
        handshake_replies.erase(endpoint);
        established_events.erase(session_id);
        parcel_overlay_sent.erase(session_id);
        queued_viewer_events.erase(session_id);
        capability_arrival_gate.clear_session(session_id);
        std::erase_if(pending_agent_movement_completes,
            [&](const PendingAgentMovementComplete& pending) {
                return pending.endpoint == endpoint;
            });
        texture_packets.erase(endpoint);
        std::erase_if(active_texture_transfers, [&](const std::string& key) {
            return key.starts_with(endpoint + '|');
        });
        std::erase_if(pending_inventory_asset_uploads, [&](const auto& entry) {
            return entry.first.starts_with(endpoint + '|');
        });
        std::erase_if(pending_inventory_asset_xfers, [&](const auto& entry) {
            return entry.first.starts_with(endpoint + '|');
        });
        std::erase_if(pending_inventory_asset_updates, [&](const auto& entry) {
            return entry.second.session_id == session_id;
        });
        std::erase_if(pending_task_inventory_files, [&](const auto& entry) {
            return entry.first.starts_with(endpoint + '|');
        });
        std::erase_if(pending_task_inventory_xfers, [&](const auto& entry) {
            return entry.first.starts_with(endpoint + '|');
        });
        sent_dynamic_transforms.erase(endpoint);
        std::erase_if(pending_event_responses, [&](const PendingEventResponse& pending) {
            if (pending.session_id != session_id) return false;
            close_socket(pending.client);
            return true;
        });
    };

    const auto take_viewer_events = [&](const std::string& session_id) {
        std::vector<std::string> events;
        const auto queued = queued_viewer_events.find(session_id);
        if (queued == queued_viewer_events.end()) return events;
        events.assign(std::make_move_iterator(queued->second.begin()),
                      std::make_move_iterator(queued->second.end()));
        queued_viewer_events.erase(queued);
        return events;
    };
    const auto flush_pending_viewer_events = [&](const std::string& session_id) {
        const auto queued = queued_viewer_events.find(session_id);
        if (queued == queued_viewer_events.end() || queued->second.empty()) return;
        const auto pending = std::find_if(pending_event_responses.begin(), pending_event_responses.end(),
            [&](const PendingEventResponse& candidate) { return candidate.session_id == session_id; });
        if (pending == pending_event_responses.end()) return;
        const auto events = take_viewer_events(session_id);
        const auto response = homeworldz::http::response_for_content(
            pending->request, 200, "application/llsd+xml",
            homeworldz::viewer::event_queue_xml(++event_id, events));
        static_cast<void>(send_all(pending->client, response.content));
        finish_http_response(pending->client);
        close_socket(pending->client);
        std::cout << "{\"level\":\"info\",\"message\":\"http request\",\"requestId\":"
                  << homeworldz::api::json_string(response.request_id)
                  << ",\"method\":" << homeworldz::api::json_string(response.method)
                  << ",\"path\":" << homeworldz::api::json_string(response.path)
                  << ",\"status\":" << response.status_code << "}" << std::endl;
        pending_event_responses.erase(pending);
    };
    const auto enqueue_viewer_event = [&](const std::string& session_id, std::string event) {
        queued_viewer_events[session_id].push_back(std::move(event));
        flush_pending_viewer_events(session_id);
    };

    // Estate owner/manager check: the estate owner and any listed manager (and the
    // provisioned region owner) bypass parcel and estate restrictions.
    const auto is_estate_manager = [&](std::string_view agent) {
        if (agent.empty()) return false;
        if (agent == region_owner_id) return true;
        if (region_estate) {
            if (agent == region_estate->owner_id) return true;
            for (const auto& manager : region_estate->managers)
                if (manager == agent) return true;
        }
        return false;
    };
    // Is `agent` banned from the estate, or barred by a private estate's access
    // lists? Estate owner/managers are always admitted.
    const auto estate_denies_entry = [&](std::string_view agent) {
        if (!region_estate || is_estate_manager(agent)) return false;
        for (const auto& banned : region_estate->bans)
            if (banned == agent) return true;
        if (region_estate->public_access) return false;
        for (const auto& allowed : region_estate->allowed_users)
            if (allowed == agent) return false;
        return true; // private estate, agent not on the allowed list (groups TBD)
    };
    // Estate/region flags for RegionHandshake and RegionInfo (indra RegionFlags).
    const auto region_flags = [&]() -> std::uint32_t {
        constexpr std::uint32_t allow_landmark = 1U << 1;
        constexpr std::uint32_t allow_set_home = 1U << 2;
        std::uint32_t flags = allow_landmark | allow_set_home;
        if (region_estate) {
            // estatechangeinfo param1 bit layout of the stored estate flags.
            constexpr std::uint32_t p_allow_dtp = 0x00100000, p_deny_anon = 0x00800000,
                p_deny_ident = 0x01000000, p_deny_trans = 0x02000000, p_allow_voice = 0x10000000,
                p_deny_minors = 0x40000000;
            const auto estate = region_estate->flags;
            if (region_estate->fixed_sun) flags |= 1U << 4;       // SunFixed
            if (estate & p_allow_dtp) flags |= 1U << 20;          // AllowDirectTeleport
            if (estate & p_deny_anon) flags |= 1U << 23;          // DenyAnonymous
            if (estate & p_deny_ident) flags |= 1U << 24;         // DenyIdentified
            if (estate & p_deny_trans) flags |= 1U << 25;         // DenyTransacted
            if (estate & p_allow_voice) flags |= 1U << 28;        // AllowVoice
            if (estate & p_deny_minors) flags |= 1U << 30;        // DenyAgeUnverified
        }
        return flags;
    };
    // Assigned here, where the estate helpers it needs are in scope. Declared
    // earlier so the terrain handlers can reach it.
    send_region_handshake = [&](const std::string& endpoint,
                                const homeworldz::viewer::Uuid& agent_id) {
        const auto region_id = registration ?
            homeworldz::viewer::parse_uuid(registration->region_id()) : std::nullopt;
        if (!region_id) return false;
        homeworldz::viewer::RegionHandshake handshake;
        handshake.name = region_name;
        handshake.region_id = *region_id;
        // The estate/region owner is authoritative from the grid record; fall back
        // to this agent only when the grid supplied no owner (older records).
        const auto estate_owner_id = region_estate && !region_estate->owner_id.empty()
            ? region_estate->owner_id : region_owner_id;
        const auto region_owner = homeworldz::viewer::parse_uuid(estate_owner_id);
        handshake.owner_id = region_owner ? *region_owner : agent_id;
        handshake.is_estate_owner =
            is_estate_manager(homeworldz::viewer::format_uuid(agent_id));
        handshake.region_flags = region_flags();
        handshake.water_height = static_cast<float>(region_settings.water_height);
        // One definition, shared with the session hello, so a viewer and a client
        // are told the same ids (homeworldz/terrain_layers.h).
        for (std::size_t index = 0; index < terrain_layers.assets.size(); ++index)
            if (const auto texture =
                    homeworldz::viewer::parse_uuid(terrain_layers.assets[index]))
                handshake.terrain_textures[index] = *texture;
        handshake.terrain_start = terrain_layers.start;
        handshake.terrain_range = terrain_layers.range;
        const auto response = circuits.send(endpoint,
            homeworldz::viewer::encode_region_handshake(handshake), true, std::chrono::steady_clock::now(), true);
        if (!response) {
            std::cout << "{\"level\":\"error\",\"message\":\"region handshake not queued\","
                         "\"endpoint\":" << homeworldz::api::json_string(endpoint) << "}"
                      << std::endl;
            return false;
        }
        const auto sent = send_udp(viewer_server, endpoint, *response);
        std::cout << "{\"level\":" << (sent ? "\"info\"" : "\"error\"")
                  << ",\"message\":\"region handshake sent\",\"endpoint\":"
                  << homeworldz::api::json_string(endpoint)
                  << ",\"bytes\":" << response->size()
                  << ",\"success\":" << (sent ? "true" : "false") << "}" << std::endl;
        return sent;
    };
    // Estate flags in RegionFlags form for the estateupdateinfo reply (floater UI).
    const auto estate_detail_flags = [&]() -> std::uint32_t {
        std::uint32_t flags = 0;
        if (region_estate) {
            const auto estate = region_estate->flags;
            if (region_estate->fixed_sun) flags |= 1U << 4;                 // SunFixed
            if (region_estate->public_access) flags |= (1U << 17) | (1U << 15); // PublicAllowed|ExternallyVisible
            if (estate & homeworldz::viewer::estate_flag_allow_direct_teleport) flags |= 1U << 20;
            if (estate & homeworldz::viewer::estate_flag_deny_anonymous) flags |= 1U << 23;
            if (estate & homeworldz::viewer::estate_flag_deny_identified) flags |= 1U << 24;
            if (estate & homeworldz::viewer::estate_flag_deny_transacted) flags |= 1U << 25;
            if (estate & homeworldz::viewer::estate_flag_allow_voice) flags |= 1U << 28;
            if (estate & homeworldz::viewer::estate_flag_deny_minors) flags |= 1U << 30;
        }
        return flags;
    };
    // One setaccess EstateOwnerMessage carrying a single access list as raw 16-byte
    // UUID params (indra sends estate list UUIDs in binary form).
    const auto send_estate_list = [&](const std::string& viewer_endpoint,
                                      const homeworldz::viewer::Uuid& agent,
                                      std::uint32_t list_bit,
                                      const std::vector<std::string>& members,
                                      std::chrono::steady_clock::time_point when) {
        const std::uint32_t estate_id = region_estate ? static_cast<std::uint32_t>(region_estate->id) : 0;
        std::vector<std::string> params(6);
        params[0] = std::to_string(estate_id);
        params[1] = std::to_string(list_bit);
        params[2] = std::to_string(list_bit == homeworldz::viewer::estate_list_allowed_agents ? members.size() : 0);
        params[3] = std::to_string(list_bit == homeworldz::viewer::estate_list_allowed_groups ? members.size() : 0);
        params[4] = std::to_string(list_bit == homeworldz::viewer::estate_list_banned_agents ? members.size() : 0);
        params[5] = std::to_string(list_bit == homeworldz::viewer::estate_list_managers ? members.size() : 0);
        for (const auto& member : members) {
            if (const auto id = homeworldz::viewer::parse_uuid(member))
                params.emplace_back(reinterpret_cast<const char*>(id->data()), id->size());
        }
        const homeworldz::viewer::Uuid invoice{};
        auto message = homeworldz::viewer::encode_estate_owner_message(agent, invoice, "setaccess", params);
        if (const auto outgoing = circuits.send(viewer_endpoint, std::move(message), true, when, true))
            static_cast<void>(send_udp(viewer_server, viewer_endpoint, *outgoing));
    };
    // Send the estateupdateinfo reply plus the four access lists (getinfo response).
    const auto send_estate_detail = [&](const std::string& viewer_endpoint,
                                        const homeworldz::viewer::Uuid& agent,
                                        const homeworldz::viewer::Uuid& invoice,
                                        std::chrono::steady_clock::time_point when) {
        if (!region_estate) return;
        std::vector<std::string> params(10);
        params[0] = region_estate->name.empty() ? region_name : region_estate->name;
        params[1] = region_estate->owner_id.empty() ? region_owner_id : region_estate->owner_id;
        params[2] = std::to_string(region_estate->id);
        params[3] = std::to_string(estate_detail_flags());
        params[4] = std::to_string(region_estate->use_global_time ? 0 :
            static_cast<std::uint32_t>(region_estate->sun_hour * 1024.0 + 0x1800));
        params[5] = std::to_string(region_estate->parent_estate_id);
        params[6] = "00000000-0000-0000-0000-000000000000"; // covenant
        params[7] = "0";                                     // covenant timestamp
        params[8] = "1";
        params[9] = region_estate->abuse_email;
        auto message = homeworldz::viewer::encode_estate_owner_message(
            agent, invoice, "estateupdateinfo", params);
        if (const auto outgoing = circuits.send(viewer_endpoint, std::move(message), true, when, true))
            static_cast<void>(send_udp(viewer_server, viewer_endpoint, *outgoing));
        send_estate_list(viewer_endpoint, agent, homeworldz::viewer::estate_list_managers,
                         region_estate->managers, when);
        send_estate_list(viewer_endpoint, agent, homeworldz::viewer::estate_list_allowed_agents,
                         region_estate->allowed_users, when);
        send_estate_list(viewer_endpoint, agent, homeworldz::viewer::estate_list_allowed_groups,
                         region_estate->allowed_groups, when);
        send_estate_list(viewer_endpoint, agent, homeworldz::viewer::estate_list_banned_agents,
                         region_estate->bans, when);
    };
    // Region-wide simulator prim capacity, scaled by the number of 256 m tiles.
    const auto region_prim_limit = [&]() {
        const long long tiles = static_cast<long long>(region_size_x / 256) *
                                static_cast<long long>(region_size_y / 256);
        return static_cast<std::int32_t>(15000LL * (std::max)(1LL, tiles));
    };
    const auto persist_parcels = [&]() {
        try {
            storage->save_parcels(parcels->parcels());
        } catch (const std::exception& error) {
            std::cout << "{\"level\":\"error\",\"message\":\"parcel persist failed\",\"error\":"
                      << homeworldz::api::json_string(error.what()) << "}" << std::endl;
        }
    };
    // Build and enqueue a ParcelProperties event for one parcel over the Event Queue.
    const auto send_parcel_properties = [&](const std::string& session_id,
                                            const homeworldz::parcel::Parcel& parcel,
                                            std::int32_t request_result, std::int32_t sequence_id,
                                            bool snap_selection) {
        const int edge = parcels->edge_cells();
        homeworldz::viewer::ParcelPropertiesEvent event;
        event.request_result = request_result;
        event.sequence_id = sequence_id;
        event.snap_selection = snap_selection;
        event.local_id = parcel.local_id;
        event.owner_id = parcel.owner_id;
        event.is_group_owned = parcel.is_group_owned;
        event.claim_date = parcel.claim_date;
        event.bitmap = parcel.bitmap;
        event.area = parcel.area(edge);
        event.status = 0; // Leased
        event.parcel_flags = parcel.flags;
        event.sale_price = parcel.sale_price;
        event.name = parcel.name;
        event.description = parcel.description;
        event.music_url = parcel.music_url;
        event.media_url = parcel.media_url;
        event.media_id = parcel.media_id;
        event.media_auto_scale = parcel.media_auto_scale;
        event.group_id = parcel.group_id;
        event.pass_price = parcel.pass_price;
        event.pass_hours = parcel.pass_hours;
        event.category = static_cast<std::uint8_t>(parcel.category);
        event.auth_buyer_id = parcel.auth_buyer_id;
        event.snapshot_id = parcel.snapshot_id;
        event.user_location = {parcel.user_location.x, parcel.user_location.y, parcel.user_location.z};
        event.user_look_at = {parcel.user_look_at.x, parcel.user_look_at.y, parcel.user_look_at.z};
        event.landing_type = parcel.landing_type;
        event.other_clean_time = parcel.other_clean_time;
        event.parcel_prim_bonus = 1.0F;
        if (region_estate) {
            const auto estate = region_estate->flags;
            event.region_deny_anonymous = (estate & 0x00800000) != 0;
            event.region_deny_identified = (estate & 0x01000000) != 0;
            event.region_deny_transacted = (estate & 0x02000000) != 0;
            event.region_deny_age_unverified = (estate & 0x40000000) != 0;
        }
        int min_x = 0, min_y = 0, max_x = 0, max_y = 0;
        if (parcel.cell_bounds(edge, min_x, min_y, max_x, max_y)) {
            // A parcel AABB spans the full buildable Z range so the viewer's
            // "are you standing inside this parcel" test (which considers Z) passes
            // for an avatar at terrain height.
            event.aabb_min = {static_cast<float>(min_x * 4), static_cast<float>(min_y * 4), 0.0F};
            event.aabb_max = {static_cast<float>(max_x * 4), static_cast<float>(max_y * 4), 4096.0F};
        }
        std::int32_t owner_prims = 0, group_prims = 0, other_prims = 0, region_prims = 0;
        for (const auto& [entity_id, entity] : scene.entities()) {
            if (entity.object_id.empty() || entity.temporary) continue;
            ++region_prims;
            const auto* here = parcels->parcel_at(static_cast<float>(entity.position.x),
                                                  static_cast<float>(entity.position.y));
            if (here == nullptr || here->local_id != parcel.local_id) continue;
            if (entity.owner_id == parcel.owner_id) ++owner_prims;
            else if (!parcel.group_id.empty() && entity.owner_id == parcel.group_id) ++group_prims;
            else ++other_prims;
        }
        event.owner_prims = owner_prims;
        event.group_prims = group_prims;
        event.other_prims = other_prims;
        event.total_prims = owner_prims + group_prims + other_prims;
        event.sim_wide_total_prims = region_prims;
        event.sim_wide_max_prims = region_prim_limit();
        const long long region_area =
            static_cast<long long>(region_size_x) * static_cast<long long>(region_size_y);
        event.max_prims = region_area > 0 ? static_cast<std::int32_t>(
            static_cast<long long>(event.area) * region_prim_limit() / region_area) : 0;
        enqueue_viewer_event(session_id, homeworldz::viewer::parcel_properties_event_xml(event));
    };
    // Tell a viewer which parcel its avatar is standing in (the "agent parcel"),
    // using a positive incrementing sequence. The viewer needs this to allow
    // parcel actions such as setting the landing point; it is re-sent when the
    // avatar crosses into a different parcel.
    const auto push_agent_parcel = [&](LiveAvatar& avatar) {
        if (!parcels || avatar.session_id.empty()) return;
        const auto& position = avatar.controller.state().position;
        const auto* parcel = parcels->parcel_at(
            static_cast<float>(position.x), static_cast<float>(position.y));
        if (parcel == nullptr || parcel->local_id == avatar.last_agent_parcel) return;
        send_parcel_properties(avatar.session_id, *parcel, homeworldz::parcel::result_single,
            static_cast<std::int32_t>(++agent_parcel_sequence), false);
        avatar.last_agent_parcel = parcel->local_id;
    };
    // Send ParcelOverlay (coloured parcel boundaries) to one viewer over UDP.
    const auto send_parcel_overlay = [&](const std::string& viewer_endpoint,
                                         const std::string& agent_id,
                                         std::chrono::steady_clock::time_point when) {
        const auto cells = parcels->overlay_for(agent_id, region_owner_id);
        for (auto& packet : homeworldz::viewer::encode_parcel_overlay(cells))
            if (const auto outgoing = circuits.send(viewer_endpoint, std::move(packet), true, when, true))
                static_cast<void>(send_udp(viewer_server, viewer_endpoint, *outgoing));
    };
    // Refresh parcel boundaries for every connected viewer after a land change.
    const auto broadcast_parcel_overlay = [&](std::chrono::steady_clock::time_point when) {
        for (const auto& [recipient_endpoint, recipient] : avatars)
            send_parcel_overlay(recipient_endpoint, recipient.user_id, when);
    };
    // Return one root object (and its linkset) to its owner's inventory: serialize
    // the linkset, store and register the asset, create an object item in the
    // owner's Lost and Found (or Objects) folder, and remove the parts from the
    // scene. Appends the removed part local ids for KillObject broadcasting. Used
    // by parcel object return and OtherCleanTime auto-return; the owner is not
    // necessarily the requester and may be offline.
    const auto return_object_to_owner = [&](homeworldz::scene::EntityId root_id,
                                            std::vector<std::uint32_t>& removed_ids,
                                            std::chrono::steady_clock::time_point when) {
        if (!viewer_grid) return;
        auto* entity = scene.find(root_id);
        if (entity == nullptr || entity->object_id.empty() || entity->parent_id != 0) return;
        const auto owner_id = entity->owner_id;
        if (owner_id.empty()) return;
        std::vector<const homeworldz::scene::Entity*> children;
        std::vector<homeworldz::scene::EntityId> part_ids{root_id};
        for (const auto& [child_id, child] : scene.entities()) {
            if (child.parent_id != root_id) continue;
            children.push_back(&child);
            part_ids.push_back(child_id);
        }
        const auto folded = homeworldz::scene::effective_permissions(scene, *entity);
        auto base_permissions = entity->base_permissions;
        auto owner_permissions = folded.owner;
        auto everyone_permissions = entity->everyone_permissions;
        const auto next_owner_permissions = folded.next_owner;
        for (const auto* child : children) {
            base_permissions &= child->base_permissions;
            everyone_permissions &= child->everyone_permissions;
        }
        everyone_permissions &= owner_permissions;
        std::string folder;
        if (const auto lost = viewer_grid->find_system_inventory_folder(owner_id, 16))
            folder = *lost;
        else if (const auto objects = viewer_grid->find_system_inventory_folder(owner_id, 6))
            folder = *objects;
        if (folder.empty()) return;
        const auto asset_id = homeworldz::viewer::random_uuid();
        const auto item_id = homeworldz::viewer::random_uuid();
        const auto content_text = homeworldz::asset::serialize_linkset_asset(*entity, children);
        const auto content = std::span(
            reinterpret_cast<const std::byte*>(content_text.data()), content_text.size());
        bool item_created = false;
        try {
            const auto metadata = storage->store_asset(asset_id, entity->creator_id, content);
            const bool asset_registered = viewer_grid->register_asset(
                metadata.viewer_id, metadata.creator_id, metadata.sha256, metadata.size,
                region_public_endpoint, true) &&
                // Write-through before the commit: the durability fetch-back
                // and this region's single thread cannot meet (ADR 0026).
                viewer_grid->store_vault_asset(metadata.viewer_id, content);
            item_created = asset_registered && viewer_grid->create_object_inventory_item(
                owner_id, homeworldz::grid::ObjectInventoryItem{
                    item_id, entity->creator_id, folder, asset_id, entity->name,
                    entity->description, base_permissions, owner_permissions,
                    everyone_permissions, next_owner_permissions});
        } catch (const std::exception& error) {
            std::cout << "{\"level\":\"error\",\"message\":\"parcel return inventory failed\",\"error\":"
                      << homeworldz::api::json_string(error.what()) << "}" << std::endl;
        }
        if (!item_created) return;
        // Capture the fields needed for the viewer notice before the entity is removed.
        const std::string item_name = entity->name;
        const std::string item_description = entity->description;
        const std::string creator_id = entity->creator_id;
        for (auto part = part_ids.rbegin(); part != part_ids.rend(); ++part)
            if (scene.remove(*part)) removed_ids.push_back(static_cast<std::uint32_t>(*part));
        // If the owner is connected, tell their viewer about the new item so it
        // appears in inventory without a relog (About Land return previously only
        // showed up after re-fetching Lost and Found).
        for (const auto& [owner_endpoint, owner_avatar] : avatars) {
            if (owner_avatar.user_id != owner_id) continue;
            const auto owner_agent = homeworldz::viewer::parse_uuid(owner_id);
            const auto owner_session = homeworldz::viewer::parse_uuid(owner_avatar.session_id);
            if (!owner_agent || !owner_session) break;
            homeworldz::viewer::InventoryItem item;
            if (const auto value = homeworldz::viewer::parse_uuid(item_id)) item.item_id = *value;
            if (const auto value = homeworldz::viewer::parse_uuid(creator_id)) item.creator_id = *value;
            item.owner_id = *owner_agent;
            if (const auto value = homeworldz::viewer::parse_uuid(folder)) item.folder_id = *value;
            if (const auto value = homeworldz::viewer::parse_uuid(asset_id)) item.asset_id = *value;
            item.asset_type = 6;
            item.inventory_type = 6;
            item.name = item_name;
            item.description = item_description;
            item.base_permissions = base_permissions;
            item.current_permissions = owner_permissions;
            item.everyone_permissions = everyone_permissions;
            item.next_permissions = next_owner_permissions;
            item.creation_date = static_cast<std::int32_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            const homeworldz::viewer::AgentMessage reply{*owner_agent, *owner_session};
            auto update = homeworldz::viewer::encode_update_create_inventory_item(reply, 0, item);
            if (const auto outgoing = circuits.send(owner_endpoint, std::move(update), true, when, true))
                static_cast<void>(send_udp(viewer_server, owner_endpoint, *outgoing));
            break;
        }
    };

    // HTTP connections whose request has not fully arrived. They are read a
    // little at a time across loop iterations rather than waited on, because
    // this loop also renews the region lease and services viewer UDP: a read
    // that blocks here stops the region, and on 2026-08-07 one did for twenty
    // minutes, dropping Welcome's lease and taking chat and asset transfer down
    // with it. A per-recv deadline bounded that but did not fix it — the loop
    // accepted one connection per iteration, so a queue of stalled peers still
    // stalled everything behind them for as long as the sum of their deadlines.
    struct IncomingHttp {
        socket_handle client;
        std::string buffer;
        std::chrono::steady_clock::time_point deadline;
    };
    std::vector<IncomingHttp> incoming_http;
    std::cout << "{\"level\":\"info\",\"message\":\"region service listening\",\"httpPort\":"
              << configured_port() << ",\"viewerPort\":" << region_viewer_port << "}" << std::endl;
    while (running) {
        const auto http_now = std::chrono::steady_clock::now();
        std::erase_if(pending_event_responses, [&](const PendingEventResponse& pending) {
            if (pending.deadline > http_now) return false;
            const auto response = homeworldz::http::response_for_content(
                pending.request, 200, "application/llsd+xml",
                homeworldz::viewer::event_queue_xml(++event_id));
            static_cast<void>(send_all(pending.client, response.content));
            finish_http_response(pending.client);
            close_socket(pending.client);
            std::cout << "{\"level\":\"info\",\"message\":\"http request\",\"requestId\":"
                      << homeworldz::api::json_string(response.request_id)
                      << ",\"method\":" << homeworldz::api::json_string(response.method)
                      << ",\"path\":" << homeworldz::api::json_string(response.path)
                      << ",\"status\":" << response.status_code << "}" << std::endl;
            return true;
        });
        std::erase_if(pending_agent_movement_completes,
            [&](const PendingAgentMovementComplete& pending) {
                const bool seed_served = capability_arrival_gate.consume_seed(
                    pending.session_id, pending.visit_id);
                if (!seed_served && pending.deadline > http_now) return false;
                if (const auto outgoing = circuits.send(
                        pending.endpoint, pending.payload, true, http_now))
                    static_cast<void>(send_udp(viewer_server, pending.endpoint, *outgoing));
                if (!seed_served) {
                    std::cout << "{\"level\":\"warning\",\"message\":\"agent movement capability gate timed out\","
                                 "\"sessionId\":"
                              << homeworldz::api::json_string(pending.session_id)
                              << ",\"visitId\":"
                              << homeworldz::api::json_string(pending.visit_id) << "}" << std::endl;
                }
                return true;
            });
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(server, &readable);
        FD_SET(viewer_server, &readable);
        auto highest = server > viewer_server ? server : viewer_server;
        for (const auto& pending : incoming_http) {
            FD_SET(pending.client, &readable);
            if (pending.client > highest) highest = pending.client;
        }
        timeval timeout{0, 10000};
        const auto ready = select(static_cast<int>(highest) + 1, &readable, nullptr, nullptr, &timeout);
        // Drain the whole accept backlog, not one per iteration. A queue that is
        // only ever shortened by one connection per pass is how seventeen
        // stalled peers held the listen queue full while the lease expired.
        if (ready > 0 && FD_ISSET(server, &readable)) {
            for (;;) {
                const auto accepted = accept(server, nullptr, nullptr);
                if (accepted == invalid_socket) break;
                set_socket_deadline(accepted, http_client_timeout_ms);
                set_socket_blocking_mode(accepted, false);
                incoming_http.push_back({accepted, std::string{},
                    http_now + std::chrono::milliseconds(http_client_timeout_ms)});
                if (incoming_http.size() >= maximum_incoming_http) break;
            }
        }
        // One non-blocking read per waiting connection, then hand on whichever
        // completed. A peer that says nothing simply never completes and is
        // dropped at its deadline, costing the loop nothing meanwhile.
        socket_handle ready_client = invalid_socket;
        std::optional<std::string> ready_request;
        for (auto entry = incoming_http.begin(); entry != incoming_http.end();) {
            bool finished = false;
            if (FD_ISSET(entry->client, &readable)) {
                std::array<char, 4096> chunk{};
                for (;;) {
                    const auto received =
                        recv(entry->client, chunk.data(), static_cast<int>(chunk.size()), 0);
                    if (received > 0) {
                        entry->buffer.append(chunk.data(), static_cast<std::size_t>(received));
                        const auto state = http_request_state(entry->buffer);
                        if (state == HttpRequestState::complete) {
                            ready_client = entry->client;
                            ready_request = std::move(entry->buffer);
                            finished = true;
                            break;
                        }
                        if (state == HttpRequestState::invalid) {
                            close_socket(entry->client);
                            finished = true;
                            break;
                        }
                        continue;
                    }
                    if (received == 0 || !socket_would_block()) {
                        // Peer closed, or the read failed outright. Closing here
                        // is what keeps these off the CLOSE-WAIT pile that made
                        // the wedge look like a socket leak.
                        close_socket(entry->client);
                        finished = true;
                    }
                    break;
                }
            }
            if (!finished && http_now >= entry->deadline) {
                close_socket(entry->client);
                finished = true;
            }
            if (finished) {
                const bool dispatching = ready_client == entry->client;
                entry = incoming_http.erase(entry);
                if (dispatching) break;
            } else {
                ++entry;
            }
        }
        if (ready_client != invalid_socket) {
            const auto client = ready_client;
            // Reads are done; the response goes to a peer already waiting for
            // it, so the socket returns to blocking and send_all keeps its
            // existing behaviour, bounded by the send deadline set above.
            set_socket_blocking_mode(client, true);
            {
                bool response_deferred = false;
                const auto received_request = std::move(ready_request);
                if (received_request) {
                    const std::string_view request(*received_request);
                    auto response = homeworldz::http::response_for(request, region_version);
                    if (response.path == "/map/terrain.raw") {
                        const auto authorization =
                            homeworldz::http::request_header_value(request, "Authorization");
                        if (response.method != "GET") {
                            response = homeworldz::http::response_for_content(
                                request, 405, "application/json",
                                homeworldz::api::to_json(homeworldz::api::Error{
                                    "method_not_allowed", "terrain map endpoint requires GET"}));
                        } else if (service_token.empty() || authorization != "Bearer " + service_token) {
                            response = homeworldz::http::response_for_content(
                                request, 401, "application/json",
                                homeworldz::api::to_json(homeworldz::api::Error{
                                    "unauthorized", "a valid grid service token is required"}));
                        } else {
                            response = homeworldz::http::response_for_content(
                                request, 200, "application/vnd.homeworldz.heightmap-f32le",
                                encode_heightmap(*terrain_heightmap));
                        }
                    }
                    if (response.path == "/map/terrain-layers.json") {
                        // The companion to terrain.raw. A heightmap alone cannot
                        // be coloured: which of the four ground textures applies
                        // at a height is decided by this region's own start and
                        // range, which an operator may have changed through the
                        // Terrain tab, and the water line decides what is sea at
                        // all. Without them the grid's map tiles fall back to a
                        // fixed height palette and a region reads as one flat
                        // colour that matches nothing a viewer draws.
                        //
                        // Corners are south-west, north-west, south-east,
                        // north-east — the order the handshake writes them, and
                        // the order a bilinear read expects.
                        const auto authorization =
                            homeworldz::http::request_header_value(request, "Authorization");
                        if (response.method != "GET") {
                            response = homeworldz::http::response_for_content(
                                request, 405, "application/json",
                                homeworldz::api::to_json(homeworldz::api::Error{
                                    "method_not_allowed", "terrain layer endpoint requires GET"}));
                        } else if (service_token.empty() || authorization != "Bearer " + service_token) {
                            response = homeworldz::http::response_for_content(
                                request, 401, "application/json",
                                homeworldz::api::to_json(homeworldz::api::Error{
                                    "unauthorized", "a valid grid service token is required"}));
                        } else {
                            std::string assets, starts, ranges;
                            for (std::size_t index = 0; index < 4; ++index) {
                                if (index != 0) { assets += ','; starts += ','; ranges += ','; }
                                assets += homeworldz::api::json_string(terrain_layers.assets[index]);
                                starts += std::to_string(terrain_layers.start[index]);
                                ranges += std::to_string(terrain_layers.range[index]);
                            }
                            response = homeworldz::http::response_for_content(
                                request, 200, "application/json",
                                "{\"assets\":[" + assets + "],\"startHeight\":[" + starts +
                                    "],\"heightRange\":[" + ranges + "],\"waterHeight\":" +
                                    std::to_string(region_settings.water_height) + "}");
                        }
                    }
                    if (response.path == "/session/terrain") {
                        // The ground itself, for session clients (client core
                        // request, 2026-07-29): the same heightmap the region
                        // collides against, float32 little-endian meters,
                        // row-major from y=0, one vertex per meter. The hello
                        // terrain block states the width and interpolation
                        // rule. Authorized by the region ticket, exactly as
                        // the mesh upload path is.
                        const auto authorization =
                            homeworldz::http::request_header_value(request, "Authorization");
                        constexpr std::string_view bearer = "Bearer ";
                        std::optional<homeworldz::grid::TicketIdentity> requester;
                        // A ticket that could not be *checked* is not a ticket that was
                        // rejected. Validation is a round trip to the grid, so an
                        // unreachable grid and a bad credential both left this unset and
                        // both answered 401 - and the two want opposite things from the
                        // person holding it. Bad credential means sign in again; could
                        // not validate means wait. Telling someone to re-authenticate
                        // during an outage produces a second failure that looks like a
                        // password problem, at the moment the grid can least answer it
                        // (client core, 2026-08-08).
                        bool ticket_unverifiable = false;
                        if (response.method == "GET" && authorization.starts_with(bearer) &&
                            viewer_grid && registration) {
                            try {
                                homeworldz::grid::Client ticket_client(
                                    homeworldz::grid::socket_transport(
                                        configured_value("grid.url", "http://localhost:42000"),
                                        region_access_key));
                                requester = ticket_client.validate_region_ticket(
                                    provisioned_region_id, authorization.substr(bearer.size()));
                            } catch (const std::exception&) {
                                ticket_unverifiable = true;
                            }
                        }
                        if (response.method != "GET") {
                            response = homeworldz::http::response_for_content(
                                request, 405, "application/json",
                                homeworldz::api::to_json(homeworldz::api::Error{
                                    "method_not_allowed", "terrain requires GET"}));
                        } else if (ticket_unverifiable) {
                            response = homeworldz::http::response_for_content(
                                request, 503, "application/json",
                                homeworldz::api::to_json(homeworldz::api::Error{
                                    "ticket_validation_unavailable",
                                    "the ticket could not be validated because the grid did not "
                                    "answer; retry rather than signing in again"}));
                        } else if (!requester) {
                            response = homeworldz::http::response_for_content(
                                request, 401, "application/json",
                                homeworldz::api::to_json(homeworldz::api::Error{
                                    "unauthorized",
                                    "a valid region ticket bearer token is required"}));
                        } else {
                            // The revision as an ETag, so a client that is
                            // already current spends no bytes learning it - and
                            // a reconnect or a crossing, where a missed edit
                            // used to survive unnoticed, becomes one cheap
                            // question. Ranges are honoured too: the map is
                            // row-major, so dirty rows are contiguous and 16
                            // rows of a 1024 region is 64 KB against 4 MB.
                            const auto etag = "\"" + std::to_string(terrain_revision) + "\"";
                            const auto known = homeworldz::http::request_header_value(
                                request, "If-None-Match");
                            auto body = encode_heightmap(*terrain_heightmap);
                            const auto range_header =
                                homeworldz::http::request_header_value(request, "Range");
                            if (!known.empty() && known != etag) {
                                // A client asking with a revision it holds that
                                // is not the current one is behind despite
                                // whatever was sent it - a patch that failed to
                                // apply, an event dropped, or heights omitted
                                // above the cap. That is invisible from here
                                // otherwise: a well-formed event goes out and no
                                // complaint comes back (client core, 2026-07-30).
                                // Soon after an announcement means the client
                                // could not use what was sent - a patch that
                                // failed to decode, or heights omitted above the
                                // cap. Long after means it was away and is
                                // refetching by design, which a reconnect and a
                                // crossing both do. Same request, different
                                // fact; the line says which rather than leaving
                                // a reader to correlate timestamps.
                                const auto since_announce = last_terrain_notice_at.time_since_epoch()
                                        .count() == 0
                                    ? -1.0
                                    : std::chrono::duration<double>(
                                          std::chrono::steady_clock::now() -
                                          last_terrain_notice_at).count();
                                const bool soon = since_announce >= 0.0 && since_announce < 2.0;
                                std::cout << "{\"level\":\"info\",\"message\":"
                                             "\"terrain refetched from behind\",\"held\":"
                                          << homeworldz::api::json_string(known)
                                          << ",\"current\":" << terrain_revision
                                          << ",\"secondsSinceAnnounce\":" << since_announce
                                          << ",\"likely\":"
                                          << (soon ? "\"the announced heights were unusable\""
                                                   : "\"the client was away\"")
                                          << "}" << std::endl;
                            }
                            if (known == etag) {
                                response = homeworldz::http::response_for_content(
                                    request, 304, "application/vnd.homeworldz.heightmap-f32le",
                                    {});
                            } else if (range_header.starts_with("bytes=")) {
                                std::size_t offset = 0;
                                std::size_t length = body.size();
                                const auto dash = range_header.find('-', 6);
                                bool ranged = false;
                                if (dash != std::string::npos) {
                                    const auto from = range_header.substr(6, dash - 6);
                                    const auto to = range_header.substr(dash + 1);
                                    char* end = nullptr;
                                    offset = std::strtoull(from.c_str(), &end, 10);
                                    if (end != nullptr && *end == 0 && offset < body.size()) {
                                        if (to.empty()) {
                                            length = body.size() - offset;
                                            ranged = true;
                                        } else {
                                            const auto last = std::strtoull(to.c_str(), &end, 10);
                                            if (end != nullptr && *end == 0 && last >= offset) {
                                                length = last - offset + 1;
                                                ranged = true;
                                            }
                                        }
                                    }
                                }
                                response = ranged
                                    ? homeworldz::http::response_for_range(
                                          request, "application/vnd.homeworldz.heightmap-f32le",
                                          body, offset, length)
                                    : homeworldz::http::response_for_content(
                                          request, 416,
                                          "application/vnd.homeworldz.heightmap-f32le", {});
                            } else {
                                response = homeworldz::http::response_for_content(
                                    request, 200, "application/vnd.homeworldz.heightmap-f32le",
                                    std::move(body));
                            }
                            homeworldz::http::add_header(response, "ETag", etag);
                            homeworldz::http::add_header(response, "Accept-Ranges", "bytes");
                        }
                    }
                    if (const auto session_asset = session_asset_request(response.path)) {
                        // Canonical asset bytes for session clients (client
                        // core request, 2026-07-29): until now a session could
                        // learn an asset id — from an upload reply, from an
                        // object's geometry block — and had no way to fetch
                        // one. Same credential as terrain and mesh upload, and
                        // the same exposure the viewer asset capability
                        // already has: any authenticated session may fetch any
                        // asset by id.
                        //
                        // The bytes are canonical, never a rendition: a client
                        // on the modern path wants what the creator uploaded.
                        // Content-Type names the format it actually got, which
                        // is load-bearing here — a mesh uploaded through the
                        // session path is glTF, and one uploaded by a viewer
                        // is Second Life mesh (ADR 0033's read-never-encode
                        // storage), so a client cannot assume from the id.
                        const auto authorization =
                            homeworldz::http::request_header_value(request, "Authorization");
                        constexpr std::string_view bearer = "Bearer ";
                        std::optional<homeworldz::grid::TicketIdentity> requester;
                        // A ticket that could not be *checked* is not a ticket that was
                        // rejected. Validation is a round trip to the grid, so an
                        // unreachable grid and a bad credential both left this unset and
                        // both answered 401 - and the two want opposite things from the
                        // person holding it. Bad credential means sign in again; could
                        // not validate means wait. Telling someone to re-authenticate
                        // during an outage produces a second failure that looks like a
                        // password problem, at the moment the grid can least answer it
                        // (client core, 2026-08-08).
                        bool ticket_unverifiable = false;
                        if (response.method == "GET" && authorization.starts_with(bearer) &&
                            viewer_grid && registration) {
                            try {
                                homeworldz::grid::Client ticket_client(
                                    homeworldz::grid::socket_transport(
                                        configured_value("grid.url", "http://localhost:42000"),
                                        region_access_key));
                                requester = ticket_client.validate_region_ticket(
                                    provisioned_region_id, authorization.substr(bearer.size()));
                            } catch (const std::exception&) {
                                ticket_unverifiable = true;
                            }
                        }
                        if (response.method != "GET") {
                            response = homeworldz::http::response_for_content(
                                request, 405, "application/json",
                                homeworldz::api::to_json(homeworldz::api::Error{
                                    "method_not_allowed", "asset fetch requires GET"}));
                        } else if (ticket_unverifiable) {
                            response = homeworldz::http::response_for_content(
                                request, 503, "application/json",
                                homeworldz::api::to_json(homeworldz::api::Error{
                                    "ticket_validation_unavailable",
                                    "the ticket could not be validated because the grid did not "
                                    "answer; retry rather than signing in again"}));
                        } else if (!requester) {
                            response = homeworldz::http::response_for_content(
                                request, 401, "application/json",
                                homeworldz::api::to_json(homeworldz::api::Error{
                                    "unauthorized",
                                    "a valid region ticket bearer token is required"}));
                        } else {
                            try {
                                const auto bytes = read_federated_asset(*session_asset);
                                if (bytes.empty()) throw std::runtime_error("empty asset");
                                auto text = std::string(
                                    reinterpret_cast<const char*>(bytes.data()), bytes.size());
                                std::string content_type = "application/octet-stream";
                                // Set only where a derived rendition may stand in
                                // for the canonical, which is what decides whether
                                // this id's representation can ever change.
                                bool derived_representation = false;
                                const auto byte_at = [&](std::size_t index) {
                                    return index < bytes.size()
                                        ? static_cast<unsigned char>(bytes[index]) : 0u;
                                };
                                if (text.starts_with("glTF")) {
                                    content_type = "model/gltf-binary";
                                } else if (byte_at(0) == 0x89 && text.substr(1, 3) == "PNG") {
                                    // Textures extracted from a GLB are canonical
                                    // in the creator's own format, so this route
                                    // serves real images and must name them - a
                                    // client deciding what to decode should not
                                    // have to sniff what the server already knew
                                    // (ADR 0033 M3).
                                    content_type = "image/png";
                                } else if (byte_at(0) == 0xff && byte_at(1) == 0xd8) {
                                    content_type = "image/jpeg";
                                } else if (byte_at(0) == 0xff && byte_at(1) == 0x4f) {
                                    // Canonical JPEG2000 means a viewer uploaded
                                    // this texture, and this route's clients
                                    // refuse that format by rule — so every
                                    // texture made in Firestorm was invisible to
                                    // them. Serve the png-texture rendition,
                                    // exactly as the mesh branch below serves
                                    // glTF for a canonical Second Life mesh: each
                                    // family asks for one id and receives the
                                    // form it can read (ADR 0033).
                                    content_type = "image/x-j2c";
                                    derived_representation = true;
                                    if (viewer_grid) {
                                        if (auto modern = viewer_grid->fetch_asset_rendition(
                                                *session_asset, "png-texture")) {
                                            text = std::move(*modern);
                                            content_type = "image/png";
                                        } else {
                                            // Not converted yet: queue it and
                                            // answer honestly with what is stored,
                                            // so content predating the derivation
                                            // heals on first demand.
                                            static_cast<void>(
                                                viewer_grid->request_asset_rendition(
                                                    *session_asset, "png-texture"));
                                        }
                                    }
                                } else if (text.starts_with("{\"")) {
                                    content_type = "application/json";
                                } else if (homeworldz::slmesh::parse(bytes)) {
                                    // Canonical Second Life mesh: serve the glTF
                                    // derivation when one exists, because this
                                    // route exists for clients that do not read
                                    // the legacy serialization. Symmetric with
                                    // the viewer's GetMesh, which is served the
                                    // legacy rendition of a canonical GLB — each
                                    // family fetches one id and receives the form
                                    // it can use. The Content-Type always names
                                    // what was actually served.
                                    content_type = "application/vnd.ll.mesh";
                                    // This branch is the one place the route is
                                    // not immutable by id, and it moves in two
                                    // ways: a rendition is regenerated when the
                                    // converter's generator changes (28 of them
                                    // reconverted on 2026-07-31), and until one
                                    // exists the honest answer is the legacy
                                    // bytes, so two requests seconds apart can
                                    // differ in body *and* Content-Type.
                                    derived_representation = true;
                                    if (viewer_grid) {
                                        if (auto modern = viewer_grid->fetch_asset_rendition(
                                                *session_asset, "gltf")) {
                                            text = std::move(*modern);
                                            content_type = "model/gltf-binary";
                                        } else {
                                            // Not converted yet: queue it and
                                            // answer honestly with the legacy
                                            // bytes this time. Requesting is
                                            // idempotent, so content that
                                            // predates the derivation heals on
                                            // first demand rather than needing
                                            // an operator sweep.
                                            static_cast<void>(
                                                viewer_grid->request_asset_rendition(
                                                    *session_asset, "gltf"));
                                        }
                                    }
                                }
                                // A cache needs to know when a stored copy is
                                // still good, and this route said nothing at all
                                // — no validator, no freshness — so any reuse
                                // rested on the client's own guess, and a cache
                                // built on a guess serves stale bytes
                                // confidently (client core, 2026-07-31).
                                //
                                // The validator is the digest of exactly what is
                                // being served, so it cannot disagree with the
                                // body: derived or canonical, ready or pending,
                                // the ETag is computed after the decision rather
                                // than from anything believed about it.
                                const auto served = std::span(
                                    reinterpret_cast<const std::byte*>(text.data()), text.size());
                                const auto etag = "\"" + homeworldz::crypto::sha256_hex(served) + "\"";
                                const auto known = homeworldz::http::request_header_value(
                                    request, "If-None-Match");
                                if (known == etag) {
                                    response = homeworldz::http::response_for_content(
                                        request, 304, content_type, {});
                                } else {
                                    response = homeworldz::http::response_for_content(
                                        request, 200, content_type, std::move(text));
                                }
                                homeworldz::http::add_header(response, "ETag", etag);
                                // Canonical bytes are never rewritten (ADR 0026),
                                // so a canonical representation is immutable for
                                // the life of its id and a client needs no round
                                // trip at all on a second visit. A derived one is
                                // regenerable, so it gets revalidation instead —
                                // the distinction is real and invisible from
                                // outside, which is why it is stated here rather
                                // than left to be inferred.
                                homeworldz::http::add_header(
                                    response, "Cache-Control",
                                    derived_representation
                                        ? "private, no-cache"
                                        : "private, max-age=31536000, immutable");
                            } catch (const std::exception&) {
                                response = homeworldz::http::response_for_content(
                                    request, 404, "application/json",
                                    homeworldz::api::to_json(homeworldz::api::Error{
                                        "asset_not_found", "no such asset"}));
                            }
                        }
                    }
                    if (response.path == homeworldz::mesh::upload_path) {
                        // The session client's GLB upload (ADR 0033 M1),
                        // authorized by the same region ticket the WebSocket
                        // authenticates with — one credential, both
                        // transports. The gate is the published policy;
                        // refusals carry the validator's own reason, sized
                        // for showing a creator verbatim.
                        const auto authorization =
                            homeworldz::http::request_header_value(request, "Authorization");
                        constexpr std::string_view bearer = "Bearer ";
                        std::optional<homeworldz::grid::TicketIdentity> uploader;
                        // A ticket that could not be *checked* is not a ticket that was
                        // rejected. Validation is a round trip to the grid, so an
                        // unreachable grid and a bad credential both left this unset and
                        // both answered 401 - and the two want opposite things from the
                        // person holding it. Bad credential means sign in again; could
                        // not validate means wait. Telling someone to re-authenticate
                        // during an outage produces a second failure that looks like a
                        // password problem, at the moment the grid can least answer it
                        // (client core, 2026-08-08).
                        bool ticket_unverifiable = false;
                        if (response.method == "POST" && authorization.starts_with(bearer) &&
                            viewer_grid && registration) {
                            try {
                                // Validation runs on the region's own
                                // credential (the access key), exactly as the
                                // WebSocket validator does: validate-ticket is
                                // a region-runtime endpoint and the ticket
                                // secret never reaches the region.
                                homeworldz::grid::Client ticket_client(
                                    homeworldz::grid::socket_transport(
                                        configured_value("grid.url", "http://localhost:42000"),
                                        region_access_key));
                                uploader = ticket_client.validate_region_ticket(
                                    provisioned_region_id, authorization.substr(bearer.size()));
                            } catch (const std::exception&) {
                                ticket_unverifiable = true;
                            }
                        }
                        if (response.method != "POST") {
                            response = homeworldz::http::response_for_content(
                                request, 405, "application/json",
                                homeworldz::api::to_json(homeworldz::api::Error{
                                    "method_not_allowed", "mesh upload requires POST"}));
                        } else if (ticket_unverifiable) {
                            response = homeworldz::http::response_for_content(
                                request, 503, "application/json",
                                homeworldz::api::to_json(homeworldz::api::Error{
                                    "ticket_validation_unavailable",
                                    "the ticket could not be validated because the grid did not "
                                    "answer; retry rather than signing in again"}));
                        } else if (!uploader) {
                            response = homeworldz::http::response_for_content(
                                request, 401, "application/json",
                                homeworldz::api::to_json(homeworldz::api::Error{
                                    "unauthorized",
                                    "a valid region ticket bearer token is required"}));
                        } else {
                            const auto body = http_request_body(request);
                            const auto content = std::span(
                                reinterpret_cast<const std::byte*>(body.data()), body.size());
                            const auto acceptance = homeworldz::mesh::validate_glb(content);
                            if (!acceptance.accepted) {
                                response = homeworldz::http::response_for_content(
                                    request, 422, "application/json",
                                    homeworldz::api::to_json(homeworldz::api::Error{
                                        "mesh_refused", acceptance.reason}));
                            } else {
                                try {
                                    auto name = homeworldz::http::request_header_value(
                                        request, "X-Homeworldz-Name");
                                    if (name.empty()) name = "Mesh";
                                    if (name.size() > 255) name.resize(255);
                                    // As in Second Life, a mesh upload yields
                                    // an OBJECT item: viewers cannot rez a
                                    // bare mesh asset. The mesh asset (the
                                    // canonical GLB) is wrapped by a one-prim
                                    // object whose sculpt entry names it, and
                                    // whose scale is the model's declared
                                    // world bounds -- the same bounds the
                                    // converter normalizes by, so it renders
                                    // at authored size (ADR 0033).
                                    const auto bounds = homeworldz::mesh::declared_world_bounds(content);
                                    if (!bounds.ok)
                                        throw std::runtime_error(
                                            "the GLB declares no position bounds");
                                    const auto stored = storage->store_asset(
                                        homeworldz::viewer::random_uuid(),
                                        uploader->user_id, content);
                                    if (!viewer_grid->register_asset(
                                            stored.viewer_id, stored.creator_id, stored.sha256,
                                            stored.size, region_public_endpoint, true))
                                        throw std::runtime_error("mesh asset registration failed");
                                    // Write-through before the commit. Load-
                                    // bearing here, not just the ADR 0026
                                    // optimization: this thread is the one
                                    // that would serve the grid's fetch-back,
                                    // so the commit must find the blob
                                    // already vault-held.
                                    if (!viewer_grid->store_vault_asset(stored.viewer_id, content))
                                        throw std::runtime_error("vault write-through failed");
                                    // The GLB's textures become assets of their
                                    // own (ADR 0033 M3). A viewer cannot read a
                                    // PNG embedded in a GLB, so each image is
                                    // stored canonically as the creator's own
                                    // bytes - a format the modern client reads
                                    // directly - and a j2c-texture rendition is
                                    // queued for the viewer pipeline. The same
                                    // canonical/derived split the mesh uses,
                                    // pointed at images, rather than storing
                                    // JPEG2000 at rest and inverting it.
                                    const auto extracted = homeworldz::mesh::extract_textures(content);
                                    if (!extracted.ok)
                                        throw std::runtime_error(extracted.error);
                                    std::vector<std::string> texture_assets;
                                    for (const auto& texture : extracted.textures) {
                                        const auto image = storage->store_asset(
                                            homeworldz::viewer::random_uuid(),
                                            uploader->user_id, texture.bytes);
                                        if (!viewer_grid->register_asset(
                                                image.viewer_id, image.creator_id, image.sha256,
                                                image.size, region_public_endpoint, true) ||
                                            !viewer_grid->store_vault_asset(
                                                image.viewer_id, texture.bytes))
                                            throw std::runtime_error(
                                                "texture asset registration failed");
                                        static_cast<void>(viewer_grid->request_asset_rendition(
                                            image.viewer_id, "j2c-texture"));
                                        texture_assets.push_back(image.viewer_id);
                                    }
                                    std::cout << "{\"level\":\"info\",\"message\":"
                                                 "\"mesh textures extracted\",\"images\":"
                                              << extracted.textures.size()
                                              << ",\"faces\":" << extracted.face_textures.size()
                                              << ",\"textured\":"
                                              << std::count_if(extracted.face_textures.begin(),
                                                               extracted.face_textures.end(),
                                                               [](int value) { return value >= 0; })
                                              << "}" << std::endl;
                                    homeworldz::scene::Entity wrapper;
                                    wrapper.name = name;
                                    wrapper.creator_id = uploader->user_id;
                                    wrapper.owner_id = uploader->user_id;
                                    wrapper.sculpt_id = stored.viewer_id;
                                    wrapper.sculpt_type = 5; // mesh
                                    // A face with no texture entry renders
                                    // transparent (verified live on
                                    // Firestorm, 2026-07-29); the default
                                    // entry is a bundled asset the startup
                                    // write-through keeps vault-held, so the
                                    // commit closure stays deadlock-free.
                                    //
                                    // Where the GLB carried images, the faces
                                    // name them instead: the extraction reports
                                    // a texture per face in the same order the
                                    // converter emits faces, from one shared
                                    // traversal, so face N means the same face
                                    // to both (ADR 0033 M3). Until the
                                    // j2c-texture rendition exists a viewer
                                    // asking for one of these gets not-yet,
                                    // which is the same contract mesh has.
                                    if (texture_assets.empty()) {
                                        wrapper.texture_entry = blank_prim_texture_entry();
                                    } else {
                                        std::vector<homeworldz::mesh_model::Face> faces;
                                        std::vector<std::optional<homeworldz::viewer::Uuid>> images;
                                        for (const auto& asset : texture_assets)
                                            images.push_back(homeworldz::viewer::parse_uuid(asset));
                                        for (const auto index : extracted.face_textures)
                                            faces.push_back({index, {1.0f, 1.0f, 1.0f, 1.0f}});
                                        wrapper.texture_entry =
                                            homeworldz::mesh_model::instance_texture_entry(
                                                blank_texture_id(), faces, images);
                                    }
                                    wrapper.scale.x = std::clamp(bounds.extent[0], 0.01f, 64.0f);
                                    wrapper.scale.y = std::clamp(bounds.extent[1], 0.01f, 64.0f);
                                    wrapper.scale.z = std::clamp(bounds.extent[2], 0.01f, 64.0f);
                                    const auto wrapped =
                                        homeworldz::asset::serialize_linkset_asset(wrapper);
                                    const auto wrapped_bytes = std::span(
                                        reinterpret_cast<const std::byte*>(wrapped.data()),
                                        wrapped.size());
                                    const auto object_stored = storage->store_asset(
                                        homeworldz::viewer::random_uuid(), uploader->user_id,
                                        wrapped_bytes);
                                    if (!viewer_grid->register_asset(
                                            object_stored.viewer_id, object_stored.creator_id,
                                            object_stored.sha256, object_stored.size,
                                            region_public_endpoint, true))
                                        throw std::runtime_error("object asset registration failed");
                                    if (!viewer_grid->store_vault_asset(
                                            object_stored.viewer_id, wrapped_bytes))
                                        throw std::runtime_error("object vault write-through failed");
                                    const auto folder = viewer_grid->find_system_inventory_folder(
                                        uploader->user_id, 6);
                                    if (!folder)
                                        throw std::runtime_error("objects folder unavailable");
                                    homeworldz::grid::InventoryItem item;
                                    item.item_id = homeworldz::viewer::random_uuid();
                                    item.creator_id = uploader->user_id;
                                    item.owner_id = uploader->user_id;
                                    item.folder_id = *folder;
                                    item.asset_id = object_stored.viewer_id;
                                    item.asset_type = 6;
                                    item.inventory_type = 6;
                                    item.name = name;
                                    item.base_permissions = 0x7fffffff;
                                    item.current_permissions = 0x7fffffff;
                                    item.everyone_permissions = 0;
                                    item.next_permissions = 581632;
                                    // The commit's closure walk finds the
                                    // wrapper and, through its sculptId, the
                                    // GLB -- both already vault-held by
                                    // write-through, so no fetch-back can
                                    // deadlock this thread.
                                    if (!viewer_grid->create_inventory_item(
                                            uploader->user_id, item))
                                        throw std::runtime_error(
                                            "inventory commit was refused");
                                    static_cast<void>(viewer_grid->request_asset_rendition(
                                        stored.viewer_id, "sl-mesh"));
                                    response = homeworldz::http::response_for_content(
                                        request, 201, "application/json",
                                        "{\"assetId\":" + homeworldz::api::json_string(stored.viewer_id) +
                                        ",\"objectAssetId\":" + homeworldz::api::json_string(object_stored.viewer_id) +
                                        ",\"itemId\":" + homeworldz::api::json_string(item.item_id) +
                                        ",\"triangles\":" + std::to_string(acceptance.triangles) +
                                        ",\"materials\":" + std::to_string(acceptance.materials) +
                                        ",\"renditions\":{\"sl-mesh\":\"queued\"}}");
                                    std::cout << "{\"level\":\"info\",\"message\":\"mesh uploaded\",\"assetId\":"
                                              << homeworldz::api::json_string(stored.viewer_id)
                                              << ",\"creator\":" << homeworldz::api::json_string(uploader->userid)
                                              << ",\"triangles\":" << acceptance.triangles << "}" << std::endl;
                                } catch (const std::exception& error) {
                                    std::cout << "{\"level\":\"error\",\"message\":\"mesh upload failed\",\"error\":"
                                              << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                                    response = homeworldz::http::response_for_content(
                                        request, 502, "application/json",
                                        homeworldz::api::to_json(homeworldz::api::Error{
                                            "mesh_upload_failed", error.what()}));
                                }
                            }
                        }
                    }
                    constexpr std::string_view arrival_prefix = "/api/v1/transits/";
                    constexpr std::string_view arrival_suffix = "/prepare-arrival";
                    if (response.path.starts_with(arrival_prefix) &&
                        response.path.ends_with(arrival_suffix)) {
                        const auto transit_id = response.path.substr(
                            arrival_prefix.size(), response.path.size() -
                            arrival_prefix.size() - arrival_suffix.size());
                        const auto authorization =
                            homeworldz::http::request_header_value(request, "Authorization");
                        if (response.method != "POST") {
                            response = homeworldz::http::response_for_content(
                                request, 405, "application/json",
                                homeworldz::api::to_json(homeworldz::api::Error{
                                    "method_not_allowed", "arrival preparation requires POST"}));
                        } else if (service_token.empty() || authorization != "Bearer " + service_token) {
                            response = homeworldz::http::response_for_content(
                                request, 401, "application/json",
                                homeworldz::api::to_json(homeworldz::api::Error{
                                    "unauthorized", "a valid grid service token is required"}));
                        } else if (!homeworldz::viewer::parse_uuid(transit_id) ||
                                   !viewer_grid || !registration) {
                            response = homeworldz::http::response_for_content(
                                request, 404, "application/json",
                                homeworldz::api::to_json(homeworldz::api::Error{
                                    "transit_not_found", "avatar transit was not found"}));
                        } else {
                            std::optional<homeworldz::grid::AvatarTransit> transit;
                            try {
                                transit = viewer_grid->find_avatar_transit(transit_id);
                                if (transit && transit->state == "prepared" &&
                                    transit->destination_region_id == registration->region_id())
                                    transit = viewer_grid->accept_avatar_transit(
                                        transit_id, registration->region_id());
                            } catch (const std::exception& error) {
                                std::cout << "{\"level\":\"error\",\"message\":\"arrival preparation failed\",\"error\":"
                                          << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                            }
                            if (!transit || !inbound_transits.stage(
                                    *transit, registration->region_id(),
                                    std::chrono::steady_clock::now())) {
                                response = homeworldz::http::response_for_content(
                                    request, 409, "application/json",
                                    homeworldz::api::to_json(homeworldz::api::Error{
                                        "transit_not_preparable", "avatar transit could not be prepared"}));
                            } else {
                                response = homeworldz::http::response_for_content(
                                    request, 200, "application/json",
                                    homeworldz::api::to_json(homeworldz::api::Status{"accepted"}));
                            }
                        }
                    }
                    if (const auto asset_request = internal_asset_request(response.path)) {
                        const auto authorization = homeworldz::http::request_header_value(request, "Authorization");
                        const auto expected_method = asset_request->replicate ? "POST" : "GET";
                        if (response.method != expected_method) {
                            response = homeworldz::http::response_for_content(
                                request, 405, "application/json",
                                homeworldz::api::to_json(homeworldz::api::Error{
                                    "method_not_allowed", "asset endpoint method is invalid"}));
                        } else if (service_token.empty() || authorization != "Bearer " + service_token) {
                            response = homeworldz::http::response_for_content(
                                request, 401, "application/json",
                                homeworldz::api::to_json(homeworldz::api::Error{
                                    "unauthorized", "a valid grid service token is required"}));
                        } else if (asset_request->replicate) {
                            try {
                                const auto asset = read_federated_asset(asset_request->asset_id);
                                response = homeworldz::http::response_for_content(
                                    request, 200, "application/json",
                                    homeworldz::api::to_json(homeworldz::api::Status{"replicated"}));
                            } catch (const std::exception&) {
                                response = homeworldz::http::response_for_content(
                                    request, 404, "application/json",
                                    homeworldz::api::to_json(homeworldz::api::Error{
                                        "asset_not_found", "no verified asset source was available"}));
                            }
                        } else {
                            try {
                                const auto asset = storage->read_asset(asset_request->asset_id);
                                response = homeworldz::http::response_for_content(
                                    request, 200, "application/octet-stream",
                                    std::string(reinterpret_cast<const char*>(asset.data()), asset.size()));
                            } catch (const std::exception&) {
                                response = homeworldz::http::response_for_content(
                                    request, 404, "application/json",
                                    homeworldz::api::to_json(homeworldz::api::Error{
                                        "asset_not_found", "asset was not found"}));
                            }
                        }
                    }
                    constexpr std::string_view start_state_prefix = "/api/v1/agents/";
                    constexpr std::string_view start_state_suffix = "/start-state";
                    if (response.path.starts_with(start_state_prefix) &&
                        response.path.ends_with(start_state_suffix)) {
                        const auto user_id = response.path.substr(
                            start_state_prefix.size(), response.path.size() -
                            start_state_prefix.size() - start_state_suffix.size());
                        const auto authorization = homeworldz::http::request_header_value(request, "Authorization");
                        if (response.method != "GET") {
                            response = homeworldz::http::response_for_content(
                                request, 405, "application/json",
                                homeworldz::api::to_json(homeworldz::api::Error{
                                    "method_not_allowed", "start-state endpoint requires GET"}));
                        } else if (service_token.empty() || authorization != "Bearer " + service_token) {
                            response = homeworldz::http::response_for_content(
                                request, 401, "application/json",
                                homeworldz::api::to_json(homeworldz::api::Error{
                                    "unauthorized", "a valid grid service token is required"}));
                        } else if (!homeworldz::viewer::parse_uuid(user_id)) {
                            response = homeworldz::http::response_for_content(
                                request, 404, "application/json",
                                homeworldz::api::to_json(homeworldz::api::Error{
                                    "agent_not_found", "agent start state was not found"}));
                        } else {
                            const homeworldz::scene::Entity* agent{};
                            for (const auto& [candidate_id, candidate] : scene.entities())
                                if (candidate.name == user_id && (!agent || candidate_id > agent->id))
                                    agent = &candidate;
                            if (!agent) {
                                response = homeworldz::http::response_for_content(
                                    request, 404, "application/json",
                                    homeworldz::api::to_json(homeworldz::api::Error{
                                        "agent_not_found", "agent start state was not found"}));
                            } else {
                                const double qx = agent->rotation.x, qy = agent->rotation.y,
                                             qz = agent->rotation.z;
                                const auto qw = std::sqrt((std::max)(
                                    0.0, 1.0 - qx * qx - qy * qy - qz * qz));
                                const auto look_x = 1.0 - 2.0 * (qy * qy + qz * qz);
                                const auto look_y = 2.0 * (qx * qy + qw * qz);
                                const auto body = std::string{"{\"position\":["} +
                                    std::to_string(agent->position.x) + ',' +
                                    std::to_string(agent->position.y) + ',' +
                                    std::to_string(agent->position.z) + "],\"lookAt\":[" +
                                    std::to_string(look_x) + ',' + std::to_string(look_y) +
                                    ",0],\"flying\":" +
                                    (agent->avatar_flying ? "true}" : "false}");
                                response = homeworldz::http::response_for_content(
                                    request, 200, "application/json", body);
                            }
                        }
                    }
                    auto session_id = homeworldz::caps::capability_session(response.path, "/caps/seed/");
                    const bool seed = !session_id.empty();
                    if (!seed) session_id = homeworldz::caps::capability_session(response.path, "/caps/event/");
                    const bool event_queue = !seed && !session_id.empty();
                    const auto capability_visit_id = seed ?
                        homeworldz::caps::capability_visit(response.path, "/caps/seed/") :
                        homeworldz::caps::capability_visit(response.path, "/caps/event/");
                    const auto viewer_asset = homeworldz::caps::viewer_asset_request(response.path);
                    // A texture is a texture whichever capability asked for it,
                    // and texture_fetch is the single place that decides so —
                    // see capability_paths.h for the bug that rule exists for.
                    const auto texture =
                        homeworldz::caps::texture_fetch(response.path, viewer_asset);
                    if (texture) session_id = texture->session;
                    if (viewer_asset) session_id = viewer_asset->session;
                    std::string simulator_features_session;
                    if (!seed && !event_queue && !texture && !viewer_asset)
                        simulator_features_session =
                            homeworldz::caps::capability_session(response.path, "/caps/simulator-features/");
                    const bool simulator_features = !simulator_features_session.empty();
                    if (simulator_features) session_id = simulator_features_session;
                    std::string environment_session;
                    if (!seed && !event_queue && !texture && !viewer_asset && !simulator_features)
                        environment_session = homeworldz::caps::capability_session(response.path, "/caps/environment/");
                    const bool environment_settings = !environment_session.empty();
                    if (environment_settings) session_id = environment_session;
                    std::string remote_parcel_session;
                    if (!seed && !event_queue && !texture && !viewer_asset && !simulator_features &&
                        !environment_settings)
                        remote_parcel_session =
                            homeworldz::caps::capability_session(response.path, "/caps/remote-parcel/");
                    const bool remote_parcel = !remote_parcel_session.empty();
                    if (remote_parcel) session_id = remote_parcel_session;
                    const auto release_notes_session =
                        homeworldz::caps::capability_session(response.path,
                                                             "/caps/server-release-notes/");
                    const bool server_release_notes = !release_notes_session.empty();
                    if (server_release_notes) session_id = release_notes_session;
                    const auto baked_upload_session =
                        homeworldz::caps::capability_session(response.path, "/caps/upload-baked/");
                    const bool baked_upload = !baked_upload_session.empty();
                    if (baked_upload) session_id = baked_upload_session;
                    const auto baked_upload_data = baked_upload_data_request(response.path);
                    if (baked_upload_data) session_id = baked_upload_data->first;
                    const auto file_upload_session =
                        homeworldz::caps::capability_session(response.path, "/caps/upload-file/");
                    const bool file_upload = !file_upload_session.empty();
                    if (file_upload) session_id = file_upload_session;
                    const auto file_upload_data = file_upload_data_request(response.path);
                    if (file_upload_data) session_id = file_upload_data->first;
                    const auto model_upload_data = model_upload_data_request(response.path);
                    if (model_upload_data) session_id = model_upload_data->first;
                    const auto mesh_upload_flag_session =
                        homeworldz::caps::capability_session(response.path, "/caps/mesh-upload-flag/");
                    const bool mesh_upload_flag = !mesh_upload_flag_session.empty();
                    if (mesh_upload_flag) session_id = mesh_upload_flag_session;
                    const auto render_materials_session =
                        homeworldz::caps::capability_session(response.path, "/caps/render-materials/");
                    const bool render_materials = !render_materials_session.empty();
                    if (render_materials) session_id = render_materials_session;
                    const auto notecard_update_session =
                        homeworldz::caps::capability_session(response.path, "/caps/update-notecard/");
                    const bool notecard_update = !notecard_update_session.empty();
                    if (notecard_update) session_id = notecard_update_session;
                    const auto script_update_session =
                        homeworldz::caps::capability_session(response.path, "/caps/update-script/");
                    const bool script_update = !script_update_session.empty();
                    if (script_update) session_id = script_update_session;
                    const auto gesture_update_session =
                        homeworldz::caps::capability_session(response.path, "/caps/update-gesture/");
                    const bool gesture_update = !gesture_update_session.empty();
                    if (gesture_update) session_id = gesture_update_session;
                    const auto task_notecard_update_session =
                        homeworldz::caps::capability_session(response.path, "/caps/update-task-notecard/");
                    const bool task_notecard_update = !task_notecard_update_session.empty();
                    if (task_notecard_update) session_id = task_notecard_update_session;
                    const auto task_script_update_session =
                        homeworldz::caps::capability_session(response.path, "/caps/update-task-script/");
                    const bool task_script_update = !task_script_update_session.empty();
                    if (task_script_update) session_id = task_script_update_session;
                    const auto inventory_asset_update_data =
                        inventory_asset_update_data_request(response.path);
                    if (inventory_asset_update_data) session_id = inventory_asset_update_data->first;
                    // Every capability with a handler below must appear here too,
                    // or its request never reaches the chain and answers 404 with
                    // nothing said. RenderMaterials was missing from this list and
                    // did exactly that, four times, while the capability was
                    // advertised and its path parsed correctly (found live
                    // 2026-07-31). That is a third place the same fact has to be
                    // written, after the seed reply and the path parser — the
                    // fall-through warning further down exists because this list
                    // cannot be checked from outside main.cpp.
                    if (seed || event_queue || texture || viewer_asset || simulator_features || environment_settings ||
                        remote_parcel || server_release_notes ||
                        baked_upload || baked_upload_data || file_upload || file_upload_data ||
                        model_upload_data || mesh_upload_flag || render_materials ||
                        notecard_update || script_update || gesture_update ||
                        task_notecard_update || task_script_update || inventory_asset_update_data) {
                        bool authorized = false;
                        std::string authorized_agent_id;
                        std::optional<homeworldz::grid::ViewerSession> authorized_session;
                        const auto expected_method =
                            texture || viewer_asset || simulator_features || environment_settings ||
                            mesh_upload_flag || server_release_notes ? "GET" : "POST";
                        // RenderMaterials is read with GET and written with PUT —
                        // Firestorm uses PUT, observed on the wire — so one
                        // expected method cannot express it. POST is accepted too
                        // rather than guessing which viewers differ.
                        const bool method_accepted = render_materials
                            ? (response.method == "GET" || response.method == "PUT" ||
                               response.method == "POST")
                            : response.method == expected_method;
                        if (method_accepted && registration && viewer_sessions) {
                            authorized_session = viewer_sessions->validate(session_id);
                            authorized = authorized_session &&
                                         authorized_session->destination_region_id == registration->region_id();
                            if (!authorized && authorized_session) {
                                const auto* transit = inbound_transits.authorize(
                                    authorized_session->agent_id, session_id,
                                    std::chrono::steady_clock::now());
                                authorized = transit &&
                                    transit->destination_region_id == registration->region_id();
                            }
                            if (authorized) authorized_agent_id = authorized_session->agent_id;
                        }
                        if (authorized && seed) {
                            // ADR 0032: a client opts into an extension by naming its
                            // capabilities here. A viewer that names none — or sends no
                            // body at all, as Firestorm's login seed does — negotiates
                            // nothing and receives exactly the baseline set.
                            const auto requested_capabilities =
                                homeworldz::viewer::parse_requested_capabilities(
                                    http_request_body(request));
                            response = homeworldz::http::response_for_content(
                                request, 200, "application/llsd+xml",
                                homeworldz::viewer::seed_capability_xml(
                                    region_public_endpoint, grid_public_endpoint, session_id,
                                    capability_visit_id,
                                    homeworldz::viewer::negotiated_extension_capabilities(
                                        homeworldz::viewer::available_region_extensions(),
                                        requested_capabilities)));
                            if (!capability_visit_id.empty())
                                static_cast<void>(capability_arrival_gate.mark_seed_served(
                                    session_id, capability_visit_id));
                        } else if (authorized && event_queue) {
                            if (established_events.insert(session_id).second) {
                                const auto sim_ip_and_port =
                                    simulator_endpoint(region_public_endpoint, region_viewer_port);
                                if (authorized_session && !sim_ip_and_port.empty()) {
                                    enqueue_viewer_event(session_id,
                                        homeworldz::viewer::establish_agent_communication_event_xml({
                                            authorized_session->agent_id, sim_ip_and_port,
                                            region_public_endpoint + "/caps/seed/" + session_id +
                                                (capability_visit_id.empty() ? std::string{} :
                                                    "/" + capability_visit_id)}));
                                } else if (authorized_session) {
                                    // Emitting the event with an unresolved
                                    // address is worse than omitting it: the
                                    // viewer would accept it and then fail
                                    // silently. Say so instead.
                                    established_events.erase(session_id);
                                    std::cerr << "{\"level\":\"error\",\"message\":\"region endpoint did not "
                                                 "resolve to an address\",\"endpoint\":"
                                              << homeworldz::api::json_string(region_public_endpoint)
                                              << "}" << std::endl;
                                }
                            }
                            const auto events = take_viewer_events(session_id);
                            if (!events.empty()) {
                                response = homeworldz::http::response_for_content(
                                    request, 200, "application/llsd+xml",
                                    homeworldz::viewer::event_queue_xml(++event_id, events));
                            } else {
                                pending_event_responses.push_back(PendingEventResponse{
                                    client, std::string(request), session_id,
                                    std::chrono::steady_clock::now() + std::chrono::seconds(20)});
                                response_deferred = true;
                            }
                        } else if (authorized && texture && homeworldz::viewer::parse_uuid(texture->texture)) {
                            try {
                                const auto asset = read_federated_asset(texture->texture);
                                auto body = std::string(
                                    reinterpret_cast<const char*>(asset.data()), asset.size());
                                // A texture extracted from a GLB is canonically
                                // the creator's PNG or JPEG, which this
                                // capability's clients cannot read. Serve the
                                // j2c-texture rendition where the canonical is
                                // not already JPEG2000 - symmetric with GetMesh
                                // serving the legacy rendition of a canonical
                                // GLB, and with the session asset route serving
                                // the modern form of a legacy mesh. Each family
                                // asks for one id and receives what it reads
                                // (ADR 0033 M3).
                                const auto j2c_codestream = asset.size() >= 4 &&
                                    asset[0] == std::byte{0xff} && asset[1] == std::byte{0x4f};
                                const auto jp2_container = asset.size() >= 12 &&
                                    std::memcmp(asset.data() + 4, "jP  ", 4) == 0;
                                if (!j2c_codestream && !jp2_container && viewer_grid) {
                                    if (auto legacy = viewer_grid->fetch_asset_rendition(
                                            texture->texture, "j2c-texture")) {
                                        body = std::move(*legacy);
                                    } else {
                                        // Not converted yet: queue it and answer
                                        // not-yet, the same contract mesh has.
                                        static_cast<void>(viewer_grid->request_asset_rendition(
                                            texture->texture, "j2c-texture"));
                                        throw std::runtime_error("texture rendition pending");
                                    }
                                }
                                response = homeworldz::http::response_for_content(
                                    request, 200, "image/x-j2c", std::move(body));
                            } catch (const std::exception&) {
                                response = homeworldz::http::response_for_content(
                                    request, 404, "application/json",
                                    homeworldz::api::to_json(homeworldz::api::Error{
                                        "asset_not_found", "texture asset was not found"}));
                            }
                        } else if (authorized && viewer_asset &&
                                   homeworldz::viewer::parse_uuid(viewer_asset->asset)) {
                            try {
                                if (viewer_asset->mesh) {
                                    // Mesh fetches get the sl-mesh rendition,
                                    // fetch-through cached: renditions are
                                    // regenerable grid-side data, so memory is
                                    // the right tier here (ADR 0033).
                                    auto cached = mesh_rendition_cache.find(viewer_asset->asset);
                                    if (cached == mesh_rendition_cache.end()) {
                                        auto rendition = viewer_grid
                                            ? viewer_grid->fetch_asset_rendition(
                                                  viewer_asset->asset, "sl-mesh")
                                            : std::nullopt;
                                        if (!rendition) {
                                            // A viewer-uploaded mesh (ADR 0033
                                            // M2) is canonical type-49 with no
                                            // rendition; serve the canonical
                                            // bytes — but only if they really
                                            // are an SL mesh, so a GLB whose
                                            // conversion is still pending
                                            // stays a 404 rather than garbage.
                                            auto canonical = read_federated_asset(viewer_asset->asset);
                                            if (!homeworldz::slmesh::parse(canonical))
                                                throw std::runtime_error("mesh rendition unavailable");
                                            rendition = std::string(
                                                reinterpret_cast<const char*>(canonical.data()),
                                                canonical.size());
                                        }
                                        if (mesh_rendition_cache.size() >= 128)
                                            mesh_rendition_cache.clear();
                                        cached = mesh_rendition_cache.emplace(
                                            viewer_asset->asset, std::move(*rendition)).first;
                                    }
                                    // Viewer mesh loading is ranged: the
                                    // header first (bytes 0..), then each LOD
                                    // at the extent the header named. A full
                                    // 200 to a ranged LOD request hands the
                                    // decompressor the wrong bytes, so honor
                                    // the range with a 206.
                                    const auto range_header = homeworldz::http::request_header_value(
                                        request, "Range");
                                    std::size_t range_offset = 0, range_length = 0;
                                    bool ranged = false;
                                    if (range_header.starts_with("bytes=")) {
                                        const auto dash = range_header.find('-', 6);
                                        if (dash != std::string::npos) {
                                            const auto from = range_header.substr(6, dash - 6);
                                            const auto to = range_header.substr(dash + 1);
                                            char* end = nullptr;
                                            range_offset = std::strtoull(from.c_str(), &end, 10);
                                            if (end != nullptr && *end == 0 && !to.empty()) {
                                                const auto last = std::strtoull(to.c_str(), &end, 10);
                                                if (end != nullptr && *end == 0 && last >= range_offset) {
                                                    range_length = last - range_offset + 1;
                                                    ranged = true;
                                                }
                                            }
                                        }
                                    }
                                    if (ranged)
                                        response = homeworldz::http::response_for_range(
                                            request, "application/vnd.ll.mesh", cached->second,
                                            range_offset, range_length);
                                    else
                                        response = homeworldz::http::response_for_content(
                                            request, 200, "application/vnd.ll.mesh", cached->second);
                                } else {
                                    const auto asset = read_federated_asset(viewer_asset->asset);
                                    response = homeworldz::http::response_for_content(
                                        request, 200, "application/octet-stream",
                                        std::string(reinterpret_cast<const char*>(asset.data()), asset.size()));
                                }
                            } catch (const std::exception&) {
                                response = homeworldz::http::response_for_content(
                                    request, 404, "application/json",
                                    homeworldz::api::to_json(homeworldz::api::Error{
                                        "asset_not_found", "viewer asset was not found"}));
                            }
                        } else if (authorized && simulator_features) {
                            response = homeworldz::http::response_for_content(
                                request, 200, "application/llsd+xml",
                                homeworldz::viewer::simulator_features_xml(
                                    homeworldz::viewer::SimulatorFeatures{
                                        .map_server_url = grid_public_endpoint + "/map/",
                                        .extensions =
                                            homeworldz::viewer::available_region_extensions()}));
                        } else if (authorized && server_release_notes) {
                            // Firestorm's About box reports "Error fetching server
                            // release notes URL" when this capability is absent,
                            // which it was until 2026-08-05.
                            response = homeworldz::http::response_for_redirect(
                                request, release_notes_url);
                        } else if (authorized && mesh_upload_flag) {
                            // The model uploader's per-agent permission
                            // query: every resident may upload mesh on this
                            // grid, so the answer is the same truth
                            // MeshUploadEnabled advertises. Refusals, when
                            // they exist, will be per-account facts from the
                            // grid, not a second copy of the region flag.
                            response = homeworldz::http::response_for_content(
                                request, 200, "application/llsd+xml",
                                "<?xml version=\"1.0\"?><llsd><map>"
                                "<key>mesh_upload_status</key><string>valid</string>"
                                "</map></llsd>");
                        } else if (authorized && render_materials) {
                            // Legacy Blinn-Phong materials. A viewer POSTs the
                            // definitions it wants ids for and GETs definitions
                            // by id; both directions carry one "Zipped" binary
                            // member holding zlib-deflated LLSD binary. Serving
                            // nothing here is what made every materials edit
                            // vanish silently: the viewer had no id to put on a
                            // face and nothing reported a failure.
                            const auto body = http_request_body(request);
                            const auto& method = response.method;
                            std::string reply_error;
                            homeworldz::llsd::Value answer;
                            answer.type = homeworldz::llsd::Value::Type::array;
                            if (method == "GET") {
                                for (const auto& [id, definition] : render_material_cache) {
                                    if (const auto stored = homeworldz::llsd::parse_binary(
                                            std::span<const std::byte>(definition))) {
                                        homeworldz::llsd::Value entry;
                                        entry.type = homeworldz::llsd::Value::Type::map;
                                        homeworldz::llsd::Value id_value;
                                        id_value.type = homeworldz::llsd::Value::Type::binary;
                                        id_value.binary = material_id_bytes(id);
                                        entry.members.emplace_back("ID", std::move(id_value));
                                        entry.members.emplace_back("Material", *stored);
                                        answer.elements.push_back(std::move(entry));
                                    }
                                }
                            } else {
                                // The request is itself a Zipped LLSD document
                                // holding the definitions to register.
                                const auto zipped = zipped_member(body);
                                const auto inflated = zipped
                                    ? homeworldz::llsd::inflate_bytes(*zipped) : std::nullopt;
                                const auto document = inflated
                                    ? homeworldz::llsd::parse_binary(*inflated) : std::nullopt;
                                if (!document) {
                                    reply_error = "the materials request carried no readable"
                                                  " zipped LLSD";
                                } else {
                                    // Definitions are found by what they contain,
                                    // not by where they sit. Firestorm wraps them
                                    // in a "FullMaterialsPerFace" array of
                                    // per-face entries; treating the document as
                                    // either one definition or a flat list parsed
                                    // that envelope *as* a material and registered
                                    // an all-default one while answering 200
                                    // (found live 2026-07-31, and it looked
                                    // exactly like success). Searching by content
                                    // is right for any wrapper without needing to
                                    // know its shape.
                                    const auto definitions =
                                        homeworldz::material::find_materials(*document);
                                    // The shape itself, logged once per request, so
                                    // the next surprise is answered by evidence
                                    // rather than by another guess about the
                                    // envelope.
                                    std::cout << "{\"level\":\"info\",\"message\":\"materials"
                                                 " request\",\"definitions\":"
                                              << definitions.size() << ",\"shape\":"
                                              << homeworldz::api::json_string(
                                                     homeworldz::material::describe(*document))
                                              << "}" << std::endl;
                                    if (definitions.empty()) {
                                        // No definitions means this is a query, not
                                        // a registration: a viewer asking for the
                                        // materials behind ids it found on faces.
                                        // Answer those rather than nothing, since
                                        // an unanswered query is why a relogged
                                        // viewer showed empty pickers.
                                        const auto wanted =
                                            homeworldz::material::find_material_ids(*document);
                                        for (const auto& id : wanted) {
                                            const auto text =
                                                homeworldz::material::format_id(id);
                                            const auto found = render_material_cache.find(text);
                                            if (found == render_material_cache.end()) continue;
                                            if (const auto stored = homeworldz::llsd::parse_binary(
                                                    std::span<const std::byte>(found->second))) {
                                                homeworldz::llsd::Value entry;
                                                entry.type = homeworldz::llsd::Value::Type::map;
                                                homeworldz::llsd::Value id_value;
                                                id_value.type =
                                                    homeworldz::llsd::Value::Type::binary;
                                                id_value.binary.assign(id.begin(), id.end());
                                                entry.members.emplace_back("ID",
                                                                           std::move(id_value));
                                                entry.members.emplace_back("Material", *stored);
                                                answer.elements.push_back(std::move(entry));
                                            }
                                        }
                                        std::cout << "{\"level\":\"info\",\"message\":\"materials"
                                                     " query\",\"requested\":" << wanted.size()
                                                  << ",\"answered\":" << answer.elements.size()
                                                  << "}" << std::endl;
                                    }
                                    bool scene_changed = false;
                                    for (const auto& placement : definitions) {
                                        const auto* definition = placement.definition;
                                        const auto parsed =
                                            homeworldz::material::from_llsd(*definition);
                                        if (!parsed.ok) continue;
                                        // The reading of this format is not
                                        // verified against a viewer, so every key
                                        // we did not recognise is reported. This
                                        // log line is the evidence that either
                                        // confirms the field names or names the
                                        // right ones (render_material.h).
                                        if (!parsed.unknown_keys.empty()) {
                                            std::string keys;
                                            for (const auto& key : parsed.unknown_keys) {
                                                if (!keys.empty()) keys += ",";
                                                keys += key;
                                            }
                                            std::cout << "{\"level\":\"warning\",\"message\":"
                                                         "\"material definition carried keys this"
                                                         " region does not know\",\"keys\":"
                                                      << homeworldz::api::json_string(keys)
                                                      << "}" << std::endl;
                                        }
                                        const auto id = homeworldz::material::identify(
                                            parsed.material);
                                        const auto text = homeworldz::material::format_id(id);
                                        auto stored = homeworldz::llsd::to_binary(
                                            homeworldz::material::to_llsd(parsed.material));
                                        try {
                                            storage->store_render_material(text, stored);
                                        } catch (const std::exception& error) {
                                            std::cout << "{\"level\":\"error\",\"message\":"
                                                         "\"material persistence failed\",\"error\":"
                                                      << homeworldz::api::json_string(error.what())
                                                      << "}" << std::endl;
                                        }
                                        render_material_cache[text] = stored;
                                        homeworldz::llsd::Value entry;
                                        entry.type = homeworldz::llsd::Value::Type::map;
                                        homeworldz::llsd::Value id_value;
                                        id_value.type = homeworldz::llsd::Value::Type::binary;
                                        id_value.binary.assign(id.begin(), id.end());
                                        entry.members.emplace_back("ID", std::move(id_value));
                                        entry.members.emplace_back(
                                            "Material",
                                            homeworldz::material::to_llsd(parsed.material));
                                        answer.elements.push_back(std::move(entry));
                                        std::cout << "{\"level\":\"info\",\"message\":"
                                                     "\"material registered\",\"materialId\":"
                                                  << homeworldz::api::json_string(text)
                                                  << "}" << std::endl;

                                        // Storing the definition was never enough:
                                        // the request names an object and a face,
                                        // and the server is what writes the id into
                                        // that face's TextureEntry. Skipping this is
                                        // why a relog showed empty pickers — the
                                        // material existed and nothing referenced it
                                        // (found live 2026-07-31).
                                        if (!placement.local_id || !placement.face) continue;
                                        auto* target = scene.find(
                                            static_cast<std::uint32_t>(*placement.local_id));
                                        if (target == nullptr) continue;
                                        if (target->owner_id != authorized_agent_id ||
                                            (target->owner_permissions &
                                             homeworldz::scene::permission_modify) == 0)
                                            continue;
                                        const auto face =
                                            static_cast<unsigned>(*placement.face);
                                        if (face >= homeworldz::texture_entry::max_faces) continue;
                                        auto entry_bytes = target->texture_entry;
                                        if (entry_bytes.empty())
                                            entry_bytes = default_prim_texture_entry();
                                        auto decoded = homeworldz::texture_entry::parse(
                                            std::span<const std::byte>(entry_bytes));
                                        if (!decoded) continue;
                                        // A definition naming neither map is the
                                        // viewer asking to remove the material, not
                                        // a material made of nothing — so the face
                                        // goes back to the nil id rather than
                                        // pointing at an empty record.
                                        const bool removing = parsed.material.normal_map.empty() &&
                                                              parsed.material.specular_map.empty();
                                        std::vector<std::byte> face_material(16, std::byte{});
                                        if (!removing)
                                            face_material.assign(id.begin(), id.end());
                                        homeworldz::texture_entry::set_face(
                                            *decoded, homeworldz::texture_entry::material_id, face,
                                            face_material);
                                        auto rewritten =
                                            homeworldz::texture_entry::encode(*decoded);
                                        if (rewritten == target->texture_entry) continue;
                                        target->texture_entry = std::move(rewritten);
                                        scene_changed = true;
                                        std::cout << "{\"level\":\"info\",\"message\":\"material"
                                                     " applied to face\",\"localId\":"
                                                  << *placement.local_id << ",\"face\":" << face
                                                  << ",\"materialId\":"
                                                  << homeworldz::api::json_string(
                                                         removing ? "removed" : text)
                                                  << "}" << std::endl;
                                    }
                                    if (scene_changed) {
                                        try {
                                            storage->save_snapshot(scene);
                                        } catch (const std::exception& error) {
                                            std::cout << "{\"level\":\"error\",\"message\":\"material"
                                                         " face persistence failed\",\"error\":"
                                                      << homeworldz::api::json_string(error.what())
                                                      << "}" << std::endl;
                                        }
                                    }
                                }
                            }
                            if (!reply_error.empty()) {
                                response = homeworldz::http::response_for_content(
                                    request, 400, "application/json",
                                    homeworldz::api::to_json(homeworldz::api::Error{
                                        "invalid_materials_request", reply_error}));
                            } else {
                                response = homeworldz::http::response_for_content(
                                    request, 200, "application/llsd+xml",
                                    zipped_llsd_reply(answer));
                            }
                        } else if (authorized && environment_settings && registration) {
                            response = homeworldz::http::response_for_content(
                                request, 200, "application/llsd+xml",
                                homeworldz::viewer::environment_settings_xml(registration->region_id()));
                        } else if (authorized && remote_parcel) {
                            // Resolve the global parcel UUID at the requested location so
                            // the viewer can show the parcel ID and create landmarks.
                            const auto body = http_request_body(request);
                            std::string parcel_id;
                            if (parcels) {
                                if (const auto location =
                                        homeworldz::viewer::parse_remote_parcel_location(body)) {
                                    if (const auto* parcel = parcels->parcel_at(
                                            static_cast<float>((*location)[0]),
                                            static_cast<float>((*location)[1])))
                                        parcel_id = parcel->global_id;
                                }
                                if (parcel_id.empty() && !parcels->parcels().empty())
                                    parcel_id = parcels->parcels().front().global_id;
                            }
                            response = homeworldz::http::response_for_content(
                                request, 200, "application/llsd+xml",
                                homeworldz::viewer::remote_parcel_reply_xml(parcel_id));
                        } else if (authorized && baked_upload) {
                            static std::atomic<std::uint64_t> upload_id{0};
                            auto base = region_public_endpoint;
                            while (!base.empty() && base.back() == '/') base.pop_back();
                            const auto uploader = base + "/caps/upload-baked-data/" + session_id + '/' +
                                                  std::to_string(++upload_id);
                            response = homeworldz::http::response_for_content(
                                request, 200, "application/llsd+xml",
                                homeworldz::viewer::baked_texture_upload_xml(uploader));
                        } else if (authorized && baked_upload_data) {
                            const auto body = http_request_body(request);
                            if (body.empty()) {
                                response = homeworldz::http::response_for_content(
                                    request, 400, "application/llsd+xml", "<llsd><undef/></llsd>");
                            } else {
                                try {
                                    const auto content = std::span(
                                        reinterpret_cast<const std::byte*>(body.data()), body.size());
                                    const auto asset_id = homeworldz::viewer::random_uuid();
                                    const auto metadata = storage->store_asset(
                                        asset_id, authorized_agent_id, content);
                                    const bool registered = !viewer_grid || (viewer_grid->register_asset(
                                        metadata.viewer_id, metadata.creator_id, metadata.sha256,
                                        metadata.size, region_public_endpoint, true) &&
                                        // Write-through (ADR 0026): the commit's
                                        // fetch-back and this thread cannot meet.
                                        viewer_grid->store_vault_asset(metadata.viewer_id, content));
                                    response = registered
                                        ? homeworldz::http::response_for_content(
                                              request, 200, "application/llsd+xml",
                                              homeworldz::viewer::baked_texture_complete_xml(asset_id))
                                        : homeworldz::http::response_for_content(
                                              request, 500, "application/llsd+xml", "<llsd><undef/></llsd>");
                                    std::cout << "{\"level\":\"info\",\"message\":\"baked texture stored\","
                                                 "\"assetId\":" << homeworldz::api::json_string(asset_id)
                                              << ",\"bytes\":" << body.size() << "}" << std::endl;
                                } catch (const std::exception& error) {
                                    response = homeworldz::http::response_for_content(
                                        request, 500, "application/llsd+xml", "<llsd><undef/></llsd>");
                                    std::cerr << "{\"level\":\"error\",\"message\":\"baked texture upload failed\","
                                                 "\"error\":" << homeworldz::api::json_string(error.what())
                                              << "}" << std::endl;
                                }
                            }
                        } else if (authorized && file_upload) {
                            const auto body = http_request_body(request);
                            // The mesh branch of NewFileAgentInventory
                            // (ADR 0033 M2): a whole-model fee request. The
                            // metadata waits for the upload POST, which
                            // carries the resources alone.
                            const auto fee = homeworldz::mesh_model::parse_fee_request(body);
                            if (fee.mesh_request) {
                                if (!fee.ok) {
                                    response = homeworldz::http::response_for_content(
                                        request, 200, "application/llsd+xml",
                                        homeworldz::mesh_model::error_response_xml(fee.error));
                                    std::cout << "{\"level\":\"info\",\"message\":\"mesh model fee refused\","
                                                 "\"error\":" << homeworldz::api::json_string(fee.error)
                                              << "}" << std::endl;
                                } else {
                                    const auto token = homeworldz::viewer::random_uuid();
                                    pending_mesh_model_uploads.insert_or_assign(token,
                                        PendingMeshModelUpload{session_id, authorized_agent_id,
                                                               fee.request.metadata});
                                    auto base = region_public_endpoint;
                                    while (!base.empty() && base.back() == '/') base.pop_back();
                                    const auto uploader =
                                        base + "/caps/upload-model-data/" + session_id + '/' + token;
                                    response = homeworldz::http::response_for_content(
                                        request, 200, "application/llsd+xml",
                                        homeworldz::mesh_model::fee_response_xml(uploader));
                                    std::cout << "{\"level\":\"info\",\"message\":\"mesh model fee granted\","
                                                 "\"name\":" << homeworldz::api::json_string(
                                                     fee.request.metadata.name)
                                              << ",\"meshes\":" << fee.request.resources.meshes.size()
                                              << ",\"instances\":" << fee.request.resources.instances.size()
                                              << ",\"textures\":" << fee.request.resources.textures.size()
                                              << "}" << std::endl;
                                }
                            } else if (const auto upload =
                                           homeworldz::viewer::parse_new_file_inventory_upload(body);
                                       !upload) {
                                response = homeworldz::http::response_for_content(
                                    request, 400, "application/llsd+xml", "<llsd><undef/></llsd>");
                            } else {
                                const auto token = homeworldz::viewer::random_uuid();
                                PendingInventoryUpload pending{session_id, authorized_agent_id,
                                    homeworldz::viewer::random_uuid(), homeworldz::viewer::random_uuid(), *upload};
                                pending_inventory_uploads.insert_or_assign(token, pending);
                                auto base = region_public_endpoint;
                                while (!base.empty() && base.back() == '/') base.pop_back();
                                const auto uploader = base + "/caps/upload-file-data/" + session_id + '/' + token;
                                response = homeworldz::http::response_for_content(
                                    request, 200, "application/llsd+xml",
                                    homeworldz::viewer::new_file_inventory_upload_xml(uploader));
                            }
                        } else if (authorized && file_upload_data) {
                            const auto pending = pending_inventory_uploads.find(file_upload_data->second);
                            const auto body = http_request_body(request);
                            if (pending == pending_inventory_uploads.end() ||
                                pending->second.session_id != session_id ||
                                pending->second.agent_id != authorized_agent_id) {
                                response = homeworldz::http::response_for_content(
                                    request, 404, "application/llsd+xml", "<llsd><undef/></llsd>");
                            } else if (!homeworldz::viewer::valid_new_file_inventory_upload_content(
                                           pending->second.request, body)) {
                                response = homeworldz::http::response_for_content(
                                    request, 400, "application/llsd+xml", "<llsd><undef/></llsd>");
                            } else {
                                const auto& upload = pending->second;
                                const auto content = std::span(
                                    reinterpret_cast<const std::byte*>(body.data()), body.size());
                                const auto metadata = storage->store_asset(
                                    upload.asset_id, authorized_agent_id, content);
                                const bool asset_registered = viewer_grid && viewer_grid->register_asset(
                                    metadata.viewer_id, metadata.creator_id, metadata.sha256,
                                    metadata.size, region_public_endpoint, true) &&
                                    // Write-through (ADR 0026): see above.
                                    viewer_grid->store_vault_asset(metadata.viewer_id, content);
                                const bool item_created = asset_registered &&
                                    viewer_grid->create_inventory_item(
                                        authorized_agent_id, homeworldz::grid::InventoryItem{
                                            upload.item_id, authorized_agent_id, authorized_agent_id,
                                            upload.request.folder_id, upload.asset_id,
                                            upload.request.asset_type, upload.request.inventory_type,
                                            upload.request.name, upload.request.description, 0,
                                            homeworldz::scene::permission_creator,
                                            homeworldz::scene::permission_creator,
                                            upload.request.everyone_permissions,
                                            upload.request.next_permissions, 0, 0});
                                if (!item_created) {
                                    response = homeworldz::http::response_for_content(
                                        request, 500, "application/llsd+xml", "<llsd><undef/></llsd>");
                                } else {
                                    response = homeworldz::http::response_for_content(
                                        request, 200, "application/llsd+xml",
                                        homeworldz::viewer::new_file_inventory_complete_xml(
                                            upload.item_id, upload.asset_id,
                                            upload.request.everyone_permissions, upload.request.next_permissions));
                                    std::cout << "{\"level\":\"info\",\"message\":\"inventory asset upload stored\","
                                                 "\"assetId\":" << homeworldz::api::json_string(upload.asset_id)
                                              << ",\"itemId\":" << homeworldz::api::json_string(upload.item_id)
                                              << ",\"creatorId\":" << homeworldz::api::json_string(authorized_agent_id)
                                              << ",\"assetType\":" << static_cast<int>(upload.request.asset_type)
                                              << ",\"inventoryType\":" << static_cast<int>(upload.request.inventory_type)
                                              << ",\"bytes\":" << body.size() << "}" << std::endl;
                                    pending_inventory_uploads.erase(pending);
                                }
                            }
                        } else if (authorized && model_upload_data) {
                            // The upload half of the mesh model flow: store
                            // every mesh verbatim as a canonical type-49
                            // asset, every texture as JPEG2000, build the
                            // linkset the instance transforms describe, and
                            // answer with the new object item. Write-through
                            // at every register (ADR 0026): this thread
                            // would be the one serving the commit's
                            // fetch-back.
                            const auto pending = pending_mesh_model_uploads.find(model_upload_data->second);
                            const auto body = http_request_body(request);
                            if (pending == pending_mesh_model_uploads.end() ||
                                pending->second.session_id != session_id ||
                                pending->second.agent_id != authorized_agent_id) {
                                response = homeworldz::http::response_for_content(
                                    request, 404, "application/llsd+xml", "<llsd><undef/></llsd>");
                            } else {
                                const auto metadata = pending->second.metadata;
                                const auto parsed = homeworldz::mesh_model::parse_upload(body);
                                if (!parsed.ok) {
                                    response = homeworldz::http::response_for_content(
                                        request, 200, "application/llsd+xml",
                                        homeworldz::mesh_model::error_response_xml(parsed.error));
                                    std::cout << "{\"level\":\"info\",\"message\":\"mesh model upload refused\","
                                                 "\"error\":" << homeworldz::api::json_string(parsed.error)
                                              << "}" << std::endl;
                                    pending_mesh_model_uploads.erase(pending);
                                } else {
                                    try {
                                        if (!viewer_grid)
                                            throw std::runtime_error("grid connection unavailable");
                                        const auto& resources = parsed.resources;
                                        const auto store_registered =
                                            [&](std::span<const std::byte> content) {
                                            const auto stored = storage->store_asset(
                                                homeworldz::viewer::random_uuid(),
                                                authorized_agent_id, content);
                                            if (!viewer_grid->register_asset(
                                                    stored.viewer_id, stored.creator_id,
                                                    stored.sha256, stored.size,
                                                    region_public_endpoint, true) ||
                                                !viewer_grid->store_vault_asset(
                                                    stored.viewer_id, content))
                                                throw std::runtime_error(
                                                    "asset registration or vault write-through failed");
                                            return stored.viewer_id;
                                        };
                                        std::vector<std::string> mesh_assets;
                                        for (const auto& mesh : resources.meshes) {
                                            mesh_assets.push_back(store_registered(mesh));
                                            // A viewer-authored mesh is canonical
                                            // Second Life mesh, which clients on
                                            // the modern path never learn to read,
                                            // so queue the glTF derivation now
                                            // rather than at first fetch
                                            // (ADR 0033 M2).
                                            static_cast<void>(viewer_grid->request_asset_rendition(
                                                mesh_assets.back(), "gltf"));
                                        }
                                        std::vector<std::optional<homeworldz::viewer::Uuid>> textures;
                                        std::size_t texture_number = 0;
                                        for (const auto& texture_bytes : resources.textures) {
                                            if (texture_bytes.empty()) {
                                                textures.emplace_back();
                                                continue;
                                            }
                                            const auto texture_asset = store_registered(texture_bytes);
                                            textures.push_back(
                                                homeworldz::viewer::parse_uuid(texture_asset));
                                            // The texture also lands in inventory, as
                                            // Second Life's uploader does, so the
                                            // creator can reuse it.
                                            homeworldz::grid::InventoryItem texture_item;
                                            texture_item.item_id = homeworldz::viewer::random_uuid();
                                            texture_item.creator_id = authorized_agent_id;
                                            texture_item.owner_id = authorized_agent_id;
                                            texture_item.folder_id = metadata.texture_folder_id;
                                            texture_item.asset_id = texture_asset;
                                            texture_item.asset_type = 0;
                                            texture_item.inventory_type = 0;
                                            texture_item.name = metadata.name + " - Texture " +
                                                std::to_string(++texture_number);
                                            texture_item.base_permissions =
                                                homeworldz::scene::permission_creator;
                                            texture_item.current_permissions =
                                                homeworldz::scene::permission_creator;
                                            texture_item.everyone_permissions =
                                                metadata.everyone_permissions;
                                            texture_item.next_permissions = metadata.next_permissions;
                                            static_cast<void>(viewer_grid->create_inventory_item(
                                                authorized_agent_id, texture_item));
                                        }
                                        // Faces the model did not texture. Blank rather than
                                        // plywood, for the same reason as the session upload
                                        // path: a creator inspecting an uploaded model reads
                                        // wood grain as a texture that went wrong.
                                        const auto untextured = blank_texture_id();
                                        const auto entity_for =
                                            [&](const homeworldz::mesh_model::Instance& instance) {
                                            homeworldz::scene::Entity entity;
                                            entity.name = instance.name.empty()
                                                ? metadata.name : instance.name;
                                            entity.creator_id = authorized_agent_id;
                                            entity.owner_id = authorized_agent_id;
                                            entity.sculpt_id =
                                                mesh_assets[static_cast<std::size_t>(instance.mesh)];
                                            entity.sculpt_type = 5; // mesh
                                            entity.material = instance.material;
                                            entity.physics_shape_type =
                                                std::min<std::uint8_t>(instance.physics_shape_type, 2);
                                            entity.scale.x = std::clamp(instance.scale[0], 0.001f, 64.0f);
                                            entity.scale.y = std::clamp(instance.scale[1], 0.001f, 64.0f);
                                            entity.scale.z = std::clamp(instance.scale[2], 0.001f, 64.0f);
                                            entity.texture_entry =
                                                homeworldz::mesh_model::instance_texture_entry(
                                                    untextured, instance.faces, textures);
                                            return entity;
                                        };
                                        const auto& root_instance = resources.instances.front();
                                        auto root = entity_for(root_instance);
                                        root.rotation = homeworldz::mesh_model::packed_rotation(
                                            root_instance.rotation);
                                        const auto root_inverse =
                                            homeworldz::mesh_model::quaternion_conjugate(
                                                root_instance.rotation);
                                        std::vector<homeworldz::scene::Entity> children;
                                        for (std::size_t index = 1;
                                             index < resources.instances.size(); ++index) {
                                            const auto& instance = resources.instances[index];
                                            auto child = entity_for(instance);
                                            const auto offset = homeworldz::mesh_model::quaternion_rotate(
                                                root_inverse,
                                                {instance.position[0] - root_instance.position[0],
                                                 instance.position[1] - root_instance.position[1],
                                                 instance.position[2] - root_instance.position[2]});
                                            child.local_position = {offset[0], offset[1], offset[2]};
                                            child.local_rotation = homeworldz::mesh_model::packed_rotation(
                                                homeworldz::mesh_model::quaternion_multiply(
                                                    root_inverse, instance.rotation));
                                            children.push_back(std::move(child));
                                        }
                                        std::vector<const homeworldz::scene::Entity*> child_pointers;
                                        for (const auto& child : children) child_pointers.push_back(&child);
                                        const auto wrapped = homeworldz::asset::serialize_linkset_asset(
                                            root, child_pointers);
                                        const auto object_asset_id = store_registered(std::span(
                                            reinterpret_cast<const std::byte*>(wrapped.data()),
                                            wrapped.size()));
                                        homeworldz::grid::InventoryItem item;
                                        item.item_id = homeworldz::viewer::random_uuid();
                                        item.creator_id = authorized_agent_id;
                                        item.owner_id = authorized_agent_id;
                                        item.folder_id = metadata.folder_id;
                                        item.asset_id = object_asset_id;
                                        item.asset_type = 6;      // object
                                        item.inventory_type = 6;  // object
                                        item.name = metadata.name;
                                        item.description = metadata.description;
                                        item.base_permissions = homeworldz::scene::permission_creator;
                                        item.current_permissions = homeworldz::scene::permission_creator;
                                        item.everyone_permissions = metadata.everyone_permissions;
                                        item.next_permissions = metadata.next_permissions;
                                        if (!viewer_grid->create_inventory_item(authorized_agent_id, item))
                                            throw std::runtime_error("inventory item creation failed");
                                        response = homeworldz::http::response_for_content(
                                            request, 200, "application/llsd+xml",
                                            homeworldz::viewer::new_file_inventory_complete_xml(
                                                item.item_id, object_asset_id,
                                                metadata.everyone_permissions,
                                                metadata.next_permissions));
                                        std::cout << "{\"level\":\"info\",\"message\":\"mesh model uploaded\","
                                                     "\"name\":" << homeworldz::api::json_string(metadata.name)
                                                  << ",\"itemId\":" << homeworldz::api::json_string(item.item_id)
                                                  << ",\"objectAssetId\":"
                                                  << homeworldz::api::json_string(object_asset_id)
                                                  << ",\"creatorId\":"
                                                  << homeworldz::api::json_string(authorized_agent_id)
                                                  << ",\"meshes\":" << mesh_assets.size()
                                                  << ",\"instances\":" << resources.instances.size()
                                                  << ",\"textures\":" << textures.size()
                                                  << "}" << std::endl;
                                        pending_mesh_model_uploads.erase(pending);
                                    } catch (const std::exception& error) {
                                        response = homeworldz::http::response_for_content(
                                            request, 500, "application/llsd+xml",
                                            homeworldz::mesh_model::error_response_xml(
                                                "the region could not store the model"));
                                        std::cerr << "{\"level\":\"error\",\"message\":\"mesh model upload failed\","
                                                     "\"error\":" << homeworldz::api::json_string(error.what())
                                                  << "}" << std::endl;
                                    }
                                }
                            }
                        } else if (authorized && (notecard_update || script_update || gesture_update ||
                                                  task_notecard_update || task_script_update)) {
                            const auto update = homeworldz::viewer::parse_inventory_asset_update(
                                http_request_body(request));
                            const bool task_update = task_notecard_update || task_script_update;
                            const bool script_asset = script_update || task_script_update;
                            const std::int8_t expected_asset_type =
                                notecard_update || task_notecard_update ? 7 : script_asset ? 10 : 21;
                            const std::int8_t expected_inventory_type =
                                notecard_update || task_notecard_update ? 7 : script_asset ? 10 : 20;
                            const auto personal_item = update && !task_update && viewer_grid
                                ? viewer_grid->find_inventory_item(authorized_agent_id, update->item_id)
                                : std::nullopt;
                            const homeworldz::scene::TaskInventoryItem* task_item = nullptr;
                            const homeworldz::scene::Entity* task_entity = nullptr;
                            if (update && task_update) {
                                for (const auto& [id, candidate] : scene.entities()) {
                                    static_cast<void>(id);
                                    if (candidate.object_id == update->task_id) {
                                        task_entity = &candidate;
                                        break;
                                    }
                                }
                                if (task_entity && task_entity->owner_id == authorized_agent_id &&
                                    (task_entity->owner_permissions & homeworldz::scene::permission_modify) != 0) {
                                    const auto found = std::find_if(
                                        task_entity->task_inventory.begin(), task_entity->task_inventory.end(),
                                        [&](const auto& candidate) { return candidate.item_id == update->item_id; });
                                    if (found != task_entity->task_inventory.end()) task_item = &*found;
                                }
                            }
                            const bool valid_target = update &&
                                (script_asset ? !update->target.empty() : update->target.empty()) &&
                                (task_update ? !update->task_id.empty() : update->task_id.empty());
                            const auto valid_item = task_update ?
                                task_item && task_item->asset_type == expected_asset_type &&
                                    task_item->inventory_type == expected_inventory_type &&
                                    (task_item->current_permissions & homeworldz::scene::permission_modify) != 0 :
                                personal_item && personal_item->asset_type == expected_asset_type &&
                                    personal_item->inventory_type == expected_inventory_type &&
                                    (personal_item->current_permissions & homeworldz::scene::permission_modify) != 0;
                            if (!update || !valid_target || !valid_item) {
                                response = homeworldz::http::response_for_content(
                                    request, 400, "application/llsd+xml", "<llsd><undef/></llsd>");
                            } else {
                                const auto token = homeworldz::viewer::random_uuid();
                                pending_inventory_asset_updates.insert_or_assign(token,
                                    PendingInventoryAssetUpdate{session_id, authorized_agent_id,
                                        update->item_id, homeworldz::viewer::random_uuid(),
                                        expected_asset_type, expected_inventory_type,
                                        update->task_id, update->script_running});
                                auto base = region_public_endpoint;
                                while (!base.empty() && base.back() == '/') base.pop_back();
                                const auto uploader = base + "/caps/update-inventory-asset-data/" +
                                    session_id + '/' + token;
                                response = homeworldz::http::response_for_content(
                                    request, 200, "application/llsd+xml",
                                    homeworldz::viewer::inventory_asset_update_upload_xml(uploader));
                            }
                        } else if (authorized && inventory_asset_update_data) {
                            const auto pending = pending_inventory_asset_updates.find(
                                inventory_asset_update_data->second);
                            const auto body = http_request_body(request);
                            if (pending == pending_inventory_asset_updates.end() ||
                                pending->second.session_id != session_id ||
                                pending->second.agent_id != authorized_agent_id) {
                                response = homeworldz::http::response_for_content(
                                    request, 404, "application/llsd+xml", "<llsd><undef/></llsd>");
                            } else if (body.empty() || body.size() > 1024 * 1024) {
                                response = homeworldz::http::response_for_content(
                                    request, 400, "application/llsd+xml", "<llsd><undef/></llsd>");
                                pending_inventory_asset_updates.erase(pending);
                            } else {
                                const auto update = pending->second;
                                bool stored = false;
                                std::optional<homeworldz::script::FalconRezResult> compiled;
                                try {
                                    const auto content = std::span(
                                        reinterpret_cast<const std::byte*>(body.data()), body.size());
                                    const auto metadata = storage->store_asset(
                                        update.asset_id, authorized_agent_id, content);
                                    const bool registered = viewer_grid && viewer_grid->register_asset(
                                        metadata.viewer_id, metadata.creator_id, metadata.sha256,
                                        metadata.size, region_public_endpoint, true) &&
                                        // Write-through (ADR 0026): see above.
                                        viewer_grid->store_vault_asset(metadata.viewer_id, content);
                                    if (registered && update.task_id.empty()) {
                                        stored = viewer_grid->update_inventory_item_asset(
                                            authorized_agent_id, update.item_id, update.asset_id);
                                    } else if (registered) {
                                        homeworldz::scene::Entity* task_entity = nullptr;
                                        for (const auto& [id, candidate] : scene.entities()) {
                                            if (candidate.object_id == update.task_id) {
                                                task_entity = scene.find(id);
                                                break;
                                            }
                                        }
                                        if (task_entity && task_entity->owner_id == authorized_agent_id &&
                                            (task_entity->owner_permissions &
                                                homeworldz::scene::permission_modify) != 0) {
                                            const auto item = std::find_if(
                                                task_entity->task_inventory.begin(),
                                                task_entity->task_inventory.end(),
                                                [&](const auto& candidate) {
                                                    return candidate.item_id == update.item_id &&
                                                        candidate.asset_type == update.asset_type &&
                                                        candidate.inventory_type == update.inventory_type &&
                                                        (candidate.current_permissions &
                                                            homeworldz::scene::permission_modify) != 0;
                                                });
                                            if (item != task_entity->task_inventory.end()) {
                                                const auto previous_asset_id = item->asset_id;
                                                const auto previous_serial = task_entity->task_inventory_serial;
                                                item->asset_id = update.asset_id;
                                                task_entity->task_inventory_serial = previous_serial == 65535
                                                    ? 1
                                                    : static_cast<std::uint16_t>(previous_serial + 1);
                                                try {
                                                    storage->save_snapshot(scene);
                                                    stored = true;
                                                } catch (...) {
                                                    item->asset_id = previous_asset_id;
                                                    task_entity->task_inventory_serial = previous_serial;
                                                    throw;
                                                }
                                            }
                                        }
                                    }
                                } catch (const std::exception& error) {
                                    std::cerr << "{\"level\":\"error\",\"message\":\"inventory asset update failed\",\"error\":"
                                              << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                                }
                                if (stored && update.asset_type == 10 &&
                                    !update.task_id.empty()) {
                                    homeworldz::scene::Entity* task_entity = nullptr;
                                    for (const auto& [id, candidate] : scene.entities()) {
                                        if (candidate.object_id == update.task_id) {
                                            task_entity = scene.find(id);
                                            break;
                                        }
                                    }
                                    if (task_entity) {
                                        compiled = falcon.rez(
                                            {update.asset_id, update.item_id,
                                             task_entity->object_id, task_entity->owner_id},
                                            body, update.script_running);
                                        broadcast_object_update(
                                            *task_entity, std::chrono::steady_clock::now());
                                    }
                                }
                                response = stored
                                    ? homeworldz::http::response_for_content(
                                          request, 200, "application/llsd+xml",
                                          homeworldz::viewer::inventory_asset_update_complete_xml(
                                              update.asset_id, update.asset_type == 10,
                                              compiled && compiled->compiled,
                                              compiled ? compiled->diagnostic : std::string{}))
                                    : homeworldz::http::response_for_content(
                                          request, 500, "application/llsd+xml", "<llsd><undef/></llsd>");
                                std::cout << "{\"level\":" << (stored ? "\"info\"" : "\"warn\"")
                                          << ",\"message\":\"inventory asset update "
                                          << (stored ? "stored" : "rejected") << "\",\"itemId\":"
                                          << homeworldz::api::json_string(update.item_id)
                                          << ",\"assetId\":" << homeworldz::api::json_string(update.asset_id)
                                          << ",\"assetType\":" << static_cast<int>(update.asset_type)
                                          << ",\"taskId\":" << homeworldz::api::json_string(update.task_id)
                                          << ",\"compiled\":"
                                          << (compiled ? (compiled->compiled ? "true" : "false") : "null")
                                          << ",\"running\":"
                                          << (compiled ? (compiled->running ? "true" : "false") : "null")
                                          << ",\"diagnostic\":"
                                          << homeworldz::api::json_string(
                                                 compiled ? compiled->diagnostic : std::string{})
                                          << ",\"bytes\":" << body.size() << "}" << std::endl;
                                pending_inventory_asset_updates.erase(pending);
                            }
                        } else {
                            response = homeworldz::http::response_for_content(
                                request, method_accepted ? 404 : 405,
                                "application/llsd+xml", "<llsd><undef/></llsd>");
                            // A capability request that reaches here matched a path
                            // and then no handler, which is a server-side mistake
                            // wearing a client-side answer: four RenderMaterials
                            // PUTs got 404 this way and read as ordinary traffic in
                            // the log (2026-07-31). The gate list above, the seed
                            // reply and the handler chain are three places that
                            // must agree and no test can compare them, so the
                            // disagreement is made to announce itself instead.
                            std::cout << "{\"level\":\"warning\",\"message\":\"capability request"
                                         " matched no handler\",\"method\":"
                                      << homeworldz::api::json_string(response.method)
                                      << ",\"path\":" << homeworldz::api::json_string(response.path)
                                      << ",\"authorized\":" << (authorized ? "true" : "false")
                                      << ",\"methodAccepted\":" << (method_accepted ? "true" : "false")
                                      << "}" << std::endl;
                        }
                    }
                    if (!response_deferred) {
                        static_cast<void>(send_all(client, response.content));
                        finish_http_response(client);
                        std::cout << "{\"level\":\"info\",\"message\":\"http request\",\"requestId\":"
                                  << homeworldz::api::json_string(response.request_id)
                                  << ",\"method\":" << homeworldz::api::json_string(response.method)
                                  << ",\"path\":" << homeworldz::api::json_string(response.path)
                                  << ",\"status\":" << response.status_code << "}" << std::endl;
                    }
                }
                if (!response_deferred) close_socket(client);
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (ready > 0 && FD_ISSET(viewer_server, &readable)) {
            constexpr std::size_t max_viewer_packets_per_tick = 256;
            for (std::size_t packet_index = 0; packet_index < max_viewer_packets_per_tick; ++packet_index) {
                if (packet_index > 0) {
                    fd_set immediately_readable;
                    FD_ZERO(&immediately_readable);
                    FD_SET(viewer_server, &immediately_readable);
                    timeval no_wait{0, 0};
                    if (select(static_cast<int>(viewer_server) + 1, &immediately_readable,
                               nullptr, nullptr, &no_wait) <= 0)
                        break;
                }
                std::array<std::byte, 65535> datagram{};
                sockaddr_in sender{};
                socket_length sender_size = sizeof(sender);
                const auto received = recvfrom(viewer_server, reinterpret_cast<char*>(datagram.data()),
                                               static_cast<int>(datagram.size()), 0,
                                               reinterpret_cast<sockaddr*>(&sender), &sender_size);
                const auto endpoint = udp_endpoint(sender);
                if (received > 0 && !endpoint.empty()) {
                const auto packet = circuits.receive(
                     endpoint, std::span<const std::byte>(datagram.data(), static_cast<std::size_t>(received)), now);
                for (const auto& replaced : circuits.take_replaced()) {
                    // A newer login took over this account. The old circuit is
                    // already gone from the registry, so frame a standalone
                    // KickUser datagram (unreliable, one-shot) telling the old
                    // viewer why it is being disconnected.
                    homeworldz::viewer::Packet kick_packet;
                    kick_packet.payload = homeworldz::viewer::encode_kick_user(
                        replaced.identity.agent_id, replaced.identity.session_id,
                        "You have logged in from another location.");
                    if (!kick_packet.payload.empty())
                        static_cast<void>(send_udp(viewer_server, replaced.endpoint,
                            homeworldz::viewer::encode_packet(kick_packet)));
                    clear_viewer_endpoint(replaced.endpoint,
                        homeworldz::viewer::format_uuid(replaced.identity.session_id));
                    std::cout << "{\"level\":\"info\",\"message\":\"stale viewer circuit replaced\",\"endpoint\":"
                              << homeworldz::api::json_string(replaced.endpoint) << "}" << std::endl;
                }
                if (packet) {
                    if (homeworldz::viewer::decode_complete_ping_check(packet->payload)) {
                        // Pong: the viewer answered our StartPingCheck, so the
                        // connection is alive. (Liveness is tracked by ping
                        // replies, not general activity — an idle-but-connected
                        // viewer still answers pings.)
                        if (auto live = avatars.find(endpoint); live != avatars.end())
                            live->second.last_pong = now;
                    }
                    const auto identity = circuits.identity(endpoint);
                    if (identity && homeworldz::viewer::decode_use_circuit_code(packet->payload)) {
                        handshake_replies.erase(endpoint);
                        static_cast<void>(send_region_handshake(endpoint, identity->agent_id));
                    } else if (identity) {
                        if (const auto ping_id = homeworldz::viewer::decode_start_ping_check(packet->payload)) {
                            if (const auto pong = circuits.send(endpoint,
                                    homeworldz::viewer::encode_complete_ping_check(*ping_id), false, now))
                                static_cast<void>(send_udp(viewer_server, endpoint, *pong));
                        }
                        if (homeworldz::viewer::is_economy_data_request(packet->payload)) {
                            if (const auto economy = circuits.send(endpoint,
                                    homeworldz::viewer::encode_economy_data(), true, now, true))
                                static_cast<void>(send_udp(viewer_server, endpoint, *economy));
                        }
                        const auto available_map_regions = [&] {
                            std::vector<homeworldz::viewer::MapBlock> regions;
                            const auto map_image_id = homeworldz::viewer::parse_uuid(
                                default_map_tile_asset_id).value_or(homeworldz::viewer::Uuid{});
                            const auto add_map_region = [&](int grid_x, int grid_y,
                                                            const std::string& name, int size_x,
                                                            int size_y, std::size_t agents) {
                                if (grid_x < 0 || grid_x > 65535 || grid_y < 0 || grid_y > 65535) return;
                                if (std::any_of(regions.begin(), regions.end(), [&](const auto& block) {
                                        return block.x == grid_x && block.y == grid_y; })) return;
                                regions.push_back(homeworldz::viewer::MapBlock{
                                    static_cast<std::uint16_t>(grid_x),
                                    static_cast<std::uint16_t>(grid_y), name,
                                    13, 0, 20,
                                    static_cast<std::uint8_t>((std::min)(agents, std::size_t{255})),
                                    map_image_id, static_cast<std::uint16_t>(size_x),
                                    static_cast<std::uint16_t>(size_y)});
                            };
                            add_map_region(region_grid_x, region_grid_y, region_name,
                                           region_size_x, region_size_y, avatars.size());
                            refresh_grid_topology();
                            for (const auto& placement : grid_topology) {
								if (!placement.online) continue;
                                add_map_region(placement.grid_x, placement.grid_y, placement.name,
                                               placement.size_x, placement.size_y, 0);
                            }
                            // If the grid is unreachable the neighbors are still
                            // known here, and a four-region map beats a blank one.
                            for (const auto& neighbor : region_neighbors) {
								if (!neighbor.online) continue;
                                add_map_region(neighbor.grid_x, neighbor.grid_y, neighbor.name,
                                               neighbor.size_x, neighbor.size_y, 0);
                            }
                            return regions;
                        };
                        if (const auto request =
                                homeworldz::viewer::decode_map_block_request(packet->payload);
                            request && request->agent_id == identity->agent_id &&
                            request->session_id == identity->session_id) {
                            auto regions = available_map_regions();
                            std::erase_if(regions, [&](const auto& region) {
                                return region.x < request->min_x || region.x > request->max_x ||
                                       region.y < request->min_y || region.y > request->max_y;
                            });
                            auto response = homeworldz::viewer::encode_map_block_reply(
                                identity->agent_id, request->flags, regions);
                            if (!response.empty())
                                if (const auto outgoing = circuits.send(
                                        endpoint, std::move(response), true, now))
                                    static_cast<void>(send_udp(viewer_server, endpoint, *outgoing));
                        }
                        if (const auto request =
                                homeworldz::viewer::decode_map_name_request(packet->payload);
                            request && request->agent_id == identity->agent_id &&
                            request->session_id == identity->session_id) {
                            auto lowercase = [](std::string value) {
                                std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
                                    return static_cast<char>(std::tolower(character));
                                });
                                return value;
                            };
                            const auto prefix = lowercase(request->name);
                            auto regions = available_map_regions();
                            std::erase_if(regions, [&](const auto& region) {
                                return !lowercase(region.name).starts_with(prefix);
                            });
                            auto response = homeworldz::viewer::encode_map_block_reply(
                                identity->agent_id, request->flags, regions);
                            if (!response.empty())
                                if (const auto outgoing = circuits.send(
                                        endpoint, std::move(response), true, now))
                                    static_cast<void>(send_udp(viewer_server, endpoint, *outgoing));
                        }
                        if (const auto request =
                                homeworldz::viewer::decode_parcel_properties_request(packet->payload);
                            request && request->agent_id == identity->agent_id &&
                            request->session_id == identity->session_id) {
                            const float mid_x = (request->west + request->east) / 2.0F;
                            const float mid_y = (request->south + request->north) / 2.0F;
                            const auto* parcel = parcels->parcel_at(mid_x, mid_y);
                            if (parcel == nullptr) parcel = parcels->parcel_at(request->west, request->south);
                            const auto session_id = homeworldz::viewer::format_uuid(identity->session_id);
                            if (parcel != nullptr)
                                send_parcel_properties(session_id, *parcel,
                                    homeworldz::parcel::result_single, request->sequence_id,
                                    request->snap_selection);
                            // Deliver the colored parcel-boundary overlay once per
                            // session, on the viewer's initial parcel sweep.
                            if (parcel_overlay_sent.insert(session_id).second)
                                send_parcel_overlay(endpoint,
                                    homeworldz::viewer::format_uuid(identity->agent_id), now);
                        }
                        if (const auto request =
                                homeworldz::viewer::decode_parcel_properties_request_by_id(packet->payload);
                            request && request->agent_id == identity->agent_id &&
                            request->session_id == identity->session_id) {
                            const auto* parcel = parcels->find_by_local_id(request->local_id);
                            const auto session_id = homeworldz::viewer::format_uuid(identity->session_id);
                            if (parcel != nullptr)
                                send_parcel_properties(session_id, *parcel,
                                    homeworldz::parcel::result_single, request->sequence_id, false);
                        }
                        if (const auto update =
                                homeworldz::viewer::decode_parcel_properties_update(packet->payload);
                            update && update->agent_id == identity->agent_id &&
                            update->session_id == identity->session_id) {
                            auto* parcel = parcels->find_by_local_id(update->local_id);
                            const auto agent = homeworldz::viewer::format_uuid(identity->agent_id);
                            const bool authorized = parcel != nullptr &&
                                (parcel->owner_id == agent ||
                                 is_estate_manager(agent));
                            if (authorized) {
                                // Preserve server-managed flags the viewer must not toggle.
                                constexpr std::uint32_t preserved =
                                    homeworldz::parcel::flag_linden_home |
                                    homeworldz::parcel::flag_for_sale_objects;
                                parcel->flags = (update->parcel_flags & ~preserved) |
                                                (parcel->flags & preserved);
                                parcel->sale_price = update->sale_price;
                                parcel->name = update->name;
                                parcel->description = update->description;
                                parcel->music_url = update->music_url;
                                parcel->media_url = update->media_url;
                                parcel->media_id = homeworldz::viewer::format_uuid(update->media_id);
                                parcel->media_auto_scale = update->media_auto_scale;
                                parcel->group_id = homeworldz::viewer::format_uuid(update->group_id);
                                parcel->pass_price = update->pass_price;
                                parcel->pass_hours = update->pass_hours;
                                parcel->category = static_cast<std::int8_t>(update->category);
                                parcel->auth_buyer_id =
                                    homeworldz::viewer::format_uuid(update->auth_buyer_id);
                                parcel->snapshot_id =
                                    homeworldz::viewer::format_uuid(update->snapshot_id);
                                parcel->user_location = {update->user_location[0],
                                    update->user_location[1], update->user_location[2]};
                                parcel->user_look_at = {update->user_look_at[0],
                                    update->user_look_at[1], update->user_look_at[2]};
                                parcel->landing_type = update->landing_type;
                                persist_parcels();
                                broadcast_parcel_overlay(now);
                                const auto session_id =
                                    homeworldz::viewer::format_uuid(identity->session_id);
                                send_parcel_properties(session_id, *parcel,
                                    homeworldz::parcel::result_single,
                                    homeworldz::parcel::selected_parcel_sequence_id, false);
                            }
                        }
                        if (const auto divide = homeworldz::viewer::decode_parcel_divide(packet->payload);
                            divide && divide->agent_id == identity->agent_id &&
                            divide->session_id == identity->session_id) {
                            const auto agent = homeworldz::viewer::format_uuid(identity->agent_id);
                            const auto* covered = parcels->parcel_covering(
                                divide->west, divide->south, divide->east, divide->north);
                            const bool authorized = covered != nullptr &&
                                (covered->owner_id == agent ||
                                 is_estate_manager(agent));
                            if (authorized) {
                                const auto carved = parcels->divide(divide->west, divide->south,
                                    divide->east, divide->north, homeworldz::viewer::random_uuid(),
                                    agent, covered->claim_date);
                                if (carved) {
                                    persist_parcels();
                                    broadcast_parcel_overlay(now);
                                    const auto session_id =
                                        homeworldz::viewer::format_uuid(identity->session_id);
                                    if (const auto* fresh = parcels->find_by_local_id(*carved))
                                        send_parcel_properties(session_id, *fresh,
                                            homeworldz::parcel::result_single,
                                            homeworldz::parcel::selected_parcel_sequence_id, true);
                                }
                            }
                        }
                        if (const auto join = homeworldz::viewer::decode_parcel_join(packet->payload);
                            join && join->agent_id == identity->agent_id &&
                            join->session_id == identity->session_id) {
                            const auto agent = homeworldz::viewer::format_uuid(identity->agent_id);
                            const bool manager = is_estate_manager(agent);
                            const auto merged = parcels->join(join->west, join->south, join->east,
                                join->north, manager && !region_owner_id.empty() ?
                                    std::string_view{region_owner_id} : std::string_view{agent});
                            if (merged) {
                                persist_parcels();
                                broadcast_parcel_overlay(now);
                                const auto session_id =
                                    homeworldz::viewer::format_uuid(identity->session_id);
                                if (const auto* fresh = parcels->find_by_local_id(*merged))
                                    send_parcel_properties(session_id, *fresh,
                                        homeworldz::parcel::result_single,
                                        homeworldz::parcel::selected_parcel_sequence_id, true);
                            }
                        }
                        if (const auto request =
                                homeworldz::viewer::decode_parcel_access_list_request(packet->payload);
                            request && request->agent_id == identity->agent_id &&
                            request->session_id == identity->session_id) {
                            const auto* parcel = parcels->find_by_local_id(request->local_id);
                            if (parcel != nullptr) {
                                homeworldz::viewer::ParcelAccessListReply reply;
                                reply.agent_id = identity->agent_id;
                                reply.sequence_id = request->sequence_id;
                                reply.flags = request->flags;
                                reply.local_id = request->local_id;
                                for (const auto& entry : parcel->access) {
                                    if ((entry.flags & request->flags) == 0) continue;
                                    homeworldz::viewer::ParcelAccessListEntry wire;
                                    if (const auto id = homeworldz::viewer::parse_uuid(entry.agent_id))
                                        wire.id = *id;
                                    wire.time = entry.time;
                                    wire.flags = entry.flags;
                                    reply.entries.push_back(wire);
                                }
                                auto response =
                                    homeworldz::viewer::encode_parcel_access_list_reply(reply);
                                if (const auto outgoing = circuits.send(
                                        endpoint, std::move(response), true, now))
                                    static_cast<void>(send_udp(viewer_server, endpoint, *outgoing));
                            }
                        }
                        if (const auto update =
                                homeworldz::viewer::decode_parcel_access_list_update(packet->payload);
                            update && update->agent_id == identity->agent_id &&
                            update->session_id == identity->session_id) {
                            auto* parcel = parcels->find_by_local_id(update->local_id);
                            const auto agent = homeworldz::viewer::format_uuid(identity->agent_id);
                            const bool authorized = parcel != nullptr &&
                                (parcel->owner_id == agent ||
                                 is_estate_manager(agent));
                            if (authorized) {
                                // Replace the entries carrying the requested access flags.
                                const std::uint32_t which = update->flags &
                                    (homeworldz::parcel::access_allowed | homeworldz::parcel::access_ban);
                                std::erase_if(parcel->access, [&](const auto& entry) {
                                    return (entry.flags & which) != 0;
                                });
                                for (const auto& wire : update->entries) {
                                    const auto id = homeworldz::viewer::format_uuid(wire.id);
                                    if (id == "00000000-0000-0000-0000-000000000000") continue;
                                    parcel->access.push_back({id, wire.time, which});
                                }
                                persist_parcels();
                            }
                        }
                        if (const auto request =
                                homeworldz::viewer::decode_parcel_object_owners_request(packet->payload);
                            request && request->agent_id == identity->agent_id &&
                            request->session_id == identity->session_id) {
                            const auto* parcel = parcels->find_by_local_id(request->local_id);
                            if (parcel != nullptr) {
                                std::unordered_set<std::string> online;
                                for (const auto& [candidate_endpoint, candidate] : avatars) {
                                    static_cast<void>(candidate_endpoint);
                                    online.insert(candidate.user_id);
                                }
                                // owner id -> prim count
                                std::unordered_map<std::string, std::int32_t> counts;
                                for (const auto& [root_id, entity] : scene.entities()) {
                                    if (entity.object_id.empty() || entity.temporary ||
                                        entity.parent_id != 0)
                                        continue;
                                    const auto* at = parcels->parcel_at(
                                        static_cast<float>(entity.position.x),
                                        static_cast<float>(entity.position.y));
                                    if (at == nullptr || at->local_id != parcel->local_id) continue;
                                    std::int32_t prims = 1;
                                    for (const auto& [child_id, child] : scene.entities()) {
                                        static_cast<void>(child_id);
                                        if (child.parent_id == root_id) ++prims;
                                    }
                                    counts[entity.owner_id] += prims;
                                }
                                std::vector<homeworldz::viewer::ParcelObjectOwner> owners;
                                for (const auto& [owner_id, count] : counts) {
                                    homeworldz::viewer::ParcelObjectOwner owner;
                                    if (const auto id = homeworldz::viewer::parse_uuid(owner_id))
                                        owner.owner_id = *id;
                                    owner.count = count;
                                    owner.online = online.count(owner_id) != 0;
                                    owners.push_back(owner);
                                }
                                auto response =
                                    homeworldz::viewer::encode_parcel_object_owners_reply(owners);
                                if (const auto outgoing = circuits.send(
                                        endpoint, std::move(response), true, now, true))
                                    static_cast<void>(send_udp(viewer_server, endpoint, *outgoing));
                            }
                        }
                        if (const auto select =
                                homeworldz::viewer::decode_parcel_select_objects(packet->payload);
                            select && select->agent_id == identity->agent_id &&
                            select->session_id == identity->session_id) {
                            const auto* parcel = parcels->find_by_local_id(select->local_id);
                            if (parcel != nullptr) {
                                std::vector<std::uint32_t> selected;
                                for (const auto& [root_id, entity] : scene.entities()) {
                                    if (entity.object_id.empty() || entity.temporary ||
                                        entity.parent_id != 0)
                                        continue;
                                    const auto* at = parcels->parcel_at(
                                        static_cast<float>(entity.position.x),
                                        static_cast<float>(entity.position.y));
                                    if (at == nullptr || at->local_id != parcel->local_id) continue;
                                    const bool owned_by_parcel = entity.owner_id == parcel->owner_id;
                                    const bool match =
                                        ((select->return_type & homeworldz::viewer::object_return_owner) &&
                                         owned_by_parcel) ||
                                        ((select->return_type & homeworldz::viewer::object_return_other) &&
                                         !owned_by_parcel);
                                    if (match) selected.push_back(static_cast<std::uint32_t>(root_id));
                                }
                                for (auto& packet_bytes :
                                     homeworldz::viewer::encode_force_object_select(selected))
                                    if (const auto outgoing = circuits.send(
                                            endpoint, std::move(packet_bytes), true, now, true))
                                        static_cast<void>(send_udp(viewer_server, endpoint, *outgoing));
                            }
                        }
                        if (const auto ret =
                                homeworldz::viewer::decode_parcel_return_objects(packet->payload);
                            ret && ret->agent_id == identity->agent_id &&
                            ret->session_id == identity->session_id) {
                            auto* parcel = parcels->find_by_local_id(ret->local_id);
                            const auto agent = homeworldz::viewer::format_uuid(identity->agent_id);
                            const bool authorized = parcel != nullptr &&
                                (parcel->owner_id == agent ||
                                 is_estate_manager(agent));
                            if (authorized) {
                                std::unordered_set<std::string> listed_tasks;
                                for (const auto& id : ret->task_ids)
                                    listed_tasks.insert(homeworldz::viewer::format_uuid(id));
                                std::vector<homeworldz::scene::EntityId> roots;
                                for (const auto& [root_id, entity] : scene.entities()) {
                                    if (entity.object_id.empty() || entity.temporary ||
                                        entity.parent_id != 0)
                                        continue;
                                    const auto* at = parcels->parcel_at(
                                        static_cast<float>(entity.position.x),
                                        static_cast<float>(entity.position.y));
                                    if (at == nullptr || at->local_id != parcel->local_id) continue;
                                    const bool owned_by_parcel = entity.owner_id == parcel->owner_id;
                                    const bool match =
                                        ((ret->return_type & homeworldz::viewer::object_return_owner) &&
                                         owned_by_parcel) ||
                                        ((ret->return_type & homeworldz::viewer::object_return_other) &&
                                         !owned_by_parcel) ||
                                        ((ret->return_type & homeworldz::viewer::object_return_list) &&
                                         listed_tasks.count(entity.object_id) != 0);
                                    if (match) roots.push_back(root_id);
                                }
                                std::vector<std::uint32_t> removed_ids;
                                for (const auto root_id : roots)
                                    return_object_to_owner(root_id, removed_ids, now);
                                if (!removed_ids.empty()) {
                                    try {
                                        storage->save_snapshot(scene);
                                    } catch (const std::exception& error) {
                                        std::cout << "{\"level\":\"error\",\"message\":"
                                                     "\"parcel return persistence failed\",\"error\":"
                                                  << homeworldz::api::json_string(error.what()) << "}"
                                                  << std::endl;
                                    }
                                    for (const auto entity_id : removed_ids)
                                        remove_physics_object(entity_id);
                                    const auto kill = homeworldz::viewer::encode_kill_object(removed_ids);
                                    deliver_to_embodied(session_kill_many(removed_ids));
                                    for (const auto& [recipient_endpoint, recipient] : avatars) {
                                        static_cast<void>(recipient);
                                        if (const auto outgoing = circuits.send(
                                                recipient_endpoint, kill, true, now))
                                            static_cast<void>(send_udp(
                                                viewer_server, recipient_endpoint, *outgoing));
                                    }
                                    std::cout << "{\"level\":\"info\",\"message\":\"parcel objects returned\","
                                                 "\"parcel\":" << ret->local_id << ",\"removed\":"
                                              << removed_ids.size() << "}" << std::endl;
                                }
                            }
                        }
                        if (const auto request =
                                homeworldz::viewer::decode_request_region_info(packet->payload);
                            request && request->agent_id == identity->agent_id &&
                            request->session_id == identity->session_id) {
                            homeworldz::viewer::RegionInfoReply reply;
                            reply.agent_id = identity->agent_id;
                            reply.session_id = identity->session_id;
                            reply.sim_name = region_name;
                            reply.estate_id = region_estate ?
                                static_cast<std::uint32_t>(region_estate->id) : 0;
                            reply.parent_estate_id = region_estate ?
                                static_cast<std::uint32_t>(region_estate->parent_estate_id) : 1;
                            reply.region_flags = region_flags();
                            reply.region_flags_extended = region_flags();
                            // SimAccess: PG(13)/Mature(21)/Adult(42) from region maturity.
                            reply.sim_access = region_size_x == 0 ? 13 :
                                (region_maturity == 1 ? 21 : (region_maturity >= 2 ? 42 : 13));
                            reply.billable_factor = region_estate ?
                                static_cast<float>(region_estate->billable_factor) : 0.0F;
                            reply.price_per_meter = region_estate ? region_estate->price_per_meter : 0;
                            reply.use_estate_sun = region_estate ? region_estate->use_global_time : true;
                            reply.sun_hour = region_estate ?
                                static_cast<float>(region_estate->sun_hour) : 0.0F;
                            reply.water_height = static_cast<float>(region_settings.water_height);
                            // Announced *and* enforced now. These were the struct's
                            // defaults and nothing read them, so the form's fields
                            // were decoration.
                            reply.terrain_raise_limit =
                                static_cast<float>(region_settings.terrain_raise);
                            reply.terrain_lower_limit =
                                static_cast<float>(region_settings.terrain_lower);
                            // A region may override the estate's sun; while it
                            // defers, the estate's values are the answer.
                            if (!region_settings.use_estate_sun) {
                                reply.use_estate_sun = false;
                                reply.sun_hour = static_cast<float>(region_settings.sun_hour);
                            }
                            auto response = homeworldz::viewer::encode_region_info(reply);
                            if (!response.empty())
                                if (const auto outgoing = circuits.send(
                                        endpoint, std::move(response), true, now, true))
                                    static_cast<void>(send_udp(viewer_server, endpoint, *outgoing));
                        }
                        if (const auto covenant =
                                homeworldz::viewer::decode_estate_covenant_request(packet->payload);
                            covenant && covenant->agent_id == identity->agent_id &&
                            covenant->session_id == identity->session_id) {
                            homeworldz::viewer::EstateCovenantReply reply;
                            reply.estate_name = region_estate && !region_estate->name.empty()
                                ? region_estate->name : region_name;
                            const auto owner_id = region_estate && !region_estate->owner_id.empty()
                                ? region_estate->owner_id : region_owner_id;
                            if (const auto owner = homeworldz::viewer::parse_uuid(owner_id))
                                reply.estate_owner_id = *owner;
                            auto response = homeworldz::viewer::encode_estate_covenant_reply(reply);
                            if (const auto outgoing = circuits.send(
                                    endpoint, std::move(response), true, now, true))
                                static_cast<void>(send_udp(viewer_server, endpoint, *outgoing));
                        }
                        if (const auto estate_message =
                                homeworldz::viewer::decode_estate_owner_message(packet->payload);
                            estate_message && estate_message->agent_id == identity->agent_id &&
                            estate_message->session_id == identity->session_id) {
                            const auto agent = homeworldz::viewer::format_uuid(identity->agent_id);
                            const auto to_u32 = [](const std::string& value) {
                                std::uint32_t parsed = 0;
                                std::from_chars(value.data(), value.data() + value.size(), parsed);
                                return parsed;
                            };
                            const bool manager = is_estate_manager(agent);
                            const bool owner = (!region_owner_id.empty() && region_owner_id == agent) ||
                                (region_estate && region_estate->owner_id == agent);
                            const auto& method = estate_message->method;
                            if (method == "getinfo") {
                                send_estate_detail(endpoint, identity->agent_id,
                                                   estate_message->invoice, now);
                            } else if (method == "estateaccessdelta" && manager &&
                                       estate_message->params.size() >= 3 && estate_client &&
                                       registration) {
                                const auto command = to_u32(estate_message->params[1]);
                                const auto& target = estate_message->params[2];
                                int role = -1;
                                bool present = true;
                                bool allowed_op = manager;
                                if (command & homeworldz::viewer::estate_access_add_allowed) { role = 1; present = true; }
                                else if (command & homeworldz::viewer::estate_access_remove_allowed) { role = 1; present = false; }
                                else if (command & homeworldz::viewer::estate_access_add_group) { role = 2; present = true; }
                                else if (command & homeworldz::viewer::estate_access_remove_group) { role = 2; present = false; }
                                else if (command & homeworldz::viewer::estate_access_ban_user) { role = 3; present = true; }
                                else if (command & homeworldz::viewer::estate_access_unban_user) { role = 3; present = false; }
                                else if (command & homeworldz::viewer::estate_access_add_manager) { role = 0; present = true; allowed_op = owner; }
                                else if (command & homeworldz::viewer::estate_access_remove_manager) { role = 0; present = false; allowed_op = owner; }
                                if (role >= 0 && allowed_op) {
                                    if (const auto updated = estate_client->set_estate_member(
                                            registration->region_id(), target, role, present))
                                        region_estate = *updated;
                                    if ((command & homeworldz::viewer::estate_access_no_reply) == 0)
                                        send_estate_detail(endpoint, identity->agent_id,
                                                           estate_message->invoice, now);
                                }
                            } else if (method == "estatechangeinfo" && manager &&
                                       estate_message->params.size() >= 3 && estate_client &&
                                       registration) {
                                const auto param1 = to_u32(estate_message->params[1]);
                                const auto param2 = to_u32(estate_message->params[2]);
                                homeworldz::grid::EstateSettingsPatch patch;
                                patch.flags = param1;
                                patch.public_access =
                                    (param1 & homeworldz::viewer::estate_flag_public_access) != 0;
                                patch.fixed_sun = (param1 & homeworldz::viewer::estate_flag_fixed_sun) != 0;
                                patch.use_global_time = param2 == 0;
                                patch.sun_hour = param2 == 0 ? 0.0 :
                                    (static_cast<double>(param2) - 0x1800) / 1024.0;
                                if (const auto updated = estate_client->update_estate_settings(
                                        registration->region_id(), patch))
                                    region_estate = *updated;
                                send_estate_detail(endpoint, identity->agent_id,
                                                   estate_message->invoice, now);
                            } else if (method == "texturedetail" ||
                                       method == "textureheights" ||
                                       method == "texturecommit") {
                              if (!manager) {
                                // Said as a refusal, not left to the no-handler
                                // warning below. "Not permitted" and "not
                                // implemented" look identical from the viewer -
                                // the tab simply fails to stick - so an operator
                                // applying from the wrong avatar would read the
                                // generic warning as the feature being absent.
                                std::cout << "{\"level\":\"warning\",\"message\":"
                                             "\"terrain change refused\",\"method\":"
                                          << homeworldz::api::json_string(method)
                                          << ",\"by\":" << homeworldz::api::json_string(agent)
                                          << ",\"reason\":\"not an estate manager or owner\"}"
                                          << std::endl;
                              } else {
                                // The viewer's Region/Estate -> Terrain tab. It sends
                                // the three in order on Apply: the four texture ids,
                                // the four per-corner elevation pairs, then a bare
                                // commit. Staged and applied together, because the
                                // viewer's own sequence is stage-stage-commit and a
                                // region that applied each as it arrived would hold a
                                // half-changed terrain for as long as the packets took
                                // — and would keep it forever if the commit were lost.
                                if (method == "texturedetail") {
                                    for (const auto& parameter : estate_message->params) {
                                        // "<layer> <uuid>"
                                        const auto space = parameter.find(' ');
                                        if (space == std::string::npos) continue;
                                        const auto layer = to_u32(parameter.substr(0, space));
                                        auto id = parameter.substr(space + 1);
                                        // Wire strings are NUL-terminated, so the
                                        // trailing byte is part of the parameter.
                                        while (!id.empty() &&
                                               (id.back() == char{0} || id.back() == ' '))
                                            id.pop_back();
                                        // The null id parses fine and would leave
                                        // the ground untextured with nothing said,
                                        // which is the silent-failure shape this
                                        // project keeps meeting. A cleared picker
                                        // is refused and named.
                                        if (id == "00000000-0000-0000-0000-000000000000") {
                                            std::cout << "{\"level\":\"warning\",\"message\":"
                                                         "\"terrain layer id is null\",\"layer\":"
                                                      << layer << "}" << std::endl;
                                            continue;
                                        }
                                        if (layer < 4 && homeworldz::viewer::parse_uuid(id))
                                            pending_terrain_layers.assets[layer] = std::move(id);
                                    }
                                } else if (method == "textureheights") {
                                    for (const auto& parameter : estate_message->params) {
                                        // "<corner> <startHeight> <heightRange>" - a
                                        // start and a span, not two bounds.
                                        std::uint32_t corner = 0;
                                        float low = 0.0F;
                                        float high = 0.0F;
                                        const char* cursor = parameter.data();
                                        const char* end = cursor + parameter.size();
                                        auto scanned = std::from_chars(cursor, end, corner);
                                        if (scanned.ec != std::errc{}) continue;
                                        cursor = scanned.ptr;
                                        while (cursor != end && *cursor == ' ') ++cursor;
                                        auto low_scan = std::from_chars(cursor, end, low);
                                        if (low_scan.ec != std::errc{}) continue;
                                        cursor = low_scan.ptr;
                                        while (cursor != end && *cursor == ' ') ++cursor;
                                        if (std::from_chars(cursor, end, high).ec != std::errc{})
                                            continue;
                                        // The viewer's own spinner range. A value outside
                                        // it is a decode error, not an operator choice, so
                                        // it is dropped rather than stored.
                                        // A range of zero would divide by zero in the
                                        // viewer's own composition arithmetic, so it is
                                        // refused rather than stored.
                                        if (corner >= 4 || low < -500.0F || low > 4000.0F ||
                                            high <= 0.0F || high > 4000.0F)
                                            continue;
                                        pending_terrain_layers.start[corner] = low;
                                        pending_terrain_layers.range[corner] = high;
                                    }
                                } else {
                                    // A commit that changes nothing means the
                                    // staging messages did not arrive first. They
                                    // are one burst from one Apply and LLUDP
                                    // promises delivery, not order, so this is
                                    // possible and recovers on the next Apply -
                                    // but only if it was visible rather than
                                    // looking like a successful no-change.
                                    const bool changed =
                                        terrain_layers.assets != pending_terrain_layers.assets ||
                                        terrain_layers.start != pending_terrain_layers.start ||
                                        terrain_layers.range != pending_terrain_layers.range;
                                    if (!changed)
                                        std::cout << "{\"level\":\"warning\",\"message\":"
                                                     "\"terrain commit changed nothing\",\"note\":"
                                                     "\"staging did not arrive before the commit,"
                                                     " or the values were already these\"}"
                                                  << std::endl;
                                    terrain_layers = pending_terrain_layers;
                                    if (storage) {
                                        try {
                                            storage->save_terrain_settings(terrain_layers.assets,
                                                                           terrain_layers.start,
                                                                           terrain_layers.range);
                                        } catch (const std::exception& error) {
                                            std::cout << "{\"level\":\"error\",\"message\":"
                                                         "\"terrain layers not saved\",\"error\":"
                                                      << homeworldz::api::json_string(error.what())
                                                      << "}" << std::endl;
                                        }
                                    }
                                    // Viewers already connected keep the terrain they were
                                    // handed at login: RegionHandshake is the only message
                                    // that carries these, and re-sending it mid-session
                                    // restarts more of the viewer's region state than a
                                    // texture change warrants. Session clients read the
                                    // hello, so they pick it up on their next connect.
                                    // Said plainly here so "it did not change" is a known
                                    // limit rather than a suspected failure.
                                    // Every connected viewer is re-handshaked. That
                                    // is the message carrying terrain, and the
                                    // viewer is written for the repeat: it diffs
                                    // the composition, re-textures the ground when
                                    // it changed, and refreshes the Region/Estate
                                    // floater. Session clients get their own event
                                    // built from the same function as the greeting.
                                    std::size_t viewers_told = 0;
                                    for (const auto& [viewer_key, viewer_avatar] : avatars) {
                                        if (viewer_avatar.transport != AvatarTransport::lludp) continue;
                                        if (const auto viewer_agent = homeworldz::viewer::parse_uuid(
                                                viewer_avatar.user_id))
                                            if (send_region_handshake(viewer_key, *viewer_agent))
                                                ++viewers_told;
                                    }
                                    std::size_t told = 0;
                                    if (session_server) {
                                        const auto notice = homeworldz::session::encode_envelope(
                                            "terrainLayersChanged", {},
                                            "{\"layers\":" +
                                            homeworldz::session::terrain_layers_json(
                                                terrain_layers) + "}");
                                        for (const auto& [session_key, session_avatar] : avatars) {
                                            static_cast<void>(session_key);
                                            if (session_avatar.transport != AvatarTransport::session)
                                                continue;
                                            session_server->send_to(session_avatar.session_id, notice);
                                            ++told;
                                        }
                                    }
                                    std::cout << "{\"level\":\"info\",\"message\":\"terrain"
                                                 " layers committed\",\"by\":"
                                              << homeworldz::api::json_string(agent)
                                              << ",\"startHeight\":" << terrain_layers.start[0]
                                              << ",\"heightRange\":" << terrain_layers.range[0]
                                              << ",\"defaults\":"
                                              << (terrain_layers.matches_defaults() ? "true" : "false")
                                              << ",\"sessionClientsTold\":" << told
                                              << ",\"viewersReHandshaked\":" << viewers_told
                                              << "}" << std::endl;
                                }
                              }
                            } else if (method == "terrain") {
                              // Firestorm's Terrain tab sends three commands under
                              // this method: "download filename", "upload filename"
                              // and "bake". Only bake is handled; the RAW transfers
                              // need the xfer path and are refused by name rather
                              // than dropped, so an operator learns which of the
                              // three buttons works.
                              const auto command = estate_message->params.empty()
                                  ? std::string{} : estate_message->params[0];
                              if (!manager) {
                                std::cout << "{\"level\":\"warning\",\"message\":"
                                             "\"terrain command refused\",\"command\":"
                                          << homeworldz::api::json_string(command)
                                          << ",\"by\":" << homeworldz::api::json_string(agent)
                                          << ",\"reason\":\"not an estate manager or owner\"}"
                                          << std::endl;
                              } else if (command == "bake") {
                                // Re-baseline: the current ground becomes what the
                                // edit limits are measured from and what the revert
                                // brush returns to. This is the control that makes
                                // the limits usable at all - without it a raise
                                // limit is a one-time budget against the shipped
                                // terrain and an operator who spends it can never
                                // raise again.
                                *revert_heightmap = *terrain_heightmap;
                                const auto saved = homeworldz::terrain::save_state(
                                    revert_state_path, *revert_heightmap);
                                std::cout << "{\"level\":" << (saved ? "\"info\"" : "\"error\"")
                                          << ",\"message\":\"terrain baseline baked\",\"by\":"
                                          << homeworldz::api::json_string(agent)
                                          << ",\"persisted\":" << (saved ? "true" : "false")
                                          << "}" << std::endl;
                              } else {
                                // Say so in the viewer, not only in a log the
                                // operator will never read. Firestorm shows nothing
                                // when this message goes unanswered, so on
                                // 2026-08-06 an operator pressed Download RAW,
                                // saw no error, and spent the next few minutes
                                // looking for a file that was never written. An
                                // absent capability reporting as success is worse
                                // than an error, because the next thing they do
                                // depends on the file existing.
                                const auto alert = homeworldz::viewer::encode_agent_alert_message(
                                    identity->agent_id, false,
                                    "RAW terrain download and upload are not implemented on this "
                                    "region yet. Bake Terrain works. No file was saved.");
                                if (const auto outgoing = circuits.send(endpoint, alert, true, now, true))
                                    static_cast<void>(send_udp(viewer_server, endpoint, *outgoing));
                                std::cout << "{\"level\":\"warning\",\"message\":"
                                             "\"terrain command not implemented\",\"command\":"
                                          << homeworldz::api::json_string(command)
                                          << ",\"by\":" << homeworldz::api::json_string(agent)
                                          << ",\"note\":\"RAW terrain download and upload need the"
                                             " xfer path; bake is implemented; viewer told\"}"
                                          << std::endl;
                              }
                            } else if (method == "setregionterrain") {
                              if (!manager) {
                                std::cout << "{\"level\":\"warning\",\"message\":"
                                             "\"region terrain change refused\",\"by\":"
                                          << homeworldz::api::json_string(agent)
                                          << ",\"reason\":\"not an estate manager or owner\"}"
                                          << std::endl;
                              } else if (estate_message->params.size() < 6) {
                                std::cout << "{\"level\":\"warning\",\"message\":"
                                             "\"region terrain change ignored\",\"paramCount\":"
                                          << estate_message->params.size()
                                          << ",\"reason\":\"fewer parameters than the message defines\"}"
                                          << std::endl;
                              } else {
                                // The Region/Estate -> Terrain tab's other half. Field
                                // order is from the viewer's own sender
                                // (llregioninfomodel.cpp, sendRegionTerrain):
                                //   0 water height, 1 terrain raise, 2 terrain lower,
                                //   3 'Y' use estate sun, 4 'Y' fixed sun, 5 sun hour,
                                //   6..8 the *estate's* use-global-time, fixed sun, sun hour.
                                //
                                // 6..8 are deliberately ignored. The viewer hard-codes
                                // them to Y / N / 0 whatever the estate actually holds,
                                // and says so itself: "*NOTE: this resets estate sun
                                // info." Honouring them would wipe an estate's sun
                                // configuration every time an operator touched this tab,
                                // and it would look exactly like success.
                                const auto number = [](const std::string& text) {
                                    double value = 0.0;
                                    const auto* begin = text.data();
                                    const auto* end = begin + text.size();
                                    return std::from_chars(begin, end, value).ec == std::errc{}
                                        ? std::optional<double>{value} : std::nullopt;
                                };
                                const auto flag = [](const std::string& text) {
                                    return !text.empty() && (text[0] == 'Y' || text[0] == 'y');
                                };
                                auto proposed = region_settings;
                                bool sane = true;
                                if (const auto value = number(estate_message->params[0]);
                                    value && *value >= 0.0 && *value <= 4096.0)
                                    proposed.water_height = *value;
                                else sane = false;
                                // The viewer's own spinner range for both limits.
                                if (const auto value = number(estate_message->params[1]);
                                    value && *value >= 0.0 && *value <= 1000.0)
                                    proposed.terrain_raise = *value;
                                else sane = false;
                                if (const auto value = number(estate_message->params[2]);
                                    value && *value >= -1000.0 && *value <= 0.0)
                                    proposed.terrain_lower = *value;
                                else sane = false;
                                proposed.use_estate_sun = flag(estate_message->params[3]);
                                proposed.fixed_sun = flag(estate_message->params[4]);
                                if (const auto value = number(estate_message->params[5]);
                                    value && *value >= 0.0 && *value < 24.0)
                                    proposed.sun_hour = *value;
                                else sane = false;
                                if (!sane) {
                                    std::cout << "{\"level\":\"warning\",\"message\":"
                                                 "\"region terrain change ignored\",\"reason\":"
                                                 "\"a value did not parse or was out of range\"}"
                                              << std::endl;
                                } else {
                                    const bool water_changed =
                                        proposed.water_height != region_settings.water_height;
                                    region_settings = proposed;
                                    if (storage) {
                                        try {
                                            storage->save_region_settings(region_settings);
                                        } catch (const std::exception& error) {
                                            std::cout << "{\"level\":\"error\",\"message\":"
                                                         "\"region settings not saved\",\"error\":"
                                                      << homeworldz::api::json_string(error.what())
                                                      << "}" << std::endl;
                                        }
                                    }
                                    // Water rides RegionHandshake, so viewers learn it
                                    // the same way they learn terrain.
                                    std::size_t viewers_told = 0;
                                    for (const auto& [viewer_key, viewer_avatar] : avatars) {
                                        if (viewer_avatar.transport != AvatarTransport::lludp) continue;
                                        if (const auto viewer_agent = homeworldz::viewer::parse_uuid(
                                                viewer_avatar.user_id))
                                            if (send_region_handshake(viewer_key, *viewer_agent))
                                                ++viewers_told;
                                    }
                                    std::size_t sessions_told = 0;
                                    if (session_server && water_changed) {
                                        const auto notice = homeworldz::session::encode_envelope(
                                            "waterChanged", {},
                                            "{\"water\":" +
                                            homeworldz::session::water_json(
                                                region_settings.water_height) + "}");
                                        for (const auto& [recipient_key, recipient] : avatars) {
                                            static_cast<void>(recipient_key);
                                            if (recipient.transport != AvatarTransport::session)
                                                continue;
                                            session_server->send_to(recipient.session_id, notice);
                                            ++sessions_told;
                                        }
                                    }
                                    std::cout << "{\"level\":\"info\",\"message\":\"region"
                                                 " terrain settings committed\",\"by\":"
                                              << homeworldz::api::json_string(agent)
                                              << ",\"water\":" << region_settings.water_height
                                              << ",\"terrainRaise\":" << region_settings.terrain_raise
                                              << ",\"terrainLower\":" << region_settings.terrain_lower
                                              << ",\"useEstateSun\":"
                                              << (region_settings.use_estate_sun ? "true" : "false")
                                              << ",\"fixedSun\":"
                                              << (region_settings.fixed_sun ? "true" : "false")
                                              << ",\"sunHour\":" << region_settings.sun_hour
                                              << ",\"viewersReHandshaked\":" << viewers_told
                                              << ",\"sessionClientsTold\":" << sessions_told
                                              << ",\"estateSunFieldsIgnored\":true}" << std::endl;
                                }
                              }
                            } else {
                                // An estate method with no handler is a viewer
                                // asking for something the region silently drops
                                // — the operator's Region/Estate Terrain tab
                                // Apply among them. Naming the method and its
                                // parameters turns the next such request into
                                // evidence rather than into nothing happening,
                                // which is how the materials envelope was
                                // learned rather than guessed.
                                std::string params;
                                for (const auto& parameter : estate_message->params) {
                                    if (!params.empty()) params += " | ";
                                    params += parameter;
                                }
                                std::cout << "{\"level\":\"warning\",\"message\":\"estate method"
                                             " has no handler\",\"method\":"
                                          << homeworldz::api::json_string(method)
                                          << ",\"paramCount\":" << estate_message->params.size()
                                          << ",\"params\":"
                                          << homeworldz::api::json_string(params)
                                          << ",\"manager\":" << (manager ? "true" : "false")
                                          << ",\"owner\":" << (owner ? "true" : "false")
                                          << "}" << std::endl;
                            }
                        }
                        if (const auto requested_names =
                                homeworldz::viewer::decode_uuid_name_request(packet->payload)) {
                            std::vector<homeworldz::viewer::UuidName> names;
                            names.reserve(requested_names->size());
                            for (const auto& requested_id : *requested_names) {
                                const auto user_id = homeworldz::viewer::format_uuid(requested_id);
                                if (const auto found = resolved_avatar_names.find(user_id);
                                    found != resolved_avatar_names.end()) {
                                    names.push_back(found->second);
                                    continue;
                                }
                                try {
                                    const auto user = viewer_grid ? viewer_grid->find_user(user_id) : std::nullopt;
                                    if (!user) continue;
                                    auto [first, last] = legacy_avatar_name(user->username);
                                    homeworldz::viewer::UuidName name{
                                        requested_id, std::move(first), std::move(last)};
                                    resolved_avatar_names.emplace(user_id, name);
                                    names.push_back(std::move(name));
                                } catch (const std::exception& error) {
                                    std::cout << "{\"level\":\"error\",\"message\":\"avatar name lookup failed\",\"error\":"
                                              << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                                }
                            }
                            auto response = homeworldz::viewer::encode_uuid_name_reply(names);
                            if (!response.empty()) {
                                if (const auto outgoing = circuits.send(
                                        endpoint, std::move(response), true, now))
                                    static_cast<void>(send_udp(viewer_server, endpoint, *outgoing));
                            }
                        }
                        const auto region_handle_of = [](int grid_x, int grid_y) {
                            return (static_cast<std::uint64_t>(grid_x * 256) << 32) |
                                static_cast<std::uint32_t>(grid_y * 256);
                        };
                        // The region handle of any region on the grid, by id.
                        // The neighbor list is the cheap answer and the grid
                        // the complete one: home and landmark destinations are
                        // usually not adjacent, so adjacency cannot be the
                        // test for whether they exist.
                        const auto region_handle_for_id = [&](const std::string& region_id)
                            -> std::optional<std::uint64_t> {
                            if (registration && region_id == registration->region_id())
                                return region_handle_of(region_grid_x, region_grid_y);
                            for (const auto& neighbor : region_neighbors)
                                if (neighbor.id == region_id)
                                    return region_handle_of(neighbor.grid_x, neighbor.grid_y);
                            if (!viewer_grid) return std::nullopt;
                            try {
                                if (const auto placement = viewer_grid->find_region(region_id))
                                    return region_handle_of(placement->grid_x, placement->grid_y);
                            } catch (const std::exception& error) {
                                std::cout << "{\"level\":\"error\",\"message\":\"teleport destination lookup failed\",\"error\":"
                                          << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                            }
                            return std::nullopt;
                        };
                        const auto perform_teleport = [&](std::uint64_t destination_handle,
                                                          const std::array<float, 3>& destination_position) {
                            const auto current_position =
                                homeworldz::region::resolve_region_teleport_position(
                                    region_grid_x, region_grid_y, region_size_x, region_size_y,
                                    destination_handle, destination_position);
                            const homeworldz::grid::RegionPlacement* target = nullptr;
                            std::optional<homeworldz::grid::RegionPlacement> distant_target;
                            std::optional<std::array<float, 3>> target_position;
                            for (const auto& neighbor : region_neighbors) {
                                auto resolved = homeworldz::region::resolve_region_teleport_position(
                                    neighbor.grid_x, neighbor.grid_y, neighbor.size_x, neighbor.size_y,
                                    destination_handle, destination_position);
                                if (!resolved) continue;
                                target = &neighbor;
                                target_position = resolved;
                                break;
                            }
                            // Adjacency is the crossing question, not the
                            // teleport question. A handle that matches no
                            // neighbor is the normal case — the map, a
                            // landmark, and home all name distant regions — so
                            // ask the grid where it is rather than refusing.
                            if (!target && !current_position && viewer_grid) {
                                try {
                                    distant_target = viewer_grid->find_region_at(
                                        static_cast<int>((destination_handle >> 32) / 256),
                                        static_cast<int>((destination_handle & 0xFFFFFFFFULL) / 256));
                                } catch (const std::exception& error) {
                                    std::cout << "{\"level\":\"error\",\"message\":\"teleport destination lookup failed\",\"error\":"
                                              << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                                }
                                if (distant_target) {
                                    if (auto resolved = homeworldz::region::resolve_region_teleport_position(
                                            distant_target->grid_x, distant_target->grid_y,
                                            distant_target->size_x, distant_target->size_y,
                                            destination_handle, destination_position)) {
                                        target = &*distant_target;
                                        target_position = resolved;
                                    }
                                }
                            }
                            const auto fail_teleport = [&](std::string reason) {
                                if (const auto failed = circuits.send(endpoint,
                                        homeworldz::viewer::encode_teleport_failed(
                                            {identity->agent_id, std::move(reason)}), true, now, true))
                                    static_cast<void>(send_udp(viewer_server, endpoint, *failed));
                            };
                            if (current_position) {
                                const auto avatar = avatars.find(endpoint);
                                const homeworldz::scene::Vector3 requested_position{
                                    (*current_position)[0], (*current_position)[1], (*current_position)[2]};
                                // Parcel entry policy and landing-point routing: deny a
                                // teleport into a parcel that bans/​restricts the agent, and
                                // redirect to the parcel's landing point when one is set.
                                auto arrival = requested_position;
                                bool entry_denied = false;
                                {
                                    const auto agent = homeworldz::viewer::format_uuid(identity->agent_id);
                                    if (estate_denies_entry(agent)) {
                                        entry_denied = true;
                                    } else if (const auto* parcel = parcels->parcel_at(
                                            static_cast<float>(arrival.x), static_cast<float>(arrival.y))) {
                                        if (!homeworldz::parcel::can_enter(*parcel, agent, region_owner_id) &&
                                            !is_estate_manager(agent)) {
                                            entry_denied = true;
                                        } else if (parcel->landing_type == static_cast<std::uint8_t>(
                                                       homeworldz::parcel::LandingType::landing_point) &&
                                                   (parcel->user_location.x != 0.0F ||
                                                    parcel->user_location.y != 0.0F ||
                                                    parcel->user_location.z != 0.0F) &&
                                                   agent != parcel->owner_id) {
                                            arrival = {parcel->user_location.x, parcel->user_location.y,
                                                       parcel->user_location.z};
                                        }
                                    }
                                }
                                if (avatar == avatars.end() || entry_denied ||
                                    requested_position.x < 0.0 ||
                                    requested_position.x > region_size_x || requested_position.y < 0.0 ||
                                    requested_position.y > region_size_y) {
                                    fail_teleport(entry_denied
                                        ? "You are not permitted to enter that parcel"
                                        : "Destination position is unavailable");
                                } else {
                                    const auto flying = avatar->second.controller.state().flying;
                                    avatar->second.controller.set_ground_height(
                                        collision_ground_height(arrival));
                                    avatar->second.controller.teleport(arrival, flying);
                                    if (physics_world && avatar->second.physics_character != 0) {
                                        if (auto state = physics_world->character_state(
                                                avatar->second.physics_character)) {
                                            state->position = avatar->second.controller.state().position;
                                            state->linear_velocity = {};
                                            state->grounded = avatar->second.controller.state().grounded;
                                            physics_world->set_character_state(
                                                avatar->second.physics_character, *state);
                                            physics_world->set_character_flying(
                                                avatar->second.physics_character, flying);
                                        }
                                    }
                                    const auto position = avatar->second.controller.viewer_position();
                                    const auto look_direction = avatar->second.controller.look_direction();
                                    const auto flags = homeworldz::viewer::teleport_flags_via_location |
                                        (flying ? homeworldz::viewer::teleport_flags_is_flying : 0U);
                                    if (const auto local = circuits.send(endpoint,
                                            homeworldz::viewer::encode_teleport_local({
                                                identity->agent_id, 2,
                                                {static_cast<float>(position.x),
                                                 static_cast<float>(position.y),
                                                 static_cast<float>(position.z)},
                                                look_direction, flags}), true, now, true))
                                        static_cast<void>(send_udp(viewer_server, endpoint, *local));
                                    std::cout << "{\"level\":\"info\",\"message\":\"avatar local teleport completed\","
                                                 "\"position\":[" << position.x << ',' << position.y << ','
                                              << position.z << "]}" << std::endl;
                                }
                            } else if (!target || !target_position || !target->online ||
								!viewer_grid || !registration) {
                                fail_teleport("Destination region is unavailable");
                            } else if (const auto simulator = simulator_event_endpoint(
                                           target->public_endpoint, target->viewer_port)) {
                                const auto session_id = homeworldz::viewer::format_uuid(identity->session_id);
                                const auto agent_id = homeworldz::viewer::format_uuid(identity->agent_id);
                                const auto transit_id = homeworldz::viewer::random_uuid();
                                const auto avatar = avatars.find(endpoint);
                                const bool flying = avatar != avatars.end() &&
                                    avatar->second.controller.state().flying;
                                const auto teleport_flags =
                                    homeworldz::viewer::teleport_flags_via_location |
                                    (flying ? homeworldz::viewer::teleport_flags_is_flying : 0U);
                                bool prepared = false;
                                try {
                                    if (const auto start = circuits.send(endpoint,
                                            homeworldz::viewer::encode_teleport_start({teleport_flags}),
                                            true, now, true))
                                        static_cast<void>(send_udp(viewer_server, endpoint, *start));
                                    // TeleportLocationRequest.LookAt is an absolute destination
                                    // point, not a direction. Preserve the live avatar's facing
                                    // across the handoff instead of interpreting that point as a
                                    // quaternion direction at the destination.
                                    const auto look_direction = avatar != avatars.end() ?
                                        avatar->second.controller.look_direction() :
                                        std::array<float, 3>{1.0F, 0.0F, 0.0F};
                                    const homeworldz::grid::AvatarTransitRequest transit_request{
                                        transit_id, agent_id, session_id, registration->region_id(),
                                        target->id, *target_position, look_direction, flying, 30};
                                    const auto transit = viewer_grid->prepare_avatar_transit(transit_request);
                                    prepared = transit && transit->state == "prepared";
                                    if (!prepared) throw std::runtime_error("grid rejected transit preparation");
                                    auto destination = homeworldz::grid::socket_transport(
                                        target->public_endpoint, service_token);
                                    if (!homeworldz::grid::prepare_avatar_arrival(*destination, transit_id))
                                        throw std::runtime_error("destination rejected transit preparation");
                                    const auto target_handle =
                                        (static_cast<std::uint64_t>(target->grid_x * 256) << 32) |
                                        static_cast<std::uint32_t>(target->grid_y * 256);
                                    enqueue_viewer_event(session_id,
                                        homeworldz::viewer::enable_simulator_event_xml(
                                            target_handle, *simulator,
                                            static_cast<std::uint32_t>(target->size_x),
                                            static_cast<std::uint32_t>(target->size_y)));
                                    enqueue_viewer_event(session_id,
                                        homeworldz::viewer::teleport_finish_event_xml({
                                            agent_id, target_handle, *simulator,
                                            target->public_endpoint + "/caps/seed/" + session_id +
                                                "/" + transit_id, 13, teleport_flags,
                                            static_cast<std::uint32_t>(target->size_x),
                                            static_cast<std::uint32_t>(target->size_y)}));
                                    std::cout << "{\"level\":\"info\",\"message\":\"avatar teleport signaled\",\"transitId\":"
                                              << homeworldz::api::json_string(transit_id)
                                              << ",\"destinationRegionId\":"
                                              << homeworldz::api::json_string(target->id) << "}" << std::endl;
                                } catch (const std::exception& error) {
                                    if (prepared) static_cast<void>(viewer_grid->rollback_avatar_transit(
                                        transit_id, registration->region_id(), error.what()));
                                    fail_teleport("Destination region could not prepare the arrival");
                                    std::cout << "{\"level\":\"error\",\"message\":\"avatar teleport preparation failed\",\"error\":"
                                              << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                                }
                            } else {
                                fail_teleport("Destination viewer address could not be resolved");
                            }
                        };
                        if (const auto teleport =
                                homeworldz::viewer::decode_teleport_location_request(packet->payload);
                            teleport && teleport->agent_id == identity->agent_id &&
                            teleport->session_id == identity->session_id) {
                            perform_teleport(teleport->region_handle, teleport->position);
                        }
                        if (const auto landmark_tp =
                                homeworldz::viewer::decode_teleport_landmark_request(packet->payload);
                            landmark_tp && landmark_tp->agent_id == identity->agent_id &&
                            landmark_tp->session_id == identity->session_id) {
                            const auto fail_landmark = [&](std::string reason) {
                                if (const auto failed = circuits.send(endpoint,
                                        homeworldz::viewer::encode_teleport_failed(
                                            {identity->agent_id, std::move(reason)}), true, now, true))
                                    static_cast<void>(send_udp(viewer_server, endpoint, *failed));
                            };
                            if (landmark_tp->landmark_id == homeworldz::viewer::Uuid{}) {
                                // Null landmark = Teleport Home.
                                const auto user_id = homeworldz::viewer::format_uuid(identity->agent_id);
                                std::optional<homeworldz::grid::HomeLocation> home;
                                if (viewer_grid) {
                                    try {
                                        home = viewer_grid->home_location(user_id);
                                    } catch (const std::exception&) {
                                    }
                                }
                                std::optional<std::uint64_t> destination_handle;
                                if (home) destination_handle = region_handle_for_id(home->region_id);
                                if (!home) fail_landmark("Home location is not set");
                                else if (destination_handle) perform_teleport(*destination_handle, home->position);
                                else fail_landmark("Your home region is unavailable");
                            } else {
                                // Landmark asset: "Landmark version 2\nregion_id <uuid>\nlocal_pos x y z".
                                std::optional<std::uint64_t> destination_handle;
                                std::array<float, 3> local_pos{};
                                try {
                                    const auto bytes = read_federated_asset(
                                        homeworldz::viewer::format_uuid(landmark_tp->landmark_id));
                                    const std::string text(
                                        reinterpret_cast<const char*>(bytes.data()), bytes.size());
                                    const auto region_key = text.find("region_id ");
                                    const auto pos_key = text.find("local_pos ");
                                    if (region_key != std::string::npos && pos_key != std::string::npos &&
                                        region_key + 10 + 36 <= text.size()) {
                                        const auto region_id = text.substr(region_key + 10, 36);
                                        std::istringstream coords(text.substr(pos_key + 10));
                                        coords >> local_pos[0] >> local_pos[1] >> local_pos[2];
                                        destination_handle = region_handle_for_id(region_id);
                                    }
                                } catch (const std::exception&) {
                                }
                                if (destination_handle) perform_teleport(*destination_handle, local_pos);
                                else fail_landmark("Landmark destination is unavailable");
                            }
                        }
                        if (const auto set_home =
                                homeworldz::viewer::decode_set_start_location_request(packet->payload);
                            set_home && set_home->agent_id == identity->agent_id &&
                            set_home->session_id == identity->session_id && viewer_grid && registration) {
                            // "World > Set Home to Here". Until parcel permissions exist, home may be
                            // set anywhere; use the server-authoritative avatar position when available.
                            const auto user_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            std::array<float, 3> position = set_home->position;
                            std::array<float, 3> look = set_home->look_at;
                            if (const auto live = avatars.find(endpoint); live != avatars.end()) {
                                const auto& state = live->second.controller.state();
                                position = {static_cast<float>(state.position.x),
                                            static_cast<float>(state.position.y),
                                            static_cast<float>(state.position.z)};
                                look = live->second.controller.look_direction();
                            }
                            bool ok = false;
                            try {
                                ok = viewer_grid->set_home_location(
                                    user_id, registration->region_id(), position, look);
                            } catch (const std::exception&) {
                            }
                            // Confirm to the viewer, matching Halcyon/SL behaviour.
                            const auto alert = homeworldz::viewer::encode_agent_alert_message(
                                identity->agent_id, false,
                                ok ? "Home position set." : "Couldn't set home position here.");
                            if (const auto outgoing = circuits.send(endpoint, alert, true, now, true))
                                static_cast<void>(send_udp(viewer_server, endpoint, *outgoing));
                            std::cout << "{\"level\":\"" << (ok ? "info" : "warn")
                                      << "\",\"message\":\"set home location "
                                      << (ok ? "stored" : "rejected") << "\",\"userId\":"
                                      << homeworldz::api::json_string(user_id) << "}" << std::endl;
                        }
                        if (const auto activate =
                                homeworldz::viewer::decode_activate_gestures(packet->payload);
                            activate && activate->agent_id == identity->agent_id &&
                            activate->session_id == identity->session_id && viewer_grid) {
                            const auto user_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            for (const auto& gesture : activate->gestures) {
                                try {
                                    static_cast<void>(viewer_grid->set_gesture_active(
                                        user_id, homeworldz::viewer::format_uuid(gesture.item_id),
                                        homeworldz::viewer::format_uuid(gesture.asset_id), true));
                                } catch (const std::exception&) {
                                }
                            }
                        }
                        if (const auto deactivate =
                                homeworldz::viewer::decode_deactivate_gestures(packet->payload);
                            deactivate && deactivate->agent_id == identity->agent_id &&
                            deactivate->session_id == identity->session_id && viewer_grid) {
                            const auto user_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            for (const auto& item_id : deactivate->item_ids) {
                                try {
                                    static_cast<void>(viewer_grid->set_gesture_active(
                                        user_id, homeworldz::viewer::format_uuid(item_id), "", false));
                                } catch (const std::exception&) {
                                }
                            }
                        }
                        const auto logout = homeworldz::viewer::decode_logout_request(packet->payload);
                        if (logout && logout->agent_id == identity->agent_id &&
                            logout->session_id == identity->session_id) {
                            homeworldz::viewer::AgentMessage reply{identity->agent_id, identity->session_id};
                            if (const auto outgoing = circuits.send(endpoint,
                                    homeworldz::viewer::encode_logout_reply(reply), true, now, true))
                                static_cast<void>(send_udp(viewer_server, endpoint, *outgoing));
                            const auto session_id = homeworldz::viewer::format_uuid(identity->session_id);
                            const auto user_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            if (viewer_grid) {
                                if (const auto live = avatars.find(endpoint);
                                    live != avatars.end() && registration) {
                                    const auto& state = live->second.controller.state();
                                    const std::array<float, 3> position{
                                        static_cast<float>(state.position.x),
                                        static_cast<float>(state.position.y),
                                        static_cast<float>(state.position.z)};
                                    try {
                                        if (!viewer_grid->update_last_location(
                                                user_id, registration->region_id(), position,
                                                live->second.controller.look_direction(), state.flying))
                                            std::cout << "{\"level\":\"warn\",\"message\":\"last location update rejected during logout\",\"userId\":"
                                                      << homeworldz::api::json_string(user_id) << "}" << std::endl;
                                    } catch (const std::exception& error) {
                                        std::cout << "{\"level\":\"warn\",\"message\":\"last location update failed during logout\",\"error\":"
                                                  << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                                    }
                                }
                                static_cast<void>(viewer_grid->clear_presence(user_id));
                                static_cast<void>(viewer_grid->revoke_viewer_session(session_id));
                            }
                            if (viewer_sessions) viewer_sessions->invalidate(session_id);
                            clear_viewer_endpoint(endpoint, session_id);
                            circuits.remove(endpoint);
                            std::cout << "{\"level\":\"info\",\"message\":\"viewer logged out\",\"sessionId\":"
                                      << homeworldz::api::json_string(session_id) << "}" << std::endl;
                            continue;
                        }
                        const auto task_inventory_request =
                            homeworldz::viewer::decode_request_task_inventory(packet->payload);
                        if (task_inventory_request &&
                            task_inventory_request->agent_id == identity->agent_id &&
                            task_inventory_request->session_id == identity->session_id) {
                            const auto* entity = scene.find(task_inventory_request->local_id);
                            const auto agent_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            bool sent = false;
                            if (entity && entity->owner_id == agent_id) {
                                const auto task_id = homeworldz::viewer::parse_uuid(entity->object_id);
                                if (task_id) {
                                    std::string filename;
                                    std::int16_t serial{};
                                    if (entity->task_inventory_serial != 0) {
                                        filename = "inventory_" + homeworldz::viewer::random_uuid() + ".tmp";
                                        const auto content = task_inventory_file(*entity);
                                        pending_task_inventory_files.insert_or_assign(
                                            endpoint + '|' + filename, content);
                                        serial = static_cast<std::int16_t>(entity->task_inventory_serial);
                                    }
                                    auto wire_filename = filename;
                                    if (!wire_filename.empty()) wire_filename.push_back('\0');
                                    const auto payload = homeworldz::viewer::encode_reply_task_inventory(
                                        {*task_id, serial, wire_filename});
                                    if (!payload.empty()) {
                                        if (const auto outgoing = circuits.send(
                                                endpoint, payload, true, now, true))
                                            sent = send_udp(viewer_server, endpoint, *outgoing);
                                    }
                                }
                            }
                            std::cout << "{\"level\":" << (sent ? "\"info\"" : "\"warn\"")
                                      << ",\"message\":\"task inventory reply "
                                      << (sent ? "sent" : "rejected") << "\",\"localId\":"
                                      << task_inventory_request->local_id << ",\"items\":"
                                      << (entity ? entity->task_inventory.size() : 0) << "}" << std::endl;
                        }
                        const auto rez_script =
                            homeworldz::viewer::decode_rez_script(packet->payload);
                        if (rez_script && rez_script->agent_id == identity->agent_id &&
                            rez_script->session_id == identity->session_id) {
                            auto* entity = scene.find(rez_script->local_id);
                            const auto agent_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            const auto source_id = homeworldz::viewer::format_uuid(rez_script->item_id);
                            bool changed = false;
                            std::string operation{"copy"};
                            std::string task_item_id;
                            std::optional<homeworldz::script::FalconRezResult> compiled;
                            try {
                                if (entity && entity->owner_id == agent_id &&
                                    (entity->owner_permissions &
                                     homeworldz::scene::permission_modify) != 0 &&
                                    rez_script->asset_type == 10 &&
                                    rez_script->inventory_type == 10) {
                                    const auto previous_serial = entity->task_inventory_serial;
                                    const auto source = viewer_grid &&
                                            rez_script->item_id != homeworldz::viewer::Uuid{}
                                        ? viewer_grid->find_inventory_item(agent_id, source_id)
                                        : std::nullopt;
                                    if (source && source->asset_type == 10 &&
                                        source->inventory_type == 10 &&
                                        (source->current_permissions &
                                         homeworldz::scene::permission_copy) != 0) {
                                        operation = "copy";
                                        task_item_id = homeworldz::viewer::random_uuid();
                                        const auto created = static_cast<std::uint64_t>(
                                            std::chrono::duration_cast<std::chrono::seconds>(
                                                std::chrono::system_clock::now()
                                                    .time_since_epoch())
                                                .count());
                                        entity->task_inventory.push_back({
                                            task_item_id, source->asset_id, source->creator_id,
                                            agent_id, source->owner_id,
                                            "00000000-0000-0000-0000-000000000000",
                                            source->name, source->description,
                                            static_cast<std::int8_t>(source->asset_type),
                                            static_cast<std::int8_t>(source->inventory_type),
                                            source->flags, source->base_permissions,
                                            source->current_permissions, 0,
                                            source->everyone_permissions, source->next_permissions,
                                            static_cast<std::uint8_t>(source->sale_type),
                                            source->sale_price, created});
                                        entity->task_inventory_serial = previous_serial == 65535
                                            ? 1
                                            : static_cast<std::uint16_t>(previous_serial + 1);
                                        try {
                                            if (storage) storage->save_snapshot(scene);
                                            changed = true;
                                        } catch (...) {
                                            entity->task_inventory.pop_back();
                                            entity->task_inventory_serial = previous_serial;
                                            throw;
                                        }
                                    } else if (source && viewer_grid) {
                                        operation = "transfer";
                                        task_item_id = homeworldz::viewer::random_uuid();
                                        const auto transfer = viewer_grid->prepare_task_inventory_transfer({
                                            homeworldz::viewer::random_uuid(), agent_id, source_id,
                                            provisioned_region_id, entity->object_id, task_item_id});
                                        if (transfer && transfer->state == "prepared") {
                                            const auto finalized =
                                                apply_task_inventory_transfer(*transfer);
                                            changed = std::any_of(
                                                entity->task_inventory.begin(),
                                                entity->task_inventory.end(),
                                                [&](const auto& item) {
                                                    return item.item_id == task_item_id;
                                                });
                                            if (changed && !finalized)
                                                std::cerr << "{\"level\":\"warning\",\"message\":\"script transfer awaits reconciliation\",\"transferId\":"
                                                          << homeworldz::api::json_string(transfer->id)
                                                          << "}" << std::endl;
                                        }
                                    } else if (rez_script->item_id == homeworldz::viewer::Uuid{} &&
                                               rez_script->transaction_id ==
                                                   homeworldz::viewer::Uuid{} &&
                                               viewer_grid && storage) {
                                        operation = "create";
                                        const auto initial =
                                            homeworldz::inventory::default_asset_content(
                                                10, 10, provisioned_region_id, entity->position);
                                        if (initial) {
                                            auto asset_id = homeworldz::viewer::random_uuid();
                                            const auto content = std::span(
                                                reinterpret_cast<const std::byte*>(initial->data()),
                                                initial->size());
                                            const auto metadata =
                                                storage->store_asset(asset_id, agent_id, content);
                                            if (!viewer_grid->register_asset(
                                                    metadata.viewer_id, metadata.creator_id,
                                                    metadata.sha256, metadata.size,
                                                    region_public_endpoint, true) ||
                                                // Write-through (ADR 0026): a later
                                                // extraction commits this asset.
                                                !viewer_grid->store_vault_asset(metadata.viewer_id, content))
                                                asset_id.clear();
                                            if (!asset_id.empty()) {
                                                task_item_id = homeworldz::viewer::random_uuid();
                                                const auto created = static_cast<std::uint64_t>(
                                                    std::chrono::duration_cast<std::chrono::seconds>(
                                                        std::chrono::system_clock::now()
                                                            .time_since_epoch())
                                                        .count());
                                                entity->task_inventory.push_back({
                                                    task_item_id, asset_id, agent_id, agent_id,
                                                    agent_id,
                                                    "00000000-0000-0000-0000-000000000000",
                                                    rez_script->name, rez_script->description, 10, 10,
                                                    rez_script->flags,
                                                    rez_script->base_permissions,
                                                    rez_script->owner_permissions,
                                                    rez_script->group_permissions,
                                                    rez_script->everyone_permissions,
                                                    rez_script->next_owner_permissions,
                                                    rez_script->sale_type,
                                                    rez_script->sale_price, created});
                                                entity->task_inventory_serial =
                                                    previous_serial == 65535
                                                        ? 1
                                                        : static_cast<std::uint16_t>(
                                                              previous_serial + 1);
                                                try {
                                                    storage->save_snapshot(scene);
                                                    changed = true;
                                                } catch (...) {
                                                    entity->task_inventory.pop_back();
                                                    entity->task_inventory_serial = previous_serial;
                                                    throw;
                                                }
                                            }
                                        }
                                    }
                                }
                            } catch (const std::exception& error) {
                                std::cout << "{\"level\":\"error\",\"message\":\"rez script failed\",\"error\":"
                                          << homeworldz::api::json_string(error.what()) << "}"
                                          << std::endl;
                            }
                            if (changed && entity) {
                                const auto item = std::find_if(
                                    entity->task_inventory.begin(),
                                    entity->task_inventory.end(),
                                    [&](const auto& candidate) {
                                        return candidate.item_id == task_item_id;
                                    });
                                if (item != entity->task_inventory.end())
                                    compiled = rez_task_script(
                                        *entity, *item, rez_script->enabled);
                                broadcast_object_update(
                                    *entity, std::chrono::steady_clock::now());
                            }
                            bool refresh_sent = false;
                            if (changed && entity) {
                                const auto task_id =
                                    homeworldz::viewer::parse_uuid(entity->object_id);
                                if (task_id) {
                                    const auto filename = "inventory_" +
                                        homeworldz::viewer::random_uuid() + ".tmp";
                                    pending_task_inventory_files.insert_or_assign(
                                        endpoint + '|' + filename,
                                        task_inventory_file(*entity));
                                    auto wire_filename = filename;
                                    wire_filename.push_back('\0');
                                    const auto payload =
                                        homeworldz::viewer::encode_reply_task_inventory({
                                            *task_id,
                                            static_cast<std::int16_t>(
                                                entity->task_inventory_serial),
                                            wire_filename});
                                    if (const auto outgoing = circuits.send(
                                            endpoint, payload, true, now, true))
                                        refresh_sent =
                                            send_udp(viewer_server, endpoint, *outgoing);
                                }
                            }
                            std::cout << "{\"level\":"
                                      << (changed ? "\"info\"" : "\"warn\"")
                                      << ",\"message\":\"rez script " << operation << ' '
                                      << (changed ? "completed" : "rejected")
                                      << "\",\"localId\":" << rez_script->local_id
                                      << ",\"itemId\":"
                                      << homeworldz::api::json_string(task_item_id)
                                      << ",\"enabled\":"
                                      << (rez_script->enabled ? "true" : "false")
                                      << ",\"compiled\":"
                                      << (compiled ? (compiled->compiled ? "true" : "false") : "null")
                                      << ",\"running\":"
                                      << (compiled ? (compiled->running ? "true" : "false") : "null")
                                      << ",\"diagnostic\":"
                                      << homeworldz::api::json_string(
                                             compiled ? compiled->diagnostic : std::string{})
                                      << ",\"refreshSent\":"
                                      << (refresh_sent ? "true" : "false") << "}"
                                      << std::endl;
                        }
                        const auto task_inventory_update =
                            homeworldz::viewer::decode_update_task_inventory(packet->payload);
                        if (task_inventory_update &&
                            task_inventory_update->agent_id == identity->agent_id &&
                            task_inventory_update->session_id == identity->session_id) {
                            auto* entity = scene.find(task_inventory_update->local_id);
                            const auto agent_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            const auto source_id = homeworldz::viewer::format_uuid(task_inventory_update->item_id);
                            bool changed = false;
                            std::string operation{"copy"};
                            try {
                                if (entity && entity->owner_id == agent_id &&
                                    (entity->owner_permissions & homeworldz::scene::permission_modify) != 0) {
                                    const auto previous_serial = entity->task_inventory_serial;
                                    const auto existing = std::find_if(
                                        entity->task_inventory.begin(), entity->task_inventory.end(),
                                        [&](const auto& item) { return item.item_id == source_id; });
                                    if (existing != entity->task_inventory.end()) {
                                        operation = "update";
                                        const auto original = *existing;
                                        if (homeworldz::scene::apply_task_inventory_update(
                                                *existing, task_inventory_update->name,
                                                task_inventory_update->description,
                                                task_inventory_update->flags,
                                                task_inventory_update->owner_permissions,
                                                task_inventory_update->group_permissions,
                                                task_inventory_update->everyone_permissions,
                                                task_inventory_update->next_owner_permissions,
                                                task_inventory_update->sale_type,
                                                task_inventory_update->sale_price)) {
                                            entity->task_inventory_serial = previous_serial == 65535
                                                ? 1
                                                : static_cast<std::uint16_t>(previous_serial + 1);
                                            try {
                                                if (storage) storage->save_snapshot(scene);
                                                changed = true;
                                            } catch (...) {
                                                *existing = original;
                                                entity->task_inventory_serial = previous_serial;
                                                throw;
                                            }
                                        }
                                    } else {
                                        const auto source = viewer_grid
                                            ? viewer_grid->find_inventory_item(agent_id, source_id)
                                            : std::nullopt;
                                        if (source && (source->current_permissions &
                                                homeworldz::scene::permission_copy) != 0) {
                                            const auto created = static_cast<std::uint64_t>(
                                                std::chrono::duration_cast<std::chrono::seconds>(
                                                    std::chrono::system_clock::now().time_since_epoch()).count());
                                            entity->task_inventory.push_back({
                                                homeworldz::viewer::random_uuid(), source->asset_id,
                                                source->creator_id, agent_id, source->owner_id,
                                                "00000000-0000-0000-0000-000000000000",
                                                source->name, source->description,
                                                static_cast<std::int8_t>(source->asset_type),
                                                static_cast<std::int8_t>(source->inventory_type),
                                                source->flags, source->base_permissions,
                                                source->current_permissions, 0,
                                                source->everyone_permissions, source->next_permissions,
                                                static_cast<std::uint8_t>(source->sale_type),
                                                source->sale_price, created});
                                            entity->task_inventory_serial = previous_serial == 65535
                                                ? 1
                                                : static_cast<std::uint16_t>(previous_serial + 1);
                                            try {
                                                if (storage) storage->save_snapshot(scene);
                                                changed = true;
                                            } catch (...) {
                                                entity->task_inventory.pop_back();
                                                entity->task_inventory_serial = previous_serial;
                                                throw;
                                            }
                                        } else if (source && viewer_grid) {
                                            operation = "transfer";
                                            const auto task_item_id = homeworldz::viewer::random_uuid();
                                            const auto transfer = viewer_grid->prepare_task_inventory_transfer({
                                                homeworldz::viewer::random_uuid(), agent_id, source_id,
                                                provisioned_region_id, entity->object_id, task_item_id});
                                            if (transfer && transfer->state == "prepared") {
                                                const auto finalized = apply_task_inventory_transfer(*transfer);
                                                changed = std::any_of(
                                                    entity->task_inventory.begin(),
                                                    entity->task_inventory.end(),
                                                    [&](const auto& item) {
                                                        return item.item_id == transfer->task_item_id;
                                                    });
                                                if (changed && !finalized)
                                                    std::cerr << "{\"level\":\"warning\",\"message\":\"task inventory transfer awaits reconciliation\",\"transferId\":"
                                                              << homeworldz::api::json_string(transfer->id)
                                                              << "}" << std::endl;
                                            }
                                        } else if (viewer_grid && storage) {
                                            // "New Script"/"New Note" in the
                                            // Contents tab: a fresh item that is
                                            // neither already in the task nor an
                                            // existing agent-inventory item. Mint
                                            // the default asset for its type and
                                            // add it to the task.
                                            const auto initial =
                                                homeworldz::inventory::default_asset_content(
                                                    task_inventory_update->asset_type,
                                                    task_inventory_update->inventory_type,
                                                    provisioned_region_id, entity->position);
                                            if (initial && task_inventory_update->transaction_id ==
                                                               homeworldz::viewer::Uuid{}) {
                                                operation = "create";
                                                std::string new_asset_id;
                                                try {
                                                    new_asset_id = homeworldz::viewer::random_uuid();
                                                    const auto content = std::span(
                                                        reinterpret_cast<const std::byte*>(initial->data()),
                                                        initial->size());
                                                    const auto metadata = storage->store_asset(
                                                        new_asset_id, agent_id, content);
                                                    if (!viewer_grid->register_asset(
                                                            metadata.viewer_id, metadata.creator_id,
                                                            metadata.sha256, metadata.size,
                                                            region_public_endpoint, true))
                                                        new_asset_id.clear();
                                                } catch (const std::exception&) {
                                                    new_asset_id.clear();
                                                }
                                                if (!new_asset_id.empty()) {
                                                    const auto created = static_cast<std::uint64_t>(
                                                        std::chrono::duration_cast<std::chrono::seconds>(
                                                            std::chrono::system_clock::now()
                                                                .time_since_epoch())
                                                            .count());
                                                    entity->task_inventory.push_back(
                                                        {homeworldz::viewer::random_uuid(), new_asset_id,
                                                         agent_id, agent_id, agent_id,
                                                         "00000000-0000-0000-0000-000000000000",
                                                         task_inventory_update->name,
                                                         task_inventory_update->description,
                                                         task_inventory_update->asset_type,
                                                         task_inventory_update->inventory_type,
                                                         task_inventory_update->flags,
                                                         homeworldz::scene::permission_creator,
                                                         homeworldz::scene::permission_creator, 0, 0,
                                                         homeworldz::scene::permission_all,
                                                         static_cast<std::uint8_t>(
                                                             task_inventory_update->sale_type),
                                                         task_inventory_update->sale_price, created});
                                                    entity->task_inventory_serial =
                                                        previous_serial == 65535
                                                            ? 1
                                                            : static_cast<std::uint16_t>(
                                                                  previous_serial + 1);
                                                    try {
                                                        storage->save_snapshot(scene);
                                                        changed = true;
                                                    } catch (...) {
                                                        entity->task_inventory.pop_back();
                                                        entity->task_inventory_serial = previous_serial;
                                                        throw;
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            } catch (const std::exception& error) {
                                std::cout << "{\"level\":\"error\",\"message\":\"task inventory mutation failed\",\"error\":"
                                          << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                            }
                            bool refresh_sent = false;
                            if (changed && entity) {
                                const auto task_id = homeworldz::viewer::parse_uuid(entity->object_id);
                                const auto content = task_inventory_file(*entity);
                                if (task_id) {
                                    const auto filename =
                                        "inventory_" + homeworldz::viewer::random_uuid() + ".tmp";
                                    pending_task_inventory_files.insert_or_assign(
                                        endpoint + '|' + filename, content);
                                    auto wire_filename = filename;
                                    wire_filename.push_back('\0');
                                    const auto payload = homeworldz::viewer::encode_reply_task_inventory({
                                        *task_id,
                                        static_cast<std::int16_t>(entity->task_inventory_serial),
                                        wire_filename});
                                    if (const auto outgoing = circuits.send(
                                            endpoint, payload, true, now, true))
                                        refresh_sent = send_udp(viewer_server, endpoint, *outgoing);
                                }
                            }
                            std::cout << "{\"level\":" << (changed ? "\"info\"" : "\"warn\"")
                                      << ",\"message\":\"task inventory item " << operation << ' '
                                      << (changed ? "completed" : "rejected") << "\",\"localId\":"
                                      << task_inventory_update->local_id << ",\"itemId\":"
                                      << homeworldz::api::json_string(source_id)
                                      << ",\"refreshSent\":" << (refresh_sent ? "true" : "false")
                                      << "}" << std::endl;
                        }
                        const auto task_inventory_remove =
                            homeworldz::viewer::decode_remove_task_inventory(packet->payload);
                        if (task_inventory_remove &&
                            task_inventory_remove->agent_id == identity->agent_id &&
                            task_inventory_remove->session_id == identity->session_id) {
                            auto* entity = scene.find(task_inventory_remove->local_id);
                            const auto agent_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            const auto item_id = homeworldz::viewer::format_uuid(task_inventory_remove->item_id);
                            bool removed = false;
                            if (entity && entity->owner_id == agent_id &&
                                (entity->owner_permissions & homeworldz::scene::permission_modify) != 0) {
                                const auto item = std::find_if(
                                    entity->task_inventory.begin(), entity->task_inventory.end(),
                                    [&](const auto& candidate) { return candidate.item_id == item_id; });
                                if (item != entity->task_inventory.end()) {
                                    const auto index = static_cast<std::size_t>(
                                        item - entity->task_inventory.begin());
                                    const auto previous_serial = entity->task_inventory_serial;
                                    const auto original = *item;
                                    entity->task_inventory.erase(item);
                                    entity->task_inventory_serial = previous_serial == 65535
                                        ? 1
                                        : static_cast<std::uint16_t>(previous_serial + 1);
                                    try {
                                        if (storage) storage->save_snapshot(scene);
                                        removed = true;
                                        static_cast<void>(falcon.erase(entity->object_id, item_id));
                                    } catch (const std::exception& error) {
                                        entity->task_inventory.insert(
                                            entity->task_inventory.begin() + index, original);
                                        entity->task_inventory_serial = previous_serial;
                                        std::cout << "{\"level\":\"error\",\"message\":\"task inventory removal persistence failed\",\"error\":"
                                                  << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                                    }
                                }
                            }
                            bool refresh_sent = false;
                            if (removed && entity) {
                                broadcast_object_update(
                                    *entity, std::chrono::steady_clock::now());
                                const auto task_id = homeworldz::viewer::parse_uuid(entity->object_id);
                                if (task_id) {
                                    std::string filename;
                                    if (entity->task_inventory_serial != 0) {
                                        const auto content = task_inventory_file(*entity);
                                        filename = "inventory_" +
                                            homeworldz::viewer::random_uuid() + ".tmp";
                                        pending_task_inventory_files.insert_or_assign(
                                            endpoint + '|' + filename, content);
                                    }
                                    auto wire_filename = filename;
                                    if (!wire_filename.empty()) wire_filename.push_back('\0');
                                    const auto payload = homeworldz::viewer::encode_reply_task_inventory({
                                        *task_id,
                                        static_cast<std::int16_t>(entity->task_inventory_serial),
                                        wire_filename});
                                    if (const auto outgoing = circuits.send(
                                            endpoint, payload, true, now, true))
                                        refresh_sent = send_udp(viewer_server, endpoint, *outgoing);
                                }
                            }
                            std::cout << "{\"level\":" << (removed ? "\"info\"" : "\"warn\"")
                                      << ",\"message\":\"task inventory item removal "
                                      << (removed ? "completed" : "rejected") << "\",\"localId\":"
                                      << task_inventory_remove->local_id << ",\"itemId\":"
                                      << homeworldz::api::json_string(item_id)
                                      << ",\"refreshSent\":" << (refresh_sent ? "true" : "false")
                                      << "}" << std::endl;
                        }
                        const auto task_inventory_move =
                            homeworldz::viewer::decode_move_task_inventory(packet->payload);
                        if (task_inventory_move &&
                            task_inventory_move->agent_id == identity->agent_id &&
                            task_inventory_move->session_id == identity->session_id) {
                            const auto agent_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            const auto folder_id = homeworldz::viewer::format_uuid(
                                task_inventory_move->folder_id);
                            const auto task_item_id = homeworldz::viewer::format_uuid(
                                task_inventory_move->item_id);
                            const auto* entity = scene.find(task_inventory_move->local_id);
                            std::optional<homeworldz::scene::TaskInventoryItem> task_item;
                            if (entity && entity->owner_id == agent_id) {
                                const auto found = std::find_if(
                                    entity->task_inventory.begin(), entity->task_inventory.end(),
                                    [&](const auto& item) { return item.item_id == task_item_id; });
                                if (found != entity->task_inventory.end() && found->owner_id == agent_id)
                                    task_item = *found;
                            }
                            const auto personal_item_id = homeworldz::viewer::random_uuid();
                            bool created = false;
                            bool removed_from_task = false;
                            if (task_item && viewer_grid) {
                                try {
                                    const homeworldz::grid::InventoryItem personal{
                                        personal_item_id, task_item->creator_id, agent_id,
                                        folder_id, task_item->asset_id, task_item->asset_type,
                                        task_item->inventory_type, task_item->name,
                                        task_item->description, task_item->flags,
                                        task_item->base_permissions,
                                        task_item->current_permissions,
                                        task_item->everyone_permissions,
                                        task_item->next_permissions, task_item->sale_type,
                                        task_item->sale_price};
                                    if ((task_item->current_permissions &
                                            homeworldz::scene::permission_copy) != 0) {
                                        created = viewer_grid->create_inventory_item(agent_id, personal);
                                    } else {
                                        const auto prepared = viewer_grid->prepare_task_inventory_extraction({
                                            homeworldz::viewer::random_uuid(), agent_id,
                                            provisioned_region_id, entity->object_id, task_item_id,
                                            folder_id, personal_item_id, personal});
                                        const auto finalized = prepared
                                            ? apply_task_inventory_extraction(*prepared)
                                            : std::nullopt;
                                        created = finalized.has_value();
                                        removed_from_task = created;
                                    }
                                } catch (const std::exception& error) {
                                    std::cout << "{\"level\":\"error\",\"message\":\"task inventory personal move failed\",\"error\":"
                                              << homeworldz::api::json_string(error.what()) << "}"
                                              << std::endl;
                                }
                            }
                            bool sent = false;
                            if (created && task_item) {
                                const auto item_id = homeworldz::viewer::parse_uuid(personal_item_id);
                                const auto creator_id = homeworldz::viewer::parse_uuid(task_item->creator_id);
                                const auto owner_id = homeworldz::viewer::parse_uuid(agent_id);
                                const auto destination_id = homeworldz::viewer::parse_uuid(folder_id);
                                const auto asset_id = homeworldz::viewer::parse_uuid(task_item->asset_id);
                                if (item_id && creator_id && owner_id && destination_id && asset_id) {
                                    homeworldz::viewer::InventoryItem response_item;
                                    response_item.item_id = *item_id;
                                    response_item.creator_id = *creator_id;
                                    response_item.owner_id = *owner_id;
                                    response_item.folder_id = *destination_id;
                                    response_item.asset_id = *asset_id;
                                    response_item.asset_type = task_item->asset_type;
                                    response_item.inventory_type = task_item->inventory_type;
                                    response_item.name = task_item->name;
                                    response_item.description = task_item->description;
                                    response_item.flags = task_item->flags;
                                    response_item.base_permissions = task_item->base_permissions;
                                    response_item.current_permissions = task_item->current_permissions;
                                    response_item.everyone_permissions = task_item->everyone_permissions;
                                    response_item.next_permissions = task_item->next_permissions;
                                    response_item.sale_type = task_item->sale_type;
                                    response_item.sale_price = task_item->sale_price;
                                    response_item.creation_date = static_cast<std::int32_t>(
                                        std::chrono::duration_cast<std::chrono::seconds>(
                                            std::chrono::system_clock::now().time_since_epoch()).count());
                                    const homeworldz::viewer::AgentMessage reply{
                                        identity->agent_id, identity->session_id};
                                    if (const auto outgoing = circuits.send(
                                            endpoint,
                                            homeworldz::viewer::encode_update_create_inventory_item(
                                                reply, 0, response_item),
                                            true, now, true))
                                        sent = send_udp(viewer_server, endpoint, *outgoing);
                                }
                            }
                            bool task_refresh_sent = false;
                            if (removed_from_task) {
                                const auto* updated = scene.find(task_inventory_move->local_id);
                                const auto task_id = updated
                                    ? homeworldz::viewer::parse_uuid(updated->object_id)
                                    : std::nullopt;
                                if (updated && task_id) {
                                    const auto content = task_inventory_file(*updated);
                                    const auto filename = "inventory_" +
                                        homeworldz::viewer::random_uuid() + ".tmp";
                                    pending_task_inventory_files.insert_or_assign(
                                        endpoint + '|' + filename, content);
                                    auto wire_filename = filename;
                                    wire_filename.push_back('\0');
                                    const auto payload = homeworldz::viewer::encode_reply_task_inventory({
                                        *task_id, static_cast<std::int16_t>(updated->task_inventory_serial),
                                        wire_filename});
                                    if (const auto outgoing = circuits.send(
                                            endpoint, payload, true, now, true))
                                        task_refresh_sent = send_udp(viewer_server, endpoint, *outgoing);
                                }
                            }
                            std::cout << "{\"level\":" << (created ? "\"info\"" : "\"warn\"")
                                      << ",\"message\":\"task inventory personal move "
                                      << (created ? "completed" : "rejected") << "\",\"localId\":"
                                      << task_inventory_move->local_id << ",\"taskItemId\":"
                                      << homeworldz::api::json_string(task_item_id)
                                      << ",\"viewerUpdateSent\":" << (sent ? "true" : "false")
                                      << ",\"removedFromTask\":" << (removed_from_task ? "true" : "false")
                                      << ",\"taskRefreshSent\":" << (task_refresh_sent ? "true" : "false")
                                      << "}" << std::endl;
                        }
                        const auto asset_transfer =
                            homeworldz::viewer::decode_transfer_request(packet->payload);
                        if (asset_transfer) {
                            // Firestorm's script and notecard editors fetch an
                            // inventory item's body over the asset-transfer channel
                            // (TransferRequest -> TransferInfo + TransferPacket(s)).
                            const auto* transfer_identity = circuits.identity(endpoint);
                            const bool authorized =
                                asset_transfer->source_type !=
                                    homeworldz::viewer::transfer_source_sim_inv_item ||
                                (transfer_identity &&
                                 asset_transfer->agent_id == transfer_identity->agent_id &&
                                 asset_transfer->session_id == transfer_identity->session_id);
                            std::int32_t status = homeworldz::viewer::transfer_status_ok;
                            std::vector<std::byte> asset_bytes;
                            if (!authorized) {
                                status = homeworldz::viewer::transfer_status_unknown_source;
                            } else {
                                try {
                                    asset_bytes = read_federated_asset(
                                        homeworldz::viewer::format_uuid(asset_transfer->asset_id));
                                } catch (const std::exception&) {
                                    status = homeworldz::viewer::transfer_status_unknown_source;
                                }
                            }
                            const auto info = homeworldz::viewer::encode_transfer_info(
                                asset_transfer->transfer_id, asset_transfer->channel_type, status,
                                status == homeworldz::viewer::transfer_status_ok
                                    ? static_cast<std::int32_t>(asset_bytes.size())
                                    : 0,
                                asset_transfer->params);
                            if (const auto outgoing = circuits.send(endpoint, info, true, now))
                                send_udp(viewer_server, endpoint, *outgoing);
                            if (status == homeworldz::viewer::transfer_status_ok) {
                                constexpr std::size_t transfer_chunk_size = 1000;
                                std::int32_t packet_number = 0;
                                std::size_t offset = 0;
                                do {
                                    const auto chunk_size = (std::min)(
                                        transfer_chunk_size, asset_bytes.size() - offset);
                                    const bool final = offset + chunk_size >= asset_bytes.size();
                                    const auto data = std::span<const std::byte>(
                                        asset_bytes.data() + offset, chunk_size);
                                    const auto pkt = homeworldz::viewer::encode_transfer_packet(
                                        asset_transfer->transfer_id, asset_transfer->channel_type,
                                        packet_number,
                                        final ? homeworldz::viewer::transfer_status_done
                                              : homeworldz::viewer::transfer_status_ok,
                                        data);
                                    if (const auto outgoing = circuits.send(endpoint, pkt, true, now))
                                        send_udp(viewer_server, endpoint, *outgoing);
                                    offset += chunk_size;
                                    ++packet_number;
                                } while (offset < asset_bytes.size());
                            }
                            std::cout << "{\"level\":" << (status == 0 ? "\"info\"" : "\"warn\"")
                                      << ",\"message\":\"asset transfer "
                                      << (status == 0 ? "served" : "rejected") << "\",\"assetId\":"
                                      << homeworldz::api::json_string(
                                             homeworldz::viewer::format_uuid(asset_transfer->asset_id))
                                      << ",\"sourceType\":" << asset_transfer->source_type
                                      << ",\"bytes\":" << asset_bytes.size() << "}" << std::endl;
                        }
                        const auto task_inventory_xfer =
                            homeworldz::viewer::decode_request_xfer(packet->payload);
                        if (task_inventory_xfer) {
                            const auto pending = pending_task_inventory_files.find(
                                endpoint + '|' + task_inventory_xfer->filename);
                            bool sent = false;
                            if (pending != pending_task_inventory_files.end()) {
                                constexpr std::size_t xfer_chunk_size = 1000;
                                const auto chunk_size = (std::min)(
                                    xfer_chunk_size, pending->second.size());
                                std::vector<std::byte> xfer_data(4);
                                const auto size = static_cast<std::uint32_t>(pending->second.size());
                                for (unsigned index = 0; index < 4; ++index)
                                    xfer_data[index] = static_cast<std::byte>(size >> (index * 8));
                                xfer_data.insert(
                                    xfer_data.end(), pending->second.begin(),
                                    pending->second.begin() + chunk_size);
                                const bool final = chunk_size == pending->second.size();
                                const auto payload = homeworldz::viewer::encode_send_xfer_packet(
                                    task_inventory_xfer->id, final ? 0x80000000U : 0U, xfer_data);
                                if (const auto outgoing = circuits.send(
                                        endpoint, payload, true, now, true))
                                    sent = send_udp(viewer_server, endpoint, *outgoing);
                                if (sent && !final) {
                                    pending_task_inventory_xfers.insert_or_assign(
                                        endpoint + '|' + std::to_string(task_inventory_xfer->id),
                                        PendingTaskInventoryXfer{
                                            std::move(pending->second), chunk_size, 1, 0});
                                }
                                if (sent) pending_task_inventory_files.erase(pending);
                            }
                            std::cout << "{\"level\":" << (sent ? "\"info\"" : "\"warn\"")
                                      << ",\"message\":\"task inventory xfer "
                                      << (sent ? "sent" : "rejected") << "\",\"filename\":"
                                      << homeworldz::api::json_string(task_inventory_xfer->filename)
                                      << "}" << std::endl;
                        }
                        const auto task_inventory_confirmation =
                            homeworldz::viewer::decode_confirm_xfer_packet(packet->payload);
                        if (task_inventory_confirmation) {
                            const auto key = endpoint + '|' +
                                std::to_string(task_inventory_confirmation->id);
                            const auto pending = pending_task_inventory_xfers.find(key);
                            if (pending != pending_task_inventory_xfers.end()) {
                                bool sent = false;
                                bool final = false;
                                std::uint32_t packet_number{};
                                if (task_inventory_confirmation->packet ==
                                    pending->second.awaiting_confirmation &&
                                    pending->second.offset < pending->second.data.size()) {
                                    constexpr std::size_t xfer_chunk_size = 1000;
                                    const auto remaining =
                                        pending->second.data.size() - pending->second.offset;
                                    const auto chunk_size = (std::min)(xfer_chunk_size, remaining);
                                    packet_number = pending->second.next_packet;
                                    final = chunk_size == remaining;
                                    const auto packet_field = final
                                        ? packet_number | 0x80000000U
                                        : packet_number;
                                    const auto begin =
                                        pending->second.data.begin() + pending->second.offset;
                                    const auto payload = homeworldz::viewer::encode_send_xfer_packet(
                                        task_inventory_confirmation->id, packet_field,
                                        std::span<const std::byte>(&*begin, chunk_size));
                                    if (const auto outgoing = circuits.send(
                                            endpoint, payload, true, now, true))
                                        sent = send_udp(viewer_server, endpoint, *outgoing);
                                    if (sent && !final) {
                                        pending->second.offset += chunk_size;
                                        pending->second.awaiting_confirmation = packet_number;
                                        ++pending->second.next_packet;
                                    }
                                    if (sent && final) pending_task_inventory_xfers.erase(pending);
                                }
                                std::cout << "{\"level\":" << (sent ? "\"info\"" : "\"warn\"")
                                          << ",\"message\":\"task inventory xfer continuation "
                                          << (sent ? "sent" : "rejected") << "\",\"packet\":"
                                          << packet_number << ",\"final\":"
                                          << (final ? "true" : "false") << "}" << std::endl;
                            }
                        }
                        const auto create_folder =
                            homeworldz::viewer::decode_create_inventory_folder(packet->payload);
                        if (create_folder && create_folder->agent_id == identity->agent_id &&
                            create_folder->session_id == identity->session_id) {
                            const auto user_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            const auto folder_id = homeworldz::viewer::format_uuid(create_folder->folder_id);
                            const auto parent_id = homeworldz::viewer::format_uuid(create_folder->parent_id);
                            bool created = false;
                            try {
                                created = viewer_grid && viewer_grid->create_inventory_folder(
                                    user_id, folder_id, parent_id, create_folder->name,
                                    static_cast<int>(create_folder->type));
                            } catch (const std::exception& error) {
                                std::cout << "{\"level\":\"error\",\"message\":\"inventory folder creation failed\",\"error\":"
                                          << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                            }
                            std::cout << "{\"level\":" << (created ? "\"info\"" : "\"warn\"")
                                      << ",\"message\":\"inventory folder creation "
                                      << (created ? "completed" : "rejected") << "\",\"folderId\":"
                                      << homeworldz::api::json_string(folder_id) << "}" << std::endl;
                        }
                        const auto move_folders =
                            homeworldz::viewer::decode_move_inventory_folder(packet->payload);
                        if (move_folders && move_folders->agent_id == identity->agent_id &&
                            move_folders->session_id == identity->session_id) {
                            const auto user_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            std::size_t moved = 0;
                            for (const auto& move : move_folders->folders) {
                                const auto folder_id = homeworldz::viewer::format_uuid(move.folder_id);
                                const auto parent_id = homeworldz::viewer::format_uuid(move.parent_id);
                                try {
                                    if (viewer_grid && viewer_grid->move_inventory_folder(
                                            user_id, folder_id, parent_id))
                                        ++moved;
                                } catch (const std::exception& error) {
                                    std::cout << "{\"level\":\"error\",\"message\":\"inventory folder move failed\",\"error\":"
                                              << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                                }
                            }
                            std::cout << "{\"level\":"
                                      << (moved == move_folders->folders.size() ? "\"info\"" : "\"warn\"")
                                      << ",\"message\":\"inventory folder move batch processed\",\"moved\":"
                                      << moved << ",\"requested\":" << move_folders->folders.size() << "}"
                                      << std::endl;
                        }
                        const auto move_items =
                            homeworldz::viewer::decode_move_inventory_item(packet->payload);
                        if (move_items && move_items->agent_id == identity->agent_id &&
                            move_items->session_id == identity->session_id) {
                            const auto user_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            std::size_t moved = 0;
                            for (const auto& move : move_items->items) {
                                const auto item_id = homeworldz::viewer::format_uuid(move.item_id);
                                const auto folder_id = homeworldz::viewer::format_uuid(move.folder_id);
                                try {
                                    if (viewer_grid && viewer_grid->move_inventory_item(
                                            user_id, item_id, folder_id, move.new_name))
                                        ++moved;
                                } catch (const std::exception& error) {
                                    std::cout << "{\"level\":\"error\",\"message\":\"inventory item move failed\",\"error\":"
                                              << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                                }
                            }
                            std::cout << "{\"level\":"
                                      << (moved == move_items->items.size() ? "\"info\"" : "\"warn\"")
                                      << ",\"message\":\"inventory item move batch processed\",\"moved\":"
                                      << moved << ",\"requested\":" << move_items->items.size() << "}"
                                      << std::endl;
                        }
                        const auto copy_item =
                            homeworldz::viewer::decode_copy_inventory_item(packet->payload);
                        if (copy_item && copy_item->agent_id == identity->agent_id &&
                            copy_item->session_id == identity->session_id) {
                            const auto user_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            const auto source_id = homeworldz::viewer::format_uuid(copy_item->old_item_id);
                            const auto destination_id = homeworldz::viewer::format_uuid(copy_item->new_folder_id);
                            const auto source_owner_id = homeworldz::viewer::format_uuid(copy_item->old_agent_id);
                            const bool library_copy = source_owner_id == system_creator_id;
                            std::optional<homeworldz::grid::InventoryItem> copied;
                            try {
                                if (viewer_grid && library_copy) copied = viewer_grid->copy_library_item(
                                    user_id, source_id, destination_id, copy_item->new_name);
                                else if (viewer_grid && source_owner_id == user_id)
                                    copied = viewer_grid->copy_inventory_item(
                                        user_id, source_id, destination_id, copy_item->new_name);
                            } catch (const std::exception& error) {
                                std::cout << "{\"level\":\"error\",\"message\":\"inventory item copy failed\",\"error\":"
                                          << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                            }
                            bool sent = false;
                            if (copied) {
                                const auto item_id = homeworldz::viewer::parse_uuid(copied->item_id);
                                const auto creator_id = homeworldz::viewer::parse_uuid(copied->creator_id);
                                const auto owner_id = homeworldz::viewer::parse_uuid(copied->owner_id);
                                const auto folder_id = homeworldz::viewer::parse_uuid(copied->folder_id);
                                const auto asset_id = homeworldz::viewer::parse_uuid(copied->asset_id);
                                if (item_id && creator_id && owner_id && folder_id && asset_id) {
                                    homeworldz::viewer::InventoryItem item;
                                    item.item_id = *item_id;
                                    item.creator_id = *creator_id;
                                    item.owner_id = *owner_id;
                                    item.folder_id = *folder_id;
                                    item.asset_id = *asset_id;
                                    item.asset_type = static_cast<std::int8_t>(copied->asset_type);
                                    item.inventory_type = static_cast<std::int8_t>(copied->inventory_type);
                                    item.name = copied->name;
                                    item.description = copied->description;
                                    item.flags = copied->flags;
                                    item.base_permissions = copied->base_permissions;
                                    item.current_permissions = copied->current_permissions;
                                    item.everyone_permissions = copied->everyone_permissions;
                                    item.next_permissions = copied->next_permissions;
                                    item.sale_type = static_cast<std::uint8_t>(copied->sale_type);
                                    item.sale_price = copied->sale_price;
                                    item.creation_date = static_cast<std::int32_t>(
                                        std::chrono::duration_cast<std::chrono::seconds>(
                                            std::chrono::system_clock::now().time_since_epoch()).count());
                                    const homeworldz::viewer::AgentMessage reply{
                                        identity->agent_id, identity->session_id};
                                    auto payload = homeworldz::viewer::encode_update_create_inventory_item(
                                        reply, copy_item->callback_id, item);
                                    if (!payload.empty()) {
                                        if (const auto outgoing = circuits.send(
                                                endpoint, std::move(payload), true, now, true))
                                            sent = send_udp(viewer_server, endpoint, *outgoing);
                                    }
                                }
                            }
                            std::cout << "{\"level\":" << (sent ? "\"info\"" : "\"warn\"")
                                      << ",\"message\":\"" << (library_copy ? "library" : "personal")
                                      << " inventory copy "
                                      << (sent ? "completed" : "rejected") << "\",\"sourceItemId\":"
                                      << homeworldz::api::json_string(source_id) << "}" << std::endl;
                        }
                        const auto create_item =
                            homeworldz::viewer::decode_create_inventory_item(packet->payload);
                        if (create_item && create_item->agent_id == identity->agent_id &&
                            create_item->session_id == identity->session_id) {
                            const auto user_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            const auto transaction_id =
                                homeworldz::viewer::format_uuid(create_item->transaction_id);
                            const auto pending =
                                pending_inventory_asset_uploads.find(endpoint + '|' + transaction_id);
                            const bool wearable = create_item->inventory_type == 18 &&
                                (create_item->asset_type == 5 || create_item->asset_type == 13);
                            // Captured before the branches below consume it:
                            // whether an upload was already staged for this
                            // transaction, and for what type. A refusal that
                            // does not say which of these was wrong costs a
                            // deploy to diagnose.
                            const int staged_asset_type =
                                pending == pending_inventory_asset_uploads.end() ?
                                    -1 : pending->second.asset_type;
                            bool created = false;
                            bool consumed_pending_upload = false;
                            homeworldz::grid::InventoryItem item;
                            const bool editable_asset = wearable ||
                                (create_item->asset_type == 7 && create_item->inventory_type == 7) ||
                                (create_item->asset_type == 10 && create_item->inventory_type == 10) ||
                                (create_item->asset_type == 21 && create_item->inventory_type == 20);
                            std::string asset_id;
                            if (editable_asset && pending != pending_inventory_asset_uploads.end() &&
                                pending->second.asset_type == create_item->asset_type) {
                                asset_id = pending->second.asset_id;
                                consumed_pending_upload = true;
                            } else if (create_item->asset_type == 2 &&
                                       create_item->inventory_type == 2) {
                                // A calling card has no asset body. Its asset_id
                                // is the avatar it names, which the viewer sends
                                // in the description as a bare UUID string
                                // (llfriendcard.cpp,
                                // create_agent_calling_card_name_cb, with a null
                                // transaction id because there is nothing to
                                // upload). Firestorm creates the agent's own card
                                // on any login where Friends/All does not hold
                                // one, so refusing this refused every first
                                // login, with an alert naming the avatar.
                                const auto named =
                                    homeworldz::viewer::parse_uuid(create_item->description);
                                asset_id = named ? homeworldz::viewer::format_uuid(*named) :
                                                   homeworldz::viewer::format_uuid(identity->agent_id);
                            } else if (pending == pending_inventory_asset_uploads.end()) {
                                const auto avatar = avatars.find(endpoint);
                                const auto position = avatar == avatars.end() ?
                                    homeworldz::scene::Vector3{128.0, 128.0, 25.0} :
                                    avatar->second.controller.state().position;
                                auto initial_content = homeworldz::inventory::default_asset_content(
                                    create_item->asset_type, create_item->inventory_type,
                                    registration->region_id(), position);
                                // Wearables have no generated stub, so before
                                // 2026-08-07 this branch produced nothing for
                                // them and the request was dropped in silence
                                // — the viewer waited forever for an item it
                                // had asked to create. Seed from the shipped
                                // default for the type instead.
                                if (!initial_content && wearable) {
                                    const auto shipped =
                                        shipped_default_wearable_asset(create_item->wearable_type);
                                    if (!shipped.empty()) {
                                        try {
                                            const auto bytes = read_federated_asset(shipped);
                                            if (!bytes.empty())
                                                initial_content = std::string(
                                                    reinterpret_cast<const char*>(bytes.data()),
                                                    bytes.size());
                                        } catch (const std::exception& error) {
                                            std::cout << "{\"level\":\"error\",\"message\":\"shipped default "
                                                         "wearable unreadable\",\"wearableType\":"
                                                      << static_cast<unsigned int>(create_item->wearable_type)
                                                      << ",\"error\":"
                                                      << homeworldz::api::json_string(error.what())
                                                      << "}" << std::endl;
                                        }
                                    }
                                }
                                if (initial_content && viewer_grid) {
                                    try {
                                        asset_id = homeworldz::viewer::random_uuid();
                                        const auto content = std::span(
                                            reinterpret_cast<const std::byte*>(initial_content->data()),
                                            initial_content->size());
                                        const auto metadata = storage->store_asset(asset_id, user_id, content);
                                        if (!viewer_grid->register_asset(
                                                metadata.viewer_id, metadata.creator_id, metadata.sha256,
                                                metadata.size, region_public_endpoint, true))
                                            asset_id.clear();
                                    } catch (const std::exception& error) {
                                        asset_id.clear();
                                        std::cout << "{\"level\":\"error\",\"message\":\"default inventory asset creation failed\",\"error\":"
                                                  << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                                    }
                                }
                            }
                            if (!asset_id.empty() && viewer_grid) {
                                item.item_id = homeworldz::viewer::random_uuid();
                                item.creator_id = user_id;
                                item.owner_id = user_id;
                                item.folder_id = homeworldz::viewer::format_uuid(create_item->folder_id);
                                item.asset_id = asset_id;
                                item.asset_type = create_item->asset_type;
                                item.inventory_type = create_item->inventory_type;
                                item.name = create_item->name;
                                item.description = create_item->description;
                                item.flags = wearable ? create_item->wearable_type : 0;
                                item.base_permissions = homeworldz::scene::permission_creator;
                                item.current_permissions = homeworldz::scene::permission_creator;
                                item.everyone_permissions = 0x00000000U;
                                item.next_permissions = create_item->next_owner_permissions;
                                try {
                                    created = viewer_grid->create_inventory_item(user_id, item);
                                } catch (const std::exception& error) {
                                    std::cout << "{\"level\":\"error\",\"message\":\"inventory asset item creation failed\",\"error\":"
                                              << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                                }
                            }
                            bool sent = false;
                            if (created) {
                                const auto item_id = homeworldz::viewer::parse_uuid(item.item_id);
                                const auto creator_id = homeworldz::viewer::parse_uuid(item.creator_id);
                                const auto owner_id = homeworldz::viewer::parse_uuid(item.owner_id);
                                const auto folder_id = homeworldz::viewer::parse_uuid(item.folder_id);
                                const auto parsed_asset_id = homeworldz::viewer::parse_uuid(item.asset_id);
                                if (item_id && creator_id && owner_id && folder_id && parsed_asset_id) {
                                    homeworldz::viewer::InventoryItem response_item;
                                    response_item.item_id = *item_id;
                                    response_item.creator_id = *creator_id;
                                    response_item.owner_id = *owner_id;
                                    response_item.folder_id = *folder_id;
                                    response_item.asset_id = *parsed_asset_id;
                                    response_item.asset_type = static_cast<std::int8_t>(item.asset_type);
                                    response_item.inventory_type = static_cast<std::int8_t>(item.inventory_type);
                                    response_item.name = item.name;
                                    response_item.description = item.description;
                                    response_item.flags = item.flags;
                                    response_item.base_permissions = item.base_permissions;
                                    response_item.current_permissions = item.current_permissions;
                                    response_item.everyone_permissions = item.everyone_permissions;
                                    response_item.next_permissions = item.next_permissions;
                                    response_item.creation_date = static_cast<std::int32_t>(
                                        std::chrono::duration_cast<std::chrono::seconds>(
                                            std::chrono::system_clock::now().time_since_epoch()).count());
                                    const homeworldz::viewer::AgentMessage reply{
                                        identity->agent_id, identity->session_id};
                                    if (const auto outgoing = circuits.send(endpoint,
                                            homeworldz::viewer::encode_update_create_inventory_item(
                                                reply, create_item->callback_id, response_item),
                                            true, now, true))
                                        sent = send_udp(viewer_server, endpoint, *outgoing);
                                }
                                if (consumed_pending_upload)
                                    pending_inventory_asset_uploads.erase(pending);
                            }
                            // A creation the region cannot honour must still
                            // answer. There is no failure form of
                            // UpdateCreateInventoryItem, so the viewer is left
                            // waiting on a callback that will never fire; an
                            // alert at least reaches the person, instead of the
                            // request vanishing with only a server log line.
                            if (!(created && sent)) {
                                const auto alert = homeworldz::viewer::encode_agent_alert_message(
                                    identity->agent_id, false,
                                    "Could not create \"" + create_item->name +
                                        "\". The region could not store its contents.");
                                if (const auto outgoing = circuits.send(endpoint, alert, true, now, true))
                                    static_cast<void>(send_udp(viewer_server, endpoint, *outgoing));
                            }
                            std::cout << "{\"level\":" << (created && sent ? "\"info\"" : "\"warn\"")
                                      << ",\"message\":\"inventory asset item "
                                      << (created && sent ? "created" : "rejected") << "\",\"name\":"
                                      << homeworldz::api::json_string(create_item->name)
                                      << ",\"wearableType\":"
                                      << static_cast<unsigned int>(create_item->wearable_type)
                                      // Cast: these are int8_t, and streaming
                                      // one writes the character with that code
                                      // rather than the number. Type 2 logged
                                      // as a raw 0x02 and journalctl then hid
                                      // the whole line as binary.
                                      << ",\"assetType\":" << static_cast<int>(create_item->asset_type)
                                      << ",\"inventoryType\":" << static_cast<int>(create_item->inventory_type)
                                      << ",\"wearable\":" << (wearable ? "true" : "false")
                                      << ",\"stagedAssetType\":" << staged_asset_type
                                      << ",\"assetIdResolved\":" << (asset_id.empty() ? "false" : "true")
                                      << ",\"created\":" << (created ? "true" : "false")
                                      << ",\"sent\":" << (sent ? "true" : "false") << "}"
                                      << std::endl;
                        }
                        const auto handshake_reply = homeworldz::viewer::decode_region_handshake_reply(packet->payload);
                        if (handshake_reply && handshake_reply->agent_id == identity->agent_id &&
                            handshake_reply->session_id == identity->session_id) {
                            handshake_replies.insert(endpoint);
                        }
                        auto cached_texture =
                            homeworldz::viewer::decode_agent_cached_texture(packet->payload);
                        if (cached_texture && cached_texture->agent_id == identity->agent_id &&
                            cached_texture->session_id == identity->session_id) {
                            std::size_t hits = 0;
                            for (auto& query : cached_texture->queries) {
                                const auto asset_id = storage->find_baked_texture(
                                    homeworldz::viewer::format_uuid(query.cache_id), query.texture_index);
                                if (!asset_id) continue;
                                if (const auto parsed = homeworldz::viewer::parse_uuid(*asset_id)) {
                                    query.texture_id = *parsed;
                                    ++hits;
                                }
                            }
                            if (const auto outgoing = circuits.send(endpoint,
                                    homeworldz::viewer::encode_agent_cached_texture_response(*cached_texture),
                                    true, now, true)) {
                                static_cast<void>(send_udp(viewer_server, endpoint, *outgoing));
                                std::cout << "{\"level\":\"info\",\"message\":\"wearable cache response sent\","
                                             "\"hits\":" << hits << ",\"misses\":"
                                          << cached_texture->queries.size() - hits << "}"
                                          << std::endl;
                            }
                        }
                        const auto appearance =
                            homeworldz::viewer::decode_agent_set_appearance(packet->payload);
                        if (appearance && appearance->agent_id == identity->agent_id &&
                            appearance->session_id == identity->session_id) {
                            avatar_appearances.insert_or_assign(endpoint, *appearance);
                            if (const auto geometry = homeworldz::viewer::avatar_geometry(*appearance)) {
                                avatar_geometries[endpoint] = *geometry;
                                if (const auto live = avatars.find(endpoint); live != avatars.end()) {
                                    live->second.controller.set_avatar_geometry(
                                        geometry->height, geometry->hip_offset);
                                    if (physics_world) {
                                        if (live->second.physics_character != 0)
                                            physics_world->remove_character(live->second.physics_character);
                                        live->second.physics_character = physics_world->create_character(
                                            character_definition(live->second.entity_id,
                                                live->second.controller.state().position,
                                                geometry->height));
                                        physics_world->set_character_flying(
                                            live->second.physics_character,
                                            live->second.controller.state().flying);
                                    }
                                }
                                std::cout << "{\"level\":\"info\",\"message\":\"avatar geometry updated\","
                                             "\"height\":" << geometry->height << ",\"hipOffset\":"
                                          << geometry->hip_offset << ",\"visualParams\":"
                                          << appearance->visual_params.size() << "}" << std::endl;
                            }
                            std::size_t stored = 0;
                            for (const auto& entry : appearance->cache_entries) {
                                if (entry.texture_index >= appearance->texture_ids.size()) continue;
                                const auto asset_id = homeworldz::viewer::format_uuid(
                                    appearance->texture_ids[entry.texture_index]);
                                if (!storage->find_asset(asset_id)) continue;
                                storage->store_baked_texture(
                                    homeworldz::viewer::format_uuid(entry.cache_id),
                                    entry.texture_index, asset_id);
                                ++stored;
                            }
                            if (stored != 0)
                                std::cout << "{\"level\":\"info\",\"message\":\"wearable cache updated\","
                                             "\"count\":" << stored << "}" << std::endl;
                            // What the wearer says it is wearing, and whether we
                            // hold it. A cloud is decided by the textures named
                            // here: the avatar renders from these and from
                            // nothing else, so an id we cannot serve is the
                            // difference between a body and a cloud — and until
                            // now nothing recorded them, which left the wearer's
                            // own view the only instrument.
                            //
                            // IMG_DEFAULT_AVATAR (c228d1cf) in a bake slot means
                            // unbaked rather than missing, so it is counted apart
                            // from ids that are simply absent.
                            //
                            // 46697265-7374-6f72-6d00-... is the ASCII bytes of
                            // "Firestorm": the viewer's own client tag, which it
                            // parks in a texture slot to identify itself to other
                            // viewers (its client_list_v2.xml carries the same
                            // id). It names no asset and never will, so counting
                            // it absent reports a defect that cannot be fixed.
                            std::size_t present = 0, absent = 0, unbaked = 0, tags = 0;
                            std::string named;
                            for (const auto& texture : appearance->texture_ids) {
                                const auto id = homeworldz::viewer::format_uuid(texture);
                                if (id == "00000000-0000-0000-0000-000000000000") continue;
                                if (id.starts_with("c228d1cf")) {
                                    ++unbaked;
                                    continue;
                                }
                                if (id.starts_with("46697265-7374-6f72-6d00")) {
                                    ++tags;
                                    continue;
                                }
                                const bool have = storage->find_asset(id).has_value();
                                have ? ++present : ++absent;
                                if (!have) {
                                    if (!named.empty()) named += ',';
                                    named += "\"" + id + "\"";
                                }
                            }
                            // textureEntryBytes and textureSlots are the check on
                            // the three counts above. texture_ids is only filled
                            // when the texture entry parses, so zero of
                            // everything reads the same whether the wearer sent
                            // no textures or sent something we could not read —
                            // two different faults behind one number, which is
                            // the defect this line exists to not have.
                            std::cout << "{\"level\":" << (absent == 0 ? "\"info\"" : "\"warn\"")
                                      << ",\"message\":\"wearer appearance textures\",\"present\":"
                                      << present << ",\"absent\":" << absent << ",\"unbaked\":"
                                      << unbaked << ",\"clientTags\":" << tags
                                      << ",\"textureEntryBytes\":"
                                      << appearance->texture_entry.size() << ",\"textureSlots\":"
                                      << appearance->texture_ids.size() << ",\"appearanceVersion\":"
                                      << static_cast<int>(appearance->appearance_version)
                                      << ",\"visualParams\":" << appearance->visual_params.size()
                                      << ",\"cacheEntries\":" << appearance->cache_entries.size()
                                      << ",\"missing\":[" << named << "]}" << std::endl;
                            // Relay the client's own appearance. But a headless
                            // client echoes back the server-bake UUIDs we seeded it,
                            // in a legacy (v0) message; relayed as-is, viewers would
                            // composite it locally (grey) instead of using the bakes.
                            // Detect our own server bake (system/zero creator) in the
                            // head slot and re-broadcast it as server-side (v1) with
                            // the matching default visual params. A real baker's own
                            // textures — and mid-bake placeholders — are never our
                            // system bake, so they relay unchanged as v0 (no shape
                            // oscillation) and the wearer keeps its local bake.
                            // Relay the client's own appearance untouched. Never
                            // substitute or re-mark it: a real baker's mid-bake
                            // states briefly reference zero-creator textures, and
                            // rewriting them oscillates the avatar. Headless clients
                            // are covered by the join-seed; the server-bake delivery
                            // to viewers is tracked separately (ADR 0029).
                            const auto remote_appearance = homeworldz::viewer::encode_avatar_appearance({
                                identity->agent_id, appearance->serial, appearance->texture_entry,
                                appearance->visual_params});
                            std::size_t recipients = 0;
                            if (!remote_appearance.empty()) {
                                // Echo the completed appearance to the originating viewer as well.
                                for (const auto& [recipient_endpoint, recipient] : avatars) {
                                    static_cast<void>(recipient);
                                    if (const auto outgoing = circuits.send(
                                            recipient_endpoint, remote_appearance, true, now, true)) {
                                        if (send_udp(viewer_server, recipient_endpoint, *outgoing))
                                            ++recipients;
                                    }
                                }
                            }
                            std::cout << "{\"level\":\"info\",\"message\":\"avatar appearance distributed\",\"bytes\":"
                                      << remote_appearance.size() << ",\"recipients\":" << recipients << "}"
                                      << std::endl;
                        }
                        const auto agent_animation =
                            homeworldz::viewer::decode_agent_animation(packet->payload);
                        if (agent_animation && agent_animation->agent_id == identity->agent_id &&
                            agent_animation->session_id == identity->session_id) {
                            auto& animations = avatar_animations[endpoint];
                            auto& next_sequence = next_animation_sequences[endpoint];
                            if (next_sequence < 2) next_sequence = 2;
                            for (const auto& change : agent_animation->animations) {
                                const auto existing = std::find_if(
                                    animations.begin(), animations.end(), [&](const auto& animation) {
                                        return animation.animation_id == change.animation_id;
                                    });
                                if (change.start && existing == animations.end()) {
                                    animations.push_back(
                                        {change.animation_id, next_sequence++, identity->agent_id});
                                } else if (!change.start && existing != animations.end()) {
                                    animations.erase(existing);
                                }
                            }
                            if (animations.empty()) {
                                if (const auto stand = homeworldz::viewer::parse_uuid(
                                        "2408fe9e-df1d-1d7d-f4ff-1384fa7b350f"))
                                    animations.push_back({*stand, 1, identity->agent_id});
                            }
                            const homeworldz::viewer::AvatarAnimation response{
                                identity->agent_id, animations};
                            // Broadcast to every viewer in the region (not just
                            // the emitter) so played animations — including
                            // gesture-triggered ones — are visible to others,
                            // mirroring the locomotion-animation broadcast.
                            const auto payload = homeworldz::viewer::encode_avatar_animation(response);
                            for (const auto& [recipient_endpoint, recipient] : avatars) {
                                static_cast<void>(recipient);
                                if (const auto outgoing = circuits.send(
                                        recipient_endpoint, payload, false, now, true))
                                    static_cast<void>(send_udp(viewer_server, recipient_endpoint, *outgoing));
                            }
                            // Session clients too, or a clip would appear only at
                            // the next movement change: a gesture played while
                            // standing still would never be published at all, and
                            // the avatar would read as idle to one client family
                            // and animated to the other.
                            std::size_t session_told = 0;
                            if (session_server) {
                                if (const auto emitter = avatars.find(endpoint);
                                    emitter != avatars.end()) {
                                    const auto notice =
                                        session_motion_envelope(emitter->second, endpoint);
                                    for (const auto& [recipient_key, recipient] : avatars) {
                                        if (recipient.transport != AvatarTransport::session) continue;
                                        const auto known = session_avatar_interest.find(recipient_key);
                                        if (known == session_avatar_interest.end() ||
                                            !known->second.contains(emitter->second.entity_id))
                                            continue;
                                        session_server->send_to(recipient.session_id, notice);
                                        ++session_told;
                                    }
                                }
                            }
                            std::cout << "{\"level\":\"info\",\"message\":\"avatar animation state updated\","
                                         "\"changes\":" << agent_animation->animations.size()
                                      << ",\"active\":" << animations.size()
                                      << ",\"recipients\":" << avatars.size()
                                      << ",\"sessionClientsTold\":" << session_told << "}" << std::endl;
                        }
                        const auto asset_upload =
                            homeworldz::viewer::decode_asset_upload_request(packet->payload);
                        if (asset_upload) {
                            bool success = false;
                            bool xfer_started = false;
                            homeworldz::viewer::Uuid asset_uuid{};
                            std::string asset_id;
                            const bool editable_inventory_asset =
                                asset_upload->asset_type == 5 || asset_upload->asset_type == 7 ||
                                asset_upload->asset_type == 10 || asset_upload->asset_type == 13 ||
                                asset_upload->asset_type == 21;
                            if (editable_inventory_asset &&
                                !asset_upload->temporary) {
                                try {
                                    const auto session = viewer_sessions ? viewer_sessions->validate(
                                        homeworldz::viewer::format_uuid(identity->session_id)) : std::nullopt;
                                    const auto secure_id = session ?
                                        homeworldz::viewer::parse_uuid(session->secure_session_id) : std::nullopt;
                                    if (!secure_id)
                                        throw std::runtime_error("secure viewer session was unavailable");
                                    asset_uuid = homeworldz::viewer::combine_uuids(
                                        asset_upload->transaction_id, *secure_id);
                                    asset_id = homeworldz::viewer::format_uuid(asset_uuid);
                                    const auto transaction_id =
                                        homeworldz::viewer::format_uuid(asset_upload->transaction_id);
                                    if (asset_upload->data.empty()) {
                                        const auto xfer_id = next_inventory_asset_xfer++;
                                        pending_inventory_asset_xfers.insert_or_assign(
                                            endpoint + '|' + std::to_string(xfer_id),
                                            PendingInventoryAssetXfer{transaction_id, asset_id, asset_uuid,
                                                asset_upload->asset_type, 0, 1000, {}, {}});
                                        if (const auto outgoing = circuits.send(endpoint,
                                                homeworldz::viewer::encode_request_xfer(
                                                    xfer_id, asset_uuid, asset_upload->asset_type),
                                                true, now, true)) {
                                            xfer_started = send_udp(viewer_server, endpoint, *outgoing);
                                        }
                                    } else {
                                        const auto metadata = storage->store_asset(
                                            asset_id, homeworldz::viewer::format_uuid(identity->agent_id),
                                            asset_upload->data);
                                        success = !viewer_grid || viewer_grid->register_asset(
                                            metadata.viewer_id, metadata.creator_id, metadata.sha256,
                                            metadata.size, region_public_endpoint, true);
                                        if (success) {
                                            pending_inventory_asset_uploads.insert_or_assign(
                                                endpoint + '|' + transaction_id,
                                                PendingInventoryAssetUpload{asset_id, asset_upload->asset_type});
                                        }
                                    }
                                } catch (const std::exception& error) {
                                    std::cout << "{\"level\":\"error\",\"message\":\"inventory asset upload failed\","
                                                 "\"error\":" << homeworldz::api::json_string(error.what())
                                              << "}" << std::endl;
                                }
                            }
                            if (!xfer_started) {
                                if (const auto outgoing = circuits.send(endpoint,
                                        homeworldz::viewer::encode_asset_upload_complete(
                                            asset_uuid, asset_upload->asset_type, success), true, now, true))
                                    static_cast<void>(send_udp(viewer_server, endpoint, *outgoing));
                            }
                            std::cout << "{\"level\":" << (success || xfer_started ? "\"info\"" : "\"warn\"")
                                      << ",\"message\":\"inventory asset upload "
                                      << (success ? "stored" : xfer_started ? "transfer requested" : "rejected")
                                      << "\",\"assetId\":"
                                      << homeworldz::api::json_string(asset_id) << ",\"bytes\":"
                                      << asset_upload->data.size() << "}" << std::endl;
                        }
                        const auto xfer_packet =
                            homeworldz::viewer::decode_send_xfer_packet(packet->payload);
                        if (xfer_packet) {
                            const auto key = endpoint + '|' + std::to_string(xfer_packet->id);
                            const auto pending = pending_inventory_asset_xfers.find(key);
                            if (pending != pending_inventory_asset_xfers.end()) {
                                auto& transfer = pending->second;
                                const auto packet_number = xfer_packet->packet & 0x7fffffffU;
                                const bool complete = (xfer_packet->packet & 0x80000000U) != 0;
                                bool accepted = false;
                                if (packet_number == 0 && xfer_packet->data.size() > 4) {
                                    transfer.expected_size =
                                        std::to_integer<std::size_t>(xfer_packet->data[0]) |
                                        (std::to_integer<std::size_t>(xfer_packet->data[1]) << 8) |
                                        (std::to_integer<std::size_t>(xfer_packet->data[2]) << 16) |
                                        (std::to_integer<std::size_t>(xfer_packet->data[3]) << 24);
                                    transfer.packet_size = xfer_packet->data.size() - 4;
                                    if (transfer.expected_size != 0 &&
                                        transfer.expected_size <= 4 * 1024 * 1024 &&
                                        transfer.packet_size <= transfer.expected_size) {
                                        transfer.data.assign(transfer.expected_size, std::byte{});
                                        std::copy(xfer_packet->data.begin() + 4, xfer_packet->data.end(),
                                                  transfer.data.begin());
                                        accepted = true;
                                    }
                                } else if (transfer.expected_size != 0 &&
                                           xfer_packet->data.size() <= transfer.packet_size) {
                                    const auto offset = static_cast<std::size_t>(packet_number) *
                                                        transfer.packet_size;
                                    if (offset + xfer_packet->data.size() <= transfer.data.size()) {
                                        std::copy(xfer_packet->data.begin(), xfer_packet->data.end(),
                                                  transfer.data.begin() + offset);
                                        accepted = true;
                                    }
                                }
                                if (accepted) {
                                    transfer.received_packets.insert(packet_number);
                                    if (const auto outgoing = circuits.send(endpoint,
                                            homeworldz::viewer::encode_confirm_xfer_packet(
                                                xfer_packet->id, packet_number), true, now, true))
                                        static_cast<void>(send_udp(viewer_server, endpoint, *outgoing));
                                }
                                if (accepted && complete) {
                                    bool contiguous = true;
                                    for (std::uint32_t index = 0; index <= packet_number; ++index)
                                        contiguous = contiguous && transfer.received_packets.contains(index);
                                    bool stored = false;
                                    if (contiguous) {
                                        try {
                                            const auto metadata = storage->store_asset(
                                                transfer.asset_id,
                                                homeworldz::viewer::format_uuid(identity->agent_id),
                                                transfer.data);
                                            stored = !viewer_grid || viewer_grid->register_asset(
                                                metadata.viewer_id, metadata.creator_id, metadata.sha256,
                                                metadata.size, region_public_endpoint, true);
                                            if (stored) {
                                                pending_inventory_asset_uploads.insert_or_assign(
                                                    endpoint + '|' + transfer.transaction_id,
                                                    PendingInventoryAssetUpload{
                                                        transfer.asset_id, transfer.asset_type});
                                            }
                                        } catch (const std::exception& error) {
                                            std::cout << "{\"level\":\"error\",\"message\":\"inventory asset transfer failed\","
                                                         "\"error\":"
                                                      << homeworldz::api::json_string(error.what()) << "}"
                                          << std::endl;
                                        }
                                    }
                                    if (const auto outgoing = circuits.send(endpoint,
                                            homeworldz::viewer::encode_asset_upload_complete(
                                                transfer.asset_uuid, transfer.asset_type, stored),
                                            true, now, true))
                                        static_cast<void>(send_udp(viewer_server, endpoint, *outgoing));
                                    std::cout << "{\"level\":" << (stored ? "\"info\"" : "\"warn\"")
                                              << ",\"message\":\"inventory asset transfer "
                                              << (stored ? "stored" : "rejected") << "\",\"assetId\":"
                                              << homeworldz::api::json_string(transfer.asset_id)
                                              << ",\"bytes\":" << transfer.expected_size << "}"
                                              << std::endl;
                                    pending_inventory_asset_xfers.erase(pending);
                                }
                            }
                        }
                        const auto inventory_asset =
                            homeworldz::viewer::decode_update_inventory_asset(packet->payload);
                        if (inventory_asset && inventory_asset->agent_id == identity->agent_id &&
                            inventory_asset->session_id == identity->session_id) {
                            const auto transaction_id =
                                homeworldz::viewer::format_uuid(inventory_asset->transaction_id);
                            const auto pending = pending_inventory_asset_uploads.find(endpoint + '|' + transaction_id);
                            const auto user_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            const auto item_id = homeworldz::viewer::format_uuid(inventory_asset->item_id);
                            bool updated = false;
                            if (pending != pending_inventory_asset_uploads.end() && viewer_grid) {
                                updated = viewer_grid->update_inventory_item_asset(
                                    user_id, item_id, pending->second.asset_id);
                                if (updated) {
                                    if (const auto item = viewer_grid->find_inventory_item(user_id, item_id)) {
                                        homeworldz::viewer::InventoryItem response_item;
                                        const auto parsed_item = homeworldz::viewer::parse_uuid(item->item_id);
                                        const auto creator = homeworldz::viewer::parse_uuid(item->creator_id);
                                        const auto owner = homeworldz::viewer::parse_uuid(item->owner_id);
                                        const auto folder = homeworldz::viewer::parse_uuid(item->folder_id);
                                        const auto asset = homeworldz::viewer::parse_uuid(item->asset_id);
                                        if (parsed_item && creator && owner && folder && asset) {
                                            response_item.item_id = *parsed_item;
                                            response_item.creator_id = *creator;
                                            response_item.owner_id = *owner;
                                            response_item.folder_id = *folder;
                                            response_item.asset_id = *asset;
                                            response_item.asset_type = static_cast<std::int8_t>(item->asset_type);
                                            response_item.inventory_type = static_cast<std::int8_t>(item->inventory_type);
                                            response_item.name = item->name;
                                            response_item.description = item->description;
                                            response_item.flags = item->flags;
                                            response_item.base_permissions = item->base_permissions;
                                            response_item.current_permissions = item->current_permissions;
                                            response_item.everyone_permissions = item->everyone_permissions;
                                            response_item.next_permissions = item->next_permissions;
                                            response_item.sale_type = static_cast<std::uint8_t>(item->sale_type);
                                            response_item.sale_price = item->sale_price;
                                            const homeworldz::viewer::AgentMessage response{
                                                identity->agent_id, identity->session_id};
                                            if (const auto outgoing = circuits.send(endpoint,
                                                    homeworldz::viewer::encode_update_create_inventory_item(
                                                        response, 0, response_item), true, now, true))
                                                static_cast<void>(send_udp(viewer_server, endpoint, *outgoing));
                                        }
                                    }
                                    pending_inventory_asset_uploads.erase(pending);
                                }
                            }
                            std::cout << "{\"level\":" << (updated ? "\"info\"" : "\"warn\"")
                                      << ",\"message\":\"wearable inventory asset "
                                      << (updated ? "updated" : "rejected") << "\",\"itemId\":"
                                      << homeworldz::api::json_string(item_id) << "}" << std::endl;
                        }
                        const auto image_request = homeworldz::viewer::decode_request_image(packet->payload);
                        if (image_request && image_request->agent_id == identity->agent_id &&
                            image_request->session_id == identity->session_id) {
                            for (const auto& requested : image_request->requests) {
                                if (requested.download_priority <= 0.0F) continue;
                                const auto asset_id = homeworldz::viewer::format_uuid(requested.image_id);
                                const auto transfer_key = endpoint + '|' + asset_id;
                                if (active_texture_transfers.contains(transfer_key)) continue;
                                try {
                                    const auto asset = read_federated_asset(asset_id);
                                    auto payloads = homeworldz::viewer::encode_image_transfer(
                                        requested.image_id, asset, requested.packet);
                                    if (payloads.empty()) continue;
                                    active_texture_transfers.insert(transfer_key);
                                    for (std::size_t index = 0; index < payloads.size(); ++index) {
                                        texture_packets[endpoint].push_back(
                                            {asset_id, std::move(payloads[index]), index + 1 == payloads.size()});
                                    }
                                    std::cout << "{\"level\":\"info\",\"message\":\"texture transfer queued\","
                                                 "\"assetId\":" << homeworldz::api::json_string(asset_id)
                                              << ",\"packets\":" << payloads.size() << "}" << std::endl;
                                } catch (const std::exception&) {
                                }
                            }
                        }
                        const auto complete = homeworldz::viewer::decode_complete_agent_movement(packet->payload);
                        const auto provisional_arrival = inbound_transits.authorize(
                            homeworldz::viewer::format_uuid(identity->agent_id),
                            homeworldz::viewer::format_uuid(identity->session_id), now);
                        if (complete && (handshake_replies.contains(endpoint) || provisional_arrival) &&
                            complete->agent_id == identity->agent_id &&
                            complete->session_id == identity->session_id &&
                            complete->circuit_code == identity->circuit_code) {
                            const auto session_id = homeworldz::viewer::format_uuid(identity->session_id);
                            const auto agent_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            std::optional<homeworldz::grid::AvatarTransit> arrival;
                            if (const auto* pending = inbound_transits.authorize(
                                    agent_id, session_id, now)) {
                                try {
                                    const auto activated = viewer_grid && registration ?
                                        viewer_grid->activate_avatar_transit(
                                            pending->id, registration->region_id()) : std::nullopt;
                                    if (!activated || activated->state != "activated")
                                        throw std::runtime_error("grid rejected transit activation");
                                    arrival = inbound_transits.consume(session_id, now);
                                    if (!arrival) throw std::runtime_error("provisional transit expired");
                                    if (viewer_sessions) viewer_sessions->invalidate(session_id);
                                    std::cout << "{\"level\":\"info\",\"message\":\"avatar transit activated\",\"transitId\":"
                                              << homeworldz::api::json_string(arrival->id) << "}" << std::endl;
                                } catch (const std::exception& error) {
                                    if (viewer_grid && registration)
                                        static_cast<void>(viewer_grid->rollback_avatar_transit(
                                            pending->id, registration->region_id(), error.what()));
                                    inbound_transits.remove(session_id);
                                    if (const auto failed = circuits.send(endpoint,
                                            homeworldz::viewer::encode_teleport_failed(
                                                {identity->agent_id, "Destination could not activate the arrival"}),
                                            true, now, true))
                                        static_cast<void>(send_udp(viewer_server, endpoint, *failed));
                                    std::cout << "{\"level\":\"error\",\"message\":\"avatar transit activation failed\",\"error\":"
                                              << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                                    continue;
                                }
                            }
                            homeworldz::viewer::AgentMovementComplete response;
                            response.agent_id = identity->agent_id;
                            response.session_id = identity->session_id;
                            response.region_handle =
                                (static_cast<std::uint64_t>(region_grid_x * 256) << 32) |
                                static_cast<std::uint32_t>(region_grid_y * 256);
                            response.timestamp = static_cast<std::uint32_t>(
                                std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
                            response.channel_version = "Homeworldz " + region_version;
                            if (!avatars.contains(endpoint)) {
                                const auto name = agent_id;
                                homeworldz::scene::EntityId entity{};
                                std::vector<homeworldz::scene::EntityId> duplicates;
                                for (const auto& [candidate_id, candidate] : scene.entities()) {
                                    if (candidate.name != name) continue;
                                    if (candidate_id > entity) {
                                        if (entity != 0) duplicates.push_back(entity);
                                        entity = candidate_id;
                                    } else {
                                        duplicates.push_back(candidate_id);
                                    }
                                }
                                for (const auto duplicate : duplicates) scene.remove(duplicate);
                                if (entity == 0) entity = scene.create(name, initial_spawn);
                                auto* persisted = scene.find(entity);
                                const auto arrival_position = arrival ? homeworldz::scene::Vector3{
                                    arrival->position[0], arrival->position[1], arrival->position[2]} :
                                    initial_spawn;
                                const auto spawn = arrival ? arrival_position :
                                    (persisted ? persisted->position : initial_spawn);
                                const auto known_geometry = avatar_geometries.find(endpoint);
                                const auto geometry = known_geometry == avatar_geometries.end() ?
                                    homeworldz::viewer::AvatarGeometry{} : known_geometry->second;
                                homeworldz::viewer::AvatarController controller{
                                    spawn, collision_ground_height(spawn),
                                    geometry.height, geometry.hip_offset,
                                    static_cast<double>(region_size_x),
                                    static_cast<double>(region_size_y)};
                                if (arrival) {
                                    const auto yaw = std::atan2(arrival->look_at[1], arrival->look_at[0]);
                                    const std::array<float, 3> rotation{
                                        0.0F, 0.0F, static_cast<float>(std::sin(yaw * 0.5))};
                                    controller.restore_motion({}, rotation, arrival->flying);
                                    if (persisted) {
                                        persisted->position = spawn;
                                        persisted->velocity = {};
                                        persisted->rotation = {rotation[0], rotation[1], rotation[2]};
                                        persisted->avatar_flying = arrival->flying;
                                    }
                                } else if (persisted) controller.restore_motion(
                                    persisted->velocity,
                                    {static_cast<float>(persisted->rotation.x),
                                     static_cast<float>(persisted->rotation.y),
                                     static_cast<float>(persisted->rotation.z)},
                                    persisted->avatar_flying);
                                const auto initial_position = controller.state().position;
                                const auto initial_viewer_position = controller.viewer_position();
                                response.position = {static_cast<float>(initial_position.x),
                                                     static_cast<float>(initial_position.y),
                                                     static_cast<float>(initial_position.z)};
                                const auto& rotation = controller.state().rotation;
                                const double qx = rotation[0], qy = rotation[1], qz = rotation[2];
                                const auto qw = std::sqrt((std::max)(
                                    0.0, 1.0 - qx * qx - qy * qy - qz * qz));
                                response.look_at = {
                                    static_cast<float>(1.0 - 2.0 * (qy * qy + qz * qz)),
                                    static_cast<float>(2.0 * (qx * qy + qw * qz)), 0.0F};
                                const auto [avatar_iterator, inserted] = avatars.emplace(endpoint, LiveAvatar{
                                    std::move(controller), entity, name,
                                    now + std::chrono::seconds(5), now + std::chrono::seconds(30),
                                    now + std::chrono::milliseconds(100), initial_viewer_position});
                                static_cast<void>(inserted);
                                avatar_iterator->second.last_pong = now;
                                avatar_iterator->second.transport = AvatarTransport::lludp;
                                avatar_iterator->second.circuit_code = identity->circuit_code;
                                avatar_iterator->second.session_id =
                                    homeworldz::viewer::format_uuid(identity->session_id);
                                push_agent_parcel(avatar_iterator->second);
                                avatar_iterator->second.restored_flying_until =
                                    avatar_iterator->second.controller.state().flying ?
                                        now + std::chrono::seconds(2) : now;
                                if (physics_world) {
                                    auto& live = avatars.at(endpoint);
                                    live.physics_character = physics_world->create_character(
                                        character_definition(entity,
                                            live.controller.state().position,
                                            live.controller.state().height));
                                    physics_world->set_character_velocity(
                                        live.physics_character, live.controller.state().velocity);
                                    physics_world->set_character_flying(
                                        live.physics_character, live.controller.state().flying);
                                }
                                if (viewer_grid && registration)
                                    static_cast<void>(viewer_grid->update_presence(name, registration->region_id()));
                                restore_attachments(name, entity, now);
                            }
                            const auto& live_avatar = avatars.at(endpoint);
                            auto& animations = avatar_animations[endpoint];
                            const auto initial_movement =
                                live_avatar.controller.movement_animation();
                            if (animations.empty()) {
                                if (const auto movement = homeworldz::viewer::parse_uuid(
                                        homeworldz::viewer::movement_animation_id(initial_movement)))
                                    animations.push_back({*movement, 1, identity->agent_id});
                                next_animation_sequences[endpoint] = 2;
                            }
                            movement_animations.insert_or_assign(
                                endpoint, initial_movement);
                            const auto initial_viewer_position = live_avatar.controller.viewer_position();
                            const std::array<float, 3> avatar_position{
                                static_cast<float>(initial_viewer_position.x),
                                static_cast<float>(initial_viewer_position.y),
                                static_cast<float>(initial_viewer_position.z)};
                            const auto new_avatar_update =
                                homeworldz::viewer::encode_avatar_object_update(
                                    response.region_handle,
                                    static_cast<std::uint32_t>(live_avatar.entity_id),
                                    identity->agent_id, avatar_position);
                            for (const auto& [recipient_endpoint, recipient] : avatars) {
                                if (const auto avatar = circuits.send(
                                        recipient_endpoint, new_avatar_update, true, now, true))
                                    static_cast<void>(send_udp(
                                        viewer_server, recipient_endpoint, *avatar));
                                if (recipient_endpoint == endpoint) continue;
                                const auto recipient_position = recipient.controller.viewer_position();
                                const auto recipient_id =
                                    homeworldz::viewer::parse_uuid(recipient.user_id);
                                if (!recipient_id) continue;
                                const auto existing_avatar_update =
                                    homeworldz::viewer::encode_avatar_object_update(
                                        response.region_handle,
                                        static_cast<std::uint32_t>(recipient.entity_id),
                                        *recipient_id,
                                        {static_cast<float>(recipient_position.x),
                                         static_cast<float>(recipient_position.y),
                                         static_cast<float>(recipient_position.z)},
                                        {static_cast<float>(recipient.controller.state().velocity.x),
                                         static_cast<float>(recipient.controller.state().velocity.y),
                                         static_cast<float>(recipient.controller.state().velocity.z)},
                                        recipient.controller.state().rotation);
                                if (const auto avatar = circuits.send(
                                        endpoint, existing_avatar_update, true, now, true))
                                    static_cast<void>(send_udp(viewer_server, endpoint, *avatar));
                            }
                            auto movement_complete =
                                homeworldz::viewer::encode_agent_movement_complete(response);
                            const bool arrival_seed_served = !arrival ||
                                capability_arrival_gate.consume_seed(session_id, arrival->id);
                            if (arrival_seed_served) {
                                if (const auto outgoing = circuits.send(
                                        endpoint, movement_complete, true, now))
                                    static_cast<void>(send_udp(viewer_server, endpoint, *outgoing));
                            } else {
                                pending_agent_movement_completes.push_back({
                                    endpoint, session_id, arrival->id, std::move(movement_complete),
                                    now + std::chrono::milliseconds(500)});
                            }
                            const homeworldz::viewer::AvatarAnimation animation_response{
                                identity->agent_id, animations};
                            const auto new_animation =
                                homeworldz::viewer::encode_avatar_animation(animation_response);
                            for (const auto& [recipient_endpoint, recipient] : avatars) {
                                static_cast<void>(recipient);
                                if (const auto outgoing = circuits.send(
                                        recipient_endpoint, new_animation, false, now))
                                    static_cast<void>(send_udp(
                                        viewer_server, recipient_endpoint, *outgoing));
                            }
                            for (const auto& [animation_endpoint, retained] : avatar_animations) {
                                if (animation_endpoint == endpoint || retained.empty()) continue;
                                const auto existing = avatars.find(animation_endpoint);
                                if (existing == avatars.end()) continue;
                                const auto sender_id =
                                    homeworldz::viewer::parse_uuid(existing->second.user_id);
                                if (!sender_id) continue;
                                const auto retained_animation =
                                    homeworldz::viewer::encode_avatar_animation(
                                        {*sender_id, retained});
                                if (const auto outgoing = circuits.send(
                                        endpoint, retained_animation, false, now))
                                    static_cast<void>(send_udp(viewer_server, endpoint, *outgoing));
                            }
                            for (const auto& [appearance_endpoint, retained] : avatar_appearances) {
                                if (appearance_endpoint == endpoint) continue;
                                const auto retained_appearance =
                                    homeworldz::viewer::encode_avatar_appearance({
                                        retained.agent_id, retained.serial, retained.texture_entry,
                                        retained.visual_params, {}, retained.appearance_version});
                                if (retained_appearance.empty()) continue;
                                if (const auto outgoing = circuits.send(
                                        endpoint, retained_appearance, true, now, true))
                                    static_cast<void>(send_udp(viewer_server, endpoint, *outgoing));
                            }
                            // If this avatar has not supplied an appearance yet,
                            // publish a server-side default-outfit bake so it
                            // rezzes immediately even if its client never bakes.
                            // A later AgentSetAppearance overrides this.
                            if (!avatar_appearances.contains(endpoint)) {
                                if (const auto* bake = ensure_default_outfit_bake()) {
                                    homeworldz::viewer::AgentSetAppearance seeded;
                                    seeded.agent_id = identity->agent_id;
                                    seeded.session_id = identity->session_id;
                                    seeded.serial = 1;
                                    seeded.texture_entry = bake->texture_entry;
                                    seeded.visual_params = default_outfit_visual_params;
                                    seeded.appearance_version = 1;
                                    avatar_appearances.insert_or_assign(endpoint, seeded);
                                    // LMV never sends AgentSetAppearance, so derive
                                    // the avatar's body geometry from the seeded
                                    // default shape too, or its physics capsule
                                    // keeps default dimensions (wrong height ->
                                    // bent-knee stance).
                                    if (const auto geometry =
                                            homeworldz::viewer::avatar_geometry(seeded)) {
                                        avatar_geometries[endpoint] = *geometry;
                                        if (const auto live = avatars.find(endpoint);
                                            live != avatars.end()) {
                                            live->second.controller.set_avatar_geometry(
                                                geometry->height, geometry->hip_offset);
                                            if (physics_world) {
                                                if (live->second.physics_character != 0)
                                                    physics_world->remove_character(
                                                        live->second.physics_character);
                                                live->second.physics_character =
                                                    physics_world->create_character(
                                                        {live->second.entity_id,
                                                         live->second.controller.state().position, 0.3,
                                                         geometry->height, 0.4});
                                                physics_world->set_character_flying(
                                                    live->second.physics_character,
                                                    live->second.controller.state().flying);
                                            }
                                        }
                                    }
                                    const auto seeded_appearance =
                                        homeworldz::viewer::encode_avatar_appearance({
                                            identity->agent_id, 1, bake->texture_entry,
                                            default_outfit_visual_params, {}, std::uint8_t{1}});
                                    // Send the default-outfit bake only to OTHER
                                    // avatars, never back to the joiner: a real
                                    // baker (e.g. Firestorm) would otherwise apply
                                    // this server-side (v1) default to itself and
                                    // lose its own local bake. Its own appearance
                                    // still overrides this entry for others when it
                                    // sends AgentSetAppearance.
                                    if (!seeded_appearance.empty())
                                        for (const auto& [recipient_endpoint, recipient] : avatars) {
                                            static_cast<void>(recipient);
                                            if (recipient_endpoint == endpoint) continue;
                                            if (const auto outgoing = circuits.send(
                                                    recipient_endpoint, seeded_appearance, true, now,
                                                    true))
                                                static_cast<void>(send_udp(
                                                    viewer_server, recipient_endpoint, *outgoing));
                                        }
                                    std::cout << "{\"level\":\"info\",\"message\":\"server default "
                                                 "appearance seeded on join\"}"
                                              << std::endl;
                                }
                            }
                            const auto terrain_patches_per_axis = terrain_width / 16;
                            constexpr std::size_t terrain_patches_per_packet = 16;
                            for (std::size_t y = 0; y < terrain_patches_per_axis; ++y)
                                for (std::size_t first_x = 0; first_x < terrain_patches_per_axis;
                                     first_x += terrain_patches_per_packet) {
                                    std::array<homeworldz::viewer::TerrainPatch,
                                        terrain_patches_per_packet> row{};
                                    const auto count = (std::min)(terrain_patches_per_packet,
                                        terrain_patches_per_axis - first_x);
                                    for (std::size_t index = 0; index < count; ++index)
                                        row[index] = {
                                            static_cast<std::uint8_t>(first_x + index),
                                            static_cast<std::uint8_t>(y)};
                                    const auto terrain_payload = homeworldz::viewer::encode_terrain(
                                        std::span<const homeworldz::viewer::TerrainPatch>(
                                            row.data(), count), *terrain_heightmap);
                                    if (const auto terrain = circuits.send(
                                            endpoint, terrain_payload, true, now))
                                        static_cast<void>(send_udp(viewer_server, endpoint, *terrain));
                                }
                            for (const auto& [entity_id, entity] : scene.entities()) {
                                static_cast<void>(entity_id);
                                const auto restored_object = static_object_from_entity(scene, entity, live_avatar.user_id, falcon);
                                if (!restored_object) continue;
                                if (const auto object = circuits.send(endpoint,
                                        homeworldz::viewer::encode_static_object_update(
                                            response.region_handle, *restored_object), true, now, true))
                                    static_cast<void>(send_udp(viewer_server, endpoint, *object));
                            }
                            // The arrival greeting, privately, once the world
                            // has been backfilled. The name comes from the
                            // account the grid knows, in its legacy two-part
                            // form; the id is the honest fallback rather than
                            // a greeting that lies about knowing you.
                            std::string arrival_name;
                            try {
                                const auto user = viewer_grid
                                    ? viewer_grid->find_user(live_avatar.user_id)
                                    : std::nullopt;
                                if (user) {
                                    auto [first, last] = legacy_avatar_name(user->username);
                                    arrival_name = first + " " + last;
                                }
                            } catch (const std::exception&) {
                            }
                            if (arrival_name.empty()) arrival_name = live_avatar.user_id;
                            if (const auto greeting =
                                    welcome_chat_message(arrival_name, region_name);
                                !greeting.empty()) {
                                homeworldz::viewer::ChatFromSimulator welcome;
                                welcome.from_name = region_name;
                                if (const auto region_uuid =
                                        homeworldz::viewer::parse_uuid(provisioned_region_id)) {
                                    welcome.source_id = *region_uuid;
                                    welcome.owner_id = *region_uuid;
                                }
                                welcome.source_type = 2; // object: the llOwnerSay shape
                                welcome.chat_type = 8;   // owner say: private, plain text
                                const auto& here = live_avatar.controller.state().position;
                                welcome.position = {static_cast<float>(here.x),
                                                    static_cast<float>(here.y),
                                                    static_cast<float>(here.z)};
                                welcome.message = greeting;
                                if (const auto sent = circuits.send(endpoint,
                                        homeworldz::viewer::encode_chat_from_simulator(welcome),
                                        true, now, true))
                                    static_cast<void>(send_udp(viewer_server, endpoint, *sent));
                            }
                        }
                        const auto object_link = homeworldz::viewer::decode_object_link(packet->payload);
                        if (object_link && object_link->agent_id == identity->agent_id &&
                            object_link->session_id == identity->session_id) {
                            const auto user_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            std::unordered_map<homeworldz::scene::EntityId, homeworldz::scene::Entity> originals;
                            std::vector<homeworldz::scene::EntityId> changed;
                            bool valid = object_link->local_ids.size() >= 2;
                            std::unordered_set<std::uint32_t> unique;
                            auto* root = valid ? scene.find(object_link->local_ids.front()) : nullptr;
                            valid = valid && root && root->parent_id == 0 && root->owner_id == user_id &&
                                (root->owner_permissions & homeworldz::scene::permission_modify) != 0;
                            if (valid) {
                                for (const auto local_id : object_link->local_ids) {
                                    auto* entity = scene.find(local_id);
                                    const bool has_children = std::any_of(
                                        scene.entities().begin(), scene.entities().end(),
                                        [local_id](const auto& entry) { return entry.second.parent_id == local_id; });
                                    if (!unique.insert(local_id).second || !entity || entity->parent_id != 0 ||
                                        has_children || entity->owner_id != user_id ||
                                        (entity->owner_permissions & homeworldz::scene::permission_modify) == 0) {
                                        valid = false;
                                        break;
                                    }
                                }
                            }
                            if (valid) {
                                for (std::size_t index = 1; index < object_link->local_ids.size(); ++index) {
                                    auto* child = scene.find(object_link->local_ids[index]);
                                    originals.emplace(child->id, *child);
                                    homeworldz::scene::establish_link(*child, *root);
                                    child->velocity = {};
                                    changed.push_back(child->id);
                                }
                                try {
                                    storage->save_snapshot(scene);
                                } catch (const std::exception& error) {
                                    valid = false;
                                    for (const auto& [entity_id, original] : originals)
                                        if (auto* entity = scene.find(entity_id)) *entity = original;
                                    std::cout << "{\"level\":\"error\",\"message\":\"linkset persistence failed\",\"error\":"
                                              << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                                }
                            }
                            if (valid) {
                                const auto region_handle =
                                    (static_cast<std::uint64_t>(region_grid_x * 256) << 32) |
                                    static_cast<std::uint32_t>(region_grid_y * 256);
                                synchronize_physics_object(*root);
                                std::vector<homeworldz::scene::EntityId> updates{root->id};
                                updates.insert(updates.end(), changed.begin(), changed.end());
                                for (const auto& [recipient_endpoint, recipient] : avatars) {
                                    for (const auto entity_id : updates) {
                                        const auto* entity = scene.find(entity_id);
                                        const auto object = entity
                                            ? static_object_from_entity(scene, *entity, recipient.user_id, falcon) : std::nullopt;
                                        if (!object) continue;
                                        if (const auto sent = circuits.send(recipient_endpoint,
                                                homeworldz::viewer::encode_static_object_update(
                                                    region_handle, *object), true, now, true))
                                            static_cast<void>(send_udp(viewer_server, recipient_endpoint, *sent));
                                    }
                                }
                                // And to session clients. Separate pass rather than a line inside
                                // the loop above: that one is nested avatars-then-entities, so the
                                // inner body runs once per recipient and would send each entity to
                                // every session client as many times as there are viewers present.
                                // See the note at multiple_object_update.
                                for (const auto entity_id : updates)
                                    if (const auto* entity = scene.find(entity_id);
                                        entity && session_object_visible(*entity))
                                        deliver_to_embodied(session_object_envelope(*entity));
                            }
                            std::cout << "{\"level\":" << (valid ? "\"info\"" : "\"warn\"")
                                      << ",\"message\":\"linkset creation "
                                      << (valid ? "completed" : "rejected") << "\",\"prims\":"
                                      << object_link->local_ids.size() << "}" << std::endl;
                        }
                        const auto object_delink = homeworldz::viewer::decode_object_delink(packet->payload);
                        if (object_delink && object_delink->agent_id == identity->agent_id &&
                            object_delink->session_id == identity->session_id) {
                            const auto user_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            std::unordered_map<homeworldz::scene::EntityId, homeworldz::scene::Entity> originals;
                            std::vector<homeworldz::scene::EntityId> changed;
                            std::unordered_set<homeworldz::scene::EntityId> affected_roots;
                            for (const auto local_id : object_delink->local_ids) {
                                auto* entity = scene.find(local_id);
                                if (!entity || entity->parent_id == 0 || entity->owner_id != user_id ||
                                    (entity->owner_permissions & homeworldz::scene::permission_modify) == 0)
                                    continue;
                                originals.emplace(entity->id, *entity);
                                affected_roots.insert(entity->parent_id);
                                entity->parent_id = 0;
                                entity->local_position = {};
                                entity->local_rotation = {};
                                changed.push_back(entity->id);
                            }
                            bool persisted = false;
                            if (!changed.empty()) {
                                try {
                                    storage->save_snapshot(scene);
                                    persisted = true;
                                } catch (const std::exception& error) {
                                    for (const auto& [entity_id, original] : originals)
                                        if (auto* entity = scene.find(entity_id)) *entity = original;
                                    std::cout << "{\"level\":\"error\",\"message\":\"delink persistence failed\",\"error\":"
                                              << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                                }
                            }
                            if (persisted) {
                                const auto region_handle =
                                    (static_cast<std::uint64_t>(region_grid_x * 256) << 32) |
                                    static_cast<std::uint32_t>(region_grid_y * 256);
                                for (const auto root_id : affected_roots)
                                    if (const auto* root = scene.find(root_id))
                                        synchronize_physics_object(*root);
                                for (const auto entity_id : changed) {
                                    const auto* entity = scene.find(entity_id);
                                    if (!entity) continue;
                                    synchronize_physics_object(*entity);
                                    for (const auto& [recipient_endpoint, recipient] : avatars) {
                                        const auto object = static_object_from_entity(scene, *entity, recipient.user_id, falcon);
                                        if (!object) continue;
                                        if (const auto sent = circuits.send(recipient_endpoint,
                                                homeworldz::viewer::encode_static_object_update(
                                                    region_handle, *object), true, now, true))
                                            static_cast<void>(send_udp(viewer_server, recipient_endpoint, *sent));
                                    }
                                    if (session_object_visible(*entity))
                                        deliver_to_embodied(session_object_envelope(*entity));
                                }
                            }
                            std::cout << "{\"level\":" << (persisted ? "\"info\"" : "\"warn\"")
                                      << ",\"message\":\"linkset separation "
                                      << (persisted ? "completed" : "rejected") << "\",\"prims\":"
                                      << changed.size() << "}" << std::endl;
                        }
                        const auto object_select = homeworldz::viewer::decode_object_select(packet->payload);
                        if (object_select && object_select->agent_id == identity->agent_id &&
                            object_select->session_id == identity->session_id) {
                            std::vector<homeworldz::viewer::ObjectProperties> properties;
                            properties.reserve(object_select->local_ids.size());
                            const auto user_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            const auto session_id =
                                homeworldz::viewer::format_uuid(identity->session_id);
                            for (const auto local_id : object_select->local_ids) {
                                const auto* entity = scene.find(local_id);
                                if (!entity) continue;
                                auto& selected = physics_edit_selections[endpoint];
                                if (entity->owner_id == user_id &&
                                    (entity->owner_permissions & homeworldz::scene::permission_modify) != 0 &&
                                    !selected.contains(entity->id)) {
                                    const auto* root = entity->parent_id == 0
                                        ? entity : scene.find(entity->parent_id);
                                    const auto physics_id = root && root->physical ? root->id : entity->id;
                                    selected.emplace(entity->id, physics_id);
                                    ++physics_edit_suspended[physics_id];
                                    if (const auto* physics_entity = scene.find(physics_id))
                                        synchronize_physics_object(*physics_entity);
                                }
                                if (const auto object = object_properties_from_entity(scene, *entity))
                                    properties.push_back(*object);
                                // The viewer's Extra Physics fields have no other
                                // source. Without this they read zero, and editing
                                // any one of them posts those zeros back over the
                                // region's real values.
                                enqueue_viewer_event(
                                    session_id,
                                    homeworldz::viewer::object_physics_properties_event_xml(
                                        physics_properties_of(*entity)));
                            }
                            auto response = homeworldz::viewer::encode_object_properties(properties);
                            if (!response.empty()) {
                                if (const auto outgoing = circuits.send(
                                        endpoint, std::move(response), true, now, true))
                                    static_cast<void>(send_udp(viewer_server, endpoint, *outgoing));
                            }
                        }
                        const auto object_deselect =
                            homeworldz::viewer::decode_object_deselect(packet->payload);
                        if (object_deselect && object_deselect->agent_id == identity->agent_id &&
                            object_deselect->session_id == identity->session_id) {
                            for (const auto local_id : object_deselect->local_ids) {
                                const auto selected = physics_edit_selections.find(endpoint);
                                if (selected == physics_edit_selections.end())
                                    continue;
                                const auto selected_part = selected->second.find(local_id);
                                if (selected_part == selected->second.end()) continue;
                                const auto physics_id = selected_part->second;
                                selected->second.erase(selected_part);
                                const auto suspended = physics_edit_suspended.find(physics_id);
                                if (suspended != physics_edit_suspended.end() &&
                                    --suspended->second == 0) {
                                    physics_edit_suspended.erase(suspended);
                                    if (auto* entity = scene.find(physics_id)) {
                                        const auto original_position = entity->position;
                                        const auto original_velocity = entity->velocity;
                                        if (raise_physical_object_above_terrain(*entity)) {
                                            try {
                                                storage->save_snapshot(scene);
                                            } catch (const std::exception& error) {
                                                entity->position = original_position;
                                                entity->velocity = original_velocity;
                                                std::cerr << "{\"level\":\"error\",\"message\":\"terrain-safe physics reactivation persistence failed\",\"entityId\":"
                                                          << entity->id << ",\"error\":"
                                                          << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                                            }
                                        }
                                        synchronize_physics_object(*entity);
                                    }
                                }
                            }
                        }
                        const auto grab =
                            homeworldz::viewer::decode_object_grab(packet->payload);
                        if (grab && grab->agent_id == identity->agent_id &&
                            grab->session_id == identity->session_id) {
                            // The initial touch (ObjectGrab), distinct from the
                            // physical drag path carried by ObjectGrabUpdate, is
                            // the only trigger for touch_start; drag motion must
                            // not synthesize duplicate touch events.
                            if (const auto* clicked = scene.find(grab->local_id)) {
                                std::size_t fired =
                                    falcon.dispatch_touch_start(clicked->object_id, 1);
                                const auto root_id = clicked->parent_id == 0
                                    ? clicked->id : clicked->parent_id;
                                if (root_id != clicked->id) {
                                    if (const auto* root = scene.find(root_id))
                                        fired += falcon.dispatch_touch_start(
                                            root->object_id, 1);
                                }
                                if (fired != 0)
                                    std::cerr << "{\"level\":\"info\",\"message\":\"Falcon touch_start dispatched\",\"localId\":"
                                              << grab->local_id << ",\"scripts\":" << fired
                                              << "}" << std::endl;
                            }
                        }
                        const auto grab_update =
                            homeworldz::viewer::decode_object_grab_update(packet->payload);
                        if (grab_update && grab_update->agent_id == identity->agent_id &&
                            grab_update->session_id == identity->session_id && physics_world &&
                            physics_scene) {
                            const auto object_id = homeworldz::viewer::format_uuid(grab_update->object_id);
                            const auto user_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            for (const auto& [entity_id, clicked] : scene.entities()) {
                                if (clicked.object_id != object_id) continue;
                                const auto root_id = clicked.parent_id == 0 ? entity_id : clicked.parent_id;
                                const auto* entity = scene.find(root_id);
                                if (!entity) break;
                                const bool may_move = entity->owner_id == user_id ||
                                    (entity->everyone_permissions & homeworldz::scene::permission_move) != 0;
                                if (!may_move || !entity->physical || entity->phantom ||
                                    physics_edit_suspended.contains(root_id))
                                    break;
                                const auto body_id = physics_scene->body_id(root_id);
                                const auto state = physics_world->body_state(body_id);
                                if (!state) break;
                                constexpr double grab_response_seconds = 0.25;
                                constexpr double maximum_delta_speed = 10.0;
                                // Firestorm sends the desired world-space position of the
                                // originally clicked point, plus that point's offset from the
                                // object origin in object-local coordinates. Preserve the
                                // offset as the body rotates instead of pulling the origin to
                                // the surface point.
                                homeworldz::scene::Vector3 local_offset{
                                    grab_update->grab_offset_initial[0],
                                    grab_update->grab_offset_initial[1],
                                    grab_update->grab_offset_initial[2]};
                                if (clicked.parent_id != 0) {
                                    const auto squared = clicked.local_rotation.x * clicked.local_rotation.x +
                                        clicked.local_rotation.y * clicked.local_rotation.y +
                                        clicked.local_rotation.z * clicked.local_rotation.z;
                                    const std::array<double, 4> child_rotation{
                                        clicked.local_rotation.x, clicked.local_rotation.y,
                                        clicked.local_rotation.z,
                                        std::sqrt((std::max)(0.0, 1.0 - squared))};
                                    const auto rotated_offset = homeworldz::physics::rotate_vector(
                                        local_offset, child_rotation);
                                    local_offset = {
                                        clicked.local_position.x + rotated_offset.x,
                                        clicked.local_position.y + rotated_offset.y,
                                        clicked.local_position.z + rotated_offset.z};
                                }
                                const auto world_offset = homeworldz::physics::rotate_vector(
                                    local_offset, state->rotation);
                                const homeworldz::scene::Vector3 target_origin{
                                    grab_update->grab_position[0] - world_offset.x,
                                    grab_update->grab_position[1] - world_offset.y,
                                    grab_update->grab_position[2] - world_offset.z};
                                homeworldz::scene::Vector3 delta_velocity{
                                    (target_origin.x - state->position.x) /
                                        grab_response_seconds - state->linear_velocity.x,
                                    (target_origin.y - state->position.y) /
                                        grab_response_seconds - state->linear_velocity.y,
                                    (target_origin.z - state->position.z) /
                                        grab_response_seconds - state->linear_velocity.z};
                                const auto delta_speed = std::sqrt(
                                    delta_velocity.x * delta_velocity.x +
                                    delta_velocity.y * delta_velocity.y +
                                    delta_velocity.z * delta_velocity.z);
                                if (delta_speed > maximum_delta_speed) {
                                    const auto scale = maximum_delta_speed / delta_speed;
                                    delta_velocity.x *= scale;
                                    delta_velocity.y *= scale;
                                    delta_velocity.z *= scale;
                                }
                                const auto mass = homeworldz::physics::linkset_mass(scene, *entity);
                                physics_world->apply_impulse(body_id, {
                                    delta_velocity.x * mass, delta_velocity.y * mass,
                                    delta_velocity.z * mass});
                                break;
                            }
                        }
                        const auto family_request =
                            homeworldz::viewer::decode_request_object_properties_family(packet->payload);
                        if (family_request && family_request->agent_id == identity->agent_id &&
                            family_request->session_id == identity->session_id) {
                            const auto requested_id = homeworldz::viewer::format_uuid(family_request->object_id);
                            for (const auto& [entity_id, entity] : scene.entities()) {
                                static_cast<void>(entity_id);
                                if (entity.object_id != requested_id) continue;
                                const auto properties = object_properties_from_entity(scene, entity);
                                if (properties) {
                                    auto response = homeworldz::viewer::encode_object_properties_family(
                                        family_request->request_flags, *properties);
                                    if (const auto outgoing = circuits.send(
                                            endpoint, std::move(response), true, now, true))
                                        static_cast<void>(send_udp(viewer_server, endpoint, *outgoing));
                                }
                                break;
                            }
                        }
                        const auto transform_update =
                            homeworldz::viewer::decode_multiple_object_update(packet->payload);
                        if (transform_update && transform_update->agent_id == identity->agent_id &&
                            transform_update->session_id == identity->session_id) {
                            std::unordered_map<homeworldz::scene::EntityId, homeworldz::scene::Entity> originals;
                            std::unordered_set<homeworldz::scene::EntityId> requested_entities;
                            std::unordered_set<homeworldz::scene::EntityId> changed_roots;
                            std::unordered_set<homeworldz::scene::EntityId> changed_children;
                            std::unordered_set<homeworldz::scene::EntityId> changed_root_frames;
                            std::unordered_map<homeworldz::scene::EntityId,
                                homeworldz::scene::Vector3> linked_scale_factors;
                            const auto user_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            for (const auto& update : transform_update->objects) {
                                auto* entity = scene.find(update.local_id);
                                if (!entity) continue;
                                requested_entities.insert(entity->id);
                                if (entity->owner_id != user_id ||
                                    (entity->owner_permissions & homeworldz::scene::permission_modify) == 0)
                                    continue;
                                const auto finite_vector = [](const std::array<float, 3>& value) {
                                    return std::all_of(value.begin(), value.end(),
                                        [](float component) { return std::isfinite(component); });
                                };
                                const bool valid_position = !update.position ||
                                    (finite_vector(*update.position) &&
                                     (entity->parent_id != 0
                                         ? std::all_of(update.position->begin(), update.position->end(),
                                               [](float component) {
                                                   return component >= -4096.0F && component <= 4096.0F;
                                               })
                                         : ((*update.position)[0] >= 0.0F &&
                                            (*update.position)[0] <= static_cast<float>(region_size_x) &&
                                            (*update.position)[1] >= 0.0F &&
                                            (*update.position)[1] <= static_cast<float>(region_size_y) &&
                                            (*update.position)[2] >= -64.0F && (*update.position)[2] <= 4096.0F)));
                                const bool valid_rotation = !update.rotation ||
                                    (finite_vector(*update.rotation) &&
                                     std::all_of(update.rotation->begin(), update.rotation->end(),
                                         [](float component) { return component >= -1.0F && component <= 1.0F; }) &&
                                     ((*update.rotation)[0] * (*update.rotation)[0] +
                                      (*update.rotation)[1] * (*update.rotation)[1] +
                                      (*update.rotation)[2] * (*update.rotation)[2]) <= 1.001F);
                                const bool valid_scale = !update.scale ||
                                    (finite_vector(*update.scale) &&
                                     std::all_of(update.scale->begin(), update.scale->end(),
                                         [](float component) { return component >= 0.01F && component <= 64.0F; }));
                                if (!valid_position || !valid_rotation || !valid_scale) continue;
                                if (!update.position && !update.rotation && !update.scale) continue;
                                originals.try_emplace(entity->id, *entity);
                                const bool linked_update = (update.type & 0x08) != 0;
                                auto& position = entity->parent_id == 0
                                    ? entity->position : entity->local_position;
                                auto& rotation = entity->parent_id == 0
                                    ? entity->rotation : entity->local_rotation;
                                if (update.position)
                                    position = {(*update.position)[0], (*update.position)[1],
                                                (*update.position)[2]};
                                if (update.rotation)
                                    rotation = {(*update.rotation)[0], (*update.rotation)[1],
                                                (*update.rotation)[2]};
                                if (update.scale && entity->parent_id == 0 && linked_update) {
                                    homeworldz::scene::Vector3 factors{
                                        (*update.scale)[0] / entity->scale.x,
                                        (*update.scale)[1] / entity->scale.y,
                                        (*update.scale)[2] / entity->scale.z};
                                    for (const auto& [candidate_id, candidate] : scene.entities()) {
                                        static_cast<void>(candidate_id);
                                        if (candidate.parent_id != entity->id) continue;
                                        factors.x = std::clamp(
                                            factors.x, 0.01 / candidate.scale.x, 64.0 / candidate.scale.x);
                                        factors.y = std::clamp(
                                            factors.y, 0.01 / candidate.scale.y, 64.0 / candidate.scale.y);
                                        factors.z = std::clamp(
                                            factors.z, 0.01 / candidate.scale.z, 64.0 / candidate.scale.z);
                                    }
                                    entity->scale = {
                                        entity->scale.x * factors.x,
                                        entity->scale.y * factors.y,
                                        entity->scale.z * factors.z};
                                    linked_scale_factors.emplace(entity->id, factors);
                                } else if (update.scale) {
                                    entity->scale = {
                                        (*update.scale)[0], (*update.scale)[1], (*update.scale)[2]};
                                }
                                if (entity->parent_id == 0) {
                                    if (linked_update) {
                                        if (update.position || update.rotation || update.scale)
                                            changed_roots.insert(entity->id);
                                    } else if (update.position || update.rotation) {
                                        changed_root_frames.insert(entity->id);
                                    }
                                } else {
                                    changed_children.insert(entity->id);
                                }
                            }
                            for (const auto& [entity_id, current] : scene.entities()) {
                                if (current.parent_id == 0) continue;
                                const auto* root = scene.find(current.parent_id);
                                auto* child = scene.find(entity_id);
                                if (!root || !child) continue;
                                if (changed_root_frames.contains(current.parent_id)) {
                                    originals.try_emplace(child->id, *child);
                                    homeworldz::scene::establish_link(*child, *root);
                                    requested_entities.insert(child->id);
                                }
                                const auto scaled = linked_scale_factors.find(current.parent_id);
                                if (scaled != linked_scale_factors.end()) {
                                    originals.try_emplace(child->id, *child);
                                    homeworldz::scene::scale_linked_child(*child, scaled->second);
                                    requested_entities.insert(child->id);
                                }
                            }
                            for (const auto& [entity_id, current] : scene.entities()) {
                                if (current.parent_id == 0 ||
                                    (!changed_roots.contains(current.parent_id) &&
                                     !changed_children.contains(entity_id)))
                                    continue;
                                const auto* root = scene.find(current.parent_id);
                                auto* child = scene.find(entity_id);
                                if (!root || !child) continue;
                                originals.try_emplace(child->id, *child);
                                homeworldz::scene::update_linked_world_transform(*child, *root);
                                requested_entities.insert(child->id);
                            }
                            bool persisted = false;
                            if (!originals.empty()) {
                                try {
                                    storage->save_snapshot(scene);
                                    persisted = true;
                                } catch (const std::exception& error) {
                                    for (const auto& [entity_id, original] : originals)
                                        if (auto* entity = scene.find(entity_id)) *entity = original;
                                    std::cout << "{\"level\":\"error\",\"message\":\"primitive update persistence failed\",\"error\":"
                                              << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                                }
                            }
                            const auto region_handle =
                                (static_cast<std::uint64_t>(region_grid_x * 256) << 32) |
                                static_cast<std::uint32_t>(region_grid_y * 256);
                            for (const auto entity_id : requested_entities) {
                                const auto* entity = scene.find(entity_id);
                                if (!entity) continue;
                                if (persisted) synchronize_physics_object(*entity);
                                for (const auto& [recipient_endpoint, recipient] : avatars) {
                                    const auto object = static_object_from_entity(scene, *entity, recipient.user_id, falcon);
                                    if (!object) continue;
                                    if (const auto sent = circuits.send(recipient_endpoint,
                                            homeworldz::viewer::encode_static_object_update(
                                                region_handle, *object), true, now, true))
                                        static_cast<void>(send_udp(viewer_server, recipient_endpoint, *sent));
                                }
                                // Session clients get the move as well. Until 2026-08-08 this
                                // loop, and twelve like it, sent only over UDP: every
                                // viewer-originated object change reached other viewers and no
                                // session client. A session client therefore pinned each object
                                // at the position it held when the scene arrived and never moved
                                // it again, with no dropped frame or error to reveal it — the
                                // world was simply, quietly wrong. Removal was the exception,
                                // because all four kill paths deliver to both transports, which
                                // is why this surfaced as "kills arrive, inserts never do".
                                if (session_object_visible(*entity))
                                    deliver_to_embodied(session_object_envelope(*entity));
                            }
                            if (persisted) {
                                std::cout << "{\"level\":\"info\",\"message\":\"primitive transforms updated\",\"count\":"
                                          << originals.size() << "}" << std::endl;
                            }
                        }
                        const auto object_name = homeworldz::viewer::decode_object_name(packet->payload);
                        if (object_name && object_name->agent_id == identity->agent_id &&
                            object_name->session_id == identity->session_id) {
                            std::unordered_map<homeworldz::scene::EntityId, std::string> original_names;
                            std::unordered_set<homeworldz::scene::EntityId> requested_entities;
                            const auto user_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            for (const auto& update : object_name->objects) {
                                auto* entity = scene.find(update.local_id);
                                if (!entity) continue;
                                requested_entities.insert(entity->id);
                                if (entity->owner_id != user_id ||
                                    (entity->owner_permissions & homeworldz::scene::permission_modify) == 0)
                                    continue;
                                original_names.try_emplace(entity->id, entity->name);
                                entity->name = update.name;
                            }
                            bool persisted = false;
                            if (!original_names.empty()) {
                                try {
                                    storage->save_snapshot(scene);
                                    persisted = true;
                                } catch (const std::exception& error) {
                                    for (const auto& [entity_id, original_name] : original_names) {
                                        if (auto* entity = scene.find(entity_id)) entity->name = original_name;
                                    }
                                    std::cout << "{\"level\":\"error\",\"message\":\"primitive name persistence failed\",\"error\":"
                                              << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                                }
                            }
                            std::vector<homeworldz::viewer::ObjectProperties> properties;
                            properties.reserve(requested_entities.size());
                            for (const auto entity_id : requested_entities) {
                                if (const auto* entity = scene.find(entity_id)) {
                                    if (const auto current = object_properties_from_entity(scene, *entity))
                                        properties.push_back(*current);
                                }
                            }
                            auto response = homeworldz::viewer::encode_object_properties(properties);
                            if (!response.empty()) {
                                if (const auto outgoing = circuits.send(
                                        endpoint, std::move(response), true, now, true))
                                    static_cast<void>(send_udp(viewer_server, endpoint, *outgoing));
                            }
                            if (persisted) {
                                std::cout << "{\"level\":\"info\",\"message\":\"primitive names updated\",\"count\":"
                                          << original_names.size() << "}" << std::endl;
                            }
                        }
                        const auto object_description =
                            homeworldz::viewer::decode_object_description(packet->payload);
                        if (object_description && object_description->agent_id == identity->agent_id &&
                            object_description->session_id == identity->session_id) {
                            std::unordered_map<homeworldz::scene::EntityId, std::string> original_descriptions;
                            std::unordered_set<homeworldz::scene::EntityId> requested_entities;
                            const auto user_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            for (const auto& update : object_description->objects) {
                                auto* entity = scene.find(update.local_id);
                                if (!entity) continue;
                                requested_entities.insert(entity->id);
                                if (entity->owner_id != user_id ||
                                    (entity->owner_permissions & homeworldz::scene::permission_modify) == 0)
                                    continue;
                                original_descriptions.try_emplace(entity->id, entity->description);
                                entity->description = update.description;
                            }
                            bool persisted = false;
                            if (!original_descriptions.empty()) {
                                try {
                                    storage->save_snapshot(scene);
                                    persisted = true;
                                } catch (const std::exception& error) {
                                    for (const auto& [entity_id, original_description] : original_descriptions) {
                                        if (auto* entity = scene.find(entity_id))
                                            entity->description = original_description;
                                    }
                                    std::cout << "{\"level\":\"error\",\"message\":\"primitive description persistence failed\",\"error\":"
                                              << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                                }
                            }
                            std::vector<homeworldz::viewer::ObjectProperties> properties;
                            properties.reserve(requested_entities.size());
                            for (const auto entity_id : requested_entities) {
                                if (const auto* entity = scene.find(entity_id)) {
                                    if (const auto current = object_properties_from_entity(scene, *entity))
                                        properties.push_back(*current);
                                }
                            }
                            auto response = homeworldz::viewer::encode_object_properties(properties);
                            if (!response.empty()) {
                                if (const auto outgoing = circuits.send(
                                        endpoint, std::move(response), true, now, true))
                                    static_cast<void>(send_udp(viewer_server, endpoint, *outgoing));
                            }
                            if (persisted) {
                                std::cout << "{\"level\":\"info\",\"message\":\"primitive descriptions updated\",\"count\":"
                                          << original_descriptions.size() << "}" << std::endl;
                            }
                        }
                        const auto object_permissions =
                            homeworldz::viewer::decode_object_permissions(packet->payload);
                        if (object_permissions && object_permissions->agent_id == identity->agent_id &&
                            object_permissions->session_id == identity->session_id) {
                            struct PermissionState {
                                std::uint32_t owner;
                                std::uint32_t group;
                                std::uint32_t everyone;
                                std::uint32_t next_owner;
                            };
                            std::unordered_map<homeworldz::scene::EntityId, PermissionState> originals;
                            std::unordered_set<homeworldz::scene::EntityId> requested_entities;
                            const auto user_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            if (!object_permissions->override_permissions) {
                                for (const auto& update : object_permissions->objects) {
                                    auto* entity = scene.find(update.local_id);
                                    if (!entity) continue;
                                    requested_entities.insert(entity->id);
                                    const PermissionState before{
                                        entity->owner_permissions, entity->group_permissions,
                                        entity->everyone_permissions, entity->next_owner_permissions};
                                    if (!homeworldz::scene::apply_permission_update(
                                            *entity, user_id, update.field, update.set, update.mask))
                                        continue;
                                    const PermissionState after{
                                        entity->owner_permissions, entity->group_permissions,
                                        entity->everyone_permissions, entity->next_owner_permissions};
                                    if (before.owner != after.owner || before.group != after.group ||
                                        before.everyone != after.everyone ||
                                        before.next_owner != after.next_owner)
                                        originals.try_emplace(entity->id, before);
                                }
                            }
                            bool persisted = false;
                            if (!originals.empty()) {
                                try {
                                    storage->save_snapshot(scene);
                                    persisted = true;
                                } catch (const std::exception& error) {
                                    for (const auto& [entity_id, original] : originals) {
                                        if (auto* entity = scene.find(entity_id)) {
                                            entity->owner_permissions = original.owner;
                                            entity->group_permissions = original.group;
                                            entity->everyone_permissions = original.everyone;
                                            entity->next_owner_permissions = original.next_owner;
                                        }
                                    }
                                    std::cout << "{\"level\":\"error\",\"message\":\"primitive permission persistence failed\",\"error\":"
                                              << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                                }
                            }
                            std::vector<homeworldz::viewer::ObjectProperties> properties;
                            properties.reserve(requested_entities.size());
                            for (const auto entity_id : requested_entities) {
                                if (const auto* entity = scene.find(entity_id)) {
                                    if (const auto current = object_properties_from_entity(scene, *entity))
                                        properties.push_back(*current);
                                }
                            }
                            auto response = homeworldz::viewer::encode_object_properties(properties);
                            if (!response.empty()) {
                                if (const auto outgoing = circuits.send(
                                        endpoint, std::move(response), true, now, true))
                                    static_cast<void>(send_udp(viewer_server, endpoint, *outgoing));
                            }
                            if (persisted) {
                                const auto region_handle =
                                    (static_cast<std::uint64_t>(region_grid_x * 256) << 32) |
                                    static_cast<std::uint32_t>(region_grid_y * 256);
                                for (const auto& [entity_id, original] : originals) {
                                    static_cast<void>(original);
                                    const auto* entity = scene.find(entity_id);
                                    if (!entity) continue;
                                    for (const auto& [recipient_endpoint, recipient] : avatars) {
                                        const auto object = static_object_from_entity(scene, *entity, recipient.user_id, falcon);
                                        if (!object) continue;
                                        if (const auto sent = circuits.send(recipient_endpoint,
                                                homeworldz::viewer::encode_static_object_update(
                                                    region_handle, *object), true, now, true))
                                            static_cast<void>(send_udp(
                                                viewer_server, recipient_endpoint, *sent));
                                    }
                                    if (session_object_visible(*entity))
                                        deliver_to_embodied(session_object_envelope(*entity));
                                }
                                std::cout << "{\"level\":\"info\",\"message\":\"primitive permissions updated\",\"count\":"
                                          << originals.size() << "}" << std::endl;
                            }
                        }
                        const auto object_duplicate =
                            homeworldz::viewer::decode_object_duplicate(packet->payload);
                        if (object_duplicate && object_duplicate->agent_id == identity->agent_id &&
                            object_duplicate->session_id == identity->session_id) {
                            constexpr std::uint32_t create_selected = 0x00000002;
                            const auto finite_offset = std::all_of(
                                object_duplicate->offset.begin(), object_duplicate->offset.end(),
                                [](float component) { return std::isfinite(component); });
                            const auto supported_flags =
                                (object_duplicate->duplicate_flags & ~create_selected) == 0;
                            const auto user_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            std::vector<homeworldz::scene::EntityId> created_entities;
                            std::unordered_set<homeworldz::scene::EntityId> requested_roots;
                            if (finite_offset && supported_flags) {
                                for (const auto local_id : object_duplicate->local_ids) {
                                    const auto* selected = scene.find(local_id);
                                    if (!selected) continue;
                                    const auto root_id = selected->parent_id != 0
                                        ? selected->parent_id : selected->id;
                                    if (!requested_roots.insert(root_id).second) continue;
                                    const auto* source = scene.find(root_id);
                                    if (!source || source->owner_id != user_id)
                                        continue;
                                    std::vector<const homeworldz::scene::Entity*> source_children;
                                    bool permitted = true;
                                    for (const auto& [candidate_id, candidate] : scene.entities()) {
                                        static_cast<void>(candidate_id);
                                        if (candidate.parent_id != root_id) continue;
                                        if (candidate.owner_id != user_id) {
                                            permitted = false;
                                            break;
                                        }
                                        source_children.push_back(&candidate);
                                    }
                                    const auto folded = homeworldz::scene::effective_permissions(scene, *source);
                                    if (!permitted ||
                                        (folded.owner & homeworldz::scene::permission_copy) == 0)
                                        continue;
                                    const auto source_copy = *source;
                                    const homeworldz::scene::Vector3 position{
                                        source_copy.position.x + object_duplicate->offset[0],
                                        source_copy.position.y + object_duplicate->offset[1],
                                        source_copy.position.z + object_duplicate->offset[2]};
                                    if (position.x < 0.0 || position.x > region_size_x ||
                                        position.y < 0.0 || position.y > region_size_y ||
                                        position.z < -64.0 || position.z > 4096.0)
                                        continue;
                                    const auto duplicate_root_id = scene.create(source_copy.name, position);
                                    auto* duplicate = scene.find(duplicate_root_id);
                                    if (!duplicate) continue;
                                    *duplicate = source_copy;
                                    duplicate->id = duplicate_root_id;
                                    duplicate->parent_id = 0;
                                    duplicate->local_position = {};
                                    duplicate->local_rotation = {};
                                    duplicate->position = position;
                                    duplicate->velocity = {};
                                    duplicate->object_id = homeworldz::viewer::random_uuid();
                                    regenerate_task_inventory_item_ids(*duplicate);
                                    duplicate->creation_date = static_cast<std::uint64_t>(
                                        std::chrono::duration_cast<std::chrono::seconds>(
                                            std::chrono::system_clock::now().time_since_epoch()).count());
                                    created_entities.push_back(duplicate_root_id);
                                    for (const auto* source_child : source_children) {
                                        const auto child_id = scene.create(source_child->name);
                                        auto* child = scene.find(child_id);
                                        if (!child) continue;
                                        *child = *source_child;
                                        child->id = child_id;
                                        child->parent_id = duplicate_root_id;
                                        child->velocity = {};
                                        child->object_id = homeworldz::viewer::random_uuid();
                                        regenerate_task_inventory_item_ids(*child);
                                        child->creation_date = duplicate->creation_date;
                                        homeworldz::scene::update_linked_world_transform(*child, *duplicate);
                                        created_entities.push_back(child_id);
                                    }
                                }
                            }
                            bool persisted = false;
                            if (!created_entities.empty()) {
                                try {
                                    storage->save_snapshot(scene);
                                    persisted = true;
                                } catch (const std::exception& error) {
                                    for (const auto entity_id : created_entities) scene.remove(entity_id);
                                    std::cout << "{\"level\":\"error\",\"message\":\"primitive duplication persistence failed\",\"error\":"
                                              << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                                }
                            }
                            if (persisted) {
                                const auto region_handle =
                                    (static_cast<std::uint64_t>(region_grid_x * 256) << 32) |
                                    static_cast<std::uint32_t>(region_grid_y * 256);
                                for (const auto entity_id : created_entities) {
                                    const auto* entity = scene.find(entity_id);
                                    if (!entity) continue;
                                    synchronize_physics_object(*entity);
                                    for (const auto& [recipient_endpoint, recipient] : avatars) {
                                        auto object = static_object_from_entity(scene, *entity, recipient.user_id, falcon);
                                        if (!object) continue;
                                        if (recipient_endpoint == endpoint)
                                            object->update_flags |=
                                                object_duplicate->duplicate_flags & create_selected;
                                        if (const auto sent = circuits.send(recipient_endpoint,
                                                homeworldz::viewer::encode_static_object_update(
                                                    region_handle, *object), true, now, true))
                                            static_cast<void>(send_udp(
                                                viewer_server, recipient_endpoint, *sent));
                                    }
                                    // An insert, not an update: these local_ids are new to every
                                    // session client. Same envelope kind as any other object
                                    // change, so the receiving handler has to upsert rather than
                                    // assume it has seen the id before.
                                    if (session_object_visible(*entity))
                                        deliver_to_embodied(session_object_envelope(*entity));
                                }
                                std::cout << "{\"level\":\"info\",\"message\":\"primitives duplicated\",\"count\":"
                                          << created_entities.size() << "}" << std::endl;
                            }
                        }
                        const auto object_material =
                            homeworldz::viewer::decode_object_material(packet->payload);
                        if (object_material && object_material->agent_id == identity->agent_id &&
                            object_material->session_id == identity->session_id) {
                            constexpr std::uint8_t last_supported_material = 0x07;
                            struct OriginalMaterial {
                                std::uint8_t material{};
                                double friction{};
                                double restitution{};
                            };
                            std::unordered_map<homeworldz::scene::EntityId, OriginalMaterial> originals;
                            std::unordered_set<homeworldz::scene::EntityId> requested_entities;
                            const auto user_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            for (const auto& update : object_material->objects) {
                                auto* entity = scene.find(update.local_id);
                                if (!entity) continue;
                                requested_entities.insert(entity->id);
                                if (update.material > last_supported_material ||
                                    entity->owner_id != user_id ||
                                    (entity->owner_permissions & homeworldz::scene::permission_modify) == 0 ||
                                    entity->material == update.material)
                                    continue;
                                originals.try_emplace(entity->id, OriginalMaterial{
                                    entity->material, entity->physics_friction,
                                    entity->physics_restitution});
                                entity->material = update.material;
                                apply_material_contact_defaults(*entity);
                            }
                            bool persisted = false;
                            if (!originals.empty()) {
                                try {
                                    storage->save_snapshot(scene);
                                    persisted = true;
                                } catch (const std::exception& error) {
                                    for (const auto& [entity_id, original] : originals) {
                                        if (auto* entity = scene.find(entity_id)) {
                                            entity->material = original.material;
                                            entity->physics_friction = original.friction;
                                            entity->physics_restitution = original.restitution;
                                        }
                                    }
                                    std::cout << "{\"level\":\"error\",\"message\":\"primitive material persistence failed\",\"error\":"
                                              << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                                }
                            }
                            const auto region_handle =
                                (static_cast<std::uint64_t>(region_grid_x * 256) << 32) |
                                static_cast<std::uint32_t>(region_grid_y * 256);
                            for (const auto entity_id : requested_entities) {
                                const auto* entity = scene.find(entity_id);
                                if (!entity) continue;
                                for (const auto& [recipient_endpoint, recipient] : avatars) {
                                    const auto object = static_object_from_entity(scene, *entity, recipient.user_id, falcon);
                                    if (!object) continue;
                                    if (const auto sent = circuits.send(recipient_endpoint,
                                            homeworldz::viewer::encode_static_object_update(
                                                region_handle, *object), true, now, true))
                                        static_cast<void>(send_udp(
                                            viewer_server, recipient_endpoint, *sent));
                                }
                                // And to session clients. See the note at multiple_object_update.
                                if (session_object_visible(*entity))
                                    deliver_to_embodied(session_object_envelope(*entity));
                            }
                            if (persisted) {
                                for (const auto& [entity_id, original] : originals) {
                                    static_cast<void>(original);
                                    if (const auto* entity = scene.find(entity_id))
                                        synchronize_physics_object(*entity);
                                }
                                std::cout << "{\"level\":\"info\",\"message\":\"primitive materials updated\",\"count\":"
                                          << originals.size() << "}" << std::endl;
                            }
                        }
                        const auto object_shape =
                            homeworldz::viewer::decode_object_shape(packet->payload);
                        if (object_shape && object_shape->agent_id == identity->agent_id &&
                            object_shape->session_id == identity->session_id) {
                            std::unordered_map<homeworldz::scene::EntityId,
                                homeworldz::scene::Entity> originals;
                            std::unordered_set<homeworldz::scene::EntityId> requested_entities;
                            const auto user_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            const auto apply_shape = [](homeworldz::scene::Entity& entity,
                                const homeworldz::viewer::ObjectShapeUpdate& update) {
                                entity.path_curve = update.path_curve;
                                entity.profile_curve = update.profile_curve;
                                entity.path_begin = update.path_begin;
                                entity.path_end = update.path_end;
                                entity.path_scale_x = update.path_scale_x;
                                entity.path_scale_y = update.path_scale_y;
                                entity.path_shear_x = update.path_shear_x;
                                entity.path_shear_y = update.path_shear_y;
                                entity.path_twist = update.path_twist;
                                entity.path_twist_begin = update.path_twist_begin;
                                entity.path_radius_offset = update.path_radius_offset;
                                entity.path_taper_x = update.path_taper_x;
                                entity.path_taper_y = update.path_taper_y;
                                entity.path_revolutions = update.path_revolutions;
                                entity.path_skew = update.path_skew;
                                entity.profile_begin = update.profile_begin;
                                entity.profile_end = update.profile_end;
                                entity.profile_hollow = update.profile_hollow;
                            };
                            const auto shape_changed = [](const homeworldz::scene::Entity& entity,
                                const homeworldz::viewer::ObjectShapeUpdate& update) {
                                return entity.path_curve != update.path_curve ||
                                    entity.profile_curve != update.profile_curve ||
                                    entity.path_begin != update.path_begin ||
                                    entity.path_end != update.path_end ||
                                    entity.path_scale_x != update.path_scale_x ||
                                    entity.path_scale_y != update.path_scale_y ||
                                    entity.path_shear_x != update.path_shear_x ||
                                    entity.path_shear_y != update.path_shear_y ||
                                    entity.path_twist != update.path_twist ||
                                    entity.path_twist_begin != update.path_twist_begin ||
                                    entity.path_radius_offset != update.path_radius_offset ||
                                    entity.path_taper_x != update.path_taper_x ||
                                    entity.path_taper_y != update.path_taper_y ||
                                    entity.path_revolutions != update.path_revolutions ||
                                    entity.path_skew != update.path_skew ||
                                    entity.profile_begin != update.profile_begin ||
                                    entity.profile_end != update.profile_end ||
                                    entity.profile_hollow != update.profile_hollow;
                            };
                            for (const auto& update : object_shape->objects) {
                                auto* entity = scene.find(update.local_id);
                                if (!entity) continue;
                                requested_entities.insert(entity->id);
                                // Same well-formedness gate as ObjectAdd: a recognized
                                // path curve and profile curve keep every basic shape
                                // and its edited variations while rejecting garbage.
                                const bool valid_path_curve = update.path_curve == 0x10 ||
                                    update.path_curve == 0x20 || update.path_curve == 0x30;
                                const bool valid_profile_curve = (update.profile_curve & 0x0f) <= 0x05;
                                if (!valid_path_curve || !valid_profile_curve ||
                                    entity->owner_id != user_id ||
                                    (entity->owner_permissions & homeworldz::scene::permission_modify) == 0 ||
                                    !shape_changed(*entity, update))
                                    continue;
                                originals.try_emplace(entity->id, *entity);
                                apply_shape(*entity, update);
                            }
                            bool persisted = false;
                            if (!originals.empty()) {
                                try {
                                    storage->save_snapshot(scene);
                                    persisted = true;
                                } catch (const std::exception& error) {
                                    for (const auto& [entity_id, original] : originals)
                                        if (auto* entity = scene.find(entity_id))
                                            *entity = original;
                                    std::cout << "{\"level\":\"error\",\"message\":\"primitive shape persistence failed\",\"error\":"
                                              << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                                }
                            }
                            const auto region_handle =
                                (static_cast<std::uint64_t>(region_grid_x * 256) << 32) |
                                static_cast<std::uint32_t>(region_grid_y * 256);
                            for (const auto entity_id : requested_entities) {
                                const auto* entity = scene.find(entity_id);
                                if (!entity) continue;
                                for (const auto& [recipient_endpoint, recipient] : avatars) {
                                    const auto object = static_object_from_entity(scene, *entity, recipient.user_id, falcon);
                                    if (!object) continue;
                                    if (const auto sent = circuits.send(recipient_endpoint,
                                            homeworldz::viewer::encode_static_object_update(
                                                region_handle, *object), true, now, true))
                                        static_cast<void>(send_udp(
                                            viewer_server, recipient_endpoint, *sent));
                                }
                                // And to session clients. See the note at multiple_object_update.
                                if (session_object_visible(*entity))
                                    deliver_to_embodied(session_object_envelope(*entity));
                            }
                            if (persisted) {
                                for (const auto& [entity_id, original] : originals) {
                                    static_cast<void>(original);
                                    if (const auto* entity = scene.find(entity_id))
                                        synchronize_physics_object(*entity);
                                }
                                std::cout << "{\"level\":\"info\",\"message\":\"primitive shapes updated\",\"count\":"
                                          << originals.size() << "}" << std::endl;
                            }
                        }
                        const auto object_image =
                            homeworldz::viewer::decode_object_image(packet->payload);
                        if (object_image && object_image->agent_id == identity->agent_id &&
                            object_image->session_id == identity->session_id) {
                            std::unordered_map<homeworldz::scene::EntityId, std::vector<std::byte>> originals;
                            std::unordered_set<homeworldz::scene::EntityId> requested_entities;
                            const auto user_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            for (const auto& update : object_image->objects) {
                                auto* entity = scene.find(update.local_id);
                                if (!entity) continue;
                                requested_entities.insert(entity->id);
                                auto texture_entry = update.texture_entry;
                                homeworldz::viewer::normalize_primitive_texture_entry(
                                    texture_entry, default_prim_texture_entry());
                                if (entity->owner_id != user_id ||
                                    (entity->owner_permissions & homeworldz::scene::permission_modify) == 0 ||
                                    entity->texture_entry == texture_entry)
                                    continue;
                                originals.try_emplace(entity->id, entity->texture_entry);
                                entity->texture_entry = std::move(texture_entry);
                            }
                            bool persisted = false;
                            if (!originals.empty()) {
                                try {
                                    storage->save_snapshot(scene);
                                    persisted = true;
                                } catch (const std::exception& error) {
                                    for (auto& [entity_id, texture_entry] : originals)
                                        if (auto* entity = scene.find(entity_id))
                                            entity->texture_entry = std::move(texture_entry);
                                    std::cout << "{\"level\":\"error\",\"message\":\"primitive texture persistence failed\",\"error\":"
                                              << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                                }
                            }
                            const auto region_handle =
                                (static_cast<std::uint64_t>(region_grid_x * 256) << 32) |
                                static_cast<std::uint32_t>(region_grid_y * 256);
                            for (const auto entity_id : requested_entities) {
                                const auto* entity = scene.find(entity_id);
                                if (!entity) continue;
                                for (const auto& [recipient_endpoint, recipient] : avatars) {
                                    const auto object = static_object_from_entity(scene, *entity, recipient.user_id, falcon);
                                    if (!object) continue;
                                    if (const auto sent = circuits.send(recipient_endpoint,
                                            homeworldz::viewer::encode_static_object_update(
                                                region_handle, *object), true, now, true))
                                        static_cast<void>(send_udp(
                                            viewer_server, recipient_endpoint, *sent));
                                }
                                // And to session clients. See the note at multiple_object_update.
                                if (session_object_visible(*entity))
                                    deliver_to_embodied(session_object_envelope(*entity));
                            }
                            if (persisted)
                                std::cout << "{\"level\":\"info\",\"message\":\"primitive textures updated\",\"count\":"
                                          << originals.size() << "}" << std::endl;
                        }
                        const auto object_flags =
                            homeworldz::viewer::decode_object_flag_update(packet->payload);
                        if (object_flags && object_flags->agent_id == identity->agent_id &&
                            object_flags->session_id == identity->session_id) {
                            auto* entity = scene.find(object_flags->local_id);
                            if (entity && entity->parent_id != 0)
                                entity = scene.find(entity->parent_id);
                            const auto user_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            if (entity && entity->owner_id == user_id &&
                                (entity->owner_permissions & homeworldz::scene::permission_modify) != 0) {
                                const auto original_physical = entity->physical;
                                const auto original_phantom = entity->phantom;
                                const auto original_temporary = entity->temporary;
                                const auto original_physics_shape_type = entity->physics_shape_type;
                                const auto original_physics_density = entity->physics_density;
                                const auto original_physics_friction = entity->physics_friction;
                                const auto original_physics_restitution = entity->physics_restitution;
                                const auto original_physics_gravity_multiplier =
                                    entity->physics_gravity_multiplier;
                                entity->physical = object_flags->use_physics;
                                entity->phantom = object_flags->phantom;
                                entity->temporary = object_flags->temporary;
                                if (object_flags->has_extra_physics)
                                    apply_extra_physics(*entity, *object_flags);
                                bool persisted = false;
                                try {
                                    storage->save_snapshot(scene);
                                    persisted = true;
                                } catch (const std::exception& error) {
                                    entity->physical = original_physical;
                                    entity->phantom = original_phantom;
                                    entity->temporary = original_temporary;
                                    entity->physics_shape_type = original_physics_shape_type;
                                    entity->physics_density = original_physics_density;
                                    entity->physics_friction = original_physics_friction;
                                    entity->physics_restitution = original_physics_restitution;
                                    entity->physics_gravity_multiplier =
                                        original_physics_gravity_multiplier;
                                    std::cout << "{\"level\":\"error\",\"message\":\"primitive flags persistence failed\",\"error\":"
                                              << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                                }
                                if (persisted) {
                                    if (entity->temporary)
                                        temporary_expirations.insert_or_assign(
                                            entity->id, now + std::chrono::seconds(60));
                                    else
                                        temporary_expirations.erase(entity->id);
                                    synchronize_physics_object(*entity);
                                    // Echo the stored values back, which are the
                                    // clamped ones rather than whatever arrived, so
                                    // the floater shows what the region actually
                                    // holds and the next edit sends that instead of
                                    // a stale or out-of-range figure.
                                    if (object_flags->has_extra_physics)
                                        enqueue_viewer_event(
                                            homeworldz::viewer::format_uuid(identity->session_id),
                                            homeworldz::viewer::object_physics_properties_event_xml(
                                                physics_properties_of(*entity)));
                                    const auto region_handle =
                                        (static_cast<std::uint64_t>(region_grid_x * 256) << 32) |
                                        static_cast<std::uint32_t>(region_grid_y * 256);
                                    for (const auto& [recipient_endpoint, recipient] : avatars) {
                                        const auto object = static_object_from_entity(scene, *entity, recipient.user_id, falcon);
                                        if (!object) continue;
                                        if (const auto sent = circuits.send(recipient_endpoint,
                                                homeworldz::viewer::encode_static_object_update(
                                                    region_handle, *object), true, now, true))
                                            static_cast<void>(send_udp(
                                                viewer_server, recipient_endpoint, *sent));
                                    }
                                    // And to session clients. See the note at
                                    // multiple_object_update.
                                    if (session_object_visible(*entity))
                                        deliver_to_embodied(session_object_envelope(*entity));
                                    std::cout << "{\"level\":\"info\",\"message\":\"primitive flags updated\",\"entityId\":"
                                              << entity->id << ",\"physical\":"
                                              << (entity->physical ? "true" : "false")
                                              << ",\"phantom\":"
                                              << (entity->phantom ? "true" : "false")
                                              << ",\"temporary\":"
                                              << (entity->temporary ? "true" : "false")
                                              << ",\"physicsShapeType\":"
                                              << static_cast<unsigned>(entity->physics_shape_type)
                                              << ",\"physicsDensity\":" << entity->physics_density
                                              << ",\"physicsFriction\":" << entity->physics_friction
                                              << ",\"physicsRestitution\":"
                                              << entity->physics_restitution
                                              << ",\"physicsGravityMultiplier\":"
                                              << entity->physics_gravity_multiplier << "}" << std::endl;
                                }
                            }
                        }
                        const auto object_add = homeworldz::viewer::decode_object_add(packet->payload);
                        if (object_add && object_add->agent_id == identity->agent_id &&
                            object_add->session_id == identity->session_id) {
                            constexpr std::uint32_t add_use_physics = 0x00000001;
                            constexpr std::uint32_t add_create_selected = 0x00000002;
                            constexpr std::uint32_t add_temporary = 0x40000000;
                            const auto valid_scale = std::all_of(
                                object_add->scale.begin(), object_add->scale.end(),
                                [](float value) { return value >= 0.01F && value <= 64.0F; });
                            const auto rotation_norm = object_add->rotation[0] * object_add->rotation[0] +
                                                       object_add->rotation[1] * object_add->rotation[1] +
                                                       object_add->rotation[2] * object_add->rotation[2];
                            const bool valid_rotation = rotation_norm <= 1.001F;
                            // Accept any well-formed basic prim: PCODE_PRIM (9) with a
                            // recognized path curve (line 0x10 / circle 0x20 / circle2
                            // 0x30) and profile curve (circle/square/iso-tri/equal-tri/
                            // right-tri/half-circle, low nibble 0x00..0x05). The full
                            // path+profile parameters are stored on the entity and
                            // echoed in the object update, so all seven basic shapes —
                            // Box, Cylinder, Prism, Sphere, Torus, Tube, Ring — plus
                            // edited variations (cut, hollow, twist, taper, shear,
                            // revolutions) rez and render from the viewer-supplied params.
                            const bool valid_path_curve = object_add->path_curve == 0x10 ||
                                object_add->path_curve == 0x20 || object_add->path_curve == 0x30;
                            const bool valid_profile_curve = (object_add->profile_curve & 0x0f) <= 0x05;
                            const bool valid_prim_shape = object_add->pcode == 9 &&
                                valid_path_curve && valid_profile_curve;
                            std::optional<homeworldz::scene::Vector3> placement;
                            if (valid_scale && object_add->bypass_raycast) {
                                const homeworldz::scene::Vector3 ray_end{
                                    object_add->ray_end[0], object_add->ray_end[1], object_add->ray_end[2]};
                                placement = homeworldz::scene::Vector3{
                                    ray_end.x, ray_end.y,
                                    ground_height(*terrain_heightmap, ray_end) + object_add->scale[2] * 0.5};
                            } else if (valid_scale) {
                                const auto target_id = homeworldz::viewer::format_uuid(object_add->ray_target_id);
                                const homeworldz::scene::Entity* target = nullptr;
                                for (const auto& [candidate_id, candidate] : scene.entities()) {
                                    static_cast<void>(candidate_id);
                                    if (candidate.object_id == target_id) {
                                        target = &candidate;
                                        break;
                                    }
                                }
                                if (target) {
                                    const auto intersection = homeworldz::scene::intersect_box(
                                        {object_add->ray_start[0], object_add->ray_start[1], object_add->ray_start[2]},
                                        {object_add->ray_end[0], object_add->ray_end[1], object_add->ray_end[2]},
                                        target->position, target->scale);
                                    if (intersection) {
                                        placement = homeworldz::scene::Vector3{
                                            intersection->position.x + intersection->normal.x * object_add->scale[0] * 0.5,
                                            intersection->position.y + intersection->normal.y * object_add->scale[1] * 0.5,
                                            intersection->position.z + intersection->normal.z * object_add->scale[2] * 0.5};
                                    }
                                }
                            }
                            const bool valid_position = placement && placement->x >= 0.0 &&
                                placement->x <= region_size_x && placement->y >= 0.0 &&
                                placement->y <= region_size_y &&
                                placement->z >= -64.0 && placement->z <= 4096.0;
                            bool parcel_allows_build = true;
                            if (placement) {
                                const auto rezzer = homeworldz::viewer::format_uuid(identity->agent_id);
                                if (const auto* parcel = parcels->parcel_at(
                                        static_cast<float>(placement->x),
                                        static_cast<float>(placement->y)))
                                    parcel_allows_build =
                                        homeworldz::parcel::can_build(*parcel, rezzer, region_owner_id) ||
                                        is_estate_manager(rezzer);
                            }
                            bool created = false;
                            std::string object_id;
                            homeworldz::scene::EntityId entity_id{};
                            if (valid_prim_shape && parcel_allows_build &&
                                valid_position && valid_rotation && object_add->material <= 7) {
                                object_id = homeworldz::viewer::random_uuid();
                                const auto owner_id = homeworldz::viewer::format_uuid(identity->agent_id);
                                entity_id = scene.create("Primitive", *placement);
                                if (auto* entity = scene.find(entity_id)) {
                                    entity->object_id = object_id;
                                    entity->owner_id = owner_id;
                                    entity->creator_id = owner_id;
                                    entity->scale = {object_add->scale[0], object_add->scale[1], object_add->scale[2]};
                                    entity->rotation = {object_add->rotation[0], object_add->rotation[1],
                                                        object_add->rotation[2]};
                                    entity->material = object_add->material;
                                    entity->physical = (object_add->add_flags & add_use_physics) != 0;
                                    entity->temporary = (object_add->add_flags & add_temporary) != 0;
                                    entity->path_curve = object_add->path_curve;
                                    entity->profile_curve = object_add->profile_curve;
                                    entity->path_begin = object_add->path_begin;
                                    entity->path_end = object_add->path_end;
                                    entity->path_scale_x = object_add->path_scale_x;
                                    entity->path_scale_y = object_add->path_scale_y;
                                    entity->path_shear_x = object_add->path_shear_x;
                                    entity->path_shear_y = object_add->path_shear_y;
                                    entity->path_twist = object_add->path_twist;
                                    entity->path_twist_begin = object_add->path_twist_begin;
                                    entity->path_radius_offset = object_add->path_radius_offset;
                                    entity->path_taper_x = object_add->path_taper_x;
                                    entity->path_taper_y = object_add->path_taper_y;
                                    entity->path_revolutions = object_add->path_revolutions;
                                    entity->path_skew = object_add->path_skew;
                                    entity->profile_begin = object_add->profile_begin;
                                    entity->profile_end = object_add->profile_end;
                                    entity->profile_hollow = object_add->profile_hollow;
                                    entity->texture_entry = default_prim_texture_entry();
                                    apply_material_contact_defaults(*entity);
                                    entity->creation_date = static_cast<std::uint64_t>(
                                        std::chrono::duration_cast<std::chrono::seconds>(
                                            std::chrono::system_clock::now().time_since_epoch()).count());
                                    try {
                                        storage->save_snapshot(scene);
                                        created = true;
                                    } catch (const std::exception& error) {
                                        std::cout << "{\"level\":\"error\",\"message\":\"primitive persistence failed\",\"error\":"
                                                  << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                                        scene.remove(entity_id);
                                    }
                                }
                            }
                            if (created) {
                                const auto* entity = scene.find(entity_id);
                                if (entity) {
                                    if (entity->temporary)
                                        temporary_expirations.insert_or_assign(
                                            entity->id, now + std::chrono::seconds(60));
                                    synchronize_physics_object(*entity);
                                    const auto region_handle =
                                        (static_cast<std::uint64_t>(region_grid_x * 256) << 32) |
                                        static_cast<std::uint32_t>(region_grid_y * 256);
                                    for (const auto& [recipient_endpoint, recipient] : avatars) {
                                        auto object = static_object_from_entity(scene, *entity, recipient.user_id, falcon);
                                        if (!object) continue;
                                        if (recipient.user_id == entity->owner_id &&
                                            (object_add->add_flags & add_create_selected) != 0)
                                            object->update_flags |= add_create_selected;
                                        if (const auto sent = circuits.send(recipient_endpoint,
                                                homeworldz::viewer::encode_static_object_update(
                                                    region_handle, *object), true, now, true))
                                            static_cast<void>(send_udp(viewer_server, recipient_endpoint, *sent));
                                    }
                                    // And to session clients: this is the insert. The loop above
                                    // is a hand-inlined copy of broadcast_object_update's UDP
                                    // half — inlined because only rez marks the new prim selected
                                    // for its owner, which the shared helper cannot express — but
                                    // the copy stopped one line short of the session delivery the
                                    // helper ends with. See the note at multiple_object_update.
                                    if (session_object_visible(*entity))
                                        deliver_to_embodied(session_object_envelope(*entity));
                                }
                            }
                            std::cout << "{\"level\":" << (created ? "\"info\"" : "\"warn\"")
                                      << ",\"message\":\"primitive creation "
                                      << (created ? "completed" : "rejected") << "\",\"objectId\":"
                                      << homeworldz::api::json_string(object_id)
                                      << ",\"pcode\":" << static_cast<unsigned>(object_add->pcode)
                                      << ",\"pathCurve\":" << static_cast<unsigned>(object_add->path_curve)
                                      << ",\"profileCurve\":" << static_cast<unsigned>(object_add->profile_curve)
                                      << ",\"pathScale\":[" << static_cast<unsigned>(object_add->path_scale_x)
                                      << ',' << static_cast<unsigned>(object_add->path_scale_y) << ']'
                                      << ",\"pathShear\":[" << static_cast<unsigned>(object_add->path_shear_x)
                                      << ',' << static_cast<unsigned>(object_add->path_shear_y) << ']'
                                      << ",\"material\":" << static_cast<unsigned>(object_add->material)
                                      << ",\"scale\":[" << object_add->scale[0] << ','
                                      << object_add->scale[1] << ',' << object_add->scale[2] << ']'
                                      << ",\"validScale\":" << (valid_scale ? "true" : "false")
                                      << ",\"validRotation\":" << (valid_rotation ? "true" : "false")
                                      << ",\"validPosition\":" << (valid_position ? "true" : "false")
                                      << "}" << std::endl;
                        }
                        const auto derez = homeworldz::viewer::decode_derez_object(packet->payload);
                        if (derez && derez->agent_id == identity->agent_id &&
                            derez->session_id == identity->session_id) {
                            constexpr std::uint8_t derez_take_copy = 0x01;
                            constexpr std::uint8_t derez_take_inventory = 0x04;
                            constexpr std::uint8_t derez_trash = 0x06;
                            constexpr std::uint8_t derez_return_owner = 0x09;
                            constexpr int objects_folder_type = 6;
                            const auto user_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            auto destination_id = homeworldz::viewer::format_uuid(derez->destination_id);
                            const bool objects_destination =
                                derez->destination == derez_take_copy ||
                                derez->destination == derez_take_inventory ||
                                derez->destination == derez_return_owner;
                            const bool removes_from_scene = derez->destination != derez_take_copy;
                            if (objects_destination &&
                                destination_id == "00000000-0000-0000-0000-000000000000" && viewer_grid) {
                                if (const auto objects_folder = viewer_grid->find_system_inventory_folder(
                                        user_id, objects_folder_type))
                                    destination_id = *objects_folder;
                            }
                            std::vector<std::uint32_t> removed_ids;
                            std::size_t inventory_items_created = 0;
                            std::unordered_set<homeworldz::scene::EntityId> processed_roots;
                            if ((objects_destination || derez->destination == derez_trash) &&
                                homeworldz::viewer::valid_derez_batch(
                                    derez->packet_count, derez->packet_number)) {
                                for (const auto local_id : derez->local_ids) {
                                    const auto* selected = scene.find(local_id);
                                    if (!selected) continue;
                                    const auto root_id = selected->parent_id != 0
                                        ? selected->parent_id : local_id;
                                    const auto* entity = scene.find(root_id);
                                    if (!processed_roots.insert(root_id).second || !entity ||
                                        entity->object_id.empty() || entity->owner_id != user_id)
                                        continue;
                                    std::vector<const homeworldz::scene::Entity*> children;
                                    std::vector<homeworldz::scene::EntityId> part_ids{root_id};
                                    bool valid_linkset = true;
                                    for (const auto& [candidate_id, candidate] : scene.entities()) {
                                        if (candidate.parent_id != root_id) continue;
                                        if (candidate.owner_id != user_id) {
                                            valid_linkset = false;
                                            break;
                                        }
                                        children.push_back(&candidate);
                                        part_ids.push_back(candidate_id);
                                    }
                                    const auto folded = homeworldz::scene::effective_permissions(scene, *entity);
                                    if (!valid_linkset ||
                                        (derez->destination == derez_take_copy &&
                                         (folded.owner & homeworldz::scene::permission_copy) == 0))
                                        continue;
                                    const auto asset_id = homeworldz::viewer::random_uuid();
                                    const auto item_id = homeworldz::viewer::random_uuid();
                                    const auto content_text =
                                        homeworldz::asset::serialize_linkset_asset(*entity, children);
                                    const auto content = std::span(
                                        reinterpret_cast<const std::byte*>(content_text.data()), content_text.size());
                                    auto base_permissions = entity->base_permissions;
                                    auto owner_permissions = folded.owner;
                                    auto everyone_permissions = entity->everyone_permissions;
                                    auto next_owner_permissions = folded.next_owner;
                                    for (const auto* child : children) {
                                        base_permissions &= child->base_permissions;
                                        everyone_permissions &= child->everyone_permissions;
                                    }
                                    everyone_permissions &= owner_permissions;
                                    bool item_created = false;
                                    try {
                                        const auto metadata = storage->store_asset(
                                            asset_id, entity->creator_id, content);
                                        const bool asset_registered = viewer_grid && viewer_grid->register_asset(
                                            metadata.viewer_id, metadata.creator_id, metadata.sha256,
                                            metadata.size, region_public_endpoint, true) &&
                                            // Write-through before the commit
                                            // (ADR 0026): the durability
                                            // fetch-back and this thread
                                            // cannot meet.
                                            viewer_grid->store_vault_asset(metadata.viewer_id, content);
                                        item_created = asset_registered && viewer_grid->create_object_inventory_item(
                                            user_id, homeworldz::grid::ObjectInventoryItem{
                                                item_id, entity->creator_id, destination_id, asset_id,
                                                entity->name, entity->description, base_permissions,
                                                owner_permissions, everyone_permissions,
                                                next_owner_permissions});
                                    } catch (const std::exception& error) {
                                        std::cout << "{\"level\":\"error\",\"message\":\"primitive derez inventory failed\",\"error\":"
                                                  << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                                    }
                                    if (!item_created) continue;
                                    homeworldz::viewer::InventoryItem item;
                                    item.item_id = *homeworldz::viewer::parse_uuid(item_id);
                                    item.creator_id = *homeworldz::viewer::parse_uuid(entity->creator_id);
                                    item.owner_id = identity->agent_id;
                                    item.folder_id = *homeworldz::viewer::parse_uuid(destination_id);
                                    item.asset_id = *homeworldz::viewer::parse_uuid(asset_id);
                                    item.asset_type = 6;
                                    item.inventory_type = 6;
                                    item.name = entity->name;
                                    item.base_permissions = base_permissions;
                                    item.current_permissions = owner_permissions;
                                    item.everyone_permissions = everyone_permissions;
                                    item.next_permissions = next_owner_permissions;
                                    item.creation_date = static_cast<std::int32_t>(
                                        std::chrono::duration_cast<std::chrono::seconds>(
                                            std::chrono::system_clock::now().time_since_epoch()).count());
                                    const homeworldz::viewer::AgentMessage reply{
                                        identity->agent_id, identity->session_id};
                                    auto inventory_update = homeworldz::viewer::encode_update_create_inventory_item(
                                        reply, 0, item);
                                    if (const auto outgoing = circuits.send(
                                            endpoint, std::move(inventory_update), true, now, true))
                                        static_cast<void>(send_udp(viewer_server, endpoint, *outgoing));
                                    if (removes_from_scene)
                                        for (auto part = part_ids.rbegin(); part != part_ids.rend(); ++part)
                                            if (scene.remove(*part))
                                                removed_ids.push_back(static_cast<std::uint32_t>(*part));
                                    ++inventory_items_created;
                                }
                            }
                            bool persisted = removed_ids.empty();
                            if (!removed_ids.empty()) {
                                try {
                                    storage->save_snapshot(scene);
                                    persisted = true;
                                } catch (const std::exception& error) {
                                    std::cout << "{\"level\":\"error\",\"message\":\"primitive derez persistence failed\",\"error\":"
                                              << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                                }
                            }
                            if (persisted && !removed_ids.empty()) {
                                for (const auto entity_id : removed_ids) remove_physics_object(entity_id);
                                const auto kill = homeworldz::viewer::encode_kill_object(removed_ids);
                                deliver_to_embodied(session_kill_many(removed_ids));
                                for (const auto& [recipient_endpoint, recipient] : avatars) {
                                    static_cast<void>(recipient);
                                    if (const auto outgoing = circuits.send(
                                            recipient_endpoint, kill, true, now))
                                        static_cast<void>(send_udp(viewer_server, recipient_endpoint, *outgoing));
                                }
                            }
                            std::cout << "{\"level\":"
                                      << (persisted && inventory_items_created == processed_roots.size()
                                              ? "\"info\"" : "\"warn\"")
                                      << ",\"message\":\"primitive derez batch processed\",\"removed\":"
                                      << removed_ids.size() << ",\"inventoryItemsCreated\":"
                                      << inventory_items_created << ",\"requested\":" << derez->local_ids.size()
                                      << ",\"destination\":" << static_cast<unsigned>(derez->destination) << "}"
                                      << std::endl;
                        }
                        const auto rez = homeworldz::viewer::decode_rez_object(packet->payload);
                        if (rez && rez->agent_id == identity->agent_id &&
                            rez->session_id == identity->session_id) {
                            const auto user_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            const auto item_id = homeworldz::viewer::format_uuid(rez->item_id);
                            bool created = false;
                            std::string object_id;
                            std::string object_rez_id;
                            bool object_rez_prepared = false;
                            bool scene_persisted = false;
                            std::vector<homeworldz::scene::EntityId> entity_ids;
                            try {
                                auto item = viewer_grid
                                    ? viewer_grid->find_inventory_item(user_id, item_id) : std::nullopt;
                                if (item && item->asset_type == 6 && item->inventory_type == 6) {
                                    const auto content = read_federated_asset(item->asset_id);
                                    const auto linkset = homeworldz::asset::parse_linkset_asset(content);
                                    const auto* asset = linkset ? &linkset->root : nullptr;
                                    std::optional<homeworldz::scene::Vector3> placement;
                                    if (asset && rez->bypass_raycast) {
                                        const homeworldz::scene::Vector3 ray_end{
                                            rez->ray_end[0], rez->ray_end[1], rez->ray_end[2]};
                                        placement = homeworldz::scene::Vector3{
                                            ray_end.x, ray_end.y,
                                            ground_height(*terrain_heightmap, ray_end) + asset->scale.z * 0.5};
                                    } else if (asset) {
                                        const auto target_id = homeworldz::viewer::format_uuid(rez->ray_target_id);
                                        const homeworldz::scene::Entity* target = nullptr;
                                        for (const auto& [candidate_id, candidate] : scene.entities()) {
                                            static_cast<void>(candidate_id);
                                            if (candidate.object_id == target_id) {
                                                target = &candidate;
                                                break;
                                            }
                                        }
                                        if (target) {
                                            const auto intersection = homeworldz::scene::intersect_box(
                                                {rez->ray_start[0], rez->ray_start[1], rez->ray_start[2]},
                                                {rez->ray_end[0], rez->ray_end[1], rez->ray_end[2]},
                                                target->position, target->scale);
                                            if (intersection) {
                                                placement = homeworldz::scene::Vector3{
                                                    intersection->position.x + intersection->normal.x * asset->scale.x * 0.5,
                                                    intersection->position.y + intersection->normal.y * asset->scale.y * 0.5,
                                                    intersection->position.z + intersection->normal.z * asset->scale.z * 0.5};
                                            }
                                        } else {
                                            const homeworldz::scene::Vector3 ray_end{
                                                rez->ray_end[0], rez->ray_end[1], rez->ray_end[2]};
                                            placement = homeworldz::scene::Vector3{
                                                ray_end.x, ray_end.y,
                                                ground_height(*terrain_heightmap, ray_end) + asset->scale.z * 0.5};
                                        }
                                    }
                                    const bool valid_position = placement &&
                                        placement->x >= 0.0 && placement->x <= region_size_x &&
                                        placement->y >= 0.0 && placement->y <= region_size_y &&
                                        placement->z >= -64.0 && placement->z <= 4096.0;
                                    bool parcel_allows_build = true;
                                    if (placement) {
                                        if (const auto* parcel = parcels->parcel_at(
                                                static_cast<float>(placement->x),
                                                static_cast<float>(placement->y)))
                                            parcel_allows_build = homeworldz::parcel::can_build(
                                                *parcel, user_id, region_owner_id) ||
                                                is_estate_manager(user_id);
                                    }
                                    if (asset && valid_position && parcel_allows_build) {
                                        object_id = homeworldz::viewer::random_uuid();
                                        const bool no_copy =
                                            (item->current_permissions & homeworldz::scene::permission_copy) == 0;
                                        if (rez->remove_item && no_copy) {
                                            object_rez_id = homeworldz::viewer::random_uuid();
                                            const auto prepared = viewer_grid
                                                ? viewer_grid->prepare_object_rez({
                                                    object_rez_id, user_id, item_id,
                                                    provisioned_region_id, object_id})
                                                : std::nullopt;
                                            if (!prepared) throw std::runtime_error("prepare no-copy object rez");
                                            item = prepared->item;
                                            object_id = prepared->object_id;
                                            object_rez_prepared = true;
                                        }
                                        const auto root_id = scene.create(item->name, *placement);
                                        entity_ids.push_back(root_id);
                                        if (auto* entity = scene.find(root_id)) {
                                            entity->object_id = object_id;
                                            entity->owner_id = user_id;
                                            entity->creator_id = item->creator_id;
                                            apply_object_asset(*entity, *asset);
                                            entity->description = item->description.empty()
                                                ? asset->description : item->description;
                                            entity->base_permissions = item->base_permissions;
                                            entity->owner_permissions = item->current_permissions;
                                            entity->everyone_permissions = item->everyone_permissions;
                                            entity->next_owner_permissions = item->next_permissions;
                                            entity->creation_date = static_cast<std::uint64_t>(
                                                std::chrono::duration_cast<std::chrono::seconds>(
                                                    std::chrono::system_clock::now().time_since_epoch()).count());
                                            for (const auto& child_asset : linkset->children) {
                                                const auto child_id = scene.create(
                                                    child_asset.name.empty() ? "Primitive" : child_asset.name);
                                                entity_ids.push_back(child_id);
                                                auto* child = scene.find(child_id);
                                                if (!child) throw std::runtime_error("create linkset child");
                                                child->object_id = homeworldz::viewer::random_uuid();
                                                child->owner_id = user_id;
                                                child->creator_id = child_asset.creator_id.empty()
                                                    ? item->creator_id : child_asset.creator_id;
                                                apply_object_asset(*child, child_asset);
                                                child->description = child_asset.description;
                                                child->base_permissions =
                                                    child_asset.base_permissions & item->base_permissions;
                                                child->owner_permissions =
                                                    child_asset.owner_permissions & item->current_permissions;
                                                child->group_permissions = child_asset.group_permissions;
                                                child->everyone_permissions =
                                                    child_asset.everyone_permissions & item->everyone_permissions;
                                                child->next_owner_permissions =
                                                    child_asset.next_owner_permissions & item->next_permissions;
                                                child->creation_date = entity->creation_date;
                                                child->parent_id = root_id;
                                                child->local_position = child_asset.local_position;
                                                child->local_rotation = child_asset.local_rotation;
                                                homeworldz::scene::update_linked_world_transform(*child, *entity);
                                            }
                                            storage->save_snapshot(scene);
                                            scene_persisted = true;
                                            created = true;
                                            // The object now stands here, so its whole
                                            // closure — face textures, contents, nested
                                            // objects — must live here too (ADR 0026,
                                            // region completeness).
                                            materialize_asset_closure(
                                                {{item->asset_id, 6}}, "rez");
                                        }
                                    }
                                }
                            } catch (const std::exception& error) {
                                for (auto entity = entity_ids.rbegin(); entity != entity_ids.rend(); ++entity)
                                    scene.remove(*entity);
                                std::cout << "{\"level\":\"error\",\"message\":\"primitive rez failed\",\"error\":"
                                          << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                            }
                            if (object_rez_prepared && !scene_persisted && viewer_grid) {
                                if (!viewer_grid->rollback_object_rez(object_rez_id, provisioned_region_id))
                                    std::cerr << "{\"level\":\"warning\",\"message\":\"object rez rollback awaits reconciliation\",\"rezId\":"
                                              << homeworldz::api::json_string(object_rez_id) << "}" << std::endl;
                            }
                            if (object_rez_prepared && scene_persisted && viewer_grid) {
                                if (!viewer_grid->finalize_object_rez(object_rez_id, provisioned_region_id))
                                    std::cerr << "{\"level\":\"warning\",\"message\":\"object rez finalization awaits reconciliation\",\"rezId\":"
                                              << homeworldz::api::json_string(object_rez_id) << "}" << std::endl;
                            }
                            if (created) {
                                for (const auto entity_id : entity_ids) {
                                    const auto* entity = scene.find(entity_id);
                                    if (!entity) continue;
                                    synchronize_physics_object(*entity);
                                    const auto region_handle =
                                        (static_cast<std::uint64_t>(region_grid_x * 256) << 32) |
                                        static_cast<std::uint32_t>(region_grid_y * 256);
                                    for (const auto& [recipient_endpoint, recipient] : avatars) {
                                        const auto object = static_object_from_entity(scene, *entity, recipient.user_id, falcon);
                                        if (!object) continue;
                                        if (const auto outgoing = circuits.send(
                                                recipient_endpoint,
                                                homeworldz::viewer::encode_static_object_update(
                                                    region_handle, *object), true, now, true))
                                            static_cast<void>(send_udp(viewer_server, recipient_endpoint, *outgoing));
                                    }
                                    // An insert, like duplicate: a local_id no session client has
                                    // seen. See the note at multiple_object_update.
                                    if (session_object_visible(*entity))
                                        deliver_to_embodied(session_object_envelope(*entity));
                                }
                            }
                            std::cout << "{\"level\":" << (created ? "\"info\"" : "\"warn\"")
                                      << ",\"message\":\"primitive inventory rez "
                                      << (created ? "completed" : "rejected") << "\",\"itemId\":"
                                      << homeworldz::api::json_string(item_id) << ",\"objectId\":"
                                      << homeworldz::api::json_string(object_id) << "}" << std::endl;
                        }
                        // Attachments: wearing (Low 395) and taking off (Low 113).
                        // A worn object is deliberately kept out of physics and
                        // out of the region snapshot — it belongs to the avatar,
                        // and both of those would outlive the avatar it hangs on.
                        const auto worn = homeworldz::viewer::decode_rez_single_attachment_from_inv(
                            packet->payload);
                        if (worn && worn->agent_id == identity->agent_id &&
                            worn->session_id == identity->session_id) {
                            const auto wearer = avatars.find(endpoint);
                            const auto user_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            const auto item_id = homeworldz::viewer::format_uuid(worn->item_id);
                            const auto outcome = wearer == avatars.end()
                                ? WearOutcome{false, false, 0, 0, "wearer has no avatar here"}
                                : wear_attachment(
                                      user_id, wearer->second.entity_id, item_id,
                                      worn->attachment_point,
                                      (worn->attachment_point & homeworldz::viewer::attachment_add) != 0,
                                      true, now);
                            std::cout << "{\"level\":"
                                      << (outcome.worn && outcome.recorded ? "\"info\"" : "\"warn\"")
                                      << ",\"message\":\"attachment "
                                      << (outcome.worn ? "worn"
                                          : outcome.inconclusive ? "not attempted" : "rejected")
                                      << "\",\"itemId\":"
                                      << homeworldz::api::json_string(item_id)
                                      << ",\"attachmentPoint\":" << static_cast<unsigned>(outcome.point)
                                      << ",\"requestedPoint\":"
                                      << static_cast<unsigned>(worn->attachment_point)
                                      << ",\"prims\":" << outcome.prims
                                      // Worn but unrecorded is worn until the next
                                      // login and then gone, which is worth saying
                                      // at the moment it happens.
                                      << ",\"recorded\":" << (outcome.recorded ? "true" : "false");
                            if (!outcome.worn)
                                std::cout << ",\"reason\":" << homeworldz::api::json_string(outcome.refused);
                            std::cout << "}" << std::endl;
                        }
                        // Low 396: what a viewer actually sends to wear anything.
                        // Batched — up to four objects per packet — so the reply
                        // to one packet is several wears.
                        const auto worn_batch =
                            homeworldz::viewer::decode_rez_multiple_attachments_from_inv(
                                packet->payload);
                        if (worn_batch && worn_batch->agent_id == identity->agent_id &&
                            worn_batch->session_id == identity->session_id) {
                            const auto wearer = avatars.find(endpoint);
                            const auto user_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            std::size_t attached = 0;
                            std::size_t inconclusive = 0;
                            if (wearer == avatars.end()) {
                                std::cout << "{\"level\":\"warn\",\"message\":\"attachment batch rejected\""
                                             ",\"reason\":\"wearer has no avatar here\",\"objects\":"
                                          << worn_batch->objects.size() << "}" << std::endl;
                            } else {
                                // "Replace outfit" asks for a clean slate first.
                                // Everything it takes off has to be forgotten on
                                // the grid too, or it all comes back at the next
                                // login and the replacement looks undone.
                                if (worn_batch->first_detach_all) {
                                    std::vector<std::string> removed_items;
                                    for (const auto& [candidate_id, candidate] : scene.entities())
                                        if (candidate.attachment_point != 0 &&
                                            candidate.parent_id == wearer->second.entity_id &&
                                            !candidate.attachment_item_id.empty())
                                            removed_items.push_back(candidate.attachment_item_id);
                                    const auto cleared = remove_avatar_attachments(
                                        wearer->second.entity_id, now);
                                    if (viewer_grid)
                                        for (const auto& removed_item : removed_items)
                                            static_cast<void>(viewer_grid->set_attachment_worn(
                                                user_id, removed_item, 0, false));
                                    std::cout << "{\"level\":\"info\",\"message\":"
                                                 "\"attachments cleared before batch\",\"cleared\":"
                                              << cleared << "}" << std::endl;
                                }
                                for (const auto& object : worn_batch->objects) {
                                    const auto item_id =
                                        homeworldz::viewer::format_uuid(object.item_id);
                                    const auto outcome = wear_attachment(
                                        user_id, wearer->second.entity_id, item_id,
                                        object.attachment_point,
                                        (object.attachment_point &
                                         homeworldz::viewer::attachment_add) != 0,
                                        true, now);
                                    if (outcome.worn) ++attached;
                                    if (outcome.inconclusive) ++inconclusive;
                                    std::cout << "{\"level\":"
                                              << (outcome.worn && outcome.recorded
                                                      ? "\"info\"" : "\"warn\"")
                                              << ",\"message\":\"attachment "
                                              << (outcome.worn ? "worn"
                                                  : outcome.inconclusive ? "not attempted"
                                                                         : "rejected")
                                              << "\",\"itemId\":"
                                              << homeworldz::api::json_string(item_id)
                                              << ",\"attachmentPoint\":"
                                              << static_cast<unsigned>(outcome.point)
                                              << ",\"requestedPoint\":"
                                              << static_cast<unsigned>(object.attachment_point)
                                              << ",\"prims\":" << outcome.prims
                                              << ",\"recorded\":"
                                              << (outcome.recorded ? "true" : "false");
                                    if (!outcome.worn)
                                        std::cout << ",\"reason\":"
                                                  << homeworldz::api::json_string(outcome.refused);
                                    std::cout << "}" << std::endl;
                                }
                                std::cout << "{\"level\":"
                                          << (attached == worn_batch->objects.size()
                                                  ? "\"info\"" : "\"warn\"")
                                          << ",\"message\":\"attachment batch processed\",\"attached\":"
                                          << attached << ",\"inconclusive\":" << inconclusive
                                          << ",\"objects\":" << worn_batch->objects.size()
                                          << ",\"totalObjects\":"
                                          << static_cast<unsigned>(worn_batch->total_objects)
                                          << ",\"detachAllFirst\":"
                                          << (worn_batch->first_detach_all ? "true" : "false")
                                          << "}" << std::endl;
                            }
                        }
                        // Low 397: taking one item off, named by inventory item.
                        // No SessionID on this message — the agent id is all the
                        // viewer sends, so it is all that can be checked.
                        const auto detach_item =
                            homeworldz::viewer::decode_detach_attachment_into_inv(packet->payload);
                        if (detach_item && detach_item->agent_id == identity->agent_id) {
                            const auto user_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            const auto item_id = homeworldz::viewer::format_uuid(detach_item->item_id);
                            std::vector<homeworldz::scene::EntityId> roots;
                            for (const auto& [candidate_id, candidate] : scene.entities())
                                if (candidate.attachment_point != 0 &&
                                    candidate.attachment_item_id == item_id &&
                                    candidate.owner_id == user_id)
                                    roots.push_back(candidate_id);
                            std::size_t detached = 0;
                            for (const auto root_id : roots)
                                if (!remove_attachment_linkset(root_id, now).empty()) ++detached;
                            const bool forgotten = detached == 0 || !viewer_grid ||
                                viewer_grid->set_attachment_worn(user_id, item_id, 0, false);
                            std::cout << "{\"level\":" << (detached > 0 && forgotten
                                              ? "\"info\"" : "\"warn\"")
                                      << ",\"message\":\"attachment detached into inventory\""
                                         ",\"itemId\":" << homeworldz::api::json_string(item_id)
                                      << ",\"detached\":" << detached
                                      << ",\"forgotten\":" << (forgotten ? "true" : "false")
                                      << "}" << std::endl;
                        }
                        const auto detach = homeworldz::viewer::decode_object_detach(packet->payload);
                        if (detach && detach->agent_id == identity->agent_id &&
                            detach->session_id == identity->session_id) {
                            const auto user_id = homeworldz::viewer::format_uuid(identity->agent_id);
                            std::size_t detached = 0;
                            std::size_t forgotten = 0;
                            for (const auto local_id : detach->local_ids) {
                                const auto* selected = scene.find(local_id);
                                if (!selected) continue;
                                // The viewer may name any prim of a worn linkset; the
                                // root is the one carrying the point.
                                const auto root_id = selected->attachment_point != 0
                                    ? static_cast<homeworldz::scene::EntityId>(local_id)
                                    : selected->parent_id;
                                const auto* root = scene.find(root_id);
                                if (!root || root->attachment_point == 0 || root->owner_id != user_id)
                                    continue;
                                const auto detached_item = root->attachment_item_id;
                                if (remove_attachment_linkset(root_id, now).empty()) continue;
                                ++detached;
                                // Taking off has to reach the grid, or the item comes
                                // back at the next login and taking it off looks like
                                // it did not work.
                                if (viewer_grid && !detached_item.empty() &&
                                    viewer_grid->set_attachment_worn(user_id, detached_item, 0, false))
                                    ++forgotten;
                            }
                            // The inventory item was never removed by wearing, so
                            // taking off needs no inventory write to keep the item.
                            std::cout << "{\"level\":" << (detached == detach->local_ids.size() &&
                                                           forgotten == detached
                                              ? "\"info\"" : "\"warn\"")
                                      << ",\"message\":\"attachment detach processed\",\"detached\":"
                                      << detached << ",\"forgotten\":" << forgotten
                                      << ",\"requested\":" << detach->local_ids.size()
                                      << "}" << std::endl;
                        }
                        const auto update = homeworldz::viewer::decode_agent_update(packet->payload);
                        const auto avatar = avatars.find(endpoint);
                        if (update && avatar != avatars.end() && update->agent_id == identity->agent_id &&
                            update->session_id == identity->session_id &&
                            (!avatar->second.has_agent_update || sequence_is_newer(
                                packet->sequence, avatar->second.last_agent_update_sequence))) {
                            const bool first_update = !avatar->second.has_agent_update;
                            const bool was_flying = avatar->second.controller.state().flying;
                            const bool grace_active = now < avatar->second.restored_flying_until;
                            auto accepted = *update;
                            if (grace_active && was_flying)
                                accepted.control_flags |= homeworldz::viewer::control_fly;
                            avatar->second.controller.apply(accepted);
                            avatar->second.last_agent_update = now;
                            avatar->second.last_agent_update_sequence = packet->sequence;
                            avatar->second.has_agent_update = true;
                            const bool is_flying = avatar->second.controller.state().flying;
                            if (first_update || was_flying != is_flying)
                                std::cout << "{\"level\":\"info\",\"message\":\"avatar flight control updated\",\"firstUpdate\":"
                                          << (first_update ? "true" : "false")
                                          << ",\"viewerFlying\":"
                                          << ((update->control_flags & homeworldz::viewer::control_fly) != 0 ? "true" : "false")
                                          << ",\"graceActive\":" << (grace_active ? "true" : "false")
                                          << ",\"flying\":" << (is_flying ? "true" : "false") << "}" << std::endl;
                        }
                        const auto terrain_edit = homeworldz::viewer::decode_modify_land(packet->payload);
                        if (terrain_edit && terrain_edit->agent_id == identity->agent_id &&
                            terrain_edit->session_id == identity->session_id) {
                            const auto changed = homeworldz::terrain::apply(
                                *terrain_heightmap, *revert_heightmap, *terrain_edit,
                                terrain_smooth_strength,
                                static_cast<float>(region_settings.terrain_raise),
                                static_cast<float>(region_settings.terrain_lower));
                            if (!changed.empty()) {
                                // Applying the edit is an in-memory operation and
                                // is all that happens here. Everything that costs
                                // region-scale work - persisting the heightmap,
                                // rebuilding the Jolt heightfield, telling viewers
                                // and sessions - is deferred to the tick below and
                                // coalesced.
                                //
                                // It used to run per ModifyLand packet, and
                                // Firestorm sends those continuously while the
                                // mouse is down: 586 edits in one ten-minute
                                // session meant 586 whole-heightmap writes (4 MB
                                // each on a 1024 region, 2.3 GB) and 586 rebuilds
                                // of a million-sample collision shape. The edits
                                // were applied correctly and arrived twenty
                                // seconds late because this thread could not keep
                                // up with the brush (operator report, 2026-07-30).
                                ++terrain_revision;
                                terrain_dirty = true;
                                // Sessions are told the ground moved, but not
                                // once per brush tick: a drag produced one
                                // event per tick, each of which cost a client
                                // a whole-heightmap fetch - 4 MB on a 1024
                                // region - so the edit path queued fetches
                                // faster than they completed and saturated
                                // this thread along the way. Dirty patches
                                // accumulate here and one coalesced event goes
                                // out below, carrying the union and the
                                // revision (client core measurement,
                                // 2026-07-30).
                                for (const auto& patch : changed) {
                                    const auto packed =
                                        (static_cast<std::uint32_t>(patch.y) << 8) | patch.x;
                                    pending_terrain_patches.insert(packed);
                                    pending_viewer_terrain_patches.insert(packed);
                                }
                                // Per-edit logging is itself part of the cost at
                                // brush rates; the coalesced flush reports what
                                // happened instead.
                                static_cast<void>(terrain_edit->action);
                            }
                        }
                        const auto chat = homeworldz::viewer::decode_chat_from_viewer(packet->payload);
                        if (chat && avatar != avatars.end() && chat->agent_id == identity->agent_id &&
                            chat->session_id == identity->session_id && chat->channel == 0 &&
                            !chat->message.empty() && chat->message.size() <= 1023) {
                            const auto& origin = avatar->second.controller.state().position;
                            const double radius = homeworldz::viewer::chat_range(chat->type);
                            homeworldz::viewer::ChatFromSimulator outgoing;
                            outgoing.from_name = homeworldz::viewer::format_uuid(identity->agent_id);
                            outgoing.source_id = identity->agent_id;
                            outgoing.owner_id = identity->agent_id;
                            outgoing.chat_type = chat->type;
                            outgoing.position = {static_cast<float>(origin.x), static_cast<float>(origin.y),
                                                 static_cast<float>(origin.z)};
                            outgoing.message = chat->message;
                            if (session_server)
                                session_server->broadcast_chat(outgoing.from_name, outgoing.message);
                            const auto payload = homeworldz::viewer::encode_chat_from_simulator(outgoing);
                            for (const auto& [recipient_endpoint, recipient] : avatars) {
                                const auto& target = recipient.controller.state().position;
                                const auto dx = target.x - origin.x, dy = target.y - origin.y, dz = target.z - origin.z;
                                if (dx * dx + dy * dy + dz * dz > radius * radius) continue;
                                if (const auto sent = circuits.send(recipient_endpoint, payload, true, now))
                                    static_cast<void>(send_udp(viewer_server, recipient_endpoint, *sent));
                            }
                        }
                    }
                }
                }
            }
        }
        // Embodiment commands from region sessions, drained once per tick
        // (docs/CLIENT2-EMBODIMENT.md). The lws thread never touches the
        // scene; everything scene-facing happens here.
        if (session_server) {
            using SessionKind = homeworldz::session::Command::Kind;
            for (auto& inbound : session_server->drain_commands()) {
                const auto participant_key = "ws:" + inbound.session_id;
                if (inbound.disconnect) {
                    retire_session_avatar(participant_key);
                    continue;
                }
                switch (inbound.command.kind) {
                case SessionKind::leave:
                    retire_session_avatar(participant_key);
                    break;
                case SessionKind::spawn: {
                    if (inbound.command.draw_distance >= 0.0)
                        session_draw_distances[inbound.session_id] =
                            std::clamp(inbound.command.draw_distance, 16.0, 512.0);
                    else
                        session_draw_distances.emplace(inbound.session_id, 128.0);
                    const auto spawned_reply = [&](const LiveAvatar& live) {
                        const auto& state = live.controller.state();
                        const double qx = state.rotation[0], qy = state.rotation[1],
                                     qz = state.rotation[2];
                        const auto qw = session_quat_w(qx, qy, qz);
                        session_server->send_to(inbound.session_id,
                            homeworldz::session::encode_envelope("spawned", {},
                                "{\"entityId\":\"" + std::to_string(live.entity_id) + "\"" +
                                ",\"position\":" + session_vec3(state.position.x,
                                    state.position.y, state.position.z) +
                                ",\"lookAt\":" + session_vec3(
                                    1.0 - 2.0 * (qy * qy + qz * qz),
                                    2.0 * (qx * qy + qw * qz), 0.0) +
                                // The avatar's own geometry, as the region
                                // computed it from shape: with the published
                                // capsule contract this is everything ground
                                // support needs (client core request,
                                // 2026-07-29). Additive fields.
                                ",\"height\":" + std::to_string(state.height) +
                                ",\"hipOffset\":" + std::to_string(state.hip_offset) + "}"));
                    };
                    if (const auto existing = avatars.find(participant_key);
                        existing != avatars.end()) {
                        spawned_reply(existing->second);  // idempotent
                        break;
                    }
                    homeworldz::scene::EntityId entity{};
                    std::vector<homeworldz::scene::EntityId> duplicates;
                    for (const auto& [candidate_id, candidate] : scene.entities()) {
                        if (candidate.name != inbound.user_id) continue;
                        if (candidate_id > entity) {
                            if (entity != 0) duplicates.push_back(entity);
                            entity = candidate_id;
                        } else {
                            duplicates.push_back(candidate_id);
                        }
                    }
                    for (const auto duplicate : duplicates) scene.remove(duplicate);
                    if (entity == 0) entity = scene.create(inbound.user_id, initial_spawn);
                    auto* persisted = scene.find(entity);
                    // World entry's resolved arrival point wins — that is how
                    // a named start, and a crossing's continuation, land where
                    // they were asked to. The controller clamps it into the
                    // region, so an out-of-bounds value cannot take effect.
                    const auto spawn = inbound.arrival ?
                        homeworldz::scene::Vector3{(*inbound.arrival)[0], (*inbound.arrival)[1],
                                                   (*inbound.arrival)[2]} :
                        (persisted ? persisted->position : initial_spawn);
                    // A session client sends no appearance — texture-entry
                    // blobs are a legacy shape it will never speak — so seed
                    // the server-side default-outfit bake, exactly as an
                    // appearance-less viewer gets. Without it viewers render
                    // a default-shaped body, and the physics capsule keeps
                    // default dimensions (the bent-knee stance).
                    const auto session_agent = homeworldz::viewer::parse_uuid(inbound.user_id);
                    if (session_agent && !avatar_appearances.contains(participant_key)) {
                        if (const auto* bake = ensure_default_outfit_bake()) {
                            homeworldz::viewer::AgentSetAppearance seeded;
                            seeded.agent_id = *session_agent;
                            seeded.serial = 1;
                            seeded.texture_entry = bake->texture_entry;
                            seeded.visual_params = default_outfit_visual_params;
                            seeded.appearance_version = 1;
                            avatar_appearances.insert_or_assign(participant_key, seeded);
                            const auto geometry = homeworldz::viewer::avatar_geometry(seeded);
                            if (geometry) avatar_geometries[participant_key] = *geometry;
                            std::cout << "{\"level\":\"info\",\"message\":\"session avatar appearance seeded\""
                                      << ",\"agent\":" << homeworldz::api::json_string(inbound.user_id)
                                      << ",\"height\":" << (geometry ? geometry->height : 0.0)
                                      << "}" << std::endl;
                        }
                    }
                    const auto known_session_geometry = avatar_geometries.find(participant_key);
                    const auto session_geometry =
                        known_session_geometry == avatar_geometries.end() ?
                            homeworldz::viewer::AvatarGeometry{} : known_session_geometry->second;
                    homeworldz::viewer::AvatarController controller{
                        spawn, collision_ground_height(spawn),
                        session_geometry.height, session_geometry.hip_offset,
                        static_cast<double>(region_size_x),
                        static_cast<double>(region_size_y)};
                    if (persisted)
                        controller.restore_motion(persisted->velocity,
                            {static_cast<float>(persisted->rotation.x),
                             static_cast<float>(persisted->rotation.y),
                             static_cast<float>(persisted->rotation.z)},
                            persisted->avatar_flying);
                    const auto initial_viewer_position = controller.viewer_position();
                    const auto [iterator, inserted] = avatars.emplace(participant_key,
                        LiveAvatar{std::move(controller), entity, inbound.user_id,
                                   now + std::chrono::seconds(5), now + std::chrono::seconds(30),
                                   now + std::chrono::milliseconds(100), initial_viewer_position});
                    static_cast<void>(inserted);
                    auto& live = iterator->second;
                    live.transport = AvatarTransport::session;
                    live.session_id = inbound.session_id;
                    live.last_pong = now;
                    // Seed the stored draw distance into controller state so
                    // the dynamic-object interest check never sees zero.
                    homeworldz::viewer::AvatarController::MovementInput seed{};
                    seed.body_rotation = live.controller.state().rotation;
                    seed.draw_distance = static_cast<float>(
                        session_draw_distances[inbound.session_id]);
                    live.controller.apply(seed);
                    if (physics_world) {
                        live.physics_character = physics_world->create_character(
                            character_definition(entity,
                                live.controller.state().position,
                                live.controller.state().height));
                        physics_world->set_character_velocity(
                            live.physics_character, live.controller.state().velocity);
                        physics_world->set_character_flying(
                            live.physics_character, live.controller.state().flying);
                    }
                    if (viewer_grid && registration)
                        static_cast<void>(viewer_grid->update_presence(
                            inbound.user_id, registration->region_id()));
                    // Before the initial scene below, so what this avatar is
                    // wearing is part of that scene rather than an update
                    // arriving after the client has drawn everything.
                    restore_attachments(inbound.user_id, entity, now);
                    spawned_reply(live);
                    // The arrival greeting, matching the viewer path: private
                    // to this session, {user} resolved to the display name
                    // the ticket carried.
                    if (const auto greeting = welcome_chat_message(
                            inbound.display_name.empty() ? inbound.user_id
                                                         : inbound.display_name,
                            region_name);
                        !greeting.empty()) {
                        session_server->send_to(inbound.session_id,
                            homeworldz::session::encode_envelope("chat", {},
                                "{\"from\":" + homeworldz::session::json_string(region_name) +
                                ",\"message\":" +
                                homeworldz::session::json_string(greeting) + "}"));
                    }
                    // Initial scene: every other avatar, then every non-avatar
                    // entity. Terrain deliberately not sent (design decision 4).
                    std::unordered_set<homeworldz::scene::EntityId> avatar_entities;
                    auto& known_avatars = session_avatar_interest[participant_key];
                    // Self is always in interest: its own transforms are its
                    // authoritative position, and the client knows itself from
                    // the spawned reply rather than an avatar message.
                    known_avatars.insert(entity);
                    for (const auto& [other_key, other] : avatars) {
                        avatar_entities.insert(other.entity_id);
                        if (other_key == participant_key) continue;
                        if (!session_interested(live, other)) continue;
                        session_server->send_to(inbound.session_id,
                                                session_avatar_envelope(other, other_key));
                        known_avatars.insert(other.entity_id);
                    }
                    for (const auto& [entity_id, scene_entity] : scene.entities()) {
                        if (avatar_entities.count(entity_id) != 0 ||
                            !session_object_visible(scene_entity)) continue;
                        session_server->send_to(inbound.session_id,
                                                session_object_envelope(scene_entity));
                    }
                    // Announce the arrival to viewers and other sessions: the
                    // avatar itself, then its appearance, so a viewer rezzes a
                    // properly shaped and dressed body rather than a default.
                    if (session_agent) {
                        const auto session_region_handle =
                            (static_cast<std::uint64_t>(region_grid_x * 256) << 32) |
                            static_cast<std::uint32_t>(region_grid_y * 256);
                        const auto announce = homeworldz::viewer::encode_avatar_object_update(
                            session_region_handle, static_cast<std::uint32_t>(entity), *session_agent,
                            {static_cast<float>(live.controller.state().position.x),
                             static_cast<float>(live.controller.state().position.y),
                             static_cast<float>(live.controller.state().position.z)});
                        std::vector<std::byte> appearance;
                        if (const auto seeded = avatar_appearances.find(participant_key);
                            seeded != avatar_appearances.end())
                            appearance = homeworldz::viewer::encode_avatar_appearance({
                                *session_agent, seeded->second.serial, seeded->second.texture_entry,
                                seeded->second.visual_params, {}, seeded->second.appearance_version});
                        for (const auto& [recipient_endpoint, recipient] : avatars) {
                            static_cast<void>(recipient);
                            if (recipient_endpoint == participant_key) continue;
                            if (const auto sent = circuits.send(
                                    recipient_endpoint, announce, true, now, true))
                                static_cast<void>(send_udp(
                                    viewer_server, recipient_endpoint, *sent));
                            if (appearance.empty()) continue;
                            if (const auto dressed = circuits.send(
                                    recipient_endpoint, appearance, true, now, true))
                                static_cast<void>(send_udp(
                                    viewer_server, recipient_endpoint, *dressed));
                        }
                    }
                    const auto arrival_envelope = session_avatar_envelope(live, participant_key);
                    for (const auto& [other_key, other] : avatars) {
                        if (other_key == participant_key ||
                            other.transport != AvatarTransport::session) continue;
                        if (!session_interested(other, live)) continue;
                        session_server->send_to(other.session_id, arrival_envelope);
                        session_avatar_interest[other_key].insert(entity);
                    }
                    std::cout << "{\"level\":\"info\",\"message\":\"session avatar spawned\",\"agent\":"
                              << homeworldz::api::json_string(inbound.user_id)
                              << ",\"localId\":" << static_cast<std::uint64_t>(entity)
                              << "}" << std::endl;
                    break;
                }
                case SessionKind::move: {
                    const auto found = avatars.find(participant_key);
                    if (found == avatars.end()) break;
                    if (inbound.command.draw_distance >= 0.0)
                        session_draw_distances[inbound.session_id] =
                            std::clamp(inbound.command.draw_distance, 16.0, 512.0);
                    const auto& state = found->second.controller.state();
                    homeworldz::viewer::AvatarController::MovementInput input{};
                    input.control_flags = inbound.command.controls;
                    input.body_rotation = inbound.command.body_rotation;
                    if (inbound.command.has_camera) {
                        input.camera_center = inbound.command.camera_center;
                        input.camera_at = inbound.command.camera_at;
                        input.camera_left = inbound.command.camera_left;
                        input.camera_up = inbound.command.camera_up;
                    } else {
                        input.camera_center = state.camera_center;
                        input.camera_at = state.camera_at;
                        input.camera_left = state.camera_left;
                        input.camera_up = state.camera_up;
                    }
                    input.draw_distance = static_cast<float>(
                        session_draw_distances[inbound.session_id]);
                    found->second.controller.apply(input);
                    found->second.last_agent_update = now;
                    break;
                }
                case SessionKind::say: {
                    const auto found = avatars.find(participant_key);
                    if (found == avatars.end()) {
                        session_server->send_to(inbound.session_id,
                            homeworldz::session::encode_envelope("error", {},
                                "{\"code\":\"not_spawned\",\"message\":\"say requires an avatar; send spawn first\"}"));
                        break;
                    }
                    const auto agent = homeworldz::viewer::parse_uuid(inbound.user_id);
                    if (!agent) break;
                    const auto& origin = found->second.controller.state().position;
                    homeworldz::viewer::ChatFromSimulator outgoing;
                    outgoing.from_name = inbound.display_name.empty() ?
                        inbound.user_id : inbound.display_name;
                    outgoing.source_id = *agent;
                    outgoing.owner_id = *agent;
                    outgoing.chat_type = 1;  // say
                    outgoing.position = {static_cast<float>(origin.x),
                                         static_cast<float>(origin.y),
                                         static_cast<float>(origin.z)};
                    outgoing.message = inbound.command.message;
                    const auto payload = homeworldz::viewer::encode_chat_from_simulator(outgoing);
                    const double radius = homeworldz::viewer::chat_range(outgoing.chat_type);
                    for (const auto& [recipient_endpoint, recipient] : avatars) {
                        const auto& target = recipient.controller.state().position;
                        const auto dx = target.x - origin.x, dy = target.y - origin.y,
                                   dz = target.z - origin.z;
                        if (dx * dx + dy * dy + dz * dz > radius * radius) continue;
                        if (const auto sent = circuits.send(
                                recipient_endpoint, payload, true, now, true))
                            static_cast<void>(send_udp(
                                viewer_server, recipient_endpoint, *sent));
                    }
                    session_server->broadcast(homeworldz::session::encode_envelope("chat", {},
                        "{\"from\":" + homeworldz::session::json_string(outgoing.from_name) +
                        ",\"fromId\":" + homeworldz::session::json_string(inbound.user_id) +
                        ",\"position\":" + session_vec3(origin.x, origin.y, origin.z) +
                        ",\"message\":" +
                        homeworldz::session::json_string(inbound.command.message) + "}"));
                    break;
                }
                }
            }
        }
        for (const auto& outgoing : circuits.poll(now))
            static_cast<void>(send_udp(viewer_server, outgoing.endpoint, outgoing.bytes));
        for (auto iterator = texture_packets.begin(); iterator != texture_packets.end();) {
            auto& queue = iterator->second;
            while (!queue.empty()) {
                const auto outgoing = circuits.send(iterator->first, queue.front().payload, true, now);
                if (!outgoing) break;
                static_cast<void>(send_udp(viewer_server, iterator->first, *outgoing));
                const auto completed_asset = queue.front().last ? queue.front().asset_id : std::string{};
                queue.pop_front();
                if (!completed_asset.empty()) {
                    active_texture_transfers.erase(iterator->first + '|' + completed_asset);
                    std::cout << "{\"level\":\"info\",\"message\":\"texture transfer sent\","
                                 "\"assetId\":" << homeworldz::api::json_string(completed_asset) << "}"
                              << std::endl;
                }
            }
            if (queue.empty()) iterator = texture_packets.erase(iterator);
            else ++iterator;
        }
        const auto script_tick = falcon.run_tick();
        if (script_tick.trapped != 0) {
            std::cerr << "{\"level\":\"warning\",\"message\":\"Falcon script runtime trapped\","
                         "\"scriptsVisited\":"
                      << script_tick.scripts_visited << ",\"instructions\":"
                      << script_tick.instructions << ",\"trapped\":"
                      << script_tick.trapped << "}" << std::endl;
        }
        const auto elapsed = std::chrono::duration<double>(now - previous_tick).count();
        const auto fixed_steps = simulation.advance(elapsed);
        std::vector<std::pair<std::string, std::string>> departed_avatars;
        // One terrain event per quarter second at most, carrying the union of
        // everything dirtied since the last one. Lossy by design and safe
        // because the revision rides along: a client that misses this entirely
        // still learns it is behind the next time it hears from us or reads an
        // ETag.
        if (!pending_terrain_patches.empty() && now >= next_terrain_notice && session_server) {
            std::string patch_list;
            for (const auto packed : pending_terrain_patches) {
                if (!patch_list.empty()) patch_list += ',';
                patch_list += '[' + std::to_string(packed & 0xffu) + ',' +
                              std::to_string((packed >> 8) & 0xffu) + ']';
            }
            // The heights themselves, so an edit costs no fetch at all: a 16x16
            // patch of float32 is 1 KB, against a whole-heightmap fetch of 4 MB
            // on a 1024 region. Each entry carries its own origin rather than
            // relying on arrival order - a coalesced event whose positions are
            // implied by array order is a contract that breaks the first time
            // either side sorts it (client core, 2026-07-30). The block is the
            // same heightmap-f32le the map endpoint serves, patch-sized and
            // row-major within the patch: one published encoding, not two.
            constexpr std::size_t patch_extent = 16;
            constexpr std::size_t maximum_patches_with_heights = 64;
            std::string heights;
            if (pending_terrain_patches.size() <= maximum_patches_with_heights) {
                for (const auto packed : pending_terrain_patches) {
                    const auto patch_x = static_cast<std::size_t>(packed & 0xffu);
                    const auto patch_y = static_cast<std::size_t>((packed >> 8) & 0xffu);
                    std::vector<std::byte> block;
                    block.reserve(patch_extent * patch_extent * sizeof(float));
                    for (std::size_t row = 0; row < patch_extent; ++row)
                        for (std::size_t column = 0; column < patch_extent; ++column) {
                            const auto x = patch_x * patch_extent + column;
                            const auto y = patch_y * patch_extent + row;
                            const float sample = x < terrain_width && y < terrain_width
                                ? (*terrain_heightmap)[y * terrain_width + x] : 0.0F;
                            const auto bits = std::bit_cast<std::uint32_t>(sample);
                            for (int shift = 0; shift < 32; shift += 8)
                                block.push_back(static_cast<std::byte>((bits >> shift) & 0xffu));
                        }
                    if (!heights.empty()) heights += ',';
                    heights += "{\"x\":" + std::to_string(patch_x) +
                               ",\"y\":" + std::to_string(patch_y) +
                               ",\"data\":\"" + homeworldz::session::base64(block) + "\"}";
                }
            }
            const auto notice = homeworldz::session::encode_envelope(
                "terrainChanged", {},
                "{\"path\":\"/session/terrain\",\"patchSize\":16"
                ",\"revision\":" + std::to_string(terrain_revision) +
                ",\"patches\":[" + patch_list + "]" +
                // Absent when a burst dirties more than the cap: the revision
                // makes that safe, because the client refetches on a mismatch
                // rather than being left silently behind.
                (heights.empty() ? std::string{} : ",\"heights\":[" + heights + "]") +
                "}");
            std::size_t told = 0;
            for (const auto& [session_key, session_avatar] : avatars) {
                static_cast<void>(session_key);
                if (session_avatar.transport != AvatarTransport::session) continue;
                session_server->send_to(session_avatar.session_id, notice);
                ++told;
            }
            std::cout << "{\"level\":\"info\",\"message\":\"terrain change announced\""
                         ",\"revision\":" << terrain_revision
                      << ",\"patches\":" << pending_terrain_patches.size()
                      << ",\"heightsIncluded\":" << (heights.empty() ? "false" : "true")
                      << ",\"sessions\":" << told << "}" << std::endl;
            pending_terrain_patches.clear();
            last_terrain_notice_at = now;
            next_terrain_notice = now + std::chrono::milliseconds(250);
        }

        // Viewers learn the same way, at the same cadence and for the union
        // rather than per edit: LayerData is what makes an edit visible, and it
        // was being encoded and sent to every avatar once per ModifyLand.
        if (!pending_viewer_terrain_patches.empty() && now >= next_viewer_terrain_notice) {
            std::vector<homeworldz::viewer::TerrainPatch> patches;
            patches.reserve(pending_viewer_terrain_patches.size());
            for (const auto packed : pending_viewer_terrain_patches)
                patches.push_back({static_cast<std::uint8_t>(packed & 0xffu),
                                   static_cast<std::uint8_t>((packed >> 8) & 0xffu)});
            // Pack by encoded size, not by a patch count. Compressed patches
            // vary from tens of bytes to hundreds depending on how busy the
            // ground is, and a datagram over the path MTU fragments and gets
            // dropped - which is invisible here and shows up as terrain that
            // missed spots along a stroke. Coalescing made this reachable: one
            // edit dirtied about three patches and always fit, while a quarter
            // second of a fast brush unions many (operator report, 2026-07-30).
            constexpr std::size_t datagram_budget = 1000;
            constexpr std::size_t patches_per_packet = 4;
            std::vector<std::vector<std::byte>> payloads;
            for (std::size_t offset = 0; offset < patches.size(); ) {
                auto count = (std::min)(patches_per_packet, patches.size() - offset);
                auto payload = homeworldz::viewer::encode_terrain(
                    std::span<const homeworldz::viewer::TerrainPatch>(
                        patches.data() + offset, count), *terrain_heightmap);
                // Too big, or refused outright: fall back to one patch per
                // packet, which is the smallest unit the format has.
                while (count > 1 && (payload.empty() || payload.size() > datagram_budget)) {
                    count = 1;
                    payload = homeworldz::viewer::encode_terrain(
                        std::span<const homeworldz::viewer::TerrainPatch>(
                            patches.data() + offset, count), *terrain_heightmap);
                }
                if (payload.empty()) {
                    // A single patch that will not encode is a real fault and
                    // used to be dropped in silence.
                    std::cerr << "{\"level\":\"error\",\"message\":\"terrain patch would not encode\""
                                 ",\"x\":" << static_cast<unsigned>(patches[offset].x)
                              << ",\"y\":" << static_cast<unsigned>(patches[offset].y)
                              << "}" << std::endl;
                    ++offset;
                    continue;
                }
                payloads.push_back(std::move(payload));
                offset += count;
            }
            for (const auto& [recipient_endpoint, recipient] : avatars) {
                static_cast<void>(recipient);
                for (const auto& payload : payloads)
                    if (const auto outgoing = circuits.send(
                            recipient_endpoint, payload, true, now))
                        static_cast<void>(send_udp(viewer_server, recipient_endpoint, *outgoing));
            }
            pending_viewer_terrain_patches.clear();
            next_viewer_terrain_notice = now + std::chrono::milliseconds(250);
        }

        // The collision surface follows promptly but not per edit: an avatar
        // standing on ground being smoothed needs it within a few frames, and
        // rebuilding a million-sample heightfield is not a per-packet cost.
        if (terrain_dirty && now >= next_terrain_physics) {
            if (synchronize_physics_terrain() && physics_world) {
                // Ground that rose into an avatar has to lift it. Jolt owns
                // grounding for characters (AvatarController::synchronize_physics
                // sets physics_grounding_, which disables the controller's own
                // snap), so a capsule left inside the new heightfield is reported
                // unsupported and falls - through ground it cannot leave.
                //
                // The operator found it while terraforming (2026-08-05): raising
                // land into yourself gave a screen alternating between the world
                // and black at this rebuild's own 500 ms cadence, with a falling
                // animation. Flying stopped the fall and left the camera inside
                // the hill, which is the same fault holding still.
                //
                // Only ever upward. Terrain lowered away from an avatar should
                // drop it, and that already works.
                for (auto& [lift_key, lift_avatar] : avatars) {
                    static_cast<void>(lift_key);
                    if (lift_avatar.physics_character == 0) continue;
                    const auto state = lift_avatar.controller.state();
                    const auto ground = collision_ground_height(state.position);
                    // The capsule's centre sits half its height above the ground,
                    // the same support rule published to session clients.
                    const auto support = ground + state.height * 0.5;
                    if (state.position.z >= support) continue;
                    auto placed = physics_world->character_state(lift_avatar.physics_character);
                    if (!placed) continue;
                    placed->position.z = support;
                    // Any downward velocity is the fall this is correcting; keep
                    // horizontal motion so someone walking is not stopped dead.
                    if (placed->linear_velocity.z < 0.0) placed->linear_velocity.z = 0.0;
                    placed->grounded = true;
                    physics_world->set_character_state(lift_avatar.physics_character, *placed);
                    lift_avatar.controller.synchronize_physics(
                        placed->position, placed->linear_velocity, true);
                    std::cout << "{\"level\":\"info\",\"message\":\"avatar lifted by rising"
                                 " terrain\",\"user\":"
                              << homeworldz::api::json_string(lift_avatar.user_id)
                              << ",\"from\":" << state.position.z
                              << ",\"to\":" << support << "}" << std::endl;
                }
            }
            next_terrain_physics = now + std::chrono::milliseconds(500);
        }

        // Durability is the slowest cadence of the three, because the whole
        // heightmap is written each time and nothing is lost by trailing: an
        // unclean stop can cost at most the last few seconds of terraforming,
        // and a clean one flushes on shutdown.
        if (terrain_dirty && now >= next_terrain_persist) {
            const auto persisted = homeworldz::terrain::save_state(
                terrain_state_path, *terrain_heightmap);
            persist_terrain_revision();
            if (!persisted)
                std::cerr << "{\"level\":\"error\",\"message\":\"terrain persistence failed\""
                             ",\"revision\":" << terrain_revision << "}" << std::endl;
            terrain_dirty = false;
            next_terrain_persist = now + std::chrono::seconds(3);
        }

        // Session avatars that crossed a border this tick; retired after the
        // loop so the map is not mutated while iterating.
        std::vector<std::string> crossing_session_avatars;
        for (auto& [endpoint, avatar] : avatars) {
            if (!avatar.outbound_transit_id.empty() && now >= avatar.outbound_transit_expires) {
                if (viewer_grid && registration)
                    static_cast<void>(viewer_grid->rollback_avatar_transit(
                        avatar.outbound_transit_id, registration->region_id(),
                        "viewer did not activate border crossing"));
                std::cout << "{\"level\":\"warning\",\"message\":\"avatar border crossing expired\","
                             "\"transitId\":"
                          << homeworldz::api::json_string(avatar.outbound_transit_id) << "}" << std::endl;
                avatar.outbound_transit_id.clear();
            }
            if (physics_world && avatar.physics_character != 0)
                if (const auto state = physics_world->character_state(avatar.physics_character))
                    avatar.controller.synchronize_physics(
                        state->position, state->linear_velocity, state->grounded);
            if (avatar.has_agent_update &&
                now - avatar.last_agent_update > std::chrono::seconds(1))
                avatar.controller.expire_transient_controls();
            // LLUDP liveness only: a session participant's liveness is its
            // socket (close synthesizes a disconnect command), and its ticket
            // was grid-validated at auth.
            if (avatar.transport == AvatarTransport::lludp && now >= avatar.next_ping) {
                const auto* circuit_identity = circuits.identity(endpoint);
                const auto session_id = circuit_identity ?
                    homeworldz::viewer::format_uuid(circuit_identity->session_id) : std::string{};
                if (now - avatar.last_pong > connection_timeout) {
                    // Connection lost: the viewer has not answered a ping within
                    // the timeout (crash, force-kill, or sustained packet loss).
                    // Retire it (the departed path broadcasts the KillObject)
                    // instead of waiting for the grid session TTL.
                    std::cout << "{\"level\":\"info\",\"message\":\"viewer connection lost (no ping reply)\","
                                 "\"sessionId\":"
                              << homeworldz::api::json_string(session_id) << ",\"secondsSincePong\":"
                              << std::chrono::duration_cast<std::chrono::seconds>(
                                     now - avatar.last_pong).count()
                              << "}" << std::endl;
                    departed_avatars.emplace_back(endpoint, session_id);
                    continue;
                }
                try {
                    const auto session = circuit_identity && viewer_sessions ?
                        viewer_sessions->validate(session_id, now) : std::nullopt;
                    if (!session || !registration ||
                        session->destination_region_id != registration->region_id()) {
                        departed_avatars.emplace_back(endpoint, session_id);
                        continue;
                    }
                } catch (const std::exception& error) {
                    std::cout << "{\"level\":\"warning\",\"message\":\"avatar authority check failed\",\"error\":"
                              << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                }
                if (const auto ping = circuits.send(endpoint,
                        homeworldz::viewer::encode_start_ping_check(++avatar.ping_id), false, now))
                    static_cast<void>(send_udp(viewer_server, endpoint, *ping));
                avatar.next_ping = now + std::chrono::seconds(5);
            }
            if (now >= avatar.next_presence && viewer_grid && registration) {
                // The result was discarded here and at every other presence call
                // site, so a failing heartbeat looked exactly like a working one.
                // The grid's own rows are how this surfaced: an avatar logged in
                // and out of Gamma on 2026-08-06 while its presence row still read
                // 2026-08-05, and nothing anywhere had said a word. Logged at most
                // once per 30s per avatar, which is the heartbeat's own rate.
                if (!viewer_grid->update_presence(avatar.user_id, registration->region_id())) {
                    std::cout << "{\"level\":\"warn\",\"message\":\"presence update refused by grid\""
                              << ",\"userId\":\"" << avatar.user_id << "\""
                              << ",\"regionId\":\"" << registration->region_id() << "\"}"
                              << std::endl;
                }
                avatar.next_presence = now + std::chrono::seconds(30);
            }
            avatar.controller.set_ground_height(
                collision_ground_height(avatar.controller.state().position));
            // A session avatar crosses by re-entering the neighbor rather than
            // by the viewers' transit handoff (docs/CLIENT2-EMBODIMENT.md
            // milestone E2), so it only needs the border detected — and only
            // toward a neighbor that serves sessions, since a client cannot
            // continue anywhere else. Containment still applies otherwise,
            // which is what keeps an avatar from walking into open space.
            const bool session_avatar = avatar.transport == AvatarTransport::session;
            const bool may_cross = !session_avatar || std::any_of(
                region_neighbors.begin(), region_neighbors.end(),
                [](const auto& neighbor) {
                    return neighbor.online && !neighbor.session_endpoint.empty();
                });
            const bool has_online_neighbor = may_cross && std::any_of(
                region_neighbors.begin(), region_neighbors.end(),
                [](const auto& neighbor) { return neighbor.online; });
            avatar.controller.set_border_crossing_enabled(
                has_online_neighbor && avatar.outbound_transit_id.empty());
            if (!avatar.outbound_transit_id.empty())
                avatar.controller.expire_transient_controls();
            avatar.controller.step(elapsed);
            if (may_cross && avatar.outbound_transit_id.empty()) {
                const auto& position = avatar.controller.state().position;
                const auto crossing = homeworldz::region::plan_avatar_border_crossing(
                    region_grid_x, region_grid_y, region_size_x, region_size_y,
                    {position.x, position.y, position.z}, region_neighbors);
                // A session avatar's crossing is a re-entry: tell the client
                // where to continue and retire the avatar here, in the same
                // tick the border is detected so it never wanders outside the
                // region. The transit machinery below stays viewer-only.
                if (crossing && session_avatar) {
                    if (crossing->destination.session_endpoint.empty()) {
                        avatar.controller.set_border_crossing_enabled(false);
                    } else {
                        const auto arrival = crossing->position;
                        const auto& rotation = avatar.controller.state().rotation;
                        const double qx = rotation[0], qy = rotation[1], qz = rotation[2];
                        const auto qw = session_quat_w(qx, qy, qz);
                        // start is pre-formatted so the client hands it back
                        // to world entry verbatim, with no float-format risk.
                        const auto start = crossing->destination.name + "/" +
                            std::to_string(static_cast<int>(arrival[0])) + "/" +
                            std::to_string(static_cast<int>(arrival[1])) + "/" +
                            std::to_string(static_cast<int>(arrival[2]));
                        session_server->send_to(avatar.session_id,
                            homeworldz::session::encode_envelope("crossing", {},
                                "{\"region\":" +
                                homeworldz::session::json_string(crossing->destination.name) +
                                ",\"sessionURL\":" + homeworldz::session::json_string(
                                    crossing->destination.session_endpoint) +
                                ",\"start\":" + homeworldz::session::json_string(start) +
                                ",\"position\":" + session_vec3(arrival[0], arrival[1], arrival[2]) +
                                ",\"lookAt\":" + session_vec3(1.0 - 2.0 * (qy * qy + qz * qz),
                                    2.0 * (qx * qy + qw * qz), 0.0) + "}"));
                        std::cout << "{\"level\":\"info\",\"message\":\"session avatar crossing\",\"agent\":"
                                  << homeworldz::api::json_string(avatar.user_id)
                                  << ",\"destination\":"
                                  << homeworldz::api::json_string(crossing->destination.name)
                                  << "}" << std::endl;
                        crossing_session_avatars.push_back(endpoint);
                    }
                    continue;
                }
                if (crossing && now >= avatar.next_crossing_attempt) {
                    const auto* identity = circuits.identity(endpoint);
                    const auto simulator = simulator_event_endpoint(
                        crossing->destination.public_endpoint,
                        crossing->destination.viewer_port);
                    bool prepared = false;
                    std::string transit_id;
                    try {
                        if (!identity || !viewer_grid || !registration || !simulator)
                            throw std::runtime_error("crossing services are unavailable");
                        const auto session_id = homeworldz::viewer::format_uuid(identity->session_id);
                        const auto agent_id = homeworldz::viewer::format_uuid(identity->agent_id);
                        transit_id = homeworldz::viewer::random_uuid();
                        const auto look_direction = avatar.controller.look_direction();
                        const bool flying = avatar.controller.state().flying;
                        const homeworldz::grid::AvatarTransitRequest request{
                            transit_id, agent_id, session_id, registration->region_id(),
                            crossing->destination.id, crossing->position, look_direction, flying, 30};
                        const auto transit = viewer_grid->prepare_avatar_transit(request);
                        prepared = transit && transit->state == "prepared";
                        if (!prepared)
                            throw std::runtime_error("grid rejected border crossing preparation");
                        auto destination = homeworldz::grid::socket_transport(
                            crossing->destination.public_endpoint, service_token);
                        if (!homeworldz::grid::prepare_avatar_arrival(*destination, transit_id))
                            throw std::runtime_error("destination rejected border crossing preparation");
                        const auto target_handle =
                            (static_cast<std::uint64_t>(crossing->destination.grid_x * 256) << 32) |
                            static_cast<std::uint32_t>(crossing->destination.grid_y * 256);
                        enqueue_viewer_event(session_id,
                            homeworldz::viewer::enable_simulator_event_xml(
                                target_handle, *simulator,
                                static_cast<std::uint32_t>(crossing->destination.size_x),
                                static_cast<std::uint32_t>(crossing->destination.size_y)));
                        enqueue_viewer_event(session_id,
                            homeworldz::viewer::crossed_region_event_xml({
                                agent_id, session_id, target_handle, *simulator,
                                crossing->destination.public_endpoint + "/caps/seed/" + session_id +
                                    "/" + transit_id,
                                crossing->position, look_direction,
                                static_cast<std::uint32_t>(crossing->destination.size_x),
                                static_cast<std::uint32_t>(crossing->destination.size_y)}));
                        avatar.outbound_transit_id = transit_id;
                        avatar.outbound_transit_expires = now + std::chrono::seconds(30);
                        std::cout << "{\"level\":\"info\",\"message\":\"avatar border crossing signaled\","
                                     "\"transitId\":"
                                  << homeworldz::api::json_string(transit_id)
                                  << ",\"destinationRegionId\":"
                                  << homeworldz::api::json_string(crossing->destination.id) << "}"
                                  << std::endl;
                    } catch (const std::exception& error) {
                        if (prepared && viewer_grid && registration)
                            static_cast<void>(viewer_grid->rollback_avatar_transit(
                                transit_id, registration->region_id(), error.what()));
                        std::cout << "{\"level\":\"error\",\"message\":\"avatar border crossing preparation failed\","
                                     "\"error\":"
                                  << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                        avatar.next_crossing_attempt = now + std::chrono::seconds(1);
                    }
                    avatar.controller.contain_horizontal();
                } else if (position.x < 0.0 || position.x > region_size_x ||
                           position.y < 0.0 || position.y > region_size_y) {
                    avatar.controller.contain_horizontal();
                }
            } else {
                avatar.controller.contain_horizontal();
            }
            if (physics_world && avatar.physics_character != 0) {
                const auto& controller_state = avatar.controller.state();
                physics_world->set_character_velocity(avatar.physics_character, controller_state.velocity);
                physics_world->set_character_flying(
                    avatar.physics_character, controller_state.flying);
            }
            const auto desired_animation = avatar.controller.movement_animation();
            const auto movement_agent_id = homeworldz::viewer::parse_uuid(avatar.user_id);
            auto& animations = avatar_animations[endpoint];
            // Read by value, not held as an iterator: the map is written below,
            // and an iterator read afterwards yields the new value. That is the
            // bug that made avatars flail (2026-08-04).
            const auto previous = movement_animations.find(endpoint);
            const bool had_previous = previous != movement_animations.end();
            const auto previous_animation =
                had_previous ? previous->second : desired_animation;
            const bool state_changed = !had_previous || previous_animation != desired_animation;
            if (state_changed) {
                // Recorded outside the viewer branch below: that branch needs the
                // legacy UUID to parse, so a state without one would never be
                // recorded and would re-announce itself on every tick forever.
                movement_animations.insert_or_assign(endpoint, desired_animation);
                if (session_server) {
                    // The state's portable name, not its Linden asset id. Sent
                    // on change rather than on every transform, because that is
                    // when it changes - transforms run at frame rate and this
                    // does not. Same interest filter as transform: a client is
                    // not told about an avatar it has not been told exists.
                    const auto motion_envelope = session_motion_envelope(avatar, endpoint);
                    for (const auto& [recipient_key, recipient] : avatars) {
                        if (recipient.transport != AvatarTransport::session) continue;
                        const auto known = session_avatar_interest.find(recipient_key);
                        if (known == session_avatar_interest.end() ||
                            !known->second.contains(avatar.entity_id))
                            continue;
                        session_server->send_to(recipient.session_id, motion_envelope);
                    }
                }
            }
            // The list owns its own correctness: exactly one movement animation,
            // whichever the state now is. It needs no previous value and so cannot
            // drift from one. Viewers are told only when the list actually changed.
            if (movement_agent_id) {
                auto& sequence = next_animation_sequences[endpoint];
                if (homeworldz::viewer::apply_movement_animation(
                        animations, desired_animation, *movement_agent_id, sequence)) {
                    const homeworldz::viewer::AvatarAnimation update{
                        *movement_agent_id, animations};
                    const auto payload = homeworldz::viewer::encode_avatar_animation(update);
                    for (const auto& [recipient_endpoint, recipient] : avatars) {
                        static_cast<void>(recipient);
                        if (const auto outgoing = circuits.send(
                                recipient_endpoint, payload, false, now, true))
                            static_cast<void>(send_udp(viewer_server, recipient_endpoint, *outgoing));
                    }
                }
            }
            if (auto* entity = scene.find(avatar.entity_id)) {
                entity->position = avatar.controller.state().position;
                entity->velocity = avatar.controller.state().velocity;
                entity->rotation = {avatar.controller.state().rotation[0],
                                    avatar.controller.state().rotation[1],
                                    avatar.controller.state().rotation[2]};
                entity->avatar_flying = avatar.controller.state().flying;
            }
            const auto& state = avatar.controller.state();
            const auto viewer_position = avatar.controller.viewer_position();
            const auto dx = viewer_position.x - avatar.last_sent_position.x;
            const auto dy = viewer_position.y - avatar.last_sent_position.y;
            const auto dz = viewer_position.z - avatar.last_sent_position.z;
            const auto dvx = state.velocity.x - avatar.last_sent_velocity.x;
            const auto dvy = state.velocity.y - avatar.last_sent_velocity.y;
            const auto dvz = state.velocity.z - avatar.last_sent_velocity.z;
            const auto drx = state.rotation[0] - avatar.last_sent_rotation[0];
            const auto dry = state.rotation[1] - avatar.last_sent_rotation[1];
            const auto drz = state.rotation[2] - avatar.last_sent_rotation[2];
            const bool transform_changed = (dx * dx + dy * dy + dz * dz) > 0.001 ||
                                           (dvx * dvx + dvy * dvy + dvz * dvz) > 0.000001 ||
                                           (drx * drx + dry * dry + drz * drz) > 0.000001F;
            if (transform_changed && now >= avatar.next_transform) {
                const auto agent_id = homeworldz::viewer::parse_uuid(avatar.user_id);
                if (agent_id) {
                    const auto region_handle =
                        (static_cast<std::uint64_t>(region_grid_x * 256) << 32) |
                        static_cast<std::uint32_t>(region_grid_y * 256);
                    const std::array<float, 3> position{
                        static_cast<float>(viewer_position.x), static_cast<float>(viewer_position.y),
                        static_cast<float>(viewer_position.z)};
                    const std::array<float, 3> velocity{
                        static_cast<float>(state.velocity.x), static_cast<float>(state.velocity.y),
                        static_cast<float>(state.velocity.z)};
                    const auto update = homeworldz::viewer::encode_avatar_object_update(
                        region_handle, static_cast<std::uint32_t>(avatar.entity_id), *agent_id,
                        position, velocity, state.rotation);
                    for (const auto& recipient_entry : avatars) {
                        const auto& recipient_endpoint = recipient_entry.first;
                        if (const auto outgoing = circuits.send(recipient_endpoint, update, false, now, true))
                            static_cast<void>(send_udp(viewer_server, recipient_endpoint, *outgoing));
                    }
                    // Avatar transforms reach the sessions that hold this
                    // avatar in interest — the sweep below owns enter and
                    // leave, so an unknown subject is silently skipped rather
                    // than freezing on a client that never learned of it.
                    if (session_server) {
                        // Sessions get the capsule center, matching the
                        // spawned reply, the avatar announcement, and the
                        // published support rule (ground + height/2). The
                        // hip offset is a viewer skeleton convention and
                        // stays on the LLUDP path; a session client that
                        // wants it has the value from its spawned reply.
                        // Found by the client core comparing sampled ground
                        // against reported z, 2026-07-29.
                        const auto transform_envelope =
                            homeworldz::session::encode_envelope("transform", {},
                                "{\"id\":\"" + std::to_string(avatar.entity_id) + "\"" +
                                ",\"position\":" + session_vec3(state.position.x,
                                    state.position.y, state.position.z) +
                                ",\"velocity\":" + session_vec3(state.velocity.x,
                                    state.velocity.y, state.velocity.z) +
                                ",\"rotation\":[" + std::to_string(state.rotation[0]) + "," +
                                    std::to_string(state.rotation[1]) + "," +
                                    std::to_string(state.rotation[2]) + "]}");
                        for (const auto& [recipient_key, recipient] : avatars) {
                            if (recipient.transport != AvatarTransport::session) continue;
                            const auto known = session_avatar_interest.find(recipient_key);
                            if (known == session_avatar_interest.end() ||
                                known->second.count(avatar.entity_id) == 0) continue;
                            session_server->send_to(recipient.session_id, transform_envelope, true);
                        }
                    }
                    avatar.last_sent_position = viewer_position;
                    avatar.last_sent_velocity = state.velocity;
                    avatar.last_sent_rotation = state.rotation;
                    avatar.next_transform = now + std::chrono::milliseconds(100);
                }
            }
        }
        for (const auto& [endpoint, session_id] : departed_avatars) {
            clear_viewer_endpoint(endpoint, session_id);
            circuits.remove(endpoint);
            std::cout << "{\"level\":\"info\",\"message\":\"departed avatar retired\",\"sessionId\":"
                      << homeworldz::api::json_string(session_id) << "}" << std::endl;
        }
        for (const auto& participant_key : crossing_session_avatars)
            retire_session_avatar(participant_key);
        // The interest sweep: emit an avatar's arrival into, and departure
        // from, each session's view. Evaluated for every pair because either
        // party moving changes the answer — a stationary observer walking away
        // from a stationary subject must still be told it is gone.
        if (session_server && now >= next_session_interest_sweep) {
            for (const auto& [observer_key, observer] : avatars) {
                if (observer.transport != AvatarTransport::session) continue;
                auto& known = session_avatar_interest[observer_key];
                for (const auto& [subject_key, subject] : avatars) {
                    const auto interested = session_interested(observer, subject);
                    const auto present = known.count(subject.entity_id) != 0;
                    if (interested && !present) {
                        session_server->send_to(observer.session_id,
                                                session_avatar_envelope(subject, subject_key));
                        known.insert(subject.entity_id);
                    } else if (!interested && present) {
                        session_server->send_to(observer.session_id,
                                                session_kill_envelope(subject.entity_id));
                        known.erase(subject.entity_id);
                    }
                }
            }
            next_session_interest_sweep = now + std::chrono::milliseconds(100);
        }
        if (physics_world)
            for (std::size_t step = 0; step < fixed_steps; ++step)
                physics_world->step(simulation.step_seconds());
        if (physics_world && physics_scene && now >= next_dynamic_sync) {
            const auto region_handle =
                (static_cast<std::uint64_t>(region_grid_x * 256) << 32) |
                static_cast<std::uint32_t>(region_grid_y * 256);
            for (const auto& [entity_id, current] : scene.entities()) {
                if (!current.physical || current.phantom || current.object_id.empty()) continue;
                const auto body_id = physics_scene->body_id(entity_id);
                const auto current_state = physics_world->body_state(body_id);
                if (!current_state) continue;
                auto state = *current_state;
                // Phase 1 has no neighboring regions. Keep body origins within
                // this region and cancel only velocity still pointing through a
                // crossed edge. Neighbor discovery will replace this with a
                // crossing handoff when an accepting neighbor exists.
                if (homeworldz::physics::contain_body_without_neighbors(
                        state, static_cast<double>(region_size_x)))
                    physics_world->set_body_state(state);
                auto* entity = scene.find(entity_id);
                if (!entity) continue;
                entity->position = state.position;
                entity->velocity = state.linear_velocity;
                entity->rotation = {state.rotation[0], state.rotation[1], state.rotation[2]};
                std::vector<homeworldz::scene::EntityId> linked_children;
                for (const auto& [candidate_id, candidate] : scene.entities())
                    if (candidate.parent_id == entity_id) linked_children.push_back(candidate_id);
                for (const auto child_id : linked_children)
                    if (auto* child = scene.find(child_id))
                        homeworldz::scene::update_linked_world_transform(*child, *entity);
                for (const auto& [recipient_endpoint, recipient] : avatars) {
                    const auto radius = homeworldz::physics::linkset_bounding_radius(
                        scene, *entity);
                    auto& recipient_cache = sent_dynamic_transforms[recipient_endpoint];
                    if (!homeworldz::physics::within_viewer_interest(
                            recipient.controller.state().position, entity->position,
                            recipient.controller.state().draw_distance, radius)) {
                        // A session keeps what it was told about, so leaving
                        // interest must be said out loud — otherwise the object
                        // sits in its scene at a stale position forever, the
                        // same trap the avatar sweep exists to avoid.
                        if (recipient.transport == AvatarTransport::session &&
                            recipient_cache.count(entity_id) != 0 && session_server)
                            session_server->send_to(recipient.session_id,
                                                    session_kill_envelope(entity_id));
                        recipient_cache.erase(entity_id);
                        continue;
                    }
                    const auto previous = recipient_cache.find(entity_id);
                    const bool heartbeat_due = previous != recipient_cache.end() &&
                        now - previous->second.sent_at >= std::chrono::seconds(1);
                    if (previous != recipient_cache.end() && !heartbeat_due &&
                        !homeworldz::physics::body_transform_changed(
                            previous->second.state, state))
                        continue;
                    if (recipient.transport == AvatarTransport::session) {
                        // First sight of this object on this session is an
                        // introduction, not a transform: a client cannot move
                        // something it was never told about.
                        if (previous == recipient_cache.end()) {
                            session_server->send_to(recipient.session_id,
                                                    session_object_envelope(*entity));
                            recipient_cache.insert_or_assign(
                                entity_id, SentDynamicTransform{state, now});
                            continue;
                        }
                        // The session's transform message: interest-filtered
                        // above exactly as viewers are, object rotation as a
                        // quaternion (4 elements discriminates the form).
                        session_server->send_to(recipient.session_id,
                            homeworldz::session::encode_envelope("transform", {},
                                "{\"id\":\"" + std::to_string(entity_id) + "\"" +
                                ",\"position\":" + session_vec3(state.position.x,
                                    state.position.y, state.position.z) +
                                ",\"velocity\":" + session_vec3(state.linear_velocity.x,
                                    state.linear_velocity.y, state.linear_velocity.z) +
                                ",\"rotation\":[" + std::to_string(state.rotation[0]) + "," +
                                    std::to_string(state.rotation[1]) + "," +
                                    std::to_string(state.rotation[2]) + "," +
                                    std::to_string(state.rotation[3]) + "]}"), true);
                        recipient_cache.insert_or_assign(
                            entity_id, SentDynamicTransform{state, now});
                        continue;
                    }
                    const auto object = static_object_from_entity(scene, *entity, recipient.user_id, falcon);
                    if (!object) continue;
                    if (const auto sent = circuits.send(recipient_endpoint,
                            homeworldz::viewer::encode_static_object_update(
                                region_handle, *object), false, now, true)) {
                        if (send_udp(viewer_server, recipient_endpoint, *sent))
                            recipient_cache.insert_or_assign(
                                entity_id, SentDynamicTransform{state, now});
                    }
                }
            }
            for (auto& [recipient_endpoint, cache] : sent_dynamic_transforms) {
                static_cast<void>(recipient_endpoint);
                std::erase_if(cache, [&](const auto& entry) {
                    const auto* entity = scene.find(entry.first);
                    return !entity || !entity->physical || entity->phantom;
                });
            }
            next_dynamic_sync = now + std::chrono::milliseconds(100);
        }
        std::unordered_set<homeworldz::scene::EntityId> expired_temporary_roots;
        std::vector<homeworldz::scene::EntityId> stale_temporary_expirations;
        for (const auto& [entity_id, expires_at] : temporary_expirations) {
            const auto* entity = scene.find(entity_id);
            if (!entity || !entity->temporary) {
                stale_temporary_expirations.push_back(entity_id);
                continue;
            }
            if (now < expires_at) continue;
            expired_temporary_roots.insert(
                entity->parent_id == 0 ? entity_id : entity->parent_id);
        }
        for (const auto entity_id : stale_temporary_expirations)
            temporary_expirations.erase(entity_id);
        for (const auto root_id : expired_temporary_roots) {
            std::vector<homeworldz::scene::EntityId> part_ids{root_id};
            for (const auto& [candidate_id, candidate] : scene.entities())
                if (candidate.parent_id == root_id) part_ids.push_back(candidate_id);
            std::vector<std::uint32_t> local_ids;
            local_ids.reserve(part_ids.size());
            for (const auto part_id : part_ids) {
                local_ids.push_back(static_cast<std::uint32_t>(part_id));
                if (physics_scene) static_cast<void>(physics_scene->remove(part_id));
                static_cast<void>(scene.remove(part_id));
                temporary_expirations.erase(part_id);
            }
            const auto payload = homeworldz::viewer::encode_kill_object(local_ids);
            deliver_to_embodied(session_kill_many(local_ids));
            for (const auto& [recipient_endpoint, recipient] : avatars) {
                static_cast<void>(recipient);
                if (const auto outgoing = circuits.send(
                        recipient_endpoint, payload, true, now, true))
                    static_cast<void>(send_udp(viewer_server, recipient_endpoint, *outgoing));
            }
            std::cout << "{\"level\":\"info\",\"message\":\"temporary object expired\",\"rootEntityId\":"
                      << root_id << ",\"parts\":" << part_ids.size() << "}" << std::endl;
        }
        previous_tick = now;
        if (now >= next_snapshot) {
            try {
                storage->save_snapshot(scene);
                next_snapshot = now + std::chrono::seconds(30);
            } catch (const std::exception& error) {
                std::cerr << "{\"level\":\"error\",\"message\":\"save scene snapshot failed\",\"error\":"
                          << homeworldz::api::json_string(error.what()) << "}" << std::endl;
                running = false;
            }
        }
        if (parcels && now >= next_parcel_sweep) {
            next_parcel_sweep = now + std::chrono::seconds(30);
            // Auto-return: objects not owned by the parcel owner (or region owner)
            // on a parcel with OtherCleanTime set are returned once they have sat
            // there longer than that many minutes.
            std::vector<homeworldz::scene::EntityId> to_return;
            std::unordered_set<homeworldz::scene::EntityId> still_eligible;
            for (const auto& [root_id, entity] : scene.entities()) {
                if (entity.object_id.empty() || entity.temporary || entity.parent_id != 0) continue;
                const auto* parcel = parcels->parcel_at(
                    static_cast<float>(entity.position.x), static_cast<float>(entity.position.y));
                if (parcel == nullptr || parcel->other_clean_time <= 0) continue;
                if (entity.owner_id == parcel->owner_id) continue;
                if (!parcel->group_id.empty() && entity.owner_id == parcel->group_id) continue;
                if (!region_owner_id.empty() && entity.owner_id == region_owner_id) continue;
                still_eligible.insert(root_id);
                const auto seen = object_clean_since.find(root_id);
                if (seen == object_clean_since.end()) {
                    object_clean_since[root_id] = now;
                } else if (now - seen->second >= std::chrono::minutes(parcel->other_clean_time)) {
                    to_return.push_back(root_id);
                }
            }
            std::erase_if(object_clean_since, [&](const auto& entry) {
                return still_eligible.count(entry.first) == 0;
            });
            std::vector<std::uint32_t> auto_removed;
            for (const auto root_id : to_return) {
                return_object_to_owner(root_id, auto_removed, now);
                object_clean_since.erase(root_id);
            }
            if (!auto_removed.empty()) {
                try {
                    storage->save_snapshot(scene);
                } catch (const std::exception& error) {
                    std::cout << "{\"level\":\"error\",\"message\":\"auto-return persistence failed\","
                                 "\"error\":" << homeworldz::api::json_string(error.what()) << "}"
                              << std::endl;
                }
                for (const auto entity_id : auto_removed) remove_physics_object(entity_id);
                const auto kill = homeworldz::viewer::encode_kill_object(auto_removed);
                deliver_to_embodied(session_kill_many(auto_removed));
                for (const auto& [recipient_endpoint, recipient] : avatars) {
                    static_cast<void>(recipient);
                    if (const auto outgoing = circuits.send(recipient_endpoint, kill, true, now))
                        static_cast<void>(send_udp(viewer_server, recipient_endpoint, *outgoing));
                }
                std::cout << "{\"level\":\"info\",\"message\":\"parcel objects auto-returned\","
                             "\"count\":" << auto_removed.size() << "}" << std::endl;
            }
            // Ban/access ejection: relocate an avatar standing on a parcel that bans
            // or access-restricts it to the nearest parcel that admits it. Avoids a
            // teleport loop because the destination parcel admits the avatar.
            for (auto& [viewer_endpoint, avatar] : avatars) {
                // Refresh the viewer's agent parcel if it has crossed a boundary.
                push_agent_parcel(avatar);
                const auto& position = avatar.controller.state().position;
                const auto* parcel = parcels->parcel_at(
                    static_cast<float>(position.x), static_cast<float>(position.y));
                if (parcel == nullptr || is_estate_manager(avatar.user_id) ||
                    homeworldz::parcel::can_enter(*parcel, avatar.user_id, region_owner_id))
                    continue;
                std::optional<homeworldz::scene::Vector3> target;
                double best = (std::numeric_limits<double>::max)();
                for (const auto& candidate : parcels->parcels()) {
                    if (!homeworldz::parcel::can_enter(candidate, avatar.user_id, region_owner_id))
                        continue;
                    homeworldz::scene::Vector3 point;
                    if (candidate.user_location.x != 0.0F || candidate.user_location.y != 0.0F ||
                        candidate.user_location.z != 0.0F) {
                        point = {candidate.user_location.x, candidate.user_location.y,
                                 candidate.user_location.z};
                    } else {
                        int min_x = 0, min_y = 0, max_x = 0, max_y = 0;
                        if (!candidate.cell_bounds(parcels->edge_cells(), min_x, min_y, max_x, max_y))
                            continue;
                        point = {static_cast<double>((min_x + max_x) * 2),
                                 static_cast<double>((min_y + max_y) * 2), 0.0};
                        point.z = collision_ground_height(point) + 1.0;
                    }
                    const auto dx = point.x - position.x, dy = point.y - position.y;
                    const auto distance = dx * dx + dy * dy;
                    if (distance < best) {
                        best = distance;
                        target = point;
                    }
                }
                if (!target) continue; // nowhere in-region admits this avatar
                const auto flying = avatar.controller.state().flying;
                avatar.controller.set_ground_height(collision_ground_height(*target));
                avatar.controller.teleport(*target, flying);
                if (physics_world && avatar.physics_character != 0) {
                    if (auto state = physics_world->character_state(avatar.physics_character)) {
                        state->position = avatar.controller.state().position;
                        state->linear_velocity = {};
                        state->grounded = avatar.controller.state().grounded;
                        physics_world->set_character_state(avatar.physics_character, *state);
                        physics_world->set_character_flying(avatar.physics_character, flying);
                    }
                }
                const auto view_position = avatar.controller.viewer_position();
                const auto look_direction = avatar.controller.look_direction();
                const auto flags = homeworldz::viewer::teleport_flags_via_location |
                    (flying ? homeworldz::viewer::teleport_flags_is_flying : 0U);
                const auto agent_uuid =
                    homeworldz::viewer::parse_uuid(avatar.user_id).value_or(homeworldz::viewer::Uuid{});
                if (const auto local = circuits.send(viewer_endpoint,
                        homeworldz::viewer::encode_teleport_local({agent_uuid, 2,
                            {static_cast<float>(view_position.x), static_cast<float>(view_position.y),
                             static_cast<float>(view_position.z)}, look_direction, flags}),
                        true, now, true))
                    static_cast<void>(send_udp(viewer_server, viewer_endpoint, *local));
                std::cout << "{\"level\":\"info\",\"message\":\"avatar ejected from parcel\",\"agent\":"
                          << homeworldz::api::json_string(avatar.user_id) << "}" << std::endl;
            }
        }
        if (registration && !registration->tick(now)) {
            std::cerr << "{\"level\":\"error\",\"message\":\"region lease renewal failed\"";
            if (!registration->last_error().empty())
                std::cerr << ",\"reason\":"
                          << homeworldz::api::json_string(registration->last_error());
            std::cerr << '}' << std::endl;
            running = false;
        }
        if (viewer_grid && now >= next_neighbor_refresh)
            static_cast<void>(refresh_region_neighbors(false));
    }
    // Graceful shutdown (SIGINT/SIGTERM set running=false): tell every connected
    // viewer why it is being disconnected — a KickUser with a reason string, so
    // it shows a clear message instead of a generic timeout/crash — then give the
    // datagrams a few seconds to leave and be processed before the process exits.
    if (!avatars.empty()) {
        const auto shutdown_now = std::chrono::steady_clock::now();
        const std::string reason = "This region is restarting. Please log back in shortly.";
        std::size_t kicked = 0;
        for (const auto& [endpoint, avatar] : avatars) {
            static_cast<void>(avatar);
            const auto* id = circuits.identity(endpoint);
            if (id == nullptr) continue;
            const auto kick = homeworldz::viewer::encode_kick_user(
                id->agent_id, id->session_id, reason);
            if (kick.empty()) continue;
            if (const auto framed = circuits.send(endpoint, kick, true, shutdown_now, true)) {
                static_cast<void>(send_udp(viewer_server, endpoint, *framed));
                ++kicked;
            }
        }
        std::cout << "{\"level\":\"info\",\"message\":\"region shutdown: viewers kicked\",\"count\":"
                  << kicked << "}" << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::cout.flush();
        std::cerr.flush();
    }
    try {
        storage->save_snapshot(scene);
    } catch (const std::exception& error) {
        std::cerr << "{\"level\":\"error\",\"message\":\"final scene snapshot failed\",\"error\":"
                  << homeworldz::api::json_string(error.what()) << "}" << std::endl;
    }
    if (registration) registration->stop();
    for (const auto& pending : pending_event_responses) close_socket(pending.client);
    close_socket(viewer_server);
    close_socket(server);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
