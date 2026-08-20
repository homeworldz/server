#include "homeworldz/viewer_capabilities.h"

#include "homeworldz/viewer_protocol.h"

#include <array>
#include <charconv>
#include <ctime>
#include <random>
#include <span>

namespace homeworldz::viewer {
namespace {
std::string xml_escape(std::string_view value) {
    std::string result;
    for (const char character : value) {
        if (character == '&') result += "&amp;";
        else if (character == '<') result += "&lt;";
        else if (character == '>') result += "&gt;";
        else if (character == '\"') result += "&quot;";
        else if (character == '\'') result += "&apos;";
        else result.push_back(character);
    }
    return result;
}

std::string xml_unescape(std::string_view value) {
    std::string result;
    for (std::size_t index = 0; index < value.size();) {
        if (value.substr(index).starts_with("&amp;")) {
            result.push_back('&');
            index += 5;
        } else if (value.substr(index).starts_with("&lt;")) {
            result.push_back('<');
            index += 4;
        } else if (value.substr(index).starts_with("&gt;")) {
            result.push_back('>');
            index += 4;
        } else if (value.substr(index).starts_with("&quot;")) {
            result.push_back('"');
            index += 6;
        } else if (value.substr(index).starts_with("&apos;")) {
            result.push_back('\'');
            index += 6;
        } else {
            result.push_back(value[index++]);
        }
    }
    return result;
}

std::optional<std::string> llsd_value(std::string_view xml, std::string_view key) {
    const auto marker = "<key>" + std::string(key) + "</key>";
    const auto found = xml.find(marker);
    if (found == std::string_view::npos) return std::nullopt;
    auto position = found + marker.size();
    while (position < xml.size() && (xml[position] == ' ' || xml[position] == '\r' ||
           xml[position] == '\n' || xml[position] == '\t')) ++position;
    for (const std::string_view tag : {"string", "uuid", "integer", "boolean"}) {
        const auto open = '<' + std::string(tag) + '>';
        if (!xml.substr(position).starts_with(open)) continue;
        position += open.size();
        const auto close = "</" + std::string(tag) + '>';
        const auto end = xml.find(close, position);
        if (end == std::string_view::npos) return std::nullopt;
        return xml_unescape(xml.substr(position, end - position));
    }
    return std::nullopt;
}

std::optional<std::uint32_t> llsd_u32(std::string_view xml, std::string_view key) {
    const auto value = llsd_value(xml, key);
    if (!value) return std::nullopt;
    std::uint32_t parsed{};
    const auto result = std::from_chars(value->data(), value->data() + value->size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value->data() + value->size()) return std::nullopt;
    return parsed;
}

std::string base64(std::span<const std::uint8_t> bytes) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t offset = 0; offset < bytes.size(); offset += 3) {
        const auto remaining = bytes.size() - offset;
        const std::uint32_t value =
            static_cast<std::uint32_t>(bytes[offset]) << 16 |
            (remaining > 1 ? static_cast<std::uint32_t>(bytes[offset + 1]) << 8 : 0U) |
            (remaining > 2 ? static_cast<std::uint32_t>(bytes[offset + 2]) : 0U);
        result.push_back(alphabet[(value >> 18) & 0x3fU]);
        result.push_back(alphabet[(value >> 12) & 0x3fU]);
        result.push_back(remaining > 1 ? alphabet[(value >> 6) & 0x3fU] : '=');
        result.push_back(remaining > 2 ? alphabet[value & 0x3fU] : '=');
    }
    return result;
}

std::string region_handle_binary(std::uint64_t handle) {
    std::array<std::uint8_t, 8> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<std::uint8_t>(handle >> ((7 - index) * 8));
    return base64(bytes);
}

std::string u32_binary(std::uint32_t value) {
    const std::array<std::uint8_t, 4> bytes{
        static_cast<std::uint8_t>(value >> 24),
        static_cast<std::uint8_t>(value >> 16),
        static_cast<std::uint8_t>(value >> 8),
        static_cast<std::uint8_t>(value)};
    return base64(bytes);
}

std::string ip_binary(const SimulatorEventEndpoint& simulator) {
    return base64(simulator.address);
}
} // namespace

std::vector<std::string> parse_requested_capabilities(std::string_view xml) {
    // The seed body is an LLSD array of capability-name strings. Names are plain
    // identifiers, so the string contents need no XML unescaping; anything
    // carrying markup is not a capability name and is skipped by the bounds below.
    std::vector<std::string> requested;
    const auto array_start = xml.find("<array>");
    if (array_start == std::string_view::npos) return requested;
    const auto array_end = xml.find("</array>", array_start);
    if (array_end == std::string_view::npos) return requested;
    std::size_t position = array_start;
    while (requested.size() < max_requested_capabilities) {
        const auto open = xml.find("<string>", position);
        if (open == std::string_view::npos || open > array_end) break;
        const auto close = xml.find("</string>", open);
        if (close == std::string_view::npos || close > array_end) break;
        const auto name = xml.substr(open + 8, close - (open + 8));
        if (!name.empty() && name.size() <= max_capability_name_length &&
            name.find('<') == std::string_view::npos)
            requested.emplace_back(name);
        position = close + 9;
    }
    return requested;
}

