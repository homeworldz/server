// Proof-of-concept confirmation for the Homeworldz "Falcon" LSL scripting engine
// (ADR 0021 / SCRIPTING.md). It exercises the whole pipeline and asserts the
// properties that make the approach viable:
//
//   1. Source -> tokens -> AST -> versioned bytecode (handwritten front end).
//   2. Explicit-stack execution with a host-call boundary and no OS access.
//   3. Cooperative scheduling: an infinite loop consumes its instruction budget
//      and yields instead of hanging the region thread.
//   4. Snapshot after ANY single instruction, restored into a fresh VM, finishes
//      identically -- the property that lets a script cross regions mid-handler.
//   5. Semantic diagnostics reject ill-typed programs at compile time.
//   6. Events: multiple handlers (state_entry, touch_start) dispatched with typed
//      parameters, mutating a persistent global across events -- the canonical
//      "touch counter" that a real in-region test will drive.
//   7. A touch handler snapshotted mid-event and restored into a fresh VM keeps
//      its globals, so per-touch state survives a crossing.

#include "homeworldz/script/compiler.h"
#include "homeworldz/script/lexer.h"
#include "homeworldz/script/vm.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using homeworldz::script::compile_source;
using homeworldz::script::HostFunction;
using homeworldz::script::Program;
using homeworldz::script::RunStatus;
using homeworldz::script::ScriptError;
using homeworldz::script::Value;
using homeworldz::script::VM;

namespace {

int g_failures = 0;

void require(bool condition, const std::string& what) {
    std::cout << (condition ? "  [ok] " : "  [FAIL] ") << what << "\n";
    if (!condition) {
        ++g_failures;
    }
}

// A deterministic host boundary that records what scripts "say". No network,
// filesystem, clock, or randomness -- exactly the boundary ADR 0021 requires.
// In a live region, llSay would instead broadcast ChatFromSimulator.
HostFunction make_host(std::vector<std::string>& out) {
    return [&out](const std::string& name,
                  const std::vector<Value>& args) -> std::optional<Value> {
        if (name == "llOwnerSay" && args.size() == 1) {
            out.push_back(args[0].str);
        } else if (name == "llSay" && args.size() == 2) {
            out.push_back(args[1].str);
        }
        return std::nullopt; // both are void
    };
}

const char* kLoopSource = R"LSL(
default
{
    state_entry()
    {
        integer i = 0;
        while (i < 5)
        {
            llOwnerSay("tick " + (string)i);
            i = i + 1;
        }
        llOwnerSay("done");
    }
}
)LSL";

// The canonical touch script: a persistent global counter incremented on each
// click, reported over public chat.
const char* kTouchSource = R"LSL(
integer count;

default
{
    state_entry()
    {
        llOwnerSay("ready");
    }

    touch_start(integer total_number)
    {
        count = count + 1;
        llSay(0, "Touched " + (string)count + " times.");
    }
}
)LSL";

} // namespace

