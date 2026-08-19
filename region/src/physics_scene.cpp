#include "homeworldz/physics_scene.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace homeworldz::physics {

MaterialProperties material_properties(std::uint8_t material) {
    // SL gives all legacy materials the same default density. Material selects
    // the contact behavior; Extra Physics / scripting may override density.
    constexpr double default_density = 1000.0;
    switch (material) {
    case 0x00: return {default_density, 0.8, 0.4}; // stone
    case 0x01: return {default_density, 0.3, 0.4}; // metal
    case 0x02: return {default_density, 0.2, 0.7}; // glass
    case 0x04: return {default_density, 0.9, 0.3}; // flesh
    case 0x05: return {default_density, 0.4, 0.7}; // plastic
    case 0x06: return {default_density, 0.9, 0.9}; // rubber
    case 0x07: return {default_density, 0.6, 0.5}; // deprecated light
    case 0x03:
    default:   return {default_density, 0.6, 0.5}; // wood
    }
}

double box_mass(scene::Vector3 scale, double density) {
    // Keep malformed or extreme viewer input from destabilizing the solver.
    // These are adapter limits, not an alternate region-side force model.
    constexpr double minimum_mass = 0.001;
    constexpr double maximum_mass = 100000.0;
    const auto volume = std::max(0.0, scale.x) * std::max(0.0, scale.y) *
                        std::max(0.0, scale.z);
    return std::clamp(volume * density, minimum_mass, maximum_mass);
}

double ellipsoid_mass(scene::Vector3 scale, double density) {
    constexpr double minimum_mass = 0.001;
    constexpr double maximum_mass = 100000.0;
    constexpr double pi_over_six = 0.52359877559829887308;
    const auto volume = pi_over_six * std::max(0.0, scale.x) *
                        std::max(0.0, scale.y) * std::max(0.0, scale.z);
    return std::clamp(volume * density, minimum_mass, maximum_mass);
}

double cylinder_mass(scene::Vector3 scale, double density) {
    constexpr double minimum_mass = 0.001;
    constexpr double maximum_mass = 100000.0;
    constexpr double pi_over_four = 0.78539816339744830962;
    const auto volume = pi_over_four * std::max(0.0, scale.x) *
                        std::max(0.0, scale.y) * std::max(0.0, scale.z);
    return std::clamp(volume * density, minimum_mass, maximum_mass);
}

double prism_mass(scene::Vector3 scale, double density) {
    constexpr double minimum_mass = 0.001;
    constexpr double maximum_mass = 100000.0;
    // Firestorm's canonical prism is a square extrusion whose top X ratio is
    // collapsed to zero and sheared to one side: exactly half a box.
    constexpr double volume_fraction = 0.5;
    const auto volume = volume_fraction * std::max(0.0, scale.x) *
                        std::max(0.0, scale.y) * std::max(0.0, scale.z);
    return std::clamp(volume * density, minimum_mass, maximum_mass);
}

double pyramid_mass(scene::Vector3 scale, double density) {
    constexpr double minimum_mass = 0.001;
    constexpr double maximum_mass = 100000.0;
    constexpr double volume_fraction = 1.0 / 3.0;
    const auto volume = volume_fraction * std::max(0.0, scale.x) *
                        std::max(0.0, scale.y) * std::max(0.0, scale.z);
    return std::clamp(volume * density, minimum_mass, maximum_mass);
}

PrimShape classify_prim_shape(const scene::Entity& entity) {
    const auto profile = entity.profile_curve & 0x0f;
    if (entity.path_curve == 0x20 && profile == 0x05) return PrimShape::Sphere;
    if (entity.path_curve == 0x20 && profile == 0x00) return PrimShape::Torus;
    if (entity.path_curve == 0x20 && profile == 0x01) return PrimShape::Tube;
    if (entity.path_curve == 0x20 && profile == 0x03) return PrimShape::Ring;
    if (entity.path_curve == 0x10 && profile == 0x00) return PrimShape::Cylinder;
    if (entity.path_curve == 0x10 && profile == 0x01 &&
        entity.path_scale_x == 200 && entity.path_scale_y == 100 &&
        entity.path_shear_x == 0xce && entity.path_shear_y == 0)
        return PrimShape::Prism;
    if (entity.path_curve == 0x10 && profile == 0x01 &&
        entity.path_scale_x == 200 && entity.path_scale_y == 200 &&
        entity.path_shear_x == 0 && entity.path_shear_y == 0)
        return PrimShape::Pyramid;
    return PrimShape::Box;
}