std::string seed_capability_xml(std::string_view public_endpoint, std::string_view grid_public_endpoint,
                                std::string_view session_id, std::string_view visit_id,
                                const std::vector<ExtensionCapability>& extension_capabilities) {
    auto base = std::string(public_endpoint);
    while (!base.empty() && base.back() == '/') base.pop_back();
    const auto visit_suffix = visit_id.empty() ? std::string{} : "/" + std::string(visit_id);
    const auto event_url = xml_escape(base + "/caps/event/" + std::string(session_id) + visit_suffix);
    const auto texture_url = xml_escape(base + "/caps/texture/" + std::string(session_id));
    const auto asset_url = xml_escape(base + "/caps/assets/" + std::string(session_id));
    const auto simulator_features_url =
        xml_escape(base + "/caps/simulator-features/" + std::string(session_id));
    const auto environment_url = xml_escape(base + "/caps/environment/" + std::string(session_id));
    const auto remote_parcel_url = xml_escape(base + "/caps/remote-parcel/" + std::string(session_id));
    const auto release_notes_url =
        xml_escape(base + "/caps/server-release-notes/" + std::string(session_id));
    const auto baked_upload_url = xml_escape(base + "/caps/upload-baked/" + std::string(session_id));
    const auto file_upload_url = xml_escape(base + "/caps/upload-file/" + std::string(session_id));
    const auto mesh_upload_flag_url =
        xml_escape(base + "/caps/mesh-upload-flag/" + std::string(session_id));
    const auto render_materials_url =
        xml_escape(base + "/caps/render-materials/" + std::string(session_id));
    const auto notecard_update_url = xml_escape(base + "/caps/update-notecard/" + std::string(session_id));
    const auto script_update_url = xml_escape(base + "/caps/update-script/" + std::string(session_id));
    const auto gesture_update_url = xml_escape(base + "/caps/update-gesture/" + std::string(session_id));
    const auto task_notecard_update_url =
        xml_escape(base + "/caps/update-task-notecard/" + std::string(session_id));
    const auto task_script_update_url =
        xml_escape(base + "/caps/update-task-script/" + std::string(session_id));
    auto grid_base = std::string(grid_public_endpoint);
    while (!grid_base.empty() && grid_base.back() == '/') grid_base.pop_back();
    const auto inventory_url = xml_escape(grid_base + "/caps/inventory/descendents/" + std::string(session_id));
    const auto library_descendents_url =
        xml_escape(grid_base + "/caps/inventory/library-descendents/" + std::string(session_id));
    const auto inventory_items_url = xml_escape(grid_base + "/caps/inventory/items/" + std::string(session_id));
    const auto create_inventory_folder_url =
        xml_escape(grid_base + "/caps/inventory/create-folder/" + std::string(session_id));
    const auto inventory_ais_url =
        xml_escape(grid_base + "/caps/inventory/ais/" + std::string(session_id));
    const auto library_ais_url =
        xml_escape(grid_base + "/caps/inventory/library/" + std::string(session_id));
    // Negotiated extension capabilities are appended after the baseline set, so a
    // client that negotiated none receives exactly the pre-extension reply.
    std::string extensions;
    for (const auto& capability : extension_capabilities) {
        extensions += "<key>" + xml_escape(capability.name) + "</key><uri>" +
                      xml_escape(base + capability.path + std::string(session_id)) + "</uri>";
    }
    return "<?xml version=\"1.0\"?><llsd><map><key>EventQueueGet</key><uri>" + event_url +
           "</uri><key>GetTexture</key><uri>" + texture_url +
           "</uri><key>ViewerAsset</key><uri>" + asset_url +
           // Mesh fetches go through the GetMesh capabilities, not
           // ViewerAsset: a viewer granted neither never asks for mesh bytes
           // and renders mesh prims invisible (verified live on Firestorm,
           // 2026-07-29). Same endpoint — the handler reads any <type>_id=
           // query, mesh_id included.
           "</uri><key>GetMesh</key><uri>" + asset_url +
           "</uri><key>GetMesh2</key><uri>" + asset_url +
           "</uri><key>SimulatorFeatures</key><uri>" + simulator_features_url +
           "</uri><key>EnvironmentSettings</key><uri>" + environment_url +
           "</uri><key>RemoteParcelRequest</key><uri>" + remote_parcel_url +
           // The About box fetches this and follows the 302 it answers with;
           // without the capability it shows "Error fetching server release
           // notes URL" instead.
           "</uri><key>ServerReleaseNotes</key><uri>" + release_notes_url +
           "</uri><key>UploadBakedTexture</key><uri>" + baked_upload_url +
           "</uri><key>NewFileAgentInventory</key><uri>" + file_upload_url +
           // The per-agent upload-permission query the model uploader makes
           // before enabling its Upload button; absent, Firestorm raises
           // RegionCapabilityRequestError at the creator (seen live,
           // 2026-07-29).
           "</uri><key>MeshUploadFlag</key><uri>" + mesh_upload_flag_url +
           // Without this a viewer has nowhere to register a material and no id
           // to put on a face, so every normal and specular assignment was
           // discarded the moment it was made - and silently, because from the
           // viewer's side nothing failed (found live 2026-07-29).
           "</uri><key>RenderMaterials</key><uri>" + render_materials_url +
           "</uri><key>UpdateNotecardAgentInventory</key><uri>" + notecard_update_url +
           "</uri><key>UpdateScriptAgentInventory</key><uri>" + script_update_url +
           "</uri><key>UpdateScriptAgent</key><uri>" + script_update_url +
           "</uri><key>UpdateGestureAgentInventory</key><uri>" + gesture_update_url +
           "</uri><key>UpdateNotecardTaskInventory</key><uri>" + task_notecard_update_url +
           "</uri><key>UpdateScriptTask</key><uri>" + task_script_update_url +
           "</uri><key>FetchInventoryDescendents2</key><uri>" + inventory_url +
           "</uri><key>FetchLibDescendents2</key><uri>" + library_descendents_url +
           "</uri><key>FetchInventory2</key><uri>" + inventory_items_url +
           "</uri><key>CreateInventoryCategory</key><uri>" + create_inventory_folder_url +
           "</uri><key>InventoryAPIv3</key><uri>" + inventory_ais_url +
           "</uri><key>LibraryAPIv3</key><uri>" + library_ais_url +
           "</uri>" + extensions + "</map></llsd>";
}

