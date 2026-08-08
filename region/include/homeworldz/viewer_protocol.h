#pragma once

#include "homeworldz/terrain_layers.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <array>

namespace homeworldz::viewer {

inline constexpr std::uint8_t flag_zero_coded = 0x80;
inline constexpr std::uint8_t flag_reliable = 0x40;
inline constexpr std::uint8_t flag_resent = 0x20;
inline constexpr std::uint8_t flag_appended_acks = 0x10;

struct Packet {
    std::uint8_t flags{};
    std::uint32_t sequence{};
    std::vector<std::byte> extra_header;
    std::vector<std::byte> payload;
    std::vector<std::uint32_t> acknowledgements;
};

using Uuid = std::array<std::byte, 16>;

std::optional<Uuid> parse_uuid(std::string_view text);
std::string format_uuid(const Uuid& value);
Uuid combine_uuids(const Uuid& first, const Uuid& second);

struct UseCircuitCode {
    std::uint32_t circuit_code{};
    Uuid session_id{};
    Uuid agent_id{};
};

struct AgentMessage {
    Uuid agent_id{};
    Uuid session_id{};
};

struct TeleportLocationRequest : AgentMessage {
    std::uint64_t region_handle{};
    std::array<float, 3> position{};
    std::array<float, 3> look_at{};
};

// TeleportLandmarkRequest (Low 65). A null landmark_id means "Teleport Home".
struct TeleportLandmarkRequest : AgentMessage {
    Uuid landmark_id{};
};

// SetStartLocationRequest (Low 324). Sets the user's Home to the given position
// in the current region ("World > Set Home to Here").
struct SetStartLocationRequest : AgentMessage {
    std::uint32_t location_id{};
    std::array<float, 3> position{};
    std::array<float, 3> look_at{};
};

// ActivateGestures (Low 316) / DeactivateGestures (Low 317). The viewer sends
// these when the user (de)activates gestures in inventory; the active set is
// persisted on the grid and replayed in the login response.
struct GestureActivation {
    Uuid item_id{};
    Uuid asset_id{};
};
struct ActivateGestures : AgentMessage {
    std::vector<GestureActivation> gestures;
};
struct DeactivateGestures : AgentMessage {
    std::vector<Uuid> item_ids;
};

struct TeleportStart {
    std::uint32_t flags{};
};

struct TeleportLocal {
    Uuid agent_id{};
    std::uint32_t location_id{2};
    std::array<float, 3> position{};
    std::array<float, 3> look_at{};
    std::uint32_t teleport_flags{};
};

struct TeleportFailed {
    Uuid agent_id{};
    std::string reason;
};

struct CreateInventoryFolder : AgentMessage {
    Uuid folder_id{};
    Uuid parent_id{};
    std::int8_t type{-1};
    std::string name;
};

struct CreateInventoryItem : AgentMessage {
    std::uint32_t callback_id{};
    Uuid folder_id{};
    Uuid transaction_id{};
    std::uint32_t next_owner_permissions{};
    std::int8_t asset_type{-1};
    std::int8_t inventory_type{-1};
    std::uint8_t wearable_type{};
    std::string name;
    std::string description;
};

struct CopyInventoryItem : AgentMessage {
    std::uint32_t callback_id{};
    Uuid old_agent_id{};
    Uuid old_item_id{};
    Uuid new_folder_id{};
    std::string new_name;
};

struct InventoryFolderMove {
    Uuid folder_id{};
    Uuid parent_id{};
};

struct MoveInventoryFolder : AgentMessage {
    bool stamp{};
    std::vector<InventoryFolderMove> folders;
};

struct InventoryItemMove {
    Uuid item_id{};
    Uuid folder_id{};
    std::string new_name;
};

struct MoveInventoryItem : AgentMessage {
    bool stamp{};
    std::vector<InventoryItemMove> items;
};

struct RequestTaskInventory : AgentMessage {
    std::uint32_t local_id{};
};

struct ReplyTaskInventory {
    Uuid task_id{};
    std::int16_t serial{};
    std::string filename;
};

struct UpdateTaskInventory : AgentMessage {
    std::uint32_t local_id{};
    std::uint8_t key{};
    Uuid item_id{};
    Uuid folder_id{};
    Uuid creator_id{};
    Uuid owner_id{};
    Uuid group_id{};
    std::uint32_t base_permissions{};
    std::uint32_t owner_permissions{};
    std::uint32_t group_permissions{};
    std::uint32_t everyone_permissions{};
    std::uint32_t next_owner_permissions{};
    bool group_owned{};
    Uuid transaction_id{};
    std::int8_t asset_type{-1};
    std::int8_t inventory_type{-1};
    std::uint32_t flags{};
    std::uint8_t sale_type{};
    std::int32_t sale_price{};
    std::string name;
    std::string description;
    std::int32_t creation_date{};
    std::uint32_t crc{};
};

struct RezScript : AgentMessage {
    Uuid agent_group_id{};
    std::uint32_t local_id{};
    bool enabled{};
    Uuid item_id{};
    Uuid folder_id{};
    Uuid creator_id{};
    Uuid owner_id{};
    Uuid group_id{};
    std::uint32_t base_permissions{};
    std::uint32_t owner_permissions{};
    std::uint32_t group_permissions{};
    std::uint32_t everyone_permissions{};
    std::uint32_t next_owner_permissions{};
    bool group_owned{};
    Uuid transaction_id{};
    std::int8_t asset_type{-1};
    std::int8_t inventory_type{-1};
    std::uint32_t flags{};
    std::uint8_t sale_type{};
    std::int32_t sale_price{};
    std::string name;
    std::string description;
    std::int32_t creation_date{};
    std::uint32_t crc{};
};

struct RemoveTaskInventory : AgentMessage {
    std::uint32_t local_id{};
    Uuid item_id{};
};

struct MoveTaskInventory : AgentMessage {
    Uuid folder_id{};
    std::uint32_t local_id{};
    Uuid item_id{};
};

struct RequestXfer {
    std::uint64_t id{};
    std::string filename;
};

struct ObjectAdd : AgentMessage {
    Uuid group_id{};
    std::uint8_t pcode{};
    std::uint8_t material{};
    std::uint32_t add_flags{};
    std::uint8_t path_curve{};
    std::uint8_t profile_curve{};
    std::uint16_t path_begin{};
    std::uint16_t path_end{};
    std::uint8_t path_scale_x{};
    std::uint8_t path_scale_y{};
    std::uint8_t path_shear_x{};
    std::uint8_t path_shear_y{};
    std::uint8_t path_twist{};
    std::uint8_t path_twist_begin{};
    std::uint8_t path_radius_offset{};
    std::uint8_t path_taper_x{};
    std::uint8_t path_taper_y{};
    std::uint8_t path_revolutions{};
    std::uint8_t path_skew{};
    std::uint16_t profile_begin{};
    std::uint16_t profile_end{};
    std::uint16_t profile_hollow{};
    std::array<float, 3> ray_start{};
    std::array<float, 3> ray_end{};
    Uuid ray_target_id{};
    bool bypass_raycast{};
    bool ray_end_is_intersection{};
    std::array<float, 3> scale{};
    std::array<float, 3> rotation{};
    std::uint8_t state{};
};

struct DeRezObject : AgentMessage {
    Uuid group_id{};
    std::uint8_t destination{};
    Uuid destination_id{};
    Uuid transaction_id{};
    std::uint8_t packet_count{};
    std::uint8_t packet_number{};
    std::vector<std::uint32_t> local_ids;
};

// RezSingleAttachmentFromInv (Low 395): wear an object from inventory. The
// viewer sends this for "Wear" and for "Attach To >" alike; `attachment_point`
// is zero when the user did not pick one, meaning "use whatever the item asks
// for, or a default".
struct RezSingleAttachmentFromInv : AgentMessage {
    Uuid item_id{};
    Uuid owner_id{};
    std::uint8_t attachment_point{};
    std::uint32_t item_flags{};
    std::uint32_t group_mask{};
    std::uint32_t everyone_mask{};
    std::uint32_t next_owner_mask{};
    std::string name;
    std::string description;
};

// ObjectDetach (Low 113): stop wearing, by the local ids of the attachments
// themselves rather than by inventory item.
struct ObjectDetach : AgentMessage {
    std::vector<std::uint32_t> local_ids;
};

std::optional<RezSingleAttachmentFromInv> decode_rez_single_attachment_from_inv(
    std::span<const std::byte> payload);
std::optional<ObjectDetach> decode_object_detach(std::span<const std::byte> payload);

struct RezObject : AgentMessage {
    Uuid group_id{};
    Uuid from_task_id{};
    std::uint8_t bypass_raycast{};
    std::array<float, 3> ray_start{};
    std::array<float, 3> ray_end{};
    Uuid ray_target_id{};
    bool ray_end_is_intersection{};
    bool rez_selected{};
    bool remove_item{};
    Uuid item_id{};
};

struct ObjectSelect : AgentMessage {
    std::vector<std::uint32_t> local_ids;
};

struct ObjectGrab : AgentMessage {
    std::uint32_t local_id{};
    std::array<float, 3> grab_offset{};
};

struct ObjectGrabUpdate : AgentMessage {
    Uuid object_id{};
    std::array<float, 3> grab_offset_initial{};
    std::array<float, 3> grab_position{};
    std::uint32_t time_since_last{};
};

struct ObjectTransformUpdate {
    std::uint32_t local_id{};
    std::uint8_t type{};
    std::optional<std::array<float, 3>> position;
    std::optional<std::array<float, 3>> rotation;
    std::optional<std::array<float, 3>> scale;
};

struct MultipleObjectUpdate : AgentMessage {
    std::vector<ObjectTransformUpdate> objects;
};

struct ObjectNameUpdate {
    std::uint32_t local_id{};
    std::string name;
};

struct ObjectName : AgentMessage {
    std::vector<ObjectNameUpdate> objects;
};

struct ObjectDescriptionUpdate {
    std::uint32_t local_id{};
    std::string description;
};

struct ObjectDescription : AgentMessage {
    std::vector<ObjectDescriptionUpdate> objects;
};

struct ObjectPermissionUpdate {
    std::uint32_t local_id{};
    std::uint8_t field{};
    bool set{};
    std::uint32_t mask{};
};

struct ObjectPermissions : AgentMessage {
    bool override_permissions{};
    std::vector<ObjectPermissionUpdate> objects;
};

struct ObjectDuplicate : AgentMessage {
    Uuid group_id{};
    std::array<float, 3> offset{};
    std::uint32_t duplicate_flags{};
    std::vector<std::uint32_t> local_ids;
};

struct ObjectMaterialUpdate {
    std::uint32_t local_id{};
    std::uint8_t material{};
};

struct ObjectMaterial : AgentMessage {
    std::vector<ObjectMaterialUpdate> objects;
};

struct ObjectShapeUpdate {
    std::uint32_t local_id{};
    std::uint8_t path_curve{};
    std::uint8_t profile_curve{};
    std::uint16_t path_begin{};
    std::uint16_t path_end{};
    std::uint8_t path_scale_x{};
    std::uint8_t path_scale_y{};
    std::uint8_t path_shear_x{};
    std::uint8_t path_shear_y{};
    std::uint8_t path_twist{};
    std::uint8_t path_twist_begin{};
    std::uint8_t path_radius_offset{};
    std::uint8_t path_taper_x{};
    std::uint8_t path_taper_y{};
    std::uint8_t path_revolutions{};
    std::uint8_t path_skew{};
    std::uint16_t profile_begin{};
    std::uint16_t profile_end{};
    std::uint16_t profile_hollow{};
};

struct ObjectShape : AgentMessage {
    std::vector<ObjectShapeUpdate> objects;
};

struct ObjectImageUpdate {
    std::uint32_t local_id{};
    std::vector<std::byte> texture_entry;
};

struct ObjectImage : AgentMessage {
    std::vector<ObjectImageUpdate> objects;
};

struct ObjectFlagUpdate : AgentMessage {
    std::uint32_t local_id{};
    bool use_physics{};
    bool temporary{};
    bool phantom{};
    bool casts_shadows{};
    std::uint8_t physics_shape_type{};
    float density{1000.0F};
    float friction{0.6F};
    float restitution{0.5F};
    float gravity_multiplier{1.0F};
    bool has_extra_physics{};
};

struct RequestObjectPropertiesFamily : AgentMessage {
    std::uint32_t request_flags{};
    Uuid object_id{};
};

struct UuidName {
    Uuid id{};
    std::string first_name;
    std::string last_name;
};

struct MapBlockRequest : AgentMessage {
    std::uint32_t flags{};
    std::uint16_t min_x{};
    std::uint16_t max_x{};
    std::uint16_t min_y{};
    std::uint16_t max_y{};
};

struct MapNameRequest : AgentMessage {
    std::uint32_t flags{};
    std::string name;
};

struct MapBlock {
    std::uint16_t x{};
    std::uint16_t y{};
    std::string name;
    std::uint8_t access{13};
    std::uint32_t region_flags{};
    std::uint8_t water_height{20};
    std::uint8_t agents{};
    Uuid map_image_id{};
    std::uint16_t size_x{256};
    std::uint16_t size_y{256};
};

struct ObjectProperties {
    Uuid object_id{};
    Uuid creator_id{};
    Uuid owner_id{};
    std::uint32_t base_permissions{0x0009e000};
    std::uint32_t owner_permissions{0x0009e000};
    std::uint32_t group_permissions{};
    std::uint32_t everyone_permissions{};
    std::uint32_t next_owner_permissions{0x0008e000};
    std::uint32_t folded_owner_permissions{0x0009e000};
    std::uint32_t folded_next_owner_permissions{0x0008e000};
    std::uint64_t creation_date{};
    std::string name;
    std::string description;
};

struct InventoryItem {
    Uuid item_id{};
    Uuid creator_id{};
    Uuid owner_id{};
    Uuid folder_id{};
    Uuid asset_id{};
    std::int8_t asset_type{};
    std::int8_t inventory_type{};
    std::string name;
    std::string description;
    std::uint32_t flags{};
    std::uint32_t base_permissions{};
    std::uint32_t current_permissions{};
    std::uint32_t everyone_permissions{};
    std::uint32_t next_permissions{};
    std::uint8_t sale_type{};
    std::int32_t sale_price{};
    std::int32_t creation_date{};
};

struct CachedTextureQuery {
    Uuid cache_id{};
    std::uint8_t texture_index{};
    Uuid texture_id{};
};

struct AgentCachedTexture : AgentMessage {
    std::int32_t serial{};
    std::vector<CachedTextureQuery> queries;
};

struct AgentSetAppearance : AgentMessage {
    std::uint32_t serial{};
    std::array<float, 3> size{};
    std::vector<CachedTextureQuery> cache_entries;
    std::array<Uuid, 32> texture_ids{};
    std::vector<std::byte> texture_entry;
    std::vector<std::uint8_t> visual_params;
    // Server-side-appearance version to broadcast for this avatar (0 = legacy,
    // 1 = server-side). Set when the region supplies a server bake so the
    // join-backfill re-broadcasts it correctly. Not part of the wire decode.
    std::uint8_t appearance_version{};
};

struct AvatarAppearance {
    Uuid sender_id{};
    std::uint32_t serial{};
    std::vector<std::byte> texture_entry;
    std::vector<std::uint8_t> visual_params;
    std::array<float, 3> hover{};
    // AppearanceVersion: 0 = legacy (viewer composites locally); 1 = server-side
    // appearance (viewer uses the baked textures in texture_entry directly).
    // Must agree with visual param 11000 or the viewer discards the message.
    std::uint8_t appearance_version{};
};

struct AgentAnimationEntry {
    Uuid animation_id{};
    bool start{};
};

struct AgentAnimation : AgentMessage {
    std::vector<AgentAnimationEntry> animations;
};

struct AvatarAnimationEntry {
    Uuid animation_id{};
    std::int32_t sequence{};
    Uuid source_id{};
};

struct AvatarAnimation {
    Uuid sender_id{};
    std::vector<AvatarAnimationEntry> animations;
};

struct AssetUploadRequest {
    Uuid transaction_id{};
    std::int8_t asset_type{};
    bool temporary{};
    bool store_local{};
    std::vector<std::byte> data;
};

struct UpdateInventoryAsset : AgentMessage {
    Uuid item_id{};
    Uuid transaction_id{};
};

struct XferPacket {
    std::uint64_t id{};
    std::uint32_t packet{};
    std::vector<std::byte> data;
};

// Asset-transfer protocol (TransferRequest/TransferInfo/TransferPacket). The
// script and notecard editors fetch an inventory item's asset body over this
// UDP channel rather than a capability.
inline constexpr std::int32_t transfer_channel_asset = 2;         // LLTCT_ASSET
inline constexpr std::int32_t transfer_source_asset = 2;          // LLTST_ASSET
inline constexpr std::int32_t transfer_source_sim_inv_item = 3;   // LLTST_SIM_INV_ITEM
inline constexpr std::int32_t transfer_status_ok = 0;             // LLTS_OK
inline constexpr std::int32_t transfer_status_done = 1;           // LLTS_DONE
inline constexpr std::int32_t transfer_status_unknown_source = -2; // LLTS_UNKNOWN_SOURCE

// A viewer's request to fetch an asset. For a SIM_INV_ITEM source (an inventory
// script/notecard) the params carry the requesting agent, the item, and the
// asset; for an ASSET source only the asset id and type. The raw params are
// retained so the TransferInfo reply can echo them, which the viewer requires
// to route the incoming data.
struct AssetTransferRequest {
    Uuid transfer_id{};
    std::int32_t channel_type{};
    std::int32_t source_type{};
    Uuid agent_id{};   // populated for SIM_INV_ITEM
    Uuid session_id{}; // populated for SIM_INV_ITEM
    Uuid item_id{};    // populated for SIM_INV_ITEM
    Uuid asset_id{};
    std::int32_t asset_type{};
    std::vector<std::byte> params;
};

struct ImageRequestBlock {
    Uuid image_id{};
    std::int8_t discard_level{};
    float download_priority{};
    std::uint32_t packet{};
    std::uint8_t type{};
};

struct RequestImage : AgentMessage {
    std::vector<ImageRequestBlock> requests;
};

struct CompleteAgentMovement : AgentMessage {
    std::uint32_t circuit_code{};
};

struct RegionHandshake {
    std::string name{"My Region"};
    Uuid region_id{};
    Uuid owner_id{};
    float water_height{20.0F};
    std::array<Uuid, 4> terrain_textures{};
    // Per-corner low then high: the maximum height of layer 1 and the minimum
    // height of layer 4, both absolute metres. Per region, because an operator
    // sets them from the viewer's own Terrain tab; the defaults live in
    // terrain_layers.h and the caller supplies this region's live values.
    std::array<float, 4> terrain_start{terrain::layer_start_height};
    std::array<float, 4> terrain_range{terrain::layer_height_range};
    bool is_estate_owner{};
    // indra RegionFlags. Default advertises region-wide landmark creation and
    // "Set Home to Here" so those viewer menu items activate everywhere.
    std::uint32_t region_flags{(1U << 1) | (1U << 2)};
};

// ParcelPropertiesRequest (Medium 11): a (west,south,east,north) metre rectangle query.
struct ParcelPropertiesRequest {
    Uuid agent_id{};
    Uuid session_id{};
    std::int32_t sequence_id{};
    float west{};
    float south{};
    float east{};
    float north{};
    bool snap_selection{};
};

// ParcelPropertiesRequestByID (Low 197): request a specific parcel by LocalID.
struct ParcelPropertiesRequestById {
    Uuid agent_id{};
    Uuid session_id{};
    std::int32_t sequence_id{};
    std::int32_t local_id{};
};

// ParcelPropertiesUpdate (Low 198): viewer edit of a parcel's About Land options.
struct ParcelPropertiesUpdate {
    Uuid agent_id{};
    Uuid session_id{};
    std::int32_t local_id{};
    std::uint32_t flags{}; // 1 = want ParcelProperties reply
    std::uint32_t parcel_flags{};
    std::int32_t sale_price{};
    std::string name;
    std::string description;
    std::string music_url;
    std::string media_url;
    Uuid media_id{};
    std::uint8_t media_auto_scale{};
    Uuid group_id{};
    std::int32_t pass_price{};
    float pass_hours{};
    std::uint8_t category{};
    Uuid auth_buyer_id{};
    Uuid snapshot_id{};
    std::array<float, 3> user_location{};
    std::array<float, 3> user_look_at{};
    std::uint8_t landing_type{};
};

// ParcelDivide (Low 211) / ParcelJoin (Low 210): metre rectangle plus agent identity.
struct ParcelRectRequest {
    Uuid agent_id{};
    Uuid session_id{};
    float west{};
    float south{};
    float east{};
    float north{};
};

// ParcelAccessListRequest (Low 215).
struct ParcelAccessListRequest {
    Uuid agent_id{};
    Uuid session_id{};
    std::int32_t sequence_id{};
    std::uint32_t flags{};
    std::int32_t local_id{};
};

struct ParcelAccessListEntry {
    Uuid id{};
    std::int32_t time{};
    std::uint32_t flags{};
};

// ParcelAccessListUpdate (Low 217).
struct ParcelAccessListUpdate {
    Uuid agent_id{};
    Uuid session_id{};
    std::uint32_t flags{};
    std::int32_t local_id{};
    Uuid transaction_id{};
    std::int32_t sequence_id{};
    std::int32_t sections{};
    std::vector<ParcelAccessListEntry> entries;
};

// ParcelAccessListReply (Low 216): sim -> viewer.
struct ParcelAccessListReply {
    Uuid agent_id{};
    std::int32_t sequence_id{};
    std::uint32_t flags{};
    std::int32_t local_id{};
    std::vector<ParcelAccessListEntry> entries;
};

// ParcelObjectOwnersRequest (Low 56).
struct ParcelObjectOwnersRequest {
    Uuid agent_id{};
    Uuid session_id{};
    std::int32_t local_id{};
};

// ParcelObjectOwnersReply (Low 57): one entry per distinct object owner on a parcel.
struct ParcelObjectOwner {
    Uuid owner_id{};
    bool is_group_owned{};
    std::int32_t count{};
    bool online{};
};

// ParcelSelectObjects (Low 202): highlight owner/group/other/listed objects.
struct ParcelSelectObjects {
    Uuid agent_id{};
    Uuid session_id{};
    std::int32_t local_id{};
    std::uint32_t return_type{};
    std::vector<Uuid> return_ids;
};

// ParcelReturnObjects (Low 199): return objects on a parcel by type or explicit list.
struct ParcelReturnObjects {
    Uuid agent_id{};
    Uuid session_id{};
    std::int32_t local_id{};
    std::uint32_t return_type{};
    std::vector<Uuid> task_ids;
    std::vector<Uuid> owner_ids;
};

// ObjectReturnType bitfield (OpenMetaverse ObjectReturnType).
inline constexpr std::uint32_t object_return_owner = 1U << 1;
inline constexpr std::uint32_t object_return_group = 1U << 2;
inline constexpr std::uint32_t object_return_other = 1U << 3;
inline constexpr std::uint32_t object_return_list = 1U << 4;

// RequestRegionInfo (Low 141): opens the Region/Estate floater.
struct RequestRegionInfo {
    Uuid agent_id{};
    Uuid session_id{};
};

// RegionInfo (Low 142): sim -> viewer, populates the Region tab.
struct RegionInfoReply {
    Uuid agent_id{};
    Uuid session_id{};
    std::string sim_name;
    std::uint32_t estate_id{};
    std::uint32_t parent_estate_id{};
    std::uint32_t region_flags{};
    std::uint8_t sim_access{13};
    std::uint8_t max_agents{40};
    float billable_factor{};
    float object_bonus_factor{1.0F};
    float water_height{20.0F};
    float terrain_raise_limit{100.0F};
    float terrain_lower_limit{-100.0F};
    std::int32_t price_per_meter{};
    std::int32_t redirect_grid_x{};
    std::int32_t redirect_grid_y{};
    bool use_estate_sun{true};
    float sun_hour{};
    std::string product_sku{"Homeworldz"};
    std::string product_name{"Homeworldz Region"};
    std::uint64_t region_flags_extended{};
};

// EstateCovenantReply (Low 204): sim -> viewer, fills the About Land Covenant tab.
struct EstateCovenantReply {
    Uuid covenant_id{};
    std::uint32_t timestamp{};
    std::string estate_name;
    Uuid estate_owner_id{};
};

// EstateOwnerMessage (Low 260): both directions. Method + invoice + string params.
struct EstateOwnerMessage {
    Uuid agent_id{};
    Uuid session_id{};
    Uuid transaction_id{};
    std::string method;
    Uuid invoice{};
    std::vector<std::string> params;
};

// estateaccessdelta command bits (Halcyon EstateAccessDeltaCommands).
inline constexpr std::uint32_t estate_access_add_allowed = 4;
inline constexpr std::uint32_t estate_access_remove_allowed = 8;
inline constexpr std::uint32_t estate_access_add_group = 16;
inline constexpr std::uint32_t estate_access_remove_group = 32;
inline constexpr std::uint32_t estate_access_ban_user = 64;
inline constexpr std::uint32_t estate_access_unban_user = 128;
inline constexpr std::uint32_t estate_access_add_manager = 256;
inline constexpr std::uint32_t estate_access_remove_manager = 512;
inline constexpr std::uint32_t estate_access_no_reply = 1024;

// setaccess reply list bits (indra ESTATE_ACCESS_*).
inline constexpr std::uint32_t estate_list_allowed_agents = 1U << 0;
inline constexpr std::uint32_t estate_list_allowed_groups = 1U << 1;
inline constexpr std::uint32_t estate_list_banned_agents = 1U << 2;
inline constexpr std::uint32_t estate_list_managers = 1U << 3;

// estatechangeinfo param1 flag bits (Halcyon handleEstateChangeInfo).
inline constexpr std::uint32_t estate_flag_fixed_sun = 0x00000010;
inline constexpr std::uint32_t estate_flag_public_access = 0x00008000;
inline constexpr std::uint32_t estate_flag_allow_direct_teleport = 0x00100000;
inline constexpr std::uint32_t estate_flag_deny_anonymous = 0x00800000;
inline constexpr std::uint32_t estate_flag_deny_identified = 0x01000000;
inline constexpr std::uint32_t estate_flag_deny_transacted = 0x02000000;
inline constexpr std::uint32_t estate_flag_allow_voice = 0x10000000;
inline constexpr std::uint32_t estate_flag_deny_minors = 0x40000000;

struct AgentMovementComplete : AgentMessage {
    std::array<float, 3> position{128.0F, 128.0F, 25.0F};
    std::array<float, 3> look_at{1.0F, 0.0F, 0.0F};
    std::uint64_t region_handle{};
    std::uint32_t timestamp{};
    std::string channel_version{"Homeworldz dev"};
};

struct AgentUpdate : AgentMessage {
    std::array<float, 3> body_rotation{};
    std::array<float, 3> head_rotation{};
    std::uint8_t state{};
    std::array<float, 3> camera_center{};
    std::array<float, 3> camera_at{};
    std::array<float, 3> camera_left{};
    std::array<float, 3> camera_up{};
    float draw_distance{};
    std::uint32_t control_flags{};
    std::uint8_t flags{};
};

struct ModifyLandArea {
    std::int32_t local_id{};
    float west{};
    float south{};
    float east{};
    float north{};
};

struct ModifyLand : AgentMessage {
    std::uint8_t action{};
    std::uint8_t brush_size{};
    float seconds{};
    float height{};
    std::vector<ModifyLandArea> areas;
    std::vector<float> extended_brush_sizes;
};

// ChatFromViewer chat types, matching the viewer's own numbering.
inline constexpr std::uint8_t chat_type_whisper = 0;
inline constexpr std::uint8_t chat_type_normal = 1;
inline constexpr std::uint8_t chat_type_shout = 2;

// Audible radius in metres for each chat type, matching the Second Life
// defaults. These are both enforced when a chat message is relayed and
// advertised in SimulatorFeatures, so a viewer's chat interface agrees with what
// the region actually delivers; they must stay a single source for both.
inline constexpr double chat_whisper_range = 10.0;
inline constexpr double chat_say_range = 20.0;
inline constexpr double chat_shout_range = 100.0;

// Audible radius for a chat type. An unrecognized type is treated as normal
// speech, which is the conservative choice: never louder than the viewer asked.
constexpr double chat_range(std::uint8_t chat_type) {
    if (chat_type == chat_type_whisper) return chat_whisper_range;
    if (chat_type == chat_type_shout) return chat_shout_range;
    return chat_say_range;
}

struct ChatFromViewer : AgentMessage {
    std::string message;
    std::uint8_t type{};
    std::int32_t channel{};
};

struct ChatFromSimulator {
    std::string from_name;
    Uuid source_id{};
    Uuid owner_id{};
    std::uint8_t source_type{1};
    std::uint8_t chat_type{1};
    std::uint8_t audible{1};
    std::array<float, 3> position{};
    std::string message;
};

struct TerrainPatch {
    std::uint8_t x{};
    std::uint8_t y{};
};

struct StaticObject {
    // A mesh or sculpted prim's shaping asset, carried to viewers in the
    // ObjectUpdate ExtraParams sculpt block (type 5 = mesh, ADR 0033). A zero
    // id emits no extra params, exactly as before.
    Uuid sculpt_id{};
    std::uint8_t sculpt_type{};
    std::uint32_t local_id{1};
    std::uint32_t parent_local_id{};
    // ObjectUpdate's State byte. Zero for ordinary prims; for an attachment it
    // carries the attachment point, which is how a viewer knows to draw the
    // object on its parent avatar rather than floating at the avatar's origin.
    // The point occupies the low nibble and the high nibble together, packed as
    // the viewer's ATTACHMENT_ADD-free form: point | (point >> 4).
    std::uint8_t state{};
    Uuid id{};
    Uuid owner_id{};
    std::uint32_t update_flags{};
    std::uint8_t pcode{9};
    std::uint8_t material{3};
    std::array<float, 3> position{132.0F, 128.0F, 26.0F};
    std::array<float, 3> velocity{};
    std::array<float, 3> acceleration{};
    std::array<float, 3> rotation{};
    std::array<float, 3> scale{2.0F, 2.0F, 2.0F};
    std::vector<std::byte> texture_entry;
    std::uint8_t path_curve{0x10};
    std::uint8_t profile_curve{0x01};
    std::uint16_t path_begin{};
    std::uint16_t path_end{};
    std::uint8_t path_scale_x{100};
    std::uint8_t path_scale_y{100};
    std::uint8_t path_shear_x{};
    std::uint8_t path_shear_y{};
    std::uint8_t path_twist{};
    std::uint8_t path_twist_begin{};
    std::uint8_t path_radius_offset{};
    std::uint8_t path_taper_x{};
    std::uint8_t path_taper_y{};
    std::uint8_t path_revolutions{};
    std::uint8_t path_skew{};
    std::uint16_t profile_begin{};
    std::uint16_t profile_end{};
    std::uint16_t profile_hollow{};
};

// Builds the canonical TextureEntry defaults for a newly created primitive:
// white tint, 1x repeats, zero offsets/rotation, and no per-face overrides.
std::vector<std::byte> default_texture_entry(const Uuid& texture_id);

// Replaces an absent, null, or viewer-local fallback default face with the
// supplied server-backed default while preserving valid face parameters.
bool normalize_primitive_texture_entry(
    std::vector<std::byte>& texture_entry, std::span<const std::byte> default_entry);

// Unpacks the TextureID section of a TextureEntry blob into 32 per-face UUIDs
// (the default fills faces without an explicit override). Returns nullopt if the
// blob is shorter than the 16-byte default or is malformed.
std::optional<std::array<Uuid, 32>> unpack_texture_entry_faces(
    std::span<const std::byte> texture_entry);

// Builds a full avatar TextureEntry wire blob from 32 per-face texture UUIDs.
// Faces equal to default_id are emitted only via the section default; the other
// attribute sections (color, repeats, offsets, rotation, material, glow,
// material id) use the same canonical defaults as default_texture_entry. This
// is the inverse of unpack_texture_entry_faces for the TextureID section and is
// used to assemble a server-baked avatar appearance.
std::vector<std::byte> encode_avatar_texture_entry(const std::array<Uuid, 32>& faces,
                                                   const Uuid& default_id);

std::vector<std::byte> encode_use_circuit_code(const UseCircuitCode& message);
std::optional<UseCircuitCode> decode_use_circuit_code(std::span<const std::byte> payload);
std::optional<TeleportLocationRequest> decode_teleport_location_request(
    std::span<const std::byte> payload);
std::optional<TeleportLandmarkRequest> decode_teleport_landmark_request(
    std::span<const std::byte> payload);
std::optional<SetStartLocationRequest> decode_set_start_location_request(
    std::span<const std::byte> payload);
std::optional<ActivateGestures> decode_activate_gestures(std::span<const std::byte> payload);
std::optional<DeactivateGestures> decode_deactivate_gestures(std::span<const std::byte> payload);
std::vector<std::byte> encode_teleport_start(const TeleportStart& message);
std::vector<std::byte> encode_teleport_local(const TeleportLocal& message);
std::vector<std::byte> encode_teleport_failed(const TeleportFailed& message);
std::vector<std::byte> encode_region_handshake(const RegionHandshake& message);
std::optional<AgentMessage> decode_region_handshake_reply(std::span<const std::byte> payload);
std::optional<CompleteAgentMovement> decode_complete_agent_movement(std::span<const std::byte> payload);
std::vector<std::byte> encode_agent_movement_complete(const AgentMovementComplete& message);
std::vector<std::byte> encode_start_ping_check(std::uint8_t ping_id, std::uint32_t oldest_unacked = 0);
std::optional<std::uint8_t> decode_start_ping_check(std::span<const std::byte> payload);
std::vector<std::byte> encode_complete_ping_check(std::uint8_t ping_id);
// Decode a viewer's CompletePingCheck (pong) reply; returns the echoed ping id.
std::optional<std::uint8_t> decode_complete_ping_check(std::span<const std::byte> payload);
// Force a viewer to log out, displaying `reason`. Used for graceful shutdown and
// connection-loss retirement so the viewer shows a clear message, not a generic
// disconnect.
std::vector<std::byte> encode_kick_user(const Uuid& agent_id, const Uuid& session_id,
                                        std::string_view reason);
bool is_economy_data_request(std::span<const std::byte> payload);
std::vector<std::byte> encode_economy_data(std::int32_t price_upload = 0,
                                           std::int32_t object_capacity = 15000,
                                           std::int32_t object_count = 0);
std::optional<AgentMessage> decode_logout_request(std::span<const std::byte> payload);
std::optional<CreateInventoryFolder> decode_create_inventory_folder(std::span<const std::byte> payload);
std::optional<CreateInventoryItem> decode_create_inventory_item(std::span<const std::byte> payload);
std::optional<CopyInventoryItem> decode_copy_inventory_item(std::span<const std::byte> payload);
std::optional<MoveInventoryFolder> decode_move_inventory_folder(std::span<const std::byte> payload);
std::optional<MoveInventoryItem> decode_move_inventory_item(std::span<const std::byte> payload);
std::optional<RequestTaskInventory> decode_request_task_inventory(std::span<const std::byte> payload);
std::vector<std::byte> encode_reply_task_inventory(const ReplyTaskInventory& message);
std::optional<UpdateTaskInventory> decode_update_task_inventory(std::span<const std::byte> payload);
std::optional<RezScript> decode_rez_script(std::span<const std::byte> payload);
std::optional<RemoveTaskInventory> decode_remove_task_inventory(std::span<const std::byte> payload);
std::optional<MoveTaskInventory> decode_move_task_inventory(std::span<const std::byte> payload);
std::optional<RequestXfer> decode_request_xfer(std::span<const std::byte> payload);
std::vector<std::byte> encode_send_xfer_packet(
    std::uint64_t id, std::uint32_t packet, std::span<const std::byte> data);
std::optional<AssetTransferRequest> decode_transfer_request(std::span<const std::byte> payload);
std::vector<std::byte> encode_transfer_info(
    const Uuid& transfer_id, std::int32_t channel_type, std::int32_t status,
    std::int32_t size, std::span<const std::byte> params);
std::vector<std::byte> encode_transfer_packet(
    const Uuid& transfer_id, std::int32_t channel_type, std::int32_t packet,
    std::int32_t status, std::span<const std::byte> data);
std::optional<ObjectAdd> decode_object_add(std::span<const std::byte> payload);
std::optional<DeRezObject> decode_derez_object(std::span<const std::byte> payload);
bool valid_derez_batch(std::uint8_t packet_count, std::uint8_t packet_number);
std::optional<RezObject> decode_rez_object(std::span<const std::byte> payload);
std::vector<std::byte> encode_kill_object(std::span<const std::uint32_t> local_ids);
std::optional<ObjectSelect> decode_object_select(std::span<const std::byte> payload);
std::optional<ObjectSelect> decode_object_deselect(std::span<const std::byte> payload);
std::optional<ObjectSelect> decode_object_link(std::span<const std::byte> payload);
std::optional<ObjectSelect> decode_object_delink(std::span<const std::byte> payload);
std::optional<ObjectGrab> decode_object_grab(std::span<const std::byte> payload);
std::optional<ObjectGrabUpdate> decode_object_grab_update(std::span<const std::byte> payload);
std::optional<MultipleObjectUpdate> decode_multiple_object_update(std::span<const std::byte> payload);
std::optional<ObjectName> decode_object_name(std::span<const std::byte> payload);
std::optional<ObjectDescription> decode_object_description(std::span<const std::byte> payload);
std::optional<ObjectPermissions> decode_object_permissions(std::span<const std::byte> payload);
std::optional<ObjectDuplicate> decode_object_duplicate(std::span<const std::byte> payload);
std::optional<ObjectMaterial> decode_object_material(std::span<const std::byte> payload);
std::optional<ObjectShape> decode_object_shape(std::span<const std::byte> payload);
std::optional<ObjectImage> decode_object_image(std::span<const std::byte> payload);
std::optional<ObjectFlagUpdate> decode_object_flag_update(std::span<const std::byte> payload);
std::optional<RequestObjectPropertiesFamily> decode_request_object_properties_family(
    std::span<const std::byte> payload);
std::vector<std::byte> encode_object_properties(std::span<const ObjectProperties> objects);
std::vector<std::byte> encode_object_properties_family(
    std::uint32_t request_flags, const ObjectProperties& object);
std::optional<std::vector<Uuid>> decode_uuid_name_request(std::span<const std::byte> payload);
std::vector<std::byte> encode_uuid_name_reply(std::span<const UuidName> names);
std::optional<MapBlockRequest> decode_map_block_request(std::span<const std::byte> payload);
std::optional<MapNameRequest> decode_map_name_request(std::span<const std::byte> payload);
std::vector<std::byte> encode_map_block_reply(const Uuid& agent_id, std::uint32_t flags,
                                               std::span<const MapBlock> regions);
std::vector<std::byte> encode_update_create_inventory_item(const AgentMessage& message,
                                                           std::uint32_t callback_id,
                                                           const InventoryItem& item);
std::vector<std::byte> encode_logout_reply(const AgentMessage& message);
std::optional<AgentCachedTexture> decode_agent_cached_texture(std::span<const std::byte> payload);
std::vector<std::byte> encode_agent_cached_texture_response(const AgentCachedTexture& message);
std::optional<AgentSetAppearance> decode_agent_set_appearance(std::span<const std::byte> payload);
std::vector<std::byte> encode_avatar_appearance(const AvatarAppearance& message);
std::optional<AgentAnimation> decode_agent_animation(std::span<const std::byte> payload);
std::vector<std::byte> encode_avatar_animation(const AvatarAnimation& message);
std::optional<AssetUploadRequest> decode_asset_upload_request(std::span<const std::byte> payload);
std::vector<std::byte> encode_asset_upload_complete(const Uuid& asset_id,
                                                    std::int8_t asset_type, bool success);
std::optional<UpdateInventoryAsset> decode_update_inventory_asset(std::span<const std::byte> payload);
std::vector<std::byte> encode_request_xfer(std::uint64_t id, const Uuid& asset_id,
                                           std::int16_t asset_type);
std::optional<XferPacket> decode_send_xfer_packet(std::span<const std::byte> payload);
std::vector<std::byte> encode_confirm_xfer_packet(std::uint64_t id, std::uint32_t packet);
std::optional<XferPacket> decode_confirm_xfer_packet(std::span<const std::byte> payload);
std::optional<RequestImage> decode_request_image(std::span<const std::byte> payload);
std::vector<std::vector<std::byte>> encode_image_transfer(
    const Uuid& image_id, std::span<const std::byte> content, std::uint32_t start_packet = 0);
std::optional<AgentUpdate> decode_agent_update(std::span<const std::byte> payload);
std::optional<ModifyLand> decode_modify_land(std::span<const std::byte> payload);
std::optional<ParcelPropertiesRequest> decode_parcel_properties_request(
    std::span<const std::byte> payload);
std::optional<ParcelPropertiesRequestById> decode_parcel_properties_request_by_id(
    std::span<const std::byte> payload);
std::optional<ParcelPropertiesUpdate> decode_parcel_properties_update(
    std::span<const std::byte> payload);
std::optional<ParcelRectRequest> decode_parcel_divide(std::span<const std::byte> payload);
std::optional<ParcelRectRequest> decode_parcel_join(std::span<const std::byte> payload);
std::optional<ParcelAccessListRequest> decode_parcel_access_list_request(
    std::span<const std::byte> payload);
std::optional<ParcelAccessListUpdate> decode_parcel_access_list_update(
    std::span<const std::byte> payload);
std::vector<std::byte> encode_parcel_access_list_reply(const ParcelAccessListReply& message);
// Pack per-cell overlay bytes (row-major, x inner) into ParcelOverlay packets,
// 1024 cells per packet, with an incrementing SequenceID. One or more packets.
std::vector<std::vector<std::byte>> encode_parcel_overlay(std::span<const std::uint8_t> cells);
std::optional<ParcelObjectOwnersRequest> decode_parcel_object_owners_request(
    std::span<const std::byte> payload);
std::vector<std::byte> encode_parcel_object_owners_reply(
    std::span<const ParcelObjectOwner> owners);
std::optional<ParcelSelectObjects> decode_parcel_select_objects(std::span<const std::byte> payload);
std::vector<std::vector<std::byte>> encode_force_object_select(std::span<const std::uint32_t> local_ids);
std::optional<ParcelReturnObjects> decode_parcel_return_objects(std::span<const std::byte> payload);
std::optional<RequestRegionInfo> decode_request_region_info(std::span<const std::byte> payload);
std::vector<std::byte> encode_agent_alert_message(const Uuid& agent_id, bool modal,
                                                  std::string_view message);
std::optional<AgentMessage> decode_estate_covenant_request(std::span<const std::byte> payload);
std::vector<std::byte> encode_estate_covenant_reply(const EstateCovenantReply& message);
std::vector<std::byte> encode_region_info(const RegionInfoReply& message);
std::optional<EstateOwnerMessage> decode_estate_owner_message(std::span<const std::byte> payload);
std::vector<std::byte> encode_estate_owner_message(const Uuid& agent_id, const Uuid& invoice,
                                                   std::string_view method,
                                                   std::span<const std::string> params);
std::optional<ChatFromViewer> decode_chat_from_viewer(std::span<const std::byte> payload);
std::vector<std::byte> encode_chat_from_simulator(const ChatFromSimulator& message);
std::vector<std::byte> encode_flat_terrain(std::span<const TerrainPatch> patches, float height);
std::vector<std::byte> encode_terrain(std::span<const TerrainPatch> patches,
                                      std::span<const float> heightmap);
std::vector<std::byte> encode_static_object_update(std::uint64_t region_handle,
                                                   const StaticObject& object);
std::vector<std::byte> encode_avatar_object_update(std::uint64_t region_handle, std::uint32_t local_id,
                                                   const Uuid& agent_id,
                                                   std::array<float, 3> position,
                                                   std::array<float, 3> velocity = {},
                                                   std::array<float, 3> rotation = {});
std::vector<std::byte> encode_packet_ack(std::span<const std::uint32_t> sequences);
std::optional<std::vector<std::uint32_t>> decode_packet_ack(std::span<const std::byte> payload);

std::vector<std::byte> encode_packet(const Packet& packet);
std::optional<Packet> decode_packet(std::span<const std::byte> datagram);

class Circuit {
public:
    using Clock = std::chrono::steady_clock;

