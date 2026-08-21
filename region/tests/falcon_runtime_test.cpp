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
    return 0;
}
