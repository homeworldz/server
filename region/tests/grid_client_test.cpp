#include "homeworldz/grid_client.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace {

struct Request {
    std::string method;
    std::string path;
    std::string body;
};

class FakeTransport final : public homeworldz::grid::Transport {
public:
    homeworldz::grid::HttpResponse send(std::string_view method, std::string_view path,
                                        std::string_view body) override {
        requests.push_back({std::string(method), std::string(path), std::string(body)});
        if (method == "POST" && path.ends_with("/validate-ticket"))
            return {200, R"({"userId":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","userid":"jim.tarber","displayName":"Jim Tarber","sessionId":"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb","expiresAt":"2026-07-27T00:00:00Z"})"};
        if (method == "POST" && path.starts_with("/api/v1/region-runtime/"))
            return {200, R"({"id":"22222222-2222-4222-8222-222222222222","name":"Sandbox Region","gridX":1001,"gridY":1000,"sizeX":256,"sizeY":256,"maturity":0,"publicEndpoint":"https://sandbox.example/region","viewerPort":43002,"gridName":"Homeworldz Test","gridPublicUrl":"https://grid.example","regionProtocol":1})"};
        if (method == "GET" && path.starts_with("/api/v1/vault/assets/"))
            return {200, "vault bytes"};
        if (method == "GET" && path == "/api/v1/regions/topology")
            return {200, R"({"regions":[{"id":"11111111-1111-4111-8111-111111111111","name":"Welcome","gridX":1000,"gridY":1000,"sizeX":256,"sizeY":256,"maturity":0,"publicEndpoint":"http://grid.example:42011","viewerPort":42012,"online":true},{"id":"44444444-4444-4444-8444-444444444444","name":"Gamma","gridX":1004,"gridY":1000,"sizeX":512,"sizeY":512,"maturity":0,"online":false}]})"};
        if (method == "GET" && path.starts_with("/api/v1/regions/lookup?")) {
            // A destination four squares away and offline: the two things the
            // neighbor list could never report.
            if (path.find("gridX=1005") != std::string_view::npos ||
                path.find("id=44444444") != std::string_view::npos)
                return {200, R"({"id":"44444444-4444-4444-8444-444444444444","name":"Gamma","gridX":1004,"gridY":1000,"sizeX":512,"sizeY":512,"maturity":0,"online":false})"};
            if (path.find("gridX=1000") != std::string_view::npos)
                return {200, R"({"id":"11111111-1111-4111-8111-111111111111","name":"Welcome","gridX":1000,"gridY":1000,"sizeX":256,"sizeY":256,"maturity":0,"publicEndpoint":"http://grid.example:42011","viewerPort":42012,"online":true})"};
            return {404, R"({"code":"region_not_found","message":"no region occupies that location"})"};
        }
        if (method == "GET" && path.ends_with("/neighbors"))
            return {200, R"({"neighbors":[{"direction":"west","region":{"id":"11111111-1111-4111-8111-111111111111","name":"Welcome","gridX":1000,"gridY":1000,"sizeX":256,"sizeY":256,"maturity":0,"publicEndpoint":"http://grid.example:42011","viewerPort":42012,"online":true}}]})"};
        if (method == "POST" && path == "/api/v1/transits")
            return {200, R"({"id":"33333333-3333-4333-8333-333333333333","generation":1,"agentId":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","sessionId":"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb","sourceRegionId":"11111111-1111-4111-8111-111111111111","destinationRegionId":"22222222-2222-4222-8222-222222222222","position":{"x":128,"y":64,"z":30},"lookAt":{"x":1,"y":0,"z":0},"flying":true,"state":"prepared"})"};
        if (method == "POST" && path.ends_with("/accept"))
            return {200, R"({"id":"33333333-3333-4333-8333-333333333333","generation":1,"agentId":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","sessionId":"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb","sourceRegionId":"11111111-1111-4111-8111-111111111111","destinationRegionId":"22222222-2222-4222-8222-222222222222","position":{"x":128,"y":64,"z":30},"lookAt":{"x":1,"y":0,"z":0},"flying":true,"state":"accepted"})"};
        if (method == "POST" && path.ends_with("/activate"))
            return {200, R"({"id":"33333333-3333-4333-8333-333333333333","generation":1,"agentId":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","sessionId":"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb","sourceRegionId":"11111111-1111-4111-8111-111111111111","destinationRegionId":"22222222-2222-4222-8222-222222222222","position":{"x":128,"y":64,"z":30},"lookAt":{"x":1,"y":0,"z":0},"flying":true,"state":"activated"})"};
        if (method == "POST" && path.ends_with("/rollback"))
            return {200, R"({"id":"33333333-3333-4333-8333-333333333333","generation":1,"agentId":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","sessionId":"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb","sourceRegionId":"11111111-1111-4111-8111-111111111111","destinationRegionId":"22222222-2222-4222-8222-222222222222","position":{"x":128,"y":64,"z":30},"lookAt":{"x":1,"y":0,"z":0},"flying":true,"state":"rolled_back"})"};
        if (method == "POST" && path == "/api/v1/task-transfers")
            return {200, R"({"id":"99999999-9999-4999-8999-999999999999","userId":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","sourceItemId":"44444444-4444-4444-8444-444444444444","regionId":"22222222-2222-4222-8222-222222222222","objectId":"55555555-5555-4555-8555-555555555555","taskItemId":"66666666-6666-4666-8666-666666666666","item":{"id":"44444444-4444-4444-8444-444444444444","ownerUserId":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","creatorUserId":"77777777-7777-4777-8777-777777777777","folderId":"88888888-8888-4888-8888-888888888888","assetId":"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa","assetType":0,"inventoryType":0,"name":"No Copy Texture","description":"","flags":0,"basePermissions":647168,"currentPermissions":614400,"everyonePermissions":0,"nextPermissions":532480,"saleType":0,"salePrice":0,"createdAt":"2026-07-18T00:00:00Z"},"state":"prepared","createdAt":"2026-07-18T00:00:00Z","updatedAt":"2026-07-18T00:00:00Z"})"};
        if (method == "GET" && path.starts_with("/api/v1/task-transfers?"))
            return {200, R"([{"id":"99999999-9999-4999-8999-999999999999","userId":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","sourceItemId":"44444444-4444-4444-8444-444444444444","regionId":"22222222-2222-4222-8222-222222222222","objectId":"55555555-5555-4555-8555-555555555555","taskItemId":"66666666-6666-4666-8666-666666666666","item":{"id":"44444444-4444-4444-8444-444444444444","ownerUserId":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","creatorUserId":"77777777-7777-4777-8777-777777777777","folderId":"88888888-8888-4888-8888-888888888888","assetId":"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa","assetType":0,"inventoryType":0,"name":"No Copy Texture","description":"","flags":0,"basePermissions":647168,"currentPermissions":614400,"everyonePermissions":0,"nextPermissions":532480,"saleType":0,"salePrice":0,"createdAt":"2026-07-18T00:00:00Z"},"state":"prepared","createdAt":"2026-07-18T00:00:00Z","updatedAt":"2026-07-18T00:00:00Z"}])"};
        const auto extraction = [](std::string_view state) {
            return std::string{R"({"id":"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa","userId":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","regionId":"22222222-2222-4222-8222-222222222222","objectId":"55555555-5555-4555-8555-555555555555","sourceTaskItemId":"66666666-6666-4666-8666-666666666666","destinationFolderId":"88888888-8888-4888-8888-888888888888","personalItemId":"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb","item":{"id":"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb","ownerUserId":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","creatorUserId":"77777777-7777-4777-8777-777777777777","folderId":"88888888-8888-4888-8888-888888888888","assetId":"dddddddd-dddd-4ddd-8ddd-dddddddddddd","assetType":0,"inventoryType":0,"name":"No Copy Texture","description":"","flags":0,"basePermissions":647168,"currentPermissions":614400,"everyonePermissions":0,"nextPermissions":565248,"saleType":0,"salePrice":0},"state":")"} + std::string(state) + R"(","createdAt":"2026-07-18T00:00:00Z","updatedAt":"2026-07-18T00:00:00Z"})";
        };
        if (method == "POST" && path == "/api/v1/task-extractions")
            return {200, extraction("prepared")};
        if (method == "GET" && path.starts_with("/api/v1/task-extractions?"))
            return {200, "[" + extraction("prepared") + "]"};
        if (method == "POST" && path.starts_with("/api/v1/task-extractions/") && path.ends_with("/finalize"))
            return {200, extraction("finalized")};
        const auto object_rez = [](std::string_view state) {
            return std::string{R"({"id":"eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee","userId":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","sourceItemId":"44444444-4444-4444-8444-444444444444","regionId":"22222222-2222-4222-8222-222222222222","objectId":"55555555-5555-4555-8555-555555555555","item":{"id":"44444444-4444-4444-8444-444444444444","ownerUserId":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","creatorUserId":"77777777-7777-4777-8777-777777777777","folderId":"88888888-8888-4888-8888-888888888888","assetId":"dddddddd-dddd-4ddd-8ddd-dddddddddddd","assetType":6,"inventoryType":6,"name":"No Copy Object","description":"","flags":0,"basePermissions":647168,"currentPermissions":548864,"everyonePermissions":0,"nextPermissions":532480,"saleType":0,"salePrice":0},"state":")"} + std::string(state) + R"(","createdAt":"2026-07-18T00:00:00Z","updatedAt":"2026-07-18T00:00:00Z"})";
        };
        if (method == "POST" && path == "/api/v1/object-rezzes")
            return {200, object_rez("prepared")};
        if (method == "GET" && path.starts_with("/api/v1/object-rezzes?"))
            return {200, "[" + object_rez("prepared") + "]"};
        if (method == "POST" && path.starts_with("/api/v1/object-rezzes/") && path.ends_with("/finalize"))
            return {200, object_rez("finalized")};
        if (method == "POST" && path.ends_with("/finalize"))
            return {200, R"({"id":"99999999-9999-4999-8999-999999999999","state":"finalized"})"};
        if (method == "POST" && path.ends_with("/prepare-arrival"))
            return {200, R"({"status":"accepted"})"};
        if (method == "GET" && path.starts_with("/api/v1/transits/"))
            return {200, R"({"id":"33333333-3333-4333-8333-333333333333","generation":1,"agentId":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","sessionId":"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb","sourceRegionId":"11111111-1111-4111-8111-111111111111","destinationRegionId":"22222222-2222-4222-8222-222222222222","position":{"x":128,"y":64,"z":30},"lookAt":{"x":1,"y":0,"z":0},"flying":true,"state":"prepared"})"};
        if (method == "POST" && path.ends_with("/copy-library-item"))
            return {201, R"({"id":"11111111-1111-4111-8111-111111111111","ownerUserId":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","creatorUserId":"00000000-0000-0000-0000-000000000002","folderId":"22222222-2222-4222-8222-222222222222","assetId":"33333333-3333-4333-8333-333333333333","assetType":5,"inventoryType":18,"name":"Default Shirt","description":"","flags":4,"basePermissions":2147483647,"currentPermissions":2147483647,"everyonePermissions":2147483647,"nextPermissions":2147483647,"saleType":0,"salePrice":0})"};
        if (method == "POST" && path.ends_with("/copy-item"))
            return {201, R"({"id":"88888888-8888-4888-8888-888888888888","ownerUserId":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","creatorUserId":"77777777-7777-4777-8777-777777777777","folderId":"55555555-5555-4555-8555-555555555555","assetId":"66666666-6666-4666-8666-666666666666","assetType":6,"inventoryType":6,"name":"Prim1 copy","description":"Tall box","flags":0,"basePermissions":647168,"currentPermissions":647168,"everyonePermissions":0,"nextPermissions":581632,"saleType":0,"salePrice":0})"};
        if (method == "POST") return {201, R"({"id":"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"})"};
        if (method == "PUT") return {200, R"({"id":"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"})"};
        if (method == "DELETE") return {204, {}};
        if (method == "GET" && path.starts_with("/api/v1/users/"))
            return {200, R"({"id":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","username":"jim.tarber","createdAt":"2026-07-14T00:00:00Z"})"};
        if (method == "GET" && path.find("/system-folders/") != std::string_view::npos)
            return {200, R"({"id":"22222222-2222-4222-8222-222222222222","ownerUserId":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","parentId":"eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee","name":"Objects","typeDefault":6,"version":1})"};
        if (method == "GET" && path.find("/inventory/") != std::string_view::npos &&
            path.find("/items/") != std::string_view::npos)
            return {200, R"({"id":"44444444-4444-4444-8444-444444444444","ownerUserId":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","creatorUserId":"77777777-7777-4777-8777-777777777777","folderId":"55555555-5555-4555-8555-555555555555","assetId":"66666666-6666-4666-8666-666666666666","assetType":6,"inventoryType":6,"name":"Prim2","description":"","flags":0,"basePermissions":647168,"currentPermissions":647168,"everyonePermissions":0,"nextPermissions":581632,"saleType":0,"salePrice":0})"};
        if (method == "GET" && path.starts_with("/api/v1/assets/"))
            return {200, R"({"id":"66666666-6666-4666-8666-666666666666","creatorUserId":"77777777-7777-4777-8777-777777777777","sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","size":332,"locations":[{"endpoint":"http://origin.example:42001","origin":true,"verifiedAt":"2026-07-14T00:00:00Z"},{"endpoint":"http://replica.example:42001","origin":false,"verifiedAt":"2026-07-14T00:00:00Z"}]})"};
        if (method == "GET") return {200, R"({"id":"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb","secureSessionId":"dddddddd-dddd-4ddd-8ddd-dddddddddddd","userId":"cccccccc-cccc-4ccc-8ccc-cccccccccccc","expiresAt":"2026-07-14T00:00:00Z","viewerCircuitCode":123456,"destinationRegionId":"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"})"};
        return {500, {}};
    }

