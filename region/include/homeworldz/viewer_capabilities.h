#pragma once

#include "homeworldz/region_extensions.h"

#include <cstdint>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace homeworldz::viewer {

inline constexpr std::uint32_t teleport_flags_via_location = 0x00000010U;
inline constexpr std::uint32_t teleport_flags_is_flying = 0x00002000U;

struct EstablishAgentCommunication {
    std::string agent_id;
    std::string simulator_endpoint;
    std::string seed_capability;
};

struct SimulatorEventEndpoint {
    std::array<std::uint8_t, 4> address{};
    std::uint16_t port{};
};

struct TeleportFinish {
    std::string agent_id;
    std::uint64_t region_handle{};
    SimulatorEventEndpoint simulator;
    std::string seed_capability;
    std::uint8_t simulator_access{};
    std::uint32_t teleport_flags{teleport_flags_via_location};
    std::uint32_t region_size_x{256};
    std::uint32_t region_size_y{256};
};

struct CrossedRegion {
    std::string agent_id;
    std::string session_id;
    std::uint64_t region_handle{};
    SimulatorEventEndpoint simulator;
    std::string seed_capability;
    std::array<float, 3> position{};
    std::array<float, 3> look_at{};
    std::uint32_t region_size_x{256};
    std::uint32_t region_size_y{256};
};

struct NewFileInventoryUpload {
    std::string folder_id;
    std::int8_t asset_type{-1};
    std::int8_t inventory_type{-1};
    std::string name;
    std::string description;
    std::uint32_t everyone_permissions{};
    std::uint32_t group_permissions{};
    std::uint32_t next_permissions{0x7fffffff};
};

struct InventoryAssetUpdate {
    std::string item_id;
    std::string target;
    std::string task_id;
    bool script_running{};
};

// The ParcelProperties event, delivered to the viewer over the Event Queue as
// LLSD (matching Halcyon/OpenSim). Field names mirror the ParcelProperties
// message so About Land, "Landmark This Place", and parcel selection populate.
struct ParcelPropertiesEvent {
    std::int32_t request_result{};
    std::int32_t sequence_id{};
    bool snap_selection{};
    std::int32_t self_count{};
    std::int32_t other_count{};
    std::int32_t public_count{};
    std::int32_t local_id{};
    std::string owner_id;
    bool is_group_owned{};
    std::uint32_t auction_id{};
    std::int32_t claim_date{};
    std::int32_t claim_price{};
    std::int32_t rent_price{};
    std::array<float, 3> aabb_min{};
    std::array<float, 3> aabb_max{};
    std::vector<std::uint8_t> bitmap;
    std::int32_t area{};
    std::uint8_t status{};
    std::int32_t sim_wide_max_prims{};
    std::int32_t sim_wide_total_prims{};
    std::int32_t max_prims{};
    std::int32_t total_prims{};
    std::int32_t owner_prims{};
    std::int32_t group_prims{};
    std::int32_t other_prims{};
    std::int32_t selected_prims{};
    float parcel_prim_bonus{1.0F};
    std::int32_t other_clean_time{};
    std::uint32_t parcel_flags{};
    std::int32_t sale_price{};
    std::string name;
    std::string description;
    std::string music_url;
    std::string media_url;
    std::string media_id;
    std::uint8_t media_auto_scale{};
    std::string group_id;
    std::int32_t pass_price{};
    float pass_hours{};
    std::uint8_t category{};
    std::string auth_buyer_id;
    std::string snapshot_id;
    std::array<float, 3> user_location{};
    std::array<float, 3> user_look_at{};
    std::uint8_t landing_type{2};
    bool region_push_override{};
    bool region_deny_anonymous{};
    bool region_deny_identified{};
    bool region_deny_transacted{};
    bool region_deny_age_unverified{};
    std::string media_type{"none/none"};
    std::string media_desc;
    std::int32_t media_width{};
    std::int32_t media_height{};
    bool media_loop{};
    bool obscure_media{};
    bool obscure_music{};
};

// Build the seed capability reply. The baseline capability set is always served,
// unchanged; `extension_capabilities` are the additional ones a client negotiated
// under ADR 0032 and is empty for a viewer that asked for none, which makes the
// reply byte-identical to the pre-extension one.
std::string seed_capability_xml(std::string_view public_endpoint, std::string_view grid_public_endpoint,
                                std::string_view session_id, std::string_view visit_id = {},
                                const std::vector<ExtensionCapability>& extension_capabilities = {});
// Extract the capability names a viewer's seed request asks for. The body is an
// LLSD array of strings; an absent or unparseable body yields none, so a client
// that requests nothing negotiates nothing.
std::vector<std::string> parse_requested_capabilities(std::string_view xml);
std::string establish_agent_communication_event_xml(const EstablishAgentCommunication& event);
std::string enable_simulator_event_xml(std::uint64_t region_handle,
                                       const SimulatorEventEndpoint& simulator,
                                       std::uint32_t region_size_x = 256,
                                       std::uint32_t region_size_y = 256);
