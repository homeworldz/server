#pragma once

#include "homeworldz/scene.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace homeworldz::asset {

struct ObjectAsset {
    std::string name;
    std::string creator_id;
    scene::Vector3 scale;
    scene::Vector3 rotation;
    std::uint8_t material{};
    std::string description;
    std::uint8_t physics_shape_type{};
    double physics_density{1000.0};
    double physics_friction{0.6};
    double physics_restitution{0.5};
    double physics_gravity_multiplier{1.0};
    std::vector<std::byte> texture_entry;
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
    bool physical{};
    bool phantom{};
    std::uint32_t base_permissions{scene::permission_creator};
    std::uint32_t owner_permissions{scene::permission_creator};
    std::uint32_t group_permissions{};
    std::uint32_t everyone_permissions{};
    std::uint32_t next_owner_permissions{scene::permission_all};
    std::uint16_t task_inventory_serial{};
    std::vector<scene::TaskInventoryItem> task_inventory;
    scene::Vector3 local_position;
    scene::Vector3 local_rotation;
    // Mesh and sculpted prims: the shaping asset and the sculpt type byte
    // (5 = mesh, ADR 0033). Empty id is an ordinary parametric prim.
    std::string sculpt_id;
    std::uint8_t sculpt_type{};
    // glTF materials by face, as (face index, material asset id). Stored with
    // the object because publishing assigns them and this asset is written
    // immediately afterwards — a field the asset cannot hold is a field that
    // never survives to be sent.
    std::vector<std::pair<std::uint8_t, std::string>> face_materials;
    // The seat a script put on the prim. Carried by the asset so that a take,
    // a rez, and a border crossing all preserve it — a seat that survived
    // being taken but not being crossed would be a strange thing to explain.
    scene::Vector3 sit_target_position;
    scene::Vector3 sit_target_rotation;
};

struct LinksetAsset {
    ObjectAsset root;
    std::vector<ObjectAsset> children;
};

// The texture asset UUIDs a viewer TextureEntry names: the default texture
// and every per-face exception, in encounter order, deduplicated, with the
// zero UUID (the format's "none") dropped. Only the texture section is read;
// the colors, repeats, and bump sections that follow carry no asset
// references. Used to walk an object's reference closure (ADR 0026).
std::vector<std::string> texture_entry_texture_ids(std::span<const std::byte> texture_entry);

std::optional<ObjectAsset> parse_object_asset(std::span<const std::byte> content);
std::optional<LinksetAsset> parse_linkset_asset(std::span<const std::byte> content);
std::string serialize_linkset_asset(
    const scene::Entity& root, std::span<const scene::Entity* const> children = {});

} // namespace homeworldz::asset
