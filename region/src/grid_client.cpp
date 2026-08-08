#include "homeworldz/grid_client.h"

#include "homeworldz/api_models.h"

#include <atomic>
#include <charconv>
#include <cstdlib>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using grid_socket = SOCKET;
constexpr grid_socket invalid_grid_socket = INVALID_SOCKET;
static void close_grid_socket(grid_socket socket) { closesocket(socket); }
static void set_grid_socket_deadline(grid_socket socket, int milliseconds) {
    const DWORD timeout = static_cast<DWORD>(milliseconds);
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
}
#else
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
using grid_socket = int;
constexpr grid_socket invalid_grid_socket = -1;
static void close_grid_socket(grid_socket socket) { close(socket); }
static void set_grid_socket_deadline(grid_socket socket, int milliseconds) {
    timeval timeout{milliseconds / 1000, (milliseconds % 1000) * 1000};
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}
#endif

namespace homeworldz::grid {
namespace {

std::string path_segment(std::string_view value) {
    constexpr char hexadecimal[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' || byte == '~') {
            result.push_back(static_cast<char>(byte));
        } else {
            result.push_back('%');
            result.push_back(hexadecimal[byte >> 4]);
            result.push_back(hexadecimal[byte & 0x0f]);
        }
    }
    return result;
}

class SocketTransport final : public Transport {
public:
    SocketTransport(std::string grid_url, std::string service_token)
        : service_token_(std::move(service_token)) {
        constexpr std::string_view prefix = "http://";
        if (!grid_url.starts_with(prefix)) throw std::invalid_argument("grid URL must use http:// in development");
        auto authority = std::string_view(grid_url).substr(prefix.size());
        const auto slash = authority.find('/');
        if (slash != std::string_view::npos) {
            base_path_ = authority.substr(slash);
            authority = authority.substr(0, slash);
        }
        const auto colon = authority.rfind(':');
        host_ = authority.substr(0, colon);
        port_ = colon == std::string_view::npos ? "80" : std::string(authority.substr(colon + 1));
        if (host_.empty()) throw std::invalid_argument("grid URL host is required");
    }

    // Bounds one blocking send or recv against the grid, not a whole transfer,
    // so a large asset body keeps its time while a silent peer is given up on.
    // Generous because the grid is normally a millisecond away on loopback:
    // reaching this means something is wrong, not merely busy.
    static constexpr int grid_request_timeout_ms = 20000;