Shape prim_collision_shape(const scene::Entity& entity) {
    Shape shape;
    shape.half_extents = {
        entity.scale.x * 0.5, entity.scale.y * 0.5, entity.scale.z * 0.5};
    const auto x = entity.scale.x * 0.5;
    const auto y = entity.scale.y * 0.5;
    const auto z = entity.scale.z * 0.5;
    switch (classify_prim_shape(entity)) {
    case PrimShape::Sphere:
        shape.type = ShapeType::Sphere;
        shape.radius = std::min({entity.scale.x, entity.scale.y, entity.scale.z}) * 0.5;
        break;
    case PrimShape::Cylinder:
    case PrimShape::Torus:
    case PrimShape::Tube:
    case PrimShape::Ring:
        shape.type = ShapeType::Cylinder;
        shape.radius = std::min(entity.scale.x, entity.scale.y) * 0.5;
        shape.height = entity.scale.z;
        break;
    case PrimShape::Prism:
        shape.type = ShapeType::ConvexHull;
        shape.hull_points = {
            {-x, -y, -z}, {-x, y, -z}, {x, -y, -z}, {x, y, -z},
            {-x, -y, z}, {-x, y, z}};
        break;
    case PrimShape::Pyramid:
        shape.type = ShapeType::ConvexHull;
        shape.hull_points = {
            {-x, -y, -z}, {-x, y, -z}, {x, -y, -z}, {x, y, -z}, {0.0, 0.0, z}};
        break;
    case PrimShape::Box:
        shape.type = ShapeType::Box;
        break;
    }
    return shape;
}

double entity_mass(const scene::Entity& entity) {
    const auto density = std::isfinite(entity.physics_density)
        ? std::clamp(entity.physics_density, 1.0, 22587.0)
        : material_properties(entity.material).density;
    switch (classify_prim_shape(entity)) {
    case PrimShape::Sphere: return ellipsoid_mass(entity.scale, density);
    // Torus/Tube/Ring mass matches their cylinder collision approximation.
    case PrimShape::Cylinder:
    case PrimShape::Torus:
    case PrimShape::Tube:
    case PrimShape::Ring: return cylinder_mass(entity.scale, density);
    case PrimShape::Prism: return prism_mass(entity.scale, density);
    case PrimShape::Pyramid: return pyramid_mass(entity.scale, density);
    case PrimShape::Box: break;
    }
    return box_mass(entity.scale, density);
}

double linkset_mass(const scene::Scene& scene, const scene::Entity& root) {
    const auto collidable = [](const scene::Entity& entity) {
        return !entity.object_id.empty() && !entity.phantom && entity.physics_shape_type != 0x01;
    };
    double result = collidable(root) ? entity_mass(root) : 0.0;
    for (const auto& [entity_id, child] : scene.entities()) {
        static_cast<void>(entity_id);
        if (child.parent_id == root.id && collidable(child)) result += entity_mass(child);
    }
    return std::max(0.001, result);
}

double linkset_bounding_radius(const scene::Scene& scene, const scene::Entity& root) {
    const auto part_radius = [](const scene::Entity& entity) {
        return 0.5 * std::sqrt(entity.scale.x * entity.scale.x +
                               entity.scale.y * entity.scale.y +
                               entity.scale.z * entity.scale.z);
    };
    double result = part_radius(root);
    for (const auto& [entity_id, child] : scene.entities()) {
        static_cast<void>(entity_id);
        if (child.parent_id != root.id) continue;
        const auto offset = std::sqrt(
            child.local_position.x * child.local_position.x +
            child.local_position.y * child.local_position.y +
            child.local_position.z * child.local_position.z);
        result = std::max(result, offset + part_radius(child));
    }
    return result;
}

