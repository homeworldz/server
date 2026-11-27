#include "homeworldz/falcon_runtime.h"

#include "homeworldz/script/compiler.h"
#include "homeworldz/script/lexer.h"
#include "homeworldz/script/vm.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <memory>
#include <cstring>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace homeworldz::script {
namespace {

std::string instance_key(std::string_view object_id, std::string_view item_id) {
    return std::string(object_id) + '|' + std::string(item_id);
}

// What a value-returning host function gives a script when the region does not
// answer it. The compiler has already emitted code that expects a value on the
// stack after the call; leaving it short would fault the script on its next
// instruction, which is worse than a well-defined empty answer.
std::optional<Value> default_result(const std::string& name) {
    if (name == "llAvatarOnSitTarget" || name == "llAvatarOnLinkSitTarget" ||
        name == "llGetPermissionsKey")
        return Value::make_string("00000000-0000-0000-0000-000000000000");
    if (name == "llGetPermissions") return Value::make_integer(0);
    if (name == "llGetInventoryName") return Value::make_string("");
    return std::nullopt;
}

// A duration a script gave, in seconds. The compiler has already widened an
// integer, so anything else here is not a number.
double seconds_of(const Value& value) {
    return value.type == Type::Float ? value.number() : 0.0;
}

// A captured script: the VM's own snapshot, wrapped with what the runtime
// keeps around the VM. Times are written as what is left of them, because
// the clock they were counted on stays behind.
constexpr char state_magic[4] = {'H', 'W', 'Z', 'R'};
constexpr std::uint8_t state_version = 1;
constexpr std::size_t max_state_bytes = 256 * 1024;
constexpr std::uint8_t state_enabled = 1;
constexpr std::uint8_t state_trapped = 2;

class StateWriter {
public:
    void u8(std::uint8_t value) { bytes_.push_back(static_cast<char>(value)); }
    void u32(std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) u8(static_cast<std::uint8_t>(value >> shift));
    }
    void f64(double value) {
        char raw[sizeof value];
        std::memcpy(raw, &value, sizeof value);
        bytes_.append(raw, sizeof raw);
    }
    void text(std::string_view value) {
        u32(static_cast<std::uint32_t>(value.size()));
        bytes_.append(value);
    }
    void value(const Value& item) {
        u8(static_cast<std::uint8_t>(item.type));
        switch (item.type) {
        case Type::Integer: u32(static_cast<std::uint32_t>(item.integer)); break;
        case Type::String:
        case Type::Key: text(item.str); break;
        case Type::Float: f64(item.components[0]); break;
        case Type::Vector:
        case Type::Rotation:
            for (const auto component : item.components) f64(component);
            break;
        case Type::List:
            u32(static_cast<std::uint32_t>(item.items().size()));
            for (const auto& element : item.items()) value(element);
            break;
        case Type::Void: break;
        }
    }
    std::string take() { return std::move(bytes_); }

private:
    std::string bytes_;
};

// Reads what StateWriter wrote. Anything that does not fit throws, and the
// caller starts the script afresh.
class StateReader {
public:
    explicit StateReader(std::string_view bytes) : bytes_(bytes) {}
    std::uint8_t u8() { return static_cast<std::uint8_t>(take(1)[0]); }
    std::uint32_t u32() {
        const auto raw = take(4);
        std::uint32_t value = 0;
        for (int index = 3; index >= 0; --index)
            value = (value << 8) | static_cast<std::uint8_t>(raw[static_cast<std::size_t>(index)]);
        return value;
    }
    double f64() {
        double value{};
        std::memcpy(&value, take(sizeof value).data(), sizeof value);
        return value;
    }
    std::string text() { return std::string(take(u32())); }
    Value value(int depth = 0) {
        const auto type = static_cast<Type>(u8());
        switch (type) {
        case Type::Integer: return Value::make_integer(static_cast<std::int32_t>(u32()));
        case Type::String: return Value::make_string(text());
        case Type::Key: return Value::make_key(text());
        case Type::Float: return Value::make_float(f64());
        case Type::Vector:
        case Type::Rotation: {
            const double x = f64(), y = f64(), z = f64(), w = f64();
            return type == Type::Vector ? Value::make_vector(x, y, z)
                                        : Value::make_rotation(x, y, z, w);
        }
        case Type::List: {
            if (depth != 0) break; // a list never holds a list
            const auto count = u32();
            if (count > bytes_.size()) break;
            std::vector<Value> items;
            items.reserve(count);
            for (std::uint32_t index = 0; index < count; ++index) items.push_back(value(1));
            return Value::make_list(std::move(items));
        }
        case Type::Void: break;
        }
        throw std::runtime_error("script state holds a value of no known type");
    }
    bool done() const { return bytes_.empty(); }