std::string establish_agent_communication_event_xml(const EstablishAgentCommunication& event) {
    return "<map><key>message</key><string>EstablishAgentCommunication</string>"
           "<key>body</key><map><key>agent-id</key><uuid>" + xml_escape(event.agent_id) +
           "</uuid><key>sim-ip-and-port</key><string>" + xml_escape(event.simulator_endpoint) +
           "</string><key>seed-capability</key><uri>" + xml_escape(event.seed_capability) +
           "</uri></map></map>";
}

std::string enable_simulator_event_xml(std::uint64_t region_handle,
                                       const SimulatorEventEndpoint& simulator,
                                       std::uint32_t region_size_x,
                                       std::uint32_t region_size_y) {
    return "<map><key>message</key><string>EnableSimulator</string><key>body</key><map>"
           "<key>SimulatorInfo</key><array><map><key>Handle</key><binary>" +
           region_handle_binary(region_handle) + "</binary><key>IP</key><binary>" +
           ip_binary(simulator) + "</binary><key>Port</key><integer>" +
           std::to_string(simulator.port) + "</integer><key>RegionSizeX</key><binary>" +
           u32_binary(region_size_x) + "</binary><key>RegionSizeY</key><binary>" +
           u32_binary(region_size_y) + "</binary></map></array></map></map>";
}

std::string teleport_finish_event_xml(const TeleportFinish& event) {
    return "<map><key>message</key><string>TeleportFinish</string><key>body</key><map>"
           "<key>Info</key><array><map><key>AgentID</key><uuid>" + xml_escape(event.agent_id) +
           "</uuid><key>LocationID</key><binary>" + u32_binary(4) +
           "</binary><key>RegionHandle</key><binary>" +
           region_handle_binary(event.region_handle) +
           "</binary><key>SeedCapability</key><string>" + xml_escape(event.seed_capability) +
           "</string><key>SimAccess</key><integer>" + std::to_string(event.simulator_access) +
           "</integer><key>SimIP</key><binary>" + ip_binary(event.simulator) +
           "</binary><key>SimPort</key><integer>" + std::to_string(event.simulator.port) +
           "</integer><key>TeleportFlags</key><binary>" + u32_binary(event.teleport_flags) +
           "</binary><key>RegionSizeX</key><binary>" + u32_binary(event.region_size_x) +
           "</binary><key>RegionSizeY</key><binary>" + u32_binary(event.region_size_y) +
           "</binary></map></array></map></map>";
}

