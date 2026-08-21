#include "homeworldz/scene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace homeworldz::scene {

namespace {
using Quaternion = std::array<double, 4>;

Quaternion quaternion(const Vector3& rotation) {
    const auto squared = rotation.x * rotation.x + rotation.y * rotation.y + rotation.z * rotation.z;
    return {rotation.x, rotation.y, rotation.z, std::sqrt((std::max)(0.0, 1.0 - squared))};
}

Quaternion multiply(const Quaternion& left, const Quaternion& right) {
    return {
        left[3] * right[0] + left[0] * right[3] + left[1] * right[2] - left[2] * right[1],
        left[3] * right[1] - left[0] * right[2] + left[1] * right[3] + left[2] * right[0],
        left[3] * right[2] + left[0] * right[1] - left[1] * right[0] + left[2] * right[3],
        left[3] * right[3] - left[0] * right[0] - left[1] * right[1] - left[2] * right[2]};
}

Vector3 quaternion_vector(Quaternion rotation) {
    if (rotation[3] < 0.0)
        for (auto& component : rotation) component = -component;
    return {rotation[0], rotation[1], rotation[2]};
}

Vector3 rotate(Vector3 value, const Quaternion& rotation) {
    const Vector3 axis{rotation[0], rotation[1], rotation[2]};
    const Vector3 cross{
        axis.y * value.z - axis.z * value.y,
        axis.z * value.x - axis.x * value.z,
        axis.x * value.y - axis.y * value.x};
    const Vector3 second_cross{
        axis.y * cross.z - axis.z * cross.y,
        axis.z * cross.x - axis.x * cross.z,
        axis.x * cross.y - axis.y * cross.x};
    return {
        value.x + 2.0 * (rotation[3] * cross.x + second_cross.x),
        value.y + 2.0 * (rotation[3] * cross.y + second_cross.y),
        value.z + 2.0 * (rotation[3] * cross.z + second_cross.z)};
}
} // namespace

bool apply_permission_update(
    Entity& entity, std::string_view agent_id, std::uint8_t field, bool set,
    std::uint32_t mask) {
    if (agent_id != entity.owner_id || mask == 0) return false;
    const auto apply = [set, mask](std::uint32_t value) {
        return set ? value | mask : value & ~mask;
    };
    switch (field) {
    case permission_field_base:
        return false; // Reserved for a future administrator override path.
    case permission_field_owner:
        if ((mask & ~(permission_move | permission_modify)) != 0) return false;
        entity.owner_permissions = apply(entity.owner_permissions) & entity.base_permissions;
        entity.owner_permissions |= permission_move;
        entity.group_permissions &= entity.owner_permissions;
        entity.everyone_permissions &= entity.owner_permissions;
        break;
    case permission_field_group:
        if ((mask & ~(permission_modify | permission_move | permission_copy)) != 0) return false;
        entity.group_permissions = apply(entity.group_permissions) & entity.owner_permissions;
        break;
    case permission_field_everyone:
        if ((mask & ~(permission_move | permission_copy | permission_export)) != 0) return false;
        if (set && (mask & permission_export) != 0 &&
            (((entity.base_permissions & permission_export) == 0) ||
             ((entity.owner_permissions & permission_export) == 0) ||
             ((entity.next_owner_permissions & permission_all) != permission_all)))
            return false;
        entity.everyone_permissions = apply(entity.everyone_permissions) & entity.owner_permissions;
        entity.everyone_permissions &= ~permission_modify;
        break;
    case permission_field_next_owner:
        if ((mask & ~(permission_modify | permission_copy | permission_transfer)) != 0) return false;
        entity.next_owner_permissions = apply(entity.next_owner_permissions) & entity.base_permissions;
        if (entity.next_owner_permissions != 0) entity.next_owner_permissions |= permission_move;
        if ((entity.next_owner_permissions & permission_copy) == 0)
            entity.next_owner_permissions |= permission_transfer;
        if ((entity.next_owner_permissions & permission_all) != permission_all)
            entity.everyone_permissions &= ~permission_export;
        break;
    default:
        return false;
    }
    return true;
}

