#pragma once

#include <chrono>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace homeworldz::grid {

struct HttpResponse {
    int status_code{};
    std::string body;
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual HttpResponse send(std::string_view method, std::string_view path,
                              std::string_view body) = 0;
};

std::shared_ptr<Transport> socket_transport(std::string grid_url, std::string service_token);

// The grid-region protocol version this region software implements
// (docs/CLIENT2.md, "the region protocol version"). Compiled in rather than
// configured so the number cannot be separated from the code it describes; it
// increments only when a change genuinely requires region software to be
// upgraded, and the grid refuses registration on any mismatch.
constexpr int region_protocol = 1;

struct RegionSettings {
    std::string name;
    int grid_x{};
    int grid_y{};
    std::string public_endpoint;
    int viewer_port{42002};
    int lease_seconds{60};
    // session_endpoint is the public ws:// or wss:// URL of this region's
    // session transport, reported at registration when it serves one
    // (docs/CLIENT2-TRANSPORT.md).
    std::string session_endpoint;
};

// TicketIdentity is who a region ticket resolves to, answered by the grid —
// the ticket-signing secret never reaches a region.
struct TicketIdentity {
    std::string user_id;
    std::string userid;
    std::string display_name;
    std::string session_id;
    // arrival is the region-local position world entry resolved for this
    // session, when it resolved one. It comes from the grid rather than the
    // client, which must never choose where it spawns.
    std::optional<std::array<float, 3>> arrival;
};

struct Estate {
    int id{};
    std::string name;
    std::string owner_id;
    int parent_estate_id{1};
    std::uint64_t flags{};
    bool public_access{true};
    double sun_hour{};
    bool use_global_time{true};
    bool fixed_sun{};
    double billable_factor{};
    int price_per_meter{};
    int redirect_grid_x{};
    int redirect_grid_y{};
    std::string abuse_email;
    std::vector<std::string> managers;
    std::vector<std::string> allowed_users;
    std::vector<std::string> allowed_groups;
    std::vector<std::string> bans;
};

// Partial estate settings update; only the engaged fields are sent.
struct EstateSettingsPatch {
    std::optional<std::string> name;
    std::optional<std::uint64_t> flags;
    std::optional<bool> public_access;
    std::optional<bool> fixed_sun;
    std::optional<bool> use_global_time;
    std::optional<double> sun_hour;
};

struct RegisteredRegion {
    std::string id;
    std::string name;
    int grid_x{};
    int grid_y{};
	std::string public_endpoint;
	int viewer_port{};
	std::string grid_name;
	std::string grid_public_url;
	int size_x{256};
	int size_y{256};
	int maturity{};
	std::string owner_id;
	std::optional<Estate> estate;
	// The grid's current grid-region protocol version from the registration
	// reply; how a region learns an increment is coming before it is enforced.
	int grid_region_protocol{};
};

// Where a region sits on the grid and how to reach it. Adjacency is a
// property of a pair of regions, not of a region, so it lives on
// RegionNeighbor below: a teleport destination is a placement with no
// direction, because it can be anywhere.
struct RegionPlacement {
    std::string id;
    std::string name;
    int grid_x{};
    int grid_y{};
	int size_x{256};
	int size_y{256};
	int maturity{};
    std::string public_endpoint;
    int viewer_port{};
	bool online{};
    // session_endpoint is the region's session URL when it serves one; empty
    // means a session avatar cannot continue there
    // (docs/CLIENT2-EMBODIMENT.md milestone E2).
    std::string session_endpoint;
    bool operator==(const RegionPlacement&) const = default;
};

struct RegionNeighbor : RegionPlacement {
    std::string direction;
    bool operator==(const RegionNeighbor&) const = default;
};

struct ViewerSession {
    std::string session_id;
    std::string secure_session_id;
    std::string agent_id;
    std::uint32_t circuit_code{};
    std::string destination_region_id;
};

struct AvatarTransitRequest {
    std::string id;
    std::string agent_id;
    std::string session_id;
    std::string source_region_id;
    std::string destination_region_id;
    std::array<float, 3> position{};
    std::array<float, 3> look_at{};
    bool flying{};
    int lifetime_seconds{30};
};

struct AvatarTransit {
    std::string id;
    std::uint64_t generation{};
    std::string agent_id;
    std::string session_id;
    std::string source_region_id;
    std::string destination_region_id;
    std::array<float, 3> position{};
    std::array<float, 3> look_at{};
    bool flying{};
    std::string state;
};

struct User {
    std::string id;
    std::string username;
};

struct AssetLocation {
    std::string endpoint;
    bool origin{};
};