    std::string_view take(std::size_t count) {
        if (count > bytes_.size()) throw std::runtime_error("script state is cut short");
        const auto piece = bytes_.substr(0, count);
        bytes_.remove_prefix(count);
        return piece;
    }

private:
    std::string_view bytes_;
};

} // namespace

struct FalconRuntime::Impl {
    struct LiveScript {
        struct PendingEvent {
            std::string name;
            std::vector<Value> args;
        };

        // A single click can arrive while an earlier handler is still yielded
        // mid-slice. Events queue here and drain one at a time as the VM goes
        // idle so no in-flight handler is ever clobbered. Bounded so a script
        // that never keeps up cannot grow the queue without limit.
        static constexpr std::size_t max_pending = 0x40;

        Identity identity;
        // Shared with every other script compiled from the same source; the
        // VM holds a reference to it, so it is declared first.
        std::shared_ptr<const Program> program;
        VM vm;
        bool enabled{};
        bool trapped{};
        // Whether the script is in the runtime's ready queue.
        bool queued{};
        // Asleep until wake_at, by llSleep or a function's forced delay.
        bool asleep{};
        double wake_at{};
        // llSetTimerEvent: the interval, when it is next due, and whether a
        // timer event is already waiting, since at most one ever is.
        double timer_interval{};
        double timer_due{};
        bool timer_pending{};
        std::deque<PendingEvent> pending;

        bool has_work() const { return !vm.finished() || !pending.empty(); }
        // Asleep, or in a host call that has not answered yet: there is work,
        // and none of it can run.
        bool blocked() const { return asleep || vm.waiting(); }

        bool queue_event(std::string event, std::vector<Value> args) {
            if (!enabled || trapped || !program->find_event(event)) return false;
            if (pending.size() >= max_pending) return false;
            pending.push_back({std::move(event), std::move(args)});
            return true;
        }

        // The script and the runtime both outlive the host function: the VM
        // that holds it is a member of the script, and the runtime owns the
        // script.
        // `fresh` starts the script at state_entry; without it the script
        // waits to be given a captured state.
        LiveScript(Identity script_identity, std::shared_ptr<const Program> compiled,
                   bool start_enabled, Impl& runtime, bool fresh = true)
            : identity(std::move(script_identity)), program(std::move(compiled)), vm(*program),
              enabled(start_enabled) {
            const auto host_identity = identity;
            const auto& sink = runtime.sink;
            const auto& sit_target_sink = runtime.sit_target_sink;
            const auto& region_call = runtime.region_call;
            vm.set_host([this, &runtime, &sink, &sit_target_sink, &region_call, host_identity](
                            const std::string& name, const std::vector<Value>& args)
                            -> HostResult {
                if (name == "llSleep" && args.size() == 1)
                    return HostResult::sleep(std::nullopt, seconds_of(args[0]));
                if (name == "llSetTimerEvent" && args.size() == 1) {
                    runtime.set_timer(*this, seconds_of(args[0]));
                    return std::nullopt;
                }
                if (name != "llSitTarget" && name != "llOwnerSay" && name != "llSay") {
                    auto answer = region_call ? region_call(host_identity, name, args) : HostResult{};
                    if (answer.kind != HostResult::Kind::Suspended &&
                        answer.kind != HostResult::Kind::Failed && !answer.value)
                        answer.value = default_result(name);
                    return answer;
                }
                if (name == "llSitTarget" && args.size() == 2 &&
                    args[0].type == Type::Vector && args[1].type == Type::Rotation) {
                    if (sit_target_sink)
                        sit_target_sink({host_identity,
                                         {args[0].components[0], args[0].components[1],
                                          args[0].components[2]},
                                         args[1].components});
                    return std::nullopt;
                }
                if (!sink) return std::nullopt;
                if (name == "llOwnerSay" && args.size() == 1 &&
                    args[0].type == Type::String) {
                    sink({host_identity, true, 0, args[0].str});
                } else if (name == "llSay" && args.size() == 2 &&
                           args[0].type == Type::Integer &&
                           args[1].type == Type::String) {
                    sink({host_identity, false, args[0].integer, args[1].str});
                }
                return std::nullopt;
            });
            if (fresh && enabled && program->find_event("state_entry"))
                vm.dispatch("state_entry", {});
        }
    };