std::string crossed_region_event_xml(const CrossedRegion& event) {
    const auto vector_xml = [](const std::array<float, 3>& value) {
        return "<array><real>" + std::to_string(value[0]) + "</real><real>" +
               std::to_string(value[1]) + "</real><real>" + std::to_string(value[2]) +
               "</real></array>";
    };
    return "<map><key>message</key><string>CrossedRegion</string><key>body</key><map>"
           "<key>Info</key><array><map><key>Position</key>" + vector_xml(event.position) +
           "<key>LookAt</key>" + vector_xml(event.look_at) + "</map></array>"
           "<key>AgentData</key><array><map><key>AgentID</key><uuid>" +
           xml_escape(event.agent_id) + "</uuid><key>SessionID</key><uuid>" +
           xml_escape(event.session_id) + "</uuid></map></array>"
           "<key>RegionData</key><array><map><key>RegionHandle</key><binary>" +
           region_handle_binary(event.region_handle) +
           "</binary><key>SeedCapability</key><string>" + xml_escape(event.seed_capability) +
           "</string><key>SimIP</key><binary>" + ip_binary(event.simulator) +
           "</binary><key>SimPort</key><integer>" + std::to_string(event.simulator.port) +
           "</integer><key>RegionSizeX</key><binary>" + u32_binary(event.region_size_x) +
           "</binary><key>RegionSizeY</key><binary>" + u32_binary(event.region_size_y) +
           "</binary></map></array></map></map>";
}

namespace {

std::string llsd_boolean(bool value) {
    return value ? "<boolean>1</boolean>" : "<boolean>0</boolean>";
}

std::string llsd_vector3(const std::array<float, 3>& value) {
    return "<array><real>" + std::to_string(value[0]) + "</real><real>" +
           std::to_string(value[1]) + "</real><real>" + std::to_string(value[2]) +
           "</real></array>";
}

std::string llsd_iso8601(std::int32_t unix_seconds) {
    const std::time_t time = unix_seconds;
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return std::string("<date>") + buffer + "</date>";
}

} // namespace

