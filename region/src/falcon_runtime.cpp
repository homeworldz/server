#include "homeworldz/falcon_runtime.h"

#include "homeworldz/script/compiler.h"
#include "homeworldz/script/lexer.h"
#include "homeworldz/script/vm.h"

#include <algorithm>
#include <deque>
#include <unordered_map>
#include <utility>

namespace homeworldz::script {
namespace {

std::string instance_key(std::string_view object_id, std::string_view item_id) {
    return std::string(object_id) + '|' + std::string(item_id);
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
        Program program;
        VM vm;
        bool enabled{};
        bool trapped{};
        std::deque<PendingEvent> pending;

        bool queue_event(std::string event, std::vector<Value> args) {
            if (!enabled || trapped || !program.find_event(event)) return false;
            if (pending.size() >= max_pending) return false;
            pending.push_back({std::move(event), std::move(args)});
            return true;
        }

        LiveScript(Identity script_identity, Program compiled, bool start_enabled,
                   const HostSink& sink, const SitTargetSink& sit_target_sink)
            : identity(std::move(script_identity)), program(std::move(compiled)), vm(program),
              enabled(start_enabled) {
            const auto host_identity = identity;
            vm.set_host([sink, sit_target_sink, host_identity](
                            const std::string& name, const std::vector<Value>& args)
                            -> std::optional<Value> {
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
            if (enabled && program.find_event("state_entry"))
                vm.dispatch("state_entry", {});
        }
    };

    HostSink sink;
    SitTargetSink sit_target_sink;
    std::unordered_map<std::string, std::unique_ptr<LiveScript>> scripts;
};

FalconRuntime::FalconRuntime(HostSink host_sink, SitTargetSink sit_target_sink)
    : impl_(std::make_unique<Impl>()) {
    impl_->sink = std::move(host_sink);
    impl_->sit_target_sink = std::move(sit_target_sink);
}

FalconRuntime::~FalconRuntime() = default;

FalconRezResult FalconRuntime::rez(Identity identity, std::string_view source,
                                   bool enabled) {
    try {
        // Firestorm terminates script-editor uploads with a wire-format NUL.
        // It is not part of the LSL source and must not reach the lexer.
        while (!source.empty() && source.back() == '\0') source.remove_suffix(1);
        auto program = compile_source(std::string(source));
        auto live = std::make_unique<Impl::LiveScript>(
            identity, std::move(program), enabled, impl_->sink, impl_->sit_target_sink);
        impl_->scripts.insert_or_assign(
            instance_key(identity.object_id, identity.inventory_item_id), std::move(live));
        return {true, enabled, {}};
    } catch (const ScriptError& error) {
        return {false, false, error.what()};
    } catch (const std::exception& error) {
        return {false, false, error.what()};
    }
}

bool FalconRuntime::set_enabled(std::string_view object_id,
                                std::string_view inventory_item_id, bool enabled) {
    const auto found = impl_->scripts.find(instance_key(object_id, inventory_item_id));
    if (found == impl_->scripts.end()) return false;
    found->second->enabled = enabled;
    return true;
}

bool FalconRuntime::erase(std::string_view object_id, std::string_view inventory_item_id) {
    return impl_->scripts.erase(instance_key(object_id, inventory_item_id)) != 0;
}

FalconRuntime::ScriptStatus FalconRuntime::object_script_status(
    std::string_view object_id) const {
    ScriptStatus status;
    for (const auto& [key, script] : impl_->scripts) {
        (void)key;
        if (script->identity.object_id != object_id) continue;
        status.scripted = true;
        if (script->enabled && !script->trapped &&
            (script->program.find_event("touch_start") ||
             script->program.find_event("touch") ||
             script->program.find_event("touch_end")))
            status.handles_touch = true;
    }
    return status;
}

std::size_t FalconRuntime::dispatch_touch_start(std::string_view object_id,
                                                std::int32_t total_number) {
    std::size_t accepted = 0;
    for (auto& [key, script] : impl_->scripts) {
        (void)key;
        if (script->identity.object_id != object_id) continue;
        if (script->queue_event("touch_start", {Value::make_integer(total_number)}))
            ++accepted;
    }
    return accepted;
}

FalconTickResult FalconRuntime::run_tick(std::uint64_t total_instruction_budget,
                                         std::uint64_t per_script_slice) {
    FalconTickResult result;
    if (total_instruction_budget == 0 || per_script_slice == 0) return result;
    auto remaining = total_instruction_budget;
    for (auto& [key, script] : impl_->scripts) {
        (void)key;
        if (remaining == 0) break;
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
    }
    return result;
}

std::size_t FalconRuntime::size() const { return impl_->scripts.size(); }

} // namespace homeworldz::script