scene::Vector3 rotate_vector(scene::Vector3 value, const std::array<double, 4>& rotation) {
    const scene::Vector3 quaternion_vector{rotation[0], rotation[1], rotation[2]};
    const auto cross = [](scene::Vector3 first, scene::Vector3 second) {
        return scene::Vector3{
            first.y * second.z - first.z * second.y,
            first.z * second.x - first.x * second.z,
            first.x * second.y - first.y * second.x};
    };
    const auto first_cross = cross(quaternion_vector, value);
    const scene::Vector3 doubled{
        2.0 * first_cross.x, 2.0 * first_cross.y, 2.0 * first_cross.z};
    const auto second_cross = cross(quaternion_vector, doubled);
    return {
        value.x + rotation[3] * doubled.x + second_cross.x,
        value.y + rotation[3] * doubled.y + second_cross.y,
        value.z + rotation[3] * doubled.z + second_cross.z};
}

scene::Vector3 rotated_box_half_extents(
    scene::Vector3 scale, const std::array<double, 4>& rotation) {
    const scene::Vector3 x = rotate_vector({scale.x * 0.5, 0.0, 0.0}, rotation);
    const scene::Vector3 y = rotate_vector({0.0, scale.y * 0.5, 0.0}, rotation);
    const scene::Vector3 z = rotate_vector({0.0, 0.0, scale.z * 0.5}, rotation);
    return {std::abs(x.x) + std::abs(y.x) + std::abs(z.x),
            std::abs(x.y) + std::abs(y.y) + std::abs(z.y),
            std::abs(x.z) + std::abs(y.z) + std::abs(z.z)};
}

bool contain_body_without_neighbors(BodyState& state, double region_extent_x,
                                    double region_extent_y) {
    // A zero Y extent means square, which is what every caller meant before
    // regions could be rectangular (ADR 0036).
    if (region_extent_y <= 0.0) region_extent_y = region_extent_x;
    bool constrained{};
    if (state.position.x < 0.0) {
        state.position.x = 0.0;
        state.linear_velocity.x = std::max(0.0, state.linear_velocity.x);
        constrained = true;
    } else if (state.position.x > region_extent_x) {
        state.position.x = region_extent_x;
        state.linear_velocity.x = std::min(0.0, state.linear_velocity.x);
        constrained = true;
    }
    if (state.position.y < 0.0) {
        state.position.y = 0.0;
        state.linear_velocity.y = std::max(0.0, state.linear_velocity.y);
        constrained = true;
    } else if (state.position.y > region_extent_y) {
        state.position.y = region_extent_y;
        state.linear_velocity.y = std::min(0.0, state.linear_velocity.y);
        constrained = true;
    }
    return constrained;
}

bool within_viewer_interest(const scene::Vector3 observer, const scene::Vector3 subject,
                            const double draw_distance, const double subject_radius) {
    if (draw_distance <= 0.0) return true;
    const auto dx = subject.x - observer.x;
    const auto dy = subject.y - observer.y;
    const auto dz = subject.z - observer.z;
    const auto range = draw_distance + (std::max)(0.0, subject_radius);
    return dx * dx + dy * dy + dz * dz <= range * range;
}

bool body_transform_changed(const BodyState& previous, const BodyState& current,
                            const double position_epsilon, const double velocity_epsilon,
                            const double rotation_epsilon) {
    const auto vector_changed = [](const scene::Vector3& first, const scene::Vector3& second,
                                   const double epsilon) {
        const auto dx = first.x - second.x;
        const auto dy = first.y - second.y;
        const auto dz = first.z - second.z;
        return dx * dx + dy * dy + dz * dz > epsilon * epsilon;
    };
    if (vector_changed(previous.position, current.position, position_epsilon) ||
        vector_changed(previous.linear_velocity, current.linear_velocity, velocity_epsilon) ||
        previous.sleeping != current.sleeping)
        return true;
    double rotation_delta{};
    double negated_rotation_delta{};
    for (std::size_t index = 0; index < current.rotation.size(); ++index) {
        const auto delta = previous.rotation[index] - current.rotation[index];
        rotation_delta += delta * delta;
        const auto negated_delta = previous.rotation[index] + current.rotation[index];
        negated_rotation_delta += negated_delta * negated_delta;
    }
    return (std::min)(rotation_delta, negated_rotation_delta) >
        rotation_epsilon * rotation_epsilon;
}