std::string parcel_properties_event_xml(const ParcelPropertiesEvent& event) {
    const auto integer = [](std::int32_t value) {
        return "<integer>" + std::to_string(value) + "</integer>";
    };
    const auto real = [](double value) { return "<real>" + std::to_string(value) + "</real>"; };
    const auto uuid = [](std::string_view value) {
        return "<uuid>" + xml_escape(value.empty() ? "00000000-0000-0000-0000-000000000000" : value) +
               "</uuid>";
    };
    const auto str = [](std::string_view value) {
        return "<string>" + xml_escape(value) + "</string>";
    };
    const auto key = [](std::string_view name) { return "<key>" + std::string(name) + "</key>"; };
    const auto bitmap_base64 = base64(std::span<const std::uint8_t>(event.bitmap.data(), event.bitmap.size()));

    std::string parcel_data = "<array><map>";
    parcel_data += key("LocalID") + integer(event.local_id);
    parcel_data += key("AABBMax") + llsd_vector3(event.aabb_max);
    parcel_data += key("AABBMin") + llsd_vector3(event.aabb_min);
    parcel_data += key("Area") + integer(event.area);
    parcel_data += key("AuctionID") + integer(static_cast<std::int32_t>(event.auction_id));
    parcel_data += key("AuthBuyerID") + uuid(event.auth_buyer_id);
    parcel_data += key("Bitmap") + "<binary>" + bitmap_base64 + "</binary>";
    parcel_data += key("Category") + integer(event.category);
    parcel_data += key("ClaimDate") + llsd_iso8601(event.claim_date);
    parcel_data += key("ClaimPrice") + integer(event.claim_price);
    parcel_data += key("Desc") + str(event.description);
    parcel_data += key("ParcelFlags") + "<binary>" + u32_binary(event.parcel_flags) + "</binary>";
    parcel_data += key("GroupID") + uuid(event.group_id);
    parcel_data += key("GroupPrims") + integer(event.group_prims);
    parcel_data += key("IsGroupOwned") + llsd_boolean(event.is_group_owned);
    parcel_data += key("LandingType") + integer(event.landing_type);
    parcel_data += key("MaxPrims") + integer(event.max_prims);
    parcel_data += key("MediaID") + uuid(event.media_id);
    parcel_data += key("MediaURL") + str(event.media_url);
    parcel_data += key("MediaAutoScale") + llsd_boolean(event.media_auto_scale != 0);
    parcel_data += key("MusicURL") + str(event.music_url);
    parcel_data += key("Name") + str(event.name);
    parcel_data += key("OtherCleanTime") + integer(event.other_clean_time);
    parcel_data += key("OtherCount") + integer(event.other_count);
    parcel_data += key("OtherPrims") + integer(event.other_prims);
    parcel_data += key("OwnerID") + uuid(event.owner_id);
    parcel_data += key("OwnerPrims") + integer(event.owner_prims);
    parcel_data += key("ParcelPrimBonus") + real(event.parcel_prim_bonus);
    parcel_data += key("PassHours") + real(event.pass_hours);
    parcel_data += key("PassPrice") + integer(event.pass_price);
    parcel_data += key("PublicCount") + integer(event.public_count);
    parcel_data += key("RegionDenyAnonymous") + llsd_boolean(event.region_deny_anonymous);
    parcel_data += key("RegionDenyIdentified") + llsd_boolean(event.region_deny_identified);
    parcel_data += key("RegionDenyTransacted") + llsd_boolean(event.region_deny_transacted);
    parcel_data += key("RegionPushOverride") + llsd_boolean(event.region_push_override);
    parcel_data += key("RentPrice") + integer(event.rent_price);
    parcel_data += key("RequestResult") + integer(event.request_result);
    parcel_data += key("SalePrice") + integer(event.sale_price);
    parcel_data += key("SelectedPrims") + integer(event.selected_prims);
    parcel_data += key("SelfCount") + integer(event.self_count);
    parcel_data += key("SequenceID") + integer(event.sequence_id);
    parcel_data += key("SimWideMaxPrims") + integer(event.sim_wide_max_prims);
    parcel_data += key("SimWideTotalPrims") + integer(event.sim_wide_total_prims);
    parcel_data += key("SnapSelection") + llsd_boolean(event.snap_selection);
    parcel_data += key("SnapshotID") + uuid(event.snapshot_id);
    parcel_data += key("Status") + integer(event.status);
    parcel_data += key("TotalPrims") + integer(event.total_prims);
    parcel_data += key("UserLocation") + llsd_vector3(event.user_location);
    parcel_data += key("UserLookAt") + llsd_vector3(event.user_look_at);
    parcel_data += "</map></array>";

    std::string media_data = "<array><map>";
    media_data += key("MediaDesc") + str(event.media_desc);
    media_data += key("MediaHeight") + integer(event.media_height);
    media_data += key("MediaWidth") + integer(event.media_width);
    media_data += key("MediaLoop") + llsd_boolean(event.media_loop);
    media_data += key("MediaType") + str(event.media_type);
    media_data += key("ObscureMedia") + llsd_boolean(event.obscure_media);
    media_data += key("ObscureMusic") + llsd_boolean(event.obscure_music);
    media_data += "</map></array>";

    std::string age = "<array><map>";
    age += key("RegionDenyAgeUnverified") + llsd_boolean(event.region_deny_age_unverified);
    age += "</map></array>";

    return "<map><key>message</key><string>ParcelProperties</string><key>body</key><map>" +
           key("ParcelData") + parcel_data + key("MediaData") + media_data +
           key("AgeVerificationBlock") + age + "</map></map>";
}

std::string object_physics_properties_event_xml(const ObjectPhysicsProperties& properties) {
    // Delivered over the Event Queue as LLSD rather than as a UDP message,
    // matching the Halcyon/Second Life shape. Without this the viewer's Extra
    // Physics fields have no source and read zero, and because the viewer posts
    // the whole set back when any one of them is edited, those zeros overwrite
    // the region's real density, friction, and gravity.
    const auto real = [](double value) { return "<real>" + std::to_string(value) + "</real>"; };
    return "<map><key>message</key><string>ObjectPhysicsProperties</string>"
           "<key>body</key><map><key>ObjectData</key><array><map>"
           "<key>LocalID</key><integer>" + std::to_string(properties.local_id) +
           "</integer><key>PhysicsShapeType</key><integer>" +
           std::to_string(static_cast<unsigned>(properties.physics_shape_type)) +
           "</integer><key>Density</key>" + real(properties.density) +
           "<key>Friction</key>" + real(properties.friction) +
           "<key>Restitution</key>" + real(properties.restitution) +
           "<key>GravityMultiplier</key>" + real(properties.gravity_multiplier) +
           "</map></array></map></map>";
}

std::string event_queue_xml(std::uint64_t id, const std::vector<std::string>& events) {
    std::string encoded_events = events.empty() ? "<array/>" : "<array>";
    for (const auto& event : events) encoded_events += event;
    if (!events.empty()) encoded_events += "</array>";
    return "<?xml version=\"1.0\"?><llsd><map><key>events</key>" + encoded_events +
           "<key>id</key><integer>" + std::to_string(id) + "</integer></map></llsd>";
}