    HostSink sink;
    SitTargetSink sit_target_sink;
    RegionCall region_call;
    Clock clock = [] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
    std::unordered_map<std::string, std::unique_ptr<LiveScript>> scripts;
    // The scripts in each object, so an event for one object does not walk
    // every script in the region.
    std::unordered_map<std::string, std::vector<LiveScript*>> by_object;
    // Scripts with something to run, in the order they asked. A tick serves
    // the front of the queue and a script that still has work rejoins at the
    // back, so a script late in the region is never starved by ones ahead of
    // it that never finish.
    std::deque<LiveScript*> ready;
    // One compiled program per distinct source: a region holds the same door
    // script in hundreds of prims. Weak, so a program nothing runs is freed.
    std::unordered_map<std::string, std::weak_ptr<const Program>> programs;
    // Sleeping scripts by the time they wake, and scripts with a timer by the
    // time it is next due. Ordered, so a tick looks only at the ones whose
    // time has come and a region full of timers costs a tick nothing between
    // them.
    std::set<std::pair<double, LiveScript*>> sleepers;
    std::set<std::pair<double, LiveScript*>> timers;

    void mark_ready(LiveScript& script) {
        if (script.queued || !script.enabled || script.trapped || script.blocked() ||
            !script.has_work())
            return;
        script.queued = true;
        ready.push_back(&script);
    }

    // llSetTimerEvent: a timer event every `seconds` from now, replacing any
    // timer the script had; zero or less stops it.
    void set_timer(LiveScript& script, double seconds) {
        if (script.timer_interval > 0.0) timers.erase({script.timer_due, &script});
        script.timer_interval = seconds > 0.0 ? seconds : 0.0;
        if (script.timer_interval <= 0.0) return;
        script.timer_due = clock() + script.timer_interval;
        timers.emplace(script.timer_due, &script);
    }

    // Timers that have come due, and sleepers whose time is up.
    void advance_clock(double now) {
        std::vector<LiveScript*> due;
        while (!timers.empty() && timers.begin()->first <= now) {
            due.push_back(timers.begin()->second);
            timers.erase(timers.begin());
        }
        for (auto* script : due) {
            // The next one is an interval from now, not from when this one
            // was due: a region that stalled does not fire the ones it missed.
            script->timer_due = now + script->timer_interval;
            timers.emplace(script->timer_due, script);
            // At most one timer event waits, however long the script takes.
            if (!script->timer_pending && queue_event(*script, "timer", {}))
                script->timer_pending = true;
        }
        while (!sleepers.empty() && sleepers.begin()->first <= now) {
            auto* script = sleepers.begin()->second;
            sleepers.erase(sleepers.begin());
            script->asleep = false;
            script->vm.wake();
            mark_ready(*script);
        }
    }

    bool queue_event(LiveScript& script, const std::string& event, const std::vector<Value>& args) {
        if (!script.queue_event(event, args)) return false;
        mark_ready(script);
        return true;
    }

    std::shared_ptr<const Program> compile(std::string_view source) {
        const std::string text(source);
        if (const auto found = programs.find(text); found != programs.end())
            if (auto program = found->second.lock()) return program;
        auto program = std::make_shared<const Program>(compile_source(text));
        // Entries whose scripts have all gone are swept when the table has
        // clearly outgrown the scripts that could be using it.
        if (programs.size() > scripts.size() + 0x40)
            std::erase_if(programs, [](const auto& entry) { return entry.second.expired(); });
        programs[text] = program;
        return program;
    }