    std::vector<Request> requests;
};

// Answers one canned response to everything and records what was asked, for
// endpoints whose interesting part is the body they parse rather than the
// sequence they take part in.
class CannedTransport final : public homeworldz::grid::Transport {
public:
    CannedTransport(int status, std::string body)
        : status_(status), body_(std::move(body)) {}

    homeworldz::grid::HttpResponse send(std::string_view method, std::string_view path,
                                        std::string_view body) override {
        requests.push_back({std::string(method), std::string(path), std::string(body)});
        return {status_, body_};
    }

    std::vector<Request> requests;

private:
    int status_;
    std::string body_;
};

// Answers every request the way the grid refuses a protocol mismatch, so the
// refusal message's path to the log can be proven.
class RefusingTransport final : public homeworldz::grid::Transport {
public:
    homeworldz::grid::HttpResponse send(std::string_view, std::string_view,
                                        std::string_view) override {
        return {409, R"({"code":"region_protocol_mismatch","message":"region is running grid-region protocol 1; this grid requires 2"})"};
    }
};

} // namespace

int main() {
    auto transport = std::make_shared<FakeTransport>();
    homeworldz::grid::Client client(transport);
    homeworldz::grid::RegionSettings settings{
        "Test Region", 1000, 1001, "http://localhost:42001", 42003, 60};
    homeworldz::grid::RegistrationLifecycle lifecycle(client, settings);
    const auto started = std::chrono::steady_clock::time_point{};
    if (!lifecycle.start(started) || lifecycle.region_id() != "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa") return 1;
    if (transport->requests.size() != 1 || transport->requests[0].method != "POST" ||
        transport->requests[0].body.find(R"("gridX":1000)") == std::string::npos ||
        transport->requests[0].body.find(R"("viewerPort":42003)") == std::string::npos) return 1;
    if (!lifecycle.tick(started + std::chrono::seconds(29)) || transport->requests.size() != 1) return 1;
    if (!lifecycle.tick(started + std::chrono::seconds(30)) || transport->requests.size() != 2 ||
        transport->requests[1].method != "PUT") return 1;
    lifecycle.stop();
    if (transport->requests.size() != 3 || transport->requests[2].method != "DELETE" ||
        !lifecycle.region_id().empty()) return 1;
    homeworldz::grid::RegionSettings provisioned_settings{
        {}, 0, 0, "http://localhost:42011", 42012, 60, "wss://sandbox.example/session"};
    const auto provisioned = client.register_provisioned_region(
        "Sandbox Region", provisioned_settings);
    if (!provisioned || provisioned->id != "22222222-2222-4222-8222-222222222222" ||
        provisioned->name != "Sandbox Region" || provisioned->grid_x != 1001 ||
		provisioned->grid_y != 1000 || provisioned->size_x != 256 || provisioned->size_y != 256 ||
		provisioned->maturity != 0 || provisioned->public_endpoint != "https://sandbox.example/region" ||
        provisioned->viewer_port != 43002 || provisioned->grid_name != "Homeworldz Test" ||
        provisioned->grid_public_url != "https://grid.example" ||
        provisioned->grid_region_protocol != 1 ||
        transport->requests.back().path != "/api/v1/region-runtime/Sandbox%20Region" ||
        transport->requests.back().body.find(
            R"("viewerPort":42012)") == std::string::npos ||
        transport->requests.back().body.find(
            R"("regionProtocol":1)") == std::string::npos ||
        transport->requests.back().body.find(
            R"("sessionEndpoint":"wss://sandbox.example/session")") == std::string::npos) return 1;
    // The grid resolves a client's region ticket; the region never holds the
    // signing secret.
    const auto ticket_identity = client.validate_region_ticket(provisioned->id, "a-ticket");
    if (!ticket_identity || ticket_identity->user_id != "cccccccc-cccc-4ccc-8ccc-cccccccccccc" ||
        ticket_identity->userid != "jim.tarber" || ticket_identity->display_name != "Jim Tarber" ||
        ticket_identity->session_id != "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb" ||
        transport->requests.back().path !=
            "/api/v1/region-runtime/" + provisioned->id + "/validate-ticket" ||
        transport->requests.back().body.find(R"("token":"a-ticket")") == std::string::npos) return 1;
    // Renewal carries the protocol too: enforcement at renewal is how a grid
    // increment drains non-matching regions within one lease period.
    if (!client.renew_provisioned_lease(provisioned->id, 60) ||
        transport->requests.back().method != "PUT" ||
        transport->requests.back().body.find(
            R"("regionProtocol":1)") == std::string::npos) return 1;
    {
        // A protocol-mismatch refusal surfaces the grid's message, which names
        // both versions, so the operator's log is actionable.
        homeworldz::grid::Client refused(std::make_shared<RefusingTransport>());
        std::string refusal;
        if (refused.register_provisioned_region("Sandbox Region", provisioned_settings, &refusal) ||
            refusal.find("requires 2") == std::string::npos) return 1;
        refusal.clear();
        if (refused.renew_provisioned_lease(provisioned->id, 60, &refusal) ||
            refusal.find("requires 2") == std::string::npos) return 1;
    }
    const auto neighbors = client.find_region_neighbors(provisioned->id);
    if (!neighbors || neighbors->size() != 1 || neighbors->front().direction != "west" ||
        neighbors->front().id != "11111111-1111-4111-8111-111111111111" ||
        neighbors->front().name != "Welcome" || neighbors->front().grid_x != 1000 ||
		neighbors->front().grid_y != 1000 || neighbors->front().size_x != 256 ||
		neighbors->front().size_y != 256 || neighbors->front().maturity != 0 ||
		!neighbors->front().online || neighbors->front().viewer_port != 42012 ||
        neighbors->front().public_endpoint != "http://grid.example:42011" ||
        transport->requests.back().path !=
            "/api/v1/regions/22222222-2222-4222-8222-222222222222/neighbors") return 1;
    // Teleport destination resolution reaches the whole grid, not just the
    // neighbors: a live region by point, a distant offline one by point and by
    // id, and an empty square as a miss rather than a fabricated placement.
    const auto welcome = client.find_region_at(1000, 1000);
    if (!welcome || welcome->id != "11111111-1111-4111-8111-111111111111" ||
        welcome->name != "Welcome" || !welcome->online || welcome->viewer_port != 42012 ||
        welcome->public_endpoint != "http://grid.example:42011" ||
        transport->requests.back().path != "/api/v1/regions/lookup?gridX=1000&gridY=1000") return 1;
    const auto gamma = client.find_region_at(1005, 1001);
    if (!gamma) return 1;
    const auto gamma_by_id = client.find_region("44444444-4444-4444-8444-444444444444");
    if (!gamma_by_id || *gamma_by_id != *gamma || gamma_by_id->online ||
        gamma_by_id->grid_x != 1004 || gamma_by_id->size_x != 512 ||
        transport->requests.back().path !=
            "/api/v1/regions/lookup?id=44444444-4444-4444-8444-444444444444") return 1;
    if (client.find_region_at(1003, 1000) || client.find_region_at(-1, 0)) return 1;
    // The vault answers for inventory-referenced bytes when no region will.
    const auto vaulted = client.fetch_vault_asset("66666666-6666-4666-8666-666666666666");
    if (!vaulted || *vaulted != "vault bytes" ||
        transport->requests.back().path !=
            "/api/v1/vault/assets/66666666-6666-4666-8666-666666666666") return 1;
    // The world map's source: every placed region, near or far, up or down.
    const auto topology = client.find_grid_topology();
    if (!topology || topology->size() != 2 || topology->front() != *welcome ||
        topology->back() != *gamma_by_id ||
        transport->requests.back().path != "/api/v1/regions/topology") return 1;
    const homeworldz::grid::AvatarTransitRequest transit_request{
        "33333333-3333-4333-8333-333333333333",
        "cccccccc-cccc-4ccc-8ccc-cccccccccccc",
        "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb",
        "11111111-1111-4111-8111-111111111111",
        "22222222-2222-4222-8222-222222222222",
        {128.0F, 64.0F, 30.0F}, {1.0F, 0.0F, 0.0F}, true, 30};
    const auto prepared_transit = client.prepare_avatar_transit(transit_request);
    if (!prepared_transit || prepared_transit->state != "prepared" ||
        prepared_transit->generation != 1 ||
        prepared_transit->position != std::array<float, 3>{128.0F, 64.0F, 30.0F} ||
        prepared_transit->look_at != std::array<float, 3>{1.0F, 0.0F, 0.0F} ||
        !prepared_transit->flying ||
        transport->requests.back().body.find(R"("position":{"x":128.000000,"y":64.000000,"z":30.000000})") == std::string::npos)
        return 1;
    const auto found_transit = client.find_avatar_transit(transit_request.id);
    if (!found_transit || found_transit->id != transit_request.id ||
        transport->requests.back().method != "GET") return 1;
    const auto accepted_transit = client.accept_avatar_transit(
        transit_request.id, transit_request.destination_region_id);
    if (!accepted_transit || accepted_transit->state != "accepted" ||
        transport->requests.back().path != "/api/v1/transits/" + transit_request.id + "/accept") return 1;
    const auto activated_transit = client.activate_avatar_transit(
        transit_request.id, transit_request.destination_region_id);
    if (!activated_transit || activated_transit->state != "activated") return 1;
    const auto rolled_back_transit = client.rollback_avatar_transit(
        transit_request.id, transit_request.source_region_id, "destination unavailable");
    if (!rolled_back_transit || rolled_back_transit->state != "rolled_back" ||
        transport->requests.back().body.find(R"("reason":"destination unavailable")") == std::string::npos)
        return 1;
    if (!homeworldz::grid::prepare_avatar_arrival(*transport, transit_request.id) ||
        transport->requests.back().path !=
            "/api/v1/transits/" + transit_request.id + "/prepare-arrival") return 1;
    const homeworldz::grid::TaskInventoryTransferRequest task_transfer_request{
        "99999999-9999-4999-8999-999999999999", transit_request.agent_id,
        "44444444-4444-4444-8444-444444444444", transit_request.destination_region_id,
        "55555555-5555-4555-8555-555555555555",
        "66666666-6666-4666-8666-666666666666"};
    const auto task_transfer = client.prepare_task_inventory_transfer(task_transfer_request);
    if (!task_transfer || task_transfer->state != "prepared" ||
        task_transfer->item.name != "No Copy Texture" ||
        task_transfer->item.current_permissions != 0x00096000 ||
        transport->requests.back().body.find(R"("sourceItemId":"44444444-4444-4444-8444-444444444444")") == std::string::npos)
        return 1;
    const auto pending_task_transfers = client.pending_task_inventory_transfers(
        transit_request.destination_region_id);
    if (!pending_task_transfers || pending_task_transfers->size() != 1 ||
        pending_task_transfers->front().id != task_transfer->id) return 1;
    if (!client.finalize_task_inventory_transfer(
            task_transfer->id, transit_request.destination_region_id) ||
        transport->requests.back().path !=
            "/api/v1/task-transfers/" + task_transfer->id + "/finalize") return 1;
    const homeworldz::grid::TaskInventoryExtractionRequest extraction_request{
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", transit_request.agent_id,
        transit_request.destination_region_id, "55555555-5555-4555-8555-555555555555",
        "66666666-6666-4666-8666-666666666666", "88888888-8888-4888-8888-888888888888",
        "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb",
        {"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", "77777777-7777-4777-8777-777777777777",
         transit_request.agent_id, "88888888-8888-4888-8888-888888888888",
         "dddddddd-dddd-4ddd-8ddd-dddddddddddd", 0, 0, "No Copy Texture", "", 0,
         0x0009e000, 0x00096000, 0, 0x0008a000, 0, 0}};
    const auto extraction = client.prepare_task_inventory_extraction(extraction_request);
    if (!extraction || extraction->state != "prepared" ||
        extraction->item.current_permissions != 0x00096000 ||
        transport->requests.back().body.find(
            R"("sourceTaskItemId":"66666666-6666-4666-8666-666666666666")") == std::string::npos)
        return 1;
    const auto pending_extractions = client.pending_task_inventory_extractions(
        transit_request.destination_region_id);
    if (!pending_extractions || pending_extractions->size() != 1 ||
        pending_extractions->front().id != extraction->id) return 1;
    const auto finalized_extraction = client.finalize_task_inventory_extraction(
        extraction->id, transit_request.destination_region_id);
    if (!finalized_extraction || finalized_extraction->state != "finalized" ||
        transport->requests.back().path !=
            "/api/v1/task-extractions/" + extraction->id + "/finalize") return 1;
    const homeworldz::grid::ObjectRezRequest object_rez_request{
        "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee", transit_request.agent_id,
        "44444444-4444-4444-8444-444444444444", transit_request.destination_region_id,
        "55555555-5555-4555-8555-555555555555"};
    const auto object_rez = client.prepare_object_rez(object_rez_request);
    if (!object_rez || object_rez->state != "prepared" ||
        object_rez->item.name != "No Copy Object" ||
        object_rez->item.current_permissions != 0x00086000 ||
        transport->requests.back().body.find(
            R"("sourceItemId":"44444444-4444-4444-8444-444444444444")") == std::string::npos)
        return 1;
    const auto pending_object_rezzes = client.pending_object_rezzes(
        transit_request.destination_region_id);
    if (!pending_object_rezzes || pending_object_rezzes->size() != 1 ||
        pending_object_rezzes->front().id != object_rez->id) return 1;
    if (!client.finalize_object_rez(object_rez->id, transit_request.destination_region_id) ||
        transport->requests.back().path !=
            "/api/v1/object-rezzes/" + object_rez->id + "/finalize") return 1;
    homeworldz::grid::RegistrationLifecycle provisioned_lifecycle(
        client, provisioned_settings, provisioned->id);
    if (!provisioned_lifecycle.start(started) ||
        !provisioned_lifecycle.tick(started + std::chrono::seconds(30)) ||
        transport->requests.back().path !=
            "/api/v1/region-runtime/22222222-2222-4222-8222-222222222222/lease") return 1;
    provisioned_lifecycle.stop();
    if (transport->requests.back().path !=
        "/api/v1/region-runtime/22222222-2222-4222-8222-222222222222") return 1;
    const auto session = client.validate_viewer_session("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    if (!session || session->agent_id != "cccccccc-cccc-4ccc-8ccc-cccccccccccc" ||
        session->secure_session_id != "dddddddd-dddd-4ddd-8ddd-dddddddddddd" ||
        session->circuit_code != 123456 || session->destination_region_id != "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa" ||
        transport->requests.back().method != "GET") return 1;
    const auto user = client.find_user(session->agent_id);
    if (!user || user->id != session->agent_id || user->username != "jim.tarber" ||
        transport->requests.back().path != "/api/v1/users/" + session->agent_id) return 1;
    homeworldz::grid::ViewerSessionCache cache(client, std::chrono::seconds(5));
    const auto requests_before_cache = transport->requests.size();
    const auto cached = cache.validate(session->session_id, started);
    if (!cached || transport->requests.size() != requests_before_cache + 1) return 1;
    if (!cache.validate(session->session_id, started + std::chrono::seconds(4)) ||
        transport->requests.size() != requests_before_cache + 1) return 1;
    if (!cache.validate(session->session_id, started + std::chrono::seconds(5)) ||
        transport->requests.size() != requests_before_cache + 2) return 1;
    cache.invalidate(session->session_id);
    if (!cache.validate(session->session_id, started + std::chrono::seconds(6)) ||
        transport->requests.size() != requests_before_cache + 3) return 1;
    if (!client.create_inventory_folder(session->agent_id,
            "dddddddd-dddd-4ddd-8ddd-dddddddddddd",
            "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee", "Projects", -1) ||
        transport->requests.back().path != "/api/v1/inventory/" + session->agent_id + "/folders" ||
        transport->requests.back().body.find(R"("name":"Projects")") == std::string::npos ||
        transport->requests.back().body.find(R"("typeDefault":-1)") == std::string::npos) return 1;
    if (!client.move_inventory_folder(session->agent_id,
            "dddddddd-dddd-4ddd-8ddd-dddddddddddd",
            "ffffffff-ffff-4fff-8fff-ffffffffffff") ||
        transport->requests.back().method != "PUT" ||
        transport->requests.back().path != "/api/v1/inventory/" + session->agent_id +
            "/folders/dddddddd-dddd-4ddd-8ddd-dddddddddddd" ||
        transport->requests.back().body.find(
            R"("parentId":"ffffffff-ffff-4fff-8fff-ffffffffffff")") == std::string::npos)
        return 1;
    if (!client.move_inventory_item(session->agent_id,
            "11111111-1111-4111-8111-111111111111",
            "ffffffff-ffff-4fff-8fff-ffffffffffff", "Renamed Texture") ||
        transport->requests.back().method != "PUT" ||
        transport->requests.back().path != "/api/v1/inventory/" + session->agent_id +
            "/items/11111111-1111-4111-8111-111111111111" ||
        transport->requests.back().body.find(
            R"("folderId":"ffffffff-ffff-4fff-8fff-ffffffffffff")") == std::string::npos ||
        transport->requests.back().body.find(R"("name":"Renamed Texture")") == std::string::npos)
        return 1;
    if (!client.update_inventory_item_asset(
            session->agent_id, "11111111-1111-4111-8111-111111111111",
            "33333333-3333-4333-8333-333333333333") ||
        transport->requests.back().method != "PUT" ||
        transport->requests.back().path != "/api/v1/inventory/" + session->agent_id +
            "/items/11111111-1111-4111-8111-111111111111/asset" ||
        transport->requests.back().body.find(
            R"("assetId":"33333333-3333-4333-8333-333333333333")") == std::string::npos)
        return 1;
    const auto objects_folder = client.find_system_inventory_folder(session->agent_id, 6);
    if (!objects_folder || *objects_folder != "22222222-2222-4222-8222-222222222222" ||
        transport->requests.back().method != "GET" ||
        transport->requests.back().path != "/api/v1/inventory/" + session->agent_id + "/system-folders/6")
        return 1;
    const auto found_object = client.find_inventory_item(
        session->agent_id, "44444444-4444-4444-8444-444444444444");
    if (!found_object || found_object->name != "Prim2" || found_object->asset_type != 6 ||
        found_object->asset_id != "66666666-6666-4666-8666-666666666666" ||
        transport->requests.back().path != "/api/v1/inventory/" + session->agent_id +
            "/items/44444444-4444-4444-8444-444444444444")
        return 1;
    const homeworldz::grid::TextureInventoryItem texture{
        "11111111-1111-4111-8111-111111111111", session->agent_id,
        "22222222-2222-4222-8222-222222222222", "33333333-3333-4333-8333-333333333333",
        "Terrain & Sky", "Uploaded <texture>", 0, 0x7fffffff};
    if (!client.create_texture_inventory_item(session->agent_id, texture) ||
        transport->requests.back().path != "/api/v1/inventory/" + session->agent_id + "/items" ||
        transport->requests.back().body.find(R"("assetType":0,"inventoryType":0)") == std::string::npos ||
        transport->requests.back().body.find(R"("creatorUserId":"cccccccc-cccc-4ccc-8ccc-cccccccccccc")") == std::string::npos ||
        transport->requests.back().body.find(R"("name":"Terrain & Sky")") == std::string::npos ||
        transport->requests.back().body.find(R"("nextPermissions":2147483647)") == std::string::npos) return 1;
    const homeworldz::grid::ObjectInventoryItem object{
        "44444444-4444-4444-8444-444444444444", "77777777-7777-4777-8777-777777777777",
        "55555555-5555-4555-8555-555555555555", "66666666-6666-4666-8666-666666666666",
        "Primitive", "", 0x0009e000, 0x0009e000, 0, 0x0008e000};
    if (!client.create_object_inventory_item(session->agent_id, object) ||
        transport->requests.back().body.find(R"("assetType":6,"inventoryType":6)") == std::string::npos ||
        transport->requests.back().body.find(
            R"("creatorUserId":"77777777-7777-4777-8777-777777777777")") == std::string::npos ||
        transport->requests.back().body.find(R"("basePermissions":647168)") == std::string::npos ||
        transport->requests.back().body.find(R"("name":"Primitive")") == std::string::npos) return 1;
    const homeworldz::grid::InventoryItem pants{
        "99999999-9999-4999-8999-999999999999", session->agent_id, session->agent_id,
        "22222222-2222-4222-8222-222222222222", "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
        5, 18, "New Pants", "", 5, 0x7fffffff, 0x7fffffff, 0x00000000, 0x0008e000};
    if (!client.create_inventory_item(session->agent_id, pants) ||
        transport->requests.back().body.find(R"("assetType":5,"inventoryType":18)") == std::string::npos ||
        transport->requests.back().body.find(R"("flags":5)") == std::string::npos ||
        transport->requests.back().body.find(R"("name":"New Pants")") == std::string::npos ||
        transport->requests.back().body.find(R"("nextPermissions":581632)") == std::string::npos)
        return 1;
    if (!client.register_asset(
            "66666666-6666-4666-8666-666666666666",
            "77777777-7777-4777-8777-777777777777",
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
            332, "http://region.example:42001", true) ||
        transport->requests.back().path != "/api/v1/assets" ||
        transport->requests.back().body.find(R"("size":332)") == std::string::npos ||
        transport->requests.back().body.find(R"("origin":true)") == std::string::npos)
        return 1;
    const auto asset = client.find_asset("66666666-6666-4666-8666-666666666666");
    if (!asset || asset->creator_id != "77777777-7777-4777-8777-777777777777" ||
        asset->size != 332 || asset->locations.size() != 2 || !asset->locations[0].origin ||
        asset->locations[1].origin || asset->locations[1].endpoint != "http://replica.example:42001" ||
        transport->requests.back().path != "/api/v1/assets/66666666-6666-4666-8666-666666666666")
        return 1;
    const auto copied = client.copy_library_item(
        session->agent_id, "d5e46210-b9d1-11dc-95ff-0800200c9a66",
        "00000000-0000-0000-0000-000000000000", "");
    if (!copied || copied->owner_id != session->agent_id || copied->creator_id != "00000000-0000-0000-0000-000000000002" ||
        copied->asset_type != 5 || copied->inventory_type != 18 || copied->flags != 4 ||
        copied->base_permissions != 2147483647 || copied->name != "Default Shirt" ||
        transport->requests.back().path != "/api/v1/inventory/" + session->agent_id + "/copy-library-item" ||
        transport->requests.back().body.find(R"("sourceItemId":"d5e46210-b9d1-11dc-95ff-0800200c9a66")") == std::string::npos)
        return 1;
    const auto personal_copy = client.copy_inventory_item(
        session->agent_id, "44444444-4444-4444-8444-444444444444",
        "55555555-5555-4555-8555-555555555555", "Prim1 copy");
    if (!personal_copy || personal_copy->owner_id != session->agent_id ||
        personal_copy->creator_id != "77777777-7777-4777-8777-777777777777" ||
        personal_copy->asset_type != 6 || personal_copy->name != "Prim1 copy" ||
        transport->requests.back().path != "/api/v1/inventory/" + session->agent_id + "/copy-item" ||
        transport->requests.back().body.find(
            R"("sourceItemId":"44444444-4444-4444-8444-444444444444")") == std::string::npos)
        return 1;
    if (!client.update_presence(session->agent_id, session->destination_region_id) ||
        transport->requests.back().method != "PUT" ||
        transport->requests.back().path != "/api/v1/presence/" + session->agent_id) return 1;
    if (!client.update_last_location(
            session->agent_id, session->destination_region_id,
            {123.0F, 128.0F, 35.0F}, {-0.5F, 0.866F, 0.0F}, true) ||
        transport->requests.back().method != "PUT" ||
        transport->requests.back().path != "/api/v1/locations/" + session->agent_id ||
        transport->requests.back().body.find(
            R"("position":{"x":123.000000,"y":128.000000,"z":35.000000})") == std::string::npos ||
        transport->requests.back().body.find(
            R"("lookAt":{"x":-0.500000,"y":0.866000,"z":0.000000})") == std::string::npos ||
        transport->requests.back().body.find(R"("flying":true)") == std::string::npos) return 1;
    if (!client.clear_presence(session->agent_id) || !client.revoke_viewer_session(session->session_id) ||
        transport->requests.back().path != "/api/v1/sessions/" + session->session_id) return 1;

    // An inventory lookup that produced no item has to say why. Reporting an
    // unreachable grid as "the item is not there" accuses a wearer's inventory
    // of a fault that is on this side of the call (client core, 2026-08-08).
    {
        auto absent = std::make_shared<CannedTransport>(404,
            R"({"code":"inventory_item_not_found","message":"inventory item was not found"})");
        homeworldz::grid::Client absent_client(absent);
        const auto missing = absent_client.lookup_inventory_item(
            "cccccccc-cccc-4ccc-8ccc-cccccccccccc", "11111111-1111-4111-8111-111111111111");
        if (missing.outcome != homeworldz::grid::InventoryLookup::missing || missing.item) return 1;

        // The grid's bare not_found is an unserved route. It is a 404 like the
        // one above and means something entirely different: this build did not
        // ask the question, so it has learned nothing about the item.
        auto unserved = std::make_shared<CannedTransport>(404,
            R"({"code":"not_found","message":"route not found"})");
        homeworldz::grid::Client unserved_client(unserved);
        if (unserved_client.lookup_inventory_item(
                "cccccccc-cccc-4ccc-8ccc-cccccccccccc",
                "11111111-1111-4111-8111-111111111111").outcome !=
            homeworldz::grid::InventoryLookup::unavailable) return 1;

        for (const int status : {401, 503}) {
            auto refusing_lookup = std::make_shared<CannedTransport>(status,
                R"({"code":"ticket_validation_unavailable","message":"unavailable"})");
            homeworldz::grid::Client refusing_lookup_client(refusing_lookup);
            if (refusing_lookup_client.lookup_inventory_item(
                    "cccccccc-cccc-4ccc-8ccc-cccccccccccc",
                    "11111111-1111-4111-8111-111111111111").outcome !=
                homeworldz::grid::InventoryLookup::unavailable) return 1;
        }
        // A 200 this build cannot parse is not an absent item either.
        auto unreadable = std::make_shared<CannedTransport>(200, R"({"unexpected":true})");
        homeworldz::grid::Client unreadable_client(unreadable);
        if (unreadable_client.lookup_inventory_item(
                "cccccccc-cccc-4ccc-8ccc-cccccccccccc",
                "11111111-1111-4111-8111-111111111111").outcome !=
            homeworldz::grid::InventoryLookup::unavailable) return 1;
    }

    // Worn attachments. The list an arriving avatar is rezzed from, so what it
    // refuses matters as much as what it parses.
    const std::string worn_user = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
    {
        auto worn_transport = std::make_shared<CannedTransport>(200,
            R"([{"itemId":"11111111-1111-4111-8111-111111111111","attachmentPoint":5},)"
            R"({"itemId":"22222222-2222-4222-8222-222222222222","attachmentPoint":31}])");
        homeworldz::grid::Client worn_client(worn_transport);
        const auto worn = worn_client.worn_attachments(worn_user);
        if (!worn || worn->size() != 2) return 1;
        if ((*worn)[0].item_id != "11111111-1111-4111-8111-111111111111" ||
            (*worn)[0].attachment_point != 5 ||
            (*worn)[1].attachment_point != 31) return 1;
        if (worn_transport->requests.back().method != "GET" ||
            worn_transport->requests.back().path != "/api/v1/attachments/" + worn_user) return 1;
    }
    {
        // Wearing nothing is a fact and must parse as an empty list. If it came
        // back as nullopt an arrival would log "grid could not answer" every
        // time anyone with an empty wardrobe arrived.
        auto empty_transport = std::make_shared<CannedTransport>(200, "[]");
        homeworldz::grid::Client empty_client(empty_transport);
        const auto worn = empty_client.worn_attachments(worn_user);
        if (!worn || !worn->empty()) return 1;
    }
    {
        // A grid that cannot answer is not an empty wardrobe. Arrival treats
        // these differently: one strips nothing, the other would be taken as
        // "you are wearing nothing" and leave the avatar bare.
        auto refusing = std::make_shared<CannedTransport>(503,
            R"({"code":"attachment_store_unavailable","message":"attachment storage is unavailable"})");
        homeworldz::grid::Client refusing_client(refusing);
        if (refusing_client.worn_attachments(worn_user)) return 1;
    }
    {
        // Point 0 means "wherever the item says" — a question, not a place. A
        // row carrying one cannot be acted on, so the whole answer is refused
        // rather than one item being rezzed somewhere invented.
        auto unresolved = std::make_shared<CannedTransport>(200,
            R"([{"itemId":"11111111-1111-4111-8111-111111111111","attachmentPoint":0}])");
        homeworldz::grid::Client unresolved_client(unresolved);
        if (unresolved_client.worn_attachments(worn_user)) return 1;
        auto nameless = std::make_shared<CannedTransport>(200, R"([{"attachmentPoint":5}])");
        homeworldz::grid::Client nameless_client(nameless);
        if (nameless_client.worn_attachments(worn_user)) return 1;
    }
    {
        auto writes = std::make_shared<CannedTransport>(204, std::string{});
        homeworldz::grid::Client write_client(writes);
        if (!write_client.set_attachment_worn(worn_user, "11111111-1111-4111-8111-111111111111", 5, true))
            return 1;
        if (writes->requests.back().method != "PUT" ||
            writes->requests.back().path != "/api/v1/attachments/" + worn_user ||
            writes->requests.back().body.find(R"("attachmentPoint":5)") == std::string::npos ||
            writes->requests.back().body.find(R"("worn":true)") == std::string::npos) return 1;
        if (!write_client.set_attachment_worn(worn_user, "11111111-1111-4111-8111-111111111111", 0, false) ||
            writes->requests.back().body.find(R"("worn":false)") == std::string::npos) return 1;
    }
    return 0;
}
