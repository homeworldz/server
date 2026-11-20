#pragma once

// The compatibility suite's harness: a script on a Falcon VM with a test host
// that records what the script says and the runtime errors it raises, an event
// queue, and a clock the test moves.
//
// Its shape (Session, drain, post_event, check, expect, says) is that of the
// semantics test in Ashley's Rookery scripting package, so her checks port
// with their bodies unchanged. Where Falcon has no answer yet (the LSL line of
// a runtime error, state changes, a memory limit) the harness reports nothing,
// so the checks that need it fail rather than pass by default.

#include "homeworldz/script/compiler.h"
#include "homeworldz/script/lexer.h"
#include "homeworldz/script/vm.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace compat {

namespace falcon = homeworldz::script;
using falcon::HostResult;
using falcon::Value;

// ---------------------------------------------------------------------------
// results

struct Check {
    std::string name;
    bool passed = false;
    std::string detail;
};

inline std::vector<Check>& results() {
    static std::vector<Check> all;
    return all;
}

inline void check(bool ok, const std::string& what, const std::string& detail = std::string()) {
    results().push_back({what, ok, detail});
}

inline std::string joined(const std::vector<std::string>& v) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += " / ";
        out += v[i];
    }
    return out;
}

// ---------------------------------------------------------------------------
// values, as a test writes them

struct V {
    static Value integer(std::int32_t v) { return Value::make_integer(v); }
    static Value string(std::string v) { return Value::make_string(std::move(v)); }
    static Value key(std::string v) {
        Value value = Value::make_string(std::move(v));
        value.type = falcon::Type::Key;
        return value;
    }
};

// ---------------------------------------------------------------------------
// compiling

struct Diagnostic {
    bool is_error = true;
    int line = 0;
    int column = 0;
    std::string message;
};

struct CompileResult {
    bool ok = false;
    std::shared_ptr<const falcon::Program> program;
    std::vector<Diagnostic> diagnostics;
};

inline CompileResult compile(const std::string& source) {
    CompileResult result;
    try {
        result.program = std::make_shared<const falcon::Program>(falcon::compile_source(source));
        result.ok = true;
    } catch (const falcon::ScriptError& error) {
        result.diagnostics.push_back({true, error.line, error.column, error.message});
    } catch (const std::exception& error) {
        result.diagnostics.push_back({true, 0, 0, error.what()});
    }
    return result;
}

inline bool compiles(const std::string& source) { return compile(source).ok; }

// ---------------------------------------------------------------------------
// the host

struct RuntimeError {
    std::string message;
    int lsl_line = 0;  // Falcon does not record the line of a runtime error yet
    bool fatal = false;
};

struct TestHost {
    std::vector<std::string> said;
    std::vector<std::string> transitions;  // "from>to"; Falcon has one state so far
    std::vector<RuntimeError> errors;
    double clock = 100.0;
    double wall = 1700000000.0;  // 2023-11-14 22:13:20 UTC
};

// ---------------------------------------------------------------------------
// a running script

struct Limits {
    std::int64_t slice_interrupts = 10000;  // instructions per run()
    std::size_t memory_bytes = 256 * 1024;  // not enforced by Falcon yet
};

enum class RunStatus {
    Idle,      // nothing to do until another event is posted
    Busy,      // the slice was spent; call run() again
    Sleeping,  // llSleep or a forced delay, until the host's clock reaches wake_time
    Waiting,   // a host call is suspended; call resume()
    Stopped,   // the script stopped for good
};

struct RunResult {
    RunStatus status = RunStatus::Idle;
    double wake_time = 0.0;
};

class Script {
  public:
    Script(std::shared_ptr<const falcon::Program> program, TestHost& host, Limits limits)
        : program_(std::move(program)), host_(host), limits_(limits), vm_(*program_) {
        vm_.set_host([this](const std::string& name, const std::vector<Value>& args) -> HostResult {
            if (name == "llOwnerSay" && args.size() == 1) {
                host_.said.push_back(args[0].str);
                return std::nullopt;
            }
            if (name == "llGetKey") return Value::make_string("a822ff2b-ff02-461d-b45d-dcd10a2de0c2");
            // A function with a forced delay, and one whose answer comes later.
            if (name == "llGetObjectName") return HostResult::sleep(Value::make_string("Rookery Box"), 0.5);
            if (name == "llRequestAgentData") return HostResult::suspend(1);
            if (name == "llSleep" && args.size() == 1) return HostResult::sleep(std::nullopt, args[0].number());
            return HostResult::fail(name + " is not available on this region");
        });
        if (program_->find_event("state_entry")) vm_.dispatch("state_entry", {});
    }