    HttpResponse send(std::string_view method, std::string_view path,
                      std::string_view body) override {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* addresses = nullptr;
        if (getaddrinfo(host_.c_str(), port_.c_str(), &hints, &addresses) != 0) {
            throw std::runtime_error("resolve grid host failed");
        }
        grid_socket connection = invalid_grid_socket;
        for (auto* address = addresses; address != nullptr; address = address->ai_next) {
            connection = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (connection != invalid_grid_socket &&
                connect(connection, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) break;
            if (connection != invalid_grid_socket) close_grid_socket(connection);
            connection = invalid_grid_socket;
        }
        freeaddrinfo(addresses);
        if (connection == invalid_grid_socket) throw std::runtime_error("connect to grid failed");
        // Every call here runs on the region's one loop — the same loop that
        // renews the lease, services viewer UDP and serves HTTP. Without a
        // deadline a grid response that never arrives stops the region
        // permanently: the lease lapses, chat stops echoing, and viewers are
        // dropped. Observed 2026-08-07 with the main thread parked in this recv
        // on an outbound socket, which read as an inbound HTTP stall because
        // both use a 4096-byte buffer and glibc implements recv as recvfrom.
        //
        // A timeout turns "the region dies" into "this one grid call fails",
        // which the callers already handle: send throws and each site treats a
        // failed grid call as a refusal rather than a crash.
        set_grid_socket_deadline(connection, grid_request_timeout_ms);

        static std::atomic<unsigned long long> request_sequence{0};
        const auto request_id = "region-" + std::to_string(++request_sequence);
        const auto target = base_path_ + std::string(path);
        auto request = std::string(method) + " " + target + " HTTP/1.1\r\nHost: " + host_ +
                       "\r\nAuthorization: Bearer " + service_token_ +
                       "\r\nX-Request-ID: " + request_id +
                       "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                       std::to_string(body.size()) + "\r\n\r\n" + std::string(body);
        std::size_t sent = 0;
        while (sent < request.size()) {
            const auto count = ::send(connection, request.data() + sent,
                                      static_cast<int>(request.size() - sent), 0);
            if (count <= 0) {
                close_grid_socket(connection);
                throw std::runtime_error("send grid request failed");
            }
            sent += static_cast<std::size_t>(count);
        }
        std::string response;
        char buffer[4096];
        for (;;) {
            const auto count = recv(connection, buffer, sizeof(buffer), 0);
            if (count <= 0) break;
            response.append(buffer, static_cast<std::size_t>(count));
        }
        close_grid_socket(connection);
        const auto first_space = response.find(' ');
        int status = 0;
        if (first_space == std::string::npos ||
            std::from_chars(response.data() + first_space + 1, response.data() + first_space + 4, status).ec != std::errc{}) {
            throw std::runtime_error("invalid grid response");
        }
        const auto body_start = response.find("\r\n\r\n");
        return {status, body_start == std::string::npos ? std::string{} : response.substr(body_start + 4)};
    }

private:
    std::string host_;
    std::string port_;
    std::string base_path_;
    std::string service_token_;
};

std::string json_field(std::string_view body, std::string_view name) {
    const auto marker = "\"" + std::string(name) + "\":\"";
    const auto start = body.find(marker);
    if (start == std::string_view::npos) return {};
    const auto value_start = start + marker.size();
    const auto end = body.find('"', value_start);
    return end == std::string_view::npos ? std::string{} : std::string(body.substr(value_start, end - value_start));
}

std::optional<std::uint32_t> json_u32(std::string_view body, std::string_view name) {
    const auto marker = "\"" + std::string(name) + "\":";
    const auto start = body.find(marker);
    if (start == std::string_view::npos) return std::nullopt;
    const auto value_start = start + marker.size();
    std::uint32_t value{};
    const auto result = std::from_chars(body.data() + value_start, body.data() + body.size(), value);
    if (result.ec != std::errc{} || result.ptr == body.data() + value_start) return std::nullopt;
    return value;
}

std::optional<std::uint64_t> json_u64(std::string_view body, std::string_view name) {
    const auto marker = "\"" + std::string(name) + "\":";
    const auto start = body.find(marker);
    if (start == std::string_view::npos) return std::nullopt;
    const auto value_start = start + marker.size();
    std::uint64_t value{};
    const auto result = std::from_chars(body.data() + value_start, body.data() + body.size(), value);
    if (result.ec != std::errc{} || result.ptr == body.data() + value_start) return std::nullopt;
    return value;
}

std::vector<AssetLocation> asset_locations_from_json(std::string_view body) {
    std::vector<AssetLocation> locations;
    constexpr std::string_view marker = "\"endpoint\":\"";
    std::size_t position = 0;
    while ((position = body.find(marker, position)) != std::string_view::npos) {
        const auto value_start = position + marker.size();
        const auto value_end = body.find('"', value_start);
        if (value_end == std::string_view::npos) break;
        const auto object_end = body.find('}', value_end);
        if (object_end == std::string_view::npos) break;
        const auto origin = body.find("\"origin\":true", value_end);
        locations.push_back(AssetLocation{
            std::string(body.substr(value_start, value_end - value_start)),
            origin != std::string_view::npos && origin < object_end});
        position = object_end + 1;
    }
    return locations;
}

std::optional<int> json_int(std::string_view body, std::string_view name) {
    const auto marker = "\"" + std::string(name) + "\":";
    const auto start = body.find(marker);
    if (start == std::string_view::npos) return std::nullopt;
    const auto value_start = start + marker.size();
    int value{};
    const auto result = std::from_chars(body.data() + value_start, body.data() + body.size(), value);
    if (result.ec != std::errc{} || result.ptr == body.data() + value_start) return std::nullopt;
    return value;
}

std::optional<float> json_float(std::string_view body, std::string_view name) {
    const auto marker = "\"" + std::string(name) + "\":";
    const auto start = body.find(marker);
    if (start == std::string_view::npos) return std::nullopt;
    const auto value_start = start + marker.size();
    float value{};
    const auto result = std::from_chars(body.data() + value_start, body.data() + body.size(), value);
    if (result.ec != std::errc{} || result.ptr == body.data() + value_start) return std::nullopt;
    return value;
}

std::optional<std::array<float, 3>> json_vector(std::string_view body, std::string_view name) {
    const auto marker = "\"" + std::string(name) + "\":{";
    const auto start = body.find(marker);
    if (start == std::string_view::npos) return std::nullopt;
    const auto end = body.find('}', start + marker.size());
    if (end == std::string_view::npos) return std::nullopt;
    const auto object = body.substr(start + marker.size(), end - start - marker.size());
    const auto x = json_float(object, "x");
    const auto y = json_float(object, "y");
    const auto z = json_float(object, "z");
    if (!x || !y || !z) return std::nullopt;
    return std::array<float, 3>{*x, *y, *z};
}

std::optional<bool> json_bool(std::string_view body, std::string_view name) {
    const auto marker = "\"" + std::string(name) + "\":";
    const auto start = body.find(marker);
    if (start == std::string_view::npos) return std::nullopt;
    const auto value = body.substr(start + marker.size());
    if (value.starts_with("true")) return true;
    if (value.starts_with("false")) return false;
    return std::nullopt;
}

std::optional<AvatarTransit> avatar_transit_from_json(std::string_view body) {
    AvatarTransit transit;
    transit.id = json_field(body, "id");
    transit.agent_id = json_field(body, "agentId");
    transit.session_id = json_field(body, "sessionId");
    transit.source_region_id = json_field(body, "sourceRegionId");
    transit.destination_region_id = json_field(body, "destinationRegionId");
    transit.state = json_field(body, "state");
    const auto generation = json_u64(body, "generation");
    const auto position = json_vector(body, "position");
    const auto look_at = json_vector(body, "lookAt");
    const auto flying = json_bool(body, "flying");
    if (transit.id.empty() || transit.agent_id.empty() || transit.session_id.empty() ||
        transit.source_region_id.empty() || transit.destination_region_id.empty() ||
        transit.state.empty() || !generation || *generation == 0 || !position || !look_at || !flying)
        return std::nullopt;
    transit.generation = *generation;
    transit.position = *position;
    transit.look_at = *look_at;
    transit.flying = *flying;
    return transit;
}

std::optional<InventoryItem> inventory_item_from_json(std::string_view body,
                                                       std::string_view user_id) {
    InventoryItem item;
    item.item_id = json_field(body, "id");
    item.creator_id = json_field(body, "creatorUserId");
    item.owner_id = json_field(body, "ownerUserId");
    item.folder_id = json_field(body, "folderId");
    item.asset_id = json_field(body, "assetId");
    item.name = json_field(body, "name");
    item.description = json_field(body, "description");
    const auto asset_type = json_int(body, "assetType");
    const auto inventory_type = json_int(body, "inventoryType");
    const auto flags = json_u32(body, "flags");
    const auto base_permissions = json_u32(body, "basePermissions");
    const auto current_permissions = json_u32(body, "currentPermissions");
    const auto everyone_permissions = json_u32(body, "everyonePermissions");
    const auto next_permissions = json_u32(body, "nextPermissions");
    const auto sale_type = json_int(body, "saleType");
    const auto sale_price = json_int(body, "salePrice");
    if (item.item_id.empty() || item.creator_id.empty() || item.owner_id != user_id ||
        item.folder_id.empty() || item.asset_id.empty() || item.name.empty() || !asset_type ||
        !inventory_type || !flags || !base_permissions || !current_permissions ||
        !everyone_permissions || !next_permissions || !sale_type || !sale_price)
        return std::nullopt;
    item.asset_type = *asset_type;
    item.inventory_type = *inventory_type;
    item.flags = *flags;
    item.base_permissions = *base_permissions;
    item.current_permissions = *current_permissions;
    item.everyone_permissions = *everyone_permissions;
    item.next_permissions = *next_permissions;
    item.sale_type = *sale_type;
    item.sale_price = *sale_price;
    return item;
}

std::optional<std::string_view> json_object(std::string_view body, std::string_view key) {
    const auto marker = "\"" + std::string(key) + "\":{";
    const auto found = body.find(marker);
    if (found == std::string_view::npos) return std::nullopt;
    const auto start = found + marker.size() - 1;
    std::size_t depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (auto position = start; position < body.size(); ++position) {
        const auto character = body[position];
        if (quoted) {
            if (escaped) escaped = false;
            else if (character == '\\') escaped = true;
            else if (character == '"') quoted = false;
        } else if (character == '"') quoted = true;
        else if (character == '{') ++depth;
        else if (character == '}' && --depth == 0)
            return body.substr(start, position - start + 1);
    }
    return std::nullopt;
}

std::vector<std::string> json_string_array(std::string_view body, std::string_view key) {
    std::vector<std::string> values;
    const auto marker = "\"" + std::string(key) + "\":[";
    const auto found = body.find(marker);
    if (found == std::string_view::npos) return values;
    auto position = found + marker.size();
    while (position < body.size() && body[position] != ']') {
        if (body[position] == '"') {
            const auto end = body.find('"', position + 1);
            if (end == std::string_view::npos) break;
            values.emplace_back(body.substr(position + 1, end - position - 1));
            position = end + 1;
        } else {
            ++position;
        }
    }
    return values;
}

Estate estate_from_json(std::string_view body) {
    Estate estate;
    estate.id = json_int(body, "id").value_or(0);
    estate.name = json_field(body, "name");
    estate.owner_id = json_field(body, "ownerUserId");
    estate.parent_estate_id = json_int(body, "parentEstateId").value_or(1);
    estate.flags = json_u64(body, "flags").value_or(0);
    estate.public_access = json_bool(body, "publicAccess").value_or(true);
    estate.sun_hour = json_float(body, "sunHour").value_or(0.0F);
    estate.use_global_time = json_bool(body, "useGlobalTime").value_or(true);
    estate.fixed_sun = json_bool(body, "fixedSun").value_or(false);
    estate.billable_factor = json_float(body, "billableFactor").value_or(0.0F);
    estate.price_per_meter = json_int(body, "pricePerMeter").value_or(0);
    estate.redirect_grid_x = json_int(body, "redirectGridX").value_or(0);
    estate.redirect_grid_y = json_int(body, "redirectGridY").value_or(0);
    estate.abuse_email = json_field(body, "abuseEmail");
    estate.managers = json_string_array(body, "managers");
    estate.allowed_users = json_string_array(body, "allowedUsers");
    estate.allowed_groups = json_string_array(body, "allowedGroups");
    estate.bans = json_string_array(body, "bans");
    return estate;
}

std::optional<TaskInventoryTransfer> task_transfer_from_json(std::string_view body) {
    TaskInventoryTransfer value;
    value.id = json_field(body, "id");
    value.user_id = json_field(body, "userId");
    value.source_item_id = json_field(body, "sourceItemId");
    value.region_id = json_field(body, "regionId");
    value.object_id = json_field(body, "objectId");
    value.task_item_id = json_field(body, "taskItemId");
    value.state = json_field(body, "state");
    const auto item = json_object(body, "item");
    if (!item) return std::nullopt;
    const auto parsed_item = inventory_item_from_json(*item, value.user_id);
    if (value.id.empty() || value.user_id.empty() || value.source_item_id.empty() ||
        value.region_id.empty() || value.object_id.empty() || value.task_item_id.empty() ||
        value.state.empty() || !parsed_item)
        return std::nullopt;
    value.item = *parsed_item;
    return value;
}

std::optional<std::vector<TaskInventoryTransfer>> task_transfer_list_from_json(
    std::string_view body) {
    std::vector<TaskInventoryTransfer> result;
    std::size_t position = body.find('[');
    if (position == std::string_view::npos) return std::nullopt;
    ++position;
    while (position < body.size()) {
        while (position < body.size() && (body[position] == ' ' || body[position] == ',')) ++position;
        if (position < body.size() && body[position] == ']') return result;
        if (position >= body.size() || body[position] != '{') return std::nullopt;
        const auto start = position;
        std::size_t depth = 0;
        bool quoted = false;
        bool escaped = false;
        for (; position < body.size(); ++position) {
            const auto character = body[position];
            if (quoted) {
                if (escaped) escaped = false;
                else if (character == '\\') escaped = true;
                else if (character == '"') quoted = false;
                continue;
            }
            if (character == '"') quoted = true;
            else if (character == '{') ++depth;
            else if (character == '}' && --depth == 0) { ++position; break; }
        }
        if (depth != 0) return std::nullopt;
        const auto parsed = task_transfer_from_json(body.substr(start, position - start));
        if (!parsed) return std::nullopt;
        result.push_back(*parsed);
    }
    return std::nullopt;
}

std::optional<TaskInventoryExtraction> task_extraction_from_json(std::string_view body) {
    TaskInventoryExtraction value;
    value.id = json_field(body, "id");
    value.user_id = json_field(body, "userId");
    value.region_id = json_field(body, "regionId");
    value.object_id = json_field(body, "objectId");
    value.source_task_item_id = json_field(body, "sourceTaskItemId");
    value.destination_folder_id = json_field(body, "destinationFolderId");
    value.personal_item_id = json_field(body, "personalItemId");
    value.state = json_field(body, "state");
    const auto item = json_object(body, "item");
    if (!item) return std::nullopt;
    const auto parsed_item = inventory_item_from_json(*item, value.user_id);
    if (value.id.empty() || value.user_id.empty() || value.region_id.empty() ||
        value.object_id.empty() || value.source_task_item_id.empty() ||
        value.destination_folder_id.empty() || value.personal_item_id.empty() ||
        value.state.empty() || !parsed_item)
        return std::nullopt;
    value.item = *parsed_item;
    return value;
}

std::optional<std::vector<TaskInventoryExtraction>> task_extraction_list_from_json(
    std::string_view body) {
    std::vector<TaskInventoryExtraction> result;
    std::size_t position = body.find('[');
    if (position == std::string_view::npos) return std::nullopt;
    ++position;
    while (position < body.size()) {
        while (position < body.size() && (body[position] == ' ' || body[position] == ',')) ++position;
        if (position < body.size() && body[position] == ']') return result;
        if (position >= body.size() || body[position] != '{') return std::nullopt;
        const auto start = position;
        std::size_t depth = 0;
        bool quoted = false;
        bool escaped = false;
        for (; position < body.size(); ++position) {
            const auto character = body[position];
            if (quoted) {
                if (escaped) escaped = false;
                else if (character == '\\') escaped = true;
                else if (character == '"') quoted = false;
                continue;
            }
            if (character == '"') quoted = true;
            else if (character == '{') ++depth;
            else if (character == '}' && --depth == 0) { ++position; break; }
        }
        if (depth != 0) return std::nullopt;
        const auto parsed = task_extraction_from_json(body.substr(start, position - start));
        if (!parsed) return std::nullopt;
        result.push_back(*parsed);
    }
    return std::nullopt;
}

std::optional<ObjectRez> object_rez_from_json(std::string_view body) {
    ObjectRez value;
    value.id = json_field(body, "id");
    value.user_id = json_field(body, "userId");
    value.source_item_id = json_field(body, "sourceItemId");
    value.region_id = json_field(body, "regionId");
    value.object_id = json_field(body, "objectId");
    value.state = json_field(body, "state");
    const auto item = json_object(body, "item");
    if (!item) return std::nullopt;
    const auto parsed_item = inventory_item_from_json(*item, value.user_id);
    if (value.id.empty() || value.user_id.empty() || value.source_item_id.empty() ||
        value.region_id.empty() || value.object_id.empty() || value.state.empty() || !parsed_item)
        return std::nullopt;
    value.item = *parsed_item;
    return value;
}

std::optional<std::vector<ObjectRez>> object_rez_list_from_json(std::string_view body) {
    std::vector<ObjectRez> result;
    std::size_t position = body.find('[');
    if (position == std::string_view::npos) return std::nullopt;
    ++position;
    while (position < body.size()) {
        while (position < body.size() && (body[position] == ' ' || body[position] == ',')) ++position;
        if (position < body.size() && body[position] == ']') return result;
        if (position >= body.size() || body[position] != '{') return std::nullopt;
        const auto start = position;
        std::size_t depth = 0;
        bool quoted = false;
        bool escaped = false;
        for (; position < body.size(); ++position) {
            const auto character = body[position];
            if (quoted) {
                if (escaped) escaped = false;
                else if (character == '\\') escaped = true;
                else if (character == '"') quoted = false;
                continue;
            }
            if (character == '"') quoted = true;
            else if (character == '{') ++depth;
            else if (character == '}' && --depth == 0) { ++position; break; }
        }
        if (depth != 0) return std::nullopt;
        const auto parsed = object_rez_from_json(body.substr(start, position - start));
        if (!parsed) return std::nullopt;
        result.push_back(*parsed);
    }
    return std::nullopt;
}

} // namespace

std::shared_ptr<Transport> socket_transport(std::string grid_url, std::string service_token) {
    return std::make_shared<SocketTransport>(std::move(grid_url), std::move(service_token));
}

std::string Client::register_region(const RegionSettings& settings) {
    const auto body = "{\"name\":" + api::json_string(settings.name) +
                      ",\"gridX\":" + std::to_string(settings.grid_x) +
                      ",\"gridY\":" + std::to_string(settings.grid_y) +
                      ",\"publicEndpoint\":" + api::json_string(settings.public_endpoint) +
                      ",\"viewerPort\":" + std::to_string(settings.viewer_port) +
                      ",\"leaseSeconds\":" + std::to_string(settings.lease_seconds) + '}';
    const auto response = transport_->send("POST", "/api/v1/regions", body);
    if (response.status_code != 201) return {};
    return json_field(response.body, "id");
}

std::optional<RegisteredRegion> Client::register_provisioned_region(
    std::string_view region_id, const RegionSettings& settings, std::string* refusal) {
    auto body = "{\"publicEndpoint\":" + api::json_string(settings.public_endpoint) +
                ",\"viewerPort\":" + std::to_string(settings.viewer_port) +
                ",\"leaseSeconds\":" + std::to_string(settings.lease_seconds) +
                ",\"regionProtocol\":" + std::to_string(region_protocol);
    if (!settings.session_endpoint.empty())
        body += ",\"sessionEndpoint\":" + api::json_string(settings.session_endpoint);
    body += '}';
    const auto response = transport_->send(
        "POST", "/api/v1/region-runtime/" + path_segment(region_id), body);
    if (response.status_code != 200) {
        if (refusal) *refusal = json_field(response.body, "message");
        return std::nullopt;
    }
    RegisteredRegion region{json_field(response.body, "id"), json_field(response.body, "name")};
    const auto grid_x = json_int(response.body, "gridX");
    const auto grid_y = json_int(response.body, "gridY");
	const auto size_x = json_int(response.body, "sizeX");
	const auto size_y = json_int(response.body, "sizeY");
	const auto maturity = json_int(response.body, "maturity");
    const auto viewer_port = json_int(response.body, "viewerPort");
    const auto grid_protocol = json_int(response.body, "regionProtocol");
    region.public_endpoint = json_field(response.body, "publicEndpoint");
    region.grid_name = json_field(response.body, "gridName");
    region.grid_public_url = json_field(response.body, "gridPublicUrl");
    region.owner_id = json_field(response.body, "ownerUserId");
	if (region.id.empty() || region.name.empty() || !grid_x || !grid_y || !size_x || !size_y ||
		(*size_x != 256 && *size_x != 512 && *size_x != 1024) || *size_x != *size_y ||
		!maturity || *maturity < 0 || *maturity > 2 ||
        region.public_endpoint.empty() || !viewer_port || *viewer_port < 1 || *viewer_port > 65535 ||
        region.grid_name.empty() || region.grid_public_url.empty() ||
        !grid_protocol || *grid_protocol < 1) return std::nullopt;
    region.grid_region_protocol = *grid_protocol;
    region.grid_x = *grid_x;
    region.grid_y = *grid_y;
	region.viewer_port = *viewer_port;
	region.size_x = *size_x;
	region.size_y = *size_y;
	region.maturity = *maturity;
    if (const auto estate = json_object(response.body, "estate"))
        region.estate = estate_from_json(*estate);
    return region;
}

std::optional<AvatarTransit> Client::prepare_avatar_transit(const AvatarTransitRequest& request) {
    const auto vector_json = [](const std::array<float, 3>& value) {
        return std::string{"{\"x\":"} + std::to_string(value[0]) +
               ",\"y\":" + std::to_string(value[1]) +
               ",\"z\":" + std::to_string(value[2]) + '}';
    };
    const auto body = "{\"id\":" + api::json_string(request.id) +
                      ",\"agentId\":" + api::json_string(request.agent_id) +
                      ",\"sessionId\":" + api::json_string(request.session_id) +
                      ",\"sourceRegionId\":" + api::json_string(request.source_region_id) +
                      ",\"destinationRegionId\":" + api::json_string(request.destination_region_id) +
                      ",\"position\":" + vector_json(request.position) +
                      ",\"lookAt\":" + vector_json(request.look_at) +
                      ",\"flying\":" + (request.flying ? "true" : "false") +
                      ",\"lifetimeSeconds\":" + std::to_string(request.lifetime_seconds) + '}';
    const auto response = transport_->send("POST", "/api/v1/transits", body);
    return response.status_code == 200 ? avatar_transit_from_json(response.body) : std::nullopt;
}

std::optional<AvatarTransit> Client::find_avatar_transit(std::string_view transit_id) {
    const auto response = transport_->send("GET", "/api/v1/transits/" + std::string(transit_id), {});
    return response.status_code == 200 ? avatar_transit_from_json(response.body) : std::nullopt;
}

namespace {
std::optional<AvatarTransit> change_avatar_transit(
    const std::shared_ptr<Transport>& transport, std::string_view transit_id,
    std::string_view action, std::string_view region_id, std::string_view reason = {}) {
    auto body = "{\"regionId\":" + api::json_string(region_id);
    if (!reason.empty()) body += ",\"reason\":" + api::json_string(reason);
    body += '}';
    const auto response = transport->send(
        "POST", "/api/v1/transits/" + std::string(transit_id) + '/' + std::string(action), body);
    return response.status_code == 200 ? avatar_transit_from_json(response.body) : std::nullopt;
}
} // namespace

std::optional<AvatarTransit> Client::accept_avatar_transit(
    std::string_view transit_id, std::string_view destination_region_id) {
    return change_avatar_transit(transport_, transit_id, "accept", destination_region_id);
}

std::optional<AvatarTransit> Client::activate_avatar_transit(
    std::string_view transit_id, std::string_view destination_region_id) {
    return change_avatar_transit(transport_, transit_id, "activate", destination_region_id);
}

std::optional<AvatarTransit> Client::rollback_avatar_transit(
    std::string_view transit_id, std::string_view region_id, std::string_view reason) {
    return change_avatar_transit(transport_, transit_id, "rollback", region_id, reason);
}

namespace {
// The region object the grid returns in both the neighbor list and a
// destination lookup. One parser so the two can never read it differently.
bool parse_region_placement(std::string_view object, RegionPlacement& placement) {
    placement.id = json_field(object, "id");
    placement.name = json_field(object, "name");
    placement.public_endpoint = json_field(object, "publicEndpoint");
    placement.session_endpoint = json_field(object, "sessionEndpoint");
    const auto grid_x = json_int(object, "gridX");
    const auto grid_y = json_int(object, "gridY");
    const auto viewer_port = json_int(object, "viewerPort");
    const auto size_x = json_int(object, "sizeX");
    const auto size_y = json_int(object, "sizeY");
    const auto maturity = json_int(object, "maturity");
    const auto online = json_bool(object, "online");
    if (placement.id.empty() || placement.name.empty() || !grid_x || !grid_y ||
        !size_x || !size_y || *size_x < 256 || *size_y < 256 || !maturity || !online ||
        (*online && (placement.public_endpoint.empty() || !viewer_port ||
         *viewer_port < 1 || *viewer_port > 65535)))
        return false;
    placement.grid_x = *grid_x;
    placement.grid_y = *grid_y;
    placement.size_x = *size_x;
    placement.size_y = *size_y;
    placement.maturity = *maturity;
    placement.online = *online;
    placement.viewer_port = viewer_port.value_or(0);
    return true;
}
} // namespace

std::optional<RegionPlacement> Client::find_region_at(int grid_x, int grid_y) {
    if (grid_x < 0 || grid_y < 0) return std::nullopt;
    const auto response = transport_->send(
        "GET", "/api/v1/regions/lookup?gridX=" + std::to_string(grid_x) +
                   "&gridY=" + std::to_string(grid_y), {});
    if (response.status_code != 200) return std::nullopt;
    RegionPlacement placement;
    if (!parse_region_placement(response.body, placement)) return std::nullopt;
    return placement;
}

std::optional<RegionPlacement> Client::find_region(std::string_view region_id) {
    const auto response = transport_->send(
        "GET", "/api/v1/regions/lookup?id=" + path_segment(region_id), {});
    if (response.status_code != 200) return std::nullopt;
    RegionPlacement placement;
    if (!parse_region_placement(response.body, placement)) return std::nullopt;
    return placement;
}

std::optional<std::vector<RegionPlacement>> Client::find_grid_topology() {
    const auto response = transport_->send("GET", "/api/v1/regions/topology", {});
    if (response.status_code != 200) return std::nullopt;
    if (response.body.find("\"regions\":[") == std::string::npos) return std::nullopt;
    std::vector<RegionPlacement> topology;
    // The list is flat objects, so each one runs from its brace to the next
    // close brace; a partial parse is a failed parse, never a short map.
    std::size_t position = response.body.find('[');
    while ((position = response.body.find('{', position)) != std::string::npos) {
        const auto object_end = response.body.find('}', position);
        if (object_end == std::string::npos) return std::nullopt;
        RegionPlacement placement;
        if (!parse_region_placement(
                std::string_view(response.body).substr(position + 1, object_end - position - 1),
                placement))
            return std::nullopt;
        topology.push_back(std::move(placement));
        position = object_end + 1;
    }
    return topology;
}

std::optional<std::vector<RegionNeighbor>> Client::find_region_neighbors(
    std::string_view region_id) {
    const auto response = transport_->send(
        "GET", "/api/v1/regions/" + std::string(region_id) + "/neighbors", {});
    if (response.status_code != 200) return std::nullopt;

    std::vector<RegionNeighbor> neighbors;
    constexpr std::string_view direction_marker = "\"direction\":\"";
    constexpr std::string_view region_marker = "\"region\":{";
    std::size_t position = 0;
    while ((position = response.body.find(direction_marker, position)) != std::string::npos) {
        const auto direction_start = position + direction_marker.size();
        const auto direction_end = response.body.find('"', direction_start);
        const auto region_start = response.body.find(region_marker, direction_end);
        if (direction_end == std::string::npos || region_start == std::string::npos)
            return std::nullopt;
        const auto object_start = region_start + region_marker.size();
        const auto object_end = response.body.find('}', object_start);
        if (object_end == std::string::npos) return std::nullopt;
        const auto object = std::string_view(response.body).substr(
            object_start, object_end - object_start);
        RegionNeighbor neighbor;
        neighbor.direction = response.body.substr(
            direction_start, direction_end - direction_start);
        const auto valid_direction = neighbor.direction == "north" ||
            neighbor.direction == "east" || neighbor.direction == "south" ||
            neighbor.direction == "west";
        if (!valid_direction || !parse_region_placement(object, neighbor))
            return std::nullopt;
        neighbors.push_back(std::move(neighbor));
        position = object_end + 1;
    }
    if (response.body.find("\"neighbors\":[") == std::string::npos)
        return std::nullopt;
    return neighbors;
}

std::optional<Estate> Client::update_estate_settings(std::string_view region_id,
                                                     const EstateSettingsPatch& patch) {
    std::string body = "{";
    bool first = true;
    const auto add = [&](std::string_view field, const std::string& value) {
        if (!first) body += ',';
        first = false;
        body += "\"" + std::string(field) + "\":" + value;
    };
    if (patch.name) add("name", api::json_string(*patch.name));
    if (patch.flags) add("flags", std::to_string(*patch.flags));
    if (patch.public_access) add("publicAccess", *patch.public_access ? "true" : "false");
    if (patch.fixed_sun) add("fixedSun", *patch.fixed_sun ? "true" : "false");
    if (patch.use_global_time) add("useGlobalTime", *patch.use_global_time ? "true" : "false");
    if (patch.sun_hour) add("sunHour", std::to_string(*patch.sun_hour));
    body += "}";
    const auto response = transport_->send(
        "POST", "/api/v1/region-runtime/" + path_segment(region_id) + "/estate", body);
    if (response.status_code != 200) return std::nullopt;
    if (const auto estate = json_object(response.body, "estate")) return estate_from_json(*estate);
    return std::nullopt;
}

std::optional<Estate> Client::set_estate_member(std::string_view region_id,
                                                std::string_view member_id, int role, bool present) {
    const auto body = "{\"memberId\":" + api::json_string(std::string(member_id)) +
                      ",\"role\":" + std::to_string(role) +
                      ",\"present\":" + (present ? "true" : "false") + "}";
    const auto response = transport_->send(
        "POST", "/api/v1/region-runtime/" + path_segment(region_id) + "/estate/members", body);
    if (response.status_code != 200) return std::nullopt;
    if (const auto estate = json_object(response.body, "estate")) return estate_from_json(*estate);
    return std::nullopt;
}

bool Client::renew_lease(std::string_view region_id, int lease_seconds) {
    const auto body = "{\"leaseSeconds\":" + std::to_string(lease_seconds) + '}';
    return transport_->send("PUT", "/api/v1/regions/" + std::string(region_id) + "/lease", body).status_code == 200;
}

bool Client::deregister(std::string_view region_id) {
    return transport_->send("DELETE", "/api/v1/regions/" + std::string(region_id), {}).status_code == 204;
}

bool Client::renew_provisioned_lease(std::string_view region_id, int lease_seconds,
                                     std::string* refusal) {
    const auto body = "{\"leaseSeconds\":" + std::to_string(lease_seconds) +
                      ",\"regionProtocol\":" + std::to_string(region_protocol) + '}';
    const auto response = transport_->send(
        "PUT", "/api/v1/region-runtime/" + std::string(region_id) + "/lease", body);
    if (response.status_code == 200) return true;
    if (refusal) *refusal = json_field(response.body, "message");
    return false;
}

bool Client::deregister_provisioned(std::string_view region_id) {
    return transport_->send("DELETE", "/api/v1/region-runtime/" + std::string(region_id), {})
               .status_code == 204;
}

std::optional<ViewerSession> Client::validate_viewer_session(std::string_view session_id) {
    const auto response = transport_->send("GET", "/api/v1/sessions/" + std::string(session_id), {});
    if (response.status_code != 200) return std::nullopt;
    ViewerSession session;
    session.session_id = json_field(response.body, "id");
    session.secure_session_id = json_field(response.body, "secureSessionId");
    session.agent_id = json_field(response.body, "userId");
    session.destination_region_id = json_field(response.body, "destinationRegionId");
    const auto circuit = json_u32(response.body, "viewerCircuitCode");
    if (session.session_id != session_id || session.secure_session_id.empty() || session.agent_id.empty() ||
        session.destination_region_id.empty() ||
        !circuit || *circuit == 0)
        return std::nullopt;
    session.circuit_code = *circuit;
    return session;
}

std::optional<User> Client::find_user(std::string_view user_id) {
    const auto response = transport_->send("GET", "/api/v1/users/" + std::string(user_id), {});
    if (response.status_code != 200) return std::nullopt;
    User user{json_field(response.body, "id"), json_field(response.body, "username")};
    if (user.id != user_id || user.username.empty()) return std::nullopt;
    return user;
}

bool Client::revoke_viewer_session(std::string_view session_id) {
    const auto status = transport_->send("DELETE", "/api/v1/sessions/" + std::string(session_id), {}).status_code;
    return status == 204 || status == 404;
}

bool Client::create_inventory_folder(std::string_view user_id, std::string_view folder_id,
                                     std::string_view parent_id, std::string_view name,
                                     int type_default) {
    const auto body = "{\"id\":" + api::json_string(folder_id) +
                      ",\"parentId\":" + api::json_string(parent_id) +
                      ",\"name\":" + api::json_string(name) +
                      ",\"typeDefault\":" + std::to_string(type_default) + '}';
    return transport_->send("POST", "/api/v1/inventory/" + std::string(user_id) + "/folders", body)
               .status_code == 201;
}

bool Client::move_inventory_folder(std::string_view user_id, std::string_view folder_id,
                                   std::string_view parent_id) {
    const auto body = "{\"parentId\":" + api::json_string(parent_id) + '}';
    return transport_->send("PUT", "/api/v1/inventory/" + std::string(user_id) +
        "/folders/" + std::string(folder_id), body).status_code == 200;
}

bool Client::move_inventory_item(std::string_view user_id, std::string_view item_id,
                                 std::string_view folder_id, std::string_view new_name) {
    const auto body = "{\"folderId\":" + api::json_string(folder_id) +
                      ",\"name\":" + api::json_string(new_name) + '}';
    return transport_->send("PUT", "/api/v1/inventory/" + std::string(user_id) +
        "/items/" + std::string(item_id), body).status_code == 200;
}

bool Client::update_inventory_item_asset(std::string_view user_id, std::string_view item_id,
                                         std::string_view asset_id) {
    const auto body = "{\"assetId\":" + api::json_string(asset_id) + '}';
    return transport_->send("PUT", "/api/v1/inventory/" + std::string(user_id) +
        "/items/" + std::string(item_id) + "/asset", body).status_code == 200;
}

std::optional<InventoryItem> Client::find_inventory_item(std::string_view user_id,
                                                          std::string_view item_id) {
    const auto response = transport_->send(
        "GET", "/api/v1/inventory/" + std::string(user_id) +
                   "/items/" + std::string(item_id), {});
    if (response.status_code != 200) return std::nullopt;
    return inventory_item_from_json(response.body, user_id);
}

std::optional<std::string> Client::find_system_inventory_folder(std::string_view user_id,
                                                                 int folder_type) {
    const auto response = transport_->send(
        "GET", "/api/v1/inventory/" + std::string(user_id) +
                   "/system-folders/" + std::to_string(folder_type), {});
    if (response.status_code != 200) return std::nullopt;
    const auto folder_id = json_field(response.body, "id");
    if (folder_id.empty()) return std::nullopt;
    return folder_id;
}

bool Client::create_texture_inventory_item(std::string_view user_id, const TextureInventoryItem& item) {
    const auto body = "{\"id\":" + api::json_string(item.item_id) +
                      ",\"creatorUserId\":" + api::json_string(item.creator_id) +
                      ",\"folderId\":" + api::json_string(item.folder_id) +
                      ",\"assetId\":" + api::json_string(item.asset_id) +
                      ",\"assetType\":0,\"inventoryType\":0,\"name\":" + api::json_string(item.name) +
                      ",\"description\":" + api::json_string(item.description) +
                      ",\"basePermissions\":" + std::to_string(0x0009e000) +
                      ",\"currentPermissions\":" + std::to_string(0x0009e000) +
                      ",\"everyonePermissions\":" + std::to_string(item.everyone_permissions) +
                      ",\"nextPermissions\":" + std::to_string(item.next_permissions) + '}';
    return transport_->send("POST", "/api/v1/inventory/" + std::string(user_id) + "/items", body)
               .status_code == 201;
}

bool Client::create_object_inventory_item(std::string_view user_id, const ObjectInventoryItem& item) {
    const auto body = "{\"id\":" + api::json_string(item.item_id) +
                      ",\"creatorUserId\":" + api::json_string(item.creator_id) +
                      ",\"folderId\":" + api::json_string(item.folder_id) +
                      ",\"assetId\":" + api::json_string(item.asset_id) +
                      ",\"assetType\":6,\"inventoryType\":6,\"name\":" + api::json_string(item.name) +
                      ",\"description\":" + api::json_string(item.description) +
                      ",\"basePermissions\":" + std::to_string(item.base_permissions) +
                      ",\"currentPermissions\":" + std::to_string(item.current_permissions) +
                      ",\"everyonePermissions\":" + std::to_string(item.everyone_permissions) +
                      ",\"nextPermissions\":" + std::to_string(item.next_permissions) + '}';
    return transport_->send("POST", "/api/v1/inventory/" + std::string(user_id) + "/items", body)
               .status_code == 201;
}

bool Client::create_inventory_item(std::string_view user_id, const InventoryItem& item) {
    const auto body = "{\"id\":" + api::json_string(item.item_id) +
                      ",\"creatorUserId\":" + api::json_string(item.creator_id) +
                      ",\"folderId\":" + api::json_string(item.folder_id) +
                      ",\"assetId\":" + api::json_string(item.asset_id) +
                      ",\"assetType\":" + std::to_string(item.asset_type) +
                      ",\"inventoryType\":" + std::to_string(item.inventory_type) +
                      ",\"name\":" + api::json_string(item.name) +
                      ",\"description\":" + api::json_string(item.description) +
                      ",\"flags\":" + std::to_string(item.flags) +
                      ",\"basePermissions\":" + std::to_string(item.base_permissions) +
                      ",\"currentPermissions\":" + std::to_string(item.current_permissions) +
                      ",\"everyonePermissions\":" + std::to_string(item.everyone_permissions) +
                      ",\"nextPermissions\":" + std::to_string(item.next_permissions) + '}';
    return transport_->send("POST", "/api/v1/inventory/" + std::string(user_id) + "/items", body)
               .status_code == 201;
}

std::optional<TaskInventoryTransfer> Client::prepare_task_inventory_transfer(
    const TaskInventoryTransferRequest& request) {
    const auto body = "{\"id\":" + api::json_string(request.id) +
        ",\"userId\":" + api::json_string(request.user_id) +
        ",\"sourceItemId\":" + api::json_string(request.source_item_id) +
        ",\"regionId\":" + api::json_string(request.region_id) +
        ",\"objectId\":" + api::json_string(request.object_id) +
        ",\"taskItemId\":" + api::json_string(request.task_item_id) + '}';
    const auto response = transport_->send("POST", "/api/v1/task-transfers", body);
    return response.status_code == 200 ? task_transfer_from_json(response.body) : std::nullopt;
}

std::optional<std::vector<TaskInventoryTransfer>> Client::pending_task_inventory_transfers(
    std::string_view region_id) {
    const auto response = transport_->send(
        "GET", "/api/v1/task-transfers?regionId=" + std::string(region_id), {});
    return response.status_code == 200 ? task_transfer_list_from_json(response.body) : std::nullopt;
}

bool Client::finalize_task_inventory_transfer(
    std::string_view transfer_id, std::string_view region_id) {
    const auto body = "{\"regionId\":" + api::json_string(region_id) + '}';
    return transport_->send("POST", "/api/v1/task-transfers/" +
        std::string(transfer_id) + "/finalize", body).status_code == 200;
}

std::optional<TaskInventoryExtraction> Client::prepare_task_inventory_extraction(
    const TaskInventoryExtractionRequest& request) {
    const auto& item = request.item;
    const auto body = "{\"id\":" + api::json_string(request.id) +
        ",\"userId\":" + api::json_string(request.user_id) +
        ",\"regionId\":" + api::json_string(request.region_id) +
        ",\"objectId\":" + api::json_string(request.object_id) +
        ",\"sourceTaskItemId\":" + api::json_string(request.source_task_item_id) +
        ",\"destinationFolderId\":" + api::json_string(request.destination_folder_id) +
        ",\"personalItemId\":" + api::json_string(request.personal_item_id) +
        ",\"item\":{\"creatorUserId\":" + api::json_string(item.creator_id) +
        ",\"ownerUserId\":" + api::json_string(item.owner_id) +
        ",\"assetId\":" + api::json_string(item.asset_id) +
        ",\"assetType\":" + std::to_string(item.asset_type) +
        ",\"inventoryType\":" + std::to_string(item.inventory_type) +
        ",\"name\":" + api::json_string(item.name) +
        ",\"description\":" + api::json_string(item.description) +
        ",\"flags\":" + std::to_string(item.flags) +
        ",\"basePermissions\":" + std::to_string(item.base_permissions) +
        ",\"currentPermissions\":" + std::to_string(item.current_permissions) +
        ",\"everyonePermissions\":" + std::to_string(item.everyone_permissions) +
        ",\"nextPermissions\":" + std::to_string(item.next_permissions) +
        ",\"saleType\":" + std::to_string(item.sale_type) +
        ",\"salePrice\":" + std::to_string(item.sale_price) + "}}";
    const auto response = transport_->send("POST", "/api/v1/task-extractions", body);
    return response.status_code == 200 ? task_extraction_from_json(response.body) : std::nullopt;
}

std::optional<std::vector<TaskInventoryExtraction>> Client::pending_task_inventory_extractions(
    std::string_view region_id) {
    const auto response = transport_->send(
        "GET", "/api/v1/task-extractions?regionId=" + std::string(region_id), {});
    return response.status_code == 200 ? task_extraction_list_from_json(response.body) : std::nullopt;
}

std::optional<TaskInventoryExtraction> Client::finalize_task_inventory_extraction(
    std::string_view extraction_id, std::string_view region_id) {
    const auto body = "{\"regionId\":" + api::json_string(region_id) + '}';
    const auto response = transport_->send("POST", "/api/v1/task-extractions/" +
        std::string(extraction_id) + "/finalize", body);
    return response.status_code == 200 ? task_extraction_from_json(response.body) : std::nullopt;
}

std::optional<ObjectRez> Client::prepare_object_rez(const ObjectRezRequest& request) {
    const auto body = "{\"id\":" + api::json_string(request.id) +
        ",\"userId\":" + api::json_string(request.user_id) +
        ",\"sourceItemId\":" + api::json_string(request.source_item_id) +
        ",\"regionId\":" + api::json_string(request.region_id) +
        ",\"objectId\":" + api::json_string(request.object_id) + '}';
    const auto response = transport_->send("POST", "/api/v1/object-rezzes", body);
    return response.status_code == 200 ? object_rez_from_json(response.body) : std::nullopt;
}

std::optional<std::vector<ObjectRez>> Client::pending_object_rezzes(std::string_view region_id) {
    const auto response = transport_->send(
        "GET", "/api/v1/object-rezzes?regionId=" + std::string(region_id), {});
    return response.status_code == 200 ? object_rez_list_from_json(response.body) : std::nullopt;
}

bool Client::finalize_object_rez(std::string_view rez_id, std::string_view region_id) {
    const auto body = "{\"regionId\":" + api::json_string(region_id) + '}';
    return transport_->send("POST", "/api/v1/object-rezzes/" +
        std::string(rez_id) + "/finalize", body).status_code == 200;
}

bool Client::rollback_object_rez(std::string_view rez_id, std::string_view region_id) {
    const auto body = "{\"regionId\":" + api::json_string(region_id) + '}';
    return transport_->send("POST", "/api/v1/object-rezzes/" +
        std::string(rez_id) + "/rollback", body).status_code == 200;
}

bool Client::register_asset(std::string_view asset_id, std::string_view creator_id,
                            std::string_view sha256, std::uint64_t size,
                            std::string_view endpoint, bool origin) {
    const auto body = "{\"id\":" + api::json_string(asset_id) +
                      ",\"creatorUserId\":" + api::json_string(creator_id) +
                      ",\"sha256\":" + api::json_string(sha256) +
                      ",\"size\":" + std::to_string(size) +
                      ",\"endpoint\":" + api::json_string(endpoint) +
                      ",\"origin\":" + (origin ? "true" : "false") + '}';
    return transport_->send("POST", "/api/v1/assets", body).status_code == 201;
}

bool Client::store_vault_asset(std::string_view asset_id, std::span<const std::byte> content) {
    return transport_->send("PUT", "/api/v1/vault/assets/" + path_segment(asset_id),
                            std::string_view(reinterpret_cast<const char*>(content.data()),
                                             content.size())).status_code == 200;
}

std::optional<std::string> Client::fetch_asset_rendition(std::string_view asset_id,
                                                         std::string_view kind) {
    const auto response = transport_->send(
        "GET", "/api/v1/assets/" + path_segment(asset_id) + "/renditions/" + std::string(kind), {});
    if (response.status_code != 200 || response.body.empty()) return std::nullopt;
    return response.body;
}

bool Client::request_asset_rendition(std::string_view asset_id, std::string_view kind) {
    const auto body = "{\"kind\":" + api::json_string(kind) + '}';
    return transport_->send("POST", "/api/v1/assets/" + path_segment(asset_id) + "/renditions",
                            body).status_code == 200;
}

std::optional<std::string> Client::fetch_vault_asset(std::string_view asset_id) {
    const auto response = transport_->send(
        "GET", "/api/v1/vault/assets/" + path_segment(asset_id), {});
    if (response.status_code != 200 || response.body.empty()) return std::nullopt;
    return response.body;
}

std::optional<FederatedAsset> Client::find_asset(std::string_view asset_id) {
    const auto response = transport_->send("GET", "/api/v1/assets/" + std::string(asset_id), {});
    if (response.status_code != 200) return std::nullopt;
    FederatedAsset asset;
    asset.asset_id = json_field(response.body, "id");
    asset.creator_id = json_field(response.body, "creatorUserId");
    asset.sha256 = json_field(response.body, "sha256");
    const auto size = json_u64(response.body, "size");
    asset.locations = asset_locations_from_json(response.body);
    if (asset.asset_id != asset_id || asset.creator_id.empty() || asset.sha256.size() != 64 ||
        !size || *size == 0 || asset.locations.empty()) return std::nullopt;
    asset.size = *size;
    return asset;
}

HttpResponse fetch_asset_from(std::string endpoint, std::string service_token,
                              std::string_view asset_id) {
    return socket_transport(std::move(endpoint), std::move(service_token))->send(
        "GET", "/api/v1/assets/" + std::string(asset_id), {});
}

bool prepare_avatar_arrival(Transport& destination, std::string_view transit_id) {
    return destination.send("POST", "/api/v1/transits/" + std::string(transit_id) +
        "/prepare-arrival", {}).status_code == 200;
}

std::optional<InventoryItem> Client::copy_library_item(std::string_view user_id,
                                                       std::string_view source_item_id,
                                                       std::string_view destination_folder_id,
                                                       std::string_view new_name) {
    const auto body = "{\"sourceItemId\":" + api::json_string(source_item_id) +
                      ",\"destinationFolderId\":" + api::json_string(destination_folder_id) +
                      ",\"name\":" + api::json_string(new_name) + '}';
    const auto response = transport_->send(
        "POST", "/api/v1/inventory/" + std::string(user_id) + "/copy-library-item", body);
    if (response.status_code != 201) return std::nullopt;
    return inventory_item_from_json(response.body, user_id);
}

std::optional<InventoryItem> Client::copy_inventory_item(std::string_view user_id,
                                                         std::string_view source_item_id,
                                                         std::string_view destination_folder_id,
                                                         std::string_view new_name) {
    const auto body = "{\"sourceItemId\":" + api::json_string(source_item_id) +
                      ",\"destinationFolderId\":" + api::json_string(destination_folder_id) +
                      ",\"name\":" + api::json_string(new_name) + '}';
    const auto response = transport_->send(
        "POST", "/api/v1/inventory/" + std::string(user_id) + "/copy-item", body);
    if (response.status_code != 201) return std::nullopt;
    return inventory_item_from_json(response.body, user_id);
}

std::optional<ViewerSession> ViewerSessionCache::validate(
    std::string_view session_id, std::chrono::steady_clock::time_point now) {
    const auto key = std::string(session_id);
    if (const auto found = entries_.find(key); found != entries_.end()) {
        if (now < found->second.expires_at) return found->second.session;
        entries_.erase(found);
    }
    auto session = client_.validate_viewer_session(session_id);
    if (session) entries_.insert_or_assign(key, Entry{*session, now + ttl_});
    return session;
}

void ViewerSessionCache::invalidate(std::string_view session_id) {
    entries_.erase(std::string(session_id));
}

bool Client::update_presence(std::string_view user_id, std::string_view region_id) {
    const auto body = "{\"regionId\":" + api::json_string(region_id) + '}';
    return transport_->send("PUT", "/api/v1/presence/" + std::string(user_id), body).status_code == 200;
}

bool Client::clear_presence(std::string_view user_id) {
    const auto status = transport_->send("DELETE", "/api/v1/presence/" + std::string(user_id), {}).status_code;
    return status == 204 || status == 404;
}

bool Client::update_last_location(std::string_view user_id, std::string_view region_id,
                                  const std::array<float, 3>& position,
                                  const std::array<float, 3>& look_at, bool flying) {
    const auto vector_json = [](const std::array<float, 3>& value) {
        return std::string{"{\"x\":"} + std::to_string(value[0]) +
               ",\"y\":" + std::to_string(value[1]) +
               ",\"z\":" + std::to_string(value[2]) + '}';
    };
    const auto body = "{\"regionId\":" + api::json_string(region_id) +
                      ",\"position\":" + vector_json(position) +
                      ",\"lookAt\":" + vector_json(look_at) +
                      ",\"flying\":" + (flying ? "true" : "false") + '}';
    return transport_->send(
        "PUT", "/api/v1/locations/" + std::string(user_id), body).status_code == 200;
}

bool Client::set_home_location(std::string_view user_id, std::string_view region_id,
                               const std::array<float, 3>& position,
                               const std::array<float, 3>& look_at) {
    const auto vector_json = [](const std::array<float, 3>& value) {
        return std::string{"{\"x\":"} + std::to_string(value[0]) +
               ",\"y\":" + std::to_string(value[1]) +
               ",\"z\":" + std::to_string(value[2]) + '}';
    };
    const auto body = "{\"regionId\":" + api::json_string(region_id) +
                      ",\"position\":" + vector_json(position) +
                      ",\"lookAt\":" + vector_json(look_at) +
                      ",\"flying\":false}";
    return transport_->send(
        "PUT", "/api/v1/locations/" + std::string(user_id) + "?scope=home", body).status_code == 200;
}

std::optional<HomeLocation> Client::home_location(std::string_view user_id) {
    const auto response = transport_->send(
        "GET", "/api/v1/locations/" + std::string(user_id) + "?scope=home", "");
    if (response.status_code != 200) return std::nullopt;
    HomeLocation home;
    home.region_id = json_field(response.body, "regionId");
    if (home.region_id.empty()) return std::nullopt;
    // Position and lookAt marshal as JSON arrays ([x,y,z]) from the grid.
    const auto parse_vec3 = [](std::string_view body, std::string_view name,
                               std::array<float, 3>& out) -> bool {
        const auto marker = "\"" + std::string(name) + "\":[";
        const auto start = body.find(marker);
        if (start == std::string_view::npos) return false;
        const std::string chunk(body.substr(start + marker.size()));
        const char* cursor = chunk.c_str();
        for (auto& component : out) {
            char* next = nullptr;
            component = std::strtof(cursor, &next);
            if (next == cursor) return false;
            cursor = next;
            while (*cursor == ',' || *cursor == ' ') ++cursor;
        }
        return true;
    };
    if (!parse_vec3(response.body, "position", home.position)) return std::nullopt;
    parse_vec3(response.body, "lookAt", home.look_at);
    return home;
}

std::optional<TicketIdentity> Client::validate_region_ticket(std::string_view region_id,
                                                             std::string_view token) {
    const auto body = "{\"token\":" + api::json_string(token) + '}';
    const auto response = transport_->send(
        "POST", "/api/v1/region-runtime/" + path_segment(region_id) + "/validate-ticket", body);
    if (response.status_code != 200) return std::nullopt;
    TicketIdentity identity;
    identity.user_id = json_field(response.body, "userId");
    identity.userid = json_field(response.body, "userid");
    identity.display_name = json_field(response.body, "displayName");
    identity.session_id = json_field(response.body, "sessionId");
    if (identity.user_id.empty() || identity.session_id.empty()) return std::nullopt;
    // position marshals as a JSON array and is absent when world entry
    // resolved no explicit arrival point.
    if (const auto marker = response.body.find("\"position\":[");
        marker != std::string::npos) {
        std::array<float, 3> arrival{};
        const char* cursor = response.body.c_str() + marker + 12;
        bool parsed = true;
        for (auto& component : arrival) {
            char* next = nullptr;
            component = std::strtof(cursor, &next);
            if (next == cursor) { parsed = false; break; }
            cursor = next;
            while (*cursor == ',' || *cursor == ' ') ++cursor;
        }
        if (parsed) identity.arrival = arrival;
    }
    return identity;
}

std::optional<std::vector<WornAttachment>> Client::worn_attachments(std::string_view user_id) {
    const auto response = transport_->send(
        "GET", "/api/v1/attachments/" + std::string(user_id), {});
    if (response.status_code != 200) return std::nullopt;
    std::vector<WornAttachment> result;
    const auto& body = response.body;
    std::size_t position = body.find('[');
    if (position == std::string::npos) return std::nullopt;
    ++position;
    while (position < body.size()) {
        while (position < body.size() && (body[position] == ' ' || body[position] == ',')) ++position;
        if (position < body.size() && body[position] == ']') return result;
        if (position >= body.size() || body[position] != '{') return std::nullopt;
        const auto start = position;
        std::size_t depth = 0;
        bool quoted = false;
        bool escaped = false;
        for (; position < body.size(); ++position) {
            const auto character = body[position];
            if (quoted) {
                if (escaped) escaped = false;
                else if (character == '\\') escaped = true;
                else if (character == '"') quoted = false;
                continue;
            }
            if (character == '"') quoted = true;
            else if (character == '{') ++depth;
            else if (character == '}' && --depth == 0) { ++position; break; }
        }
        if (depth != 0) return std::nullopt;
        const auto element = std::string_view(body).substr(start, position - start);
        WornAttachment worn;
        worn.item_id = json_field(element, "itemId");
        const auto point = json_int(element, "attachmentPoint");
        // A row without a usable point is a row this region cannot act on, and
        // rezzing it somewhere arbitrary is worse than reporting it: refuse the
        // whole answer rather than silently wear one thing in the wrong place.
        if (worn.item_id.empty() || !point || *point < 1 || *point > 127) return std::nullopt;
        worn.attachment_point = static_cast<std::uint8_t>(*point);
        result.push_back(std::move(worn));
    }
    return std::nullopt;
}

bool Client::set_attachment_worn(std::string_view user_id, std::string_view item_id,
                                 std::uint8_t attachment_point, bool worn) {
    const auto body = "{\"itemId\":" + api::json_string(item_id) +
                      ",\"attachmentPoint\":" + std::to_string(attachment_point) +
                      ",\"worn\":" + (worn ? "true" : "false") + '}';
    const auto status = transport_->send(
        "PUT", "/api/v1/attachments/" + std::string(user_id), body).status_code;
    return status == 200 || status == 204;
}

bool Client::set_gesture_active(std::string_view user_id, std::string_view item_id,
                                std::string_view asset_id, bool active) {
    const auto body = "{\"itemId\":" + api::json_string(item_id) +
                      ",\"assetId\":" + api::json_string(asset_id) +
                      ",\"active\":" + (active ? "true" : "false") + '}';
    const auto status = transport_->send(
        "PUT", "/api/v1/gestures/" + std::string(user_id), body).status_code;
    return status == 200 || status == 204;
}

RegistrationLifecycle::RegistrationLifecycle(Client client, RegionSettings settings,
                                               std::string registered_region_id)
    : client_(std::move(client)), settings_(std::move(settings)),
      region_id_(std::move(registered_region_id)), already_registered_(!region_id_.empty()) {}

bool RegistrationLifecycle::start(std::chrono::steady_clock::time_point now) {
    if (!already_registered_) region_id_ = client_.register_region(settings_);
    if (region_id_.empty()) return false;
    renew_at_ = now + std::chrono::seconds(settings_.lease_seconds / 2);
    return true;
}

bool RegistrationLifecycle::tick(std::chrono::steady_clock::time_point now) {
    if (region_id_.empty() || now < renew_at_) return !region_id_.empty();
    last_error_.clear();
    const auto renewed = already_registered_ ?
        client_.renew_provisioned_lease(region_id_, settings_.lease_seconds, &last_error_) :
        client_.renew_lease(region_id_, settings_.lease_seconds);
    if (!renewed) return false;
    renew_at_ = now + std::chrono::seconds(settings_.lease_seconds / 2);
    return true;
}

void RegistrationLifecycle::stop() {
    if (!region_id_.empty()) {
        if (already_registered_) client_.deregister_provisioned(region_id_);
        else client_.deregister(region_id_);
    }
    region_id_.clear();
}

} // namespace homeworldz::grid