std::string simulator_features_xml(const SimulatorFeatures& features) {
    // Each advertised extension carries its own version and the capability names
    // a client names in its seed request to opt in. With none available the map
    // is present but empty, which tells a client the mechanism exists and that
    // there is currently nothing to negotiate.
    std::string advertised;
    for (const auto& extension : features.extensions) {
        std::string names;
        for (const auto& capability : extension.capabilities)
            names += "<string>" + xml_escape(capability.name) + "</string>";
        advertised += "<key>" + xml_escape(extension.name) +
                      "</key><map><key>version</key><integer>" +
                      std::to_string(extension.version) +
                      "</integer><key>capabilities</key>" +
                      (names.empty() ? "<array/>" : "<array>" + names + "</array>") + "</map>";
    }
    // Chat ranges come from the same constants the region enforces, so the
    // advertised distances cannot drift from the audible ones.
    return "<?xml version=\"1.0\"?><llsd><map><key>OpenSimExtras</key><map>"
           "<key>currency</key><string>" + xml_escape(features.currency) +
           "</string><key>map-server-url</key><string>" + xml_escape(features.map_server_url) +
           "</string><key>ExportSupported</key>" + llsd_boolean(features.export_supported) +
           "<key>whisper-range</key><integer>" +
           std::to_string(static_cast<int>(chat_range(chat_type_whisper))) +
           "</integer><key>say-range</key><integer>" +
           std::to_string(static_cast<int>(chat_range(chat_type_normal))) +
           "</integer><key>shout-range</key><integer>" +
           std::to_string(static_cast<int>(chat_range(chat_type_shout))) +
           "</integer></map>"
           "<key>MeshRezEnabled</key>" + llsd_boolean(features.mesh) +
           "<key>MeshUploadEnabled</key>" + llsd_boolean(features.mesh_upload) +
           "<key>MeshXferEnabled</key>" + llsd_boolean(features.mesh) +
           "<key>PhysicsMaterialsEnabled</key>" + llsd_boolean(features.physics_materials) +
           "<key>DynamicPathfindingEnabled</key>" + llsd_boolean(features.dynamic_pathfinding) +
           "<key>AvatarHoverHeightEnabled</key>" + llsd_boolean(features.avatar_hover_height) +
           "<key>PhysicsShapeTypes</key><map>"
           "<key>convex</key>" + llsd_boolean(features.physics_shape_convex) +
           "<key>none</key>" + llsd_boolean(features.physics_shape_none) +
           "<key>prim</key>" + llsd_boolean(features.physics_shape_prim) +
           "</map>"
           "<key>HomeworldzExtensions</key><map><key>version</key><integer>" +
           std::to_string(extension_map_version) + "</integer><key>extensions</key>" +
           (advertised.empty() ? "<map/>" : "<map>" + advertised + "</map>") +
           "</map></map></llsd>";
}

std::string remote_parcel_reply_xml(std::string_view parcel_id) {
    return "<?xml version=\"1.0\"?><llsd><map><key>parcel_id</key><uuid>" +
           xml_escape(parcel_id.empty() ? "00000000-0000-0000-0000-000000000000" : parcel_id) +
           "</uuid></map></llsd>";
}

std::optional<std::array<double, 3>> parse_remote_parcel_location(std::string_view xml) {
    const auto key = xml.find("<key>location</key>");
    if (key == std::string_view::npos) return std::nullopt;
    const auto array_start = xml.find("<array>", key);
    if (array_start == std::string_view::npos) return std::nullopt;
    const auto array_end = xml.find("</array>", array_start);
    if (array_end == std::string_view::npos) return std::nullopt;
    std::array<double, 3> location{};
    std::size_t found = 0;
    std::size_t position = array_start;
    while (found < 3) {
        const auto real_open = xml.find("<real>", position);
        if (real_open == std::string_view::npos || real_open > array_end) break;
        const auto real_close = xml.find("</real>", real_open);
        if (real_close == std::string_view::npos || real_close > array_end) break;
        const auto text = xml.substr(real_open + 6, real_close - (real_open + 6));
        double value = 0.0;
        std::from_chars(text.data(), text.data() + text.size(), value);
        location[found++] = value;
        position = real_close + 7;
    }
    if (found < 2) return std::nullopt;
    return location;
}

