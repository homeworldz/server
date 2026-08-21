#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace homeworldz::scene {

using EntityId = std::uint64_t;

constexpr std::uint32_t permission_transfer = 0x00002000;
constexpr std::uint32_t permission_modify = 0x00004000;
constexpr std::uint32_t permission_copy = 0x00008000;
constexpr std::uint32_t permission_export = 0x00010000;
constexpr std::uint32_t permission_move = 0x00080000;
constexpr std::uint32_t permission_all = permission_transfer | permission_modify |
    permission_copy | permission_move;
constexpr std::uint32_t permission_creator = permission_all | permission_export;

constexpr std::uint8_t permission_field_base = 0x01;
constexpr std::uint8_t permission_field_owner = 0x02;
constexpr std::uint8_t permission_field_group = 0x04;
constexpr std::uint8_t permission_field_everyone = 0x08;
constexpr std::uint8_t permission_field_next_owner = 0x10;

struct Vector3 {
    double x{};
    double y{};
    double z{};
};

struct TaskInventoryItem {
    std::string item_id;
    std::string asset_id;
    std::string creator_id;
    std::string owner_id;
    std::string last_owner_id;
    std::string group_id;
    std::string name;
    std::string description;
    std::int8_t asset_type{-1};
    std::int8_t inventory_type{-1};
    std::uint32_t flags{};
    std::uint32_t base_permissions{};
    std::uint32_t current_permissions{};
    std::uint32_t group_permissions{};
    std::uint32_t everyone_permissions{};
    std::uint32_t next_permissions{};
    std::uint8_t sale_type{};
    std::int32_t sale_price{};
    std::uint64_t creation_date{};
};

struct EffectivePermissions {
    std::uint32_t owner{};
    std::uint32_t next_owner{};
};

struct Entity {
    EntityId id{};
    std::string name;
    Vector3 position;
    Vector3 velocity;
    std::string object_id;
    std::string owner_id;
    Vector3 scale{1.0, 1.0, 1.0};
    std::uint8_t material{3};
    std::string creator_id;
    std::uint32_t base_permissions{permission_creator};
    std::uint32_t owner_permissions{permission_creator};
    std::uint32_t group_permissions{};
    std::uint32_t everyone_permissions{};
    std::uint32_t next_owner_permissions{permission_all};
    std::uint64_t creation_date{};
    Vector3 rotation;
    std::string description;
    std::vector<std::byte> texture_entry;
    bool avatar_flying{};
    bool physical{};
    bool phantom{};
    bool temporary{};
    std::uint8_t physics_shape_type{};
    double physics_density{1000.0};
    double physics_friction{0.6};
    double physics_restitution{0.5};
    double physics_gravity_multiplier{1.0};
    std::uint8_t path_curve{0x10};
    std::uint8_t profile_curve{0x01};
    std::uint16_t path_begin{};
    std::uint16_t path_end{};
    std::uint8_t path_scale_x{100};
    std::uint8_t path_scale_y{100};
    std::uint8_t path_shear_x{};
    std::uint8_t path_shear_y{};
    std::uint8_t path_twist{};
    std::uint8_t path_twist_begin{};
    std::uint8_t path_radius_offset{};
    std::uint8_t path_taper_x{};
    std::uint8_t path_taper_y{};
    std::uint8_t path_revolutions{};
    std::uint8_t path_skew{};
    std::uint16_t profile_begin{};
    std::uint16_t profile_end{};
    std::uint16_t profile_hollow{};
    EntityId parent_id{};
    Vector3 local_position;
    Vector3 local_rotation;
    std::uint16_t task_inventory_serial{};
    std::vector<TaskInventoryItem> task_inventory;
    // A sculpted or mesh prim: the asset whose bytes shape it, and the sculpt
    // type byte viewers read (5 = mesh, ADR 0033). Empty id means an ordinary
    // parametric prim.
    std::string sculpt_id;
    std::uint8_t sculpt_type{};
    // glTF materials assigned per face, as (face index, material asset id).
    //
    // This is what carries the things a TextureEntry cannot say — two-sidedness
    // above all, without which single-sided hair cards lose half their faces to
    // backface culling. It rides ObjectUpdate's ExtraParams as the render
    // material block (0x80), which holds at most fourteen entries, and a face
    // with no entry keeps the TextureEntry alone.
    std::vector<std::pair<std::uint8_t, std::string>> face_materials;
    // Worn on an avatar. `attachment_point` is the viewer's point number with
    // ATTACHMENT_ADD already stripped; zero means this entity is not an
    // attachment, which is the only test any caller should make. When it is
    // set, `parent_id` is the wearer's avatar entity and `local_position` /
    // `local_rotation` are the offset from the attachment joint.
    std::uint8_t attachment_point{};
    // The inventory item being worn, on the root prim only. Detach reports it
    // back to the viewer, and it is how a second Wear of the same item is
    // recognised as a re-wear rather than a second copy.
    std::string attachment_item_id;
    // The seat `llSitTarget` put on this prim: an offset from the prim's centre
    // and a rotation, both in the prim's own space.
    //
    // A zero offset means no sit target, which is not a shortcut — it is LSL's
    // own rule, where `llSitTarget(ZERO_VECTOR, ZERO_ROTATION)` is how a script
    // takes a seat away. Keeping that rule means there is one answer to "does
    // this prim seat anyone", rather than a flag that can disagree with the
    // offset beside it.
    Vector3 sit_target_position;
    Vector3 sit_target_rotation;
};

// Whether a script has put a seat on this prim. The offset carries the answer;
// see the note on Entity::sit_target_position.
bool has_sit_target(const Entity& entity);

struct RayIntersection {
    Vector3 position;
    Vector3 normal;
};

bool apply_permission_update(
    Entity& entity, std::string_view agent_id, std::uint8_t field, bool set,
    std::uint32_t mask);

bool apply_task_inventory_update(
    TaskInventoryItem& item, std::string_view name, std::string_view description,
    std::uint32_t flags, std::uint32_t owner_permissions,
    std::uint32_t group_permissions, std::uint32_t everyone_permissions,
    std::uint32_t next_permissions, std::uint8_t sale_type, std::int32_t sale_price);

EffectivePermissions effective_permissions(const Entity& entity);

std::optional<RayIntersection> intersect_box(
    Vector3 ray_start, Vector3 ray_end, Vector3 center, Vector3 scale);

void establish_link(Entity& child, const Entity& root);
void update_linked_world_transform(Entity& child, const Entity& root);
void scale_linked_child(Entity& child, Vector3 factors);

class Scene {
public:
    EntityId create(std::string name, Vector3 position = {}, Vector3 velocity = {});
    bool remove(EntityId id);
    Entity* find(EntityId id);
    const Entity* find(EntityId id) const;
    void step(double seconds);
    void restore(std::uint64_t revision, std::vector<Entity> entities);

    std::size_t size() const { return entities_.size(); }
    std::uint64_t revision() const { return revision_; }
    std::uint64_t simulation_steps() const { return simulation_steps_; }
    const std::unordered_map<EntityId, Entity>& entities() const { return entities_; }

private:
    EntityId next_id_{1};
    std::uint64_t revision_{};
    std::uint64_t simulation_steps_{};
    std::unordered_map<EntityId, Entity> entities_;
};

EffectivePermissions effective_permissions(const Scene& scene, const Entity& selected);

} // namespace homeworldz::scene