struct HomeLocation {
    std::string region_id;
    std::array<float, 3> position{};
    std::array<float, 3> look_at{};
};

struct FederatedAsset {
    std::string asset_id;
    std::string creator_id;
    std::string sha256;
    std::uint64_t size{};
    std::vector<AssetLocation> locations;
};

struct TextureInventoryItem {
    std::string item_id;
    std::string creator_id;
    std::string folder_id;
    std::string asset_id;
    std::string name;
    std::string description;
    std::uint32_t everyone_permissions{};
    std::uint32_t next_permissions{};
};

struct ObjectInventoryItem {
    std::string item_id;
    std::string creator_id;
    std::string folder_id;
    std::string asset_id;
    std::string name;
    std::string description;
    std::uint32_t base_permissions{};
    std::uint32_t current_permissions{};
    std::uint32_t everyone_permissions{};
    std::uint32_t next_permissions{};
};

struct InventoryItem {
    std::string item_id;
    std::string creator_id;
    std::string owner_id;
    std::string folder_id;
    std::string asset_id;
    int asset_type{};
    int inventory_type{};
    std::string name;
    std::string description;
    std::uint32_t flags{};
    std::uint32_t base_permissions{};
    std::uint32_t current_permissions{};
    std::uint32_t everyone_permissions{};
    std::uint32_t next_permissions{};
    int sale_type{};
    int sale_price{};
};

// Why an inventory lookup produced no item. `missing` is the grid answering
// about the item; `unavailable` is no answer at all — an unreachable grid, a
// refused credential, or a route this grid does not serve. Callers that report
// a reason must not print the second as the first: "your item is not there" is
// a verdict, and a lookup that could not be run has not reached one.
enum class InventoryLookup { found, missing, unavailable };

struct InventoryItemLookup {
    InventoryLookup outcome{InventoryLookup::unavailable};
    std::optional<InventoryItem> item;
};

// One entry of an inventory folder, reduced to what a caller acts on. A link
// contributes its own identity and its target's asset: the wearer sees the link
// in the Current Outfit folder, and a bake fetches the asset behind it. An
// entry whose asset_id is empty is a link whose target inventory no longer
// holds — a broken outfit, which the caller reports rather than skips.
struct FolderEntry {
    std::string item_id;
    std::string name;
    std::string asset_id;
    int asset_type{};
};

// One worn item, as the grid records it: the inventory item and the point. The
// object asset is resolved through inventory at attach time rather than stored,
// so a worn item that changed comes back as it is now.
struct WornAttachment {
    std::string item_id;
    std::uint8_t attachment_point{};
};

struct TaskInventoryTransferRequest {
    std::string id;
    std::string user_id;
    std::string source_item_id;
    std::string region_id;
    std::string object_id;
    std::string task_item_id;
};

struct TaskInventoryTransfer {
    std::string id;
    std::string user_id;
    std::string source_item_id;
    std::string region_id;
    std::string object_id;
    std::string task_item_id;
    InventoryItem item;
    std::string state;
};

struct TaskInventoryExtractionRequest {
    std::string id;
    std::string user_id;
    std::string region_id;
    std::string object_id;
    std::string source_task_item_id;
    std::string destination_folder_id;
    std::string personal_item_id;
    InventoryItem item;
};

struct TaskInventoryExtraction {
    std::string id;
    std::string user_id;
    std::string region_id;
    std::string object_id;
    std::string source_task_item_id;
    std::string destination_folder_id;
    std::string personal_item_id;
    InventoryItem item;
    std::string state;
};

struct ObjectRezRequest {
    std::string id;
    std::string user_id;
    std::string source_item_id;
    std::string region_id;
    std::string object_id;
};

struct ObjectRez {
    std::string id;
    std::string user_id;
    std::string source_item_id;
    std::string region_id;
    std::string object_id;
    InventoryItem item;
    std::string state;
};