std::optional<std::uint64_t> parse_remote_parcel_handle(std::string_view xml) {
    const auto key = xml.find("<key>region_handle</key>");
    if (key == std::string_view::npos) return std::nullopt;
    const auto open = xml.find("<binary", key);
    if (open == std::string_view::npos) return std::nullopt;
    const auto content = xml.find('>', open);
    const auto close = xml.find("</binary>", open);
    if (content == std::string_view::npos || close == std::string_view::npos || close < content)
        return std::nullopt;
    const auto text = xml.substr(content + 1, close - content - 1);
    const auto sextet = [](char character) -> int {
        if (character >= 'A' && character <= 'Z') return character - 'A';
        if (character >= 'a' && character <= 'z') return character - 'a' + 26;
        if (character >= '0' && character <= '9') return character - '0' + 52;
        if (character == '+') return 62;
        if (character == '/') return 63;
        return -1;
    };
    std::array<std::uint8_t, 8> bytes{};
    std::size_t produced = 0;
    unsigned buffer = 0;
    int bits = 0;
    for (const char character : text) {
        if (character == '=') break;
        const auto value = sextet(character);
        if (value < 0) continue;  // whitespace between the tag's text nodes
        buffer = (buffer << 6) | static_cast<unsigned>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (produced == bytes.size()) return std::nullopt;
            bytes[produced++] = static_cast<std::uint8_t>((buffer >> bits) & 0xffu);
        }
    }
    if (produced != bytes.size()) return std::nullopt;
    std::uint64_t handle = 0;
    for (const auto byte : bytes) handle = (handle << 8) | byte;
    return handle;
}

std::string environment_settings_xml(std::string_view region_id) {
    return "<?xml version=\"1.0\"?><llsd><array><map>"
           "<key>messageID</key><uuid>00000000-0000-0000-0000-000000000000</uuid>"
           "<key>regionID</key><uuid>" + xml_escape(region_id) +
           "</uuid></map>"
           "<array><array><real>0</real><string>Homeworldz Default</string></array></array>"
           "<map><key>Homeworldz Default</key><map>"
           "<key>gamma</key><array><real>1</real><real>0</real><real>0</real><real>1</real></array>"
           "</map></map>"
           "<map><key>blurMultiplier</key><real>0.04</real></map>"
           "</array></llsd>";
}

std::string baked_texture_upload_xml(std::string_view uploader) {
    return "<?xml version=\"1.0\"?><llsd><map><key>state</key><string>upload</string>"
           "<key>uploader</key><uri>" + xml_escape(uploader) + "</uri></map></llsd>";
}

std::string baked_texture_complete_xml(std::string_view asset_id) {
    return "<?xml version=\"1.0\"?><llsd><map><key>state</key><string>complete</string>"
           "<key>new_asset</key><uuid>" + xml_escape(asset_id) + "</uuid></map></llsd>";
}

std::optional<NewFileInventoryUpload> parse_new_file_inventory_upload(std::string_view xml) {
    if (!xml.starts_with("<?xml") && !xml.starts_with("<llsd")) return std::nullopt;
    const auto folder = llsd_value(xml, "folder_id");
    const auto asset_type = llsd_value(xml, "asset_type");
    const auto inventory_type = llsd_value(xml, "inventory_type");
    const auto name = llsd_value(xml, "name");
    const auto description = llsd_value(xml, "description");
    std::int8_t encoded_asset_type{-1};
    std::int8_t encoded_inventory_type{-1};
    if (asset_type == "texture" && inventory_type == "texture") {
        encoded_asset_type = 0;
        encoded_inventory_type = 0;
    } else if ((asset_type == "texture" || asset_type == "snapshot") &&
               inventory_type == "snapshot") {
        encoded_asset_type = 0;
        encoded_inventory_type = 15;
    } else if (asset_type == "sound" && inventory_type == "sound") {
        encoded_asset_type = 1;
        encoded_inventory_type = 1;
    } else if (asset_type == "animation" && inventory_type == "animation") {
        encoded_asset_type = 20;
        encoded_inventory_type = 19;
    }
    if (!folder || !parse_uuid(*folder) || encoded_asset_type < 0 ||
        !name || name->empty() || name->size() > 255 ||
        !description || description->size() > 1024) return std::nullopt;
    NewFileInventoryUpload result{*folder, encoded_asset_type, encoded_inventory_type,
                                  *name, *description};
    if (const auto permissions = llsd_u32(xml, "everyone_mask"))
        result.everyone_permissions = *permissions;
    if (const auto permissions = llsd_u32(xml, "group_mask"))
        result.group_permissions = *permissions;
    if (const auto permissions = llsd_u32(xml, "next_owner_mask"))
        result.next_permissions = *permissions;
    return result;
}