std::string teleport_finish_event_xml(const TeleportFinish& event);
std::string crossed_region_event_xml(const CrossedRegion& event);
std::string parcel_properties_event_xml(const ParcelPropertiesEvent& event);
// The Extra Physics values the viewer's Features tab displays for one object.
// The region is the authority for these; the viewer has no other source, and
// treats what it last received as the current state when the creator edits any
// one of them.
struct ObjectPhysicsProperties {
    std::uint32_t local_id{};
    std::uint8_t physics_shape_type{};
    double density{1000.0};
    double friction{0.6};
    double restitution{0.5};
    double gravity_multiplier{1.0};
};

std::string object_physics_properties_event_xml(const ObjectPhysicsProperties& properties);
std::string event_queue_xml(std::uint64_t id, const std::vector<std::string>& events = {});
// What the region tells a viewer it supports, served through the
// `SimulatorFeatures` capability.
//
// A viewer enables interface on the strength of these flags, so every one must
// reflect behavior the region actually implements. Advertising an unimplemented
// feature produces controls that silently do nothing, and withholding an
// implemented one hides working behavior — the defaults below are therefore the
// single declaration of what this region software supports, and each is stated
// against the code that backs it.
struct SimulatorFeatures {
    std::string currency{"C$"};
    std::string map_server_url;

    // Physics shape types offered in the viewer's Features tab. Prim is the
    // default shape; None is honored by excluding the entity from the collision
    // mirror. Convex Hull is deliberately false: the value is accepted, clamped,
    // and persisted, but the collision shape is selected from the prim's own
    // shape, so a viewer offering it would be offering a choice the region
    // silently ignores.
    bool physics_shape_prim{true};
    bool physics_shape_none{true};
    bool physics_shape_convex{false};
    // Density, friction, and restitution are decoded from ObjectFlagUpdate,
    // persisted, and applied to the Jolt body.
    bool physics_materials{true};

    // Mesh rez and transfer are live (ADR 0033 M1: GLB upload, sl-mesh
    // renditions, ranged serving, the mesh ExtraParams block). This flag is
    // load-bearing for RENDERING, not just UI: a viewer honors
    // MeshRezEnabled=false by never building render volumes for mesh
    // objects — they fetch, parse, and select, but draw nothing. Found the
    // hard way, 2026-07-29. Viewer-side mesh upload is the mesh branch of
    // NewFileAgentInventory plus the upload-model-data route (ADR 0033 M2).
    bool mesh{true};
    bool mesh_upload{true};
    bool dynamic_pathfinding{false};
    // The region emits a hover-height block in AvatarAppearance but accepts no
    // hover-height update, so the viewer must not offer the control.
    bool avatar_hover_height{false};
    // The Export permission bit is enforced in the permission core, including
    // folding across linksets and task inventory.
    bool export_supported{true};

    // Negotiated region extensions (ADR 0032); empty until one is implemented.
    std::vector<RegionExtension> extensions;
};

// Build the SimulatorFeatures reply. Legacy viewers ignore keys they do not
// know, including the Homeworldz extension map.
std::string simulator_features_xml(const SimulatorFeatures& features);
std::string environment_settings_xml(std::string_view region_id);
// Build the RemoteParcelRequest reply carrying a parcel's global UUID.
std::string remote_parcel_reply_xml(std::string_view parcel_id);
// Extract the {x,y,z} location from a RemoteParcelRequest LLSD body, if present.
std::optional<std::array<double, 3>> parse_remote_parcel_location(std::string_view xml);
// Extract the region_handle from a RemoteParcelRequest LLSD body, if present.
// Firestorm quantizes it to the requested point's own 256 m tile, so on a
// rectangular region it carries the facet-to-macro shift the location needs
// (ADR 0036). Sent as an 8-byte big-endian LLSD binary.
std::optional<std::uint64_t> parse_remote_parcel_handle(std::string_view xml);
std::string baked_texture_upload_xml(std::string_view uploader);
std::string baked_texture_complete_xml(std::string_view asset_id);
std::optional<NewFileInventoryUpload> parse_new_file_inventory_upload(std::string_view xml);
bool valid_new_file_inventory_upload_content(const NewFileInventoryUpload& upload,
                                             std::string_view content);
std::optional<InventoryAssetUpdate> parse_inventory_asset_update(std::string_view xml);
std::string inventory_asset_update_upload_xml(std::string_view uploader);
std::string inventory_asset_update_complete_xml(
    std::string_view asset_id, bool script, bool compiled = false,
    std::string_view diagnostic = {});
std::string new_file_inventory_upload_xml(std::string_view uploader);
std::string new_file_inventory_complete_xml(std::string_view item_id, std::string_view asset_id,
                                            std::uint32_t everyone_permissions,
                                            std::uint32_t next_permissions);
std::string random_uuid();

} // namespace homeworldz::viewer