class Client {
public:
    explicit Client(std::shared_ptr<Transport> transport) : transport_(std::move(transport)) {}
    std::string register_region(const RegionSettings& settings);
	// On failure, *refusal (when given) receives the grid's error message —
	// a protocol-mismatch refusal names both versions and belongs in the log.
	std::optional<RegisteredRegion> register_provisioned_region(
		std::string_view region_id, const RegionSettings& settings,
		std::string* refusal = nullptr);
    std::optional<std::vector<RegionNeighbor>> find_region_neighbors(
        std::string_view region_id);
    // Teleport destination resolution. The neighbor list answers crossings,
    // but a map, landmark, or home teleport names a region that is usually
    // not adjacent, so these ask the grid instead. Both return offline
    // regions too — placed but down is a different report from not there.
    std::optional<RegionPlacement> find_region_at(int grid_x, int grid_y);
    std::optional<RegionPlacement> find_region(std::string_view region_id);
    // Every region the grid has placed. What a world map draws: a map of the
    // regions next door is not a map.
    std::optional<std::vector<RegionPlacement>> find_grid_topology();
    std::optional<Estate> update_estate_settings(std::string_view region_id,
                                                 const EstateSettingsPatch& patch);
    std::optional<Estate> set_estate_member(std::string_view region_id, std::string_view member_id,
                                            int role, bool present);
    bool renew_lease(std::string_view region_id, int lease_seconds);
    bool deregister(std::string_view region_id);
	bool renew_provisioned_lease(std::string_view region_id, int lease_seconds,
	                             std::string* refusal = nullptr);
	bool deregister_provisioned(std::string_view region_id);
    std::optional<ViewerSession> validate_viewer_session(std::string_view session_id);
    std::optional<AvatarTransit> prepare_avatar_transit(const AvatarTransitRequest& request);
    std::optional<AvatarTransit> find_avatar_transit(std::string_view transit_id);
    std::optional<AvatarTransit> accept_avatar_transit(
        std::string_view transit_id, std::string_view destination_region_id);
    std::optional<AvatarTransit> activate_avatar_transit(
        std::string_view transit_id, std::string_view destination_region_id);
    std::optional<AvatarTransit> rollback_avatar_transit(
        std::string_view transit_id, std::string_view region_id, std::string_view reason);
    std::optional<User> find_user(std::string_view user_id);
    bool revoke_viewer_session(std::string_view session_id);
    bool create_inventory_folder(std::string_view user_id, std::string_view folder_id,
                                 std::string_view parent_id, std::string_view name, int type_default);
    bool move_inventory_folder(std::string_view user_id, std::string_view folder_id,
                               std::string_view parent_id);
    bool move_inventory_item(std::string_view user_id, std::string_view item_id,
                             std::string_view folder_id, std::string_view new_name);
    bool update_inventory_item_asset(std::string_view user_id, std::string_view item_id,
                                     std::string_view asset_id);
    // lookup_inventory_item is find_inventory_item with the reason kept. Prefer
    // it wherever the absence of an item is reported rather than merely acted on.
    InventoryItemLookup lookup_inventory_item(std::string_view user_id,
                                              std::string_view item_id);
    std::optional<InventoryItem> find_inventory_item(std::string_view user_id,
                                                     std::string_view item_id);
    std::optional<std::string> find_system_inventory_folder(std::string_view user_id,
                                                            int folder_type);
    // The contents of one folder, each link already followed. Written for the
    // Current Outfit folder, whose entries are links (asset type 24) naming
    // items elsewhere in inventory, so `asset_id` and `asset_type` describe
    // what is worn while `item_id` and `name` stay those of the link itself.
    // An empty vector is an empty folder; nullopt is no answer.
    std::optional<std::vector<FolderEntry>> list_inventory_folder_items(
        std::string_view user_id, std::string_view folder_id);
    bool create_texture_inventory_item(std::string_view user_id, const TextureInventoryItem& item);
    bool create_object_inventory_item(std::string_view user_id, const ObjectInventoryItem& item);
    bool create_inventory_item(std::string_view user_id, const InventoryItem& item);
    std::optional<TaskInventoryTransfer> prepare_task_inventory_transfer(
        const TaskInventoryTransferRequest& request);
    std::optional<std::vector<TaskInventoryTransfer>> pending_task_inventory_transfers(
        std::string_view region_id);
    bool finalize_task_inventory_transfer(
        std::string_view transfer_id, std::string_view region_id);
    std::optional<TaskInventoryExtraction> prepare_task_inventory_extraction(
        const TaskInventoryExtractionRequest& request);
    std::optional<std::vector<TaskInventoryExtraction>> pending_task_inventory_extractions(
        std::string_view region_id);
    std::optional<TaskInventoryExtraction> finalize_task_inventory_extraction(
        std::string_view extraction_id, std::string_view region_id);
    std::optional<ObjectRez> prepare_object_rez(const ObjectRezRequest& request);
    std::optional<std::vector<ObjectRez>> pending_object_rezzes(std::string_view region_id);
    bool finalize_object_rez(std::string_view rez_id, std::string_view region_id);
    bool rollback_object_rez(std::string_view rez_id, std::string_view region_id);
    bool register_asset(std::string_view asset_id, std::string_view creator_id,
                        std::string_view sha256, std::uint64_t size,
                        std::string_view endpoint, bool origin);
    std::optional<FederatedAsset> find_asset(std::string_view asset_id);
    // Read inventory-referenced bytes from the grid's asset vault (ADR 0026).
    // For anything inventory references this location always has the content,
    // which is what stops a region from being the last copy of it. Returns the
    // raw bytes; the caller still verifies them against the registry checksum,
    // as it does for a peer region.
    std::optional<std::string> fetch_vault_asset(std::string_view asset_id);
    // Queue a derived-encoding conversion for an asset (ADR 0033); idempotent
    // on the grid side, so re-requesting is safe.
    bool request_asset_rendition(std::string_view asset_id, std::string_view kind);
    // Read a derived encoding back (ADR 0033) — what a region serves viewers
    // when they fetch a mesh. nullopt covers both absent-yet and errors; the
    // caller answers not-found either way and the viewer retries.
    std::optional<std::string> fetch_asset_rendition(std::string_view asset_id,
                                                     std::string_view kind);
    // Write asset bytes through to the grid vault (ADR 0026). The vault
    // verifies them against the registered checksum, so this cannot vouch for
    // wrong bytes. On the upload path it is load-bearing, not just an
    // optimization: the inventory commit's durability check must find the
    // blob already held, because this region's single HTTP thread is busy
    // with the upload and cannot answer a fetch-back until it returns.
    bool store_vault_asset(std::string_view asset_id, std::span<const std::byte> content);
    std::optional<InventoryItem> copy_library_item(std::string_view user_id,
                                                   std::string_view source_item_id,
                                                   std::string_view destination_folder_id,
                                                   std::string_view new_name);
    std::optional<InventoryItem> copy_inventory_item(std::string_view user_id,
                                                     std::string_view source_item_id,
                                                     std::string_view destination_folder_id,
                                                     std::string_view new_name);
    bool update_presence(std::string_view user_id, std::string_view region_id);
    bool clear_presence(std::string_view user_id);
    bool update_last_location(std::string_view user_id, std::string_view region_id,
                              const std::array<float, 3>& position,
                              const std::array<float, 3>& look_at, bool flying);
    bool set_home_location(std::string_view user_id, std::string_view region_id,
                           const std::array<float, 3>& position,
                           const std::array<float, 3>& look_at);
    std::optional<HomeLocation> home_location(std::string_view user_id);
    bool set_gesture_active(std::string_view user_id, std::string_view item_id,
                            std::string_view asset_id, bool active);
    // What a user is wearing, so an avatar arriving here rezzes back what it
    // had on wherever it was last. nullopt is a grid that could not answer,
    // which is not the same as an empty list: arriving with nothing on is a
    // fact, and failing to ask is not a reason to strip someone.
    std::optional<std::vector<WornAttachment>> worn_attachments(std::string_view user_id);
    bool set_attachment_worn(std::string_view user_id, std::string_view item_id,
                             std::uint8_t attachment_point, bool worn);
    // validate_region_ticket asks the grid to resolve a client's region
    // ticket for this region; nullopt is any refusal.
    std::optional<TicketIdentity> validate_region_ticket(std::string_view region_id,
                                                         std::string_view token);

private:
    std::shared_ptr<Transport> transport_;
};

