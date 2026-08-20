#pragma once

// The region-session listener (docs/CLIENT2-TRANSPORT.md, option A): a
// WebSocket server on libwebsockets running its own service thread, speaking
// the session protocol of session_protocol.h. TLS is the fronting
// infrastructure's concern in this deployment (the grid's edge or a local
// proxy terminates wss); in-region TLS arrives with direct home-hosted
// serving.

#include "homeworldz/session_protocol.h"

#include <memory>
#include <string>
#include <string_view>

namespace homeworldz::session {

class Server {
public:
    struct Options {
        int port{};
        std::string region_name;
        // validator runs on the service thread; it is expected to block
        // briefly (one grid round trip) during auth only.
        TicketValidator validator;
        // Heightmap vertices along x and y, published in the hello terrain
        // block. Equal on a square region; a rectangle's map is macro-sized.
        std::size_t terrain_width{};
        std::size_t terrain_height{};
        // This region's configured walkable slope, published in the hello
        // avatar block.
        double walkable_slope_degrees{};
        // The region's water plane height, published in the hello water block.
        double water_height{};
        // This region's live terrain layers, read per greeting so an operator's
        // change reaches the next client to connect.
        std::function<terrain::Settings()> terrain_layers;
        // Current terrain revision, read per greeting.
        std::function<std::uint64_t()> terrain_revision;
    };

    // start returns a running server, or null when the listener could not be
    // created. The server stops and joins its thread on destruction.
    static std::unique_ptr<Server> start(Options options);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // broadcast_chat delivers one public chat line to every authenticated
    // session. Thread-safe; called from the simulation thread.
    void broadcast_chat(std::string_view from_name, std::string_view message);

    // InboundCommand is an embodiment command an authenticated session
    // issued, tagged with who issued it. kind==disconnect is synthesized
    // when an embodied session's socket closes, so the simulation retires
    // the avatar; it is not a client message.
    struct InboundCommand {
        std::string session_id;
        std::string user_id;
        std::string display_name;
        std::optional<std::array<float, 3>> arrival;
        Command command;
        bool disconnect{};
    };

    // drain_commands returns and clears the queued inbound commands, in
    // arrival order. Thread-safe; called once per simulation tick.
    std::vector<InboundCommand> drain_commands();

    // send_to delivers one pre-encoded envelope to every connection of one
    // session (normally exactly one). Thread-safe. droppable marks a frame a
    // later one supersedes (a transform), so a client that stops reading
    // loses those before anything that matters.
    void send_to(std::string_view session_id, std::string message, bool droppable = false);

    // broadcast delivers one pre-encoded envelope to every authenticated
    // session. Thread-safe; droppable as above.
    void broadcast(std::string message, bool droppable = false);

    // session_count reports authenticated sessions, for logs and tests.
    int session_count() const;

private:
    Server();
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace homeworldz::session