int main() {
    std::cout << "Homeworldz Falcon VM PoC\n";

    // (1) Compile the loop script.
    const Program loop = compile_source(kLoopSource);
    std::cout << "loop script: " << loop.code.size() << " instructions, "
              << loop.events.size() << " event handler(s), bytecode ABI v"
              << loop.abi_version << "\n";

    const std::vector<std::string> loop_expected = {
        "tick 0", "tick 1", "tick 2", "tick 3", "tick 4", "done"};

    // (2) Dispatch state_entry and run to completion.
    std::uint64_t loop_total = 0;
    {
        std::vector<std::string> out;
        VM vm(loop);
        vm.set_host(make_host(out));
        vm.dispatch("state_entry", {});
        const RunStatus status = vm.run(1'000'000);
        require(status == RunStatus::Finished, "state_entry runs to completion");
        require(out == loop_expected, "produces the expected output");
        loop_total = vm.total_instructions();
        std::cout << "  (executed " << loop_total << " instructions)\n";
    }

    // (3) Cooperative scheduling: run in tiny slices; still finishes identically.
    {
        std::vector<std::string> out;
        VM vm(loop);
        vm.set_host(make_host(out));
        vm.dispatch("state_entry", {});
        int slices = 0;
        RunStatus s = RunStatus::Yielded;
        while (s != RunStatus::Finished && slices <= 100000) {
            s = vm.run(4);
            ++slices;
        }
        require(s == RunStatus::Finished && out == loop_expected,
                "same result across many 4-instruction slices");
        require(slices > 1, "execution actually spanned multiple ticks");
    }

    // (3b) An infinite loop is contained, not fatal.
    {
        const Program spin =
            compile_source("default{state_entry(){ integer i=0; while(1){ i=i+1; } }}");
        std::vector<std::string> out;
        VM vm(spin);
        vm.set_host(make_host(out));
        vm.dispatch("state_entry", {});
        const RunStatus a = vm.run(1000);
        const RunStatus b = vm.run(1000);
        require(a == RunStatus::Yielded && b == RunStatus::Yielded,
                "infinite loop yields instead of hanging");
        require(vm.total_instructions() == 2000,
                "each slice runs exactly its instruction budget");
    }

    // (4) Snapshot after EVERY instruction, restore into a fresh VM, finish.
    {
        bool all_identical = true;
        for (std::uint64_t k = 1; k <= loop_total; ++k) {
            std::vector<std::string> combined;
            VM first(loop);
            first.set_host(make_host(combined));
            first.dispatch("state_entry", {});
            first.run(k); // stop after exactly k instructions

            const std::vector<std::uint8_t> snap = first.snapshot();

            VM second(loop);
            second.set_host(make_host(combined));
            second.restore(snap); // resumes without re-dispatching
            second.run(1'000'000);

            if (!(second.finished() && combined == loop_expected)) {
                all_identical = false;
                std::cout << "  [FAIL] mismatch when crossing after instruction " << k << "\n";
                break;
            }
        }
        require(all_identical,
                "snapshot/restore after any instruction reproduces the run exactly");
    }

    // (5) Semantic diagnostics.
    {
        bool threw = false;
        try {
            compile_source("default{state_entry(){ integer i = \"nope\"; }}");
        } catch (const ScriptError&) {
            threw = true;
        }
        require(threw, "ill-typed initializer is rejected at compile time");
    }

    // Lexical diagnostics must identify the exact source location reported to
    // viewer script editors.
    {
        std::string diagnostic;
        try {
            compile_source("default\n{\n'\n}");
        } catch (const ScriptError& error) {
            diagnostic = error.what();
        }
        require(diagnostic == "unexpected character ''' (line 3, column 1)",
                "lexical diagnostics include line and column");
    }

    // (6) Events: state_entry then repeated touch_start, mutating a global.
    const Program touch = compile_source(kTouchSource);
    {
        std::vector<std::string> out;
        VM vm(touch);
        vm.set_host(make_host(out));

        vm.dispatch("state_entry", {});
        require(vm.run(1000) == RunStatus::Finished, "touch script: state_entry fires on rez");

        for (int click = 1; click <= 3; ++click) {
            vm.dispatch("touch_start", {Value::make_integer(1)}); // 1 avatar detected
            require(vm.run(1000) == RunStatus::Finished,
                    "touch_start #" + std::to_string(click) + " runs to completion");
        }

        const std::vector<std::string> expected = {
            "ready", "Touched 1 times.", "Touched 2 times.", "Touched 3 times."};
        require(out == expected, "global counter persists across touch events");
    }

    // (7) Snapshot mid-touch, restore into a fresh VM, and confirm the global
    //     survived: the next touch continues counting from where it left off.
    {
        std::vector<std::string> out;
        VM origin(touch);
        origin.set_host(make_host(out));
        origin.dispatch("touch_start", {Value::make_integer(1)});
        const RunStatus partial = origin.run(4); // mid-handler: count bumped, not yet said
        require(partial == RunStatus::Yielded, "touch handler suspended mid-event");

        const std::vector<std::uint8_t> snap = origin.snapshot();

        VM destination(touch);
        destination.set_host(make_host(out));
        destination.restore(snap);
        require(destination.run(1000) == RunStatus::Finished,
                "restored touch handler finishes on the destination");

        // The next click on the destination must continue the count, proving the
        // global crossed with the snapshot.
        destination.dispatch("touch_start", {Value::make_integer(1)});
        destination.run(1000);

        const std::vector<std::string> expected = {"Touched 1 times.", "Touched 2 times."};
        require(out == expected, "globals survive a mid-event crossing");
    }

    // 8. A typical pose-ball script (ADR 0040) compiles and runs: keys and their
    //    truth, named constants, vector and rotation globals, `&` against
    //    CHANGED_LINK, `!=` against NULL_KEY, and the permission round trip.
    {
        const std::string pose_ball = R"(
            vector SEAT = <0.0, 0.0, 0.45>;
            rotation FACING = ZERO_ROTATION;
            key sitter = NULL_KEY;
            integer link = -4;
            default {
                state_entry() {
                    llSitTarget(SEAT, FACING);
                    llSetLinkSitTarget(2, <0.25, 0.0, 0.6>, <0.0, 0.0, 0.707107, 0.707107>);
                    llSetSitText("Relax");
                    llSetClickAction(CLICK_ACTION_SIT);
                }
                changed(integer change) {
                    if (change & CHANGED_LINK) {
                        key av = llAvatarOnSitTarget();
                        if (av) {
                            sitter = av;
                            llRequestPermissions(av, PERMISSION_TRIGGER_ANIMATION);
                        } else if (sitter != NULL_KEY) {
                            sitter = NULL_KEY;
                            llOwnerSay("stood");
                        }
                    }
                }
                run_time_permissions(integer perm) {
                    if (perm & PERMISSION_TRIGGER_ANIMATION && !(link == 0)) {
                        llStopAnimation("sit");
                        llStartAnimation(llGetInventoryName(INVENTORY_ANIMATION, 0));
                    }
                }
            }
        )";
        std::vector<std::string> calls;
        std::vector<Value> seat_args;
        std::string seated = "11111111-2222-4333-8444-555555555555";
        const HostFunction host = [&](const std::string& name,
                                      const std::vector<Value>& args) -> std::optional<Value> {
            calls.push_back(name);
            if (name == "llSitTarget" || name == "llSetLinkSitTarget") {
                seat_args.insert(seat_args.end(), args.begin(), args.end());
            }
            if (name == "llAvatarOnSitTarget") return Value::make_string(seated);
            if (name == "llGetInventoryName") return Value::make_string("relax pose");
            if (name == "llStartAnimation") calls.push_back("start:" + args[0].str);
            if (name == "llOwnerSay") calls.push_back("say:" + args[0].str);
            return std::nullopt;
        };
        Program program = compile_source(pose_ball);
        VM vm(program);
        vm.set_host(host);
        vm.dispatch("state_entry", {});
        require(vm.run(1000) == RunStatus::Finished, "pose ball state_entry runs");
        // The two seats must stay two seats: pooling constants by type, integer
        // and string alone made every vector literal the first one written.
        require(seat_args.size() == 5 && seat_args[0].components[2] == 0.45 &&
                    seat_args[3].components[0] == 0.25 && seat_args[3].components[2] == 0.6 &&
                    seat_args[1].components[3] == 1.0 && seat_args[4].components[2] > 0.7,
                "distinct vector and rotation literals keep their own values");

        vm.dispatch("changed", {Value::make_integer(0x20)});
        vm.run(1000);
        require(calls.back() == "llRequestPermissions", "a sitter is asked for permissions");
        vm.dispatch("run_time_permissions", {Value::make_integer(0x10)});
        vm.run(1000);
        require(calls.back() == "start:relax pose", "the granted script starts its pose");

        seated = "00000000-0000-0000-0000-000000000000";
        vm.dispatch("changed", {Value::make_integer(0x20)});
        vm.run(1000);
        require(calls.back() == "say:stood", "NULL_KEY is false and the stand is seen");

        // Some other change is not a link change.
        const auto before = calls.size();
        vm.dispatch("changed", {Value::make_integer(0x01)});
        vm.run(1000);
        require(calls.size() == before, "a non-link change does nothing");
    }

    // 9. A vector waiting on the operand stack survives a snapshot as itself.
    {
        const std::string source = R"(
            default { state_entry() { llSitTarget(<1.5, -2.0, 0.25>, <0.0, 0.0, 0.0, 1.0>); } }
        )";
        Program program = compile_source(source);
        std::vector<Value> received;
        const HostFunction host = [&](const std::string&, const std::vector<Value>& args)
            -> std::optional<Value> {
            received = args;
            return std::nullopt;
        };
        VM origin(program);
        origin.set_host(host);
        origin.dispatch("state_entry", {});
        origin.run(1); // pushed the vector, not yet the rotation or the call
        VM destination(program);
        destination.set_host(host);
        destination.restore(origin.snapshot());
        destination.run(100);
        require(received.size() == 2 && received[0].type == homeworldz::script::Type::Vector &&
                    received[0].components[0] == 1.5 && received[0].components[1] == -2.0 &&
                    received[1].components[3] == 1.0,
                "a vector on the stack crosses a snapshot intact");
    }

    // 10. Diagnostics for the new types.
    {
        bool refused = false;
        try {
            compile_source(R"(default { state_entry() { integer i = "x" & 1; } })");
        } catch (const ScriptError&) {
            refused = true;
        }
        require(refused, "'&' refuses a string operand");
        refused = false;
        try {
            compile_source(R"(key k = 5; default { state_entry() { } })");
        } catch (const ScriptError&) {
            refused = true;
        }
        require(refused, "a key global refuses an integer initializer");
        refused = false;
        try {
            compile_source(R"(integer i = llGetPermissions(); default { state_entry() { } })");
        } catch (const ScriptError&) {
            refused = true;
        }
        require(refused, "a global initializer must be a constant");
    }

    if (g_failures == 0) {
        std::cout << "ALL PoC CHECKS PASSED\n";
        return EXIT_SUCCESS;
    }
    std::cout << g_failures << " CHECK(S) FAILED\n";
    return EXIT_FAILURE;
}
