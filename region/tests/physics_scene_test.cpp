#include "homeworldz/physics_scene.h"

#include <cmath>
#include <unordered_map>

namespace {

class RecordingWorld final : public homeworldz::physics::World {
public:
    homeworldz::physics::BodyId create_body(
        const homeworldz::physics::BodyDefinition& definition) override {
        last_definition = definition;
        const auto id = next_id++;
        definitions.emplace(id, definition);
        return id;
    }
    bool remove_body(homeworldz::physics::BodyId id) override {
        return definitions.erase(id) != 0;
    }
    std::optional<homeworldz::physics::BodyState> body_state(
        homeworldz::physics::BodyId) const override { return std::nullopt; }
    void set_body_state(const homeworldz::physics::BodyState&) override {}
    void apply_impulse(homeworldz::physics::BodyId, homeworldz::scene::Vector3) override {}
    homeworldz::physics::CharacterId create_character(
        const homeworldz::physics::CharacterDefinition&) override { return 1; }
    bool remove_character(homeworldz::physics::CharacterId) override { return true; }
    std::optional<homeworldz::physics::BodyState> character_state(
        homeworldz::physics::CharacterId) const override { return std::nullopt; }
    void set_character_state(homeworldz::physics::CharacterId,
                             const homeworldz::physics::BodyState&) override {}
    void set_character_velocity(homeworldz::physics::CharacterId,
                                homeworldz::scene::Vector3) override {}
    void step(double) override {}
    std::span<const homeworldz::physics::Contact> contacts() const override { return {}; }
    std::optional<homeworldz::physics::RayHit> ray_cast(
        homeworldz::scene::Vector3, homeworldz::scene::Vector3, double) const override {
        return std::nullopt;
    }
    homeworldz::physics::TransferState capture(
        std::span<const homeworldz::physics::BodyId>) const override { return {}; }
    void restore(const homeworldz::physics::TransferState&) override {}

    homeworldz::physics::BodyDefinition last_definition;
    std::unordered_map<homeworldz::physics::BodyId,
                       homeworldz::physics::BodyDefinition> definitions;
    homeworldz::physics::BodyId next_id{1};
};

bool close(double first, double second) {
    return std::abs(first - second) < 0.000001;
}

} // namespace

