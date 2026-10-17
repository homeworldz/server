#include "homeworldz/region_transit.h"

#include <chrono>
#include <string>
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
        std::array<int, 2>{900, 899},
        {0.0, 0.0, 0.7071067811865476, 0.7071067811865476},
        {-3.5, 0.125, -9.8},
        {0.0, 1.25, -0.5},
        // Two riders, each with a position unlike the object's, so a parser
        // that read the wrong "position" would be caught.
        {{"88888888-8888-4888-8888-888888888888", "77777777-7777-4777-8777-777777777777",
          {0.75, -0.5, 1.3156}, {0.0, 0.0, 0.3826834323650898}},
         {"99999999-9999-4999-8999-999999999999", "66666666-6666-4666-8666-666666666666",
          {0.0, 0.0, 0.5156}, {0.0, 0.0, 0.0}}},
        std::string("{\"format\":\"homeworldz-linkset-v1\",\"parts\":[{\"name\":\"Crate\","
                    "\"position\":[9,9,9]},{\"name\":\"Lid\"}]}")};
    const auto document = homeworldz::region::encode_object_transit(outbound);
    const auto inbound = homeworldz::region::parse_object_transit(document);
    if (!inbound || inbound->sitters.size() != 2 || inbound->facet_grid != outbound.facet_grid)
        return 1;
    // Without the corner, the destination's own: what an older source meant.
    {
        auto cornerless = outbound;
        cornerless.facet_grid.reset();
        const auto parsed = homeworldz::region::parse_object_transit(
            homeworldz::region::encode_object_transit(cornerless));
        if (!parsed || parsed->facet_grid) return 1;
    }
    for (std::size_t index = 0; index < 2; ++index) {
        const auto& sent = outbound.sitters[index];
        const auto& read = inbound->sitters[index];
        if (read.agent_id != sent.agent_id || read.prim_object_id != sent.prim_object_id ||
            read.position != sent.position || read.rotation != sent.rotation)
            return 1;
    }
    // A source that predates seated crossings sends no riders at all, which is
    // an empty list and not a malformed document.
    {
        auto older = document;
        const auto from = older.find(",\"sitters\":");
        older.erase(from, older.find(",\"linkset\":") - from);
        const auto parsed = homeworldz::region::parse_object_transit(older);
        if (!parsed || !parsed->sitters.empty() || parsed->position != outbound.position) return 1;
    }
    // A rider half-described is refused whole.
    if (homeworldz::region::parse_object_transit(std::string(document).replace(
            document.find("\"primObjectId\":"), std::string_view("\"primObjectId\":").size(),
            "\"primObject\":")))
        return 1;
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
        ",\"homeRegionId\":\"") + std::string(destination) +
        "\",\"position\":[1.0,2.0,3.0],\"worn\":[]}";
    std::string refusal;
    if (homeworldz::region::parse_child_agent_request(no_circuit, &refusal)) return 1;
    if (refusal != "circuit_code") return 1;
    if (homeworldz::region::parse_child_agent_request("{}", &refusal)) return 1;
    if (refusal != "identity") return 1;
    // A circuit code of zero is not malformed: it is what every avatar on the
    // client session transport has, since a circuit is an LLUDP concept. This
    // was refused as malformed, so two client-only regions refused each other's
    // child agents 400 in both directions, always (2026-08-24).
    const auto session_avatar = std::string(
        "{\"agentId\":\"33333333-3333-4333-8333-333333333333\""
        ",\"sessionId\":\"44444444-4444-4444-8444-444444444444\""
        ",\"circuitCode\":0,\"homeRegionId\":\"") + std::string(destination) +
        "\",\"position\":[1.0,2.0,3.0],\"worn\":[]}";
    const auto without_circuit = homeworldz::region::parse_child_agent_request(session_avatar);
    if (!without_circuit || without_circuit->circuit_code != 0 ||
        without_circuit->agent_id != "33333333-3333-4333-8333-333333333333") return 1;
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
    // The answer carries the seed and only the seed. Which facet of a neighbour
    // borders you is the grid's answer, not the destination's — two authorities
    // for one fact is what pointed a viewer at the wrong half of a neighbour.
    {
        // The child lifecycle: renewed by its session's own traffic, released
        // when its home region says the session is done, and expiring only
        // when neither happens.
        using namespace std::chrono_literals;
        homeworldz::region::ChildAgentRegistry registry;
        homeworldz::region::ChildAgent visitor;
        visitor.agent_id = "33333333-3333-4333-8333-333333333333";
        visitor.session_id = "44444444-4444-4444-8444-444444444444";
        visitor.home_region_id = std::string(destination);
        const auto start = std::chrono::steady_clock::time_point{};
        registry.establish(visitor, start, 300s);

        // Four minutes on, a capability request renews it; five minutes after
        // *that* it is still there. Without renewal the entry would have gone
        // at start + 300s, which is what killed a viewer's standing circuit to
        // a neighbour while it was still holding it.
        if (!registry.renew(visitor.session_id, start + 240s, 300s)) return 1;
        if (registry.find(visitor.session_id, start + 480s) == nullptr) return 1;
        // A session nobody has heard from does still expire.
        if (registry.find(visitor.session_id, start + 600s) != nullptr) return 1;

        // Release ends it at once, and is idempotent: releasing what is gone,
        // or what a crossing already promoted, is the outcome the caller
        // wanted either way.
        registry.establish(visitor, start, 300s);
        if (registry.find(visitor.session_id, start + 10s) == nullptr) return 1;
        registry.remove(visitor.session_id);
        if (registry.find(visitor.session_id, start + 10s) != nullptr) return 1;
        registry.remove(visitor.session_id);
        registry.remove("");
        // Renewing what is not there says so rather than creating it.
        if (registry.renew(visitor.session_id, start + 10s, 300s)) return 1;
        if (registry.size(start + 10s) != 0) return 1;
    }
    if (homeworldz::region::parse_child_agent_acceptance(
            homeworldz::region::encode_child_agent_acceptance("/caps/seed/abc")) !=
        "/caps/seed/abc") return 1;
    // And an answer with no seed is a refusal, which a source must not announce
    // a neighbour on.
    if (!homeworldz::region::parse_child_agent_acceptance("{}").empty()) return 1;
    // A farewell to a child circuit names what that circuit was shown, so a
    // viewer whose avatar is next door drops this region's content instead of
    // keeping a frozen copy of it at its last position.
    {
        constexpr std::string_view watcher = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
        constexpr std::string_view resident = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
        const std::vector<homeworldz::region::FacetOccupant> occupants{
            {10, 0, {}},                       // an object in facet 0
            {11, 1, {}},                       // an object in facet 1
            {20, 0, std::string(resident)},    // someone standing in facet 0
            {21, 0, std::string(watcher)},     // the watcher's own avatar
            {22, 1, std::string(resident)},    // and someone in the other facet
        };
        const auto facet0 = homeworldz::region::child_circuit_farewell_ids(
            occupants, 0, watcher);
        // The object and the other resident, and neither the watcher's own
        // avatar nor anything belonging to a facet this circuit never carried.
        if (facet0 != std::vector<std::uint32_t>{10, 20}) return 30;
        const auto facet1 = homeworldz::region::child_circuit_farewell_ids(
            occupants, 1, watcher);
        if (facet1 != std::vector<std::uint32_t>{11, 22}) return 31;
        // The watcher's own avatar is excluded because it is the recipient, not
        // because of anything about that avatar: to a circuit that is not its
        // own, it is an avatar like any other.
        const auto seen_by_resident = homeworldz::region::child_circuit_farewell_ids(
            occupants, 0, resident);
        if (seen_by_resident != std::vector<std::uint32_t>{10, 21}) return 32;
        // A facet holding nothing yields nothing, so a farewell for it is not
        // sent at all rather than sent empty.
        if (!homeworldz::region::child_circuit_farewell_ids(occupants, 7, watcher).empty())
            return 33;
        // An object never carries a session id, so an empty recipient — which
        // a malformed child record could produce — must not sweep every object
        // out of the farewell.
        if (homeworldz::region::child_circuit_farewell_ids(occupants, 0, "") !=
            std::vector<std::uint32_t>{10, 20, 21}) return 34;
    }

    // Which sides an avatar may leave by, shaped exactly as the grid's
    // neighbours endpoint shapes them — sizes in METRES, one entry per facet.
    // Four live faults in one evening came from getting this wrong, and none of
    // them would have failed a test, because there wasn't one.
    {
        // Nova: 2x1 at (900,900), so facet 0 is cell (900,900) and facet 1 is
        // (901,900). Its only neighbours are Nova B's two facets, to the south
        // and southwest of facet 0 — nothing lies east, north, or west.
        homeworldz::grid::RegionNeighbor south;
        south.direction = "south";
        south.grid_x = 900; south.grid_y = 899;
        south.size_x = 256; south.size_y = 256; south.online = true;
        homeworldz::grid::RegionNeighbor southwest;
        southwest.direction = "southwest";
        southwest.grid_x = 899; southwest.grid_y = 899;
        southwest.size_x = 256; southwest.size_y = 256; southwest.online = true;
        const homeworldz::grid::RegionNeighbor both[]{south, southwest};

        // Standing on the western facet: south is the way to Nova B 2.
        const auto west_facet = homeworldz::region::open_sides_at(900, 900, 512, 256, {128.0, 128.0}, both);
        if (!west_facet.south) return 35;
        // And nothing else is. "southwest" must not open the west: west of
        // Nova is empty, and a corner is reached by two edges, not one.
        if (west_facet.west || west_facet.east || west_facet.north) return 36;

        // Standing on the eastern facet, one cell further along: the same
        // region, and now south is empty too. A region-wide answer got this
        // wrong and walked an avatar out of the map.
        const auto east_facet = homeworldz::region::open_sides_at(900, 900, 512, 256, {384.0, 128.0}, both);
        if (east_facet.south || east_facet.east || east_facet.north || east_facet.west)
            return 37;

        // The units. A facet reports 256x256 metres and is ONE cell; read as
        // cells it would cover a 256-cell square and open every side there is.
        const auto far_north = homeworldz::region::open_sides_at(900, 1050, 512, 256, {128.0, 128.0}, both);
        if (far_north.south || far_north.north || far_north.east || far_north.west)
            return 38;

        // An offline neighbour is not a way out.
        homeworldz::grid::RegionNeighbor dark = south;
        dark.online = false;
        const homeworldz::grid::RegionNeighbor unlit[]{dark};
        if (homeworldz::region::open_sides_at(900, 900, 512, 256, {128.0, 128.0}, unlit).south)
            return 39;

        // A 2x1 neighbour announced as one entry covers both of its cells.
        homeworldz::grid::RegionNeighbor wide;
        wide.direction = "south";
        wide.grid_x = 900; wide.grid_y = 899;
        wide.size_x = 512; wide.size_y = 256; wide.online = true;
        const homeworldz::grid::RegionNeighbor spanning[]{wide};
        if (!homeworldz::region::open_sides_at(900, 900, 512, 256, {128.0, 128.0}, spanning).south)
            return 40;
        if (!homeworldz::region::open_sides_at(900, 900, 512, 256, {384.0, 128.0}, spanning).south)
            return 41;

        // A few centimetres past an open edge is still this region's cell. The
        // values are the ones logged on the Nova/Lazy seam (2026-10-01): taken
        // unclamped, y = -0.024 named Lazy's cell, so south read closed and
        // west read open, and the avatar bounced off the border.
        const auto stepping_south =
            homeworldz::region::open_sides_at(900, 900, 512, 256, {235.987, -0.02071}, both);
        if (!stepping_south.south || stepping_south.west || stepping_south.east ||
            stepping_south.north)
            return 42;

        // The same from Lazy's side: 2x1 at (899,899), Nova's two facets to the
        // north and northeast. At y = 256.036 north closed and east opened.
        homeworldz::grid::RegionNeighbor north;
        north.direction = "north";
        north.grid_x = 900; north.grid_y = 900;
        north.size_x = 256; north.size_y = 256; north.online = true;
        homeworldz::grid::RegionNeighbor northeast = north;
        northeast.direction = "northeast";
        northeast.grid_x = 901;
        const homeworldz::grid::RegionNeighbor above[]{north, northeast};
        const auto stepping_north =
            homeworldz::region::open_sides_at(899, 899, 512, 256, {490.746, 256.036}, above);
        if (!stepping_north.north || stepping_north.east || stepping_north.west ||
            stepping_north.south)
            return 43;
    }

    // Facets are 256 m tiles numbered row by row from the corner. A 4x2 -
    // Homeworldz Strait - is eight of them; as two 512 m facets it crashed
    // Firestorm. A northern point is in a northern facet: choosing by the
    // longer axis alone put the whole northern row in the southern one.
    {
        using homeworldz::region::facet_at_position;
        if (facet_at_position(10, 10, 4, 2, 8) != 0) return 61;
        if (facet_at_position(300, 10, 4, 2, 8) != 1) return 62;
        if (facet_at_position(300, 400, 4, 2, 8) != 5) return 63;
        if (facet_at_position(1023.9, 511.9, 4, 2, 8) != 7) return 64;
        // Clamped on both axes.
        if (facet_at_position(-50, -50, 4, 2, 8) != 0) return 65;
        if (facet_at_position(5000, 5000, 4, 2, 8) != 7) return 66;
        if (facet_at_position(5000, -50, 4, 2, 8) != 3) return 67;
        // A one-tile-high rectangle numbers its facets as it always has, and
        // a one-tile-wide one counts northward.
        if (facet_at_position(600, 100, 2, 1, 2) != 1) return 68;
        if (facet_at_position(100, 300, 1, 2, 2) != 1) return 69;
        // A single tile is always facet 0.
        if (facet_at_position(255, 255, 1, 1, 1) != 0) return 70;
    }

    // A teleport away from a region keeps its viewer circuit there only when the
    // destination borders it. Kept for a far teleport, the viewer held a stale
    // Nova, and on the return its first appearance request found no capability
    // (2026-10-14).
    {
        homeworldz::grid::RegionNeighbor lazy;
        lazy.id = "4aa21394-7aa9-4294-bf3b-584dca7a62a3";
        const std::vector<homeworldz::grid::RegionNeighbor> nova_neighbors{lazy};
        if (!homeworldz::region::region_borders(nova_neighbors, lazy.id)) return 44;
        if (homeworldz::region::region_borders(nova_neighbors,
                                               "6720c401-788d-4d1a-b97c-e193c42e39b9"))
            return 45;  // Welcome
        if (homeworldz::region::region_borders(nova_neighbors, "")) return 46;  // logged out
    }

    // The release a neighbour receives, round-tripped through its URL target.
    {
        using homeworldz::region::ChildRelease;
        const ChildRelease moved{"s-1", "home-a", "dest-c"};
        const auto parsed =
            homeworldz::region::parse_child_release_target(homeworldz::region::child_release_target(moved));
        if (!parsed || parsed->session_id != "s-1" || parsed->home_region_id != "home-a" ||
            parsed->destination_region_id != "dest-c")
            return 47;
        // A logout names only the session, as every release did before.
        const auto ended = homeworldz::region::parse_child_release_target("s-1");
        if (!ended || ended->session_id != "s-1" || !ended->home_region_id.empty() ||
            homeworldz::region::child_release_target(*ended) != "s-1")
            return 48;
        if (homeworldz::region::parse_child_release_target("") ||
            homeworldz::region::parse_child_release_target("?home=a"))
            return 49;
    }

    // Three regions in a row, A B C. The avatar stands in A, so B holds it as a
    // child, homed at A. It teleports to C. A closes its own circuit and tells B
    // where the avatar went; B borders C, so C is about to claim the child, and
    // closing it would hand the viewer a second seed for a region it holds.
    {
        using homeworldz::region::ChildReleaseAction;
        using homeworldz::region::decide_child_release;
        homeworldz::region::ChildAgent held;
        held.session_id = "s-1";
        held.home_region_id = "region-a";
        const homeworldz::region::ChildRelease to_c{"s-1", "region-a", "region-c"};
        // B, beside C: kept.
        if (decide_child_release(to_c, &held, false, true) != ChildReleaseAction::keep) return 50;
        // A region beside A but not C: closed.
        if (decide_child_release(to_c, &held, false, false) != ChildReleaseAction::close) return 51;
        // C already re-announced the child before A's release arrived, so it is
        // C's now, and A's release is not A's to act on.
        auto claimed = held;
        claimed.home_region_id = "region-c";
        if (decide_child_release(to_c, &claimed, false, false) != ChildReleaseAction::ignore)
            return 52;
        // The teleport was to B itself and has already arrived: never close the
        // circuit the avatar is standing on.
        const homeworldz::region::ChildRelease to_b{"s-1", "region-a", "region-b"};
        if (decide_child_release(to_b, nullptr, true, true) != ChildReleaseAction::ignore)
            return 53;
        // To B, still in flight: the arrival will promote this child.
        if (decide_child_release(to_b, &held, false, true) != ChildReleaseAction::keep) return 54;
        // Nothing held: nothing to do.
        if (decide_child_release(to_c, nullptr, false, false) != ChildReleaseAction::ignore)
            return 55;
        // A region's own far departure, closed a minute later. Not at once: the
        // viewer still stands in this region until its teleport completes, and
        // closing then logged Jim out mid-teleport.
        homeworldz::grid::RegionNeighbor beside;
        beside.id = "region-b";
        const std::vector<homeworldz::grid::RegionNeighbor> a_neighbors{beside};
        homeworldz::region::ChildAgent left_for_c = held;
        left_for_c.home_region_id = "region-c";
        if (!homeworldz::region::far_departure_closes(&left_for_c, "region-c", false, a_neighbors))
            return 57;
        // Back in A within the minute.
        if (homeworldz::region::far_departure_closes(nullptr, "region-c", true, a_neighbors))
            return 58;
        // Moved on from C into B, which borders A, and B claimed A's child.
        auto moved_beside = left_for_c;
        moved_beside.home_region_id = "region-b";
        if (homeworldz::region::far_departure_closes(&moved_beside, "region-c", false, a_neighbors))
            return 59;
        // The child already went (expired or released): nothing to close.
        if (homeworldz::region::far_departure_closes(nullptr, "region-c", false, a_neighbors))
            return 60;
        // A logout still forgets the child, as it always did.
        const homeworldz::region::ChildRelease ended{"s-1", "", ""};
        if (decide_child_release(ended, &held, false, false) != ChildReleaseAction::forget)
            return 56;
    }
    return 0;
}
