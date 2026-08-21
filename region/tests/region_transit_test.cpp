#include "homeworldz/region_transit.h"

#include <chrono>
#include <vector>

int main() {
    using namespace std::chrono_literals;
    const auto now = std::chrono::steady_clock::time_point{};
    constexpr std::string_view destination = "22222222-2222-4222-8222-222222222222";
    const auto gamma_origin =
        (static_cast<std::uint64_t>(1004 * 256) << 32) | static_cast<std::uint32_t>(1000 * 256);
    const auto gamma_internal =
        (static_cast<std::uint64_t>(1006 * 256) << 32) | static_cast<std::uint32_t>(1002 * 256);
    const auto origin_teleport = homeworldz::region::resolve_region_teleport_position(
        1004, 1000, 1024, 1024, gamma_origin, {512.0F, 512.0F, 30.0F});
    if (!origin_teleport || *origin_teleport != std::array<float, 3>{512.0F, 512.0F, 30.0F})
        return 1;
    const auto tiled_teleport = homeworldz::region::resolve_region_teleport_position(
        1004, 1000, 1024, 1024, gamma_internal, {0.0F, 0.0F, 30.0F});
    if (!tiled_teleport || *tiled_teleport != std::array<float, 3>{512.0F, 512.0F, 30.0F})
        return 1;
    if (homeworldz::region::resolve_region_teleport_position(
            1004, 1000, 1024, 1024, gamma_internal, {700.0F, 0.0F, 30.0F}) ||
        homeworldz::region::resolve_region_teleport_position(
            1004, 1000, 512, 512, gamma_internal, {0.0F, 0.0F, 30.0F}))
        return 1;
    homeworldz::grid::AvatarTransit transit{
        "33333333-3333-4333-8333-333333333333", 1,
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
        "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb",
        "11111111-1111-4111-8111-111111111111", std::string(destination),
        {128.0F, 64.0F, 30.0F}, {1.0F, 0.0F, 0.0F}, true, "accepted"};
    homeworldz::region::InboundTransitRegistry registry;
    if (!registry.stage(transit, destination, now, 30s) || registry.size(now) != 1) return 1;
    if (!registry.stage(transit, destination, now + 1s, 30s) || registry.size(now + 1s) != 1) return 1;
    if (registry.authorize("wrong-agent", transit.session_id, now + 2s)) return 1;
    const auto* authorized = registry.authorize(transit.agent_id, transit.session_id, now + 2s);
    if (!authorized || authorized->position != transit.position || !authorized->flying) return 1;
    auto conflict = transit;
    conflict.id = "44444444-4444-4444-8444-444444444444";
    if (registry.stage(conflict, destination, now + 2s, 30s)) return 1;
    const auto consumed = registry.consume(transit.session_id, now + 3s);
    if (!consumed || consumed->id != transit.id || registry.size(now + 3s) != 0) return 1;

    transit.state = "prepared";
    if (registry.stage(transit, destination, now, 30s)) return 1;
    transit.state = "accepted";
    if (registry.stage(transit, "55555555-5555-4555-8555-555555555555", now, 30s)) return 1;
    if (!registry.stage(transit, destination, now, 10s)) return 1;
    if (registry.authorize(transit.agent_id, transit.session_id, now + 10s) ||
        registry.size(now + 10s) != 0) return 1;

    homeworldz::region::CapabilityArrivalGate arrival_gate;
    if (arrival_gate.mark_seed_served({}, transit.id) ||
        arrival_gate.mark_seed_served(transit.session_id, {}) ||
        arrival_gate.consume_seed(transit.session_id, transit.id)) return 1;
    if (!arrival_gate.mark_seed_served(transit.session_id, transit.id) ||
        arrival_gate.mark_seed_served(transit.session_id, transit.id) ||
        arrival_gate.size() != 1 ||
        !arrival_gate.consume_seed(transit.session_id, transit.id) ||
        arrival_gate.consume_seed(transit.session_id, transit.id) ||
        arrival_gate.size() != 0) return 1;
    if (!arrival_gate.mark_seed_served(transit.session_id, transit.id)) return 1;
    auto second_transit = transit;
    second_transit.id = "44444444-4444-4444-8444-444444444444";
    if (!arrival_gate.mark_seed_served(transit.session_id, second_transit.id) ||
        arrival_gate.size() != 2) return 1;
    arrival_gate.clear_session(transit.session_id);
    if (arrival_gate.size() != 0) return 1;

    const homeworldz::grid::RegionNeighbor beta{
        {"beta", "Beta", 1002, 1000, 512, 512, 13,
         "http://region.example:42021", 42022, true},
        "east"};
    const std::vector sandbox_neighbors{beta};
    const auto into_beta = homeworldz::region::plan_border_crossing(
        1001, 1000, 256, 256, {256.2, 200.0, 30.0}, sandbox_neighbors);
    if (!into_beta || into_beta->destination.id != "beta" ||
        into_beta->position != std::array<float, 3>{0.3F, 200.0F, 30.0F}) return 1;

    const homeworldz::grid::RegionNeighbor sandbox{
        {"sandbox", "Sandbox", 1001, 1000, 256, 256, 13,
         "http://region.example:42001", 42002, true},
        "west"};
    const std::vector beta_neighbors{sandbox};
    const auto into_sandbox = homeworldz::region::plan_border_crossing(
        1002, 1000, 512, 512, {-0.2, 200.0, 31.0}, beta_neighbors);
    if (!into_sandbox || into_sandbox->destination.id != "sandbox" ||
        into_sandbox->position != std::array<float, 3>{255.7F, 200.0F, 31.0F}) return 1;
    if (homeworldz::region::plan_border_crossing(
            1002, 1000, 512, 512, {-0.2, 400.0, 31.0}, beta_neighbors)) return 1;
    auto offline_sandbox = sandbox;
    offline_sandbox.online = false;
    const std::vector offline_neighbors{offline_sandbox};
    if (homeworldz::region::plan_border_crossing(
            1002, 1000, 512, 512, {-0.2, 200.0, 31.0}, offline_neighbors)) return 1;

    // An object crossing survives the round trip whole. Every field is checked,
    // because the fields the asset cannot carry are exactly the ones a crossing
    // is capable of silently dropping.
    homeworldz::region::ObjectTransit outbound{
        "55555555-5555-4555-8555-555555555555",
        "11111111-1111-4111-8111-111111111111", std::string(destination),
        "66666666-6666-4666-8666-666666666666",
        {"77777777-7777-4777-8777-777777777777"},
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
        1723500000ULL,
        {0.25, 200.5, 31.75},
        {0.0, 0.0, 0.7071067811865476, 0.7071067811865476},
        {-3.5, 0.125, -9.8},
        {0.0, 1.25, -0.5},
        std::string("{\"format\":\"homeworldz-linkset-v1\",\"parts\":[{\"name\":\"Crate\"},"
                    "{\"name\":\"Lid\"}]}")};
    const auto document = homeworldz::region::encode_object_transit(outbound);
    const auto inbound = homeworldz::region::parse_object_transit(document);
    if (!inbound || inbound->id != outbound.id ||
        inbound->source_region_id != outbound.source_region_id ||
        inbound->destination_region_id != outbound.destination_region_id ||
        inbound->object_id != outbound.object_id ||
        inbound->child_object_ids != outbound.child_object_ids ||
        inbound->owner_id != outbound.owner_id ||
        inbound->creation_date != outbound.creation_date ||
        inbound->position != outbound.position ||
        inbound->rotation != outbound.rotation ||
        inbound->linear_velocity != outbound.linear_velocity ||
        inbound->angular_velocity != outbound.angular_velocity ||
        inbound->linkset != outbound.linkset)
        return 1;
    // A truncated vector is a malformed document, never a vector of zeroes.
    if (homeworldz::region::parse_object_transit(
            std::string(document).replace(document.find("\"linearVelocity\":["),
                                          std::string_view("\"linearVelocity\":[").size() + 4,
                                          "\"linearVelocity\":[1,2")))
        return 1;
    if (homeworldz::region::parse_object_transit("{}")) return 1;

    homeworldz::region::InboundObjectRegistry objects;
    if (objects.stage(outbound, "wrong-region", now)) return 1;
    if (!objects.stage(outbound, destination, now) || objects.size(now) != 1) return 1;
    // Re-staging the same transit is a retry, not a second object.
    if (!objects.stage(outbound, destination, now + 1s) || objects.size(now + 1s) != 1) return 1;
    auto impostor = outbound;
    impostor.object_id = "88888888-8888-4888-8888-888888888888";
    if (objects.stage(impostor, destination, now + 1s)) return 1;
    const auto activated = objects.activate(outbound.id, now + 2s);
    if (!activated || activated->object_id != outbound.object_id ||
        objects.size(now + 2s) != 0)
        return 1;
    // Activation is one-shot until the arrival is recorded, and idempotent
    // after: a source that never heard the reply must not rez a second object.
    if (objects.activate(outbound.id, now + 2s) || objects.arrived(outbound.id)) return 1;
    objects.note_arrival(outbound.id, now + 2s);
    if (!objects.arrived(outbound.id)) return 1;
    if (!objects.stage(outbound, destination, now + 3s) || objects.size(now + 3s) != 0) return 1;
    if (objects.activate(outbound.id, now + 3s)) return 1;
    // Staging that is never activated expires having created nothing.
    homeworldz::region::ObjectTransit stale = outbound;
    stale.id = "99999999-9999-4999-8999-999999999999";
    if (!objects.stage(stale, destination, now + 3s, 30s)) return 1;
    if (objects.size(now + 34s) != 0 || objects.activate(stale.id, now + 34s)) return 1;

    // Child agents (ADR 0038).
    homeworldz::region::ChildAgentRegistry children;
    homeworldz::region::ChildAgent visitor;
    visitor.agent_id = "33333333-3333-4333-8333-333333333333";
    visitor.session_id = "44444444-4444-4444-8444-444444444444";
    visitor.circuit_code = 7;
    visitor.home_region_id = std::string(destination);
    visitor.seed = "seed-one";
    visitor.position = {10.0F, 20.0F, 30.0F};
    visitor.worn = {{"66666666-6666-4666-8666-666666666666", 33},
                    {"77777777-7777-4777-8777-777777777777", 5}};
    if (children.establish(visitor, now).seed != "seed-one") return 1;
    if (children.size(now) != 1) return 1;
    // A retry carries a fresh seed and must not take effect: the viewer already
    // opened a circuit against the first one, and replacing it strands that
    // circuit. Everything else the retry says is newer and does apply.
    auto retried = visitor;
    retried.seed = "seed-two";
    retried.position = {11.0F, 21.0F, 31.0F};
    const auto& refreshed = children.establish(retried, now + 1s);
    if (refreshed.seed != "seed-one") return 1;
    if (refreshed.position != std::array<float, 3>{11.0F, 21.0F, 31.0F}) return 1;
    if (children.size(now + 1s) != 1) return 1;
    // Establishing refreshed the lease, so the original deadline has no effect.
    if (children.find(visitor.session_id, now + 299s) == nullptr) return 1;
    // A promotion answers once: an avatar cannot arrive twice, and a retry that
    // appeared to work would leave a child and a root for one session.
    const auto promoted = children.promote(visitor.session_id, now + 2s);
    if (!promoted || promoted->seed != "seed-one" || promoted->circuit_code != 7) return 1;
    if (children.promote(visitor.session_id, now + 2s)) return 1;
    if (children.size(now + 2s) != 0) return 1;
    // A child nobody refreshed expires, and a promotion then finds nothing —
    // which is the case the cold arrival path still has to answer.
    auto forgotten = visitor;
    forgotten.session_id = "55555555-5555-4555-8555-555555555555";
    children.establish(forgotten, now, 300s);
    if (children.find(forgotten.session_id, now + 301s) != nullptr) return 1;
    if (children.promote(forgotten.session_id, now + 301s) ||
        children.size(now + 301s) != 0) return 1;
    // A neighbour that restarted holds nothing, so it mints afresh rather than
    // answering with a seed it no longer has.
    auto reestablished = forgotten;
    reestablished.seed = "seed-three";
    if (children.establish(reestablished, now + 302s).seed != "seed-three") return 1;
    children.remove(forgotten.session_id);
    if (children.size(now + 302s) != 0) return 1;

    // The establishment call's wire format.
    const auto request = homeworldz::region::encode_child_agent_request(visitor);
    const auto decoded = homeworldz::region::parse_child_agent_request(request);
    if (!decoded || decoded->agent_id != visitor.agent_id ||
        decoded->session_id != visitor.session_id ||
        decoded->circuit_code != visitor.circuit_code ||
        decoded->home_region_id != visitor.home_region_id ||
        decoded->position != visitor.position) return 1;
    // The worn set rides with the establishment, in order, with its points.
    if (decoded->worn.size() != 2 ||
        decoded->worn[0].item_id != visitor.worn[0].item_id ||
        decoded->worn[0].attachment_point != 33 ||
        decoded->worn[1].item_id != visitor.worn[1].item_id ||
        decoded->worn[1].attachment_point != 5) return 1;
    // The seed is the destination's to mint. A request that names one is
    // accepted with the name dropped, not honoured: a source that could choose
    // it could choose a capability path on a region it does not run.
    if (!decoded->seed.empty()) return 1;
    const auto forged = std::string(
        "{\"agentId\":\"33333333-3333-4333-8333-333333333333\""
        ",\"sessionId\":\"44444444-4444-4444-8444-444444444444\""
        ",\"circuitCode\":7,\"homeRegionId\":\"") + std::string(destination) +
        "\",\"position\":[1.0,2.0,3.0],\"worn\":[]"
        ",\"seed\":\"/caps/seed/somebody-elses\"}";
    const auto sanitized = homeworldz::region::parse_child_agent_request(forged);
    if (!sanitized || !sanitized->seed.empty()) return 1;
    // A circuit code of zero is not a circuit, and it is what both an absent
    // field and a malformed one become once they are numbers.
    const auto no_circuit = std::string(
        "{\"agentId\":\"33333333-3333-4333-8333-333333333333\""
        ",\"sessionId\":\"44444444-4444-4444-8444-444444444444\""
        ",\"circuitCode\":0,\"homeRegionId\":\"") + std::string(destination) +
        "\",\"position\":[1.0,2.0,3.0]}";
    if (homeworldz::region::parse_child_agent_request(no_circuit)) return 1;
    if (homeworldz::region::parse_child_agent_request("{}")) return 1;
    // Wearing nothing is an answer; a worn set that cannot be read is not, and
    // must not arrive as a short one. Half a worn set dresses the avatar in some
    // of its clothes and looks like a success.
    const auto bare = std::string(
        "{\"agentId\":\"33333333-3333-4333-8333-333333333333\""
        ",\"sessionId\":\"44444444-4444-4444-8444-444444444444\""
        ",\"circuitCode\":7,\"homeRegionId\":\"") + std::string(destination) +
        "\",\"position\":[1.0,2.0,3.0],\"worn\":[]}";
    const auto naked = homeworldz::region::parse_child_agent_request(bare);
    if (!naked || !naked->worn.empty()) return 1;
    // A point of zero is "wherever the item says", resolved long before worn
    // state is stored, so it is a question arriving where an answer belongs.
    const auto unresolved = std::string(
        "{\"agentId\":\"33333333-3333-4333-8333-333333333333\""
        ",\"sessionId\":\"44444444-4444-4444-8444-444444444444\""
        ",\"circuitCode\":7,\"homeRegionId\":\"") + std::string(destination) +
        "\",\"position\":[1.0,2.0,3.0]"
        ",\"worn\":[{\"itemId\":\"66666666-6666-4666-8666-666666666666\""
        ",\"attachmentPoint\":0}]}";
    if (homeworldz::region::parse_child_agent_request(unresolved)) return 1;
    // One unreadable element refuses the whole request rather than dropping it.
    const auto partial = std::string(
        "{\"agentId\":\"33333333-3333-4333-8333-333333333333\""
        ",\"sessionId\":\"44444444-4444-4444-8444-444444444444\""
        ",\"circuitCode\":7,\"homeRegionId\":\"") + std::string(destination) +
        "\",\"position\":[1.0,2.0,3.0]"
        ",\"worn\":[{\"itemId\":\"66666666-6666-4666-8666-666666666666\""
        ",\"attachmentPoint\":33},{\"attachmentPoint\":5}]}";
    if (homeworldz::region::parse_child_agent_request(partial)) return 1;
    // An absent worn field is not an empty one: the source did not say.
    const auto silent = std::string(
        "{\"agentId\":\"33333333-3333-4333-8333-333333333333\""
        ",\"sessionId\":\"44444444-4444-4444-8444-444444444444\""
        ",\"circuitCode\":7,\"homeRegionId\":\"") + std::string(destination) +
        "\",\"position\":[1.0,2.0,3.0]}";
    if (homeworldz::region::parse_child_agent_request(silent)) return 1;
    // Appearance metadata rides with the establishment; the assets it names do
    // not. Round trips through base64 with its version intact.
    auto dressed = visitor;
    dressed.texture_entry = {std::byte{0x01}, std::byte{0xff}, std::byte{0x00}, std::byte{0x7f}};
    dressed.visual_params = {0, 1, 128, 255};
    dressed.cof_version = 4321;
    dressed.appearance_version = 1;
    const auto dressed_wire = homeworldz::region::encode_child_agent_request(dressed);
    const auto dressed_back = homeworldz::region::parse_child_agent_request(dressed_wire);
    if (!dressed_back || !dressed_back->has_appearance() ||
        dressed_back->texture_entry != dressed.texture_entry ||
        dressed_back->visual_params != dressed.visual_params ||
        dressed_back->cof_version != 4321 ||
        dressed_back->appearance_version != 1) return 1;
    // No appearance at all is legitimate: the source has not established one.
    if (decoded->has_appearance()) return 1;
    // Half an appearance is not a smaller one. Either half alone is refused.
    const auto half = std::string(
        "{\"agentId\":\"33333333-3333-4333-8333-333333333333\""
        ",\"sessionId\":\"44444444-4444-4444-8444-444444444444\""
        ",\"circuitCode\":7,\"homeRegionId\":\"") + std::string(destination) +
        "\",\"position\":[1.0,2.0,3.0],\"worn\":[]"
        ",\"textureEntry\":\"AQIDBA==\"}";
    if (homeworldz::region::parse_child_agent_request(half)) return 1;
    // And the other way round, which is the direction that matters: without the
    // pairing rule this one does not fail, it silently becomes an establishment
    // with no appearance at all. Found by removing the rule and watching the
    // test above still pass.
    const auto half_reversed = std::string(
        "{\"agentId\":\"33333333-3333-4333-8333-333333333333\""
        ",\"sessionId\":\"44444444-4444-4444-8444-444444444444\""
        ",\"circuitCode\":7,\"homeRegionId\":\"") + std::string(destination) +
        "\",\"position\":[1.0,2.0,3.0],\"worn\":[]"
        ",\"visualParams\":\"AQIDBA==\",\"cofVersion\":1,\"appearanceVersion\":1}";
    if (homeworldz::region::parse_child_agent_request(half_reversed)) return 1;
    // An appearance version the encoder cannot express is refused here, where
    // the refusal can be traced, rather than there, where it becomes an empty
    // message nobody can attribute.
    const auto impossible = std::string(
        "{\"agentId\":\"33333333-3333-4333-8333-333333333333\""
        ",\"sessionId\":\"44444444-4444-4444-8444-444444444444\""
        ",\"circuitCode\":7,\"homeRegionId\":\"") + std::string(destination) +
        "\",\"position\":[1.0,2.0,3.0],\"worn\":[]"
        ",\"textureEntry\":\"AQIDBA==\",\"visualParams\":\"AQIDBA==\""
        ",\"cofVersion\":1,\"appearanceVersion\":7}";
    if (homeworldz::region::parse_child_agent_request(impossible)) return 1;
    // The answer, and the refusal a source must not announce a neighbour on.
    if (homeworldz::region::parse_child_agent_acceptance(
            homeworldz::region::encode_child_agent_acceptance("/caps/seed/abc")) !=
        "/caps/seed/abc") return 1;
    if (!homeworldz::region::parse_child_agent_acceptance("{}").empty()) return 1;
    return 0;
}
