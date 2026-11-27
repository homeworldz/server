#pragma once

#include "homeworldz/script_runtime.h"
#include "homeworldz/script/bytecode.h"
#include "homeworldz/script/vm.h"

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
    // The same error as a viewer's script editor reads it,
    // "(row, column) : ERROR : message" with both counted from 0, which is
    // what lets a click on the error move the cursor to it. Without a known
    // position it is the diagnostic alone.
    std::string viewer_error;
    // Whether the script went on from a captured state instead of starting
    // at state_entry.
    bool restored{};
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
    // default to the script rather than leaving its stack short. A function
    // that cannot answer at once returns HostResult::suspend(token) and calls
    // resume() when it can; one with a forced delay returns HostResult::sleep.
    using RegionCall = std::function<HostResult(
        const Identity&, const std::string& name, const std::vector<Value>& args)>;
    // Seconds on a clock that only moves forward. Sleeps and timers are
    // counted on it; the default is the machine's steady clock.
    using Clock = std::function<double()>;

    explicit FalconRuntime(HostSink host_sink = {}, SitTargetSink sit_target_sink = {},
                           RegionCall region_call = {});
    ~FalconRuntime();
    FalconRuntime(const FalconRuntime&) = delete;
    FalconRuntime& operator=(const FalconRuntime&) = delete;

    void set_clock(Clock clock);
    // Completes a host call a script is suspended in, with the value the
    // function returns or nothing for one that returns none. False when that
    // script is not waiting on that token.
    bool resume(std::string_view object_id, std::string_view inventory_item_id,
                std::uint64_t token, std::optional<Value> value);

    // Starts a script. With `saved_state` (what capture() gave, here or in
    // another region) the script goes on from exactly there: its globals, the
    // instruction it had reached, a sleep or timer part-way through, and the
    // events still waiting for it. A state that does not fit, because it was
    // written for other source or by another bytecode ABI, is set aside and
    // the script starts at state_entry.
    FalconRezResult rez(Identity identity, std::string_view source, bool enabled,
                        std::string_view saved_state = {});
    // Everything one script would need to go on elsewhere, as opaque bytes;
    // empty when there is no such script or its state is too large to carry.
    // The script itself is untouched and keeps running.
    std::string capture(std::string_view object_id, std::string_view inventory_item_id);
    bool set_enabled(std::string_view object_id, std::string_view inventory_item_id,
                     bool enabled);
    bool erase(std::string_view object_id, std::string_view inventory_item_id);
    // Stops every script in an object that has left the scene. Returns how
    // many there were.
    std::size_t erase_object(std::string_view object_id);
    // Stops an object's running scripts where they are, for a hand-off that
    // may yet be called off, and returns the inventory items of the ones it
    // stopped. set_enabled starts each again from where it was.
    std::vector<std::string> suspend_object(std::string_view object_id);
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
    // Raises an event in an object that is about to go, as attach(NULL_KEY)
    // in one being taken off: each idle script that declares it runs the
    // handler now, for at most `budget` instructions, so what it did is in
    // the state the object leaves with. A script still busy with something
    // else has the event queued behind that instead, and a handler that
    // outlasts the budget goes on from where it stopped the next time the
    // script runs. Returns how many scripts took the event.
    std::size_t raise_now(std::string_view object_id, const std::string& event,
                          const std::vector<Value>& args, std::uint64_t budget = 0x4000);
    // Queues an event for one script, as run_time_permissions answers only the
    // script that asked.
    bool dispatch_to_script(std::string_view object_id, std::string_view inventory_item_id,
                            const std::string& event, const std::vector<Value>& args);
    // Runs the scripts that have work, in the order they came to have it, one
    // slice each, until the tick's budget is spent. Scripts the budget did not
    // reach are first in line on the next tick; one that still has work after
    // its slice goes to the back.
    FalconTickResult run_tick(std::uint64_t total_instruction_budget = 0x00010000,
                              std::uint64_t per_script_slice = 0x00000400);
    std::size_t size() const;
    // How many distinct programs the running scripts share: scripts with the
    // same source are compiled once.
    std::size_t compiled_programs() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace homeworldz::script