bool valid_new_file_inventory_upload_content(const NewFileInventoryUpload& upload,
                                             std::string_view content) {
    if (content.empty() || content.size() > 1024 * 1024) return false;
    if (upload.asset_type == 0) {
        const auto byte = [&](std::size_t index) {
            return static_cast<unsigned char>(content[index]);
        };
        const bool codestream = content.size() >= 4 &&
                                byte(0) == 0xff && byte(1) == 0x4f &&
                                byte(2) == 0xff && byte(3) == 0x51;
        const bool jp2 = content.size() >= 12 &&
                         byte(0) == 0 && byte(1) == 0 && byte(2) == 0 && byte(3) == 12 &&
                         content.substr(4, 4) == "jP  " && byte(8) == 0x0d &&
                         byte(9) == 0x0a && byte(10) == 0x87 && byte(11) == 0x0a;
        return codestream || jp2;
    }
    if (upload.asset_type == 1)
        return content.size() >= 4 && content.substr(0, 4) == "OggS";
    if (upload.asset_type == 20)
        return content.size() >= 4 &&
               static_cast<unsigned char>(content[0]) == 0x01 &&
               static_cast<unsigned char>(content[1]) == 0x00 &&
               static_cast<unsigned char>(content[2]) == 0x00 &&
               static_cast<unsigned char>(content[3]) == 0x00;
    return false;
}

std::optional<InventoryAssetUpdate> parse_inventory_asset_update(std::string_view xml) {
    if (!xml.starts_with("<?xml") && !xml.starts_with("<llsd")) return std::nullopt;
    const auto item_id = llsd_value(xml, "item_id");
    if (!item_id || !parse_uuid(*item_id)) return std::nullopt;
    InventoryAssetUpdate result{*item_id, {}, {}, false};
    if (const auto target = llsd_value(xml, "target")) {
        if (*target != "lsl2" && *target != "mono") return std::nullopt;
        result.target = *target;
    }
    if (const auto task_id = llsd_value(xml, "task_id")) {
        if (!parse_uuid(*task_id)) return std::nullopt;
        result.task_id = *task_id;
    }
    if (const auto running = llsd_value(xml, "is_script_running")) {
        if (*running != "true" && *running != "false" && *running != "1" && *running != "0")
            return std::nullopt;
        result.script_running = *running == "true" || *running == "1";
    }
    return result;
}

std::string inventory_asset_update_upload_xml(std::string_view uploader) {
    return "<?xml version=\"1.0\"?><llsd><map><key>state</key><string>upload</string>"
           "<key>uploader</key><uri>" + xml_escape(uploader) + "</uri></map></llsd>";
}

std::string inventory_asset_update_complete_xml(
    std::string_view asset_id, bool script, bool compiled,
    std::string_view diagnostic) {
    return "<?xml version=\"1.0\"?><llsd><map><key>state</key><string>complete</string>"
           "<key>new_asset</key><uuid>" + xml_escape(asset_id) + "</uuid>" +
           (script ? std::string("<key>compiled</key><boolean>") +
                         (compiled ? "true" : "false") + "</boolean>"
                   : "") +
           (script && !diagnostic.empty()
                ? "<key>errors</key><array><string>" + xml_escape(diagnostic) +
                      "</string></array>"
                : "") +
           "</map></llsd>";
}

std::string new_file_inventory_upload_xml(std::string_view uploader) {
    return "<?xml version=\"1.0\"?><llsd><map><key>state</key><string>upload</string>"
           "<key>uploader</key><uri>" + xml_escape(uploader) + "</uri>"
           "<key>resource_cost</key><integer>0</integer>"
           "<key>upload_price</key><integer>0</integer></map></llsd>";
}

std::string new_file_inventory_complete_xml(std::string_view item_id, std::string_view asset_id,
                                            std::uint32_t everyone_permissions,
                                            std::uint32_t next_permissions) {
    return "<?xml version=\"1.0\"?><llsd><map><key>state</key><string>complete</string>"
           "<key>new_inventory_item</key><uuid>" + xml_escape(item_id) + "</uuid>"
           "<key>new_asset</key><uuid>" + xml_escape(asset_id) + "</uuid>"
           "<key>new_base_mask</key><integer>2147483647</integer>"
           "<key>new_owner_mask</key><integer>2147483647</integer>"
           "<key>new_everyone_mask</key><integer>" + std::to_string(everyone_permissions) + "</integer>"
           "<key>new_next_owner_mask</key><integer>" + std::to_string(next_permissions) +
           "</integer></map></llsd>";
}

std::string random_uuid() {
    std::array<unsigned char, 16> bytes{};
    std::random_device source;
    for (auto& value : bytes) value = static_cast<unsigned char>(source());
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);
    constexpr char hex[] = "0123456789abcdef";
    std::string encoded(32, '0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        encoded[index * 2] = hex[bytes[index] >> 4];
        encoded[index * 2 + 1] = hex[bytes[index] & 0x0fU];
    }
    return encoded.substr(0, 8) + '-' + encoded.substr(8, 4) + '-' + encoded.substr(12, 4) + '-' +
           encoded.substr(16, 4) + '-' + encoded.substr(20, 12);
}

} // namespace homeworldz::viewer