int main() {
    using namespace homeworldz;
    const auto glass = physics::material_properties(0x02);
    const auto wood = physics::material_properties(0x03);
    const auto rubber = physics::material_properties(0x06);
    if (!close(glass.density, 1000.0) || !close(wood.density, 1000.0) ||
        !close(glass.friction, 0.2) || !close(glass.restitution, 0.7) ||
        !close(wood.friction, 0.6) || !close(wood.restitution, 0.5) ||
        !close(rubber.friction, 0.9) || !close(rubber.restitution, 0.9))
        return 1;
    if (!close(physics::box_mass({2.0, 3.0, 4.0}, 1000.0), 24000.0) ||
        !close(physics::box_mass({0.0, 1.0, 1.0}, 1000.0), 0.001) ||
        !close(physics::box_mass({100.0, 100.0, 100.0}, 1000.0), 100000.0) ||
        !close(physics::ellipsoid_mass({2.0, 3.0, 4.0}, 1000.0), 12566.3706143592) ||
        !close(physics::cylinder_mass({2.0, 3.0, 4.0}, 1000.0), 18849.5559215388) ||
        !close(physics::prism_mass({2.0, 3.0, 4.0}, 1000.0), 12000.0) ||
        !close(physics::pyramid_mass({2.0, 3.0, 4.0}, 1000.0), 8000.0))
        return 2;
    constexpr double half_root_two = 0.70710678118654752440;
    const std::array<double, 4> quarter_turn_z{0.0, 0.0, half_root_two, half_root_two};
    const auto rotated = physics::rotate_vector({1.0, 0.0, 0.0}, quarter_turn_z);
    const auto rotated_extents =
        physics::rotated_box_half_extents({2.0, 4.0, 6.0}, quarter_turn_z);
    if (!close(rotated.x, 0.0) || !close(rotated.y, 1.0) || !close(rotated.z, 0.0) ||
        !close(rotated_extents.x, 2.0) || !close(rotated_extents.y, 1.0) ||
        !close(rotated_extents.z, 3.0))
        return 10;
    physics::BodyState escaped;
    escaped.position = {-2.0, 259.0, 20.0};
    escaped.linear_velocity = {-3.0, 4.0, -5.0};
    if (!physics::contain_body_without_neighbors(escaped) ||
        !close(escaped.position.x, 0.0) || !close(escaped.position.y, 256.0) ||
        !close(escaped.linear_velocity.x, 0.0) || !close(escaped.linear_velocity.y, 0.0) ||
        !close(escaped.linear_velocity.z, -5.0) ||
        physics::contain_body_without_neighbors(escaped))
        return 11;
    if (!physics::within_viewer_interest({0.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, 9.0, 1.0) ||
        physics::within_viewer_interest({0.0, 0.0, 0.0}, {10.01, 0.0, 0.0}, 9.0, 1.0) ||
        !physics::within_viewer_interest({0.0, 0.0, 0.0}, {1000.0, 0.0, 0.0}, 0.0))
        return 17;
    physics::BodyState previous_transform;
    physics::BodyState current_transform;
    current_transform.position.x = 0.009;
    if (physics::body_transform_changed(previous_transform, current_transform)) return 18;
    current_transform.position.x = 0.011;
    if (!physics::body_transform_changed(previous_transform, current_transform)) return 19;
    current_transform = previous_transform;
    current_transform.sleeping = true;
    if (!physics::body_transform_changed(previous_transform, current_transform)) return 20;
    previous_transform.rotation = {0.0, 0.0, 0.0, 1.0};
    current_transform = previous_transform;
    current_transform.rotation = {0.0, 0.0, 0.0, -1.0};
    if (physics::body_transform_changed(previous_transform, current_transform)) return 21;

    RecordingWorld world;
    physics::StaticSceneMirror mirror(world);
    scene::Entity entity;
    entity.id = 42;
    entity.object_id = "object";
    entity.physical = true;
    entity.material = 0x02;
    entity.scale = {2.0, 3.0, 4.0};
    entity.physics_density = 125.0;
    entity.physics_friction = 0.7;
    entity.physics_restitution = 0.25;
    entity.physics_gravity_multiplier = 1.5;
    if (!mirror.synchronize(entity) || mirror.size() != 1 ||
        world.last_definition.motion != physics::MotionType::Dynamic ||
        !close(world.last_definition.mass, 3000.0) ||
        !close(world.last_definition.friction, 0.7) ||
        !close(world.last_definition.restitution, 0.25) ||
        !close(world.last_definition.gravity_multiplier, 1.5))
        return 3;

    const auto original_body = mirror.body_id(entity.id);
    entity.material = 0x06;
    entity.physics_friction = 0.9;
    entity.physics_restitution = 0.9;
    if (!mirror.synchronize(entity) || mirror.body_id(entity.id) == original_body ||
        world.definitions.contains(original_body) || !close(world.last_definition.friction, 0.9))
        return 4;
    entity.path_curve = 0x20;
    entity.profile_curve = 0x05;
    entity.scale = {2.0, 2.0, 2.0};
    if (!mirror.synchronize(entity) || world.last_definition.shape.type != physics::ShapeType::Sphere ||
        !close(world.last_definition.shape.radius, 1.0) ||
        !close(world.last_definition.mass, 523.598775598299))
        return 6;
    entity.path_curve = 0x10;
    entity.profile_curve = 0x00;
    entity.scale = {2.0, 2.0, 3.0};
    if (!mirror.synchronize(entity) || world.last_definition.shape.type != physics::ShapeType::Cylinder ||
        !close(world.last_definition.shape.radius, 1.0) ||
        !close(world.last_definition.shape.height, 3.0) ||
        !close(world.last_definition.mass, 1178.09724509617))
        return 7;
    entity.profile_curve = 0x01;
    entity.path_scale_x = 200;
    entity.path_scale_y = 100;
    entity.path_shear_x = 0xce;
    entity.path_shear_y = 0;
    if (!mirror.synchronize(entity) ||
        world.last_definition.shape.type != physics::ShapeType::ConvexHull ||
        world.last_definition.shape.hull_points.size() != 6 ||
        !close(world.last_definition.shape.hull_points[0].x, -1.0) ||
        !close(world.last_definition.shape.hull_points[1].y, 1.0) ||
        !close(world.last_definition.shape.hull_points[2].x, 1.0) ||
        !close(world.last_definition.shape.hull_points[4].z, 1.5) ||
        !close(world.last_definition.mass, 750.0))
        return 8;
    entity.path_scale_y = 200;
    entity.path_shear_x = 0;
    if (!mirror.synchronize(entity) ||
        world.last_definition.shape.type != physics::ShapeType::ConvexHull ||
        world.last_definition.shape.hull_points.size() != 5 ||
        !close(world.last_definition.shape.hull_points[0].x, -1.0) ||
        !close(world.last_definition.shape.hull_points[3].y, 1.0) ||
        !close(world.last_definition.shape.hull_points[4].x, 0.0) ||
        !close(world.last_definition.shape.hull_points[4].z, 1.5) ||
        !close(world.last_definition.mass, 500.0) ||
        !close(physics::entity_mass(entity), world.last_definition.mass))
        return 9;
    // Torus, Tube, and Ring (path 0x20 with circle/square/triangle profiles)
    // approximate collision as a solid cylinder over the prim extents.
    entity.path_curve = 0x20;
    entity.path_scale_x = 100;
    entity.path_scale_y = 25;
    for (const std::uint8_t revolved_profile : {0x00, 0x01, 0x03}) {
        entity.profile_curve = revolved_profile;
        if (!mirror.synchronize(entity) ||
            world.last_definition.shape.type != physics::ShapeType::Cylinder ||
            !close(world.last_definition.shape.radius, 1.0) ||
            !close(world.last_definition.shape.height, 3.0) ||
            !close(world.last_definition.mass, 1178.09724509617) ||
            !close(physics::entity_mass(entity), world.last_definition.mass))
            return 22;
    }
    if (physics::classify_prim_shape(entity) != physics::PrimShape::Ring) return 23;
    entity.path_curve = 0x10;
    entity.profile_curve = 0x01;
    entity.path_scale_x = 100;
    entity.path_scale_y = 100;
    if (physics::classify_prim_shape(entity) != physics::PrimShape::Box ||
        !mirror.synchronize(entity) ||
        world.last_definition.shape.type != physics::ShapeType::Box)
        return 24;
    const auto rubber_body = mirror.body_id(entity.id);
    entity.physics_shape_type = 0x01;
    if (!mirror.synchronize(entity) || mirror.size() != 0 || world.definitions.contains(rubber_body))
        return 5;
    entity.physics_shape_type = 0x00;
    if (!mirror.synchronize(entity) || mirror.size() != 1)
        return 12;
    entity.parent_id = 7;
    if (!mirror.synchronize(entity) || mirror.size() != 0 || mirror.body_id(entity.id) != 0)
        return 13;
    if (!mirror.synchronize(entity, physics::MotionType::Static) || mirror.size() != 1 ||
        world.last_definition.motion != physics::MotionType::Static)
        return 14;
    scene::Scene linked_scene;
    const auto root_id = linked_scene.create("root");
    const auto child_id = linked_scene.create("child", {2.0, 0.0, 0.0});
    linked_scene.find(root_id)->object_id = "root";
    linked_scene.find(child_id)->object_id = "child";
    scene::establish_link(*linked_scene.find(child_id), *linked_scene.find(root_id));
    RecordingWorld linked_world;
    physics::StaticSceneMirror linked_mirror(linked_world);
    linked_mirror.synchronize(linked_scene);
    if (linked_mirror.size() != 2 || linked_mirror.body_id(child_id) == 0)
        return 15;
    linked_scene.find(root_id)->physical = true;
    linked_mirror.synchronize(linked_scene);
    if (linked_mirror.size() != 1 || linked_mirror.body_id(root_id) == 0 ||
        linked_mirror.body_id(child_id) != 0 ||
        linked_world.last_definition.shape.type != physics::ShapeType::Compound ||
        linked_world.last_definition.shape.compound_parts.size() != 2 ||
        !close(linked_world.last_definition.shape.compound_parts[1].local_position.x, 2.0) ||
        !close(linked_world.last_definition.mass, 2000.0) ||
        !close(physics::linkset_mass(linked_scene, *linked_scene.find(root_id)), 2000.0) ||
        !close(physics::linkset_bounding_radius(
            linked_scene, *linked_scene.find(root_id)), 2.8660254037844386))
        return 16;
    // The one-pass form agrees with the per-root one, for every root.
    const auto radii = physics::linkset_bounding_radii(linked_scene);
    for (const auto& [entity_id, entity] : linked_scene.entities()) {
        if (entity.parent_id != 0) {
            if (radii.contains(entity_id)) return 17;
            continue;
        }
        const auto found = radii.find(entity_id);
        if (found == radii.end() ||
            !close(found->second, physics::linkset_bounding_radius(linked_scene, entity)))
            return 17;
    }
    if (!close(radii.at(root_id), 2.8660254037844386)) return 17;
    return 0;
}