    // One script's state, for restore() to read here or in another region.
    std::string capture(LiveScript& script) {
        const double now = clock();
        StateWriter out;
        for (const auto character : state_magic) out.u8(static_cast<std::uint8_t>(character));
        out.u8(state_version);
        out.text(script.identity.asset_id);
        out.u8(static_cast<std::uint8_t>((script.enabled ? state_enabled : 0) |
                                         (script.trapped ? state_trapped : 0)));
        out.f64(script.timer_interval);
        out.f64(script.timer_interval > 0.0 ? (std::max)(0.0, script.timer_due - now) : 0.0);
        out.u32(static_cast<std::uint32_t>(script.pending.size()));
        for (const auto& event : script.pending) {
            out.text(event.name);
            out.u32(static_cast<std::uint32_t>(event.args.size()));
            for (const auto& arg : event.args) out.value(arg);
        }
        // The VM records a sleep as what is left of it, and only the runtime
        // knows how much that is by now.
        if (script.asleep) script.vm.set_sleep_remaining((std::max)(0.0, script.wake_at - now));
        const auto vm_state = script.vm.snapshot();
        out.u32(static_cast<std::uint32_t>(vm_state.size()));
        auto bytes = out.take();
        bytes.append(reinterpret_cast<const char*>(vm_state.data()), vm_state.size());
        if (bytes.size() > max_state_bytes) return {};
        return bytes;
    }

    // Puts a captured state into a script made without `fresh`. Throws when
    // the state does not fit this script; the script is then of no use and
    // the caller makes another. Nothing is entered in the runtime's tables
    // here, so a script that failed leaves nothing behind.
    void restore(LiveScript& script, std::string_view state) {
        StateReader in(state);
        for (const auto character : state_magic)
            if (in.u8() != static_cast<std::uint8_t>(character))
                throw std::runtime_error("not a script state");
        if (in.u8() != state_version) throw std::runtime_error("script state of another version");
        // The state belongs to the source it ran: the same asset, or nothing.
        if (in.text() != script.identity.asset_id)
            throw std::runtime_error("script state is for other source");
        const auto flags = in.u8();
        const double timer_interval = in.f64();
        const double timer_left = in.f64();
        const auto event_count = in.u32();
        if (event_count > LiveScript::max_pending) throw std::runtime_error("too many events");
        std::deque<LiveScript::PendingEvent> pending;
        for (std::uint32_t index = 0; index < event_count; ++index) {
            LiveScript::PendingEvent event;
            event.name = in.text();
            const auto arg_count = in.u32();
            if (arg_count > 0x40 || !script.program->find_event(event.name))
                throw std::runtime_error("script state holds an event the script has not");
            for (std::uint32_t arg = 0; arg < arg_count; ++arg) event.args.push_back(in.value());
            pending.push_back(std::move(event));
        }
        const auto vm_size = in.u32();
        const auto vm_bytes = in.take(vm_size);
        if (!in.done()) throw std::runtime_error("script state runs on past its end");
        script.vm.restore(std::vector<std::uint8_t>(vm_bytes.begin(), vm_bytes.end()));
        // A host call in progress was being answered by the region the script
        // left, and nobody here will ever answer it.
        if (script.vm.waiting()) throw std::runtime_error("script state is inside a host call");
        const double now = clock();
        script.enabled = script.enabled && (flags & state_enabled) != 0;
        script.trapped = (flags & state_trapped) != 0;
        script.pending = std::move(pending);
        script.timer_pending = std::any_of(script.pending.begin(), script.pending.end(),
                                           [](const auto& event) { return event.name == "timer"; });
        if (timer_interval > 0.0 && timer_left >= 0.0 && timer_left <= timer_interval) {
            script.timer_interval = timer_interval;
            script.timer_due = now + timer_left;
        }
        if (script.vm.sleep_remaining() > 0.0) {
            script.asleep = true;
            script.wake_at = now + script.vm.sleep_remaining();
        }
    }

