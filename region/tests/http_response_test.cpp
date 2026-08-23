#include "homeworldz/api_models.h"
#include "homeworldz/http_response.h"
#include "homeworldz/viewer_capabilities.h"

#include <array>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool contains(const std::string& value, const std::string& expected) {
    if (value.find(expected) != std::string::npos) return true;
    std::cerr << "response did not contain: " << expected << '\n' << value << '\n';
    return false;
}

struct ContractCase {
    std::string method;
    std::string path;
    int status;
    std::string schema;
};

std::vector<ContractCase> load_contract() {
    std::ifstream input(HOMEWORLDZ_OPERATIONAL_CONTRACT_PATH);
    if (!input) {
        std::cerr << "could not open contract: " << HOMEWORLDZ_OPERATIONAL_CONTRACT_PATH << '\n';
        return {};
    }
    std::vector<ContractCase> contracts;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') continue;
        std::vector<std::string> fields;
        std::size_t position = 0;
        while (position <= line.size()) {
            const auto tab = line.find('\t', position);
            fields.push_back(line.substr(position, tab - position));
            if (tab == std::string::npos) break;
            position = tab + 1;
        }
        if (fields.size() != 4) {
            std::cerr << "invalid contract line: " << line << '\n';
            return {};
        }
        contracts.push_back({fields[0], fields[1], std::stoi(fields[2]), fields[3]});
    }
    return contracts;
}

bool matches_schema(const std::string& response, const std::string& schema) {
    const auto body_start = response.find("\r\n\r\n");
    if (body_start == std::string::npos) return false;
    const auto body = response.substr(body_start + 4);
    std::vector<std::string> fields;
    if (schema == "Status") {
        fields = {"status"};
    } else if (schema == "Version") {
        fields = {"service", "version", "apiVersion"};
    } else if (schema == "Error") {
        fields = {"code", "message"};
    } else {
        std::cerr << "unknown contract schema: " << schema << '\n';
        return false;
    }
    for (const auto& field : fields) {
        if (body.find("\"" + field + "\":\"") == std::string::npos) {
            std::cerr << schema << " response lacks string field " << field << ": " << body << '\n';
            return false;
        }
    }
    return !body.empty() && body.front() == '{' && body.back() == '}';
}

} // namespace