bool apply_task_inventory_update(
    TaskInventoryItem& item, std::string_view name, std::string_view description,
    std::uint32_t flags, std::uint32_t owner_permissions,
    std::uint32_t group_permissions, std::uint32_t everyone_permissions,
    std::uint32_t next_permissions, std::uint8_t sale_type, std::int32_t sale_price) {
    if (name.size() > 255 || description.size() > 255 || sale_type > 3 || sale_price < 0)
        return false;
    item.name = name;
    item.description = description;
    item.flags = flags;
    item.current_permissions = owner_permissions & item.base_permissions;
    item.current_permissions |= permission_move & item.base_permissions;
    item.group_permissions = group_permissions & item.current_permissions;
    item.everyone_permissions = everyone_permissions & item.current_permissions;
    item.everyone_permissions &= ~permission_modify;
    item.next_permissions = next_permissions & item.base_permissions;
    if (item.next_permissions != 0)
        item.next_permissions |= permission_move & item.base_permissions;
    if ((item.next_permissions & permission_copy) == 0)
        item.next_permissions |= permission_transfer & item.base_permissions;
    if ((item.next_permissions & permission_all) != permission_all)
        item.everyone_permissions &= ~permission_export;
    item.sale_type = sale_type;
    item.sale_price = sale_price;
    return true;
}

bool has_sit_target(const Entity& entity) {
    return entity.sit_target_position.x != 0.0 || entity.sit_target_position.y != 0.0 ||
           entity.sit_target_position.z != 0.0;
}

EffectivePermissions effective_permissions(const Entity& entity) {
    EffectivePermissions result{entity.owner_permissions, entity.next_owner_permissions};
    constexpr auto restricted = permission_modify | permission_copy | permission_transfer;
    for (const auto& item : entity.task_inventory) {
        for (const auto permission : {permission_modify, permission_copy, permission_transfer}) {
            if ((item.current_permissions & permission) == 0)
                result.owner &= ~permission;
            if ((item.current_permissions & item.next_permissions & permission) == 0)
                result.next_owner &= ~permission;
        }
    }
    if ((result.owner & restricted) != restricted)
        result.owner &= ~permission_export;
    if ((result.next_owner & restricted) != restricted)
        result.next_owner &= ~permission_export;
    return result;
}

EffectivePermissions effective_permissions(const Scene& scene, const Entity& selected) {
    const auto root_id = selected.parent_id != 0 ? selected.parent_id : selected.id;
    EffectivePermissions result{permission_creator, permission_creator};
    for (const auto& [entity_id, entity] : scene.entities()) {
        if (entity_id != root_id && entity.parent_id != root_id) continue;
        const auto part = effective_permissions(entity);
        result.owner &= part.owner;
        result.next_owner &= part.next_owner;
    }
    constexpr auto restricted = permission_modify | permission_copy | permission_transfer;
    if ((result.owner & restricted) != restricted)
        result.owner &= ~permission_export;
    if ((result.next_owner & restricted) != restricted)
        result.next_owner &= ~permission_export;
    return result;
}

std::optional<RayIntersection> intersect_box(
    Vector3 ray_start, Vector3 ray_end, Vector3 center, Vector3 scale) {
    const std::array start{ray_start.x, ray_start.y, ray_start.z};
    const std::array direction{
        ray_end.x - ray_start.x, ray_end.y - ray_start.y, ray_end.z - ray_start.z};
    const std::array minimum{
        center.x - scale.x * 0.5, center.y - scale.y * 0.5, center.z - scale.z * 0.5};
    const std::array maximum{
        center.x + scale.x * 0.5, center.y + scale.y * 0.5, center.z + scale.z * 0.5};
    double near_time = 0.0;
    double far_time = 1.0;
    std::size_t near_axis = 0;
    double near_sign = 0.0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (scale.x <= 0.0 || scale.y <= 0.0 || scale.z <= 0.0) return std::nullopt;
        if (std::abs(direction[axis]) < 1e-12) {
            if (start[axis] < minimum[axis] || start[axis] > maximum[axis]) return std::nullopt;
            continue;
        }
        double first = (minimum[axis] - start[axis]) / direction[axis];
        double second = (maximum[axis] - start[axis]) / direction[axis];
        double sign = -1.0;
        if (first > second) {
            std::swap(first, second);
            sign = 1.0;
        }
        if (first > near_time) {
            near_time = first;
            near_axis = axis;
            near_sign = sign;
        }
        far_time = std::min(far_time, second);
        if (near_time > far_time) return std::nullopt;
    }
    if (near_sign == 0.0 || near_time < 0.0 || near_time > 1.0) return std::nullopt;
    RayIntersection result;
    result.position = {ray_start.x + direction[0] * near_time,
                       ray_start.y + direction[1] * near_time,
                       ray_start.z + direction[2] * near_time};
    if (near_axis == 0) result.normal.x = near_sign;
    if (near_axis == 1) result.normal.y = near_sign;
    if (near_axis == 2) result.normal.z = near_sign;
    return result;
}