    explicit Circuit(Clock::time_point now, double bytes_per_second = 128000,
                     std::chrono::seconds idle_timeout = std::chrono::seconds(30));

    std::optional<std::vector<std::byte>> send(std::vector<std::byte> payload, bool reliable,
                                               Clock::time_point now, bool zero_coded = false);
    std::optional<Packet> receive(std::span<const std::byte> datagram, Clock::time_point now);
    std::vector<std::vector<std::byte>> poll(Clock::time_point now);
    bool expired(Clock::time_point now) const;
    std::size_t pending_reliable() const { return pending_.size(); }

private:
    struct Pending {
        Packet packet;
        Clock::time_point sent_at;
        unsigned attempts{1};
    };

    bool consume(std::size_t bytes, Clock::time_point now);
    std::vector<std::uint32_t> take_acks();

    std::uint32_t next_sequence_{1};
    std::unordered_map<std::uint32_t, Pending> pending_;
    std::unordered_set<std::uint32_t> received_reliable_;
    std::vector<std::uint32_t> queued_acks_;
    Clock::time_point last_activity_;
    Clock::time_point token_time_;
    double rate_;
    double capacity_;
    double tokens_;
    std::chrono::seconds idle_timeout_;
};

struct OutboundDatagram {
    std::string endpoint;
    std::vector<std::byte> bytes;
};

struct ReplacedCircuit {
    std::string endpoint;
    UseCircuitCode identity;
};

class CircuitRegistry {
public:
    using Clock = Circuit::Clock;
    using Authorizer = std::function<bool(const UseCircuitCode&)>;

    explicit CircuitRegistry(Authorizer authorizer) : authorizer_(std::move(authorizer)) {}
    std::optional<Packet> receive(std::string_view endpoint, std::span<const std::byte> datagram,
                                  Clock::time_point now);
    std::optional<std::vector<std::byte>> send(std::string_view endpoint, std::vector<std::byte> payload,
                                               bool reliable, Clock::time_point now, bool zero_coded = false);
    std::vector<OutboundDatagram> poll(Clock::time_point now);
    std::vector<ReplacedCircuit> take_replaced();
    const UseCircuitCode* identity(std::string_view endpoint) const;
    bool remove(std::string_view endpoint);
    std::size_t size() const { return circuits_.size(); }

private:
    struct Entry {
        UseCircuitCode identity;
        Circuit circuit;
    };

    Authorizer authorizer_;
    std::unordered_map<std::string, Entry> circuits_;
    std::vector<ReplacedCircuit> replaced_;
};

} // namespace homeworldz::viewer
