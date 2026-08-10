#include "homeworldz/avatar_controller.h"

#include <iostream>
#include <vector>
#include <algorithm>
#include <set>
#include <string>
#include <string_view>
#include <cmath>

int main() {
    homeworldz::viewer::AgentSetAppearance appearance;
    appearance.size = {0.45F, 0.60F, 2.0F};
    const auto fallback_geometry = homeworldz::viewer::avatar_geometry(appearance);
    if (!fallback_geometry || fallback_geometry->height != 2.0 || fallback_geometry->hip_offset != 0.0)
        return 1;
    appearance.visual_params.assign(149, 42);
    const auto calculated_geometry = homeworldz::viewer::avatar_geometry(appearance);
    if (!calculated_geometry || calculated_geometry->height <= 1.0 || calculated_geometry->height >= 3.0 ||
        calculated_geometry->hip_offset >= 0.0)
        return 2;

    homeworldz::viewer::AvatarController airborne{{202.0, 144.0, 27.873474}, 22.0, 1.77149};
    if (airborne.state().grounded || std::abs(airborne.state().position.z - 27.873474) > 1e-9)
        return 19;

    homeworldz::viewer::AvatarController avatar;
    homeworldz::viewer::AgentUpdate update;
    update.control_flags = homeworldz::viewer::control_forward;
    update.camera_center = {1.F, 2.F, 3.F};
    update.body_rotation = {0.F, 0.F, 0.5F};
    update.draw_distance = 128.F;
    avatar.apply(update);
    avatar.step(0.25);
    if (std::abs(avatar.state().position.x - 128.5) > 1e-9 ||
        std::abs(avatar.state().position.y - (128.0 + std::sqrt(3.0) / 2.0)) > 1e-9 ||
        std::abs(avatar.state().position.z - 25.78) > 1e-9 ||
        avatar.state().camera_center[1] != 2.F || avatar.state().rotation != update.body_rotation)
        return 3;
    const auto facing = avatar.look_direction();
    if (std::abs(facing[0] - 0.5F) > 1e-6F ||
        std::abs(facing[1] - static_cast<float>(std::sqrt(3.0) / 2.0)) > 1e-6F ||
        facing[2] != 0.0F)
        return 20;
    if (avatar.movement_animation() != homeworldz::viewer::MovementAnimation::walk) return 11;
    avatar.expire_transient_controls();
    avatar.step(0.1);
    if (avatar.state().velocity.x != 0.0 || avatar.state().velocity.y != 0.0) return 15;

    update.control_flags = homeworldz::viewer::control_up;
    avatar.apply(update);
    avatar.step(0.1);
    if (avatar.state().grounded || avatar.state().position.z <= 25.78) return 4;
    if (avatar.movement_animation() != homeworldz::viewer::MovementAnimation::jump) return 12;
    update.control_flags = 0;
    avatar.apply(update);
    bool saw_land_animation = false;
    for (int index = 0; index < 20; ++index) {
        avatar.step(0.1);
        saw_land_animation = saw_land_animation ||
            avatar.movement_animation() == homeworldz::viewer::MovementAnimation::land;
    }
    if (!avatar.state().grounded || std::abs(avatar.state().position.z - 25.78) > 1e-9) return 5;
    if (!saw_land_animation) return 14;

    homeworldz::viewer::AvatarController drop_avatar;
    drop_avatar.set_ground_height(20.0);
    drop_avatar.step(0.1);
    if (drop_avatar.state().grounded || drop_avatar.state().velocity.z >= 0.0 ||
        drop_avatar.movement_animation() != homeworldz::viewer::MovementAnimation::fall)
        return 16;

    avatar.set_avatar_geometry(2.0, -0.075);
    avatar.set_ground_height(26.0);
    avatar.step(0.1);
    if (!avatar.state().grounded || avatar.state().height != 2.0 || avatar.state().position.z != 27.0 ||
        std::abs(avatar.viewer_position().z - 27.075) > 1e-9)
        return 6;

    update.control_flags = homeworldz::viewer::control_fly | homeworldz::viewer::control_up |
                           homeworldz::viewer::control_fast_up;
    avatar.apply(update);
    avatar.step(0.25);
    if (!avatar.state().flying || avatar.state().velocity.z != 10.0 || avatar.state().position.z != 29.5)
        return 7;
    if (avatar.movement_animation() != homeworldz::viewer::MovementAnimation::hover_up) return 13;

    homeworldz::viewer::AvatarController launch_avatar;
    update.control_flags = homeworldz::viewer::control_fly;
    launch_avatar.apply(update);
    const auto launch_start = launch_avatar.state().position.z;
    for (int index = 0; index < 150; ++index) launch_avatar.step(0.01);
    const auto launch_rise = launch_avatar.state().position.z - launch_start;
    if (!launch_avatar.state().flying || launch_avatar.state().grounded ||
        launch_rise < 0.49 || launch_rise > 0.53 || std::abs(launch_avatar.state().velocity.z) > 0.01 ||
        launch_avatar.movement_animation() != homeworldz::viewer::MovementAnimation::hover)
        return 8;

    homeworldz::viewer::AvatarController edge_avatar{{257.0, -1.0, 25.0}, 25.0, 1.56, 0.0,
                                                       256.0, 256.0};
    if (std::abs(edge_avatar.state().position.x - 255.7) > 1e-9 ||
        std::abs(edge_avatar.state().position.y - 0.3) > 1e-9)
        return 9;
    update.control_flags = homeworldz::viewer::control_forward;
    update.body_rotation = {0.F, 0.F, 0.F};
    edge_avatar.apply(update);
    edge_avatar.step(0.25);
    if (std::abs(edge_avatar.state().position.x - 255.7) > 1e-9 ||
        edge_avatar.state().velocity.x != 0.0)
        return 10;
    homeworldz::viewer::AvatarController crossing_avatar{{255.7, 128.0, 25.0}, 25.0};
    crossing_avatar.set_border_crossing_enabled(true);
    crossing_avatar.apply(update);
    crossing_avatar.step(0.25);
    if (crossing_avatar.state().position.x <= 256.0 || crossing_avatar.state().velocity.x <= 0.0)
        return 21;
    crossing_avatar.contain_horizontal();
    if (std::abs(crossing_avatar.state().position.x - 255.7) > 1e-9 ||
        crossing_avatar.state().velocity.x != 0.0)
        return 22;
    edge_avatar.synchronize_physics({12, 13, 30}, {1, 2, 3}, false);
    if (edge_avatar.state().position.x != 12 || edge_avatar.state().position.y != 13 ||
        edge_avatar.state().position.z != 30 || edge_avatar.state().velocity.z != 3 ||
        edge_avatar.state().grounded)
        return 17;
    edge_avatar.restore_motion({4, 5, 6}, {0, 0, 0.5F}, true);
    if (!edge_avatar.state().flying || edge_avatar.state().grounded ||
        edge_avatar.state().velocity.x != 4 || edge_avatar.state().rotation[2] != 0.5F)
        return 18;
    edge_avatar.teleport({100, 110, 20}, false);
    if (edge_avatar.state().position.x != 100 || edge_avatar.state().position.y != 110 ||
        std::abs(edge_avatar.state().position.z - 25.78) > 1e-9 ||
        !edge_avatar.state().grounded || edge_avatar.state().flying ||
        edge_avatar.state().velocity.x != 0)
        return 23;
    edge_avatar.teleport({200, 210, 40}, true);
    if (edge_avatar.state().position.x != 200 || edge_avatar.state().position.y != 210 ||
        edge_avatar.state().position.z != 40 || edge_avatar.state().grounded ||
        !edge_avatar.state().flying)
        return 24;

    // Stopping flight mid-air keeps forward momentum: the fall is ballistic, not
    // straight down. Fly forward at altitude, toggle flight off, release keys.
    homeworldz::viewer::AvatarController glider;
    glider.teleport({100, 100, 60}, true);
    update.control_flags = homeworldz::viewer::control_fly | homeworldz::viewer::control_forward;
    update.body_rotation = {0.F, 0.F, 0.F}; // facing +x
    glider.apply(update);
    glider.step(0.25);
    if (!glider.state().flying || std::abs(glider.state().velocity.x - 4.0) > 1e-9) return 25;
    update.control_flags = 0; // flight and keys released together
    glider.apply(update);
    glider.step(0.25);
    if (glider.state().flying || glider.state().grounded) return 26;
    // Horizontal momentum carried through; gravity owns the vertical.
    if (std::abs(glider.state().velocity.x - 4.0) > 1e-9 || glider.state().velocity.y != 0.0 ||
        glider.state().velocity.z >= 0.0)
        return 27;
    const auto glide_x = glider.state().position.x;
    glider.step(0.25);
    if (glider.state().position.x <= glide_x) return 28;
    // Directional input still steers the fall.
    update.control_flags = homeworldz::viewer::control_back;
    glider.apply(update);
    glider.step(0.25);
    if (std::abs(glider.state().velocity.x + 4.0) > 1e-9) return 29;
    // Landing still stops the slide on key release: grounded resumes control.
    homeworldz::viewer::AvatarController lander;
    update.control_flags = homeworldz::viewer::control_fly | homeworldz::viewer::control_forward;
    lander.apply(update);
    lander.step(0.25);
    update.control_flags = 0;
    lander.apply(update);
    for (int index = 0; index < 400; ++index) lander.step(0.05);
    if (!lander.state().grounded || lander.state().velocity.x != 0.0) return 30;

    // Every state has both representations, and no two states share either. The
    // legacy id names Linden viewer content; the name is what session clients
    // are told. Asserted rather than trusted because both are switch statements
    // with a `default`, so a state added to the enum and forgotten in either one
    // compiles, runs, and silently reports the wrong thing - `stand` for a
    // walking avatar, which is exactly the failure that looks like success.
    {
        using homeworldz::viewer::MovementAnimation;
        constexpr MovementAnimation every[]{
            MovementAnimation::stand, MovementAnimation::walk, MovementAnimation::run,
            MovementAnimation::jump, MovementAnimation::fall, MovementAnimation::fly,
            MovementAnimation::hover, MovementAnimation::hover_up,
            MovementAnimation::hover_down, MovementAnimation::land};
        // Guards the list itself against the enum growing past it: land is last.
        if (static_cast<int>(MovementAnimation::land) + 1 !=
            static_cast<int>(std::size(every))) return 31;
        std::set<std::string_view> names, ids;
        for (const auto animation : every) {
            const auto name = homeworldz::viewer::movement_animation_name(animation);
            const auto id = homeworldz::viewer::movement_animation_id(animation);
            if (name.empty() || id.size() != 36) return 32;
            names.insert(name);
            ids.insert(id);
        }
        if (names.size() != std::size(every) || ids.size() != std::size(every)) return 33;

        // Every movement id is recognised as one, and something else is not.
        for (const auto animation : every)
            if (!homeworldz::viewer::is_movement_animation_id(
                    homeworldz::viewer::movement_animation_id(animation))) return 34;
        if (homeworldz::viewer::is_movement_animation_id(
                "0dd0d0d0-1111-4222-8333-444444444444")) return 35;
    }

    // Every published state actually occurs. The client core reported seeing six
    // of the ten and asked itself the right question - six observed cannot be
    // told from six *observable* without a denominator - and the half of that
    // question which is this side's is whether a name can be produced at all. A
    // state that cannot would leave every client waiting forever for something
    // impossible, and publishing it would be advertising a control that does
    // nothing, one layer down. This is also the part of a state diagram worth
    // having: derived from the controller, so it cannot drift from it.
    {
        using homeworldz::viewer::MovementAnimation;
        std::set<MovementAnimation> seen;
        const auto observe = [&](homeworldz::viewer::AvatarController& subject) {
            seen.insert(subject.movement_animation());
        };
        homeworldz::viewer::AgentUpdate input;
        input.body_rotation = {0.F, 0.F, 0.F};

        // Grounded and still, then walking, then running: the run threshold is
        // 6 m/s and the fast flag is what crosses it, so a controller that
        // ignored the flag on the ground would make `run` unreachable.
        homeworldz::viewer::AvatarController ground;
        input.control_flags = 0;
        ground.apply(input);
        ground.step(0.1);
        observe(ground);
        input.control_flags = homeworldz::viewer::control_forward;
        ground.apply(input);
        ground.step(0.1);
        observe(ground);
        input.control_flags = homeworldz::viewer::control_forward |
                              homeworldz::viewer::control_fast_forward;
        ground.apply(input);
        ground.step(0.1);
        observe(ground);

        // Up, then the descent back: jump while rising, fall while dropping, and
        // land in the moment after touchdown.
        homeworldz::viewer::AvatarController leaper;
        input.control_flags = homeworldz::viewer::control_up;
        leaper.apply(input);
        leaper.step(0.1);
        observe(leaper);
        input.control_flags = 0;
        leaper.apply(input);
        for (int index = 0; index < 20; ++index) {
            leaper.step(0.1);
            observe(leaper);
        }

        // Flight: horizontal motion is `fly` whatever the vertical, so hover and
        // its two vertical variants need the horizontal input released.
        homeworldz::viewer::AvatarController flier;
        input.control_flags = homeworldz::viewer::control_fly |
                              homeworldz::viewer::control_forward;
        flier.apply(input);
        flier.step(0.1);
        observe(flier);
        // Releasing everything but fly is not immediately `hover`: engaging
        // flight imparts a rise, so the state passes through `hoverUp` until the
        // vertical velocity decays. That is the transition the client core
        // observed in flight and could not have predicted from the vocabulary,
        // and it is why this steps until it settles rather than once.
        input.control_flags = homeworldz::viewer::control_fly;
        flier.apply(input);
        for (int index = 0; index < 40; ++index) {
            flier.step(0.1);
            observe(flier);
        }
        input.control_flags = homeworldz::viewer::control_fly | homeworldz::viewer::control_up;
        flier.apply(input);
        flier.step(0.1);
        observe(flier);
        input.control_flags = homeworldz::viewer::control_fly | homeworldz::viewer::control_down;
        flier.apply(input);
        flier.step(0.1);
        observe(flier);

        // Named individually rather than counted, so a failure says which state
        // was never produced instead of only how many were.
        constexpr MovementAnimation every[]{
            MovementAnimation::stand, MovementAnimation::walk, MovementAnimation::run,
            MovementAnimation::jump, MovementAnimation::fall, MovementAnimation::fly,
            MovementAnimation::hover, MovementAnimation::hover_up,
            MovementAnimation::hover_down, MovementAnimation::land};
        for (const auto animation : every)
            if (!seen.contains(animation)) {
                std::cerr << "state never occurred: "
                          << homeworldz::viewer::movement_animation_name(animation)
                          << std::endl;
                return 40;
            }
    }

    // The viewer's animation list holds exactly one movement animation. This is
    // the invariant that broke on 2026-08-04 and it broke in a way no server-side
    // check noticed: the operator reported an avatar flailing as if falling at
    // all times, then - after clearing animations - walking forever while stood
    // still. Both are one fault. The old code erased "the previously recorded
    // state's id" from a map that had already been assigned the new value, so it
    // removed the new id and left the old one in the list; every state an avatar
    // passed through accumulated and the viewer played them together.
    {
        using homeworldz::viewer::MovementAnimation;
        using homeworldz::viewer::apply_movement_animation;
        std::vector<homeworldz::viewer::AvatarAnimationEntry> playing;
        const homeworldz::viewer::Uuid agent{std::byte{7}};
        std::int32_t sequence = 0;
        const auto count_of = [&](MovementAnimation animation) {
            const auto id = homeworldz::viewer::parse_uuid(
                homeworldz::viewer::movement_animation_id(animation));
            return id ? std::count_if(playing.begin(), playing.end(),
                [&](const auto& entry) { return entry.animation_id == *id; }) : -1;
        };
        const auto movement_entries = [&] {
            return std::count_if(playing.begin(), playing.end(), [](const auto& entry) {
                return homeworldz::viewer::is_movement_animation_id(
                    homeworldz::viewer::format_uuid(entry.animation_id));
            });
        };

        // Walking: added, and reported as a change so viewers are told.
        if (!apply_movement_animation(playing, MovementAnimation::walk, agent, sequence)) return 41;
        if (count_of(MovementAnimation::walk) != 1 || movement_entries() != 1) return 42;
        // Called again with no change: nothing added, and no resend claimed.
        if (apply_movement_animation(playing, MovementAnimation::walk, agent, sequence)) return 43;
        if (movement_entries() != 1) return 44;

        // Stopping. The reported failure exactly: walk must be gone, not merely
        // joined by stand.
        if (!apply_movement_animation(playing, MovementAnimation::stand, agent, sequence)) return 45;
        if (count_of(MovementAnimation::walk) != 0) return 46;
        if (count_of(MovementAnimation::stand) != 1 || movement_entries() != 1) return 47;

        // The accumulation case, walked through every state in turn: never more
        // than one movement animation, whatever route was taken to get there.
        constexpr MovementAnimation route[]{
            MovementAnimation::fall, MovementAnimation::land, MovementAnimation::stand,
            MovementAnimation::walk, MovementAnimation::run, MovementAnimation::jump,
            MovementAnimation::fly, MovementAnimation::hover_up,
            MovementAnimation::hover_down, MovementAnimation::hover};
        for (const auto animation : route) {
            apply_movement_animation(playing, animation, agent, sequence);
            if (movement_entries() != 1) return 48;
            if (count_of(animation) != 1) return 49;
        }

        // A gesture is not a movement animation and must survive all of it.
        const auto gesture = homeworldz::viewer::parse_uuid("aaaa1111-2222-4333-8444-555555555555");
        if (!gesture) return 50;
        playing.push_back({*gesture, sequence++, agent});
        apply_movement_animation(playing, MovementAnimation::walk, agent, sequence);
        if (playing.size() != 2 || movement_entries() != 1) return 51;

        // Sequence numbers rise, since a viewer uses them to order updates, and
        // stay above the range the legacy path reserves for itself.
        std::int32_t fresh = 0;
        std::vector<homeworldz::viewer::AvatarAnimationEntry> ordered;
        apply_movement_animation(ordered, MovementAnimation::walk, agent, fresh);
        if (ordered.size() != 1 || ordered[0].sequence < 2) return 52;
        const auto first = ordered[0].sequence;
        apply_movement_animation(ordered, MovementAnimation::run, agent, fresh);
        if (ordered.size() != 1 || ordered[0].sequence <= first) return 53;
    }

    // The payload the session avatar announcement and the motion event share.
    // Tested here because the client core's sharpest finding lands on this side:
    // a published field whose correctness nothing can observe comes to be
    // believed rather than known. The emission still lives in main.cpp, but the
    // part with reasoning in it does not.
    {
        using homeworldz::viewer::motion_fields_json;
        using homeworldz::viewer::MovementAnimation;
        const std::string gesture = "aaaa1111-2222-4333-8444-555555555555";
        const std::string other = "bbbb1111-2222-4333-8444-555555555555";
        const std::string walk_id{
            homeworldz::viewer::movement_animation_id(MovementAnimation::walk)};
        const std::string stand_id{
            homeworldz::viewer::movement_animation_id(MovementAnimation::stand)};

        // Nothing playing: the state alone, and clips present but empty rather
        // than absent, so a client parses one shape always.
        if (motion_fields_json(MovementAnimation::stand, {}) !=
            "\"motion\":\"stand\",\"clips\":[]") return 36;

        // The movement animation is in the list, as it always is on a live
        // avatar, and must not be echoed as a clip: that is the same fact twice,
        // once portably and once as an id the client cannot use.
        const std::string walking[]{walk_id};
        if (motion_fields_json(MovementAnimation::walk, walking) !=
            "\"motion\":\"walk\",\"clips\":[]") return 37;

        // A gesture alongside it survives, and order is preserved.
        const std::string mixed[]{walk_id, gesture, other};
        if (motion_fields_json(MovementAnimation::walk, mixed) !=
            "\"motion\":\"walk\",\"clips\":[\"" + gesture + "\",\"" + other + "\"]")
            return 38;

        // A movement id left behind by a state change - stand still listed while
        // the avatar walks - is filtered too. Filtering only the current state's
        // id would leak it as a clip, and a client would draw a standing
        // animation on a walking avatar while being told it is walking.
        const std::string stale[]{stand_id, walk_id, gesture};
        if (motion_fields_json(MovementAnimation::walk, stale) !=
            "\"motion\":\"walk\",\"clips\":[\"" + gesture + "\"]") return 39;
    }

    // Replacing a standing avatar's physics capsule loses its ground contact
    // for one step, and regaining it is indistinguishable here from touching
    // down after a fall. The region rebuilds the capsule whenever an avatar's
    // shape changes, so without this a re-bake plays a landing animation on an
    // avatar that never left the ground — which is what the operator saw.
    {
        using homeworldz::viewer::MovementAnimation;
        homeworldz::viewer::AvatarController standing{{128.0, 128.0, 25.0}, 25.0, 1.78};
        standing.synchronize_physics({128.0, 128.0, 25.0}, {}, true);
        if (standing.movement_animation() == MovementAnimation::land) return 40;

        // The contact lost and recovered, with nothing said about why: a
        // landing, because from here that is exactly what it looks like.
        standing.synchronize_physics({128.0, 128.0, 25.0}, {}, false);
        standing.synchronize_physics({128.0, 128.0, 25.0}, {}, true);
        if (standing.movement_animation() != MovementAnimation::land) return 41;

        // The same sequence, declared as a capsule rebuild, is not.
        homeworldz::viewer::AvatarController reshaped{{128.0, 128.0, 25.0}, 25.0, 1.78};
        reshaped.synchronize_physics({128.0, 128.0, 25.0}, {}, true);
        reshaped.ignore_next_landing();
        reshaped.synchronize_physics({128.0, 128.0, 25.0}, {}, false);
        reshaped.synchronize_physics({128.0, 128.0, 25.0}, {}, true);
        if (reshaped.movement_animation() == MovementAnimation::land) return 42;

        // And it is spent once. A real landing after the rebuild still animates,
        // or an avatar that steps off something shortly afterwards falls silently.
        reshaped.synchronize_physics({128.0, 128.0, 25.0}, {}, false);
        reshaped.synchronize_physics({128.0, 128.0, 25.0}, {}, true);
        if (reshaped.movement_animation() != MovementAnimation::land) return 43;
    }
    return 0;
}
