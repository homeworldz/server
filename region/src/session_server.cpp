#include "homeworldz/session_server.h"

#include <libwebsockets.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace homeworldz::session {
namespace {

// OutboundFrame is one queued message. droppable marks a frame whose loss a
// client can absorb because a later frame supersedes it (transforms); chat,
// kills, and protocol replies are not droppable.
struct OutboundFrame {
    std::string text;
    bool droppable{};
};

// One connection's state, owned by the service thread.
struct Connection {
    SessionCore core;
    std::string inbound;
    std::deque<OutboundFrame> outbox;

    explicit Connection(SessionCore state) : core(std::move(state)) {}
};

// A client that stops reading — a frozen or heavily throttled browser tab is
// the realistic case, since hidden tabs can be throttled to one tick a
// minute — must not grow the region's memory without bound. Past the soft
// limit superseded frames are dropped first; past the hard limit the oldest
// frames go regardless, and the client resynchronizes by spawning again.
constexpr std::size_t outbox_soft_limit = 256;
constexpr std::size_t outbox_hard_limit = 1024;

// authTimeoutSeconds bounds how long an unauthenticated connection may hold
// a socket, matching the grid channel's rule.
constexpr int auth_timeout_seconds = 10;

// inboundLimit bounds one message; session traffic is small.
constexpr std::size_t inbound_limit = 64 * 1024;

} // namespace

struct Server::State {
    Options options;
    lws_context* context{};
    std::thread service;
    std::atomic<bool> running{true};
    std::atomic<int> established{0};

    // Outbound messages queued by the simulation thread, drained on the
    // service thread. An empty target targets every authenticated session.
    struct PendingSend {
        std::string target;
        std::string message;
        bool droppable{};
    };
    std::mutex pending_mutex;
    std::vector<PendingSend> pending_sends;

    // Inbound embodiment commands queued on the service thread, drained by
    // the simulation thread once per tick.
    std::mutex inbound_mutex;
    std::vector<Server::InboundCommand> inbound;

    // Live connections; service-thread only.
    std::unordered_set<lws*> connections;

    // The flush sweep: a service-thread timer that drains cross-thread sends
    // and re-arms writability for any connection with a queued outbox. The
    // cancel-service wake remains the fast path; the sweep bounds worst-case
    // delivery at its period after a missed wake — the client core measured
    // ~60 s stalls when a wake was lost.
    static constexpr unsigned sweep_period_us = 25 * LWS_US_PER_MS;
    lws_sorted_usec_list_t sweep{};

    static int callback(lws* wsi, lws_callback_reasons reason, void* user, void* in, size_t len);
    static void sweep_callback(lws_sorted_usec_list_t* scheduled);
    void service_loop();
    void drain_broadcasts();
    bool flush_one(lws* wsi, Connection* connection);

    static State* of(lws* wsi) {
        return static_cast<State*>(lws_context_user(lws_get_context(wsi)));
    }

    // trim_outbox enforces the limits above, returning how many frames were
    // dropped so the caller can say so once rather than per frame.
    static std::size_t trim_outbox(Connection* connection) {
        if (connection->outbox.size() <= outbox_soft_limit) return 0;
        std::size_t dropped = 0;
        for (auto frame = connection->outbox.begin();
             frame != connection->outbox.end() && connection->outbox.size() > outbox_soft_limit;) {
            if (frame->droppable) {
                frame = connection->outbox.erase(frame);
                ++dropped;
            } else {
                ++frame;
            }
        }
        while (connection->outbox.size() > outbox_hard_limit) {
            connection->outbox.pop_front();
            ++dropped;
        }
        return dropped;
    }

    static void queue_messages(lws* wsi, Connection* connection, std::vector<std::string> messages,
                               bool close_after, const std::string& reason) {
        for (auto& message : messages)
            connection->outbox.push_back({std::move(message), false});
        if (close_after) {
            lws_close_reason(wsi, LWS_CLOSE_STATUS_POLICY_VIOLATION,
                             reinterpret_cast<unsigned char*>(const_cast<char*>(reason.data())),
                             reason.size());
        }
        lws_callback_on_writable(wsi);
    }
};

