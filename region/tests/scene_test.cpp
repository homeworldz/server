#include "homeworldz/scene.h"
#include "homeworldz/simulation_loop.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

bool near(double value, double expected) {
    if (std::abs(value - expected) < 1e-9) return true;
    std::cerr << "value " << value << ", want " << expected << '\n';
    return false;
}

} // namespace

int main() {
    const auto top = homeworldz::scene::intersect_box(
        {0.0, 0.0, 5.0}, {0.0, 0.0, -5.0}, {0.0, 0.0, 1.0}, {2.0, 2.0, 2.0});
    const auto side = homeworldz::scene::intersect_box(
        {5.0, 0.0, 1.0}, {-5.0, 0.0, 1.0}, {0.0, 0.0, 1.0}, {2.0, 2.0, 2.0});
    if (!top || !near(top->position.z, 2.0) || top->normal.z != 1.0 ||
        !side || !near(side->position.x, 1.0) || side->normal.x != 1.0 ||
        homeworldz::scene::intersect_box(
            {5.0, 5.0, 5.0}, {4.0, 4.0, 4.0}, {0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}))
        return 1;
    homeworldz::scene::Entity root;
    root.id = 10;
    root.position = {10.0, 20.0, 30.0};
    homeworldz::scene::Entity child;
    child.id = 11;
    child.position = {12.0, 20.0, 31.0};
    homeworldz::scene::establish_link(child, root);
    if (child.parent_id != root.id || !near(child.local_position.x, 2.0) ||
        !near(child.local_position.y, 0.0) || !near(child.local_position.z, 1.0))
        return 1;
    constexpr double half_root_two = 0.70710678118654752440;
    root.position = {20.0, 30.0, 40.0};
    root.rotation.z = half_root_two;
    homeworldz::scene::update_linked_world_transform(child, root);
    if (!near(child.position.x, 20.0) || !near(child.position.y, 32.0) ||
        !near(child.position.z, 41.0) || !near(child.rotation.z, half_root_two))
        return 1;
    child.local_position = {0.0, -3.0, 2.0};
    homeworldz::scene::update_linked_world_transform(child, root);
    if (!near(child.position.x, 23.0) || !near(child.position.y, 30.0) ||
        !near(child.position.z, 42.0))
        return 1;
    child.scale = {1.0, 2.0, 3.0};
    homeworldz::scene::scale_linked_child(child, {2.0, 0.5, 3.0});
    if (!near(child.local_position.x, 0.0) || !near(child.local_position.y, -1.5) ||
        !near(child.local_position.z, 6.0) || !near(child.scale.x, 2.0) ||
        !near(child.scale.y, 1.0) || !near(child.scale.z, 9.0))
        return 1;
    homeworldz::scene::Scene scene;
    const auto id = scene.create("moving object", {1.0, 2.0, 3.0}, {4.0, -2.0, 1.0});
    if (scene.size() != 1 || scene.revision() != 1) return 1;

    homeworldz::simulation::FixedStepLoop loop(scene, 0.02, 4);
    if (loop.advance(0.01) != 0 || !near(loop.interpolation_alpha(), 0.5)) return 1;
    if (loop.advance(0.03) != 2) return 1;
    const auto* entity = scene.find(id);
    if (entity == nullptr || !near(entity->position.x, 1.16) ||
        !near(entity->position.y, 1.92) || !near(entity->position.z, 3.04)) return 1;
    if (scene.simulation_steps() != 2 || scene.revision() != 3) return 1;

    if (loop.advance(10.0) != 4) return 1;
    if (scene.simulation_steps() != 6 || loop.interpolation_alpha() >= 1.0) return 1;
    if (!scene.remove(id) || scene.remove(id) || scene.size() != 0) return 1;

    scene.restore(42, std::vector<homeworldz::scene::Entity>{
        {7, "restored", {9.0, 8.0, 7.0}, {1.0, 2.0, 3.0}}});
    const auto* restored = scene.find(7);
    if (scene.size() != 1 || scene.revision() != 42 || scene.simulation_steps() != 0 ||
        restored == nullptr || restored->name != "restored" || !near(restored->position.y, 8.0)) return 1;
    if (scene.create("after restore") != 8) return 1;

    const auto rejects_restore = [](std::vector<homeworldz::scene::Entity> entities) {
        homeworldz::scene::Scene invalid;
        try {
            invalid.restore(1, std::move(entities));
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    homeworldz::scene::Entity orphan;
    orphan.id = 1;
    orphan.parent_id = 99;
    if (!rejects_restore({orphan})) return 1;
    homeworldz::scene::Entity nested_root;
    nested_root.id = 1;
    homeworldz::scene::Entity nested_child;
    nested_child.id = 2;
    nested_child.parent_id = 1;
    homeworldz::scene::Entity nested_grandchild;
    nested_grandchild.id = 3;
    nested_grandchild.parent_id = 2;
    if (!rejects_restore({nested_root, nested_child, nested_grandchild})) return 1;

    homeworldz::scene::Entity permissions;
    permissions.owner_id = "owner";
    if (!homeworldz::scene::apply_permission_update(
            permissions, "owner", homeworldz::scene::permission_field_everyone, true,
            homeworldz::scene::permission_move) ||
        permissions.everyone_permissions != homeworldz::scene::permission_move)
        return 1;
    if (homeworldz::scene::apply_permission_update(
            permissions, "intruder", homeworldz::scene::permission_field_everyone, true,
            homeworldz::scene::permission_copy) ||
        homeworldz::scene::apply_permission_update(
            permissions, "owner", homeworldz::scene::permission_field_base, false,
            homeworldz::scene::permission_modify))
        return 1;
    if (!homeworldz::scene::apply_permission_update(
            permissions, "owner", homeworldz::scene::permission_field_next_owner, false,
            homeworldz::scene::permission_copy) ||
        (permissions.next_owner_permissions & homeworldz::scene::permission_copy) != 0 ||
        (permissions.next_owner_permissions & homeworldz::scene::permission_transfer) == 0 ||
        (permissions.next_owner_permissions & homeworldz::scene::permission_move) == 0)
        return 1;
    if (!homeworldz::scene::apply_permission_update(
            permissions, "owner", homeworldz::scene::permission_field_owner, false,
            homeworldz::scene::permission_modify | homeworldz::scene::permission_move) ||
        (permissions.owner_permissions & homeworldz::scene::permission_move) == 0 ||
        (permissions.owner_permissions & homeworldz::scene::permission_modify) != 0)
        return 1;
    homeworldz::scene::TaskInventoryItem task_item;
    task_item.name = "Old";
    task_item.base_permissions = homeworldz::scene::permission_creator;
    if (!homeworldz::scene::apply_task_inventory_update(
            task_item, "Renamed", "Description", 0x01020304,
            homeworldz::scene::permission_all, homeworldz::scene::permission_copy,
            homeworldz::scene::permission_modify | homeworldz::scene::permission_copy |
                homeworldz::scene::permission_export,
            homeworldz::scene::permission_modify, 1, 25) ||
        task_item.name != "Renamed" || task_item.description != "Description" ||
        task_item.flags != 0x01020304 ||
        (task_item.current_permissions & homeworldz::scene::permission_move) == 0 ||
        task_item.group_permissions != homeworldz::scene::permission_copy ||
        (task_item.everyone_permissions & homeworldz::scene::permission_modify) != 0 ||
        (task_item.everyone_permissions & homeworldz::scene::permission_export) != 0 ||
        (task_item.next_permissions & homeworldz::scene::permission_copy) != 0 ||
        (task_item.next_permissions & homeworldz::scene::permission_transfer) == 0 ||
        task_item.sale_type != 1 || task_item.sale_price != 25)
        return 1;
    const auto unchanged = task_item;
    if (homeworldz::scene::apply_task_inventory_update(
            task_item, "Invalid", "", 0, 0, 0, 0, 0, 4, -1) ||
        task_item.name != unchanged.name || task_item.current_permissions != unchanged.current_permissions)
        return 1;

    homeworldz::scene::Entity innermost;
    innermost.owner_permissions = homeworldz::scene::permission_creator;
    innermost.next_owner_permissions = homeworldz::scene::permission_creator;
    homeworldz::scene::TaskInventoryItem restricted_item;
    restricted_item.current_permissions = homeworldz::scene::permission_creator &
        ~homeworldz::scene::permission_copy;
    restricted_item.next_permissions = homeworldz::scene::permission_creator;
    innermost.task_inventory.push_back(restricted_item);
    const auto inner_effective = homeworldz::scene::effective_permissions(innermost);
    if ((inner_effective.owner & homeworldz::scene::permission_copy) != 0 ||
        (inner_effective.owner & homeworldz::scene::permission_export) != 0 ||
        (inner_effective.next_owner & homeworldz::scene::permission_copy) != 0)
        return 1;

    const auto nested_item = [&](const homeworldz::scene::EffectivePermissions& folded) {
        homeworldz::scene::TaskInventoryItem item;
        item.current_permissions = folded.owner;
        item.next_permissions = folded.next_owner;
        return item;
    };
    homeworldz::scene::Entity middle;
    middle.owner_permissions = homeworldz::scene::permission_creator;
    middle.next_owner_permissions = homeworldz::scene::permission_creator;
    middle.task_inventory.push_back(nested_item(inner_effective));
    const auto middle_effective = homeworldz::scene::effective_permissions(middle);
    homeworldz::scene::Entity outer;
    outer.owner_permissions = homeworldz::scene::permission_creator;
    outer.next_owner_permissions = homeworldz::scene::permission_creator;
    outer.task_inventory.push_back(nested_item(middle_effective));
    const auto outer_effective = homeworldz::scene::effective_permissions(outer);
    if ((outer_effective.owner & homeworldz::scene::permission_copy) != 0 ||
        (outer_effective.next_owner & homeworldz::scene::permission_copy) != 0 ||
        (outer_effective.owner & homeworldz::scene::permission_modify) == 0 ||
        (outer_effective.owner & homeworldz::scene::permission_transfer) == 0)
        return 1;

    homeworldz::scene::Entity permission_root;
    permission_root.id = 20;
    permission_root.owner_permissions = homeworldz::scene::permission_creator;
    permission_root.next_owner_permissions = homeworldz::scene::permission_creator;
    homeworldz::scene::Entity permission_child;
    permission_child.id = 21;
    permission_child.parent_id = permission_root.id;
    permission_child.owner_permissions = homeworldz::scene::permission_creator;
    permission_child.next_owner_permissions = homeworldz::scene::permission_creator;
    permission_child.task_inventory.push_back(nested_item(inner_effective));
    homeworldz::scene::Scene permission_scene;
    permission_scene.restore(1, {permission_root, permission_child});
    const auto* restored_permission_root = permission_scene.find(permission_root.id);
    if (!restored_permission_root) return 1;
    const auto linkset_effective = homeworldz::scene::effective_permissions(
        permission_scene, *restored_permission_root);
    if ((linkset_effective.owner & homeworldz::scene::permission_copy) != 0 ||
        (linkset_effective.next_owner & homeworldz::scene::permission_copy) != 0)
        return 1;

    // A region's ids start from its UUID: first and last four hex digits,
    // folded into the middle half of the 32-bit range.
    using homeworldz::scene::local_id_seed;
    if (local_id_seed("60ed06a2-dbd0-40f8-bd3f-e23786752f81") != 0xA0ED2F81ULL) return 2;
    if (local_id_seed("FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF") != 0xBFFFFFFFULL) return 3;
    if (local_id_seed("00000000-0000-0000-0000-000000000000") != 0x40000000ULL) return 4;
    if (local_id_seed("not-a-uuid") != 0 ||
        local_id_seed("60ed06a2-dbd0-40f8-bd3f-e23786752f8g") != 0)
        return 5;
    // Two regions differing only in the middle of their UUIDs share a seed;
    // differing at either end, they do not.
    if (local_id_seed("4aa21394-7aa9-4294-bf3b-584dca7a62a3") ==
        local_id_seed("60ed06a2-dbd0-40f8-bd3f-e23786752f81"))
        return 6;

    // The seed moves the counter up past restored ids, never back.
    homeworldz::scene::Scene seeded;
    homeworldz::scene::Entity stored;
    stored.id = 7;
    stored.name = "stored";
    seeded.restore(1, {stored});
    seeded.reserve_ids_from(0x40000000ULL);
    if (seeded.create("first") != 0x40000000ULL) return 7;
    seeded.reserve_ids_from(5);
    if (seeded.create("second") != 0x40000001ULL) return 8;
    if (!seeded.find(7)) return 9;

    // Renumbering keeps the entity's state under a new id and frees the old.
    auto* moving = seeded.find(7);
    moving->position = {1.0, 2.0, 3.0};
    const auto renumbered = seeded.renumber(7);
    if (renumbered != 0x40000002ULL) return 10;
    const auto* moved = seeded.find(renumbered);
    if (!moved || moved->id != renumbered || moved->name != "stored" || moved->position.y != 2.0)
        return 11;
    if (seeded.find(7) || seeded.size() != 3) return 12;
    if (seeded.renumber(7) != 0) return 13;
    return 0;
}