bool StaticSceneMirror::synchronize(
    const scene::Entity& entity, std::optional<MotionType> linked_motion) {
    if (entity.object_id.empty() || (entity.parent_id != 0 && !linked_motion) || entity.phantom ||
        entity.physics_shape_type == 0x01) {
        static_cast<void>(remove(entity.id));
        return true;
    }
    BodyDefinition definition;
    definition.entity_id = entity.id;
    definition.motion = linked_motion.value_or(
        entity.physical ? MotionType::Dynamic : MotionType::Static);
    definition.shape = prim_collision_shape(entity);
    definition.position = entity.position;
    definition.velocity = entity.velocity;
    const auto properties = material_properties(entity.material);
    definition.mass = entity_mass(entity);
    definition.friction = std::isfinite(entity.physics_friction)
        ? std::clamp(entity.physics_friction, 0.0, 255.0)
        : properties.friction;
    definition.restitution = std::isfinite(entity.physics_restitution)
        ? std::clamp(entity.physics_restitution, 0.0, 1.0)
        : properties.restitution;
    definition.gravity_multiplier = std::isfinite(entity.physics_gravity_multiplier)
        ? std::clamp(entity.physics_gravity_multiplier, -1.0, 28.0)
        : 1.0;
    const auto squared = entity.rotation.x * entity.rotation.x +
                         entity.rotation.y * entity.rotation.y +
                         entity.rotation.z * entity.rotation.z;
    definition.rotation = {entity.rotation.x, entity.rotation.y, entity.rotation.z,
                           std::sqrt(std::max(0.0, 1.0 - squared))};
    const auto replacement = world_.create_body(definition);
    if (replacement == 0) return false;
    if (const auto found = bodies_.find(entity.id); found != bodies_.end())
        world_.remove_body(found->second);
    bodies_[entity.id] = replacement;
    return true;
}