    bool erase(const std::string& key) {
        const auto found = scripts.find(key);
        if (found == scripts.end()) return false;
        auto* script = found->second.get();
        if (script->queued) std::erase(ready, script);
        if (script->asleep) sleepers.erase({script->wake_at, script});
        if (script->timer_interval > 0.0) timers.erase({script->timer_due, script});
        const auto object = by_object.find(script->identity.object_id);
        if (object != by_object.end()) {
            std::erase(object->second, script);
            if (object->second.empty()) by_object.erase(object);
        }
        scripts.erase(found);
        return true;
    }
};

FalconRuntime::FalconRuntime(HostSink host_sink, SitTargetSink sit_target_sink,
                             RegionCall region_call)
    : impl_(std::make_unique<Impl>()) {
    impl_->sink = std::move(host_sink);
    impl_->sit_target_sink = std::move(sit_target_sink);
    impl_->region_call = std::move(region_call);
}

FalconRuntime::~FalconRuntime() = default;

void FalconRuntime::set_clock(Clock clock) { impl_->clock = std::move(clock); }

bool FalconRuntime::resume(std::string_view object_id, std::string_view inventory_item_id,
                           std::uint64_t token, std::optional<Value> value) {
    const auto found = impl_->scripts.find(instance_key(object_id, inventory_item_id));
    if (found == impl_->scripts.end() || !found->second->vm.resume(token, std::move(value)))
        return false;
    impl_->mark_ready(*found->second);
    return true;
}

FalconRezResult FalconRuntime::rez(Identity identity, std::string_view source,
                                   bool enabled, std::string_view saved_state) {
    try {
        // Firestorm terminates script-editor uploads with a wire-format NUL.
        // It is not part of the LSL source and must not reach the lexer.
        while (!source.empty() && source.back() == '\0') source.remove_suffix(1);
        auto program = impl_->compile(source);
        std::unique_ptr<Impl::LiveScript> live;
        if (!saved_state.empty()) {
            try {
                auto resumed = std::make_unique<Impl::LiveScript>(identity, program, enabled, *impl_, false);
                impl_->restore(*resumed, saved_state);
                live = std::move(resumed);
            } catch (const std::exception&) {
                // The state does not fit this script: start it afresh.
            }
        }
        const bool restored = live != nullptr;
        if (!live)
            live = std::make_unique<Impl::LiveScript>(identity, std::move(program), enabled, *impl_);
        // A failed compile has already thrown, so a script being replaced
        // keeps running until its replacement is ready.
        const auto key = instance_key(identity.object_id, identity.inventory_item_id);
        impl_->erase(key);
        auto& stored = *impl_->scripts.emplace(key, std::move(live)).first->second;
        impl_->by_object[identity.object_id].push_back(&stored);
        // A restored script may be part-way through a sleep or a timer.
        if (stored.asleep) impl_->sleepers.emplace(stored.wake_at, &stored);
        if (stored.timer_interval > 0.0) impl_->timers.emplace(stored.timer_due, &stored);
        impl_->mark_ready(stored);
        return {true, stored.enabled, {}, {}, restored};
    } catch (const ScriptError& error) {
        if (error.line == 0) return {false, false, error.what(), error.what()};
        return {false, false, error.what(),
                "(" + std::to_string(error.line - 1) + ", " + std::to_string(error.column - 1) +
                    ") : ERROR : " + error.message};
    } catch (const std::exception& error) {
        return {false, false, error.what(), error.what()};
    }
}

bool FalconRuntime::set_enabled(std::string_view object_id,
                                std::string_view inventory_item_id, bool enabled) {
    const auto found = impl_->scripts.find(instance_key(object_id, inventory_item_id));
    if (found == impl_->scripts.end()) return false;
    found->second->enabled = enabled;
    // A script stopped part-way through an event, or with events waiting,
    // picks up where it was.
    impl_->mark_ready(*found->second);
    return true;
}

bool FalconRuntime::erase(std::string_view object_id, std::string_view inventory_item_id) {
    return impl_->erase(instance_key(object_id, inventory_item_id));
}

std::size_t FalconRuntime::erase_object(std::string_view object_id) {
    const auto object = impl_->by_object.find(std::string(object_id));
    if (object == impl_->by_object.end()) return 0;
    std::vector<std::string> keys;
    for (const auto* script : object->second)
        keys.push_back(instance_key(script->identity.object_id, script->identity.inventory_item_id));
    for (const auto& key : keys) impl_->erase(key);
    return keys.size();
}