    // Queues an event. False (with why) when the arguments do not match the
    // handler the script declares. An event the script has no handler for is
    // accepted and goes nowhere, as in a region.
    bool post_event(const std::string& name, std::vector<Value> args, std::string* why = nullptr) {
        const auto* entry = program_->find_event(name);
        if (!entry) return true;
        if (args.size() != entry->param_types.size()) {
            if (why) *why = name + " takes " + std::to_string(entry->param_types.size()) + " arguments";
            return false;
        }
        for (std::size_t i = 0; i < args.size(); ++i) {
            const auto want = entry->param_types[i];
            const bool text = args[i].type == falcon::Type::String || args[i].type == falcon::Type::Key;
            if (text && (want == falcon::Type::String || want == falcon::Type::Key)) {
                args[i].type = want;
            } else if (args[i].type != want) {
                if (why) *why = name + " argument " + std::to_string(i + 1) + " has the wrong type";
                return false;
            }
        }
        queue_.push_back({name, std::move(args)});
        return true;
    }

    // One slice.
    RunResult run() {
        if (vm_.waiting()) return {RunStatus::Waiting, 0.0};
        if (asleep_) {
            if (host_.clock < wake_time_) return {RunStatus::Sleeping, wake_time_};
            asleep_ = false;
            vm_.wake();
        }
        if (vm_.finished()) {
            if (queue_.empty()) return {RunStatus::Idle, 0.0};
            auto event = std::move(queue_.front());
            queue_.pop_front();
            vm_.dispatch(event.name, event.args);
        }
        switch (vm_.run(static_cast<std::uint64_t>(limits_.slice_interrupts))) {
        case falcon::RunStatus::Yielded: return {RunStatus::Busy, 0.0};
        case falcon::RunStatus::Sleeping:
            asleep_ = true;
            wake_time_ = host_.clock + vm_.sleep_remaining();
            return {RunStatus::Sleeping, wake_time_};
        case falcon::RunStatus::Waiting: return {RunStatus::Waiting, 0.0};
        case falcon::RunStatus::Error:
            // The event ends; the next one still runs.
            host_.errors.push_back({vm_.error(), 0, false});
            break;
        case falcon::RunStatus::Finished: break;
        }
        return {queue_.empty() ? RunStatus::Idle : RunStatus::Busy, 0.0};
    }

    // Completes the host call the script is suspended in.
    void resume(Value result) {
        if (vm_.waiting()) vm_.resume(vm_.waiting_token(), std::move(result));
    }

    std::string current_state() const { return "default"; }
    bool stopped() const { return false; }

  private:
    struct Event {
        std::string name;
        std::vector<Value> args;
    };
    std::shared_ptr<const falcon::Program> program_;
    TestHost& host_;
    Limits limits_;
    falcon::VM vm_;
    std::deque<Event> queue_;
    bool asleep_ = false;
    double wake_time_ = 0.0;
};

struct Session {
    TestHost host;
    std::unique_ptr<Script> script;
    std::string compile_error;

    explicit Session(const std::string& source, Limits limits = Limits{}) {
        CompileResult compiled = compile(source);
        if (!compiled.ok) {
            for (const auto& d : compiled.diagnostics)
                compile_error += "line " + std::to_string(d.line) + ": " + d.message + "; ";
            return;
        }
        try {
            script = std::make_unique<Script>(compiled.program, host, limits);
        } catch (const std::exception& error) {
            compile_error = std::string("start: ") + error.what();
        }
    }

    // Runs until the script has nothing more to do right now.
    RunResult drain(int max_slices = 100000) {
        RunResult r;
        for (int i = 0; i < max_slices && script; ++i) {
            r = script->run();
            if (r.status != RunStatus::Busy) return r;
        }
        return r;
    }
};

// Runs a script's start (globals and state_entry) and compares what it said.
inline void expect(const std::string& name, const std::string& source,
                   const std::vector<std::string>& expected) {
    Session s(source);
    if (!s.script) {
        check(false, name, "does not compile: " + s.compile_error);
        return;
    }
    s.drain();
    std::string detail;
    if (s.host.said != expected)
        detail = "said [" + joined(s.host.said) + "] expected [" + joined(expected) + "]";
    if (!s.host.errors.empty()) detail += " runtime error: " + s.host.errors[0].message;
    check(s.host.said == expected && s.host.errors.empty(), name, detail);
}

// A script whose state_entry runs each line.
inline std::string says(const std::string& globals, const std::vector<std::string>& lines) {
    std::string src = globals + "\ndefault {\n state_entry() {\n";
    for (const auto& l : lines) src += "  " + l + "\n";
    src += " }\n}\n";
    return src;
}

}  // namespace compat
