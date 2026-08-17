#include "homeworldz/avatar_controller.h"

#include <string>

#include <algorithm>
#include <cmath>

namespace homeworldz::viewer {

std::string_view movement_animation_id(MovementAnimation animation) {
    switch (animation) {
    case MovementAnimation::walk: return "6ed24bd8-91aa-4b12-ccc7-c97c857ab4e0";
    case MovementAnimation::run: return "05ddbff8-aaa9-92a1-2b74-8fe77a29b445";
    case MovementAnimation::jump: return "2305bd75-1ca9-b03b-1faa-b176b8a8c49e";
    case MovementAnimation::fall: return "666307d9-a860-572d-6fd4-c3ab8865c094";
    case MovementAnimation::fly: return "aec4610c-757f-bc4e-c092-c6e9caf18daf";
    case MovementAnimation::hover: return "4ae8016b-31b9-03bb-c401-b1ea941db41d";
    case MovementAnimation::hover_up: return "62c5de58-cb33-5743-3d07-9e4cd4352864";
    case MovementAnimation::hover_down: return "20f063ea-8306-2562-0b07-5c853b37b31e";
    case MovementAnimation::land: return "7a17b059-12b2-41b1-570a-186368b6aa6f";
    case MovementAnimation::stand:
    default: return "2408fe9e-df1d-1d7d-f4ff-1384fa7b350f";
    }
}

std::string_view movement_animation_name(MovementAnimation animation) {
    switch (animation) {
    case MovementAnimation::walk: return "walk";
    case MovementAnimation::run: return "run";
    case MovementAnimation::jump: return "jump";
    case MovementAnimation::fall: return "fall";
    case MovementAnimation::fly: return "fly";
    case MovementAnimation::hover: return "hover";
    case MovementAnimation::hover_up: return "hoverUp";
    case MovementAnimation::hover_down: return "hoverDown";
    case MovementAnimation::land: return "land";
    case MovementAnimation::stand:
    default: return "stand";
    }
}

bool is_movement_animation_id(std::string_view animation_id) {
    constexpr MovementAnimation every[]{
        MovementAnimation::stand, MovementAnimation::walk, MovementAnimation::run,
        MovementAnimation::jump, MovementAnimation::fall, MovementAnimation::fly,
        MovementAnimation::hover, MovementAnimation::hover_up,
        MovementAnimation::hover_down, MovementAnimation::land};
    for (const auto animation : every)
        if (movement_animation_id(animation) == animation_id) return true;
    return false;
}

bool apply_movement_animation(std::vector<AvatarAnimationEntry>& playing,
                              MovementAnimation state, const Uuid& agent_id,
                              std::int32_t& next_sequence) {
    const auto wanted = parse_uuid(movement_animation_id(state));
    if (!wanted) return false;
    const auto before = playing.size();
    std::erase_if(playing, [&](const AvatarAnimationEntry& entry) {
        return entry.animation_id != *wanted &&
               is_movement_animation_id(format_uuid(entry.animation_id));
    });
    bool changed = playing.size() != before;
    const bool present = std::any_of(playing.begin(), playing.end(),
        [&](const AvatarAnimationEntry& entry) { return entry.animation_id == *wanted; });
    if (!present) {
        // Sequence numbers below 2 are reserved by the legacy path's own
        // bookkeeping, so a fresh counter starts above them.
        if (next_sequence < 2) next_sequence = 2;
        playing.push_back({*wanted, next_sequence++, agent_id});
        changed = true;
    }
    return changed;
}

std::string motion_fields_json(MovementAnimation state,
                               std::span<const std::string> playing) {
    std::string clips;
    for (const auto& animation_id : playing) {
        if (is_movement_animation_id(animation_id)) continue;
        if (!clips.empty()) clips.push_back(',');
        clips += "\"" + animation_id + "\"";
    }
    return "\"motion\":\"" + std::string(movement_animation_name(state)) +
           "\",\"clips\":[" + clips + "]";
}

std::optional<AvatarGeometry> avatar_geometry(const AgentSetAppearance& appearance) {
    // Match the Halcyon/InWorldz visual-parameter height and hip calculation.
    // The viewer-provided Size is a fallback for clients with a shorter block.
    const auto& values = appearance.visual_params;
    if (values.size() > 148) {
        const auto shoe_heel = 0.08 * values[77] / 255.0;
        const auto shoe_platform = 0.07 * values[78] / 255.0;
        const auto leg_length = 0.3836 * values[125] / 255.0;
        const auto height = 1.23077
                          + 0.516945 * values[25] / 255.0
                          + 0.072514 * values[120] / 255.0
                          + leg_length + shoe_heel + shoe_platform
                          + 0.076 * values[148] / 255.0;
        const auto hip_offset =
            (0.615385 + shoe_heel + shoe_platform + leg_length - height * 0.5) * 0.3 - 0.04;
        return AvatarGeometry{height, hip_offset};
    }
    const auto viewer_height = static_cast<double>(appearance.size[2]);
    if (std::isfinite(viewer_height) && viewer_height >= 1.0 && viewer_height <= 3.0)
        return AvatarGeometry{viewer_height, 0.0};
    return std::nullopt;
}

AvatarController::AvatarController(scene::Vector3 spawn, double ground_height, double avatar_height,
                                   double hip_offset, double region_width, double region_height)
    : state_{spawn}, ground_height_(ground_height),
      region_width_(std::max(region_width, 1.0)), region_height_(std::max(region_height, 1.0)) {
    state_.position.x = std::clamp(state_.position.x, avatar_capsule_radius,
                                   region_width_ - avatar_capsule_radius);
    state_.position.y = std::clamp(state_.position.y, avatar_capsule_radius,
                                   region_height_ - avatar_capsule_radius);
    // The default state is grounded for a default-constructed avatar, but a
    // restored spawn may be airborne. Geometry must not snap that spawn to
    // terrain before support is evaluated below.
    state_.grounded = false;
    set_avatar_geometry(avatar_height, hip_offset);
    const auto support_height = ground_height_ + state_.height * 0.5;
    if (state_.position.z <= support_height) state_.position.z = support_height;
    state_.grounded = state_.position.z <= support_height + avatar_grounded_tolerance;
}

void AvatarController::apply(const AgentUpdate& update) {
    apply(MovementInput{update.control_flags, update.body_rotation, update.camera_center,
                        update.camera_at, update.camera_left, update.camera_up,
                        update.draw_distance});
}

void AvatarController::apply(const MovementInput& update) {
    controls_ = update.control_flags;
    body_rotation_ = update.body_rotation;
    state_.rotation = update.body_rotation;
    const auto flying = (controls_ & control_fly) != 0;
    if (flying && !state_.flying && state_.grounded) {
        // Halcyon launches a grounded avatar into flight with a 2 m/s vertical
        // impulse. Exponential damping approximates the flight controller's
        // settling behavior until Jolt owns the character capsule.
        flight_lift_velocity_ = 2.0;
        state_.grounded = false;
    } else if (!flying) {
        flight_lift_velocity_ = 0.0;
    }
    state_.flying = flying;
    state_.camera_center = update.camera_center;
    state_.camera_at = update.camera_at;
    state_.camera_left = update.camera_left;
    state_.camera_up = update.camera_up;
    state_.draw_distance = update.draw_distance;
}

void AvatarController::expire_transient_controls() {
    constexpr auto transient_controls = control_forward | control_back | control_left | control_right |
                                        control_up | control_down | control_fast_forward |
                                        control_fast_left | control_fast_up;
    controls_ &= ~transient_controls;
}

void AvatarController::set_avatar_geometry(double height, double hip_offset) {
    if (!std::isfinite(height)) return;
    state_.height = std::clamp(height, 1.0, 3.0);
    if (std::isfinite(hip_offset)) state_.hip_offset = std::clamp(hip_offset, -0.5, 0.5);
    if (state_.grounded) state_.position.z = ground_height_ + state_.height * 0.5;
}

void AvatarController::set_ground_height(double height) {
    if (std::isfinite(height)) ground_height_ = height;
}

void AvatarController::contain_horizontal() {
    const auto bounded_x = std::clamp(
        state_.position.x, avatar_capsule_radius, region_width_ - avatar_capsule_radius);
    const auto bounded_y = std::clamp(
        state_.position.y, avatar_capsule_radius, region_height_ - avatar_capsule_radius);
    if (bounded_x != state_.position.x) state_.velocity.x = 0.0;
    if (bounded_y != state_.position.y) state_.velocity.y = 0.0;
    state_.position.x = bounded_x;
    state_.position.y = bounded_y;
}

void AvatarController::restore_motion(
    scene::Vector3 velocity, std::array<float, 3> rotation, bool flying) {
    if (std::isfinite(velocity.x) && std::isfinite(velocity.y) && std::isfinite(velocity.z))
        state_.velocity = velocity;
    if (std::all_of(rotation.begin(), rotation.end(), [](float value) { return std::isfinite(value); })) {
        state_.rotation = rotation;
        body_rotation_ = rotation;
    }
    state_.flying = flying;
    if (flying) state_.grounded = false;
}

void AvatarController::teleport(scene::Vector3 position, bool flying) {
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
        return;
    position.x = std::clamp(position.x, avatar_capsule_radius,
                            region_width_ - avatar_capsule_radius);
    position.y = std::clamp(position.y, avatar_capsule_radius,
                            region_height_ - avatar_capsule_radius);
    state_.position = position;
    state_.velocity = {};
    state_.flying = flying;
    state_.grounded = false;
    flight_lift_velocity_ = 0.0;
    const auto support_height = ground_height_ + state_.height * 0.5;
    if (!flying && state_.position.z <= support_height) {
        state_.position.z = support_height;
        state_.grounded = true;
    }
}

void AvatarController::synchronize_physics(
    scene::Vector3 position, scene::Vector3 velocity, bool grounded) {
    const auto was_grounded = state_.grounded;
    if (std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z))
        state_.position = position;
    if (std::isfinite(velocity.x) && std::isfinite(velocity.y) && std::isfinite(velocity.z))
        state_.velocity = velocity;
    state_.grounded = grounded;
    physics_grounding_ = true;
    if (!was_grounded && grounded) {
        // A capsule that was just replaced reports no contact until the next
        // step resolves one, and that recovery is not a landing however much it
        // looks like one from here.
        if (ignore_next_landing_) ignore_next_landing_ = false;
        else landing_animation_remaining_ = 0.4;
    }
}