int Server::State::callback(lws* wsi, lws_callback_reasons reason, void* user, void* in, size_t len) {
    auto* state = of(wsi);
    auto** slot = static_cast<Connection**>(user);
    switch (reason) {
    case LWS_CALLBACK_ESTABLISHED: {
        *slot = new Connection(SessionCore(state->options.region_name, state->options.validator,
                                    state->options.terrain_width,
                                    state->options.terrain_height,
                                    state->options.walkable_slope_degrees,
                                    state->options.water_height,
                                    state->options.terrain_layers,
                                    state->options.terrain_revision));
        state->connections.insert(wsi);
        lws_set_timeout(wsi, PENDING_TIMEOUT_USER_REASON_BASE, auth_timeout_seconds);
        return 0;
    }
    case LWS_CALLBACK_CLOSED: {
        if (slot && *slot) {
            if ((*slot)->core.established()) {
                state->established.fetch_sub(1);
                // The simulation retires whatever this session embodied.
                std::lock_guard<std::mutex> hold(state->inbound_mutex);
                state->inbound.push_back({(*slot)->core.identity().session_id,
                                          (*slot)->core.identity().user_id,
                                          (*slot)->core.identity().display_name,
                                          (*slot)->core.identity().arrival, {}, true});
            }
            delete *slot;
            *slot = nullptr;
        }
        state->connections.erase(wsi);
        return 0;
    }
    case LWS_CALLBACK_RECEIVE: {
        auto* connection = slot ? *slot : nullptr;
        if (!connection) return -1;
        if (connection->inbound.size() + len > inbound_limit) return -1;
        connection->inbound.append(static_cast<const char*>(in), len);
        if (!lws_is_final_fragment(wsi)) return 0;
        std::string message;
        message.swap(connection->inbound);

        const auto was_established = connection->core.established();
        const auto result = !lws_frame_is_binary(wsi)
                                ? connection->core.handle_text(message)
                                : connection->core.handle_binary();
        if (!was_established && connection->core.established()) {
            state->established.fetch_add(1);
            // Authenticated: the auth deadline no longer applies.
            lws_set_timeout(wsi, NO_PENDING_TIMEOUT, 0);
        }
        if (result.command && connection->core.established()) {
            std::lock_guard<std::mutex> hold(state->inbound_mutex);
            state->inbound.push_back({connection->core.identity().session_id,
                                      connection->core.identity().user_id,
                                      connection->core.identity().display_name,
                                      connection->core.identity().arrival,
                                      std::move(*result.command), false});
        }
        queue_messages(wsi, connection, result.send, result.close, result.close_reason);
        if (result.close && result.send.empty()) return -1;
        return 0;
    }
    case LWS_CALLBACK_SERVER_WRITEABLE: {
        auto* connection = slot ? *slot : nullptr;
        if (!connection) return -1;
        if (!state->flush_one(wsi, connection)) return -1;
        if (!connection->outbox.empty()) lws_callback_on_writable(wsi);
        return 0;
    }
    case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
        state->drain_broadcasts();
        return 0;
    default:
        return 0;
    }
}

bool Server::State::flush_one(lws* wsi, Connection* connection) {
    if (connection->outbox.empty()) return true;
    const auto& message = connection->outbox.front().text;
    std::vector<unsigned char> frame(LWS_PRE + message.size());
    std::memcpy(frame.data() + LWS_PRE, message.data(), message.size());
    const auto written = lws_write(wsi, frame.data() + LWS_PRE, message.size(), LWS_WRITE_TEXT);
    if (written < 0 || static_cast<std::size_t>(written) != message.size()) return false;
    connection->outbox.pop_front();
    return true;
}

void Server::State::sweep_callback(lws_sorted_usec_list_t* scheduled) {
    auto* state = lws_container_of(scheduled, State, sweep);
    state->drain_broadcasts();
    for (auto* wsi : state->connections) {
        void* user = lws_wsi_user(wsi);
        auto* connection = user ? *static_cast<Connection**>(user) : nullptr;
        if (connection && !connection->outbox.empty()) lws_callback_on_writable(wsi);
    }
    lws_sul_schedule(state->context, 0, &state->sweep, &State::sweep_callback,
                     sweep_period_us);
}

