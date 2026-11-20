#pragma once

// The explicit-stack Falcon bytecode VM. Per ADR 0021 / SCRIPTING.md, script
// state lives entirely in data (instruction pointer, operand stack, locals, and
// persistent globals), never on the native C++ call stack, so execution can be
// suspended after any single instruction and resumed elsewhere. run() executes
// a bounded instruction slice and yields, which is how an infinite LSL loop is
// contained rather than hanging the region thread.
//
// A VM instance represents one script's live state. The region dispatches an
// event (state_entry on rez, touch_start on a click, ...) which seeds the event
// parameters and positions execution at that handler; run() then advances it
// under a budget. Globals persist across dispatches; locals do not.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "homeworldz/script/bytecode.h"

namespace homeworldz::script {

enum class RunStatus {
    Finished, // the current event reached Halt (or none is active)
    Yielded,  // spent its instruction budget with work remaining
    Error,    // a runtime fault stopped the script
    Sleeping, // a host call put the script to sleep; wake() ends it
    Waiting,  // a host call is still in progress; resume() completes it
};

// What a host function did with a call. A function that answers at once
// returns its value, or nothing if it has none; an optional value converts to
// that, so most host functions never name this type.
struct HostResult {
    enum class Kind {
        Returned,  // the call is done
        Sleep,     // the call is done, and the script sleeps before it goes on
        Suspended, // the call is in progress; VM::resume() delivers its answer
        Failed,    // the call raises a runtime error that stops the script
    };
    Kind kind = Kind::Returned;
    std::optional<Value> value;  // Returned and Sleep
    double sleep_seconds = 0.0;  // Sleep
    std::uint64_t token = 0;     // Suspended: names the operation in progress
    std::string error;           // Failed

    HostResult() = default;
    HostResult(std::optional<Value> returned) : value(std::move(returned)) {}
    HostResult(std::nullopt_t) {}
    HostResult(Value returned) : value(std::move(returned)) {}

    // Second Life's forced delays, and llSleep itself.
    static HostResult sleep(std::optional<Value> returned, double seconds) {
        HostResult result(std::move(returned));
        result.kind = Kind::Sleep;
        result.sleep_seconds = seconds;
        return result;
    }
    // The token is the host's name for the operation. It is a number so it
    // can travel in a snapshot and be answered on another region.
    static HostResult suspend(std::uint64_t operation) {
        HostResult result;
        result.kind = Kind::Suspended;
        result.token = operation;
        return result;
    }
    static HostResult fail(std::string message) {
        HostResult result;
        result.kind = Kind::Failed;
        result.error = std::move(message);
        return result;
    }
};

// The host-function boundary. Given a bounded name and evaluated arguments, it
// performs bounded region work, or begins a longer operation and suspends the
// script until it is done. It must not touch the network, filesystem, clocks,
// or randomness directly.
using HostFunction =
    std::function<HostResult(const std::string& name, const std::vector<Value>& args)>;

class VM {
public:
    // The Program must outlive the VM (bytecode is a shared immutable asset).
    // Globals are seeded from the program's initial values.
    explicit VM(const Program& program);

    void set_host(HostFunction host) { host_ = std::move(host); }

    // Positions the VM at an event handler and seeds its parameters. Throws
    // ScriptError if the event is unknown or the argument count/types do not
    // match the handler. Globals are preserved; locals and the operand stack are
    // reset. Call run() afterward to execute.
    void dispatch(const std::string& event, const std::vector<Value>& args);

    // Executes up to instruction_budget instructions, then returns.
    RunStatus run(std::uint64_t instruction_budget);

    bool finished() const { return finished_; }
    const std::string& error() const { return error_; }
    // The source line the last runtime error was raised on, from 1; 0 when
    // the program carries no line for it.
    int error_line() const { return error_line_; }
    std::uint64_t total_instructions() const { return total_; }

    // How long the script has left to sleep, in seconds; 0 when it is awake.
    // The VM has no clock: whoever drives it counts the time and calls wake().
    // Before a snapshot of a sleeping script, set_sleep_remaining() records
    // how much of the sleep is left, so the duration travels and not a time.
    double sleep_remaining() const { return sleep_seconds_; }
    void set_sleep_remaining(double seconds) { sleep_seconds_ = seconds > 0.0 ? seconds : 0.0; }
    void wake() { sleep_seconds_ = 0.0; }

    // Whether a host call is in progress, and the token it gave.
    bool waiting() const { return waiting_; }
    std::uint64_t waiting_token() const { return waiting_token_; }
    // Completes the host call in progress with its value, or with nothing for
    // a function that returns none. False when no call with that token waits.
    bool resume(std::uint64_t token, std::optional<Value> value);

    // Compact, versioned binary capture of the full runtime state (this PoC's
    // slice of the crossing snapshot in SCRIPTING.md): ip, operand stack,
    // locals, globals, the sleep left, and the host call in progress. Bytecode
    // is referenced by ABI version, not embedded.
    std::vector<std::uint8_t> snapshot() const;

    // Restores state produced by snapshot() into this VM. Throws ScriptError if
    // the format, snapshot version, or bytecode ABI does not match.
    void restore(const std::vector<std::uint8_t>& bytes);

private:
    void step();

    const Program& program_;
    HostFunction host_;

    std::size_t ip_ = 0;
    std::vector<Value> stack_;
    std::vector<Value> locals_;
    std::vector<Value> globals_;
    bool finished_ = true;
    std::string error_;
    int error_line_ = 0;
    std::uint64_t total_ = 0;
    double sleep_seconds_ = 0.0;
    bool waiting_ = false;
    std::uint64_t waiting_token_ = 0;
};

} // namespace homeworldz::script