scene::Vector3 AvatarController::viewer_position() const {
    // The capsule's centre, unadjusted, because that is what a viewer expects to
    // be handed.
    //
    // `LLVOAvatar` converts the position it receives into a pelvis position
    // itself: `root_pos.z -= (0.5 * mBodySize.z) - mPelvisToFoot`
    // (llvoavatar.cpp). Solving that for feet-on-ground gives
    // `sent = ground + 0.5 * mBodySize`, which is exactly `position.z` above —
    // so any further offset here is added on top of a correction the viewer has
    // already made.
    //
    // This used to subtract `hip_offset`, which comes from the Halcyon visual
    // parameter formula in avatar_geometry() above. That formula is right about
    // *height*; the hip term belongs to Halcyon's own physics convention, where
    // the body origin is not the capsule centre. Ours is centred, under Jolt, so
    // importing one half of that convention raised every avatar by the offset:
    // about 66 mm for a shape 1.78 m tall, and 64 mm for the seeded default. It
    // showed up as imported bodies standing above the ground, and as an avatar
    // measuring nearer 1.87 m than the 1.805 m its mesh actually is.
    //
    // hip_offset is still computed and logged, because it is a real quantity and
    // reports the shape's leg proportion; it simply is not this.
    return state_.position;
}

std::array<float, 3> AvatarController::look_direction() const {
    const double x = state_.rotation[0], y = state_.rotation[1], z = state_.rotation[2];
    const double w = std::sqrt(std::max(0.0, 1.0 - x * x - y * y - z * z));
    return {static_cast<float>(1.0 - 2.0 * (y * y + z * z)),
            static_cast<float>(2.0 * (x * y + w * z)), 0.0F};
}

