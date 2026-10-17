#pragma once

#include "homeworldz/script_runtime.h"
#include "homeworldz/script/bytecode.h"

#include <array>
#include <optional>
#include <vector>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace homeworldz::script {

struct FalconHostMessage {
    Identity identity;
    bool owner_only{};
    std::int32_t channel{};
    std::string text;
};

// A seat a script set with llSitTarget on the prim it lives in. The runtime
// does not know what a prim is; it reports the call and the region applies it,
// which is the same split the chat sink already uses.
struct FalconSitTarget {
    Identity identity;
    std::array<double, 3> position{};
    std::array<double, 4> rotation{};
};

struct FalconRezResult {
    bool compiled{};
    bool running{};
    std::string diagnostic;
};

struct FalconTickResult {
    std::size_t scripts_visited{};
    std::uint64_t instructions{};
    std::size_t trapped{};
};

class FalconRuntime {
public:
    using HostSink = std::function<void(FalconHostMessage)>;
    using SitTargetSink = std::function<void(FalconSitTarget)>;
    // Every other host function: the calling script's identity, the function's
    // name, and its evaluated arguments. The region answers with a value for a
    // function that returns one, and nothing for one that does not. A function
    // the region does not answer that the compiler accepted returns its type's
    // default to the script rather than leaving its stack short.
    using RegionCall = std::function<std::optional<Value>(
        const Identity&, const std::string& name, const std::vector<Value>& args)>;

    explicit FalconRuntime(HostSink host_sink = {}, SitTargetSink sit_target_sink = {},
                           RegionCall region_call = {});
    ~FalconRuntime();
    FalconRuntime(const FalconRuntime&) = delete;
    FalconRuntime& operator=(const FalconRuntime&) = delete;

    FalconRezResult rez(Identity identity, std::string_view source, bool enabled);
    bool set_enabled(std::string_view object_id, std::string_view inventory_item_id,
                     bool enabled);
    bool erase(std::string_view object_id, std::string_view inventory_item_id);
    // Whether an object carries scripts and whether any enabled script declares a
    // touch handler. The region advertises these to Firestorm as the SCRIPTED and
    // HANDLE_TOUCH object-update flags so the viewer enables the Touch action and
    // sends ObjectGrab in the first place.
    struct ScriptStatus {
        bool scripted{};
        bool handles_touch{};
    };
    ScriptStatus object_script_status(std::string_view object_id) const;
    // Queues touch_start(total_number) for every enabled compiled script in the
    // named object whose program declares that handler. Events are queued rather
    // than dispatched immediately so an in-flight handler is never clobbered;
    // run_tick() drains one queued event per idle script. Returns the number of
    // scripts that accepted the event.
    std::size_t dispatch_touch_start(std::string_view object_id,
                                     std::int32_t total_number = 1);
    // Queues an event for every script in an object that declares it — how
    // changed(CHANGED_LINK) reaches a seat's scripts. Returns how many accepted.
    std::size_t dispatch_event(std::string_view object_id, const std::string& event,
                               const std::vector<Value>& args);
    // Queues an event for one script, as run_time_permissions answers only the
    // script that asked.
    bool dispatch_to_script(std::string_view object_id, std::string_view inventory_item_id,
                            const std::string& event, const std::vector<Value>& args);
    FalconTickResult run_tick(std::uint64_t total_instruction_budget = 0x00010000,
                              std::uint64_t per_script_slice = 0x00000400);
    std::size_t size() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace homeworldz::script
