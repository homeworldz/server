#include "homeworldz/object_asset.h"
#include "homeworldz/viewer_protocol.h"

#include <algorithm>
#include <span>
#include <vector>

#include <array>
#include <string>

static int mesh_wrapper_chain();

int main() {
    const std::string json = R"({"format":"homeworldz-object-v1","creatorId":"10000000-0000-4000-8000-000000000001","name":"Prism","scale":[0.500000,0.500000,0.500000],"rotation":[0.000000,0.000000,0.382683],"description":"Round \"prim\"","material":3,"physicsShapeType":2,"physicsDensity":125.000000,"physicsFriction":0.700000,"physicsRestitution":0.250000,"physicsGravityMultiplier":1.500000,"textureEntry":"aabbcc","pathCurve":16,"profileCurve":1,"pathBegin":0,"pathEnd":0,"pathScaleX":200,"pathScaleY":100,"pathShearX":206,"pathShearY":0,"pathTwist":0,"pathTwistBegin":0,"pathRadiusOffset":0,"pathTaperX":0,"pathTaperY":0,"pathRevolutions":0,"pathSkew":0,"profileBegin":0,"profileEnd":0,"profileHollow":0,"physical":true,"phantom":false,"basePermissions":647168})";
    const auto bytes = std::span(reinterpret_cast<const std::byte*>(json.data()), json.size());
    const auto asset = homeworldz::asset::parse_object_asset(bytes);
    if (!asset || asset->scale.x != 0.5 || asset->rotation.z != 0.382683 ||
        asset->material != 3 || asset->physics_shape_type != 2 ||
        asset->physics_density != 125.0 || asset->physics_friction != 0.7 ||
        asset->physics_restitution != 0.25 || asset->physics_gravity_multiplier != 1.5 ||
        asset->texture_entry != std::vector<std::byte>{std::byte{0xaa}, std::byte{0xbb}, std::byte{0xcc}} ||
        asset->path_curve != 0x10 || asset->profile_curve != 0x01 || asset->path_scale_x != 200 ||
        asset->path_scale_y != 100 || asset->path_shear_x != 0xce || !asset->physical || asset->phantom ||
        asset->description != "Round \"prim\"")
        return 1;
    auto no_texture_json = json;
    const std::string texture_text = "\"textureEntry\":\"aabbcc\",";
    const auto texture_field = no_texture_json.find(texture_text);
    if (texture_field == std::string::npos) return 1;
    no_texture_json.erase(texture_field, texture_text.size());
    const auto no_texture = homeworldz::asset::parse_object_asset(std::span(
        reinterpret_cast<const std::byte*>(no_texture_json.data()), no_texture_json.size()));
    if (!no_texture || !no_texture->texture_entry.empty()) return 1;
    const std::string invalid = R"({"format":"homeworldz-object-v1","scale":[0,1,1],"rotation":[0,0,0],"description":"","material":3})";
    if (homeworldz::asset::parse_object_asset(
            std::span(reinterpret_cast<const std::byte*>(invalid.data()), invalid.size())))
        return 1;
    homeworldz::scene::Entity root;
    root.id = 10;
    root.name = "Root Prim";
    root.creator_id = "10000000-0000-4000-8000-000000000001";
    root.scale = {1.0, 2.0, 3.0};
    root.rotation = {0.0, 0.0, 0.25};
    root.owner_permissions = 0x0008e000;
    root.task_inventory_serial = 7;
    root.task_inventory.push_back({
        "30000000-0000-4000-8000-000000000003",
        "40000000-0000-4000-8000-000000000004",
        root.creator_id, "50000000-0000-4000-8000-000000000005",
        "60000000-0000-4000-8000-000000000006",
        "00000000-0000-0000-0000-000000000000", "Surface Texture", "Task content",
        0, 0, 0x01020304, 0x0008e000, 0x0008a000, 0, 0x00008000, 0x00002000,
        1, 25, 1234567890});
    homeworldz::scene::Entity child;
    child.id = 11;
    child.parent_id = root.id;
    child.name = "Child Prim";
    child.creator_id = "20000000-0000-4000-8000-000000000002";
    child.scale = {0.5, 0.75, 1.0};
    child.local_position = {2.0, -3.0, 4.0};
    child.local_rotation = {0.125, 0.0, -0.25};
    child.next_owner_permissions = 0x00082000;
    child.task_inventory_serial = 2;
    child.task_inventory.push_back(root.task_inventory.front());
    child.task_inventory.front().name = "Child Texture";
    // A seat on the child, none on the root: a take must carry the seat with
    // the prim that has it and must not invent one on the prim that does not.
    child.sit_target_position = {0.0, 0.4, 0.6};
    child.sit_target_rotation = {0.0, 0.0, -0.5};
    const std::array<const homeworldz::scene::Entity*, 1> children{&child};
    const auto linkset_text = homeworldz::asset::serialize_linkset_asset(root, children);
    const auto linkset = homeworldz::asset::parse_linkset_asset(std::span(
        reinterpret_cast<const std::byte*>(linkset_text.data()), linkset_text.size()));
    if (!linkset || linkset->root.name != "Root Prim" || linkset->root.scale.y != 2.0 ||
        linkset->root.rotation.z != 0.25 || linkset->root.owner_permissions != 0x0008e000 ||
        linkset->children.size() != 1 || linkset->children[0].name != "Child Prim" ||
        linkset->children[0].creator_id != child.creator_id ||
        linkset->children[0].local_position.x != 2.0 ||
        linkset->children[0].local_position.y != -3.0 ||
        linkset->children[0].local_rotation.z != -0.25 ||
        linkset->children[0].next_owner_permissions != 0x00082000 ||
        linkset->root.task_inventory_serial != 7 || linkset->root.task_inventory.size() != 1 ||
        linkset->root.task_inventory[0].name != "Surface Texture" ||
        linkset->root.task_inventory[0].flags != 0x01020304 ||
        linkset->root.task_inventory[0].base_permissions != 0x0008e000 ||
        linkset->root.task_inventory[0].current_permissions != 0x0008a000 ||
        linkset->root.task_inventory[0].everyone_permissions != 0x00008000 ||
        linkset->root.task_inventory[0].next_permissions != 0x00002000 ||
        linkset->root.task_inventory[0].sale_price != 25 ||
        linkset->children[0].task_inventory_serial != 2 ||
        linkset->children[0].task_inventory.size() != 1 ||
        linkset->children[0].task_inventory[0].name != "Child Texture" ||
        linkset->children[0].sit_target_position.y != 0.4 ||
        linkset->children[0].sit_target_position.z != 0.6 ||
        linkset->children[0].sit_target_rotation.z != -0.5 ||
        linkset->root.sit_target_position.x != 0.0 ||
        linkset->root.sit_target_position.y != 0.0 ||
        linkset->root.sit_target_position.z != 0.0)
        return 1;
    // The offset is the whole answer to "is there a seat here".
    if (!homeworldz::scene::has_sit_target(child) || homeworldz::scene::has_sit_target(root))
        return 1;
    const auto single = homeworldz::asset::parse_linkset_asset(bytes);
    if (!single || !single->children.empty() || single->root.description != "Round \"prim\"")
        return 1;
    if (single->root.task_inventory_serial != 0 || !single->root.task_inventory.empty()) return 1;

    // The texture UUIDs a TextureEntry names: the default, then per-face
    // exceptions behind varint bitfields, terminated by a zero bitfield. The
    // zero UUID means "none" and duplicates collapse; sections after the
    // terminator (colors etc.) must not be read as textures.
    {
        const auto uuid_bytes = [](std::uint8_t seed) {
            std::vector<std::byte> raw(16);
            for (std::size_t index = 0; index < 16; ++index)
                raw[index] = static_cast<std::byte>(seed);
            return raw;
        };
        std::vector<std::byte> entry = uuid_bytes(0x11);       // default texture
        entry.push_back(static_cast<std::byte>(0x02));         // face 1
        const auto face_texture = uuid_bytes(0xab);
        entry.insert(entry.end(), face_texture.begin(), face_texture.end());
        entry.push_back(static_cast<std::byte>(0x81));         // two-byte varint...
        entry.push_back(static_cast<std::byte>(0x04));         // ...faces 0x84
        const auto duplicate = uuid_bytes(0x11);               // same as default
        entry.insert(entry.end(), duplicate.begin(), duplicate.end());
        entry.push_back(static_cast<std::byte>(0x00));         // terminator
        const auto trailing = uuid_bytes(0xff);                // colors section noise
        entry.insert(entry.end(), trailing.begin(), trailing.end());
        const auto ids = homeworldz::asset::texture_entry_texture_ids(entry);
        if (ids.size() != 2 || ids[0] != "11111111-1111-1111-1111-111111111111" ||
            ids[1] != "abababab-abab-abab-abab-abababababab")
            return 1;
        // The zero UUID and short buffers yield nothing.
        std::vector<std::byte> zero(16, std::byte{});
        if (!homeworldz::asset::texture_entry_texture_ids(zero).empty()) return 1;
        std::vector<std::byte> tiny(7, std::byte{0x22});
        if (!homeworldz::asset::texture_entry_texture_ids(tiny).empty()) return 1;
    }
    if (const auto chain = mesh_wrapper_chain(); chain != 0) return chain;
    return 0;
}
// Appended: the mesh wrapper chain end to end at the wire level — an entity
// with a sculpt reference serializes, parses, and encodes into an
// ObjectUpdate whose ExtraParams carry the mesh parameter (0x60) with the
// shaping asset and type 5. Guards the exact chain a rezzed GLB wrapper
// travels (ADR 0033).
static int mesh_wrapper_chain() {
    homeworldz::scene::Entity wrapper;
    wrapper.name = "Wrapped Mesh";
    wrapper.creator_id = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
    wrapper.owner_id = wrapper.creator_id;
    wrapper.scale = {2.0, 1.0, 0.5};
    wrapper.sculpt_id = "17b03cc1-091e-49d8-bc57-18c2d5d10f93";
    wrapper.sculpt_type = 5;
    const auto serialized = homeworldz::asset::serialize_linkset_asset(wrapper);
    const auto parsed = homeworldz::asset::parse_linkset_asset(std::span(
        reinterpret_cast<const std::byte*>(serialized.data()), serialized.size()));
    if (!parsed || parsed->root.sculpt_id != wrapper.sculpt_id ||
        parsed->root.sculpt_type != 5)
        return 100;

    homeworldz::viewer::StaticObject object;
    object.local_id = 7;
    object.id = homeworldz::viewer::parse_uuid("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa").value();
    object.owner_id = homeworldz::viewer::parse_uuid(wrapper.creator_id).value();
    object.sculpt_id = homeworldz::viewer::parse_uuid(wrapper.sculpt_id).value();
    object.sculpt_type = 5;
    const auto update = homeworldz::viewer::encode_static_object_update(0, object);
    if (update.empty()) return 101;
    // The ExtraParams payload: count 1, parameter 0x60 (little-endian u16),
    // size 17 (little-endian u32), 16-byte uuid, type byte 5. Find it as a
    // byte sequence: [01][60 00][11 00 00 00][uuid...][05].
    std::vector<std::byte> expected{std::byte{1}, std::byte{0x60}, std::byte{0},
                                    std::byte{17}, std::byte{0}, std::byte{0}, std::byte{0}};
    const auto uuid_raw = homeworldz::viewer::parse_uuid(wrapper.sculpt_id).value();
    expected.insert(expected.end(), uuid_raw.begin(), uuid_raw.end());
    expected.push_back(std::byte{5});
    const auto found = std::search(update.begin(), update.end(),
                                   expected.begin(), expected.end());
    if (found == update.end()) return 102;
    // And the length byte of the variable field precedes the block.
    if (found == update.begin() || *(found - 1) != std::byte{24}) return 103;
    return 0;
}

// Chain check runs after the existing assertions via a static initializer
// shim replaced below by an explicit call — see main.