std::string FalconRuntime::capture(std::string_view object_id, std::string_view inventory_item_id) {
    const auto found = impl_->scripts.find(instance_key(object_id, inventory_item_id));
    if (found == impl_->scripts.end()) return {};
    return impl_->capture(*found->second);
}

FalconRuntime::ScriptStatus FalconRuntime::object_script_status(
    std::string_view object_id) const {
    ScriptStatus status;
    const auto object = impl_->by_object.find(std::string(object_id));
    if (object == impl_->by_object.end()) return status;
    for (const auto* script : object->second) {
        status.scripted = true;
        if (script->enabled && !script->trapped &&
            (script->program->find_event("touch_start") ||
             script->program->find_event("touch") ||
             script->program->find_event("touch_end")))
            status.handles_touch = true;
    }
    return status;
}

std::size_t FalconRuntime::dispatch_touch_start(std::string_view object_id,
                                                std::int32_t total_number) {
    return dispatch_event(object_id, "touch_start", {Value::make_integer(total_number)});
}

std::size_t FalconRuntime::dispatch_event(std::string_view object_id, const std::string& event,
                                          const std::vector<Value>& args) {
    std::size_t accepted = 0;
    const auto object = impl_->by_object.find(std::string(object_id));
    if (object == impl_->by_object.end()) return accepted;
    for (auto* script : object->second)
        if (impl_->queue_event(*script, event, args)) ++accepted;
    return accepted;
}

bool FalconRuntime::dispatch_to_script(std::string_view object_id,
                                       std::string_view inventory_item_id,
                                       const std::string& event, const std::vector<Value>& args) {
    const auto found = impl_->scripts.find(instance_key(object_id, inventory_item_id));
    return found != impl_->scripts.end() && impl_->queue_event(*found->second, event, args);
}

FalconTickResult FalconRuntime::run_tick(std::uint64_t total_instruction_budget,
                                         std::uint64_t per_script_slice) {
    FalconTickResult result;
    if (total_instruction_budget == 0 || per_script_slice == 0) return result;
    const double now = impl_->clock();
    impl_->advance_clock(now);
    auto remaining = total_instruction_budget;
    // Only the scripts waiting when the tick began: one that rejoins the queue
    // gets its next slice on a later tick, so each runs at most one slice per
    // tick. When the budget runs out, the rest stay at the front for the next.
    for (auto waiting = impl_->ready.size(); waiting != 0 && remaining != 0; --waiting) {
        auto* script = impl_->ready.front();
        impl_->ready.pop_front();
        script->queued = false;
        if (!script->enabled || script->trapped) continue;
        if (script->vm.finished()) {
            // Idle: seed the next queued event, or skip if nothing is pending.
            if (script->pending.empty()) continue;
            auto event = std::move(script->pending.front());
            script->pending.pop_front();
            if (event.name == "timer") script->timer_pending = false;
            script->vm.dispatch(event.name, event.args);
        }
        ++result.scripts_visited;
        const auto slice = (std::min)(remaining, per_script_slice);
        const auto before = script->vm.total_instructions();
        const auto status = script->vm.run(slice);
        const auto consumed = script->vm.total_instructions() - before;
        result.instructions += consumed;
        remaining -= (std::min)(remaining, consumed);
        if (status == RunStatus::Error) {
            script->trapped = true;
            ++result.trapped;
        } else if (status == RunStatus::Sleeping) {
            script->asleep = true;
            script->wake_at = now + script->vm.sleep_remaining();
            impl_->sleepers.emplace(script->wake_at, script);
        }
        impl_->mark_ready(*script);
    }
    return result;
}

std::size_t FalconRuntime::size() const { return impl_->scripts.size(); }

std::size_t FalconRuntime::compiled_programs() const {
    return static_cast<std::size_t>(
        std::count_if(impl_->programs.begin(), impl_->programs.end(),
                      [](const auto& entry) { return !entry.second.expired(); }));
}

} // namespace homeworldz::script
