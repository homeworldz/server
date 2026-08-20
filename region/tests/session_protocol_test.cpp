#include "homeworldz/mesh_acceptance.h"
#include "homeworldz/session_protocol.h"
#include "homeworldz/terrain_layers.h"

#include <span>
#include <string>

using homeworldz::session::Envelope;
using homeworldz::session::ParseError;
using homeworldz::session::SessionCore;
using homeworldz::session::SessionIdentity;

int main() {
    // Encoding renders the envelope shape the grid channel established.
    const auto encoded = homeworldz::session::encode_envelope(
        "chat", "req-1", R"({"from":"Object","message":"hi \"you\""})");
    if (encoded != R"({"type":"chat","version":1,"correlationId":"req-1","payload":)"
                   R"({"from":"Object","message":"hi \"you\""}})")
        return 1;

    // Round trip: parse what was encoded.
    ParseError error{};
    const auto parsed = homeworldz::session::parse_envelope(encoded, error);
    if (!parsed || error != ParseError::none || parsed->type != "chat" ||
        parsed->version != 1 || parsed->correlation_id != "req-1" ||
        homeworldz::session::json_field(parsed->payload, "message") != "hi \"you\"")
        return 2;

    // First-byte discrimination: any other leading byte is a different
    // encoding, refused by name rather than parsed as JSON.
    if (homeworldz::session::parse_envelope("P\x02二进制", error) ||
        error != ParseError::wrong_encoding)
        return 3;
    if (homeworldz::session::parse_envelope("{\"version\":1}", error) ||
        error != ParseError::malformed)
        return 4;

    // Escapes render and parse both ways.
    if (homeworldz::session::json_string("a\nb\t\"c\"\\") != R"("a\nb\t\"c\"\\")") return 5;

    // Text above ASCII survives intact when it is well-formed UTF-8, and is
    // replaced when it is not. Both matter to a client displaying our strings:
    // the region interpolates names read straight out of an uploaded file into
    // refusal reasons, and nothing obliges an untrusted GLB to hold valid UTF-8.
    // Emitting those bytes raw made the whole response un-parseable, so a
    // creator got a parse error instead of the sentence naming the problem.
    if (homeworldz::session::json_string("caf\xc3\xa9") != "\"caf\xc3\xa9\"") return 39;
    if (homeworldz::session::json_string("\xe6\x97\xa5\xe6\x9c\xac") !=
        "\"\xe6\x97\xa5\xe6\x9c\xac\"")
        return 40;
    // A lone continuation byte, and a truncated sequence: one replacement each,
    // and the valid text around them is untouched.
    // The replacement is written as the � escape rather than as raw
    // U+FFFD bytes, so a response carrying mangled input is still pure ASCII on
    // the wire and cannot compound one encoding problem with another.
    // Written with doubled backslashes rather than a raw literal: MSVC treats
    // \u inside R"(...)" as a universal character name, so the raw form silently
    // became the very U+FFFD byte sequence the test meant to prove absent.
    if (homeworldz::session::json_string("a\x80z") != "\"a\\ufffdz\"") return 41;
    if (homeworldz::session::json_string("a\xc3") != "\"a\\ufffd\"") return 42;
    // Overlong encoding of '/', a UTF-16 surrogate half, and a code point past
    // U+10FFFF are all well-formed to a validator that only counts continuation
    // bytes, and all invalid UTF-8.
    if (homeworldz::session::json_string("\xc0\xaf") != "\"\\ufffd\\ufffd\"") return 43;
    if (homeworldz::session::json_string("\xed\xa0\x80") !=
        "\"\\ufffd\\ufffd\\ufffd\"")
        return 44;
    if (homeworldz::session::json_string("\xf5\x80\x80\x80") !=
        "\"\\ufffd\\ufffd\\ufffd\\ufffd\"")
        return 45;
    const auto tricky = homeworldz::session::parse_envelope(
        R"({"type":"auth","version":1,"payload":{"token":"with \"quotes\" and {braces}"}})", error);
    if (!tricky || homeworldz::session::json_field(tricky->payload, "token") !=
                       "with \"quotes\" and {braces}")
        return 6;

    // A surrogate pair decodes to one supplementary code point (proper
    // UTF-8, never CESU-8), and a lone surrogate is refused rather than
    // substituted.
    const auto emoji = homeworldz::session::parse_envelope(
        "{\"type\":\"chat\",\"version\":1,\"payload\":{\"message\":\"\\ud83d\\ude00 hi\"}}", error);
    if (!emoji || homeworldz::session::json_field(emoji->payload, "message") !=
                      "\xf0\x9f\x98\x80 hi")
        return 16;
    const auto lone = homeworldz::session::parse_envelope(
        R"({"type":"chat","version":1,"payload":{"message":"\ud83d oops"}})", error);
    if (!lone || !homeworldz::session::json_field(lone->payload, "message").empty()) return 17;
    if (homeworldz::session::parse_envelope(R"({"type":"\udc00","version":1})", error) ||
        error != ParseError::malformed)
        return 18;

    // A field named inside a nested payload must not satisfy a top-level
    // lookup: the envelope's own type wins.
    const auto nested = homeworldz::session::parse_envelope(
        R"({"payload":{"type":"impostor"},"type":"ping","version":1})", error);
    if (!nested || nested->type != "ping") return 7;

    // A region an operator has changed: layer 1 raised above the waterline and
    // the top layer lowered. Using non-default values here is the point — a
    // hello that read the constants instead of this region's settings would pass
    // any fixture that happened to match the defaults.
    const auto test_layers = [] {
        homeworldz::terrain::Settings settings;
        settings.start = {20.0F, 20.0F, 20.0F, 20.0F};
        settings.range = {60.0F, 60.0F, 60.0F, 60.0F};
        return settings;
    };

    // --- SessionCore: the connection state machine.
    const SessionIdentity jim{"efa3f54c-0000-4000-8000-000000000001", "jim.tarber", "Jim Tarber",
                              "bbbbbbbb-0000-4000-8000-000000000002"};
    const auto validator = [&](const std::string& token) -> std::optional<SessionIdentity> {
        if (token == "good-ticket") return jim;
        return std::nullopt;
    };

    // The first message must be auth; anything else closes.
    SessionCore impatient("Sandbox", validator, 256, 256, 65.0, 20.0, test_layers, [] { return 7u; });
    if (const auto result = impatient.handle_text(R"({"type":"ping","version":1})"); !result.close)
        return 8;

    // A refused ticket closes with the refusal named.
    SessionCore refused("Sandbox", validator, 256, 256, 65.0, 20.0, test_layers, [] { return 7u; });
    if (const auto result = refused.handle_text(
            R"({"type":"auth","version":1,"payload":{"token":"bad"}})");
        !result.close || result.close_reason.find("ticket") == std::string::npos)
        return 9;

    // The happy path: auth resolves, hello names the region and identity. The
    // core is deliberately rectangular (ADR 0036): the session transport is
    // macro-native, so the hello must describe width and height independently.
    SessionCore session("Sandbox", validator, 512, 256, 65.0, 20.0, test_layers, [] { return 7u; });
    const auto hello = session.handle_text(
        R"({"type":"auth","version":1,"payload":{"token":"good-ticket"}})");
    if (hello.close || hello.send.size() != 1 || !session.established() ||
        session.identity().userid != "jim.tarber")
        return 10;
    const auto greeting = homeworldz::session::parse_envelope(hello.send.front(), error);
    if (!greeting || greeting->type != "hello" ||
        homeworldz::session::json_field(greeting->payload, "region") != "Sandbox")
        return 11;
    // The hello publishes the movement constants a predicting client
    // simulates with, and the interest-sweep period its extrapolation cap
    // derives from. These are the controller's own numbers, not copies.
    if (greeting->payload.find("\"movement\":{\"walkSpeed\":4,\"runSpeed\":8,"
                               "\"jumpVelocity\":5,\"gravity\":9.81}") == std::string::npos ||
        greeting->payload.find("\"interestSweepMs\":100") == std::string::npos)
        return 27;
    // The mesh acceptance gate rides in the hello too, and is the same JSON
    // the validator's constants produce — read, never encode (ADR 0033).
    if (greeting->payload.find("\"meshAcceptance\":" +
                               homeworldz::mesh::acceptance_policy_json()) == std::string::npos)
        return 28;
    // The avatar capsule and ground contract, and the terrain fetch: the
    // published ground rules a predicting client lands on (client core
    // request, 2026-07-29). The capsule numbers are the controller's own.
    if (greeting->payload.find("\"avatar\":{\"capsuleRadius\":0.3,"
                               "\"supportOffsetFactor\":0.5,"
                               "\"groundedTolerance\":0.05,"
                               "\"walkableSlopeDegrees\":65"
                    ",\"slopeLimitGoverns\":\"traversal-and-resting\""
                    ",\"restingBoundedBySlope\":true}") == std::string::npos)
        return 33;
    if (greeting->payload.find("\"terrain\":{\"path\":\"/session/terrain\","
                               "\"format\":\"heightmap-f32le\",\"width\":512,"
                               "\"height\":256,"
                               "\"spacing\":1,\"interpolation\":"
                               "\"cell-triangles-diagonal-x,y+1-x+1,y\","
                               "\"changedEvent\":\"terrainChanged\","
                               "\"revision\":7,\"ranges\":true,"
                               "\"patchHeights\":{\"format\":\"heightmap-f32le\","
                               // One closing brace, not two: the terrain object
                               // continues into the layers block below.
                               "\"encoding\":\"base64\",\"rowMajorWithinPatch\":true}")
        == std::string::npos)
        return 34;
    // The water plane: a height, not a surface. A viewer gets this in
    // RegionHandshake and a session client got nothing at all until now, so a
    // client drew water wherever it guessed (client core, 2026-07-30).
    // The event name rides with it, as terrain's does: water became settable from
    // the Region/Estate form on 2026-08-04, so the greeting alone no longer holds
    // for the life of a connection and a client should not have to read a document
    // to learn that.
    if (greeting->payload.find(
            "\"water\":{\"height\":20,\"changedEvent\":\"waterChanged\"}") == std::string::npos)
        return 35;
    // The ground's surface: which four textures, and the elevation band that
    // selects between them. Without these a client knows the shape of the
    // ground and nothing about how it looks, so it draws one flat colour
    // (client core, 2026-07-31). The ids must be the same ones the viewer's
    // RegionHandshake names — asserted against the shared definition rather
    // than a literal, since two copies of this list is the defect it was
    // hoisted to prevent.
    {
        std::string expected = "\"layers\":{\"assets\":[";
        for (const auto asset : homeworldz::terrain::layer_assets) {
            if (expected.back() != '[') expected += ",";
            expected += "\"" + std::string(asset) + "\"";
        }
        expected += "],\"selectedBy\":\"elevation\""
                    // Absolute metres both: layer 1's ceiling and layer 4's
                    // floor. Not a start and a span - the viewer displays what
                    // the region sends, and it labels these Low and High.
                    // This fixture's region, not the shipped defaults: the
                    // hello reads per-region settings, so asserting the default
                    // numbers here would pass even if it ignored them.
                    //
                    // A start and a span, not two bounds. Renamed from
                    // lowHeight/highHeight on 2026-08-04 when an operator's
                    // screenshot showed the viewer's renderer disagreeing with
                    // the viewer's own dialog text, which is where the previous
                    // names came from.
                    ",\"startHeight\":[20,20,20,20]"
                    ",\"heightRange\":[60,60,60,60]"
                    ",\"corners\":\"sw,nw,se,ne\""
                    // The two bounds do not imply where layer 2 becomes layer 3,
                    // so the rule is published rather than left to be guessed.
                    ",\"selection\":\"t=clamp((h-startHeight)*4/heightRange,0,3);"
                    " layer n (1-based) peaks at t=n-1 with linear blend between"
                    " neighbours; boundaries at t=0.5,1.5,2.5\""
                    // False because this region has been changed from the
                    // defaults. A client reads the fact instead of assuming the
                    // grid is uniform.
                    // Published because it is the only part of the ground the
                    // two client families cannot be made to agree on. A client
                    // printing this beside its own render explains a difference
                    // that would otherwise read as a rendering fault on one side.
                    ",\"boundaryNoise\":\"viewer only: a two-octave turbulence sum"
                    " is added to height before t, wobbling each boundary by a few"
                    " metres in a pattern never reproduced outside Linden; this"
                    " region adds none\""
                    ",\"gridWide\":false"
                    // The layers' own change event, so a client discovers it
                    // rather than reading a document. Layers are per region and
                    // an operator changes them without a restart, so unlike
                    // water the greeting alone no longer suffices for the life
                    // of a connection.
                    ",\"changedEvent\":\"terrainLayersChanged\"}";
        if (greeting->payload.find(expected) == std::string::npos) return 36;
    }
    // No blend key of any kind. `selection` determines the crossfade -
    // neighbouring layers blend linearly between their peaks, so it is
    // heightRange/4 wide - and a second number beside it can only disagree.
    // blendMetres was published until 2026-08-05 and said 2 m where the rule
    // gave 15; asserted absent so it cannot come back without a thought.
    if (greeting->payload.find("\"blend\"") != std::string::npos) return 37;
    if (greeting->payload.find("blendMetres") != std::string::npos) return 38;
    // base64 round-trips against a known vector, since a terrainChanged's
    // heights are unreadable if this is subtly wrong and nothing else here
    // would notice.
    {
        const std::string text = "Man";
        const auto encoded = homeworldz::session::base64(std::as_bytes(std::span(text)));
        if (encoded != "TWFu") return 36;
        const std::string one = "M";
        if (homeworldz::session::base64(std::as_bytes(std::span(one))) != "TQ==") return 37;
        const std::string two = "Ma";
        if (homeworldz::session::base64(std::as_bytes(std::span(two))) != "TWE=") return 38;
    }
    // Canonical asset bytes are fetchable, and the hello says from where: a
    // session that learns an asset id must have a way to read it.
    if (greeting->payload.find("\"assets\":{\"base\":\"/session/assets/\"}") == std::string::npos)
        return 35;

    // Ping answers pong carrying the correlation identifier.
    const auto pong = session.handle_text(
        R"({"type":"ping","version":1,"correlationId":"beat-7"})");
    if (pong.close || pong.send.size() != 1) return 12;
    const auto beat = homeworldz::session::parse_envelope(pong.send.front(), error);
    if (!beat || beat->type != "pong" || beat->correlation_id != "beat-7") return 13;

    // An unknown type earns an error envelope, not a close.
    const auto unknown = session.handle_text(R"({"type":"mystery","version":1})");
    if (unknown.close || unknown.send.size() != 1 ||
        unknown.send.front().find("unsupported_type") == std::string::npos)
        return 14;

    // Binary frames are refused: no binary encoding exists yet.
    if (const auto result = session.handle_binary(); !result.close) return 15;

    // --- Embodiment commands (docs/CLIENT2-EMBODIMENT.md) parse into host
    // commands rather than being answered at the protocol layer.
    using Kind = homeworldz::session::Command::Kind;
    const auto spawn = session.handle_text(
        R"({"type":"spawn","version":1,"payload":{"drawDistance":96}})");
    if (spawn.close || !spawn.send.empty() || !spawn.command ||
        spawn.command->kind != Kind::spawn || spawn.command->draw_distance != 96.0)
        return 19;
    // Absent drawDistance stays negative: "not carried", never zero.
    const auto bare_spawn = session.handle_text(R"({"type":"spawn","version":1})");
    if (!bare_spawn.command || bare_spawn.command->draw_distance >= 0.0) return 20;

    const auto move = session.handle_text(
        R"({"type":"move","version":1,"payload":{"controls":2049,"bodyRotation":[0,0,0.7],)"
        R"("camera":{"center":[1,2,3],"at":[1,0,0],"left":[0,1,0],"up":[0,0,1]}}})");
    if (!move.command || move.command->kind != Kind::move || move.command->controls != 2049 ||
        move.command->body_rotation[2] < 0.69F || !move.command->has_camera ||
        move.command->camera_center[2] != 3.0F || move.command->draw_distance >= 0.0)
        return 21;
    const auto bare_move = session.handle_text(
        R"({"type":"move","version":1,"payload":{"controls":0,"bodyRotation":[0,0,0]}})");
    if (!bare_move.command || bare_move.command->has_camera) return 22;

    const auto say = session.handle_text(
        R"({"type":"say","version":1,"payload":{"message":"hello world"}})");
    if (!say.command || say.command->kind != Kind::say || say.command->message != "hello world")
        return 23;
    const auto empty_say = session.handle_text(
        R"({"type":"say","version":1,"payload":{"message":""}})");
    if (empty_say.command || empty_say.send.size() != 1 ||
        empty_say.send.front().find("invalid_message") == std::string::npos)
        return 24;

    const auto leave = session.handle_text(R"({"type":"leave","version":1})");
    if (!leave.command || leave.command->kind != Kind::leave) return 25;

    // Commands from an unauthenticated connection never reach the host.
    SessionCore stranger("Sandbox", validator, 256, 256, 65.0, 20.0, test_layers, [] { return 7u; });
    if (const auto result = stranger.handle_text(R"({"type":"spawn","version":1})");
        !result.close)
        return 26;

    return 0;
}