MovementAnimation AvatarController::movement_animation() const {
    if (state_.grounded && landing_animation_remaining_ > 0.0) return MovementAnimation::land;
    const auto horizontal_speed = std::hypot(state_.velocity.x, state_.velocity.y);
    if (state_.flying) {
        if (horizontal_speed > 0.05) return MovementAnimation::fly;
        if (state_.velocity.z > 0.1) return MovementAnimation::hover_up;
        if (state_.velocity.z < -0.1) return MovementAnimation::hover_down;
        return MovementAnimation::hover;
    }
    if (!state_.grounded)
        return state_.velocity.z > 0.0 ? MovementAnimation::jump : MovementAnimation::fall;
    if (horizontal_speed > 6.0) return MovementAnimation::run;
    if (horizontal_speed > 0.05) return MovementAnimation::walk;
    return MovementAnimation::stand;
}

void AvatarController::step(double seconds) {
    seconds = std::clamp(seconds, 0.0, 0.25);
    landing_animation_remaining_ = std::max(0.0, landing_animation_remaining_ - seconds);
    const auto support_height = ground_height_ + state_.height * 0.5;
    if (!physics_grounding_ && !state_.flying && state_.grounded) {
        if (state_.position.z > support_height + 0.25)
            state_.grounded = false;
        else
            state_.position.z = support_height;
    }
    const double x = body_rotation_[0], y = body_rotation_[1], z = body_rotation_[2];
    const double w = std::sqrt(std::max(0.0, 1.0 - x * x - y * y - z * z));
    const double forward_x = 1.0 - 2.0 * (y * y + z * z);
    const double forward_y = 2.0 * (x * y + w * z);
    const double left_x = -forward_y;
    const double left_y = forward_x;
    double forward = ((controls_ & control_forward) ? 1.0 : 0.0) - ((controls_ & control_back) ? 1.0 : 0.0);
    double left = ((controls_ & control_left) ? 1.0 : 0.0) - ((controls_ & control_right) ? 1.0 : 0.0);
    const double length = std::hypot(forward, left);
    if (length > 1.0) { forward /= length; left /= length; }
    const bool fast = controls_ & (control_fast_forward | control_fast_left | control_fast_up);
    const double speed = fast ? avatar_fast_speed : avatar_walk_speed;
    // Horizontal velocity is control-driven while flying (release hovers in
    // place) or grounded (release stops), but an airborne avatar that is not
    // flying is ballistic: momentum from the moment flight ended or the jump
    // launched carries through the fall, as in Second Life, with directional
    // input still steering. Without this, releasing the keys — or toggling
    // flight off — zeroed horizontal velocity in one tick and the avatar fell
    // straight down instead of like a slow bullet.
    const bool steering = forward != 0.0 || left != 0.0;
    if (state_.flying || state_.grounded || steering) {
        state_.velocity.x = (forward_x * forward + left_x * left) * speed;
        state_.velocity.y = (forward_y * forward + left_y * left) * speed;
    }
    if (state_.flying) {
        const double vertical = ((controls_ & control_up) ? 1.0 : 0.0) - ((controls_ & control_down) ? 1.0 : 0.0);
        state_.velocity.z = vertical * speed + flight_lift_velocity_;
        flight_lift_velocity_ *= std::exp(-4.0 * seconds);
        if (flight_lift_velocity_ < 0.01) flight_lift_velocity_ = 0.0;
        state_.grounded = false;
    } else {
        if ((controls_ & control_up) && state_.grounded) {
            state_.velocity.z = avatar_jump_velocity;
            state_.grounded = false;
        }
        if (!state_.grounded) state_.velocity.z -= avatar_gravity * seconds;
    }
    state_.position.x += state_.velocity.x * seconds;
    state_.position.y += state_.velocity.y * seconds;
    state_.position.z += state_.velocity.z * seconds;
    if (!border_crossing_enabled_) contain_horizontal();
    if (!physics_grounding_ && !state_.flying && state_.position.z <= support_height) {
        if (!state_.grounded) landing_animation_remaining_ = 0.4;
        state_.position.z = support_height;
        state_.velocity.z = 0.0;
        state_.grounded = true;
    }
}

} // namespace homeworldz::viewer