void Server::State::drain_broadcasts() {
    std::vector<PendingSend> drained;
    {
        std::lock_guard<std::mutex> hold(pending_mutex);
        drained.swap(pending_sends);
    }
    if (drained.empty()) return;
    for (auto* wsi : connections) {
        void* user = lws_wsi_user(wsi);
        auto* connection = user ? *static_cast<Connection**>(user) : nullptr;
        if (!connection || !connection->core.established()) continue;
        bool queued = false;
        for (const auto& pending : drained) {
            if (!pending.target.empty() &&
                pending.target != connection->core.identity().session_id) continue;
            connection->outbox.push_back({pending.message, pending.droppable});
            queued = true;
        }
        if (!queued) continue;
        if (const auto dropped = trim_outbox(connection); dropped > 0)
            std::cout << "{\"level\":\"warning\",\"message\":\"session outbox trimmed\",\"session\":\""
                      << connection->core.identity().session_id << "\",\"dropped\":" << dropped
                      << ",\"queued\":" << connection->outbox.size() << "}" << std::endl;
        lws_callback_on_writable(wsi);
    }
}

void Server::State::service_loop() {
    while (running.load()) {
        if (lws_service(context, 0) < 0) break;
    }
}

Server::Server() = default;

std::unique_ptr<Server> Server::start(Options options) {
    if (options.port <= 0 || options.port > 65535) return nullptr;

    static const lws_protocols protocols[] = {
        {"homeworldz-session", &State::callback, sizeof(Connection*), inbound_limit, 0, nullptr, 0},
        LWS_PROTOCOL_LIST_TERM,
    };

    auto server = std::unique_ptr<Server>(new Server());
    server->state_ = std::make_unique<State>();
    server->state_->options = std::move(options);

    lws_context_creation_info info{};
    info.port = server->state_->options.port;
    info.protocols = protocols;
    info.user = server->state_.get();
    info.gid = static_cast<gid_t>(-1);
    info.uid = static_cast<uid_t>(-1);
    // No origin enforcement: the session is credentialed by the region
    // ticket in the first message, and every browser client is cross-origin
    // by design (the page comes from the client's own host, never from a
    // region). lws's security-best-practices option would refuse exactly
    // those upgrades.
    info.options = 0;

    lws_set_log_level(LLL_ERR | LLL_WARN, nullptr);
    server->state_->context = lws_create_context(&info);
    if (!server->state_->context) return nullptr;
    lws_sul_schedule(server->state_->context, 0, &server->state_->sweep,
                     &State::sweep_callback, State::sweep_period_us);
    server->state_->service = std::thread([state = server->state_.get()] { state->service_loop(); });
    return server;
}

Server::~Server() {
    if (!state_) return;
    state_->running.store(false);
    if (state_->context) lws_cancel_service(state_->context);
    if (state_->service.joinable()) state_->service.join();
    if (state_->context) lws_context_destroy(state_->context);
}

void Server::broadcast_chat(std::string_view from_name, std::string_view message) {
    broadcast(encode_envelope("chat", {},
        "{\"from\":" + json_string(from_name) + ",\"message\":" + json_string(message) + "}"));
}

void Server::broadcast(std::string message, bool droppable) {
    if (!state_) return;
    {
        std::lock_guard<std::mutex> hold(state_->pending_mutex);
        state_->pending_sends.push_back({std::string{}, std::move(message), droppable});
    }
    lws_cancel_service(state_->context);
}

void Server::send_to(std::string_view session_id, std::string message, bool droppable) {
    if (!state_ || session_id.empty()) return;
    {
        std::lock_guard<std::mutex> hold(state_->pending_mutex);
        state_->pending_sends.push_back({std::string(session_id), std::move(message), droppable});
    }
    lws_cancel_service(state_->context);
}

std::vector<Server::InboundCommand> Server::drain_commands() {
    std::vector<InboundCommand> drained;
    if (!state_) return drained;
    std::lock_guard<std::mutex> hold(state_->inbound_mutex);
    drained.swap(state_->inbound);
    return drained;
}

int Server::session_count() const {
    return state_ ? state_->established.load() : 0;
}

} // namespace homeworldz::session