bool StaticSceneMirror::synchronize_linkset(
    const scene::Scene& scene, scene::EntityId entity_id, bool suspend_dynamic) {
    const auto* selected = scene.find(entity_id);
    if (!selected) return false;
    const auto root_id = selected->parent_id == 0 ? selected->id : selected->parent_id;
    const auto* root = scene.find(root_id);
    if (!root || root->parent_id != 0) return false;

    std::vector<const scene::Entity*> children;
    for (const auto& [candidate_id, candidate] : scene.entities()) {
        static_cast<void>(candidate_id);
        if (candidate.parent_id == root_id) children.push_back(&candidate);
    }
    if (root->phantom) {
        static_cast<void>(remove(root_id));
        for (const auto* child : children) static_cast<void>(remove(child->id));
        return true;
    }
    if (!root->physical || children.empty()) {
        auto synchronized_root = *root;
        if (suspend_dynamic) synchronized_root.physical = false;
        if (!synchronize(synchronized_root)) return false;
        for (const auto* child : children) {
            if (!synchronize(*child, MotionType::Static)) return false;
        }
        return true;
    }

    const auto quaternion = [](scene::Vector3 rotation) {
        const auto squared = rotation.x * rotation.x + rotation.y * rotation.y +
                             rotation.z * rotation.z;
        return std::array<double, 4>{rotation.x, rotation.y, rotation.z,
            std::sqrt(std::max(0.0, 1.0 - squared))};
    };
    const auto part_for = [&](const scene::Entity& entity, scene::Vector3 position,
                              std::array<double, 4> rotation)
        -> std::optional<CompoundShapePart> {
        if (entity.phantom || entity.physics_shape_type == 0x01 || entity.object_id.empty())
            return std::nullopt;
        CompoundShapePart part;
        const auto shape = prim_collision_shape(entity);
        part.type = shape.type;
        part.half_extents = shape.half_extents;
        part.radius = shape.radius;
        part.height = shape.height;
        part.hull_points = shape.hull_points;
        part.local_position = position;
        part.local_rotation = rotation;
        return part;
    };

    BodyDefinition definition;
    definition.entity_id = root_id;
    definition.motion = suspend_dynamic ? MotionType::Static : MotionType::Dynamic;
    definition.shape.type = ShapeType::Compound;
    double total_mass{};
    double weighted_friction{};
    double weighted_restitution{};
    double weighted_gravity{};
    const auto add_part = [&](const scene::Entity& entity, scene::Vector3 position,
                              std::array<double, 4> rotation) {
        const auto part = part_for(entity, position, rotation);
        if (!part) return;
        definition.shape.compound_parts.push_back(*part);
        const auto mass = entity_mass(entity);
        const auto properties = material_properties(entity.material);
        const auto friction = std::isfinite(entity.physics_friction)
            ? std::clamp(entity.physics_friction, 0.0, 255.0) : properties.friction;
        const auto restitution = std::isfinite(entity.physics_restitution)
            ? std::clamp(entity.physics_restitution, 0.0, 1.0) : properties.restitution;
        const auto gravity = std::isfinite(entity.physics_gravity_multiplier)
            ? std::clamp(entity.physics_gravity_multiplier, -1.0, 28.0) : 1.0;
        total_mass += mass;
        weighted_friction += mass * friction;
        weighted_restitution += mass * restitution;
        weighted_gravity += mass * gravity;
    };
    add_part(*root, {}, {0.0, 0.0, 0.0, 1.0});
    for (const auto* child : children)
        add_part(*child, child->local_position, quaternion(child->local_rotation));
    if (definition.shape.compound_parts.empty()) {
        static_cast<void>(remove(root_id));
        for (const auto* child : children) static_cast<void>(remove(child->id));
        return true;
    }
    definition.position = root->position;
    definition.velocity = root->velocity;
    definition.mass = total_mass;
    definition.friction = weighted_friction / total_mass;
    definition.restitution = weighted_restitution / total_mass;
    definition.gravity_multiplier = weighted_gravity / total_mass;
    definition.rotation = quaternion(root->rotation);
    const auto replacement = world_.create_body(definition);
    if (replacement == 0) return false;
    if (const auto found = bodies_.find(root_id); found != bodies_.end())
        world_.remove_body(found->second);
    bodies_[root_id] = replacement;
    for (const auto* child : children) static_cast<void>(remove(child->id));
    return true;
}

void StaticSceneMirror::synchronize(const scene::Scene& scene) {
    std::unordered_set<scene::EntityId> present;
    for (const auto& [entity_id, entity] : scene.entities()) {
        if (entity.parent_id != 0) continue;
        if (!synchronize_linkset(scene, entity_id)) continue;
        const bool collidable_child = std::any_of(
            scene.entities().begin(), scene.entities().end(), [&](const auto& entry) {
                const auto& child = entry.second;
                return child.parent_id == entity_id && !child.object_id.empty() &&
                    !child.phantom && child.physics_shape_type != 0x01;
            });
        if (!entity.object_id.empty() && !entity.phantom &&
            (entity.physics_shape_type != 0x01 || (entity.physical && collidable_child)))
            present.insert(entity_id);
        if (!entity.physical) {
            for (const auto& [child_id, child] : scene.entities())
                if (child.parent_id == entity_id && !child.object_id.empty() && !child.phantom &&
                    child.physics_shape_type != 0x01)
                    present.insert(child_id);
        }
    }
    for (auto iterator = bodies_.begin(); iterator != bodies_.end();) {
        if (present.contains(iterator->first)) {
            ++iterator;
            continue;
        }
        world_.remove_body(iterator->second);
        iterator = bodies_.erase(iterator);
    }
}

bool StaticSceneMirror::remove(scene::EntityId entity_id) {
    const auto found = bodies_.find(entity_id);
    if (found == bodies_.end()) return false;
    const auto removed = world_.remove_body(found->second);
    bodies_.erase(found);
    return removed;
}

BodyId StaticSceneMirror::body_id(scene::EntityId entity_id) const {
    const auto found = bodies_.find(entity_id);
    return found == bodies_.end() ? 0 : found->second;
}

} // namespace homeworldz::physics
