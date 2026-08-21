#include "homeworldz/object_asset.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

namespace homeworldz::asset {
namespace {

std::optional<double> number_after(std::string_view content, std::string_view marker,
                                   std::size_t& position) {
    const auto marker_position = content.find(marker, position);
    if (marker_position == std::string_view::npos) return std::nullopt;
    const auto start = marker_position + marker.size();
    double value{};
    const auto parsed = std::from_chars(content.data() + start, content.data() + content.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr == content.data() + start || !std::isfinite(value))
        return std::nullopt;
    position = static_cast<std::size_t>(parsed.ptr - content.data());
    return value;
}

std::optional<scene::Vector3> vector_after(std::string_view content, std::string_view marker) {
    std::size_t position = 0;
    const auto x = number_after(content, marker, position);
    const auto y = number_after(content, ",", position);
    const auto z = number_after(content, ",", position);
    if (!x || !y || !z || position >= content.size() || content[position] != ']') return std::nullopt;
    return scene::Vector3{*x, *y, *z};
}

std::optional<std::string> string_after(std::string_view content, std::string_view marker) {
    const auto marker_position = content.find(marker);
    if (marker_position == std::string_view::npos) return std::nullopt;
    std::string result;
    for (std::size_t position = marker_position + marker.size(); position < content.size(); ++position) {
        const auto character = content[position];
        if (character == '"') return result;
        if (character != '\\') {
            if (static_cast<unsigned char>(character) < 0x20) return std::nullopt;
            result.push_back(character);
            continue;
        }
        if (++position >= content.size()) return std::nullopt;
        switch (content[position]) {
        case '"': result.push_back('"'); break;
        case '\\': result.push_back('\\'); break;
        case '/': result.push_back('/'); break;
        case 'b': result.push_back('\b'); break;
        case 'f': result.push_back('\f'); break;
        case 'n': result.push_back('\n'); break;
        case 'r': result.push_back('\r'); break;
        case 't': result.push_back('\t'); break;
        default: return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<bool> boolean_after(std::string_view content, std::string_view marker) {
    const auto marker_position = content.find(marker);
    if (marker_position == std::string_view::npos) return std::nullopt;
    const auto start = marker_position + marker.size();
    if (content.substr(start, 4) == "true") return true;
    if (content.substr(start, 5) == "false") return false;
    return std::nullopt;
}

template <typename Integer>
std::optional<Integer> integer_after(std::string_view content, std::string_view marker) {
    const auto marker_position = content.find(marker);
    if (marker_position == std::string_view::npos) return std::nullopt;
    const auto start = marker_position + marker.size();
    Integer value{};
    const auto parsed = std::from_chars(content.data() + start, content.data() + content.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr == content.data() + start) return std::nullopt;
    return value;
}

std::optional<std::vector<std::byte>> bytes_from_hex(std::string_view value) {
    if (value.size() % 2 != 0 || value.size() > 131070) return std::nullopt;
    std::vector<std::byte> result(value.size() / 2);
    for (std::size_t index = 0; index < result.size(); ++index) {
        unsigned parsed{};
        const auto converted = std::from_chars(
            value.data() + index * 2, value.data() + index * 2 + 2, parsed, 16);
        if (converted.ec != std::errc{} || converted.ptr != value.data() + index * 2 + 2)
            return std::nullopt;
        result[index] = static_cast<std::byte>(parsed);
    }
    return result;
}

std::string json_string(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result{"\""};
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        switch (character) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (byte < 0x20) {
                result += "\\u00";
                result.push_back(hex[byte >> 4]);
                result.push_back(hex[byte & 0x0f]);
            } else {
                result.push_back(character);
            }
        }
    }
    result.push_back('"');
    return result;
}

std::optional<scene::TaskInventoryItem> parse_task_inventory_item(std::string_view text) {
    scene::TaskInventoryItem item;
    const auto item_id = string_after(text, R"("itemId":")");
    const auto asset_id = string_after(text, R"("assetId":")");
    const auto creator_id = string_after(text, R"("creatorId":")");
    const auto owner_id = string_after(text, R"("ownerId":")");
    const auto last_owner_id = string_after(text, R"("lastOwnerId":")");
    const auto group_id = string_after(text, R"("groupId":")");
    const auto name = string_after(text, R"("name":")");
    const auto description = string_after(text, R"("description":")");
    const auto asset_type = integer_after<int>(text, R"("assetType":)");
    const auto inventory_type = integer_after<int>(text, R"("inventoryType":)");
    const auto flags = integer_after<std::uint32_t>(text, R"("flags":)");
    const auto base_permissions = integer_after<std::uint32_t>(text, R"("basePermissions":)");
    const auto current_permissions = integer_after<std::uint32_t>(text, R"("currentPermissions":)");
    const auto group_permissions = integer_after<std::uint32_t>(text, R"("groupPermissions":)");
    const auto everyone_permissions = integer_after<std::uint32_t>(text, R"("everyonePermissions":)");
    const auto next_permissions = integer_after<std::uint32_t>(text, R"("nextPermissions":)");
    const auto sale_type = integer_after<unsigned>(text, R"("saleType":)");
    const auto sale_price = integer_after<std::int32_t>(text, R"("salePrice":)");
    const auto creation_date = integer_after<std::uint64_t>(text, R"("creationDate":)");
    if (!item_id || !asset_id || !creator_id || !owner_id || !last_owner_id || !group_id ||
        !name || !description || !asset_type || !inventory_type || !flags || !base_permissions ||
        !current_permissions || !group_permissions || !everyone_permissions || !next_permissions ||
        !sale_type || !sale_price || !creation_date || *asset_type < -1 || *asset_type > 127 ||
        *inventory_type < -1 || *inventory_type > 127 || *sale_type > 255 ||
        item_id->size() > 128 || asset_id->size() > 128 || creator_id->size() > 128 ||
        owner_id->size() > 128 || last_owner_id->size() > 128 || group_id->size() > 128 ||
        name->size() > 1024 || description->size() > 4096)
        return std::nullopt;
    item.item_id = *item_id;
    item.asset_id = *asset_id;
    item.creator_id = *creator_id;
    item.owner_id = *owner_id;
    item.last_owner_id = *last_owner_id;
    item.group_id = *group_id;
    item.name = *name;
    item.description = *description;
    item.asset_type = static_cast<std::int8_t>(*asset_type);
    item.inventory_type = static_cast<std::int8_t>(*inventory_type);
    item.flags = *flags;
    item.base_permissions = *base_permissions;
    item.current_permissions = *current_permissions;
    item.group_permissions = *group_permissions;
    item.everyone_permissions = *everyone_permissions;
    item.next_permissions = *next_permissions;
    item.sale_type = static_cast<std::uint8_t>(*sale_type);
    item.sale_price = *sale_price;
    item.creation_date = *creation_date;
    return item;
}

std::optional<std::vector<scene::TaskInventoryItem>> parse_task_inventory(std::string_view text) {
    const auto marker = text.find(R"("taskInventory":[)");
    if (marker == std::string_view::npos) return std::vector<scene::TaskInventoryItem>{};
    std::vector<scene::TaskInventoryItem> result;
    std::size_t position = marker + 17;
    while (position < text.size()) {
        while (position < text.size() && (text[position] == ' ' || text[position] == ',')) ++position;
        if (position < text.size() && text[position] == ']') return result;
        if (position >= text.size() || text[position] != '{' || result.size() >= 1024)
            return std::nullopt;
        const auto start = position;
        std::size_t depth = 0;
        bool quoted = false;
        bool escaped = false;
        for (; position < text.size(); ++position) {
            const auto character = text[position];
            if (quoted) {
                if (escaped) escaped = false;
                else if (character == '\\') escaped = true;
                else if (character == '"') quoted = false;
                continue;
            }
            if (character == '"') quoted = true;
            else if (character == '{') ++depth;
            else if (character == '}' && --depth == 0) {
                ++position;
                break;
            }
        }
        if (depth != 0) return std::nullopt;
        const auto item = parse_task_inventory_item(text.substr(start, position - start));
        if (!item) return std::nullopt;
        result.push_back(*item);
    }
    return std::nullopt;
}

std::string task_inventory_json(const scene::Entity& entity) {
    auto result = ",\"taskInventorySerial\":" + std::to_string(entity.task_inventory_serial) +
        ",\"taskInventory\":[";
    bool first = true;
    for (const auto& item : entity.task_inventory) {
        if (!first) result.push_back(',');
        first = false;
        result += "{\"itemId\":" + json_string(item.item_id) +
            ",\"assetId\":" + json_string(item.asset_id) +
            ",\"creatorId\":" + json_string(item.creator_id) +
            ",\"ownerId\":" + json_string(item.owner_id) +
            ",\"lastOwnerId\":" + json_string(item.last_owner_id) +
            ",\"groupId\":" + json_string(item.group_id) +
            ",\"name\":" + json_string(item.name) +
            ",\"description\":" + json_string(item.description) +
            ",\"assetType\":" + std::to_string(item.asset_type) +
            ",\"inventoryType\":" + std::to_string(item.inventory_type) +
            ",\"flags\":" + std::to_string(item.flags) +
            ",\"basePermissions\":" + std::to_string(item.base_permissions) +
            ",\"currentPermissions\":" + std::to_string(item.current_permissions) +
            ",\"groupPermissions\":" + std::to_string(item.group_permissions) +
            ",\"everyonePermissions\":" + std::to_string(item.everyone_permissions) +
            ",\"nextPermissions\":" + std::to_string(item.next_permissions) +
            ",\"saleType\":" + std::to_string(item.sale_type) +
            ",\"salePrice\":" + std::to_string(item.sale_price) +
            ",\"creationDate\":" + std::to_string(item.creation_date) + '}';
    }
    return result + ']';
}

std::string object_json(const scene::Entity& entity, bool child) {
    constexpr char hex[] = "0123456789abcdef";
    std::string texture_entry(entity.texture_entry.size() * 2, '0');
    for (std::size_t index = 0; index < entity.texture_entry.size(); ++index) {
        const auto value = std::to_integer<unsigned>(entity.texture_entry[index]);
        texture_entry[index * 2] = hex[value >> 4];
        texture_entry[index * 2 + 1] = hex[value & 0x0f];
    }
    const auto& rotation = child ? entity.local_rotation : entity.rotation;
    auto result = "{\"format\":\"homeworldz-object-v1\",\"creatorId\":" +
        json_string(entity.creator_id) + ",\"name\":" + json_string(entity.name) +
        ",\"scale\":[" + std::to_string(entity.scale.x) + ',' +
        std::to_string(entity.scale.y) + ',' + std::to_string(entity.scale.z) +
        "],\"rotation\":[" + std::to_string(rotation.x) + ',' +
        std::to_string(rotation.y) + ',' + std::to_string(rotation.z) +
        "],\"description\":" + json_string(entity.description) +
        ",\"material\":" + std::to_string(entity.material) +
        ",\"physicsShapeType\":" + std::to_string(entity.physics_shape_type) +
        ",\"physicsDensity\":" + std::to_string(entity.physics_density) +
        ",\"physicsFriction\":" + std::to_string(entity.physics_friction) +
        ",\"physicsRestitution\":" + std::to_string(entity.physics_restitution) +
        ",\"physicsGravityMultiplier\":" + std::to_string(entity.physics_gravity_multiplier) +
        ",\"textureEntry\":" + json_string(texture_entry) +
        ",\"pathCurve\":" + std::to_string(entity.path_curve) +
        ",\"profileCurve\":" + std::to_string(entity.profile_curve) +
        ",\"pathBegin\":" + std::to_string(entity.path_begin) +
        ",\"pathEnd\":" + std::to_string(entity.path_end) +
        ",\"pathScaleX\":" + std::to_string(entity.path_scale_x) +
        ",\"pathScaleY\":" + std::to_string(entity.path_scale_y) +
        ",\"pathShearX\":" + std::to_string(entity.path_shear_x) +
        ",\"pathShearY\":" + std::to_string(entity.path_shear_y) +
        ",\"pathTwist\":" + std::to_string(entity.path_twist) +
        ",\"pathTwistBegin\":" + std::to_string(entity.path_twist_begin) +
        ",\"pathRadiusOffset\":" + std::to_string(entity.path_radius_offset) +
        ",\"pathTaperX\":" + std::to_string(entity.path_taper_x) +
        ",\"pathTaperY\":" + std::to_string(entity.path_taper_y) +
        ",\"pathRevolutions\":" + std::to_string(entity.path_revolutions) +
        ",\"pathSkew\":" + std::to_string(entity.path_skew) +
        ",\"profileBegin\":" + std::to_string(entity.profile_begin) +
        ",\"profileEnd\":" + std::to_string(entity.profile_end) +
        ",\"profileHollow\":" + std::to_string(entity.profile_hollow) +
        ",\"physical\":" + (entity.physical ? "true" : "false") +
        ",\"phantom\":" + (entity.phantom ? "true" : "false") +
        ",\"basePermissions\":" + std::to_string(entity.base_permissions) +
        ",\"ownerPermissions\":" + std::to_string(entity.owner_permissions) +
        ",\"groupPermissions\":" + std::to_string(entity.group_permissions) +
        ",\"everyonePermissions\":" + std::to_string(entity.everyone_permissions) +
        ",\"nextOwnerPermissions\":" + std::to_string(entity.next_owner_permissions) +
        task_inventory_json(entity);
    if (!entity.sculpt_id.empty())
        result += ",\"sculptId\":" + json_string(entity.sculpt_id) +
            ",\"sculptType\":" + std::to_string(entity.sculpt_type);
    // Face materials, without which two-sidedness is lost the moment the object
    // is stored: publish assigns them and this serialisation is the very next
    // step, so a field missing here is a field that never reaches anybody.
    if (!entity.face_materials.empty()) {
        result += ",\"faceMaterials\":[";
        bool first = true;
        for (const auto& [face, material] : entity.face_materials) {
            if (!first) result += ',';
            first = false;
            result += "{\"face\":" + std::to_string(static_cast<unsigned>(face)) +
                ",\"id\":" + json_string(material) + '}';
        }
        result += ']';
    }
    if (scene::has_sit_target(entity))
        result += ",\"sitTarget\":[" + std::to_string(entity.sit_target_position.x) + ',' +
            std::to_string(entity.sit_target_position.y) + ',' +
            std::to_string(entity.sit_target_position.z) + ']' +
            ",\"sitTargetRotation\":[" + std::to_string(entity.sit_target_rotation.x) + ',' +
            std::to_string(entity.sit_target_rotation.y) + ',' +
            std::to_string(entity.sit_target_rotation.z) + ']';
    if (child)
        result += ",\"localPosition\":[" + std::to_string(entity.local_position.x) + ',' +
            std::to_string(entity.local_position.y) + ',' +
            std::to_string(entity.local_position.z) + ']';
    return result + '}';
}

} // namespace

std::vector<std::string> texture_entry_texture_ids(std::span<const std::byte> texture_entry) {
    constexpr char hex[] = "0123456789abcdef";
    const auto format = [&](std::span<const std::byte> raw) {
        std::string id;
        id.reserve(36);
        for (std::size_t index = 0; index < 16; ++index) {
            if (index == 4 || index == 6 || index == 8 || index == 10) id.push_back('-');
            const auto value = std::to_integer<unsigned>(raw[index]);
            id.push_back(hex[value >> 4]);
            id.push_back(hex[value & 0x0f]);
        }
        return id;
    };
    std::vector<std::string> ids;
    const auto add = [&](std::span<const std::byte> raw) {
        auto id = format(raw);
        if (id == "00000000-0000-0000-0000-000000000000") return;
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(std::move(id));
    };
    if (texture_entry.size() < 16) return ids;
    add(texture_entry.subspan(0, 16));
    // The texture section: {face-bitfield varint, 16-byte texture} exceptions
    // until a zero bitfield. The varint uses the high bit as a continuation
    // flag, viewer convention; a run of continuation bytes past any plausible
    // face count is malformed and ends the walk rather than reading colors as
    // textures.
    std::size_t offset = 16;
    while (true) {
        std::uint64_t bits = 0;
        int length = 0;
        bool complete = false;
        while (offset < texture_entry.size()) {
            const auto value = std::to_integer<unsigned>(texture_entry[offset]);
            ++offset;
            bits = (bits << 7) | (value & 0x7fU);
            if ((value & 0x80U) == 0) {
                complete = true;
                break;
            }
            if (++length > 8) return ids;
        }
        if (!complete || bits == 0) break;
        if (offset + 16 > texture_entry.size()) break;
        add(texture_entry.subspan(offset, 16));
        offset += 16;
    }
    return ids;
}

std::optional<ObjectAsset> parse_object_asset(std::span<const std::byte> content) {
    const std::string_view text(reinterpret_cast<const char*>(content.data()), content.size());
    if (text.find(R"("format":"homeworldz-object-v1")") == std::string_view::npos)
        return std::nullopt;
    const auto scale = vector_after(text, R"("scale":[)");
    const auto rotation = vector_after(text, R"("rotation":[)");
    const auto description = string_after(text, R"("description":")");
    const auto name = string_after(text, R"("name":")");
    const auto creator_id = string_after(text, R"("creatorId":")");
    const auto local_position = vector_after(text, R"("localPosition":[)");
    // Absent on every asset written before seats existed, and on every prim
    // without one, so its absence is ordinary rather than a parse failure.
    const auto sit_target = vector_after(text, R"("sitTarget":[)");
    const auto sit_target_rotation = vector_after(text, R"("sitTargetRotation":[)");
    std::size_t position = 0;
    const auto material = number_after(text, R"("material":)", position);
    position = 0;
    auto physics_shape_type = number_after(text, R"("physicsShapeType":)", position);
    position = 0;
    auto physics_density = number_after(text, R"("physicsDensity":)", position);
    position = 0;
    auto physics_friction = number_after(text, R"("physicsFriction":)", position);
    position = 0;
    auto physics_restitution = number_after(text, R"("physicsRestitution":)", position);
    position = 0;
    auto physics_gravity_multiplier = number_after(text, R"("physicsGravityMultiplier":)", position);
    position = 0;
    auto path_curve = number_after(text, R"("pathCurve":)", position);
    position = 0;
    auto profile_curve = number_after(text, R"("profileCurve":)", position);
    const auto shape_number = [&](std::string_view key, double default_value) {
        std::size_t shape_position = 0;
        const auto value = number_after(text, key, shape_position);
        return value.value_or(default_value);
    };
    const auto path_begin = shape_number(R"("pathBegin":)", 0.0);
    const auto path_end = shape_number(R"("pathEnd":)", 0.0);
    const auto path_scale_x = shape_number(R"("pathScaleX":)", 100.0);
    const auto path_scale_y = shape_number(R"("pathScaleY":)", 100.0);
    const auto path_shear_x = shape_number(R"("pathShearX":)", 0.0);
    const auto path_shear_y = shape_number(R"("pathShearY":)", 0.0);
    const auto path_twist = shape_number(R"("pathTwist":)", 0.0);
    const auto path_twist_begin = shape_number(R"("pathTwistBegin":)", 0.0);
    const auto path_radius_offset = shape_number(R"("pathRadiusOffset":)", 0.0);
    const auto path_taper_x = shape_number(R"("pathTaperX":)", 0.0);
    const auto path_taper_y = shape_number(R"("pathTaperY":)", 0.0);
    const auto path_revolutions = shape_number(R"("pathRevolutions":)", 0.0);
    const auto path_skew = shape_number(R"("pathSkew":)", 0.0);
    const auto profile_begin = shape_number(R"("profileBegin":)", 0.0);
    const auto profile_end = shape_number(R"("profileEnd":)", 0.0);
    const auto profile_hollow = shape_number(R"("profileHollow":)", 0.0);
    const auto base_permissions = shape_number(R"("basePermissions":)", scene::permission_creator);
    const auto owner_permissions = shape_number(R"("ownerPermissions":)", scene::permission_creator);
    const auto group_permissions = shape_number(R"("groupPermissions":)", 0.0);
    const auto everyone_permissions = shape_number(R"("everyonePermissions":)", 0.0);
    const auto next_owner_permissions = shape_number(R"("nextOwnerPermissions":)", scene::permission_all);
    auto physical = boolean_after(text, R"("physical":)");
    auto phantom = boolean_after(text, R"("phantom":)");
    const auto texture_entry_hex = string_after(text, R"("textureEntry":")");
    const auto task_inventory = parse_task_inventory(text);
    const auto task_inventory_serial_marker = text.find(R"("taskInventorySerial":)");
    const auto parsed_task_inventory_serial = integer_after<std::uint16_t>(
        text, R"("taskInventorySerial":)");
    if (task_inventory_serial_marker != std::string_view::npos &&
        !parsed_task_inventory_serial)
        return std::nullopt;
    const auto task_inventory_serial = parsed_task_inventory_serial.value_or(0);
    if (!scale || !rotation || !description || !material ||
        scale->x <= 0.0 || scale->y <= 0.0 || scale->z <= 0.0 ||
        scale->x > 64.0 || scale->y > 64.0 || scale->z > 64.0 ||
        *material < 0.0 || *material > 7.0 || std::floor(*material) != *material)
        return std::nullopt;
    if (!physics_shape_type) physics_shape_type = 0.0;
    if (!physics_density) physics_density = 1000.0;
    if (!physics_friction) physics_friction = 0.6;
    if (!physics_restitution) physics_restitution = 0.5;
    if (!physics_gravity_multiplier) physics_gravity_multiplier = 1.0;
    if (!path_curve) path_curve = 0x10;
    if (!profile_curve) profile_curve = 0x01;
    if (!physical) physical = false;
    if (!phantom) phantom = false;
    if (*physics_shape_type < 0.0 || *physics_shape_type > 2.0 ||
        std::floor(*physics_shape_type) != *physics_shape_type ||
        *physics_density < 1.0 || *physics_density > 22587.0 ||
        *physics_friction < 0.0 || *physics_friction > 255.0 ||
        *physics_restitution < 0.0 || *physics_restitution > 1.0 ||
        *physics_gravity_multiplier < -1.0 || *physics_gravity_multiplier > 28.0 ||
        *path_curve < 0.0 || *path_curve > 255.0 || std::floor(*path_curve) != *path_curve ||
        *profile_curve < 0.0 || *profile_curve > 255.0 ||
        std::floor(*profile_curve) != *profile_curve)
        return std::nullopt;
    const std::array byte_values{path_scale_x, path_scale_y, path_shear_x, path_shear_y,
        path_twist, path_twist_begin, path_radius_offset, path_taper_x, path_taper_y,
        path_revolutions, path_skew};
    const std::array word_values{path_begin, path_end, profile_begin, profile_end, profile_hollow};
    const std::array permission_values{base_permissions, owner_permissions, group_permissions,
        everyone_permissions, next_owner_permissions};
    if (std::any_of(byte_values.begin(), byte_values.end(), [](double value) {
            return value < 0.0 || value > 255.0 || std::floor(value) != value;
        }) || std::any_of(word_values.begin(), word_values.end(), [](double value) {
            return value < 0.0 || value > 65535.0 || std::floor(value) != value;
        }) || std::any_of(permission_values.begin(), permission_values.end(), [](double value) {
            return value < 0.0 || value > std::numeric_limits<std::uint32_t>::max() ||
                std::floor(value) != value;
        }))
        return std::nullopt;
    auto texture_entry = texture_entry_hex
        ? bytes_from_hex(*texture_entry_hex)
        : std::optional<std::vector<std::byte>>(std::vector<std::byte>{});
    if (!texture_entry || !task_inventory) return std::nullopt;
    ObjectAsset result;
    result.name = name.value_or("");
    result.creator_id = creator_id.value_or("");
    result.scale = *scale;
    result.rotation = *rotation;
    result.material = static_cast<std::uint8_t>(*material);
    result.description = *description;
    result.physics_shape_type = static_cast<std::uint8_t>(*physics_shape_type);
    result.physics_density = *physics_density;
    result.physics_friction = *physics_friction;
    result.physics_restitution = *physics_restitution;
    result.physics_gravity_multiplier = *physics_gravity_multiplier;
    result.texture_entry = std::move(*texture_entry);
    result.path_curve = static_cast<std::uint8_t>(*path_curve);
    result.profile_curve = static_cast<std::uint8_t>(*profile_curve);
    result.path_begin = static_cast<std::uint16_t>(path_begin);
    result.path_end = static_cast<std::uint16_t>(path_end);
    result.path_scale_x = static_cast<std::uint8_t>(path_scale_x);
    result.path_scale_y = static_cast<std::uint8_t>(path_scale_y);
    result.path_shear_x = static_cast<std::uint8_t>(path_shear_x);
    result.path_shear_y = static_cast<std::uint8_t>(path_shear_y);
    result.path_twist = static_cast<std::uint8_t>(path_twist);
    result.path_twist_begin = static_cast<std::uint8_t>(path_twist_begin);
    result.path_radius_offset = static_cast<std::uint8_t>(path_radius_offset);
    result.path_taper_x = static_cast<std::uint8_t>(path_taper_x);
    result.path_taper_y = static_cast<std::uint8_t>(path_taper_y);
    result.path_revolutions = static_cast<std::uint8_t>(path_revolutions);
    result.path_skew = static_cast<std::uint8_t>(path_skew);
    result.profile_begin = static_cast<std::uint16_t>(profile_begin);
    result.profile_end = static_cast<std::uint16_t>(profile_end);
    result.profile_hollow = static_cast<std::uint16_t>(profile_hollow);
    result.physical = *physical;
    result.phantom = *phantom;
    result.base_permissions = static_cast<std::uint32_t>(base_permissions);
    result.owner_permissions = static_cast<std::uint32_t>(owner_permissions);
    result.group_permissions = static_cast<std::uint32_t>(group_permissions);
    result.everyone_permissions = static_cast<std::uint32_t>(everyone_permissions);
    result.next_owner_permissions = static_cast<std::uint32_t>(next_owner_permissions);
    result.task_inventory_serial = task_inventory_serial;
    result.task_inventory = *task_inventory;
    result.local_position = local_position.value_or(scene::Vector3{});
    result.sit_target_position = sit_target.value_or(scene::Vector3{});
    result.sit_target_rotation = sit_target_rotation.value_or(scene::Vector3{});
    result.local_rotation = result.rotation;
    if (const auto sculpt_id = string_after(text, R"("sculptId":")")) {
        const auto sculpt_type = integer_after<unsigned>(text, R"("sculptType":)");
        if (!sculpt_type || *sculpt_type > 255 || sculpt_id->size() > 128) return std::nullopt;
        result.sculpt_id = *sculpt_id;
        result.sculpt_type = static_cast<std::uint8_t>(*sculpt_type);
    }
    if (const auto list = text.find(R"("faceMaterials":[)"); list != std::string_view::npos) {
        auto rest = text.substr(list + 17);
        const auto end = rest.find(']');
        if (end != std::string_view::npos) rest = rest.substr(0, end);
        while (true) {
            const auto entry = rest.find(R"("face":)");
            if (entry == std::string_view::npos) break;
            const auto face = integer_after<unsigned>(rest.substr(entry), R"("face":)");
            const auto id = string_after(rest.substr(entry), R"("id":")");
            if (!face || !id || *face > 255 || id->size() > 128) return std::nullopt;
            // Fourteen is the viewer's own cap on the wire; a file naming more
            // is malformed rather than merely generous.
            if (result.face_materials.size() >= 14) return std::nullopt;
            result.face_materials.emplace_back(static_cast<std::uint8_t>(*face), *id);
            const auto next = rest.find('}', entry);
            if (next == std::string_view::npos) break;
            rest = rest.substr(next + 1);
        }
    }
    return result;
}

std::optional<LinksetAsset> parse_linkset_asset(std::span<const std::byte> content) {
    const std::string_view text(reinterpret_cast<const char*>(content.data()), content.size());
    if (text.find(R"("format":"homeworldz-linkset-v1")") == std::string_view::npos) {
        const auto root = parse_object_asset(content);
        if (!root) return std::nullopt;
        return LinksetAsset{*root, {}};
    }
    const auto parts = text.find(R"("parts":[)");
    if (parts == std::string_view::npos) return std::nullopt;
    std::vector<ObjectAsset> parsed;
    std::size_t position = parts + 9;
    while (position < text.size()) {
        while (position < text.size() && (text[position] == ' ' || text[position] == ',')) ++position;
        if (position < text.size() && text[position] == ']') break;
        if (position >= text.size() || text[position] != '{' || parsed.size() >= 256) return std::nullopt;
        const auto start = position;
        std::size_t depth = 0;
        bool quoted = false;
        bool escaped = false;
        for (; position < text.size(); ++position) {
            const auto character = text[position];
            if (quoted) {
                if (escaped) escaped = false;
                else if (character == '\\') escaped = true;
                else if (character == '"') quoted = false;
                continue;
            }
            if (character == '"') quoted = true;
            else if (character == '{') ++depth;
            else if (character == '}' && --depth == 0) {
                ++position;
                break;
            }
        }
        if (depth != 0) return std::nullopt;
        const auto part = text.substr(start, position - start);
        const auto asset = parse_object_asset(std::span(
            reinterpret_cast<const std::byte*>(part.data()), part.size()));
        if (!asset) return std::nullopt;
        parsed.push_back(*asset);
    }
    if (parsed.empty()) return std::nullopt;
    LinksetAsset result{std::move(parsed.front()), {}};
    result.children.assign(
        std::make_move_iterator(parsed.begin() + 1), std::make_move_iterator(parsed.end()));
    return result;
}

std::string serialize_linkset_asset(
    const scene::Entity& root, std::span<const scene::Entity* const> children) {
    if (children.empty()) return object_json(root, false);
    std::string result{"{\"format\":\"homeworldz-linkset-v1\",\"parts\":["};
    result += object_json(root, false);
    for (const auto* child : children) {
        if (!child || child->parent_id != root.id) continue;
        result += ',';
        result += object_json(*child, true);
    }
    return result + "]}";
}

} // namespace homeworldz::asset