int main() {
    bool passed = true;

    // CORS on the browser-facing session routes only. The client is a browser
    // on another origin; Firestorm is not a browser and /caps/ needs none of
    // this.
    passed &= homeworldz::http::is_browser_session_path("/session/assets/abc");
    passed &= homeworldz::http::is_browser_session_path("/session/terrain");
    passed &= !homeworldz::http::is_browser_session_path("/caps/texture/abc");
    passed &= !homeworldz::http::is_browser_session_path("/ready");
    passed &= !homeworldz::http::is_browser_session_path("/sessionish");

    auto asset = homeworldz::http::response_for_content(
        "GET /session/assets/abc HTTP/1.1\r\n\r\n", 200, "model/gltf+json", "{}");
    homeworldz::http::add_cors_headers(asset, false);
    passed &= contains(asset.content, "Access-Control-Allow-Origin: *");
    // Named or the browser hides them, and the client's revalidation and
    // ranged heightmap reads break against a region answering correctly.
    passed &= contains(asset.content, "Access-Control-Expose-Headers: ETag");
    passed &= contains(asset.content, "Content-Range");
    // A non-preflight reply must not advertise methods or request headers.
    passed &= asset.content.find("Access-Control-Allow-Methods") == std::string::npos;
    passed &= asset.content.find("Access-Control-Max-Age") == std::string::npos;
    // Never, with Allow-Origin *: these routes carry an explicit bearer
    // ticket, never a cookie, and Allow-Credentials with * is invalid anyway.
    passed &= asset.content.find("Access-Control-Allow-Credentials") == std::string::npos;

    auto preflight = homeworldz::http::response_for_content(
        "OPTIONS /session/assets/abc HTTP/1.1\r\n\r\n", 204, "text/plain", {});
    homeworldz::http::add_cors_headers(preflight, true);
    passed &= preflight.status_code == 204;
    // Authorization, If-None-Match and Range are all outside the CORS
    // safelist, so a browser sends none of them until a preflight allows it.
    passed &= contains(preflight.content, "Access-Control-Allow-Headers: Authorization");
    passed &= contains(preflight.content, "If-None-Match");
    passed &= contains(preflight.content, "Range");
    passed &= contains(preflight.content, "Access-Control-Allow-Methods: GET, HEAD, POST, OPTIONS");
    passed &= contains(preflight.content, "Access-Control-Max-Age: 86400");
    passed &= preflight.content.find("Access-Control-Allow-Credentials") == std::string::npos;

    passed &= homeworldz::api::to_json(homeworldz::api::Status{"line\nbreak"}) ==
              R"({"status":"line\nbreak"})";
    passed &= homeworldz::api::to_json(homeworldz::api::Version{
                  "region", "1.2.3", std::string(homeworldz::api::api_version)}) ==
              R"({"service":"region","version":"1.2.3","apiVersion":"v1"})";
    passed &= homeworldz::api::to_json(homeworldz::api::Error{"bad_input", "quote: \""}) ==
              R"({"code":"bad_input","message":"quote: \""})";

    const auto ping = homeworldz::http::response_for(
        "GET /ping HTTP/1.1\r\nX-Request-ID: caller-request-123\r\n\r\n");
    passed &= ping.status_code == 200;
    passed &= ping.request_id == "caller-request-123";
    passed &= ping.method == "GET";
    passed &= ping.path == "/ping";
    passed &= contains(ping.content, "HTTP/1.1 200 OK");
    passed &= contains(ping.content, "X-Request-ID: caller-request-123");
    passed &= contains(ping.content, R"({"status":"ok"})");
    passed &= contains(ping.content, "Content-Length: 15");

    const auto ready = homeworldz::http::response_for("GET /ready HTTP/1.1\r\n\r\n");
    passed &= !ready.request_id.empty();
    passed &= contains(ready.content, R"({"status":"ready"})");

    const auto version = homeworldz::http::response_for("GET /version HTTP/1.1\r\n\r\n");
    passed &= contains(version.content, std::string(R"({"service":"region","version":")") +
        HOMEWORLDZ_VERSION + R"(","apiVersion":"v1"})");
    const auto packaged_version =
        homeworldz::http::response_for("GET /version HTTP/1.1\r\n\r\n", "1.2.3");
    passed &= contains(
        packaged_version.content, R"({"service":"region","version":"1.2.3","apiVersion":"v1"})");

    const auto missing = homeworldz::http::response_for("GET /missing HTTP/1.1\r\n\r\n");
    passed &= missing.status_code == 404;
    passed &= contains(missing.content, "HTTP/1.1 404 Not Found");
    passed &= contains(missing.content, R"("code":"not_found")");

    const auto llsd = homeworldz::http::response_for_content(
        "POST /caps/seed/id HTTP/1.1\r\nX-Request-ID: cap-request\r\n\r\n", 200,
        "application/llsd+xml", "<llsd><map/></llsd>");
    passed &= llsd.status_code == 200 && llsd.method == "POST" && llsd.path == "/caps/seed/id";
    passed &= contains(llsd.content, "Content-Type: application/llsd+xml");
    passed &= contains(llsd.content, "X-Request-ID: cap-request");
    passed &= homeworldz::http::request_content_length(
                  "POST /caps/seed/id HTTP/1.1\r\ncontent-length: 8192\r\n\r\n") == 8192;
    passed &= homeworldz::http::request_content_length("GET /ready HTTP/1.1\r\n\r\n") == 0;
    passed &= homeworldz::http::request_header_value(
                  "GET /asset HTTP/1.1\r\nauthorization: Bearer secret\r\n\r\n", "Authorization") ==
              "Bearer secret";
    const auto unauthorized = homeworldz::http::response_for_content(
        "GET /asset HTTP/1.1\r\n\r\n", 401, "application/json", "{}");
    passed &= contains(unauthorized.content, "HTTP/1.1 401 Unauthorized");
    // Every code the handlers use reaches the wire as itself. These two were
    // emitted as "500 Internal Server Error" until 2026-07-30 while the log
    // recorded the intended code: a successful mesh upload read as a server
    // failure to any client that believed the status line, and a mesh refusal
    // lost the 422 that distinguishes "your model is not acceptable" from
    // "the region broke".
    const auto created = homeworldz::http::response_for_content(
        "POST /session/uploads/mesh HTTP/1.1\r\n\r\n", 201, "application/json", "{}");
    passed &= created.status_code == 201 &&
              contains(created.content, "HTTP/1.1 201 Created");
    const auto refused = homeworldz::http::response_for_content(
        "POST /session/uploads/mesh HTTP/1.1\r\n\r\n", 422, "application/json", "{}");
    passed &= refused.status_code == 422 &&
              contains(refused.content, "HTTP/1.1 422 Unprocessable Content");
    // An unlisted code keeps its number and simply has no phrase; only a code
    // outside the valid range is treated as the caller's bug.
    const auto unlisted = homeworldz::http::response_for_content(
        "GET /teapot HTTP/1.1\r\n\r\n", 418, "application/json", "{}");
    passed &= contains(unlisted.content, "HTTP/1.1 418\r\n");
    const auto nonsense = homeworldz::http::response_for_content(
        "GET /x HTTP/1.1\r\n\r\n", 7, "application/json", "{}");
    passed &= contains(nonsense.content, "HTTP/1.1 500 Internal Server Error");
    passed &= !homeworldz::http::request_content_length(
                   "POST /caps/seed/id HTTP/1.1\r\nContent-Length: invalid\r\n\r\n");
    const auto seed = homeworldz::viewer::seed_capability_xml(
        "http://region.example:42001/", "http://grid.example:42000/", "session-id",
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    passed &= contains(seed, "<key>EventQueueGet</key><uri>http://region.example:42001/caps/event/"
                             "session-id/aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa</uri>");
    const auto login_seed = homeworldz::viewer::seed_capability_xml(
        "http://region.example:42001/", "http://grid.example:42000/", "session-id");
    passed &= contains(login_seed,
        "<key>EventQueueGet</key><uri>http://region.example:42001/caps/event/session-id</uri>");
    passed &= contains(seed, "<key>GetTexture</key><uri>http://region.example:42001/caps/texture/session-id</uri>");
    passed &= contains(seed, "<key>ViewerAsset</key><uri>http://region.example:42001/caps/assets/session-id</uri>");
    passed &= contains(seed, "<key>SimulatorFeatures</key><uri>http://region.example:42001/caps/simulator-features/session-id</uri>");
    passed &= contains(seed, "<key>EnvironmentSettings</key><uri>http://region.example:42001/caps/environment/session-id</uri>");
    passed &= contains(seed, "<key>ServerReleaseNotes</key><uri>http://region.example:42001/caps/server-release-notes/session-id</uri>");
    // The About box takes the URL from Location and reads no body, so the
    // status line and that header are the entire contract.
    const auto redirect = homeworldz::http::response_for_redirect(
        "GET /caps/server-release-notes/session-id HTTP/1.1\r\n\r\n",
        "https://homeworldz.com/roadmaps/server");
    passed &= redirect.status_code == 302;
    passed &= contains(redirect.content, "HTTP/1.1 302 Found\r\n");
    passed &= contains(redirect.content, "Location: https://homeworldz.com/roadmaps/server\r\n");
    passed &= contains(seed, "<key>UploadBakedTexture</key><uri>http://region.example:42001/caps/upload-baked/session-id</uri>");
    passed &= contains(seed, "<key>NewFileAgentInventory</key><uri>http://region.example:42001/caps/upload-file/session-id</uri>");
    passed &= contains(seed, "<key>UpdateNotecardAgentInventory</key><uri>http://region.example:42001/caps/update-notecard/session-id</uri>");
    passed &= contains(seed, "<key>UpdateScriptAgent</key><uri>http://region.example:42001/caps/update-script/session-id</uri>");
    passed &= contains(seed, "<key>UpdateGestureAgentInventory</key><uri>http://region.example:42001/caps/update-gesture/session-id</uri>");
    passed &= contains(seed, "<key>UpdateNotecardTaskInventory</key><uri>http://region.example:42001/caps/update-task-notecard/session-id</uri>");
    passed &= contains(seed, "<key>UpdateScriptTask</key><uri>http://region.example:42001/caps/update-task-script/session-id</uri>");
    passed &= contains(seed, "<key>FetchInventoryDescendents2</key><uri>http://grid.example:42000/caps/inventory/descendents/session-id</uri>");
    passed &= contains(seed, "<key>FetchLibDescendents2</key><uri>http://grid.example:42000/caps/inventory/library-descendents/session-id</uri>");
    passed &= contains(seed, "<key>FetchInventory2</key><uri>http://grid.example:42000/caps/inventory/items/session-id</uri>");
    passed &= contains(seed, "<key>CreateInventoryCategory</key><uri>http://grid.example:42000/caps/inventory/create-folder/session-id</uri>");
    passed &= contains(seed, "<key>InventoryAPIv3</key><uri>http://grid.example:42000/caps/inventory/ais/session-id</uri>");
    passed &= contains(seed, "<key>LibraryAPIv3</key><uri>http://grid.example:42000/caps/inventory/library/session-id</uri>");
    const auto establish = homeworldz::viewer::establish_agent_communication_event_xml(
        homeworldz::viewer::EstablishAgentCommunication{
            "11111111-2222-4333-8444-555555555555", "region.example:42002",
            "http://region.example:42001/caps/seed/session&amp;id"});
    const auto event = homeworldz::viewer::event_queue_xml(7, {establish});
    passed &= contains(event, "<string>EstablishAgentCommunication</string>");
    passed &= contains(event, "<key>id</key><integer>7</integer>");
    passed &= contains(event, "session&amp;amp;id");
    const homeworldz::viewer::SimulatorEventEndpoint event_endpoint{{192, 0, 2, 10}, 42002};
    const auto enable = homeworldz::viewer::enable_simulator_event_xml(
        0x0102030405060708ULL, event_endpoint, 512, 512);
    passed &= contains(enable, "<string>EnableSimulator</string>");
    passed &= contains(enable, "<key>Handle</key><binary>AQIDBAUGBwg=</binary>");
    passed &= contains(enable, "<key>IP</key><binary>wAACCg==</binary>");
    passed &= contains(enable, "<key>Port</key><integer>42002</integer>");
    passed &= contains(enable, "<key>RegionSizeX</key><binary>AAACAA==</binary>");
    passed &= contains(enable, "<key>RegionSizeY</key><binary>AAACAA==</binary>");
    const auto teleport_finish = homeworldz::viewer::teleport_finish_event_xml({
        "11111111-2222-4333-8444-555555555555", 0x0102030405060708ULL, event_endpoint,
        "https://region.example/caps/seed/session&amp;id", 13,
        homeworldz::viewer::teleport_flags_via_location, 1024, 1024});
    passed &= contains(teleport_finish, "<string>TeleportFinish</string>");
    passed &= contains(teleport_finish, "<key>RegionHandle</key><binary>AQIDBAUGBwg=</binary>");
    passed &= contains(teleport_finish, "<key>SeedCapability</key><string>https://region.example/caps/seed/session&amp;amp;id</string>");
    passed &= contains(teleport_finish, "<key>SimAccess</key><integer>13</integer>");
    passed &= contains(teleport_finish, "<key>LocationID</key><binary>AAAABA==</binary>");
    passed &= contains(teleport_finish, "<key>TeleportFlags</key><binary>AAAAEA==</binary>");
    passed &= contains(teleport_finish, "<key>RegionSizeX</key><binary>AAAEAA==</binary>");
    passed &= contains(teleport_finish, "<key>RegionSizeY</key><binary>AAAEAA==</binary>");
    const auto flying_finish = homeworldz::viewer::teleport_finish_event_xml({
        "11111111-2222-4333-8444-555555555555", 0x0102030405060708ULL, event_endpoint,
        "https://region.example/caps/seed/session", 13,
        homeworldz::viewer::teleport_flags_via_location |
            homeworldz::viewer::teleport_flags_is_flying});
    passed &= contains(flying_finish, "<key>TeleportFlags</key><binary>AAAgEA==</binary>");
    const auto crossed = homeworldz::viewer::crossed_region_event_xml({
        "11111111-2222-4333-8444-555555555555",
        "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
        0x0102030405060708ULL, event_endpoint,
        "https://region.example/caps/seed/session&amp;id",
        {1.25F, 255.5F, 30.0F}, {0.5F, -0.25F, 0.0F}, 512, 768});
    passed &= contains(crossed, "<string>CrossedRegion</string>");
    passed &= contains(crossed, "<key>AgentID</key><uuid>11111111-2222-4333-8444-555555555555</uuid>");
    passed &= contains(crossed, "<key>SessionID</key><uuid>aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee</uuid>");
    passed &= contains(crossed, "<key>RegionHandle</key><binary>AQIDBAUGBwg=</binary>");
    passed &= contains(crossed, "<key>SeedCapability</key><string>https://region.example/caps/seed/session&amp;amp;id</string>");
    passed &= contains(crossed, "<key>SimIP</key><binary>wAACCg==</binary>");
    passed &= contains(crossed, "<key>SimPort</key><integer>42002</integer>");
    passed &= contains(crossed, "<key>RegionSizeX</key><binary>AAACAA==</binary>");
    passed &= contains(crossed, "<key>RegionSizeY</key><binary>AAADAA==</binary>");
    passed &= contains(crossed, "<key>Position</key><array><real>1.250000</real><real>255.500000</real><real>30.000000</real></array>");
    passed &= contains(crossed, "<key>LookAt</key><array><real>0.500000</real><real>-0.250000</real><real>0.000000</real></array>");
    const auto simulator_features = homeworldz::viewer::simulator_features_xml(
        {.map_server_url = "https://grid.example/map/"});
    passed &= contains(simulator_features,
                       "<key>OpenSimExtras</key><map><key>currency</key><string>C$</string>");
    passed &= contains(simulator_features,
                       "<key>map-server-url</key><string>https://grid.example/map/</string>");
    const auto environment = homeworldz::viewer::environment_settings_xml(
        "11111111-2222-4333-8444-555555555555");
    passed &= contains(environment, "<key>messageID</key><uuid>00000000-0000-0000-0000-000000000000</uuid>");
    passed &= contains(environment, "<key>regionID</key><uuid>11111111-2222-4333-8444-555555555555</uuid>");
    passed &= contains(environment, "<real>0</real><string>Homeworldz Default</string>");
    passed &= contains(environment, "<key>gamma</key>");
    passed &= contains(environment, "<key>blurMultiplier</key><real>0.04</real>");
    const auto upload = homeworldz::viewer::baked_texture_upload_xml(
        "http://region.example:42001/caps/upload-baked-data/session&amp;id/7");
    passed &= contains(upload, "<key>state</key><string>upload</string>");
    passed &= contains(upload, "session&amp;amp;id/7");
    const std::string baked_id{"11111111-2222-4333-8444-555555555555"};
    const auto complete = homeworldz::viewer::baked_texture_complete_xml(baked_id);
    passed &= contains(complete, "<key>state</key><string>complete</string>");
    passed &= contains(complete, "<key>new_asset</key><uuid>" + baked_id + "</uuid>");
    const auto file_request = homeworldz::viewer::parse_new_file_inventory_upload(
        "<?xml version=\"1.0\"?><llsd><map>"
        "<key>folder_id</key><uuid>11111111-1111-4111-8111-111111111111</uuid>"
        "<key>asset_type</key><string>texture</string>"
        "<key>inventory_type</key><string>texture</string>"
        "<key>name</key><string>Terrain &amp; Sky</string>"
        "<key>description</key><string>Library &lt;source&gt;</string>"
        "<key>everyone_mask</key><integer>8</integer>"
        "<key>group_mask</key><integer>16</integer>"
        "<key>next_owner_mask</key><integer>2147483647</integer></map></llsd>");
    passed &= file_request && file_request->folder_id == "11111111-1111-4111-8111-111111111111" &&
              file_request->asset_type == 0 && file_request->inventory_type == 0 &&
              file_request->name == "Terrain & Sky" && file_request->description == "Library <source>" &&
              file_request->everyone_permissions == 8 && file_request->group_permissions == 16 &&
              file_request->next_permissions == 0x7fffffff;
    const auto sound_request = homeworldz::viewer::parse_new_file_inventory_upload(
        "<llsd><map><key>folder_id</key><uuid>11111111-1111-4111-8111-111111111111</uuid>"
        "<key>asset_type</key><string>sound</string><key>inventory_type</key><string>sound</string>"
        "<key>name</key><string>Bell</string><key>description</key><string></string></map></llsd>");
    const auto animation_request = homeworldz::viewer::parse_new_file_inventory_upload(
        "<llsd><map><key>folder_id</key><uuid>11111111-1111-4111-8111-111111111111</uuid>"
        "<key>asset_type</key><string>animation</string><key>inventory_type</key><string>animation</string>"
        "<key>name</key><string>Wave</string><key>description</key><string></string></map></llsd>");
    passed &= sound_request && sound_request->asset_type == 1 && sound_request->inventory_type == 1 &&
              homeworldz::viewer::valid_new_file_inventory_upload_content(*sound_request, "OggSdata") &&
              !homeworldz::viewer::valid_new_file_inventory_upload_content(*sound_request, "RIFFdata");
    const std::string animation_data{"\x01\x00\x00\x00payload", 11};
    passed &= animation_request && animation_request->asset_type == 20 && animation_request->inventory_type == 19 &&
              homeworldz::viewer::valid_new_file_inventory_upload_content(*animation_request, animation_data);
    passed &= !homeworldz::viewer::parse_new_file_inventory_upload(
        "<llsd><map><key>asset_type</key><string>mesh</string></map></llsd>");
    const auto file_upload = homeworldz::viewer::new_file_inventory_upload_xml(
        "http://region.example/upload?a=1&amp;b=2");
    passed &= contains(file_upload, "<key>state</key><string>upload</string>") &&
              contains(file_upload, "upload?a=1&amp;amp;b=2");
    const auto file_complete = homeworldz::viewer::new_file_inventory_complete_xml(
        "22222222-2222-4222-8222-222222222222", "33333333-3333-4333-8333-333333333333", 8, 16);
    passed &= contains(file_complete, "<key>new_inventory_item</key><uuid>22222222-2222-4222-8222-222222222222</uuid>") &&
              contains(file_complete, "<key>new_asset</key><uuid>33333333-3333-4333-8333-333333333333</uuid>") &&
              contains(file_complete, "<key>new_everyone_mask</key><integer>8</integer>");
    const auto item_update = homeworldz::viewer::parse_inventory_asset_update(
        "<llsd><map><key>item_id</key><uuid>11111111-1111-4111-8111-111111111111</uuid>"
        "<key>target</key><string>mono</string></map></llsd>");
    passed &= item_update && item_update->item_id == "11111111-1111-4111-8111-111111111111" &&
              item_update->target == "mono";
    const auto task_update = homeworldz::viewer::parse_inventory_asset_update(
        "<llsd><map><key>item_id</key><uuid>11111111-1111-4111-8111-111111111111</uuid>"
        "<key>task_id</key><uuid>22222222-2222-4222-8222-222222222222</uuid>"
        "<key>target</key><string>lsl2</string>"
        "<key>is_script_running</key><boolean>true</boolean></map></llsd>");
    passed &= task_update && task_update->task_id == "22222222-2222-4222-8222-222222222222" &&
              task_update->target == "lsl2" && task_update->script_running;
    passed &= !homeworldz::viewer::parse_inventory_asset_update(
        "<llsd><map><key>item_id</key><uuid>bad</uuid></map></llsd>");
    const auto update_upload = homeworldz::viewer::inventory_asset_update_upload_xml(
        "http://region.example/update?a=1&amp;b=2");
    passed &= contains(update_upload, "<key>state</key><string>upload</string>") &&
              contains(update_upload, "update?a=1&amp;amp;b=2");
    const auto update_complete = homeworldz::viewer::inventory_asset_update_complete_xml(
        "33333333-3333-4333-8333-333333333333", true);
    passed &= contains(update_complete, "<key>new_asset</key><uuid>33333333-3333-4333-8333-333333333333</uuid>") &&
              contains(update_complete, "<key>compiled</key><boolean>false</boolean>");
    const auto compiled_update = homeworldz::viewer::inventory_asset_update_complete_xml(
        "33333333-3333-4333-8333-333333333333", true, true);
    passed &= contains(compiled_update, "<key>compiled</key><boolean>true</boolean>");
    const auto failed_compile = homeworldz::viewer::inventory_asset_update_complete_xml(
        "33333333-3333-4333-8333-333333333333", true, false,
        "unexpected token <name> & value");
    passed &= contains(failed_compile, "<key>compiled</key><boolean>false</boolean>") &&
              contains(failed_compile,
                  "<key>errors</key><array><string>unexpected token &lt;name&gt; &amp; value</string></array>");
    const auto random_id = homeworldz::viewer::random_uuid();
    passed &= random_id.size() == 36 && random_id[14] == '4' && random_id[19] >= '8' && random_id[19] <= 'b';

    const auto unsafe_id = homeworldz::http::response_for(
        "GET /ping HTTP/1.1\r\nX-Request-ID: unsafe value\r\n\r\n");
    passed &= unsafe_id.request_id != "unsafe value";
    passed &= unsafe_id.request_id.size() == 32;

    const auto contracts = load_contract();
    passed &= !contracts.empty();
    for (const auto& contract : contracts) {
        const auto response = homeworldz::http::response_for(
            contract.method + " " + contract.path + " HTTP/1.1\r\n\r\n");
        if (response.status_code != contract.status) {
            std::cerr << contract.method << ' ' << contract.path << " status "
                      << response.status_code << ", want " << contract.status << '\n';
            passed = false;
        }
        passed &= !response.request_id.empty();
        passed &= matches_schema(response.content, contract.schema);
    }
    return passed ? 0 : 1;
}
