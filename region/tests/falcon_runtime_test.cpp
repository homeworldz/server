#include "homeworldz/falcon_runtime.h"

#include <cassert>
#include <string>
#include <vector>

int main() {
    using namespace std::string_literals;
    using homeworldz::script::FalconHostMessage;
    using homeworldz::script::FalconRuntime;
    using homeworldz::script::FalconSitTarget;
    using homeworldz::script::Identity;

    std::vector<FalconHostMessage> messages;
    std::vector<FalconSitTarget> seats;
    FalconRuntime runtime([&](FalconHostMessage message) {
        messages.push_back(std::move(message));
    }, [&](FalconSitTarget seat) {
        seats.push_back(std::move(seat));
    });
    const Identity identity{
        "asset", "item", "object", "owner"};
    const auto loaded = runtime.rez(identity, R"LSL(
        default { state_entry() { llSay(7, "Hello, Avatar!"); } }
    )LSL", true);
    assert(loaded.compiled && loaded.running && runtime.size() == 1);
    const auto tick = runtime.run_tick(1000, 100);
    assert(tick.scripts_visited == 1 && tick.instructions != 0 && tick.trapped == 0);
    assert(messages.size() == 1 && !messages[0].owner_only && messages[0].channel == 7 &&
           messages[0].text == "Hello, Avatar!" &&
           messages[0].identity.inventory_item_id == "item");

    messages.clear();
    const std::string firestorm_source =
        "default { state_entry() { llOwnerSay(\"saved\"); } }\0"s;
    const auto firestorm_upload = runtime.rez(
        {"firestorm-asset", "firestorm-item", "firestorm-object", "owner"},
        firestorm_source, true);
    assert(firestorm_upload.compiled && firestorm_upload.running);
    runtime.run_tick();
    assert(messages.size() == 1 && messages[0].owner_only &&
           messages[0].text == "saved");

    messages.clear();
    const auto stopped = runtime.rez(
        {"asset2", "item2", "object", "owner"},
        "default { state_entry() { llOwnerSay(\"not yet\"); } }", false);
    assert(stopped.compiled && !stopped.running && runtime.size() == 3);
    runtime.run_tick();
    assert(messages.empty());
    assert(runtime.set_enabled("object", "item2", true));
    runtime.run_tick();
    assert(messages.empty()); // enabling does not synthesize state_entry after an inactive rez

    const auto invalid = runtime.rez(
        {"bad", "bad", "object", "owner"},
        "default { state_entry() { integer value = \"bad\"; } }", true);
    assert(!invalid.compiled && !invalid.diagnostic.empty() && runtime.size() == 3);
    // The viewer's script editor reads the position from the front of the
    // error, counted from 0, and moves the cursor there on a click.
    const auto placed = runtime.rez(
        {"bad", "bad", "object", "owner"},
        "default\n{\n    state_entry()\n    {\n        llNoSuchThing();\n    }\n}", true);
    assert(!placed.compiled && runtime.size() == 3);
    assert(placed.viewer_error == "(4, 8) : ERROR : unknown function 'llNoSuchThing'");
    assert(placed.diagnostic == "unknown function 'llNoSuchThing' (line 5, column 9)");

    assert(runtime.erase("object", "item") && runtime.size() == 2);

    // touch_start dispatch: an enabled script in the touched object receives the
    // event and fires it on the next tick.
    messages.clear();
    const auto touch_rez = runtime.rez(
        {"touch-asset", "touch-item", "touch-object", "owner"},
        "default { touch_start(integer n) { llOwnerSay(\"Touched!\"); } }", true);
    assert(touch_rez.compiled && touch_rez.running);
    // The object now advertises as scripted and touch-handling so Firestorm
    // enables its Touch action.
    {
        const auto status = runtime.object_script_status("touch-object");
        assert(status.scripted && status.handles_touch);
        const auto none = runtime.object_script_status("no-such-object");
        assert(!none.scripted && !none.handles_touch);
    }
    runtime.run_tick(); // drains the ctor-dispatched state_entry (there is none here)
    assert(messages.empty());
    // A non-matching object id reaches no script; the touched object reaches one.
    assert(runtime.dispatch_touch_start("no-such-object", 1) == 0);
    assert(runtime.dispatch_touch_start("touch-object", 1) == 1);
    runtime.run_tick();
    assert(messages.size() == 1 && messages[0].owner_only &&
           messages[0].text == "Touched!" &&
           messages[0].identity.inventory_item_id == "touch-item");

    // Queued touches drain one per idle tick rather than clobbering each other.
    messages.clear();
    assert(runtime.dispatch_touch_start("touch-object", 1) == 1);
    assert(runtime.dispatch_touch_start("touch-object", 1) == 1);
    runtime.run_tick();
    assert(messages.size() == 1);
    runtime.run_tick();
    assert(messages.size() == 2);

    // A disabled script accepts no touch, and a script without a touch_start
    // handler is not counted as a recipient.
    messages.clear();
    const auto silent = runtime.rez(
        {"silent-asset", "silent-item", "silent-object", "owner"},
        "default { state_entry() { llOwnerSay(\"ready\"); } }", true);
    assert(silent.compiled && silent.running);
    runtime.run_tick();
    messages.clear();
    // A script without a touch handler is scripted but not touch-handling.
    {
        const auto status = runtime.object_script_status("silent-object");
        assert(status.scripted && !status.handles_touch);
    }
    assert(runtime.dispatch_touch_start("silent-object", 1) == 0);
    assert(runtime.set_enabled("touch-object", "touch-item", false));
    assert(runtime.dispatch_touch_start("touch-object", 1) == 0);
    runtime.run_tick();
    assert(messages.empty());
    // Disabling the only touch script clears HANDLE_TOUCH but the object stays
    // scripted.
    {
        const auto status = runtime.object_script_status("touch-object");
        assert(status.scripted && !status.handles_touch);
    }

    // llSitTarget: a vector and a rotation literal reach the region as the
    // seat the script asked for, attributed to the prim the script lives in.
    messages.clear();
    const auto seated = runtime.rez({"seat-asset", "seat-item", "seat-object", "owner"},
        R"LSL(
        default { state_entry() { llSitTarget(<0.0, 0.25, .5>, <0, 0, -0.5, 1>); } }
    )LSL", true);
    assert(seated.compiled && seated.running);
    runtime.run_tick();
    assert(seats.size() == 1 && seats[0].identity.object_id == "seat-object");
    assert(seats[0].position[0] == 0.0 && seats[0].position[1] == 0.25 &&
           seats[0].position[2] == 0.5);
    assert(seats[0].rotation[2] == -0.5 && seats[0].rotation[3] == 1.0);
    // The call is not chat, and must not arrive as any.
    assert(messages.empty());

    // The argument types are checked, not coerced: a rotation is four
    // components and a vector three, and passing one for the other is the
    // mistake most worth catching at compile time.
    const auto swapped = runtime.rez({"bad-asset", "bad-item", "bad-object", "owner"},
        R"LSL(
        default { state_entry() { llSitTarget(<0, 0, 0, 1>, <0, 0, 0>); } }
    )LSL", true);
    assert(!swapped.compiled && !swapped.diagnostic.empty());
    // A float outside a vector literal says so plainly rather than parsing as
    // something else.
    const auto stray_float = runtime.rez({"f-asset", "f-item", "f-object", "owner"},
        R"LSL(
        default { state_entry() { llSay(0, (string)1.5); } }
    )LSL", true);
    assert(!stray_float.compiled);
    // A three-component rotation or a five-component vector is a mistake, not
    // a shape to guess at.
    const auto miscounted = runtime.rez({"m-asset", "m-item", "m-object", "owner"},
        R"LSL(
        default { state_entry() { llSitTarget(<0, 0, 0, 0, 0>, <0, 0, 0, 1>); } }
    )LSL", true);
    assert(!miscounted.compiled);

    // Scheduling. Four scripts that never finish, and a tick budget that
    // reaches only one of them: every one must still get its turn. Walking the
    // scripts in a fixed order from the start gave every tick to the same one.
    {
        std::vector<FalconHostMessage> said;
        FalconRuntime fair([&](FalconHostMessage message) { said.push_back(std::move(message)); });
        const std::string looper = R"LSL(
            default { state_entry() { integer i = 0; while (i < 1) { llOwnerSay("turn"); } } }
        )LSL";
        const std::vector<std::string> items{"loop-a", "loop-b", "loop-c", "loop-d"};
        for (const auto& item : items)
            assert(fair.rez({"loop-asset", item, "loop-object-" + item, "owner"}, looper, true).running);
        // One source, one program.
        assert(fair.size() == 4 && fair.compiled_programs() == 1);
        for (int tick = 0; tick < 4; ++tick) {
            const auto ran = fair.run_tick(64, 64);
            assert(ran.scripts_visited == 1 && ran.instructions == 64);
        }
        for (const auto& item : items) {
            bool spoke = false;
            for (const auto& message : said)
                if (message.identity.inventory_item_id == item) spoke = true;
            assert(spoke);
        }
        // With budget for all four, each runs one slice a tick, no more.
        const auto all = fair.run_tick(1000, 64);
        assert(all.scripts_visited == 4 && all.instructions == 256);

        // A script stopped part-way through an event is skipped, and picks up
        // where it was when it is started again.
        assert(fair.set_enabled("loop-object-loop-a", "loop-a", false));
        assert(fair.run_tick(1000, 64).scripts_visited == 3);
        assert(fair.set_enabled("loop-object-loop-a", "loop-a", true));
        assert(fair.run_tick(1000, 64).scripts_visited == 4);

        // Removing a script that is waiting its turn leaves the rest running,
        // and the program stays while any script uses it.
        assert(fair.erase("loop-object-loop-b", "loop-b"));
        assert(fair.run_tick(1000, 64).scripts_visited == 3 && fair.compiled_programs() == 1);
        // Saving a different source into an item replaces the script there.
        assert(fair.rez({"other-asset", "loop-c", "loop-object-loop-c", "owner"},
                        "default { state_entry() { llOwnerSay(\"replaced\"); } }", true).running);
        assert(fair.size() == 3 && fair.compiled_programs() == 2);
        said.clear();
        assert(fair.run_tick(1000, 64).scripts_visited == 3);
        bool replaced = false;
        for (const auto& message : said)
            if (message.text == "replaced" && message.identity.inventory_item_id == "loop-c") replaced = true;
        assert(replaced);
        // The last scripts on the looping source go, and so does its program.
        assert(fair.erase("loop-object-loop-a", "loop-a") && fair.erase("loop-object-loop-d", "loop-d"));
        assert(fair.size() == 1 && fair.compiled_programs() == 1);
        // An idle region costs a tick nothing.
        fair.run_tick();
        assert(fair.run_tick().scripts_visited == 0);
    }

    // Sleeps and timers, on a clock the test moves.
    {
        using homeworldz::script::HostResult;
        using homeworldz::script::Value;
        double now = 100.0;
        std::vector<FalconHostMessage> said;
        FalconRuntime timed(
            [&](FalconHostMessage message) { said.push_back(std::move(message)); }, {},
            [&](const Identity&, const std::string& name, const std::vector<Value>&) -> HostResult {
                if (name == "llGetInventoryName") return HostResult::suspend(7);
                if (name == "llGetPermissions") return HostResult::fail("refused");
                return std::nullopt;
            });
        timed.set_clock([&] { return now; });
        const auto texts = [&] {
            std::vector<std::string> out;
            for (const auto& message : said) out.push_back(message.text);
            return out;
        };
        using Texts = std::vector<std::string>;

        // llSleep: the script stops for the time it asked and costs nothing
        // while it sleeps.
        assert(timed.rez({"a", "sleeper", "sleep-object", "owner"}, R"LSL(
            default { state_entry() { llOwnerSay("before"); llSleep(1.0); llOwnerSay("after"); } }
        )LSL", true).running);
        timed.run_tick();
        assert(texts() == Texts{"before"});
        assert(timed.run_tick().scripts_visited == 0);
        now = 100.9;
        assert(timed.run_tick().scripts_visited == 0);
        now = 101.0;
        timed.run_tick();
        assert((texts() == Texts{"before", "after"}));

        // llSetTimerEvent: an event each interval, and none for intervals a
        // stalled region missed.
        said.clear();
        now = 200.0;
        assert(timed.rez({"a", "ticker", "timer-object", "owner"}, R"LSL(
            default {
                state_entry() { llSetTimerEvent(0.5); }
                timer() { llOwnerSay("tick"); }
                touch_start(integer n) { llSetTimerEvent(0); }
            }
        )LSL", true).running);
        timed.run_tick();
        now = 200.4;
        timed.run_tick();
        assert(said.empty());
        now = 200.5;
        timed.run_tick();
        assert(said.size() == 1);
        now = 203.0;
        timed.run_tick();
        timed.run_tick();
        assert(said.size() == 2);
        now = 203.4;
        timed.run_tick();
        assert(said.size() == 2);
        now = 203.5;
        timed.run_tick();
        assert(said.size() == 3);
        // Zero stops it.
        assert(timed.dispatch_touch_start("timer-object", 1) == 1);
        timed.run_tick();
        now = 210.0;
        timed.run_tick();
        timed.run_tick();
        assert(said.size() == 3);

        // A handler slower than its timer: one timer event waits for it, not
        // one for every interval that passed.
        said.clear();
        now = 300.0;
        // The first run sleeps through nine more intervals; the second stops
        // the timer, so anything after it was queued while the first slept.
        assert(timed.rez({"a", "slow", "slow-object", "owner"}, R"LSL(
            integer runs;
            default {
                state_entry() { llSetTimerEvent(1.0); }
                timer() {
                    runs = runs + 1;
                    llOwnerSay("slow tick");
                    if (runs == 1) { llSleep(10.0); }
                    if (runs == 2) { llSetTimerEvent(0); }
                }
            }
        )LSL", true).running);
        timed.run_tick();
        now = 301.0;
        timed.run_tick(); // the first run, and the handler goes to sleep
        assert(said.size() == 1);
        for (now = 302.0; now < 311.0; now += 1.0) timed.run_tick();
        assert(said.size() == 1);
        now = 311.0;
        timed.run_tick(); // the handler wakes and finishes
        timed.run_tick(); // the one waiting timer event runs, and stops the timer
        assert(said.size() == 2);
        for (int tick = 0; tick < 12; ++tick) timed.run_tick();
        assert(said.size() == 2);

        // A script asleep with a timer running can be removed.
        assert(timed.rez({"a", "doomed", "doomed-object", "owner"}, R"LSL(
            default {
                state_entry() { llSetTimerEvent(1.0); llSleep(50.0); llOwnerSay("woke"); }
                timer() { llOwnerSay("doomed tick"); }
            }
        )LSL", true).running);
        timed.run_tick();
        assert(timed.erase("doomed-object", "doomed"));
        now = 400.0;
        timed.run_tick();
        timed.run_tick();
        assert(said.size() == 2);

        // A region function that cannot answer at once: the script waits, and
        // goes on when the region resumes it with the answer.
        said.clear();
        assert(timed.rez({"a", "asker", "ask-object", "owner"}, R"LSL(
            default { state_entry() { llOwnerSay(llGetInventoryName(INVENTORY_ALL, 0)); } }
        )LSL", true).running);
        timed.run_tick();
        assert(said.empty() && timed.run_tick().scripts_visited == 0);
        assert(!timed.resume("ask-object", "asker", 8, Value::make_string("wrong")));
        assert(!timed.resume("ask-object", "no-such-item", 7, Value::make_string("wrong")));
        assert(timed.resume("ask-object", "asker", 7, Value::make_string("a notecard")));
        timed.run_tick();
        assert(texts() == Texts{"a notecard"});

        // A region function that fails stops the script.
        assert(timed.rez({"a", "failer", "fail-object", "owner"}, R"LSL(
            default { state_entry() { integer p = llGetPermissions(); llOwnerSay("unreached"); } }
        )LSL", true).running);
        assert(timed.run_tick().trapped == 1);
        assert(texts() == Texts{"a notecard"});
    }
    return 0;
}
