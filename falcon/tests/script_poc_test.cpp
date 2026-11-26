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

    // Syntax and semantic diagnostics carry the position too, at the token or
    // the node the error belongs to.
    {
        const auto error_of = [](const std::string& source) {
            try {
                compile_source(source);
            } catch (const ScriptError& error) {
                return error;
            }
            return ScriptError("compiled");
        };
        // A missing semicolon is noticed at the token that should have been one.
        auto error = error_of("default\n{\n    state_entry()\n    {\n        llOwnerSay(\"a\")\n    }\n}");
        require(error.line == 6 && error.column == 5 &&
                    std::string(error.what()) == "expected ';' after expression (line 6, column 5)",
                "a syntax error names the token it stopped at");
        // An unknown function is reported at the call, not at its argument.
        error = error_of("default\n{\n    state_entry()\n    {\n        integer i = 1;\n"
                         "        llNoSuchThing(i);\n    }\n}");
        require(error.line == 6 && error.column == 9 && error.message == "unknown function 'llNoSuchThing'",
                "an unknown function is reported at the call");
        // A type error found after both operands compiled belongs to the operator.
        error = error_of("default\n{\n    state_entry()\n    {\n        integer i = \"x\" & 1;\n    }\n}");
        require(error.line == 5 && error.column == 25, "an operator's type error is reported at the operator");
        // A bad argument is reported at the call that refused it.
        error = error_of("default { state_entry() {\n  llSay(\"zero\", \"hi\");\n} }");
        require(error.line == 2 && error.column == 3, "a refused argument is reported at its call");
        // `print` is a keyword in LSL's grammar and cannot name a variable.
        error = error_of("default { state_entry() {\n  integer print = 1;\n} }");
        require(error.line == 2 && error.column == 11 && error.message == "expected variable name",
                "print is reserved");
        error = error_of("integer a;\ninteger a;\ndefault { state_entry() { } }");
        require(error.line == 2 && error.column == 1, "a duplicate global is reported where it is declared again");
        error = error_of("default {\n  state_entry() { }\n  no_such_event() { }\n}");
        require(error.line == 3 && error.column == 3, "an unsupported event is reported at its handler");
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
                    llLinkSitTarget(2, <0.25, 0.0, 0.6>, <0.0, 0.0, 0.707107, 0.707107>);
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
            if (name == "llSitTarget" || name == "llLinkSitTarget") {
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

    // 11. The generated index supplies signatures and constants, and decides
    // nothing about what compiles.
    {
        const auto diagnostic_of = [](const std::string& source) {
            try {
                compile_source(source);
            } catch (const ScriptError& error) {
                return std::string(error.what());
            }
            return std::string();
        };
        // llGetPos is in the index with a signature Falcon could express, and
        // the region does not answer it.
        require(diagnostic_of("default { state_entry() { vector v = llGetPos(); } }")
                        .find("unknown function 'llGetPos'") != std::string::npos,
                "a function the index knows but the region does not answer is refused");
        require(diagnostic_of("default { state_entry() { llSetLinkSitTarget(2, ZERO_VECTOR, ZERO_ROTATION); } }")
                        .find("unknown function 'llSetLinkSitTarget'") != std::string::npos,
                "llSetLinkSitTarget, which no grid has, is refused");
        require(diagnostic_of("default { state_entry() { llLinkSitTarget(2, ZERO_VECTOR, ZERO_ROTATION); } }")
                        .empty(),
                "llLinkSitTarget compiles with the index's signature");
        require(diagnostic_of("default { state_entry() { llLinkSitTarget(ZERO_VECTOR, ZERO_ROTATION); } }")
                        .find("expects 3 argument(s)") != std::string::npos,
                "an implemented function's argument count comes from the index");
        require(diagnostic_of("default { money(key id, integer amount) { } }")
                        .find("unsupported event 'money'") != std::string::npos,
                "an event the index knows but the region does not raise is refused");

        // A constant the hand-kept table never had, with Second Life's value.
        const Program program = compile_source(
            "default { state_entry() { llSay(CHANGED_INVENTORY, EOF); } }");
        bool has_value = false, has_eof = false;
        for (const auto& constant : program.constants) {
            if (constant.type == homeworldz::script::Type::Integer && constant.integer == 0x1) has_value = true;
            if (constant.type == homeworldz::script::Type::String && constant.str == "\n\n\n") has_eof = true;
        }
        require(has_value && has_eof, "constants take their values from the index");
    }

    // 12. Host call results: a call can put the script to sleep, stay in
    // progress until the host answers, or fail. Each is state the VM holds in
    // data, so each crosses a snapshot.
    {
        using homeworldz::script::HostResult;
        using homeworldz::script::Type;
        std::vector<std::string> out;
        std::vector<Value> slept;
        const HostFunction host = [&](const std::string& name,
                                      const std::vector<Value>& args) -> HostResult {
            if (name == "llOwnerSay") {
                out.push_back(args[0].str);
                return std::nullopt;
            }
            if (name == "llSleep") {
                slept.push_back(args[0]);
                return HostResult::sleep(std::nullopt, args[0].number());
            }
            if (name == "llGetPermissionsKey") return HostResult::suspend(42);
            if (name == "llGetPermissions") return HostResult::fail("the region refused");
            return std::nullopt;
        };

        const Program sleeper = compile_source(
            "default { state_entry() { llOwnerSay(\"a\"); llSleep(2.5); llOwnerSay(\"b\"); } }");
        VM vm(sleeper);
        vm.set_host(host);
        vm.dispatch("state_entry", {});
        require(vm.run(1000) == RunStatus::Sleeping && out == std::vector<std::string>{"a"} &&
                    vm.sleep_remaining() == 2.5,
                "llSleep stops the script where it is, for the time it asked");
        const auto before = vm.total_instructions();
        require(vm.run(1000) == RunStatus::Sleeping && vm.total_instructions() == before,
                "a sleeping script runs nothing");
        // Part of the sleep has passed when the snapshot is taken.
        vm.set_sleep_remaining(1.0);
        VM elsewhere(sleeper);
        elsewhere.set_host(host);
        elsewhere.restore(vm.snapshot());
        require(elsewhere.run(1000) == RunStatus::Sleeping && elsewhere.sleep_remaining() == 1.0,
                "the sleep left crosses a snapshot");
        elsewhere.wake();
        require(elsewhere.run(1000) == RunStatus::Finished &&
                    out == (std::vector<std::string>{"a", "b"}),
                "a woken script goes on from the instruction after the call");

        // A float is a value a script can write; an integer is widened to one.
        slept.clear();
        const Program widened = compile_source("default { state_entry() { llSleep(2); llSleep(.5); } }");
        VM widening(widened);
        widening.set_host(host);
        widening.dispatch("state_entry", {});
        widening.run(1000);
        widening.wake();
        widening.run(1000);
        require(slept.size() == 2 && slept[0].type == Type::Float && slept[0].number() == 2.0 &&
                    slept[1].type == Type::Float && slept[1].number() == 0.5,
                "an integer given for a float arrives as a float");
        bool refused = false;
        try {
            compile_source("default { state_entry() { llOwnerSay(1.5); } }");
        } catch (const ScriptError&) {
            refused = true;
        }
        require(refused, "a float is not accepted where a string is wanted");

        // A call still in progress: the script waits, and the answer arrives
        // by token, on this VM or on the one a snapshot was restored into.
        out.clear();
        const Program asker = compile_source(
            "default { state_entry() { llOwnerSay((string)llGetPermissionsKey()); } }");
        VM asking(asker);
        asking.set_host(host);
        asking.dispatch("state_entry", {});
        require(asking.run(1000) == RunStatus::Waiting && asking.waiting() &&
                    asking.waiting_token() == 42 && out.empty(),
                "a suspended host call leaves the script waiting on its token");
        require(!asking.resume(41, Value::make_string("wrong")) && asking.waiting(),
                "an answer for another token is refused");
        VM arrived(asker);
        arrived.set_host(host);
        arrived.restore(asking.snapshot());
        require(arrived.run(1000) == RunStatus::Waiting && arrived.waiting_token() == 42,
                "a host call in progress crosses a snapshot");
        require(arrived.resume(42, Value::make_string("the-answer")) &&
                    arrived.run(1000) == RunStatus::Finished &&
                    out == std::vector<std::string>{"the-answer"},
                "the answer completes the call and the script goes on");

        // A call that fails stops the script with the host's message.
        const Program failing = compile_source(
            "default { state_entry() { integer p = llGetPermissions(); llOwnerSay(\"unreached\"); } }");
        VM failed(failing);
        failed.set_host(host);
        failed.dispatch("state_entry", {});
        out.clear();
        require(failed.run(1000) == RunStatus::Error && failed.error() == "the region refused" &&
                    out.empty(),
                "a failed host call is a runtime error");
    }

    // (floats) A float is a 32-bit value with its own arithmetic, an integer
    // widens to one where a float is wanted, and nothing narrows without a cast.
    {
        std::vector<std::string> out;
        const auto run = [&out](const std::string& globals, const std::string& body) {
            out.clear();
            const Program program =
                compile_source(globals + "\ndefault { state_entry() {\n" + body + "\n} }");
            VM vm(program);
            vm.set_host(make_host(out));
            vm.dispatch("state_entry", {});
            return vm.run(10000);
        };
        const auto refused = [](const std::string& body) {
            try {
                compile_source("default { state_entry() { " + body + " } }");
            } catch (const ScriptError&) {
                return true;
            }
            return false;
        };

        run("float g = 2; float h = -1.5; float tiny = 1e-3;",
            "float f = 0.1; f = f + 1;\n"
            "llOwnerSay((string)f + \" \" + (string)g + \" \" + (string)h + \" \" + (string)tiny);\n"
            "llOwnerSay((string)(7 / 2) + \" \" + (string)(7 / 2.0) + \" \" + (string)(1 - .5) + \" \" + (string)(3 * 1.5f));\n"
            "llOwnerSay((string)(-7 / 2) + \" \" + (string)(-7 % 2) + \" \" + (string)(7 % -2));\n"
            "llOwnerSay((string)(-f));");
        require(out == std::vector<std::string>{"1.100000 2.000000 -1.500000 0.001000",
                                                "3 3.500000 0.500000 4.500000", "-3 -1 1",
                                                "-1.100000"},
                "floats print six decimals, integers widen beside them, and integer division truncates");

        run("", "float f = 16777217; integer i = 16777217;\n"
                "llOwnerSay((string)f + \" \" + (string)i + \" \" + (string)(16777216.0 + 1));\n"
                "llOwnerSay((string)(0.1 + 0.2 == 0.3) + (string)(0.5 + 0.25 == 0.75));");
        // In 32 bits 0.1 + 0.2 is exactly 0.3; in 64 it is not.
        require(out == std::vector<std::string>{"16777216.000000 16777217 16777216.000000", "11"},
                "a float holds 32 bits, at a store and at every result");

        run("", "float a = 1.5; integer two = 2;\n"
                "llOwnerSay((string)(a < two) + (string)(a > two) + (string)(a <= 1.5) + (string)(a >= two)"
                " + (string)(a == 1.5) + (string)(a != 1.5) + (string)(two == 2.0) + (string)(a == two));\n"
                "integer n = 3;\n"
                "llOwnerSay((string)(n <= 3) + (string)(n >= 4) + (string)(n <= 2) + (string)(n >= 3));\n"
                "float nan = (float)\"nan\";\n"
                "llOwnerSay((string)(nan < 1) + (string)(nan >= 1) + (string)(nan <= 1) + (string)(nan == nan)"
                " + (string)(nan != nan));\n"
                "float zero; if (zero) { llOwnerSay(\"zero is true\"); } if (a) { llOwnerSay(\"a is true\"); }");
        require(out == std::vector<std::string>{"10101010", "1001", "00001", "a is true"},
                "floats and integers compare with each other, and nothing orders against NaN");

        run("", "llOwnerSay((string)((integer)1.9) + \" \" + (string)((integer)-1.9) + \" \" + (string)((integer)1e20)"
                " + \" \" + (string)((integer)(float)\"nan\"));\n"
                "llOwnerSay((string)((float)\"1.5e2\") + \" \" + (string)((float)\"  -3.25xyz\") + \" \" + (string)((float)\"x\")"
                " + \" \" + (string)((float)\".\") + \" \" + (string)((float)\"-INF\") + \" \" + (string)((float)7));\n"
                "llOwnerSay((string)((integer)\"0x1A\") + \" \" + (string)((integer)\"  42abc\") + \" \" + (string)((integer)\"abc\")"
                " + \" \" + (string)((integer)\"-17\") + \" \" + (string)((integer)\"4294967296\"));\n"
                "llOwnerSay((string)1e30 + \" \" + (string)PI + \" \" + (string)(TWO_PI / 2 == PI));");
        require(out == std::vector<std::string>{"1 -1 -2147483648 -2147483648",
                                                "150.000000 -3.250000 0.000000 0.000000 -Infinity 7.000000",
                                                "26 42 0 -17 -1",
                                                "1000000015047466219876688855040.000000 3.141593 1"},
                "casts between float, integer and string follow Second Life");

        require(refused("integer i = 1.5;") && refused("integer i; i = 1.0;") &&
                    refused("llOwnerSay(\"x\" + 1.5);") && refused("float f = \"1.5\";") &&
                    refused("float f = 1.5 % 2;") && refused("key k = (key)1.5;") &&
                    !refused("float f = 1; f = 2; llSleep(1); llOwnerSay(\"x\" + (string)1.5);"),
                "a float never narrows or turns into a string without a cast");

        // A division by zero stops the event with LSL's message and the line
        // it was written on; the script takes the next event.
        const Program dividing = compile_source(
            "default {\n"
            " state_entry() {\n"
            "  integer zero = 0;\n"
            "  llOwnerSay(\"before\");\n"
            "  llOwnerSay((string)(5 /\n"
            "      zero));\n"
            " }\n"
            " touch_start(integer n) {\n"
            "  float z;\n"
            "  llOwnerSay((string)(n / z));\n"
            " }\n"
            " timer() { llOwnerSay((string)(5 % 0)); }\n"
            " changed(integer c) { integer lo = -2147483647 - 1;\n"
            "  llOwnerSay((string)(lo / c) + \" \" + (string)(lo % c)); }\n"
            "}\n");
        VM divider(dividing);
        out.clear();
        divider.set_host(make_host(out));
        divider.dispatch("state_entry", {});
        require(divider.run(1000) == RunStatus::Error && divider.error() == "Math Error" &&
                    divider.error_line() == 5 && out == std::vector<std::string>{"before"},
                "integer / 0 is a Math Error at the operator's line");
        divider.dispatch("touch_start", {Value::make_integer(1)});
        const bool float_fault = divider.run(1000) == RunStatus::Error && divider.error_line() == 10;
        divider.dispatch("timer", {});
        require(float_fault && divider.run(1000) == RunStatus::Error && divider.error() == "Math Error" &&
                    divider.error_line() == 12,
                "float / 0.0 and integer % 0 are Math Errors, and the script takes the next event");
        out.clear();
        divider.dispatch("changed", {Value::make_integer(-1)});
        require(divider.run(1000) == RunStatus::Finished &&
                    out == std::vector<std::string>{"-2147483648 0"},
                "the least integer divided by -1 is itself, with no remainder");

        // A float on the stack and in a local crosses a snapshot.
        const Program crossing = compile_source(
            "float total;\n"
            "default { state_entry() { float step = 0.25; integer i = 0;\n"
            " while (i < 4) { total = total + step; i = i + 1; }\n"
            " llOwnerSay((string)total + \" \" + (string)(total * (step + 1))); } }");
        VM whole(crossing);
        std::vector<std::string> whole_out;
        whole.set_host(make_host(whole_out));
        whole.dispatch("state_entry", {});
        whole.run(100000);
        bool every_point = whole_out == std::vector<std::string>{"1.000000 1.250000"};
        for (std::uint64_t cut = 1; cut < whole.total_instructions(); ++cut) {
            VM first(crossing);
            std::vector<std::string> cut_out;
            first.set_host(make_host(cut_out));
            first.dispatch("state_entry", {});
            first.run(cut);
            VM second(crossing);
            second.set_host(make_host(cut_out));
            second.restore(first.snapshot());
            second.run(100000);
            every_point = every_point && cut_out == whole_out;
        }
        require(every_point, "a script doing float arithmetic can be snapshotted after any instruction");
    }

    // (lists) A list holds values of any other type and remembers the type of
    // each; every operation on one makes a new list.
    {
        std::vector<std::string> out;
        const auto run = [&out](const std::string& globals, const std::string& body) {
            out.clear();
            const Program program =
                compile_source(globals + "\ndefault { state_entry() {\n" + body + "\n} }");
            VM vm(program);
            vm.set_host(make_host(out));
            vm.dispatch("state_entry", {});
            const RunStatus status = vm.run(100000);
            if (status == RunStatus::Error) out.push_back("error: " + vm.error());
            return status;
        };
        const auto refused = [](const std::string& body) {
            try {
                compile_source("default { state_entry() { " + body + " } }");
            } catch (const ScriptError&) {
                return true;
            }
            return false;
        };

        run("integer g = 5; list gl = [g, \"x\", 1.5]; list empty; key who = \"abc\";",
            "list l = [1, 2.5, \"a\", <1,2,3>, (key)\"k\", <0,0,0,1>, who, NULL_KEY];\n"
            "llOwnerSay((string)l);\n"
            "llOwnerSay((string)gl + \" \" + (string)llGetListLength(gl) + (string)llGetListLength(empty));\n"
            "string types = (string)llGetListEntryType(l, 0) + (string)llGetListEntryType(l, 1);\n"
            "types = types + (string)llGetListEntryType(l, 2) + (string)llGetListEntryType(l, 3);\n"
            "types = types + (string)llGetListEntryType(l, 4) + (string)llGetListEntryType(l, 5);\n"
            "types = types + (string)llGetListEntryType(l, 6) + (string)llGetListEntryType(l, -1);\n"
            "llOwnerSay(types + (string)llGetListEntryType(l, 8));");
        require(out == std::vector<std::string>{
                           "12.500000a<1.00000, 2.00000, 3.00000>k<0.00000, 0.00000, 0.00000, 1.00000>abc"
                           "00000000-0000-0000-0000-000000000000",
                           "5x1.500000 30", "123546440"},
                "a list keeps each element and its type, a key apart from a string");

        run("", "list a = [1, 2]; list b = a; b = b + 3; b = 0 + b; a = a + [\"x\", \"y\"] + [];\n"
                "llOwnerSay((string)a + \" \" + (string)b + \" \" + (string)llGetListLength((list)\"one\"));\n"
                "llOwnerSay((string)([1, 2] == [3, 4]) + (string)([1] == [1, 2]) + \" \" + (string)([1, 2, 3] != [1])"
                " + \" \" + (string)([1] != [1, 2, 3]));\n"
                "list none; if (none) { llOwnerSay(\"empty is true\"); } if (a) { llOwnerSay(\"a is true\"); }\n"
                "while (b) { b = llDeleteSubList(b, 0, 0); } llOwnerSay(\"[\" + (string)b + \"]\");");
        require(out == std::vector<std::string>{"12xy 0123 1", "10 2 -2", "a is true", "[]"},
                "lists are values: + makes a new one, == compares lengths, and an empty one is false");

        run("list l = [7, 2.75, \"12abc\", <1,2,3>, \"0x10\", \" 1.5e1x\"];",
            "key k = \"key-text\"; list keys = [k, \"s\", 4];\n"
            "llOwnerSay((string)llList2Integer(l, 0) + \" \" + (string)llList2Integer(l, 1) + \" \" + (string)llList2Integer(l, 2)"
            " + \" \" + (string)llList2Integer(l, 3) + \" \" + (string)llList2Integer(l, 4) + \" \" + (string)llList2Integer(l, 99)"
            " + \" \" + (string)llList2Integer(l, -6) + \" \" + (string)llList2Integer(l, -7));\n"
            "llOwnerSay((string)llList2Float(l, 0) + \" \" + (string)llList2Float(l, 1) + \" \" + (string)llList2Float(l, 5)"
            " + \" \" + (string)llList2Float(l, 3) + \" \" + llList2String(l, 1) + \" \" + llList2String(l, 3) + \"|\" + llList2String(l, 9) + \"|\");\n"
            "llOwnerSay((string)llList2Key(keys, 0) + \"|\" + (string)llList2Key(keys, 1) + \"|\" + (string)llList2Key(keys, 2) + \"|\");\n"
            "llOwnerSay(llDumpList2String([1, 2.5, \"x\"], \", \") + \"|\" + llDumpList2String([], \",\") + \"|\");");
        require(out == std::vector<std::string>{"7 2 12 0 16 0 7 0",
                                                "7.000000 2.750000 15.000000 0.000000 2.750000 <1.00000, 2.00000, 3.00000>||",
                                                "key-text|s||", "1, 2.500000, x||"},
                "llList2Integer, Float, String and Key convert what they find and give nothing outside the list");

        run("list l = [0, 1, 2, 3, 4, 5]; list s = [0, 1, 2, 3];",
            "llOwnerSay((string)llList2List(l, 1, 3) + \" \" + (string)llList2List(l, 4, 1) + \" \" + (string)llList2List(l, -2, -1)"
            " + \" \" + (string)llList2List(l, 2, 99) + \" \" + (string)llList2List(l, -99, 1) + \"|\" + (string)llList2List(l, 7, 9) + \"|\");\n"
            "llOwnerSay((string)llDeleteSubList(l, 1, 3) + \" \" + (string)llDeleteSubList(l, 3, 1) + \" \" + (string)llDeleteSubList(l, 0, -1)"
            " + \"|\" + (string)llDeleteSubList(l, 9, 12) + \" \" + (string)llDeleteSubList(l, -1, -1));\n"
            "llOwnerSay((string)llListInsertList(s, [\"x\"], 2) + \" \" + (string)llListInsertList(s, [\"x\"], -1) + \" \""
            " + (string)llListInsertList(s, [\"x\"], 9) + \" \" + (string)llListInsertList(s, [\"x\"], -9));\n"
            "llOwnerSay((string)llListReplaceList(s, [\"x\", \"y\"], 1, 2) + \" \" + (string)llListReplaceList(s, [\"x\"], 3, 1)"
            " + \" \" + (string)llListReplaceList(s, [\"x\"], 9, 9) + \" \" + (string)llListReplaceList(s, [], 0, 0));\n"
            "llOwnerSay((string)llList2ListStrided([0, 1, 2, 3, 4, 5, 6, 7], 0, -1, 2) + \" \""
            " + (string)llList2ListStrided([0, 1, 2, 3, 4, 5, 6, 7], 1, 6, 3) + \" \" + (string)llList2ListStrided(s, 0, -1, 0));");
        require(out == std::vector<std::string>{"123 0145 45 2345 01||", "045 2 |012345 01234",
                                                "01x23 012x3 0123x x0123", "0xy3 2x 0123x 123",
                                                "0246 36 0123"},
                "list ranges count from the end when negative and wrap when inverted");

        run("list l = [1, 2, 3, 2, \"2\", 2.0];",
            "key two = \"2\";\n"
            "llOwnerSay((string)llListFindList(l, [2]) + \" \" + (string)llListFindList(l, [3, 2]) + \" \" + (string)llListFindList(l, [\"2\"])"
            " + \" \" + (string)llListFindList(l, [9]) + \" \" + (string)llListFindList(l, []) + \" \" + (string)llListFindList([\"1\"], [1])"
            " + \" \" + (string)llListFindList(l, [2.0]) + \" \" + (string)llListFindList(l, [two]) + \" \" + (string)llListFindList(l, [2, 3, 2, \"2\", 2.0, 1]));");
        require(out == std::vector<std::string>{"1 2 4 -1 0 -1 5 -1 -1"},
                "llListFindList matches value and type together");

        require(refused("list l = [[1]];") && refused("list l = [llOwnerSay(\"x\")];") &&
                    refused("list l = 1;") && refused("integer n = [1];") && refused("string s = [1];") &&
                    refused("list l = [1] - [1];") && refused("integer n = llGetListLength(1);") &&
                    !refused("list l = (list)1 + 2 + \"x\"; string s = (string)l; integer n = llGetListLength([]);"),
                "a list holds no list, and becomes one only by a cast or by adding to one");

        // A list in a global, a local and on the stack crosses a snapshot at
        // every instruction, keys and all.
        const Program crossing = compile_source(
            "list seen;\n"
            "default { state_entry() { list mine = [(key)\"k\", 1.5]; integer i = 0;\n"
            " while (i < 3) { seen = seen + [i, \"s\"] + mine; i = i + 1; }\n"
            " llOwnerSay((string)seen + \" \" + (string)llGetListEntryType(seen, 2) + (string)llGetListEntryType(seen, 1)); } }");
        VM whole(crossing);
        std::vector<std::string> whole_out;
        whole.set_host(make_host(whole_out));
        whole.dispatch("state_entry", {});
        whole.run(100000);
        bool every_point = whole_out == std::vector<std::string>{"0sk1.5000001sk1.5000002sk1.500000 43"};
        for (std::uint64_t cut = 1; cut < whole.total_instructions(); ++cut) {
            VM first(crossing);
            std::vector<std::string> cut_out;
            first.set_host(make_host(cut_out));
            first.dispatch("state_entry", {});
            first.run(cut);
            VM second(crossing);
            second.set_host(make_host(cut_out));
            second.restore(first.snapshot());
            second.run(100000);
            every_point = every_point && cut_out == whole_out;
        }
        require(every_point, "a script building lists can be snapshotted after any instruction");

        // A key is a key at runtime wherever it came from: a host function's
        // answer, an event's argument, a string assigned to one.
        const Program keyed = compile_source(
            "default { state_entry() { key me = llGetPermissionsKey(); string text = me; key again = text;\n"
            " llOwnerSay((string)llGetListEntryType([me], 0) + (string)llGetListEntryType([text], 0)"
            " + (string)llGetListEntryType([again], 0) + (string)llGetListEntryType([llGetPermissionsKey()], 0)"
            " + (string)llGetListEntryType([(string)me], 0));\n"
            " if (me) { llOwnerSay(\"named \" + (string)me + (string)(me == text)); } } }");
        VM keyed_vm(keyed);
        out.clear();
        keyed_vm.set_host([&out](const std::string& name, const std::vector<Value>& args) -> homeworldz::script::HostResult {
            if (name == "llOwnerSay") out.push_back(args[0].str);
            // A region answers a key as plain text.
            if (name == "llGetPermissionsKey") return Value::make_string("a822ff2b-ff02-461d-b45d-dcd10a2de0c2");
            return std::nullopt;
        });
        keyed_vm.dispatch("state_entry", {});
        require(keyed_vm.run(1000) == RunStatus::Finished &&
                    out == std::vector<std::string>{"43443", "named a822ff2b-ff02-461d-b45d-dcd10a2de0c21"},
                "a key stays a key through assignment and a host call, and a string a string");
    }

    // (functions) A script's own functions run in call frames the VM keeps as
    // data, so a call can be interrupted and snapshotted like anything else.
    {
        std::vector<std::string> out;
        const auto run = [&out](const std::string& source) {
            out.clear();
            const Program program = compile_source(source);
            VM vm(program);
            vm.set_host(make_host(out));
            vm.dispatch("state_entry", {});
            const RunStatus status = vm.run(1000000);
            if (status == RunStatus::Error) {
                out.push_back("error: " + vm.error() + " at line " + std::to_string(vm.error_line()));
            }
            return status;
        };
        const auto refused = [](const std::string& source) {
            try {
                compile_source(source);
            } catch (const ScriptError&) {
                return true;
            }
            return false;
        };
        const std::string entry = "default { state_entry() { ";

        run("integer fact(integer n) { if (n <= 1) return 1; return n * fact(n - 1); }\n"
            "integer fib(integer n) { if (n < 2) return n; return fib(n - 1) + fib(n - 2); }\n"
            "integer even(integer n) { if (n == 0) return 1; return odd(n - 1); }\n"
            "integer odd(integer n) { if (n == 0) { return 0; } else { return even(n - 1); } }\n"
            "string count;\n"
            "note(string what) { count = count + what; if (what == \"stop\") return; count = count + \",\"; }\n"
            "float half(float x) { return x / 2; }\n"
            "float whole(integer n) { return n; }\n"
            "key named(string text) { return text; }\n"
            "list pair(integer a, string b) { return [a, b]; }\n" +
            entry +
            "llOwnerSay((string)fact(10) + \" \" + (string)fib(15) + \" \" + (string)even(10) + (string)even(7));\n"
            "note(\"a\"); note(\"stop\"); note(\"b\"); llOwnerSay(count);\n"
            "llOwnerSay((string)half(5) + \" \" + (string)whole(16777217) + \" \" + (string)llGetListEntryType([named(\"k\")], 0)"
            " + \" \" + (string)pair(1, \"x\") + (string)llGetListLength(pair(2, \"y\")));\n"
            "} }");
        require(out == std::vector<std::string>{"3628800 610 10", "a,stopb,", "2.500000 16777216.000000 4 1x2"},
                "functions recurse, call each other in any order, and convert what they are given and return");

        // Each call has locals of its own, and a caller's survive the call.
        run("integer depth(integer n) { integer mine = n * 10; if (n > 0) { integer below = depth(n - 1);"
            " return mine + below; } return mine; }\n"
            "integer g = 1;\n"
            "integer g(integer g) { return g + 1; }\n" +
            entry + "integer keep = 7; integer sum = depth(3); llOwnerSay((string)sum + \" \" + (string)keep + \" \" + (string)g(g)); } }");
        require(out == std::vector<std::string>{"60 7 2"},
                "each call has its own locals, and a function may share a global's name");

        // LSL runs the right operand first, then the left; arguments and list
        // elements run left to right. The value is the same either way.
        run("integer f(integer x) { llOwnerSay(\"f\" + (string)x); return x; }\n"
            "string s(string x) { llOwnerSay(\"s\" + x); return x; }\n"
            "integer two(integer a, integer b) { return a * 10 + b; }\n" +
            entry +
            "llOwnerSay((string)(f(1) - f(2)));\n"
            "llOwnerSay(s(\"a\") + s(\"b\"));\n"
            "llOwnerSay((string)two(f(3), f(4)) + (string)[f(5), f(6)]);\n"
            "llOwnerSay((string)(f(7) < f(8)) + (string)(f(8) / f(2)) + (string)(f(9) % f(4)) + (string)(f(1) >= f(1)));\n"
            "llOwnerSay((string)([f(1)] + f(2)) + (string)(f(3) + [f(4)]) + (string)([1, 2, 3] != [f(0)]));\n"
            "} }");
        // In a chain of +, that means the last term runs first.
        require(out == std::vector<std::string>{"f2", "f1", "-1", "sb", "sa", "ab", "f5", "f6", "f3", "f4", "3456",
                                                "f1", "f1", "f4", "f9", "f2", "f8", "f8", "f7", "1411",
                                                "f0", "f4", "f3", "f2", "f1", "12342"},
                "the right operand of an operator runs first, and the result is unchanged");

        run("spin(integer n) { spin(n + 1); }\n"
            "default {\n state_entry() {\n  spin(0);\n }\n touch_start(integer n) { llOwnerSay(\"still here\"); }\n}");
        require(out == std::vector<std::string>{"error: Stack-Heap Collision at line 1"},
                "recursion with no end stops the event with a Stack-Heap Collision");

        // A list or a string grown without end stops the same way, and what
        // the script held before the value too large to build is kept.
        run("default {\n state_entry() {\n  list l;\n  integer n;\n"
            "  while (1) {\n   n = llGetListLength(l);\n   llOwnerSay((string)n);\n"
            "   l = l + [\"0123456789abcdef\", \"0123456789abcdef\", \"0123456789abcdef\", \"0123456789abcdef\"] + l;\n"
            "  }\n }\n}");
        require(out.size() > 2 && out.size() < 20 && out.back() == "error: Stack-Heap Collision at line 8",
                "a list grown without end stops the event with a Stack-Heap Collision");
        run("default {\n state_entry() {\n  string s = \"0123456789abcdef\";\n  while (1) s = s + s;\n }\n}");
        require(out == std::vector<std::string>{"error: Stack-Heap Collision at line 4"},
                "a string grown without end stops the event with a Stack-Heap Collision");

        run("integer zero;\n"
            "integer divide(integer n) {\n"
            "  return n /\n"
            "      zero;\n"
            "}\n"
            "default { state_entry() { llOwnerSay(\"a\"); return; llOwnerSay(\"b\"); }\n"
            " touch_start(integer n) { llOwnerSay((string)divide(n)); } }");
        require(out == std::vector<std::string>{"a"}, "a bare return ends an event handler");
        {
            const Program program = compile_source(
                "integer zero;\n"
                "integer divide(integer n) {\n"
                "  return n /\n"
                "      zero;\n"
                "}\n"
                "default { touch_start(integer n) { llOwnerSay((string)divide(n)); llOwnerSay(\"unreached\"); }\n"
                " timer() { llOwnerSay((string)divide(0 - 1 + 1)); } }");
            VM vm(program);
            out.clear();
            vm.set_host(make_host(out));
            vm.dispatch("touch_start", {Value::make_integer(4)});
            const bool faulted = vm.run(1000) == RunStatus::Error && vm.error() == "Math Error" && vm.error_line() == 3;
            // The frame the error left behind is gone when the next event starts.
            vm.dispatch("touch_start", {Value::make_integer(4)});
            require(faulted && vm.run(1000) == RunStatus::Error && vm.error_line() == 3 && out.empty(),
                    "a runtime error inside a function names its line and ends the event");
        }

        const std::string tail = " default { state_entry() { } }";
        require(refused("integer f() { }" + tail) && refused("integer f(integer n) { if (n) return 1; }" + tail) &&
                    refused("f() { return 1; }" + tail) && refused("integer f() { return; }" + tail) &&
                    refused("integer f() { return \"s\"; }" + tail) && refused("f() { } f() { }" + tail) &&
                    refused("llOwnerSay(string s) { }" + tail) && refused("llGetPos() { }" + tail) &&
                    refused("f(integer a, integer a) { }" + tail) &&
                    refused("f(integer a) { }" + entry + "f(); } }") &&
                    refused("f(integer a) { }" + entry + "f(\"s\"); } }") &&
                    refused("f() { }" + entry + "integer n = f(); } }") &&
                    refused(entry + "return 1; } }") && refused(entry + "g(); } }") &&
                    refused(entry + "if (1) integer n = 2; } }") &&
                    !refused("integer f(integer n) { if (n) return 1; else return 2; }\n"
                             "g() { ; if (1) ; else f(1); while (0) f(2); }" + entry + "g(); f(1); } }"),
                "a function must return what it says, and a call must match its definition");

        // Source nested or chained without end is refused with an error; it
        // must never reach the bottom of the native stack.
        {
            const auto repeat = [](const std::string& piece, int times) {
                std::string all;
                for (int i = 0; i < times; ++i) all += piece;
                return all;
            };
            const auto message = [](const std::string& body) -> std::string {
                try {
                    compile_source("default { state_entry() { " + body + " } }");
                } catch (const ScriptError& error) {
                    return error.message;
                }
                return "compiled";
            };
            require(message("integer n = " + repeat("(", 10000) + "1" + repeat(")", 10000) + ";") == "nested too deeply" &&
                        message("integer n = " + repeat("- ", 10000) + "1;") == "nested too deeply" &&
                        message("integer n = 1" + repeat(" + 1", 10000) + ";") ==
                            "expression too long or nested too deeply" &&
                        message("list l = " + repeat("(list)", 150) + repeat("[", 1) + "1" + repeat(" + 1", 150) + "];") ==
                            "expression too long or nested too deeply" &&
                        message(repeat("if (1) ", 10000) + ";") == "nested too deeply" &&
                        message(repeat("if (1) { ", 10000) + repeat("}", 10000)) == "nested too deeply" &&
                        message("integer n = " + repeat("(", 150) + "1" + repeat(" + 1", 150) + repeat(")", 150) + ";" +
                                repeat("if (n) ", 150) + ";") == "compiled",
                    "source nested or chained past the limit is refused, not followed off the stack");
        }

        // A script deep in recursion crosses a snapshot after any instruction.
        const Program crossing = compile_source(
            "string path;\n"
            "integer walk(integer n) { path = path + (string)n; if (n == 0) return 0;"
            " integer left = walk(n - 1); return left + n; }\n"
            "default { state_entry() { llOwnerSay((string)walk(4) + \" \" + path + \" \" + (string)(walk(1) - walk(2))); } }");
        VM whole(crossing);
        std::vector<std::string> whole_out;
        whole.set_host(make_host(whole_out));
        whole.dispatch("state_entry", {});
        whole.run(100000);
        // Right to left: the difference runs first, and `path` is read before
        // walk(4) adds to it.
        bool every_point = whole_out == std::vector<std::string>{"10 21010 -2"};
        for (std::uint64_t cut = 1; cut < whole.total_instructions(); ++cut) {
            VM first(crossing);
            std::vector<std::string> cut_out;
            first.set_host(make_host(cut_out));
            first.dispatch("state_entry", {});
            first.run(cut);
            VM second(crossing);
            second.set_host(make_host(cut_out));
            second.restore(first.snapshot());
            second.run(100000);
            every_point = every_point && cut_out == whole_out;
        }
        require(every_point && whole.total_instructions() > 100,
                "a script inside nested calls can be snapshotted after any instruction");
    }

    // (assignment and loops) An assignment is an expression with the variable's
    // new value; ++ and -- come before or after; for and do ... while loop.
    {
        std::vector<std::string> out;
        const auto run = [&out](const std::string& globals, const std::string& body) {
            out.clear();
            const Program program =
                compile_source(globals + "\ndefault { state_entry() {\n" + body + "\n} }");
            VM vm(program);
            vm.set_host(make_host(out));
            vm.dispatch("state_entry", {});
            const RunStatus status = vm.run(100000);
            if (status == RunStatus::Error) out.push_back("error: " + vm.error());
            if (status == RunStatus::Yielded) out.push_back("still running");
            return status;
        };
        const auto refused = [](const std::string& body) {
            try {
                compile_source("default { state_entry() { " + body + " } }");
            } catch (const ScriptError&) {
                return true;
            }
            return false;
        };

        run("integer g = 10;",
            "integer i = 10; float f = 1.5; string s = \"a\"; list l;\n"
            "i += 5; i -= 3; i *= 2; i /= 5; i %= 3; g += i; g -= 1;\n"
            "llOwnerSay((string)i + \" \" + (string)g);\n"
            "i = 5; i *= 1.5; f += 1; f *= 2; f -= 0.5; f /= 2;\n"
            "llOwnerSay((string)i + \" \" + (string)f);\n"
            "s += \"b\"; s += s; l += 1; l += [2.5, \"x\"]; l += l;\n"
            "llOwnerSay(s + \" \" + (string)l);");
        // ((10 + 5 - 3) * 2 / 5) % 3 = 1; 5 * 1.5 = 7.5 kept as 7; ((1.5 + 1) * 2 - 0.5) / 2 = 2.25
        require(out == std::vector<std::string>{"1 10", "7 2.250000", "abab 12.500000x12.500000x"},
                "compound assignment applies its operator, and integer *= float keeps the whole part");

        run("", "integer i = 5; integer j; integer k; float f = 1.5;\n"
                "j = i++; k = ++i; llOwnerSay((string)i + \" \" + (string)j + \" \" + (string)k);\n"
                "j = i--; k = --i; llOwnerSay((string)i + \" \" + (string)j + \" \" + (string)k);\n"
                "i++; ++i; i--; f++; --f; ++f;\n"
                "llOwnerSay((string)i + \" \" + (string)f + \" \" + (string)(f++) + \" \" + (string)(--f) + \" \" + (string)f);\n"
                "integer lo = -2147483647; lo--; lo--; llOwnerSay((string)lo);");
        // The third line is read from its right end: f, then --f, then f++.
        require(out == std::vector<std::string>{"7 5 7", "5 7 5", "6 2.500000 1.500000 1.500000 2.500000", "2147483647"},
                "++ and -- give the new value before the name and the old one after it");

        run("", "integer a; integer b; integer c; integer x = 1;\n"
                "a = b = c = 4; a += b -= 1;\n"
                "llOwnerSay((string)a + (string)b + (string)c + \" \" + (string)(x + (x = 5)) + \" \" + (string)((x = 2) + x)"
                " + \" \" + (string)(a = 9) + (string)a);\n"
                "if ((a = 0)) { llOwnerSay(\"zero is true\"); } string s; if ((s = \"t\") == \"t\") { llOwnerSay(s); }");
        // Read from the right end: a is still 7 when it is printed last, and 9
        // when it is printed first. (x = 2) + x reads x (1) and then sets it,
        // giving 3; then x + (x = 5) sets x before reading it, giving 10.
        require(out == std::vector<std::string>{"934 10 3 97", "t"},
                "an assignment is an expression, takes the right operand first, and chains to the right");

        run("", "integer i; string s;\n"
                "for (i = 0; i < 5; ++i) s += (string)i;\n"
                "integer j; for (i = 0, j = 10; i < j; i += 3, j--) { s += \".\"; }\n"
                "for (; i < 12; ) i++;\n"
                "llOwnerSay(s + \" \" + (string)i + \" \" + (string)j);\n"
                "s = \"\"; i = 0; do { i++; s += (string)i; } while (i < 4);\n"
                "do s += \"!\"; while (0);\n"
                "for (i = 0; i < 3; i++) { integer k; for (k = 0; k < i; k++) s += \"*\"; }\n"
                "llOwnerSay(s);");
        require(out == std::vector<std::string>{"01234... 12 7", "1234!***"},
                "for runs its first part once and its last part after each pass; do runs its body first");

        require(refused("integer i; i + 1 = 2;") && refused("integer i; 5 = i;") &&
                    refused("string s; s++;") && refused("list l; --l;") && refused("integer i; i += 1.5;") &&
                    refused("integer i; i -= \"a\";") && refused("string s; s *= 2;") && refused("++5;") &&
                    refused("integer i; for (i = 0; ; i++) { }") && refused("integer i; do i++; while (i < 3)") &&
                    refused("for (integer i = 0; i < 3; i++) { }") && refused("integer i; i =+ ;") &&
                    !refused("integer i; float f; f = i = 3; f += i; i *= f; for (;i;) i--; do ; while (i);"),
                "only a variable is assigned to, and a compound assignment still checks its types");
    }

    if (g_failures == 0) {
        std::cout << "ALL PoC CHECKS PASSED\n";
        return EXIT_SUCCESS;
    }
    std::cout << g_failures << " CHECK(S) FAILED\n";
    return EXIT_FAILURE;
}
