#include "homeworldz/falcon_runtime.h"

#include "homeworldz/script/compiler.h"
#include "homeworldz/script/lexer.h"
#include "homeworldz/script/vm.h"

#include <algorithm>
#include <deque>
#include <memory>
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
        std::deque<PendingEvent> pending;

        bool has_work() const { return !vm.finished() || !pending.empty(); }

        bool queue_event(std::string event, std::vector<Value> args) {
            if (!enabled || trapped || !program->find_event(event)) return false;
            if (pending.size() >= max_pending) return false;
            pending.push_back({std::move(event), std::move(args)});
            return true;
        }

        LiveScript(Identity script_identity, std::shared_ptr<const Program> compiled,
                   bool start_enabled,
                   const HostSink& sink, const SitTargetSink& sit_target_sink,
                   const RegionCall& region_call)
            : identity(std::move(script_identity)), program(std::move(compiled)), vm(*program),
              enabled(start_enabled) {
            const auto host_identity = identity;
            vm.set_host([sink, sit_target_sink, region_call, host_identity](
                            const std::string& name, const std::vector<Value>& args)
                            -> std::optional<Value> {
                if (name != "llSitTarget" && name != "llOwnerSay" && name != "llSay") {
                    auto answer = region_call ? region_call(host_identity, name, args)
                                              : std::optional<Value>{};
                    return answer ? answer : default_result(name);
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
            if (enabled && program->find_event("state_entry"))
                vm.dispatch("state_entry", {});
        }
    };

    HostSink sink;
    SitTargetSink sit_target_sink;
    RegionCall region_call;
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

    void mark_ready(LiveScript& script) {
        if (script.queued || !script.enabled || script.trapped || !script.has_work()) return;
        script.queued = true;
        ready.push_back(&script);
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

    bool erase(const std::string& key) {
        const auto found = scripts.find(key);
        if (found == scripts.end()) return false;
        auto* script = found->second.get();
        if (script->queued) std::erase(ready, script);
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

FalconRezResult FalconRuntime::rez(Identity identity, std::string_view source,
                                   bool enabled) {
    try {
        // Firestorm terminates script-editor uploads with a wire-format NUL.
        // It is not part of the LSL source and must not reach the lexer.
        while (!source.empty() && source.back() == '\0') source.remove_suffix(1);
        auto program = impl_->compile(source);
        auto live = std::make_unique<Impl::LiveScript>(
            identity, std::move(program), enabled, impl_->sink, impl_->sit_target_sink,
            impl_->region_call);
        // A failed compile has already thrown, so a script being replaced
        // keeps running until its replacement is ready.
        const auto key = instance_key(identity.object_id, identity.inventory_item_id);
        impl_->erase(key);
        auto& stored = *impl_->scripts.emplace(key, std::move(live)).first->second;
        impl_->by_object[identity.object_id].push_back(&stored);
        impl_->mark_ready(stored);
        return {true, enabled, {}, {}};
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
