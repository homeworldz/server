#pragma once

#include "homeworldz/scene.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace homeworldz::physics {

using BodyId = std::uint64_t;
using CharacterId = std::uint64_t;

enum class MotionType { Static, Kinematic, Dynamic };
enum class ShapeType { Box, Sphere, Capsule, Cylinder, ConvexHull, Compound };

struct CompoundShapePart {
    ShapeType type{ShapeType::Box};
    scene::Vector3 half_extents{0.5, 0.5, 0.5};
    double radius{0.5};
    double height{1.0};
    std::vector<scene::Vector3> hull_points;
    scene::Vector3 local_position;
    std::array<double, 4> local_rotation{0.0, 0.0, 0.0, 1.0};
};

struct Shape {
    ShapeType type{ShapeType::Box};
    scene::Vector3 half_extents{0.5, 0.5, 0.5};
    double radius{0.5};
    double height{1.0};
    std::vector<scene::Vector3> hull_points;
    std::vector<CompoundShapePart> compound_parts;
};

struct BodyDefinition {
    scene::EntityId entity_id{};
    MotionType motion{MotionType::Static};
    Shape shape;
    scene::Vector3 position;
    scene::Vector3 velocity;
    double mass{1.0};
    double friction{0.5};
    double restitution{};
    double gravity_multiplier{1.0};
    std::array<double, 4> rotation{0.0, 0.0, 0.0, 1.0};
};

struct BodyState {
    BodyId body_id{};
    scene::EntityId entity_id{};
    scene::Vector3 position;
    scene::Vector3 linear_velocity;
    scene::Vector3 angular_velocity;
    bool sleeping{};
    bool grounded{};
    std::array<double, 4> rotation{0.0, 0.0, 0.0, 1.0};
};

// The default for the steepest ground a character stands on; beyond it the
// capsule is held by contact but never grounded, so it slides and the
// standing support rule makes no claim. Overridable per region
// (region.walkable_slope_degrees) and published in the session hello — it
// was Jolt's silent default (50°) before 2026-07-29, and a limit that gates
// behavior must be announced, never inherited (the SimulatorFeatures rule).
// 65 is Halcyon's MAX_WALKABLE_SLOPE, adopted 2026-07-29 for the InWorldz
// feel and confirmed in-world the next day: the operator, who set the
// original value, walked it and found it good. Tuning stays open, so treat
// this as a tested default rather than a settled constant — but it is a
// judgement someone made and stood behind, not a number inherited from a
// physics engine's header (which is what 50 was).
inline constexpr double character_walkable_slope_degrees = 65.0;

struct CharacterDefinition {
    scene::EntityId entity_id{};
    scene::Vector3 position;
    double radius{0.35};
    double height{1.8};
    double step_height{0.4};
    double mass{70.0};
    double maximum_horizontal_acceleration{30.0};
    double walkable_slope_degrees{character_walkable_slope_degrees};
};

struct HeightFieldDefinition {
    scene::EntityId entity_id{};
    std::vector<float> samples;
    std::uint32_t sample_count{};
    double spacing{1.0};
    // World placement of the field's southwest corner. A rectangular region
    // (ADR 0036) is covered by one square field per facet, each offset here;
    // a square region keeps the default zero origin.
    scene::Vector3 origin{};
    // Terrain contact material, defaulting to the stone preset of the legacy
    // material table. Without an explicit material the terrain body inherits the
    // engine default — restitution zero in Jolt — and the averaging restitution
    // combine then silently caps every ground bounce at half the object's own
    // setting.
    double friction{0.8};
    double restitution{0.4};
};

struct Contact {
    BodyId first{};
    BodyId second{};
    scene::Vector3 point;
    scene::Vector3 normal;
    double penetration{};
};

struct RayHit {
    BodyId body{};
    scene::Vector3 point;
    scene::Vector3 normal;
    double fraction{};
};

struct TransferState {
    std::vector<BodyState> bodies;
};

class World {
public:
    virtual ~World() = default;

    virtual BodyId create_body(const BodyDefinition& definition) = 0;
    virtual bool remove_body(BodyId id) = 0;
    virtual std::optional<BodyState> body_state(BodyId id) const = 0;
    virtual void set_body_state(const BodyState& state) = 0;
    virtual void apply_impulse(BodyId id, scene::Vector3 impulse) = 0;
    virtual BodyId create_heightfield(const HeightFieldDefinition&) { return 0; }

    virtual CharacterId create_character(const CharacterDefinition& definition) = 0;
    virtual bool remove_character(CharacterId id) = 0;
    virtual std::optional<BodyState> character_state(CharacterId id) const = 0;
    virtual void set_character_state(CharacterId id, const BodyState& state) = 0;
    virtual void set_character_velocity(CharacterId id, scene::Vector3 velocity) = 0;
    virtual void set_character_flying(CharacterId, bool) {}

    virtual void step(double seconds) = 0;
    virtual std::span<const Contact> contacts() const = 0;
    virtual std::optional<RayHit> ray_cast(scene::Vector3 origin, scene::Vector3 direction,
                                           double maximum_distance) const = 0;
    virtual std::optional<RayHit> ray_cast_body(BodyId, scene::Vector3, scene::Vector3,
                                                double) const { return std::nullopt; }

    virtual TransferState capture(std::span<const BodyId> bodies) const = 0;
    virtual void restore(const TransferState& state) = 0;
};

} // namespace homeworldz::physics