void establish_link(Entity& child, const Entity& root) {
    const auto root_rotation = quaternion(root.rotation);
    const Quaternion inverse_root{
        -root_rotation[0], -root_rotation[1], -root_rotation[2], root_rotation[3]};
    const Vector3 offset{
        child.position.x - root.position.x,
        child.position.y - root.position.y,
        child.position.z - root.position.z};
    child.parent_id = root.id;
    child.local_position = rotate(offset, inverse_root);
    child.local_rotation = quaternion_vector(multiply(inverse_root, quaternion(child.rotation)));
}

void update_linked_world_transform(Entity& child, const Entity& root) {
    const auto root_rotation = quaternion(root.rotation);
    const auto offset = rotate(child.local_position, root_rotation);
    child.position = {
        root.position.x + offset.x,
        root.position.y + offset.y,
        root.position.z + offset.z};
    child.rotation = quaternion_vector(multiply(root_rotation, quaternion(child.local_rotation)));
}

void scale_linked_child(Entity& child, Vector3 factors) {
    child.local_position.x *= factors.x;
    child.local_position.y *= factors.y;
    child.local_position.z *= factors.z;
    child.scale.x *= factors.x;
    child.scale.y *= factors.y;
    child.scale.z *= factors.z;
}

EntityId Scene::create(std::string name, Vector3 position, Vector3 velocity) {
    const auto id = next_id_++;
    entities_.emplace(id, Entity{id, std::move(name), position, velocity});
    ++revision_;
    return id;
}

bool Scene::remove(EntityId id) {
    if (entities_.erase(id) == 0) return false;
    ++revision_;
    return true;
}

Entity* Scene::find(EntityId id) {
    const auto found = entities_.find(id);
    return found == entities_.end() ? nullptr : &found->second;
}

const Entity* Scene::find(EntityId id) const {
    const auto found = entities_.find(id);
    return found == entities_.end() ? nullptr : &found->second;
}

void Scene::step(double seconds) {
    for (auto& [id, entity] : entities_) {
        static_cast<void>(id);
        entity.position.x += entity.velocity.x * seconds;
        entity.position.y += entity.velocity.y * seconds;
        entity.position.z += entity.velocity.z * seconds;
    }
    ++simulation_steps_;
    ++revision_;
}

void Scene::restore(std::uint64_t revision, std::vector<Entity> entities) {
    std::unordered_map<EntityId, Entity> restored;
    restored.reserve(entities.size());
    EntityId next_id = 1;
    for (auto& entity : entities) {
        if (entity.id == 0 || entity.id == std::numeric_limits<EntityId>::max()) {
            throw std::invalid_argument("restored entity ID is outside the supported range");
        }
        next_id = std::max(next_id, entity.id + 1);
        const auto [position, inserted] = restored.emplace(entity.id, std::move(entity));
        static_cast<void>(position);
        if (!inserted) throw std::invalid_argument("restored scene contains duplicate entity IDs");
    }
    for (const auto& [entity_id, entity] : restored) {
        if (entity.parent_id == 0) continue;
        const auto parent = restored.find(entity.parent_id);
        if (entity.parent_id == entity_id || parent == restored.end())
            throw std::invalid_argument("restored scene contains an invalid link parent");
        if (parent->second.parent_id != 0)
            throw std::invalid_argument("restored scene contains nested or cyclic links");
    }
    entities_ = std::move(restored);
    next_id_ = next_id;
    revision_ = revision;
    simulation_steps_ = 0;
}

} // namespace homeworldz::scene