HttpResponse fetch_asset_from(std::string endpoint, std::string service_token,
                              std::string_view asset_id);
bool prepare_avatar_arrival(Transport& destination, std::string_view transit_id);

class ViewerSessionCache {
public:
    explicit ViewerSessionCache(Client& client,
                                std::chrono::steady_clock::duration ttl = std::chrono::seconds(5))
        : client_(client), ttl_(ttl) {}
    std::optional<ViewerSession> validate(
        std::string_view session_id,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    void invalidate(std::string_view session_id);

private:
    struct Entry {
        ViewerSession session;
        std::chrono::steady_clock::time_point expires_at;
    };
    Client& client_;
    std::chrono::steady_clock::duration ttl_;
    std::unordered_map<std::string, Entry> entries_;
};

class RegistrationLifecycle {
public:
    RegistrationLifecycle(Client client, RegionSettings settings,
                          std::string registered_region_id = {});
    bool start(std::chrono::steady_clock::time_point now);
    bool tick(std::chrono::steady_clock::time_point now);
    void stop();
    const std::string& region_id() const { return region_id_; }
    // The grid's message from the most recent failed renewal, empty otherwise.
    const std::string& last_error() const { return last_error_; }

private:
    Client client_;
    RegionSettings settings_;
    std::string region_id_;
    std::chrono::steady_clock::time_point renew_at_{};
	bool already_registered_{};
    std::string last_error_;
};

} // namespace homeworldz::grid
