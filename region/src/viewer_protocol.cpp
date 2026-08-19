#include "homeworldz/viewer_protocol.h"

#include "homeworldz/terrain_layers.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>

namespace homeworldz::viewer {
namespace {

constexpr std::array<std::byte, 4> use_circuit_code_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x03}};
constexpr std::array<std::byte, 4> teleport_location_request_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x3f}};
constexpr std::array<std::byte, 4> teleport_local_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x40}};
constexpr std::array<std::byte, 4> teleport_landmark_request_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x41}};
constexpr std::array<std::byte, 4> set_start_location_request_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x01}, std::byte{0x44}};
constexpr std::array<std::byte, 4> activate_gestures_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x01}, std::byte{0x3c}};
constexpr std::array<std::byte, 4> deactivate_gestures_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x01}, std::byte{0x3d}};
constexpr std::array<std::byte, 4> teleport_start_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x49}};
constexpr std::array<std::byte, 4> teleport_failed_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x4a}};
constexpr std::array<std::byte, 4> packet_ack_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xfb}};
constexpr std::array<std::byte, 4> region_handshake_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x94}};
constexpr std::array<std::byte, 4> region_handshake_reply_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x95}};
constexpr std::array<std::byte, 4> complete_agent_movement_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0xf9}};
constexpr std::array<std::byte, 4> agent_movement_complete_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0xfa}};
constexpr std::array<std::byte, 4> chat_from_viewer_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x50}};
constexpr std::array<std::byte, 4> modify_land_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x7c}};
constexpr std::array<std::byte, 4> request_region_info_id{ // Low 141
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x8d}};
constexpr std::array<std::byte, 4> region_info_id{ // Low 142
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x8e}};
constexpr std::array<std::byte, 4> agent_alert_message_id{ // Low 135
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x87}};
constexpr std::array<std::byte, 4> estate_covenant_request_id{ // Low 203
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0xcb}};
constexpr std::array<std::byte, 4> estate_covenant_reply_id{ // Low 204
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0xcc}};
constexpr std::array<std::byte, 4> estate_owner_message_id{ // Low 260 (0x0104)
    std::byte{0xff}, std::byte{0xff}, std::byte{0x01}, std::byte{0x04}};
constexpr std::array<std::byte, 4> parcel_object_owners_request_id{ // Low 56
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x38}};
constexpr std::array<std::byte, 4> parcel_object_owners_reply_id{ // Low 57
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x39}};
constexpr std::array<std::byte, 4> parcel_return_objects_id{ // Low 199
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0xc7}};
constexpr std::array<std::byte, 4> parcel_select_objects_id{ // Low 202
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0xca}};
constexpr std::array<std::byte, 4> force_object_select_id{ // Low 205
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0xcd}};
constexpr std::array<std::byte, 4> parcel_overlay_id{ // Low 196
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0xc4}};
constexpr std::array<std::byte, 2> parcel_properties_request_id{ // Medium 11
    std::byte{0xff}, std::byte{0x0b}};
constexpr std::array<std::byte, 4> parcel_properties_request_by_id_id{ // Low 197
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0xc5}};
constexpr std::array<std::byte, 4> parcel_properties_update_id{ // Low 198
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0xc6}};
constexpr std::array<std::byte, 4> parcel_join_id{ // Low 210
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0xd2}};
constexpr std::array<std::byte, 4> parcel_divide_id{ // Low 211
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0xd3}};
constexpr std::array<std::byte, 4> parcel_access_list_request_id{ // Low 215
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0xd7}};
constexpr std::array<std::byte, 4> parcel_access_list_reply_id{ // Low 216
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0xd8}};
constexpr std::array<std::byte, 4> parcel_access_list_update_id{ // Low 217
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0xd9}};
constexpr std::array<std::byte, 4> chat_from_simulator_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x8b}};
constexpr std::array<std::byte, 4> logout_request_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0xfc}};
constexpr std::array<std::byte, 4> logout_reply_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0xfd}};
constexpr std::array<std::byte, 4> kick_user_id{  // KickUser, Low 163 (0xA3)
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0xa3}};
constexpr std::array<std::byte, 4> agent_cached_texture_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x01}, std::byte{0x80}};
constexpr std::array<std::byte, 4> agent_cached_texture_response_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x01}, std::byte{0x81}};
constexpr std::array<std::byte, 4> agent_set_appearance_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x54}};
constexpr std::array<std::byte, 4> create_inventory_folder_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x01}, std::byte{0x11}};
constexpr std::array<std::byte, 4> create_inventory_item_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x01}, std::byte{0x31}};
constexpr std::array<std::byte, 4> copy_inventory_item_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x01}, std::byte{0x0d}};
constexpr std::array<std::byte, 4> update_create_inventory_item_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x01}, std::byte{0x0b}};
constexpr std::array<std::byte, 4> move_inventory_folder_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x01}, std::byte{0x13}};
constexpr std::array<std::byte, 4> move_inventory_item_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x01}, std::byte{0x0c}};
constexpr std::array<std::byte, 4> request_task_inventory_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x01}, std::byte{0x21}};
constexpr std::array<std::byte, 4> reply_task_inventory_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x01}, std::byte{0x22}};
constexpr std::array<std::byte, 4> update_task_inventory_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x01}, std::byte{0x1e}};
constexpr std::array<std::byte, 4> rez_script_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x01}, std::byte{0x30}};
constexpr std::array<std::byte, 4> remove_task_inventory_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x01}, std::byte{0x1f}};
constexpr std::array<std::byte, 4> move_task_inventory_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x01}, std::byte{0x20}};
constexpr std::array<std::byte, 4> request_xfer_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x9c}};
constexpr std::array<std::byte, 4> transfer_request_id{ // Low 153
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x99}};
constexpr std::array<std::byte, 4> transfer_info_id{ // Low 154
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x9a}};
constexpr std::byte transfer_packet_id{17}; // High 17
constexpr std::array<std::byte, 2> object_add_id{std::byte{0xff}, std::byte{0x01}};
constexpr std::array<std::byte, 4> derez_object_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x01}, std::byte{0x23}};
constexpr std::array<std::byte, 4> rez_object_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x01}, std::byte{0x25}};
constexpr std::array<std::byte, 4> object_select_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x6e}};
constexpr std::array<std::byte, 4> object_deselect_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x6f}};
constexpr std::array<std::byte, 4> object_link_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x73}};
constexpr std::array<std::byte, 4> object_delink_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x74}};
constexpr std::array<std::byte, 4> object_grab_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x75}};
constexpr std::array<std::byte, 4> object_grab_update_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x76}};
constexpr std::array<std::byte, 2> multiple_object_update_id{
    std::byte{0xff}, std::byte{0x02}};
constexpr std::array<std::byte, 4> object_name_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x6b}};
constexpr std::array<std::byte, 4> object_description_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x6c}};
constexpr std::array<std::byte, 4> object_permissions_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x69}};
constexpr std::array<std::byte, 4> object_duplicate_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x5a}};
constexpr std::array<std::byte, 4> object_material_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x61}};
constexpr std::array<std::byte, 4> object_shape_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x62}};
constexpr std::array<std::byte, 4> object_image_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x60}};
constexpr std::array<std::byte, 4> object_flag_update_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x5e}};
constexpr std::array<std::byte, 2> request_object_properties_family_id{
    std::byte{0xff}, std::byte{0x05}};
constexpr std::array<std::byte, 4> uuid_name_request_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0xeb}};
constexpr std::array<std::byte, 4> uuid_name_reply_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0xec}};
constexpr std::array<std::byte, 4> map_block_request_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x01}, std::byte{0x97}};
constexpr std::array<std::byte, 4> map_name_request_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x01}, std::byte{0x98}};
constexpr std::array<std::byte, 4> map_block_reply_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x01}, std::byte{0x99}};
constexpr std::array<std::byte, 4> economy_data_request_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x18}};
constexpr std::array<std::byte, 4> economy_data_id{
    std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x19}};

class BitWriter {
public:
    void write(std::uint32_t value, unsigned bits) {
        for (unsigned remaining = bits; remaining > 0; --remaining) {
            current_ = static_cast<std::uint8_t>((current_ << 1) | ((value >> (remaining - 1)) & 1));
            if (++used_ == 8) flush_byte();
        }
    }
    void write_byte(std::uint8_t value) { write(value, 8); }
    std::vector<std::byte> finish() {
        if (used_ != 0) {
            current_ <<= (8 - used_);
            flush_byte();
        }
        return std::move(bytes_);
    }
private:
    void flush_byte() {
        bytes_.push_back(static_cast<std::byte>(current_));
        current_ = 0;
        used_ = 0;
    }
    std::vector<std::byte> bytes_;
    std::uint8_t current_{};
    unsigned used_{};
};

void write_ll_bits(BitWriter& output, std::uint32_t value, unsigned count) {
    while (count >= 8) {
        output.write_byte(static_cast<std::uint8_t>(value));
        value >>= 8;
        count -= 8;
    }
    if (count != 0) output.write(value, count);
}

struct TerrainPatchHeader {
    float dc_offset{};
    int range{};
    std::uint8_t quant_wbits{136};
    std::uint32_t patch_id{};
};

// Terrain compression follows OpenMetaverse TerrainCompressor's compatible
// DCT, quantization, zig-zag, and bit-packing algorithm. See the retained BSD
// notice in docs/third-party/openmetaverse-terrain.md.

const std::array<float, 256>& terrain_cosines() {
    static const auto table = [] {
        std::array<float, 256> result{};
        constexpr float pi = 3.14159265358979323846F;
        for (int u = 0; u < 16; ++u)
            for (int n = 0; n < 16; ++n)
                result[u * 16 + n] = std::cos((2.0F * n + 1.0F) * u * pi * 0.5F / 16.0F);
        return result;
    }();
    return table;
}

const std::array<int, 256>& terrain_copy_matrix() {
    static const auto table = [] {
        std::array<int, 256> result{};
        bool diagonal = false;
        bool right = true;
        int x = 0;
        int y = 0;
        int count = 0;
        while (x < 16 && y < 16) {
            result[y * 16 + x] = count++;
            if (!diagonal) {
                if (right) {
                    if (x < 15) ++x; else ++y;
                    right = false;
                } else {
                    if (y < 15) ++y; else ++x;
                    right = true;
                }
                diagonal = true;
            } else if (right) {
                ++x;
                --y;
                if (x == 15 || y == 0) diagonal = false;
            } else {
                --x;
                ++y;
                if (y == 15 || x == 0) diagonal = false;
            }
        }
        return result;
    }();
    return table;
}

// The patch's samples are read from a square window of a possibly larger
// heightmap: `stride` is the heightmap's full row width and (window_x,
// window_y) is the window's origin within it (ADR 0036). The patch id written
// to the wire stays window-relative. A whole-map encode is the degenerate case
// window (0, 0) with stride equal to the map width.
std::array<int, 256> compress_terrain_patch(std::span<const float> heightmap,
                                            std::uint8_t patch_x, std::uint8_t patch_y,
                                            std::size_t stride, std::size_t window_x,
                                            std::size_t window_y, bool extended,
                                            TerrainPatchHeader& header) {
    const auto base = (window_y + static_cast<std::size_t>(patch_y) * 16) * stride +
                      window_x + static_cast<std::size_t>(patch_x) * 16;
    float minimum = std::numeric_limits<float>::max();
    float maximum = std::numeric_limits<float>::lowest();
    for (std::size_t y = 0; y < 16; ++y) {
        for (std::size_t x = 0; x < 16; ++x) {
            const auto value = heightmap[base + y * stride + x];
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
        }
    }
    header.dc_offset = minimum;
    header.range = static_cast<int>(maximum - minimum + 1.0F);
    header.patch_id = extended ?
        (static_cast<std::uint32_t>(patch_x) << 16) | patch_y :
        (static_cast<std::uint32_t>(patch_x) << 5) | patch_y;

    const auto premultiply = 1024.0F / static_cast<float>(header.range);
    const auto subtract = 512.0F + header.dc_offset * premultiply;
    std::array<float, 256> block{};
    std::size_t index = 0;
    for (std::size_t y = 0; y < 16; ++y)
        for (std::size_t x = 0; x < 16; ++x)
            block[index++] = heightmap[base + y * stride + x] * premultiply - subtract;

    constexpr float inverse_sqrt_two = 0.7071067811865475244F;
    const auto& cosine = terrain_cosines();
    std::array<float, 256> intermediate{};
    for (int line = 0; line < 16; ++line) {
        const auto offset = line * 16;
        float total = 0.0F;
        for (int n = 0; n < 16; ++n) total += block[offset + n];
        intermediate[offset] = inverse_sqrt_two * total;
        for (int u = 1; u < 16; ++u) {
            total = 0.0F;
            for (int n = 0; n < 16; ++n)
                total += block[offset + n] * cosine[u * 16 + n];
            intermediate[offset + u] = total;
        }
    }

    const auto& copy = terrain_copy_matrix();
    std::array<int, 256> compressed{};
    for (int column = 0; column < 16; ++column) {
        float total = 0.0F;
        for (int n = 0; n < 16; ++n) total += intermediate[n * 16 + column];
        compressed[copy[column]] = static_cast<int>(inverse_sqrt_two * total * 0.125F /
                                                     (1.0F + 2.0F * column));
        for (int u = 1; u < 16; ++u) {
            total = 0.0F;
            for (int n = 0; n < 16; ++n)
                total += intermediate[n * 16 + column] * cosine[u * 16 + n];
            compressed[copy[u * 16 + column]] = static_cast<int>(total * 0.125F /
                                                                  (1.0F + 2.0F * (u + column)));
        }
    }
    return compressed;
}

unsigned write_terrain_patch_header(BitWriter& output, TerrainPatchHeader header,
                                    const std::array<int, 256>& coefficients, bool extended = false) {
    unsigned word_bits = ((header.quant_wbits & 0x0f) + 2) >> 1;
    const auto maximum_bits = static_cast<unsigned>((header.quant_wbits & 0x0f) + 7);
    for (auto value : coefficients) {
        auto magnitude = static_cast<unsigned>(value < 0 ? -static_cast<std::int64_t>(value) : value);
        for (auto bit = maximum_bits; bit > word_bits; --bit) {
            if ((magnitude & (1U << bit)) != 0) {
                word_bits = bit;
                break;
            }
        }
    }
    ++word_bits;
    word_bits = std::clamp(word_bits, 2U, 17U);
    header.quant_wbits = static_cast<std::uint8_t>((header.quant_wbits & 0xf0) | (word_bits - 2));
    output.write_byte(header.quant_wbits);
    std::uint32_t offset_bits{};
    std::memcpy(&offset_bits, &header.dc_offset, sizeof(offset_bits));
    write_ll_bits(output, offset_bits, 32);
    write_ll_bits(output, static_cast<std::uint32_t>(header.range), 16);
    write_ll_bits(output, header.patch_id, extended ? 32 : 10);
    return word_bits;
}

void write_terrain_coefficients(BitWriter& output, const std::array<int, 256>& coefficients,
                                unsigned word_bits) {
    for (std::size_t index = 0; index < coefficients.size(); ++index) {
        auto value = coefficients[index];
        if (value == 0) {
            if (std::all_of(coefficients.begin() + static_cast<std::ptrdiff_t>(index), coefficients.end(),
                            [](int remaining) { return remaining == 0; })) {
                output.write(2, 2);
                return;
            }
            output.write(0, 1);
            continue;
        }
        output.write(value < 0 ? 7 : 6, 3);
        auto magnitude = static_cast<std::uint32_t>(value < 0 ? -static_cast<std::int64_t>(value) : value);
        magnitude = std::min(magnitude, (1U << word_bits) - 1U);
        write_ll_bits(output, magnitude, word_bits);
    }
}

std::uint32_t read_be_u32(std::span<const std::byte> data, std::size_t offset) {
    return (std::to_integer<std::uint32_t>(data[offset]) << 24) |
           (std::to_integer<std::uint32_t>(data[offset + 1]) << 16) |
           (std::to_integer<std::uint32_t>(data[offset + 2]) << 8) |
           std::to_integer<std::uint32_t>(data[offset + 3]);
}

void append_be_u32(std::vector<std::byte>& output, std::uint32_t value) {
    output.push_back(static_cast<std::byte>((value >> 24) & 0xff));
    output.push_back(static_cast<std::byte>((value >> 16) & 0xff));
    output.push_back(static_cast<std::byte>((value >> 8) & 0xff));
    output.push_back(static_cast<std::byte>(value & 0xff));
}

std::uint32_t read_le_u32(std::span<const std::byte> data, std::size_t offset) {
    return std::to_integer<std::uint32_t>(data[offset]) |
           (std::to_integer<std::uint32_t>(data[offset + 1]) << 8) |
           (std::to_integer<std::uint32_t>(data[offset + 2]) << 16) |
           (std::to_integer<std::uint32_t>(data[offset + 3]) << 24);
}

std::uint16_t read_le_u16(std::span<const std::byte> data, std::size_t offset) {
    return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(data[offset]) |
        (std::to_integer<std::uint16_t>(data[offset + 1]) << 8));
}

std::uint64_t read_le_u64(std::span<const std::byte> data, std::size_t offset) {
    return static_cast<std::uint64_t>(read_le_u32(data, offset)) |
           (static_cast<std::uint64_t>(read_le_u32(data, offset + 4)) << 32);
}

float read_f32(std::span<const std::byte> data, std::size_t offset) {
    const auto bits = read_le_u32(data, offset);
    float result{};
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

std::array<float, 3> read_vector3(std::span<const std::byte> data, std::size_t offset) {
    return {read_f32(data, offset), read_f32(data, offset + 4), read_f32(data, offset + 8)};
}

std::optional<std::pair<std::string, std::size_t>> read_variable2(
    std::span<const std::byte> data, std::size_t offset) {
    if (offset + 2 > data.size()) return std::nullopt;
    const auto size = std::to_integer<std::size_t>(data[offset]) |
                      (std::to_integer<std::size_t>(data[offset + 1]) << 8);
    if (size == 0 || offset + 2 + size > data.size() || data[offset + 1 + size] != std::byte{})
        return std::nullopt;
    const auto begin = reinterpret_cast<const char*>(data.data() + offset + 2);
    return std::pair{std::string(begin, size - 1), offset + 2 + size};
}

// Variable-1 string: 1-byte length prefix (including trailing NUL), NUL-terminated.
// A zero length is valid and yields an empty string.
std::optional<std::pair<std::string, std::size_t>> read_variable1(
    std::span<const std::byte> data, std::size_t offset) {
    if (offset + 1 > data.size()) return std::nullopt;
    const auto size = std::to_integer<std::size_t>(data[offset]);
    if (offset + 1 + size > data.size()) return std::nullopt;
    if (size == 0) return std::pair{std::string{}, offset + 1};
    if (data[offset + size] != std::byte{}) return std::nullopt;
    const auto begin = reinterpret_cast<const char*>(data.data() + offset + 1);
    return std::pair{std::string(begin, size - 1), offset + 1 + size};
}

Uuid read_uuid(std::span<const std::byte> data, std::size_t offset) {
    Uuid value{};
    if (offset + 16 <= data.size()) std::copy_n(data.begin() + offset, 16, value.begin());
    return value;
}

void append_le_u32(std::vector<std::byte>& output, std::uint32_t value) {
    output.push_back(static_cast<std::byte>(value & 0xff));
    output.push_back(static_cast<std::byte>((value >> 8) & 0xff));
    output.push_back(static_cast<std::byte>((value >> 16) & 0xff));
    output.push_back(static_cast<std::byte>((value >> 24) & 0xff));
}

void append_le_u16(std::vector<std::byte>& output, std::uint16_t value) {
    output.push_back(static_cast<std::byte>(value));
    output.push_back(static_cast<std::byte>(value >> 8));
}

void append_le_u64(std::vector<std::byte>& output, std::uint64_t value) {
    append_le_u32(output, static_cast<std::uint32_t>(value));
    append_le_u32(output, static_cast<std::uint32_t>(value >> 32));
}

void append_f32(std::vector<std::byte>& output, float value) {
    static_assert(sizeof(float) == sizeof(std::uint32_t));
    std::uint32_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    append_le_u32(output, bits);
}

void append_uuid(std::vector<std::byte>& output, const Uuid& value) {
    output.insert(output.end(), value.begin(), value.end());
}

bool append_variable1(std::vector<std::byte>& output, std::string_view value) {
    if (value.size() > 254) return false;
    output.push_back(static_cast<std::byte>(value.size() + 1));
    output.insert(output.end(), reinterpret_cast<const std::byte*>(value.data()),
                  reinterpret_cast<const std::byte*>(value.data() + value.size()));
    output.push_back(std::byte{});
    return true;
}

bool append_variable2(std::vector<std::byte>& output, std::string_view value) {
    if (value.size() > 65534) return false;
    const auto size = static_cast<std::uint16_t>(value.size() + 1);
    output.push_back(static_cast<std::byte>(size & 0xff));
    output.push_back(static_cast<std::byte>((size >> 8) & 0xff));
    output.insert(output.end(), reinterpret_cast<const std::byte*>(value.data()),
                  reinterpret_cast<const std::byte*>(value.data() + value.size()));
    output.push_back(std::byte{});
    return true;
}

bool append_binary(std::vector<std::byte>& output, std::span<const std::byte> value, unsigned length_bytes) {
    if ((length_bytes == 1 && value.size() > 255) || value.size() > 65535) return false;
    output.push_back(static_cast<std::byte>(value.size()));
    if (length_bytes == 2) output.push_back(static_cast<std::byte>(value.size() >> 8));
    output.insert(output.end(), value.begin(), value.end());
    return true;
}

std::vector<std::byte> zero_encode(std::span<const std::byte> input) {
    std::vector<std::byte> output;
    output.reserve(input.size());
    for (std::size_t index = 0; index < input.size();) {
        if (input[index] != std::byte{}) {
            output.push_back(input[index++]);
            continue;
        }
        std::size_t count = 0;
        while (index < input.size() && input[index] == std::byte{} && count < 255) {
            ++index;
            ++count;
        }
        output.push_back(std::byte{});
        output.push_back(static_cast<std::byte>(count));
    }
    return output;
}

std::optional<std::vector<std::byte>> zero_decode(std::span<const std::byte> input) {
    std::vector<std::byte> output;
    for (std::size_t index = 0; index < input.size(); ++index) {
        if (input[index] != std::byte{}) {
            output.push_back(input[index]);
            continue;
        }
        if (++index >= input.size()) return std::nullopt;
        const auto count = std::to_integer<unsigned>(input[index]);
        if (count == 0 || output.size() > 65535 - count) return std::nullopt;
        output.insert(output.end(), count, std::byte{});
    }
    return output;
}

} // namespace

std::optional<Uuid> parse_uuid(std::string_view text) {
    if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-')
        return std::nullopt;
    Uuid result;
    std::size_t input = 0;
    for (auto& byte : result) {
        if (input == 8 || input == 13 || input == 18 || input == 23) ++input;
        unsigned value{};
        const auto parsed = std::from_chars(text.data() + input, text.data() + input + 2, value, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + input + 2) return std::nullopt;
        byte = static_cast<std::byte>(value);
        input += 2;
    }
    return result;
}

std::string format_uuid(const Uuid& value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(36);
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) result.push_back('-');
        const auto byte = std::to_integer<unsigned>(value[index]);
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

Uuid combine_uuids(const Uuid& first, const Uuid& second) {
    constexpr std::array<std::uint32_t, 64> constants{
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    constexpr std::array<unsigned, 64> shifts{
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    std::array<std::byte, 64> block{};
    std::copy(first.begin(), first.end(), block.begin());
    std::copy(second.begin(), second.end(), block.begin() + 16);
    block[32] = std::byte{0x80};
    block[57] = std::byte{0x01}; // 32 bytes = 256 bits, little endian
    std::array<std::uint32_t, 16> words{};
    for (std::size_t index = 0; index < words.size(); ++index) {
        const auto offset = index * 4;
        words[index] = std::to_integer<std::uint32_t>(block[offset]) |
                       (std::to_integer<std::uint32_t>(block[offset + 1]) << 8) |
                       (std::to_integer<std::uint32_t>(block[offset + 2]) << 16) |
                       (std::to_integer<std::uint32_t>(block[offset + 3]) << 24);
    }
    std::uint32_t a = 0x67452301;
    std::uint32_t b = 0xefcdab89;
    std::uint32_t c = 0x98badcfe;
    std::uint32_t d = 0x10325476;
    const auto initial_a = a;
    const auto initial_b = b;
    const auto initial_c = c;
    const auto initial_d = d;
    for (std::uint32_t index = 0; index < 64; ++index) {
        std::uint32_t function{};
        std::uint32_t word{};
        if (index < 16) {
            function = (b & c) | (~b & d);
            word = index;
        } else if (index < 32) {
            function = (d & b) | (~d & c);
            word = (5 * index + 1) % 16;
        } else if (index < 48) {
            function = b ^ c ^ d;
            word = (3 * index + 5) % 16;
        } else {
            function = c ^ (b | ~d);
            word = (7 * index) % 16;
        }
        const auto next_d = d;
        d = c;
        c = b;
        b += std::rotl(a + function + constants[index] + words[word], shifts[index]);
        a = next_d;
    }
    const std::array<std::uint32_t, 4> digest{
        initial_a + a, initial_b + b, initial_c + c, initial_d + d};
    Uuid result{};
    for (std::size_t index = 0; index < digest.size(); ++index)
        for (std::size_t byte = 0; byte < 4; ++byte)
            result[index * 4 + byte] = static_cast<std::byte>(digest[index] >> (byte * 8));
    return result;
}

std::vector<std::byte> encode_use_circuit_code(const UseCircuitCode& message) {
    std::vector<std::byte> output(use_circuit_code_id.begin(), use_circuit_code_id.end());
    output.reserve(40);
    append_le_u32(output, message.circuit_code);
    output.insert(output.end(), message.session_id.begin(), message.session_id.end());
    output.insert(output.end(), message.agent_id.begin(), message.agent_id.end());
    return output;
}

std::optional<UseCircuitCode> decode_use_circuit_code(std::span<const std::byte> payload) {
    if (payload.size() != 40 || !std::equal(use_circuit_code_id.begin(), use_circuit_code_id.end(), payload.begin()))
        return std::nullopt;
    UseCircuitCode message;
    message.circuit_code = read_le_u32(payload, 4);
    std::copy_n(payload.begin() + 8, 16, message.session_id.begin());
    std::copy_n(payload.begin() + 24, 16, message.agent_id.begin());
    return message;
}

std::optional<TeleportLocationRequest> decode_teleport_location_request(
    std::span<const std::byte> payload) {
    if (payload.size() != 68 ||
        !std::equal(teleport_location_request_id.begin(), teleport_location_request_id.end(), payload.begin()))
        return std::nullopt;
    TeleportLocationRequest result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    result.region_handle = read_le_u64(payload, 36);
    result.position = read_vector3(payload, 44);
    result.look_at = read_vector3(payload, 56);
    const auto finite = [](const std::array<float, 3>& value) {
        return std::all_of(value.begin(), value.end(), [](float component) { return std::isfinite(component); });
    };
    return finite(result.position) && finite(result.look_at)
        ? std::optional<TeleportLocationRequest>{result} : std::nullopt;
}

std::optional<TeleportLandmarkRequest> decode_teleport_landmark_request(
    std::span<const std::byte> payload) {
    // Info block: AgentID, SessionID, LandmarkID (16 each) after the 4-byte id.
    if (payload.size() != 52 ||
        !std::equal(teleport_landmark_request_id.begin(), teleport_landmark_request_id.end(),
                    payload.begin()))
        return std::nullopt;
    TeleportLandmarkRequest result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    std::copy_n(payload.begin() + 36, 16, result.landmark_id.begin());
    return result;
}

std::optional<SetStartLocationRequest> decode_set_start_location_request(
    std::span<const std::byte> payload) {
    // AgentData: AgentID(16), SessionID(16); StartLocationData: SimName(Variable 1),
    // LocationID(U32), LocationPos(LLVector3), LocationLookAt(LLVector3).
    if (payload.size() < 4 + 16 + 16 + 1 ||
        !std::equal(set_start_location_request_id.begin(), set_start_location_request_id.end(),
                    payload.begin()))
        return std::nullopt;
    SetStartLocationRequest result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    std::size_t offset = 36;
    const auto name_len = std::to_integer<std::size_t>(payload[offset]);
    offset += 1 + name_len; // skip the SimName string
    if (payload.size() < offset + 4 + 12 + 12) return std::nullopt;
    result.location_id = read_le_u32(payload, offset);
    result.position = read_vector3(payload, offset + 4);
    result.look_at = read_vector3(payload, offset + 16);
    const auto finite = [](const std::array<float, 3>& value) {
        return std::all_of(value.begin(), value.end(),
                           [](float component) { return std::isfinite(component); });
    };
    return finite(result.position) ? std::optional<SetStartLocationRequest>{result} : std::nullopt;
}

std::optional<ActivateGestures> decode_activate_gestures(std::span<const std::byte> payload) {
    // AgentID(16), SessionID(16), Flags(U32); then a 1-byte Data count and that
    // many {ItemID(16), AssetID(16), GestureFlags(U32)} blocks.
    constexpr std::size_t header = 4 + 16 + 16 + 4;
    constexpr std::size_t block = 16 + 16 + 4;
    if (payload.size() < header + 1 ||
        !std::equal(activate_gestures_id.begin(), activate_gestures_id.end(), payload.begin()))
        return std::nullopt;
    ActivateGestures result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    const auto count = std::to_integer<std::size_t>(payload[header]);
    std::size_t offset = header + 1;
    if (payload.size() < offset + count * block) return std::nullopt;
    for (std::size_t index = 0; index < count; ++index) {
        GestureActivation gesture;
        std::copy_n(payload.begin() + offset, 16, gesture.item_id.begin());
        std::copy_n(payload.begin() + offset + 16, 16, gesture.asset_id.begin());
        result.gestures.push_back(gesture);
        offset += block;
    }
    return result;
}

std::optional<DeactivateGestures> decode_deactivate_gestures(std::span<const std::byte> payload) {
    // AgentID(16), SessionID(16), Flags(U32); then a 1-byte Data count and that
    // many {ItemID(16), GestureFlags(U32)} blocks.
    constexpr std::size_t header = 4 + 16 + 16 + 4;
    constexpr std::size_t block = 16 + 4;
    if (payload.size() < header + 1 ||
        !std::equal(deactivate_gestures_id.begin(), deactivate_gestures_id.end(), payload.begin()))
        return std::nullopt;
    DeactivateGestures result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    const auto count = std::to_integer<std::size_t>(payload[header]);
    std::size_t offset = header + 1;
    if (payload.size() < offset + count * block) return std::nullopt;
    for (std::size_t index = 0; index < count; ++index) {
        Uuid item_id{};
        std::copy_n(payload.begin() + offset, 16, item_id.begin());
        result.item_ids.push_back(item_id);
        offset += block;
    }
    return result;
}

std::vector<std::byte> encode_teleport_start(const TeleportStart& message) {
    std::vector<std::byte> output(teleport_start_id.begin(), teleport_start_id.end());
    append_le_u32(output, message.flags);
    return output;
}

std::vector<std::byte> encode_teleport_local(const TeleportLocal& message) {
    std::vector<std::byte> output(teleport_local_id.begin(), teleport_local_id.end());
    append_uuid(output, message.agent_id);
    append_le_u32(output, message.location_id);
    for (const auto value : message.position) append_f32(output, value);
    for (const auto value : message.look_at) append_f32(output, value);
    append_le_u32(output, message.teleport_flags);
    return output;
}

std::vector<std::byte> encode_teleport_failed(const TeleportFailed& message) {
    std::vector<std::byte> output(teleport_failed_id.begin(), teleport_failed_id.end());
    append_uuid(output, message.agent_id);
    if (!append_variable1(output, message.reason)) return {};
    output.push_back(std::byte{}); // no AlertInfo blocks
    return output;
}

std::vector<std::byte> encode_region_handshake(const RegionHandshake& message) {
    std::vector<std::byte> output(region_handshake_id.begin(), region_handshake_id.end());
    // Region-wide permission flags (indra llregionflags.h): estate/region deny
    // flags, voice, direct-teleport, fixed-sun, plus landmark/set-home which stay
    // on so those viewer menu items activate everywhere in the region.
    append_le_u32(output, message.region_flags);
    output.push_back(std::byte{13}); // PG access
    if (!append_variable1(output, message.name)) return {};
    append_uuid(output, message.owner_id);
    output.push_back(static_cast<std::byte>(message.is_estate_owner ? 1 : 0)); // IsEstateManager
    append_f32(output, message.water_height);
    append_f32(output, 1.0F); // billable factor
    Uuid zero{};
    append_uuid(output, zero); // cache ID
    for (const auto& texture : message.terrain_textures) append_uuid(output, texture); // terrain base
    for (const auto& texture : message.terrain_textures) append_uuid(output, texture); // terrain detail
    // Per-corner start height then height range. The field names in the message
    // template are TerrainStartHeightNN and TerrainHeightRangeNN, and that is what
    // they mean - the viewer's dialog calls them Low and High and its renderer does
    // not (terrain_layers.h).
    for (const float value : message.terrain_start) append_f32(output, value);
    for (const float value : message.terrain_range) append_f32(output, value);
    append_uuid(output, message.region_id);
    append_le_u32(output, 0); // CPU class
    append_le_u32(output, 1); // CPU ratio
    if (!append_variable1(output, "") || !append_variable1(output, "homeworldz") ||
        !append_variable1(output, "Homeworldz Region")) return {};
    output.push_back(std::byte{}); // no RegionInfo4 blocks
    return output;
}

std::optional<AgentMessage> decode_region_handshake_reply(std::span<const std::byte> payload) {
    if (payload.size() != 40 || !std::equal(region_handshake_reply_id.begin(), region_handshake_reply_id.end(), payload.begin()))
        return std::nullopt;
    AgentMessage result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    return result;
}

std::optional<CompleteAgentMovement> decode_complete_agent_movement(std::span<const std::byte> payload) {
    if (payload.size() != 40 || !std::equal(complete_agent_movement_id.begin(), complete_agent_movement_id.end(), payload.begin()))
        return std::nullopt;
    CompleteAgentMovement result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    result.circuit_code = read_le_u32(payload, 36);
    return result;
}

std::vector<std::byte> encode_agent_movement_complete(const AgentMovementComplete& message) {
    std::vector<std::byte> output(agent_movement_complete_id.begin(), agent_movement_complete_id.end());
    append_uuid(output, message.agent_id);
    append_uuid(output, message.session_id);
    for (const auto value : message.position) append_f32(output, value);
    for (const auto value : message.look_at) append_f32(output, value);
    append_le_u64(output, message.region_handle);
    append_le_u32(output, message.timestamp);
    if (!append_variable2(output, message.channel_version)) return {};
    return output;
}

std::vector<std::byte> encode_start_ping_check(std::uint8_t ping_id, std::uint32_t oldest_unacked) {
    std::vector<std::byte> output{std::byte{1}, static_cast<std::byte>(ping_id)};
    append_le_u32(output, oldest_unacked);
    return output;
}

std::optional<std::uint8_t> decode_start_ping_check(std::span<const std::byte> payload) {
    if (payload.size() != 6 || payload[0] != std::byte{1}) return std::nullopt;
    return std::to_integer<std::uint8_t>(payload[1]);
}

std::vector<std::byte> encode_complete_ping_check(std::uint8_t ping_id) {
    return {std::byte{2}, static_cast<std::byte>(ping_id)};
}

std::optional<std::uint8_t> decode_complete_ping_check(std::span<const std::byte> payload) {
    if (payload.size() != 2 || payload[0] != std::byte{2}) return std::nullopt;
    return std::to_integer<std::uint8_t>(payload[1]);
}

std::vector<std::byte> encode_disable_simulator() {
    // DisableSimulator, Low 152 (0x98): no body. Tells a viewer to drop its
    // connection to this simulator immediately instead of pinging a dead
    // circuit for thirty seconds — sent on a facet's old endpoint when an
    // internal crossing re-tags the circuit away from it (ADR 0036).
    constexpr std::array<std::byte, 4> disable_simulator_id{
        std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x98}};
    return {disable_simulator_id.begin(), disable_simulator_id.end()};
}

std::vector<std::byte> encode_kick_user(const Uuid& agent_id, const Uuid& session_id,
                                        std::string_view reason) {
    std::vector<std::byte> output(kick_user_id.begin(), kick_user_id.end());
    // TargetBlock: TargetIP (U32) + TargetPort (U16). The viewer only shows the
    // Reason, so the target address is irrelevant here — send zeros.
    append_le_u32(output, 0);
    output.push_back(std::byte{0});
    output.push_back(std::byte{0});
    // UserInfo: AgentID, SessionID, Reason (Variable, 2-byte length).
    append_uuid(output, agent_id);
    append_uuid(output, session_id);
    if (!append_variable2(output, reason)) return {};
    return output;
}

bool is_economy_data_request(std::span<const std::byte> payload) {
    return payload.size() == economy_data_request_id.size() &&
           std::equal(economy_data_request_id.begin(), economy_data_request_id.end(), payload.begin());
}

std::vector<std::byte> encode_economy_data(std::int32_t price_upload,
                                           std::int32_t object_capacity,
                                           std::int32_t object_count) {
    std::vector<std::byte> output(economy_data_id.begin(), economy_data_id.end());
    const auto integer = [&output](std::int32_t value) {
        append_le_u32(output, static_cast<std::uint32_t>(value));
    };
    integer(object_capacity);
    integer(object_count);
    integer(0); // price energy unit
    integer(0); // price object claim
    integer(0); // price public object decay
    integer(0); // price public object delete
    integer(0); // price parcel claim
    append_f32(output, 0.0F); // price parcel claim factor
    integer(price_upload);
    integer(0); // price rent light
    integer(0); // teleport minimum price
    append_f32(output, 0.0F); // teleport price exponent
    append_f32(output, 1.0F); // energy efficiency
    append_f32(output, 0.0F); // price object rent
    append_f32(output, 0.0F); // price object scale factor
    integer(0); // price parcel rent
    integer(0); // price group create
    return output;
}

std::optional<AgentMessage> decode_logout_request(std::span<const std::byte> payload) {
    if (payload.size() != 36 || !std::equal(logout_request_id.begin(), logout_request_id.end(), payload.begin()))
        return std::nullopt;
    AgentMessage result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    return result;
}

std::optional<CreateInventoryFolder> decode_create_inventory_folder(std::span<const std::byte> payload) {
    constexpr std::size_t fixed_size = 70;
    if (payload.size() < fixed_size ||
        !std::equal(create_inventory_folder_id.begin(), create_inventory_folder_id.end(), payload.begin()))
        return std::nullopt;
    const auto name_size = std::to_integer<std::size_t>(payload[69]);
    if (name_size == 0 || payload.size() != fixed_size + name_size) return std::nullopt;
    CreateInventoryFolder result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    std::copy_n(payload.begin() + 36, 16, result.folder_id.begin());
    std::copy_n(payload.begin() + 52, 16, result.parent_id.begin());
    result.type = static_cast<std::int8_t>(std::to_integer<std::uint8_t>(payload[68]));
    result.name.assign(reinterpret_cast<const char*>(payload.data() + fixed_size), name_size);
    while (!result.name.empty() && result.name.back() == '\0') result.name.pop_back();
    if (result.name.empty() || result.name.find('\0') != std::string::npos) return std::nullopt;
    return result;
}

std::optional<CreateInventoryItem> decode_create_inventory_item(std::span<const std::byte> payload) {
    constexpr std::size_t name_offset = 80;
    if (payload.size() < name_offset + 1 ||
        !std::equal(create_inventory_item_id.begin(), create_inventory_item_id.end(), payload.begin()))
        return std::nullopt;
    const auto name_size = std::to_integer<std::size_t>(payload[79]);
    if (payload.size() < name_offset + name_size + 1) return std::nullopt;
    const auto description_length_offset = name_offset + name_size;
    const auto description_size = std::to_integer<std::size_t>(payload[description_length_offset]);
    if (payload.size() != description_length_offset + 1 + description_size) return std::nullopt;

    CreateInventoryItem result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    result.callback_id = read_le_u32(payload, 36);
    std::copy_n(payload.begin() + 40, 16, result.folder_id.begin());
    std::copy_n(payload.begin() + 56, 16, result.transaction_id.begin());
    result.next_owner_permissions = read_le_u32(payload, 72);
    result.asset_type = static_cast<std::int8_t>(std::to_integer<std::uint8_t>(payload[76]));
    result.inventory_type = static_cast<std::int8_t>(std::to_integer<std::uint8_t>(payload[77]));
    result.wearable_type = std::to_integer<std::uint8_t>(payload[78]);
    result.name.assign(reinterpret_cast<const char*>(payload.data() + name_offset), name_size);
    result.description.assign(
        reinterpret_cast<const char*>(payload.data() + description_length_offset + 1), description_size);
    while (!result.name.empty() && result.name.back() == '\0') result.name.pop_back();
    while (!result.description.empty() && result.description.back() == '\0') result.description.pop_back();
    if (result.name.empty() || result.name.find('\0') != std::string::npos ||
        result.description.find('\0') != std::string::npos)
        return std::nullopt;
    return result;
}

std::optional<CopyInventoryItem> decode_copy_inventory_item(std::span<const std::byte> payload) {
    constexpr std::size_t fixed_size = 90;
    if (payload.size() < fixed_size ||
        !std::equal(copy_inventory_item_id.begin(), copy_inventory_item_id.end(), payload.begin()) ||
        payload[36] != std::byte{1})
        return std::nullopt;
    const auto name_size = std::to_integer<std::size_t>(payload[89]);
    if (payload.size() != fixed_size + name_size) return std::nullopt;
    CopyInventoryItem result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    result.callback_id = read_le_u32(payload, 37);
    std::copy_n(payload.begin() + 41, 16, result.old_agent_id.begin());
    std::copy_n(payload.begin() + 57, 16, result.old_item_id.begin());
    std::copy_n(payload.begin() + 73, 16, result.new_folder_id.begin());
    result.new_name.assign(reinterpret_cast<const char*>(payload.data() + fixed_size), name_size);
    while (!result.new_name.empty() && result.new_name.back() == '\0') result.new_name.pop_back();
    if (result.new_name.find('\0') != std::string::npos) return std::nullopt;
    return result;
}

std::optional<MoveInventoryFolder> decode_move_inventory_folder(std::span<const std::byte> payload) {
    constexpr std::size_t header_size = 38;
    constexpr std::size_t block_size = 32;
    if (payload.size() < header_size ||
        !std::equal(move_inventory_folder_id.begin(), move_inventory_folder_id.end(), payload.begin()))
        return std::nullopt;
    const auto count = std::to_integer<std::size_t>(payload[37]);
    if (count == 0 || payload.size() != header_size + count * block_size) return std::nullopt;
    MoveInventoryFolder result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    result.stamp = payload[36] != std::byte{};
    result.folders.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto offset = header_size + index * block_size;
        InventoryFolderMove move;
        std::copy_n(payload.begin() + offset, 16, move.folder_id.begin());
        std::copy_n(payload.begin() + offset + 16, 16, move.parent_id.begin());
        result.folders.push_back(move);
    }
    return result;
}

std::optional<MoveInventoryItem> decode_move_inventory_item(std::span<const std::byte> payload) {
    constexpr std::size_t header_size = 38;
    constexpr std::size_t fixed_block_size = 33;
    if (payload.size() < header_size ||
        !std::equal(move_inventory_item_id.begin(), move_inventory_item_id.end(), payload.begin()))
        return std::nullopt;
    const auto count = std::to_integer<std::size_t>(payload[37]);
    if (count == 0) return std::nullopt;
    MoveInventoryItem result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    result.stamp = payload[36] != std::byte{};
    result.items.reserve(count);
    std::size_t offset = header_size;
    for (std::size_t index = 0; index < count; ++index) {
        if (payload.size() < offset + fixed_block_size) return std::nullopt;
        InventoryItemMove move;
        std::copy_n(payload.begin() + offset, 16, move.item_id.begin());
        std::copy_n(payload.begin() + offset + 16, 16, move.folder_id.begin());
        const auto name_size = std::to_integer<std::size_t>(payload[offset + 32]);
        if (payload.size() < offset + fixed_block_size + name_size)
            return std::nullopt;
        move.new_name.assign(
            reinterpret_cast<const char*>(payload.data() + offset + fixed_block_size), name_size);
        while (!move.new_name.empty() && move.new_name.back() == '\0') move.new_name.pop_back();
        if (move.new_name.find('\0') != std::string::npos) return std::nullopt;
        result.items.push_back(std::move(move));
        offset += fixed_block_size + name_size;
    }
    if (offset != payload.size()) return std::nullopt;
    return result;
}

std::optional<RequestTaskInventory> decode_request_task_inventory(
    std::span<const std::byte> payload) {
    constexpr std::size_t message_size = 40;
    if (payload.size() != message_size ||
        !std::equal(request_task_inventory_id.begin(), request_task_inventory_id.end(), payload.begin()))
        return std::nullopt;
    RequestTaskInventory result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    result.local_id = read_le_u32(payload, 36);
    return result;
}

std::vector<std::byte> encode_reply_task_inventory(const ReplyTaskInventory& message) {
    if (message.filename.size() > 255) return {};
    std::vector<std::byte> output(reply_task_inventory_id.begin(), reply_task_inventory_id.end());
    append_uuid(output, message.task_id);
    append_le_u16(output, static_cast<std::uint16_t>(message.serial));
    output.push_back(static_cast<std::byte>(message.filename.size()));
    output.insert(output.end(), reinterpret_cast<const std::byte*>(message.filename.data()),
                  reinterpret_cast<const std::byte*>(message.filename.data() + message.filename.size()));
    return output;
}

std::optional<UpdateTaskInventory> decode_update_task_inventory(
    std::span<const std::byte> payload) {
    constexpr std::size_t name_length_offset = 169;
    if (payload.size() < name_length_offset + 1 ||
        !std::equal(update_task_inventory_id.begin(), update_task_inventory_id.end(), payload.begin()))
        return std::nullopt;
    auto position = name_length_offset;
    const auto name_size = std::to_integer<std::size_t>(payload[position++]);
    if (position + name_size + 1 > payload.size()) return std::nullopt;
    const auto name_position = position;
    position += name_size;
    const auto description_size = std::to_integer<std::size_t>(payload[position++]);
    if (position + description_size + 8 != payload.size()) return std::nullopt;
    const auto description_position = position;
    UpdateTaskInventory result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    result.local_id = read_le_u32(payload, 36);
    result.key = std::to_integer<std::uint8_t>(payload[40]);
    std::copy_n(payload.begin() + 41, 16, result.item_id.begin());
    std::copy_n(payload.begin() + 57, 16, result.folder_id.begin());
    std::copy_n(payload.begin() + 73, 16, result.creator_id.begin());
    std::copy_n(payload.begin() + 89, 16, result.owner_id.begin());
    std::copy_n(payload.begin() + 105, 16, result.group_id.begin());
    result.base_permissions = read_le_u32(payload, 121);
    result.owner_permissions = read_le_u32(payload, 125);
    result.group_permissions = read_le_u32(payload, 129);
    result.everyone_permissions = read_le_u32(payload, 133);
    result.next_owner_permissions = read_le_u32(payload, 137);
    result.group_owned = payload[141] != std::byte{};
    std::copy_n(payload.begin() + 142, 16, result.transaction_id.begin());
    result.asset_type = static_cast<std::int8_t>(std::to_integer<std::uint8_t>(payload[158]));
    result.inventory_type = static_cast<std::int8_t>(std::to_integer<std::uint8_t>(payload[159]));
    result.flags = read_le_u32(payload, 160);
    result.sale_type = std::to_integer<std::uint8_t>(payload[164]);
    result.sale_price = static_cast<std::int32_t>(read_le_u32(payload, 165));
    result.name.assign(reinterpret_cast<const char*>(payload.data() + name_position), name_size);
    if (!result.name.empty() && result.name.back() == '\0') result.name.pop_back();
    result.description.assign(
        reinterpret_cast<const char*>(payload.data() + description_position), description_size);
    if (!result.description.empty() && result.description.back() == '\0')
        result.description.pop_back();
    const auto trailing_position = description_position + description_size;
    result.creation_date = static_cast<std::int32_t>(read_le_u32(payload, trailing_position));
    result.crc = read_le_u32(payload, trailing_position + 4);
    return result;
}

std::optional<RezScript> decode_rez_script(std::span<const std::byte> payload) {
    constexpr std::size_t name_length_offset = 185;
    if (payload.size() < name_length_offset + 1 ||
        !std::equal(rez_script_id.begin(), rez_script_id.end(), payload.begin()))
        return std::nullopt;
    auto position = name_length_offset;
    const auto name_size = std::to_integer<std::size_t>(payload[position++]);
    if (position + name_size + 1 > payload.size()) return std::nullopt;
    const auto name_position = position;
    position += name_size;
    const auto description_size = std::to_integer<std::size_t>(payload[position++]);
    if (position + description_size + 8 != payload.size()) return std::nullopt;
    const auto description_position = position;
    RezScript result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    std::copy_n(payload.begin() + 36, 16, result.agent_group_id.begin());
    result.local_id = read_le_u32(payload, 52);
    result.enabled = payload[56] != std::byte{};
    std::copy_n(payload.begin() + 57, 16, result.item_id.begin());
    std::copy_n(payload.begin() + 73, 16, result.folder_id.begin());
    std::copy_n(payload.begin() + 89, 16, result.creator_id.begin());
    std::copy_n(payload.begin() + 105, 16, result.owner_id.begin());
    std::copy_n(payload.begin() + 121, 16, result.group_id.begin());
    result.base_permissions = read_le_u32(payload, 137);
    result.owner_permissions = read_le_u32(payload, 141);
    result.group_permissions = read_le_u32(payload, 145);
    result.everyone_permissions = read_le_u32(payload, 149);
    result.next_owner_permissions = read_le_u32(payload, 153);
    result.group_owned = payload[157] != std::byte{};
    std::copy_n(payload.begin() + 158, 16, result.transaction_id.begin());
    result.asset_type = static_cast<std::int8_t>(std::to_integer<std::uint8_t>(payload[174]));
    result.inventory_type = static_cast<std::int8_t>(std::to_integer<std::uint8_t>(payload[175]));
    result.flags = read_le_u32(payload, 176);
    result.sale_type = std::to_integer<std::uint8_t>(payload[180]);
    result.sale_price = static_cast<std::int32_t>(read_le_u32(payload, 181));
    result.name.assign(reinterpret_cast<const char*>(payload.data() + name_position), name_size);
    if (!result.name.empty() && result.name.back() == '\0') result.name.pop_back();
    result.description.assign(
        reinterpret_cast<const char*>(payload.data() + description_position), description_size);
    if (!result.description.empty() && result.description.back() == '\0')
        result.description.pop_back();
    const auto trailing_position = description_position + description_size;
    result.creation_date = static_cast<std::int32_t>(read_le_u32(payload, trailing_position));
    result.crc = read_le_u32(payload, trailing_position + 4);
    return result;
}

std::optional<RemoveTaskInventory> decode_remove_task_inventory(
    std::span<const std::byte> payload) {
    constexpr std::size_t message_size = 56;
    if (payload.size() != message_size ||
        !std::equal(remove_task_inventory_id.begin(), remove_task_inventory_id.end(), payload.begin()))
        return std::nullopt;
    RemoveTaskInventory result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    result.local_id = read_le_u32(payload, 36);
    std::copy_n(payload.begin() + 40, 16, result.item_id.begin());
    return result;
}

std::optional<MoveTaskInventory> decode_move_task_inventory(
    std::span<const std::byte> payload) {
    constexpr std::size_t message_size = 72;
    if (payload.size() != message_size ||
        !std::equal(move_task_inventory_id.begin(), move_task_inventory_id.end(), payload.begin()))
        return std::nullopt;
    MoveTaskInventory result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    std::copy_n(payload.begin() + 36, 16, result.folder_id.begin());
    result.local_id = read_le_u32(payload, 52);
    std::copy_n(payload.begin() + 56, 16, result.item_id.begin());
    return result;
}

std::optional<RequestXfer> decode_request_xfer(std::span<const std::byte> payload) {
    constexpr std::size_t filename_offset = 13;
    constexpr std::size_t trailing_size = 21;
    if (payload.size() < filename_offset + trailing_size ||
        !std::equal(request_xfer_id.begin(), request_xfer_id.end(), payload.begin()))
        return std::nullopt;
    const auto filename_size = std::to_integer<std::size_t>(payload[12]);
    if (payload.size() != filename_offset + filename_size + trailing_size) return std::nullopt;
    RequestXfer result;
    result.id = read_le_u32(payload, 4) |
        (static_cast<std::uint64_t>(read_le_u32(payload, 8)) << 32);
    result.filename.assign(
        reinterpret_cast<const char*>(payload.data() + filename_offset), filename_size);
    while (!result.filename.empty() && result.filename.back() == '\0') result.filename.pop_back();
    if (result.filename.empty() || result.filename.find('\0') != std::string::npos)
        return std::nullopt;
    return result;
}

std::vector<std::byte> encode_send_xfer_packet(
    std::uint64_t id, std::uint32_t packet, std::span<const std::byte> data) {
    if (data.size() > 65535) return {};
    std::vector<std::byte> output{std::byte{18}};
    append_le_u64(output, id);
    append_le_u32(output, packet);
    append_le_u16(output, static_cast<std::uint16_t>(data.size()));
    output.insert(output.end(), data.begin(), data.end());
    return output;
}

std::optional<AssetTransferRequest> decode_transfer_request(std::span<const std::byte> payload) {
    // id(4) + TransferID(16) + ChannelType(4) + SourceType(4) + Priority(4) then Params (Variable 2).
    constexpr std::size_t params_length_offset = 32;
    if (payload.size() < params_length_offset + 2 ||
        !std::equal(transfer_request_id.begin(), transfer_request_id.end(), payload.begin()))
        return std::nullopt;
    const auto params_length = read_le_u16(payload, params_length_offset);
    const std::size_t params_offset = params_length_offset + 2;
    if (payload.size() < params_offset + params_length) return std::nullopt;

    AssetTransferRequest result;
    std::copy_n(payload.begin() + 4, 16, result.transfer_id.begin());
    result.channel_type = static_cast<std::int32_t>(read_le_u32(payload, 20));
    result.source_type = static_cast<std::int32_t>(read_le_u32(payload, 24));
    result.params.assign(payload.begin() + params_offset,
                         payload.begin() + params_offset + params_length);
    const auto params = std::span<const std::byte>(result.params);

    if (result.source_type == transfer_source_sim_inv_item && params.size() >= 100) {
        std::copy_n(params.begin() + 0, 16, result.agent_id.begin());
        std::copy_n(params.begin() + 16, 16, result.session_id.begin());
        // owner id (32..48) and task id (48..64) are not needed for agent inventory.
        std::copy_n(params.begin() + 64, 16, result.item_id.begin());
        std::copy_n(params.begin() + 80, 16, result.asset_id.begin());
        result.asset_type = static_cast<std::int32_t>(read_le_u32(params, 96));
    } else if (result.source_type == transfer_source_asset && params.size() >= 20) {
        std::copy_n(params.begin() + 0, 16, result.asset_id.begin());
        result.asset_type = static_cast<std::int32_t>(read_le_u32(params, 16));
    } else {
        return std::nullopt; // unsupported source type or truncated params
    }
    return result;
}

std::vector<std::byte> encode_transfer_info(
    const Uuid& transfer_id, std::int32_t channel_type, std::int32_t status,
    std::int32_t size, std::span<const std::byte> params) {
    if (params.size() > 65535) return {};
    std::vector<std::byte> output(transfer_info_id.begin(), transfer_info_id.end());
    append_uuid(output, transfer_id);
    append_le_u32(output, static_cast<std::uint32_t>(channel_type));
    append_le_u32(output, 0); // TargetType (unused by the viewer for asset routing)
    append_le_u32(output, static_cast<std::uint32_t>(status));
    append_le_u32(output, static_cast<std::uint32_t>(size));
    append_le_u16(output, static_cast<std::uint16_t>(params.size()));
    output.insert(output.end(), params.begin(), params.end());
    return output;
}

std::vector<std::byte> encode_transfer_packet(
    const Uuid& transfer_id, std::int32_t channel_type, std::int32_t packet,
    std::int32_t status, std::span<const std::byte> data) {
    if (data.size() > 65535) return {};
    std::vector<std::byte> output{transfer_packet_id};
    append_uuid(output, transfer_id);
    append_le_u32(output, static_cast<std::uint32_t>(channel_type));
    append_le_u32(output, static_cast<std::uint32_t>(packet));
    append_le_u32(output, static_cast<std::uint32_t>(status));
    append_le_u16(output, static_cast<std::uint16_t>(data.size()));
    output.insert(output.end(), data.begin(), data.end());
    return output;
}

std::optional<ObjectAdd> decode_object_add(std::span<const std::byte> payload) {
    constexpr std::size_t message_size = 146;
    if (payload.size() != message_size ||
        !std::equal(object_add_id.begin(), object_add_id.end(), payload.begin()))
        return std::nullopt;
    ObjectAdd result;
    std::copy_n(payload.begin() + 2, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 18, 16, result.session_id.begin());
    std::copy_n(payload.begin() + 34, 16, result.group_id.begin());
    result.pcode = std::to_integer<std::uint8_t>(payload[50]);
    result.material = std::to_integer<std::uint8_t>(payload[51]);
    result.add_flags = read_le_u32(payload, 52);
    result.path_curve = std::to_integer<std::uint8_t>(payload[56]);
    result.profile_curve = std::to_integer<std::uint8_t>(payload[57]);
    result.path_begin = read_le_u16(payload, 58);
    result.path_end = read_le_u16(payload, 60);
    result.path_scale_x = std::to_integer<std::uint8_t>(payload[62]);
    result.path_scale_y = std::to_integer<std::uint8_t>(payload[63]);
    result.path_shear_x = std::to_integer<std::uint8_t>(payload[64]);
    result.path_shear_y = std::to_integer<std::uint8_t>(payload[65]);
    result.path_twist = std::to_integer<std::uint8_t>(payload[66]);
    result.path_twist_begin = std::to_integer<std::uint8_t>(payload[67]);
    result.path_radius_offset = std::to_integer<std::uint8_t>(payload[68]);
    result.path_taper_x = std::to_integer<std::uint8_t>(payload[69]);
    result.path_taper_y = std::to_integer<std::uint8_t>(payload[70]);
    result.path_revolutions = std::to_integer<std::uint8_t>(payload[71]);
    result.path_skew = std::to_integer<std::uint8_t>(payload[72]);
    result.profile_begin = read_le_u16(payload, 73);
    result.profile_end = read_le_u16(payload, 75);
    result.profile_hollow = read_le_u16(payload, 77);
    result.bypass_raycast = payload[79] != std::byte{};
    result.ray_start = read_vector3(payload, 80);
    result.ray_end = read_vector3(payload, 92);
    std::copy_n(payload.begin() + 104, 16, result.ray_target_id.begin());
    result.ray_end_is_intersection = payload[120] != std::byte{};
    result.scale = read_vector3(payload, 121);
    result.rotation = read_vector3(payload, 133);
    result.state = std::to_integer<std::uint8_t>(payload[145]);
    const auto finite_vector = [](const auto& values) {
        return std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); });
    };
    if (!finite_vector(result.ray_start) || !finite_vector(result.ray_end) ||
        !finite_vector(result.scale) || !finite_vector(result.rotation))
        return std::nullopt;
    return result;
}

std::optional<DeRezObject> decode_derez_object(std::span<const std::byte> payload) {
    constexpr std::size_t header_size = 88;
    if (payload.size() < header_size ||
        !std::equal(derez_object_id.begin(), derez_object_id.end(), payload.begin()))
        return std::nullopt;
    const auto count = std::to_integer<std::size_t>(payload[87]);
    if (count == 0 || payload.size() != header_size + count * sizeof(std::uint32_t))
        return std::nullopt;
    DeRezObject result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    std::copy_n(payload.begin() + 36, 16, result.group_id.begin());
    result.destination = std::to_integer<std::uint8_t>(payload[52]);
    std::copy_n(payload.begin() + 53, 16, result.destination_id.begin());
    std::copy_n(payload.begin() + 69, 16, result.transaction_id.begin());
    result.packet_count = std::to_integer<std::uint8_t>(payload[85]);
    result.packet_number = std::to_integer<std::uint8_t>(payload[86]);
    result.local_ids.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
        result.local_ids.push_back(read_le_u32(payload, header_size + index * sizeof(std::uint32_t)));
    return result;
}

bool valid_derez_batch(std::uint8_t packet_count, std::uint8_t packet_number) {
    return packet_count > 0 && packet_number <= packet_count;
}

std::optional<RezObject> decode_rez_object(std::span<const std::byte> payload) {
    constexpr std::size_t minimum_size = 144;
    if (payload.size() < minimum_size ||
        !std::equal(rez_object_id.begin(), rez_object_id.end(), payload.begin()))
        return std::nullopt;
    RezObject result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    std::copy_n(payload.begin() + 36, 16, result.group_id.begin());
    std::copy_n(payload.begin() + 52, 16, result.from_task_id.begin());
    result.bypass_raycast = std::to_integer<std::uint8_t>(payload[68]);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        result.ray_start[axis] = read_f32(payload, 69 + axis * sizeof(float));
        result.ray_end[axis] = read_f32(payload, 81 + axis * sizeof(float));
    }
    std::copy_n(payload.begin() + 93, 16, result.ray_target_id.begin());
    result.ray_end_is_intersection = payload[109] != std::byte{};
    result.rez_selected = payload[110] != std::byte{};
    result.remove_item = payload[111] != std::byte{};
    std::copy_n(payload.begin() + 128, 16, result.item_id.begin());
    const auto finite = [](const auto& values) {
        return std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); });
    };
    if (!finite(result.ray_start) || !finite(result.ray_end)) return std::nullopt;
    return result;
}

std::vector<std::byte> encode_kill_object(std::span<const std::uint32_t> local_ids) {
    if (local_ids.empty() || local_ids.size() > 255) return {};
    std::vector<std::byte> output{std::byte{0x10}, static_cast<std::byte>(local_ids.size())};
    output.reserve(2 + local_ids.size() * sizeof(std::uint32_t));
    for (const auto local_id : local_ids) append_le_u32(output, local_id);
    return output;
}

std::optional<ObjectSelect> decode_object_select(std::span<const std::byte> payload) {
    constexpr std::size_t header_size = 37;
    if (payload.size() < header_size ||
        !std::equal(object_select_id.begin(), object_select_id.end(), payload.begin()))
        return std::nullopt;
    const auto count = std::to_integer<std::size_t>(payload[36]);
    if (count == 0 || payload.size() != header_size + count * sizeof(std::uint32_t))
        return std::nullopt;
    ObjectSelect result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    result.local_ids.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
        result.local_ids.push_back(read_le_u32(payload, header_size + index * sizeof(std::uint32_t)));
    return result;
}

std::optional<ObjectSelect> decode_object_deselect(std::span<const std::byte> payload) {
    constexpr std::size_t header_size = 37;
    if (payload.size() < header_size ||
        !std::equal(object_deselect_id.begin(), object_deselect_id.end(), payload.begin()))
        return std::nullopt;
    const auto count = std::to_integer<std::size_t>(payload[36]);
    if (count == 0 || payload.size() != header_size + count * sizeof(std::uint32_t))
        return std::nullopt;
    ObjectSelect result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    result.local_ids.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
        result.local_ids.push_back(read_le_u32(payload, header_size + index * sizeof(std::uint32_t)));
    return result;
}

namespace {
std::optional<ObjectSelect> decode_object_relationship_request(
    std::span<const std::byte> payload, const std::array<std::byte, 4>& message_id) {
    constexpr std::size_t header_size = 37;
    if (payload.size() < header_size ||
        !std::equal(message_id.begin(), message_id.end(), payload.begin()))
        return std::nullopt;
    const auto count = std::to_integer<std::size_t>(payload[36]);
    if (count == 0 || count > 256 ||
        payload.size() != header_size + count * sizeof(std::uint32_t))
        return std::nullopt;
    ObjectSelect result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    result.local_ids.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
        result.local_ids.push_back(read_le_u32(payload, header_size + index * sizeof(std::uint32_t)));
    return result;
}
} // namespace

std::optional<ObjectSelect> decode_object_link(std::span<const std::byte> payload) {
    return decode_object_relationship_request(payload, object_link_id);
}

std::optional<ObjectSelect> decode_object_delink(std::span<const std::byte> payload) {
    return decode_object_relationship_request(payload, object_delink_id);
}

std::optional<ObjectGrab> decode_object_grab(std::span<const std::byte> payload) {
    // ObjectGrab (Low 117): AgentID, SessionID, LocalID (U32), GrabOffset
    // (LLVector3), then a variable SurfaceInfo array. Unlike ObjectGrabUpdate
    // this identifies the clicked prim by its region-local id, not its full
    // object UUID, and carries the initial touch offset rather than a drag path.
    constexpr std::size_t fixed_size = 53;
    constexpr std::size_t surface_block_size = 64;
    if (payload.size() < fixed_size ||
        !std::equal(object_grab_id.begin(), object_grab_id.end(), payload.begin()))
        return std::nullopt;
    const auto surface_count = std::to_integer<std::size_t>(payload[52]);
    if (payload.size() != fixed_size + surface_count * surface_block_size) return std::nullopt;
    ObjectGrab result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    result.local_id = read_le_u32(payload, 36);
    result.grab_offset = {read_f32(payload, 40), read_f32(payload, 44), read_f32(payload, 48)};
    if (!std::all_of(result.grab_offset.begin(), result.grab_offset.end(),
                     [](float value) { return std::isfinite(value); }))
        return std::nullopt;
    return result;
}

std::optional<ObjectGrabUpdate> decode_object_grab_update(std::span<const std::byte> payload) {
    constexpr std::size_t fixed_size = 81;
    constexpr std::size_t surface_block_size = 64;
    if (payload.size() < fixed_size ||
        !std::equal(object_grab_update_id.begin(), object_grab_update_id.end(), payload.begin()))
        return std::nullopt;
    const auto surface_count = std::to_integer<std::size_t>(payload[80]);
    if (payload.size() != fixed_size + surface_count * surface_block_size) return std::nullopt;
    ObjectGrabUpdate result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    std::copy_n(payload.begin() + 36, 16, result.object_id.begin());
    result.grab_offset_initial = {
        read_f32(payload, 52), read_f32(payload, 56), read_f32(payload, 60)};
    result.grab_position = {
        read_f32(payload, 64), read_f32(payload, 68), read_f32(payload, 72)};
    result.time_since_last = read_le_u32(payload, 76);
    const auto finite = [](const auto& values) {
        return std::all_of(values.begin(), values.end(),
            [](float value) { return std::isfinite(value); });
    };
    if (!finite(result.grab_offset_initial) || !finite(result.grab_position))
        return std::nullopt;
    return result;
}

std::optional<MultipleObjectUpdate> decode_multiple_object_update(std::span<const std::byte> payload) {
    constexpr std::uint8_t update_position = 0x01;
    constexpr std::uint8_t update_rotation = 0x02;
    constexpr std::uint8_t update_scale = 0x04;
    constexpr std::uint8_t known_flags = 0x1f;
    constexpr std::size_t header_size = 35;
    if (payload.size() < header_size ||
        !std::equal(multiple_object_update_id.begin(), multiple_object_update_id.end(), payload.begin()))
        return std::nullopt;
    MultipleObjectUpdate result;
    std::copy_n(payload.begin() + 2, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 18, 16, result.session_id.begin());
    const auto count = std::to_integer<std::size_t>(payload[34]);
    if (count == 0) return std::nullopt;
    result.objects.reserve(count);
    std::size_t offset = header_size;
    for (std::size_t index = 0; index < count; ++index) {
        if (offset + 6 > payload.size()) return std::nullopt;
        ObjectTransformUpdate update;
        update.local_id = read_le_u32(payload, offset);
        update.type = std::to_integer<std::uint8_t>(payload[offset + 4]);
        const auto data_size = std::to_integer<std::size_t>(payload[offset + 5]);
        offset += 6;
        const auto vector_count = static_cast<std::size_t>((update.type & update_position) != 0) +
            static_cast<std::size_t>((update.type & update_rotation) != 0) +
            static_cast<std::size_t>((update.type & update_scale) != 0);
        if ((update.type & ~known_flags) != 0 || vector_count == 0 ||
            data_size != vector_count * 12 || offset + data_size > payload.size())
            return std::nullopt;
        const auto read_vector = [&]() {
            std::array<float, 3> value{
                read_f32(payload, offset), read_f32(payload, offset + 4), read_f32(payload, offset + 8)};
            offset += 12;
            return value;
        };
        if ((update.type & update_position) != 0) update.position = read_vector();
        if ((update.type & update_rotation) != 0) update.rotation = read_vector();
        if ((update.type & update_scale) != 0) update.scale = read_vector();
        result.objects.push_back(update);
    }
    if (offset != payload.size()) return std::nullopt;
    return result;
}

std::optional<ObjectName> decode_object_name(std::span<const std::byte> payload) {
    constexpr std::size_t header_size = 37;
    if (payload.size() < header_size ||
        !std::equal(object_name_id.begin(), object_name_id.end(), payload.begin()))
        return std::nullopt;
    ObjectName result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    const auto count = std::to_integer<std::size_t>(payload[36]);
    if (count == 0) return std::nullopt;
    result.objects.reserve(count);
    std::size_t offset = header_size;
    for (std::size_t index = 0; index < count; ++index) {
        if (offset + 5 > payload.size()) return std::nullopt;
        ObjectNameUpdate update;
        update.local_id = read_le_u32(payload, offset);
        const auto encoded_size = std::to_integer<std::size_t>(payload[offset + 4]);
        offset += 5;
        if (encoded_size < 2 || encoded_size > 64 || offset + encoded_size > payload.size() ||
            payload[offset + encoded_size - 1] != std::byte{})
            return std::nullopt;
        update.name.assign(reinterpret_cast<const char*>(payload.data() + offset), encoded_size - 1);
        offset += encoded_size;
        result.objects.push_back(std::move(update));
    }
    if (offset != payload.size()) return std::nullopt;
    return result;
}

std::optional<ObjectDescription> decode_object_description(std::span<const std::byte> payload) {
    constexpr std::size_t header_size = 37;
    if (payload.size() < header_size ||
        !std::equal(object_description_id.begin(), object_description_id.end(), payload.begin()))
        return std::nullopt;
    ObjectDescription result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    const auto count = std::to_integer<std::size_t>(payload[36]);
    if (count == 0) return std::nullopt;
    result.objects.reserve(count);
    std::size_t offset = header_size;
    for (std::size_t index = 0; index < count; ++index) {
        if (offset + 5 > payload.size()) return std::nullopt;
        ObjectDescriptionUpdate update;
        update.local_id = read_le_u32(payload, offset);
        const auto encoded_size = std::to_integer<std::size_t>(payload[offset + 4]);
        offset += 5;
        if (encoded_size == 0 || encoded_size > 128 || offset + encoded_size > payload.size() ||
            payload[offset + encoded_size - 1] != std::byte{})
            return std::nullopt;
        update.description.assign(
            reinterpret_cast<const char*>(payload.data() + offset), encoded_size - 1);
        offset += encoded_size;
        result.objects.push_back(std::move(update));
    }
    if (offset != payload.size()) return std::nullopt;
    return result;
}

std::optional<ObjectPermissions> decode_object_permissions(std::span<const std::byte> payload) {
    constexpr std::size_t header_size = 38;
    constexpr std::size_t block_size = 10;
    if (payload.size() < header_size ||
        !std::equal(object_permissions_id.begin(), object_permissions_id.end(), payload.begin()))
        return std::nullopt;
    ObjectPermissions result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    const auto override_value = std::to_integer<std::uint8_t>(payload[36]);
    if (override_value > 1) return std::nullopt;
    result.override_permissions = override_value != 0;
    const auto count = std::to_integer<std::size_t>(payload[37]);
    if (count == 0 || payload.size() != header_size + count * block_size) return std::nullopt;
    result.objects.reserve(count);
    std::size_t offset = header_size;
    for (std::size_t index = 0; index < count; ++index) {
        ObjectPermissionUpdate update;
        update.local_id = read_le_u32(payload, offset);
        update.field = std::to_integer<std::uint8_t>(payload[offset + 4]);
        const auto set_value = std::to_integer<std::uint8_t>(payload[offset + 5]);
        if (set_value > 1) return std::nullopt;
        update.set = set_value != 0;
        update.mask = read_le_u32(payload, offset + 6);
        result.objects.push_back(update);
        offset += block_size;
    }
    return result;
}

std::optional<ObjectDuplicate> decode_object_duplicate(std::span<const std::byte> payload) {
    constexpr std::size_t header_size = 69;
    if (payload.size() < header_size ||
        !std::equal(object_duplicate_id.begin(), object_duplicate_id.end(), payload.begin()))
        return std::nullopt;
    ObjectDuplicate result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    std::copy_n(payload.begin() + 36, 16, result.group_id.begin());
    result.offset = {read_f32(payload, 52), read_f32(payload, 56), read_f32(payload, 60)};
    result.duplicate_flags = read_le_u32(payload, 64);
    const auto count = std::to_integer<std::size_t>(payload[68]);
    if (count == 0 || payload.size() != header_size + count * sizeof(std::uint32_t))
        return std::nullopt;
    result.local_ids.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
        result.local_ids.push_back(read_le_u32(payload, header_size + index * sizeof(std::uint32_t)));
    return result;
}

std::optional<ObjectMaterial> decode_object_material(std::span<const std::byte> payload) {
    constexpr std::size_t header_size = 37;
    constexpr std::size_t block_size = 5;
    if (payload.size() < header_size ||
        !std::equal(object_material_id.begin(), object_material_id.end(), payload.begin()))
        return std::nullopt;
    ObjectMaterial result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    const auto count = std::to_integer<std::size_t>(payload[36]);
    if (count == 0 || payload.size() != header_size + count * block_size) return std::nullopt;
    result.objects.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto offset = header_size + index * block_size;
        result.objects.push_back({
            read_le_u32(payload, offset), std::to_integer<std::uint8_t>(payload[offset + 4])});
    }
    return result;
}

std::optional<ObjectShape> decode_object_shape(std::span<const std::byte> payload) {
    constexpr std::size_t header_size = 37;
    constexpr std::size_t block_size = 27;
    if (payload.size() < header_size ||
        !std::equal(object_shape_id.begin(), object_shape_id.end(), payload.begin()))
        return std::nullopt;
    ObjectShape result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    const auto count = std::to_integer<std::size_t>(payload[36]);
    if (count == 0 || payload.size() != header_size + count * block_size) return std::nullopt;
    result.objects.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto offset = header_size + index * block_size;
        ObjectShapeUpdate update;
        update.local_id = read_le_u32(payload, offset);
        update.path_curve = std::to_integer<std::uint8_t>(payload[offset + 4]);
        update.profile_curve = std::to_integer<std::uint8_t>(payload[offset + 5]);
        update.path_begin = read_le_u16(payload, offset + 6);
        update.path_end = read_le_u16(payload, offset + 8);
        update.path_scale_x = std::to_integer<std::uint8_t>(payload[offset + 10]);
        update.path_scale_y = std::to_integer<std::uint8_t>(payload[offset + 11]);
        update.path_shear_x = std::to_integer<std::uint8_t>(payload[offset + 12]);
        update.path_shear_y = std::to_integer<std::uint8_t>(payload[offset + 13]);
        update.path_twist = std::to_integer<std::uint8_t>(payload[offset + 14]);
        update.path_twist_begin = std::to_integer<std::uint8_t>(payload[offset + 15]);
        update.path_radius_offset = std::to_integer<std::uint8_t>(payload[offset + 16]);
        update.path_taper_x = std::to_integer<std::uint8_t>(payload[offset + 17]);
        update.path_taper_y = std::to_integer<std::uint8_t>(payload[offset + 18]);
        update.path_revolutions = std::to_integer<std::uint8_t>(payload[offset + 19]);
        update.path_skew = std::to_integer<std::uint8_t>(payload[offset + 20]);
        update.profile_begin = read_le_u16(payload, offset + 21);
        update.profile_end = read_le_u16(payload, offset + 23);
        update.profile_hollow = read_le_u16(payload, offset + 25);
        result.objects.push_back(update);
    }
    return result;
}

std::optional<ObjectImage> decode_object_image(std::span<const std::byte> payload) {
    constexpr std::size_t header_size = 37;
    if (payload.size() < header_size ||
        !std::equal(object_image_id.begin(), object_image_id.end(), payload.begin()))
        return std::nullopt;
    ObjectImage result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    const auto count = std::to_integer<std::size_t>(payload[36]);
    if (count == 0) return std::nullopt;
    result.objects.reserve(count);
    std::size_t offset = header_size;
    for (std::size_t index = 0; index < count; ++index) {
        if (offset + 7 > payload.size()) return std::nullopt;
        ObjectImageUpdate update;
        update.local_id = read_le_u32(payload, offset);
        offset += 4;
        const auto media_size = std::to_integer<std::size_t>(payload[offset++]);
        if (offset + media_size + 2 > payload.size()) return std::nullopt;
        offset += media_size;
        const auto texture_size = static_cast<std::size_t>(read_le_u16(payload, offset));
        offset += 2;
        if (offset + texture_size > payload.size()) return std::nullopt;
        update.texture_entry.assign(payload.begin() + offset, payload.begin() + offset + texture_size);
        offset += texture_size;
        result.objects.push_back(std::move(update));
    }
    if (offset != payload.size()) return std::nullopt;
    return result;
}

std::optional<ObjectFlagUpdate> decode_object_flag_update(std::span<const std::byte> payload) {
    constexpr std::size_t fixed_size = 45;
    constexpr std::size_t extra_physics_size = 17;
    if (payload.size() < fixed_size ||
        !std::equal(object_flag_update_id.begin(), object_flag_update_id.end(), payload.begin()))
        return std::nullopt;
    const auto count = std::to_integer<std::size_t>(payload[44]);
    if (payload.size() != fixed_size + count * extra_physics_size) return std::nullopt;
    ObjectFlagUpdate result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    result.local_id = read_le_u32(payload, 36);
    result.use_physics = payload[40] != std::byte{};
    result.temporary = payload[41] != std::byte{};
    result.phantom = payload[42] != std::byte{};
    result.casts_shadows = payload[43] != std::byte{};
    if (count > 0) {
        constexpr std::size_t extra_offset = fixed_size;
        result.physics_shape_type = std::to_integer<std::uint8_t>(payload[extra_offset]);
        result.density = read_f32(payload, extra_offset + 1);
        result.friction = read_f32(payload, extra_offset + 5);
        result.restitution = read_f32(payload, extra_offset + 9);
        result.gravity_multiplier = read_f32(payload, extra_offset + 13);
        if (!std::isfinite(result.density) || !std::isfinite(result.friction) ||
            !std::isfinite(result.restitution) || !std::isfinite(result.gravity_multiplier))
            return std::nullopt;
        result.has_extra_physics = true;
    }
    return result;
}

std::optional<RequestObjectPropertiesFamily> decode_request_object_properties_family(
    std::span<const std::byte> payload) {
    if (payload.size() != 54 ||
        !std::equal(request_object_properties_family_id.begin(),
                    request_object_properties_family_id.end(), payload.begin()))
        return std::nullopt;
    RequestObjectPropertiesFamily result;
    std::copy_n(payload.begin() + 2, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 18, 16, result.session_id.begin());
    result.request_flags = read_le_u32(payload, 34);
    std::copy_n(payload.begin() + 38, 16, result.object_id.begin());
    return result;
}

std::vector<std::byte> encode_object_properties(std::span<const ObjectProperties> objects) {
    if (objects.empty() || objects.size() > 255) return {};
    std::vector<std::byte> output{std::byte{0xff}, std::byte{0x09},
                                  static_cast<std::byte>(objects.size())};
    Uuid zero{};
    for (const auto& object : objects) {
        append_uuid(output, object.object_id);
        append_uuid(output, object.creator_id);
        append_uuid(output, object.owner_id);
        append_uuid(output, zero); // group
        append_le_u64(output, object.creation_date * 1000000); // protocol uses microseconds
        append_le_u32(output, object.base_permissions);
        append_le_u32(output, object.owner_permissions);
        append_le_u32(output, object.group_permissions);
        append_le_u32(output, object.everyone_permissions);
        append_le_u32(output, object.next_owner_permissions);
        append_le_u32(output, 0); // ownership cost
        output.push_back(std::byte{}); // not for sale
        append_le_u32(output, 0); // sale price
        std::uint8_t aggregate_permissions = 0;
        if ((object.folded_owner_permissions & 0x00008000) != 0) aggregate_permissions |= 0x03; // copy
        if ((object.folded_owner_permissions & 0x00004000) != 0) aggregate_permissions |= 0x0c; // modify
        if ((object.folded_owner_permissions & 0x00002000) != 0) aggregate_permissions |= 0x30; // transfer
        output.push_back(static_cast<std::byte>(aggregate_permissions));
        output.insert(output.end(), 2, std::byte{}); // aggregate texture permissions
        append_le_u32(output, 0); // category
        append_le_u16(output, 0); // inventory serial
        for (int index = 0; index < 3; ++index) append_uuid(output, zero); // inventory IDs
        append_uuid(output, object.creator_id); // initial owner is also the last owner
        if (!append_variable1(output, object.name) || !append_variable1(output, object.description) ||
            !append_variable1(output, {}) || !append_variable1(output, {}) || !append_variable1(output, {}))
            return {};
    }
    return output;
}

std::vector<std::byte> encode_object_properties_family(
    std::uint32_t request_flags, const ObjectProperties& object) {
    std::vector<std::byte> output{std::byte{0xff}, std::byte{0x0a}};
    Uuid zero{};
    append_le_u32(output, request_flags);
    append_uuid(output, object.object_id);
    append_uuid(output, object.owner_id);
    append_uuid(output, zero); // group
    append_le_u32(output, object.base_permissions);
    append_le_u32(output, object.owner_permissions);
    append_le_u32(output, object.group_permissions);
    append_le_u32(output, object.everyone_permissions);
    append_le_u32(output, object.next_owner_permissions);
    append_le_u32(output, 0); // ownership cost
    output.push_back(std::byte{}); // not for sale
    append_le_u32(output, 0); // sale price
    append_le_u32(output, 0); // category
    append_uuid(output, object.creator_id); // initial owner is also the last owner
    if (!append_variable1(output, object.name) || !append_variable1(output, object.description)) return {};
    return output;
}

std::optional<std::vector<Uuid>> decode_uuid_name_request(std::span<const std::byte> payload) {
    constexpr std::size_t header_size = 5;
    if (payload.size() < header_size ||
        !std::equal(uuid_name_request_id.begin(), uuid_name_request_id.end(), payload.begin()))
        return std::nullopt;
    const auto count = std::to_integer<std::size_t>(payload[4]);
    if (count == 0 || payload.size() != header_size + count * 16) return std::nullopt;
    std::vector<Uuid> result(count);
    for (std::size_t index = 0; index < count; ++index)
        std::copy_n(payload.begin() + header_size + index * 16, 16, result[index].begin());
    return result;
}

std::vector<std::byte> encode_uuid_name_reply(std::span<const UuidName> names) {
    if (names.empty() || names.size() > 255) return {};
    std::vector<std::byte> output(uuid_name_reply_id.begin(), uuid_name_reply_id.end());
    output.push_back(static_cast<std::byte>(names.size()));
    for (const auto& name : names) {
        append_uuid(output, name.id);
        if (!append_variable1(output, name.first_name) || !append_variable1(output, name.last_name))
            return {};
    }
    return output;
}

std::optional<MapBlockRequest> decode_map_block_request(std::span<const std::byte> payload) {
    constexpr std::size_t size = 53;
    if (payload.size() != size ||
        !std::equal(map_block_request_id.begin(), map_block_request_id.end(), payload.begin()))
        return std::nullopt;
    MapBlockRequest request;
    std::copy_n(payload.begin() + 4, 16, request.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, request.session_id.begin());
    request.flags = read_le_u32(payload, 36);
    request.min_x = read_le_u16(payload, 45);
    request.max_x = read_le_u16(payload, 47);
    request.min_y = read_le_u16(payload, 49);
    request.max_y = read_le_u16(payload, 51);
    if (request.min_x > request.max_x || request.min_y > request.max_y)
        return std::nullopt;
    return request;
}

std::optional<MapNameRequest> decode_map_name_request(std::span<const std::byte> payload) {
    constexpr std::size_t fixed_size = 46;
    if (payload.size() < fixed_size ||
        !std::equal(map_name_request_id.begin(), map_name_request_id.end(), payload.begin()))
        return std::nullopt;
    const auto name_size = std::to_integer<std::size_t>(payload[45]);
    if (name_size == 0 || payload.size() != fixed_size + name_size) return std::nullopt;
    MapNameRequest request;
    std::copy_n(payload.begin() + 4, 16, request.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, request.session_id.begin());
    request.flags = read_le_u32(payload, 36);
    request.name.assign(reinterpret_cast<const char*>(payload.data() + fixed_size), name_size);
    while (!request.name.empty() && request.name.back() == '\0') request.name.pop_back();
    if (request.name.empty() || request.name.find('\0') != std::string::npos) return std::nullopt;
    return request;
}

std::vector<std::byte> encode_map_block_reply(const Uuid& agent_id, std::uint32_t flags,
                                               std::span<const MapBlock> regions) {
    if (regions.empty() || regions.size() > 255) return {};
    std::vector<std::byte> output(map_block_reply_id.begin(), map_block_reply_id.end());
    append_uuid(output, agent_id);
    append_le_u32(output, flags);
    output.push_back(static_cast<std::byte>(regions.size()));
    for (const auto& region : regions) {
        append_le_u16(output, region.x);
        append_le_u16(output, region.y);
        if (!append_variable1(output, region.name)) return {};
        output.push_back(static_cast<std::byte>(region.access));
        append_le_u32(output, region.region_flags);
        output.push_back(static_cast<std::byte>(region.water_height));
        output.push_back(static_cast<std::byte>(region.agents));
        append_uuid(output, region.map_image_id);
    }
    const bool include_sizes = std::any_of(regions.begin(), regions.end(), [](const MapBlock& region) {
        return region.size_x > 256 || region.size_y > 256;
    });
    output.push_back(static_cast<std::byte>(include_sizes ? regions.size() : 0));
    if (include_sizes) {
        for (const auto& region : regions) {
            append_le_u16(output, region.size_x);
            append_le_u16(output, region.size_y);
        }
    }
    return output;
}

std::vector<std::byte> encode_update_create_inventory_item(const AgentMessage& message,
                                                           std::uint32_t callback_id,
                                                           const InventoryItem& item) {
    std::vector<std::byte> output(update_create_inventory_item_id.begin(),
                                  update_create_inventory_item_id.end());
    append_uuid(output, message.agent_id);
    output.push_back(std::byte{1}); // simulator approved
    append_uuid(output, Uuid{}); // no transaction ID for an inventory copy
    output.push_back(std::byte{1}); // InventoryData block count
    append_uuid(output, item.item_id);
    append_uuid(output, item.folder_id);
    append_le_u32(output, callback_id);
    append_uuid(output, item.creator_id);
    append_uuid(output, item.owner_id);
    append_uuid(output, Uuid{}); // group ID
    append_le_u32(output, item.base_permissions);
    append_le_u32(output, item.current_permissions);
    append_le_u32(output, 0); // group permissions
    append_le_u32(output, item.everyone_permissions);
    append_le_u32(output, item.next_permissions);
    output.push_back(std::byte{0}); // not group owned
    append_uuid(output, item.asset_id);
    output.push_back(static_cast<std::byte>(item.asset_type));
    output.push_back(static_cast<std::byte>(item.inventory_type));
    append_le_u32(output, item.flags);
    output.push_back(static_cast<std::byte>(item.sale_type));
    append_le_u32(output, static_cast<std::uint32_t>(item.sale_price));
    if (!append_variable1(output, item.name) || !append_variable1(output, item.description)) return {};
    append_le_u32(output, static_cast<std::uint32_t>(item.creation_date));
    append_le_u32(output, 0); // viewer does not validate inventory CRC
    return output;
}

std::vector<std::byte> encode_logout_reply(const AgentMessage& message) {
    std::vector<std::byte> output(logout_reply_id.begin(), logout_reply_id.end());
    append_uuid(output, message.agent_id);
    append_uuid(output, message.session_id);
    output.push_back(std::byte{1}); // InventoryData block count
    append_uuid(output, Uuid{}); // no changed inventory items
    return output;
}

std::optional<AgentCachedTexture> decode_agent_cached_texture(std::span<const std::byte> payload) {
    constexpr std::size_t header_size = 41;
    constexpr std::size_t block_size = 17;
    if (payload.size() < header_size ||
        !std::equal(agent_cached_texture_id.begin(), agent_cached_texture_id.end(), payload.begin()))
        return std::nullopt;
    const auto count = std::to_integer<std::size_t>(payload[40]);
    if (count == 0 || payload.size() != header_size + count * block_size) return std::nullopt;
    AgentCachedTexture result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    result.serial = static_cast<std::int32_t>(read_le_u32(payload, 36));
    result.queries.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto offset = header_size + index * block_size;
        CachedTextureQuery query;
        std::copy_n(payload.begin() + offset, 16, query.cache_id.begin());
        query.texture_index = std::to_integer<std::uint8_t>(payload[offset + 16]);
        result.queries.push_back(query);
    }
    return result;
}

std::vector<std::byte> encode_agent_cached_texture_response(const AgentCachedTexture& message) {
    if (message.queries.empty() || message.queries.size() > 255) return {};
    std::vector<std::byte> output(agent_cached_texture_response_id.begin(), agent_cached_texture_response_id.end());
    append_uuid(output, message.agent_id);
    append_uuid(output, message.session_id);
    append_le_u32(output, static_cast<std::uint32_t>(message.serial));
    output.push_back(static_cast<std::byte>(message.queries.size()));
    for (const auto& query : message.queries) {
        append_uuid(output, query.texture_id);
        output.push_back(static_cast<std::byte>(query.texture_index));
        if (!append_variable1(output, "")) return {};
    }
    return output;
}

std::optional<AgentSetAppearance> decode_agent_set_appearance(std::span<const std::byte> payload) {
    constexpr std::size_t fixed_size = 53;
    constexpr std::size_t cache_block_size = 17;
    if (payload.size() < fixed_size ||
        !std::equal(agent_set_appearance_id.begin(), agent_set_appearance_id.end(), payload.begin()))
        return std::nullopt;
    AgentSetAppearance result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    result.serial = read_le_u32(payload, 36);
    for (std::size_t axis = 0; axis < result.size.size(); ++axis)
        result.size[axis] = read_f32(payload, 40 + axis * 4);
    const auto cache_count = std::to_integer<std::size_t>(payload[52]);
    auto position = fixed_size;
    if (position + cache_count * cache_block_size + 2 > payload.size()) return std::nullopt;
    result.cache_entries.reserve(cache_count);
    for (std::size_t index = 0; index < cache_count; ++index) {
        CachedTextureQuery entry;
        std::copy_n(payload.begin() + position, 16, entry.cache_id.begin());
        entry.texture_index = std::to_integer<std::uint8_t>(payload[position + 16]);
        result.cache_entries.push_back(entry);
        position += cache_block_size;
    }
    const auto texture_entry_size = std::to_integer<std::size_t>(payload[position]) |
                                    (std::to_integer<std::size_t>(payload[position + 1]) << 8);
    position += 2;
    if (position + texture_entry_size + 1 > payload.size()) return std::nullopt;
    const auto texture_entry = payload.subspan(position, texture_entry_size);
    result.texture_entry.assign(texture_entry.begin(), texture_entry.end());
    position += texture_entry_size;
    const auto visual_count = std::to_integer<std::size_t>(payload[position++]);
    if (position + visual_count != payload.size()) return std::nullopt;
    result.visual_params.reserve(visual_count);
    for (std::size_t index = 0; index < visual_count; ++index)
        result.visual_params.push_back(std::to_integer<std::uint8_t>(payload[position + index]));
    if (texture_entry.size() >= 16) {
        auto faces = unpack_texture_entry_faces(texture_entry);
        if (!faces) return std::nullopt;
        result.texture_ids = *faces;
    }
    return result;
}

std::vector<std::byte> encode_avatar_appearance(const AvatarAppearance& message) {
    if (message.texture_entry.empty() || message.texture_entry.size() > 65535 ||
        message.visual_params.empty() || message.visual_params.size() > 255)
        return {};
    constexpr std::array<std::byte, 4> message_id{
        std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x9e}};
    std::vector<std::byte> output(message_id.begin(), message_id.end());
    append_uuid(output, message.sender_id);
    output.push_back(std::byte{}); // not a trial avatar
    append_le_u16(output, static_cast<std::uint16_t>(message.texture_entry.size()));
    output.insert(output.end(), message.texture_entry.begin(), message.texture_entry.end());
    output.push_back(static_cast<std::byte>(message.visual_params.size()));
    for (const auto value : message.visual_params) output.push_back(static_cast<std::byte>(value));
    output.push_back(std::byte{1}); // one appearance metadata block
    output.push_back(static_cast<std::byte>(message.appearance_version)); // 0=legacy, 1=server-side
    append_le_u32(output, message.serial);
    append_le_u32(output, 0); // no server-side appearance flags yet
    output.push_back(std::byte{1}); // one hover-height block
    for (const auto value : message.hover) append_f32(output, value);
    output.push_back(std::byte{}); // no attachments in this update
    return output;
}

std::optional<AgentAnimation> decode_agent_animation(std::span<const std::byte> payload) {
    constexpr std::size_t fixed_size = 34;
    constexpr std::size_t animation_size = 17;
    if (payload.size() < fixed_size || payload[0] != std::byte{5}) return std::nullopt;
    AgentAnimation result;
    std::copy_n(payload.begin() + 1, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 17, 16, result.session_id.begin());
    const auto count = std::to_integer<std::size_t>(payload[33]);
    auto position = fixed_size;
    if (position + count * animation_size + 1 > payload.size()) return std::nullopt;
    result.animations.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        AgentAnimationEntry entry;
        std::copy_n(payload.begin() + position, 16, entry.animation_id.begin());
        entry.start = payload[position + 16] != std::byte{};
        result.animations.push_back(entry);
        position += animation_size;
    }
    const auto physical_count = std::to_integer<std::size_t>(payload[position++]);
    for (std::size_t index = 0; index < physical_count; ++index) {
        if (position >= payload.size()) return std::nullopt;
        const auto size = std::to_integer<std::size_t>(payload[position++]);
        if (position + size > payload.size()) return std::nullopt;
        position += size;
    }
    if (position != payload.size()) return std::nullopt;
    return result;
}

std::vector<std::byte> encode_avatar_animation(const AvatarAnimation& message) {
    if (message.animations.empty() || message.animations.size() > 255) return {};
    std::vector<std::byte> output{std::byte{20}};
    append_uuid(output, message.sender_id);
    output.push_back(static_cast<std::byte>(message.animations.size()));
    for (const auto& animation : message.animations) {
        append_uuid(output, animation.animation_id);
        append_le_u32(output, static_cast<std::uint32_t>(animation.sequence));
    }
    output.push_back(static_cast<std::byte>(message.animations.size()));
    for (const auto& animation : message.animations) append_uuid(output, animation.source_id);
    output.push_back(std::byte{}); // no physical avatar events
    return output;
}

// Low 395. Layout from message_template.msg: AgentID, SessionID, then the
// ObjectData block - ItemID, OwnerID, AttachmentPt, and four masks - followed by
// two length-prefixed strings the viewer sends but the region does not need.
std::optional<RezSingleAttachmentFromInv> decode_rez_single_attachment_from_inv(
    std::span<const std::byte> payload) {
    constexpr std::array<std::byte, 4> message_id{
        std::byte{0xff}, std::byte{0xff}, std::byte{0x01}, std::byte{0x8b}};
    // 4 header + 16 agent + 16 session + 16 item + 16 owner + 1 point
    // + 16 masks + 1 name length + 1 description length
    if (payload.size() < 87 ||
        !std::equal(message_id.begin(), message_id.end(), payload.begin()))
        return std::nullopt;
    RezSingleAttachmentFromInv result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    std::copy_n(payload.begin() + 36, 16, result.item_id.begin());
    std::copy_n(payload.begin() + 52, 16, result.owner_id.begin());
    result.attachment_point = std::to_integer<std::uint8_t>(payload[68]);
    const auto read_u32 = [&](std::size_t at) {
        return static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(payload[at]) |
               (std::to_integer<std::uint32_t>(payload[at + 1]) << 8) |
               (std::to_integer<std::uint32_t>(payload[at + 2]) << 16) |
               (std::to_integer<std::uint32_t>(payload[at + 3]) << 24));
    };
    result.item_flags = read_u32(69);
    result.group_mask = read_u32(73);
    result.everyone_mask = read_u32(77);
    result.next_owner_mask = read_u32(81);
    // Both strings are length-prefixed and NUL-terminated by the viewer; the
    // trailing NUL is dropped rather than carried into a std::string, because a
    // name that compares unequal to itself is the kind of thing found weeks
    // later in an inventory search.
    std::size_t at = 85;
    const auto take_string = [&](std::string& into) {
        if (at >= payload.size()) return false;
        const auto length = std::to_integer<std::size_t>(payload[at]);
        ++at;
        if (at + length > payload.size()) return false;
        auto text = std::string(reinterpret_cast<const char*>(payload.data() + at), length);
        if (!text.empty() && text.back() == '\0') text.pop_back();
        into = std::move(text);
        at += length;
        return true;
    };
    if (!take_string(result.name)) return std::nullopt;
    if (!take_string(result.description)) return std::nullopt;
    return result;
}

// Low 113. A variable block, so a count byte then that many local ids: the
// viewer detaches a multi-prim outfit in one message.
std::optional<ObjectDetach> decode_object_detach(std::span<const std::byte> payload) {
    constexpr std::array<std::byte, 4> message_id{
        std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x71}};
    if (payload.size() < 37 ||
        !std::equal(message_id.begin(), message_id.end(), payload.begin()))
        return std::nullopt;
    ObjectDetach result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    const auto count = std::to_integer<std::size_t>(payload[36]);
    if (payload.size() < 37 + count * 4) return std::nullopt;
    for (std::size_t index = 0; index < count; ++index) {
        const auto at = 37 + index * 4;
        result.local_ids.push_back(
            static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(payload[at]) |
            (std::to_integer<std::uint32_t>(payload[at + 1]) << 8) |
            (std::to_integer<std::uint32_t>(payload[at + 2]) << 16) |
            (std::to_integer<std::uint32_t>(payload[at + 3]) << 24)));
    }
    return result;
}

// Low 396. AgentData (agent, session), HeaderData (compound id, total, detach
// all), then a variable ObjectData block: a count byte and that many objects,
// each ending in two length-prefixed strings. Layout read off the viewer's own
// sender in llattachmentsmgr.cpp, which is where the offsets are settled — the
// template says what the fields are, the sender says what goes on the wire.
std::optional<RezMultipleAttachmentsFromInv> decode_rez_multiple_attachments_from_inv(
    std::span<const std::byte> payload) {
    constexpr std::array<std::byte, 4> message_id{
        std::byte{0xff}, std::byte{0xff}, std::byte{0x01}, std::byte{0x8c}};
    // 4 header + 16 agent + 16 session + 16 compound + 1 total + 1 detach-all
    // + 1 object count
    if (payload.size() < 55 ||
        !std::equal(message_id.begin(), message_id.end(), payload.begin()))
        return std::nullopt;
    RezMultipleAttachmentsFromInv result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    std::copy_n(payload.begin() + 36, 16, result.compound_id.begin());
    result.total_objects = std::to_integer<std::uint8_t>(payload[52]);
    result.first_detach_all = std::to_integer<std::uint8_t>(payload[53]) != 0;
    const auto count = std::to_integer<std::size_t>(payload[54]);
    std::size_t at = 55;
    const auto read_u32 = [&](std::size_t from) {
        return static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(payload[from]) |
               (std::to_integer<std::uint32_t>(payload[from + 1]) << 8) |
               (std::to_integer<std::uint32_t>(payload[from + 2]) << 16) |
               (std::to_integer<std::uint32_t>(payload[from + 3]) << 24));
    };
    const auto take_string = [&](std::string& into) {
        if (at >= payload.size()) return false;
        const auto length = std::to_integer<std::size_t>(payload[at]);
        ++at;
        if (at + length > payload.size()) return false;
        auto text = std::string(reinterpret_cast<const char*>(payload.data() + at), length);
        if (!text.empty() && text.back() == '\0') text.pop_back();
        into = std::move(text);
        at += length;
        return true;
    };
    for (std::size_t index = 0; index < count; ++index) {
        // 16 item + 16 owner + 1 point + 16 of the four masks
        if (at + 49 > payload.size()) return std::nullopt;
        AttachmentRequest object;
        std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(at), 16, object.item_id.begin());
        std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(at) + 16, 16,
                    object.owner_id.begin());
        object.attachment_point = std::to_integer<std::uint8_t>(payload[at + 32]);
        object.item_flags = read_u32(at + 33);
        // GroupMask, EveryoneMask and NextOwnerMask follow. The viewer's own
        // comment calls them cruft the server ignores, and the permissions that
        // decide anything come from inventory, so they are skipped rather than
        // carried somewhere they might get believed.
        at += 49;
        if (!take_string(object.name)) return std::nullopt;
        if (!take_string(object.description)) return std::nullopt;
        result.objects.push_back(std::move(object));
    }
    return result;
}

// Low 397. One block, and unusually it carries no SessionID.
std::optional<DetachAttachmentIntoInv> decode_detach_attachment_into_inv(
    std::span<const std::byte> payload) {
    constexpr std::array<std::byte, 4> message_id{
        std::byte{0xff}, std::byte{0xff}, std::byte{0x01}, std::byte{0x8d}};
    if (payload.size() < 36 ||
        !std::equal(message_id.begin(), message_id.end(), payload.begin()))
        return std::nullopt;
    DetachAttachmentIntoInv result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.item_id.begin());
    return result;
}

std::optional<AssetUploadRequest> decode_asset_upload_request(std::span<const std::byte> payload) {
    constexpr std::array<std::byte, 4> message_id{
        std::byte{0xff}, std::byte{0xff}, std::byte{0x01}, std::byte{0x4d}};
    if (payload.size() < 25 ||
        !std::equal(message_id.begin(), message_id.end(), payload.begin()))
        return std::nullopt;
    const auto size = std::to_integer<std::size_t>(payload[23]) |
                      (std::to_integer<std::size_t>(payload[24]) << 8);
    if (payload.size() != 25 + size) return std::nullopt;
    AssetUploadRequest result;
    std::copy_n(payload.begin() + 4, 16, result.transaction_id.begin());
    result.asset_type = static_cast<std::int8_t>(std::to_integer<std::uint8_t>(payload[20]));
    result.temporary = payload[21] != std::byte{};
    result.store_local = payload[22] != std::byte{};
    result.data.assign(payload.begin() + 25, payload.end());
    return result;
}

std::vector<std::byte> encode_asset_upload_complete(const Uuid& asset_id,
                                                    std::int8_t asset_type, bool success) {
    std::vector<std::byte> output{
        std::byte{0xff}, std::byte{0xff}, std::byte{0x01}, std::byte{0x4e}};
    append_uuid(output, asset_id);
    output.push_back(static_cast<std::byte>(asset_type));
    output.push_back(success ? std::byte{1} : std::byte{});
    return output;
}

std::optional<UpdateInventoryAsset> decode_update_inventory_asset(
    std::span<const std::byte> payload) {
    constexpr std::array<std::byte, 4> message_id{
        std::byte{0xff}, std::byte{0xff}, std::byte{0x01}, std::byte{0x0a}};
    constexpr std::size_t block_offset = 53;
    constexpr std::size_t transaction_offset = block_offset + 105;
    constexpr std::size_t string_offset = block_offset + 132;
    if (payload.size() < string_offset + 10 ||
        !std::equal(message_id.begin(), message_id.end(), payload.begin()) ||
        payload[52] != std::byte{1})
        return std::nullopt;
    auto position = string_offset;
    for (int field = 0; field < 2; ++field) {
        if (position >= payload.size()) return std::nullopt;
        const auto size = std::to_integer<std::size_t>(payload[position++]);
        if (size == 0 || position + size > payload.size() ||
            payload[position + size - 1] != std::byte{})
            return std::nullopt;
        position += size;
    }
    if (position + 8 != payload.size()) return std::nullopt;
    UpdateInventoryAsset result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    std::copy_n(payload.begin() + block_offset, 16, result.item_id.begin());
    std::copy_n(payload.begin() + transaction_offset, 16, result.transaction_id.begin());
    return result;
}

std::vector<std::byte> encode_request_xfer(std::uint64_t id, const Uuid& asset_id,
                                           std::int16_t asset_type) {
    std::vector<std::byte> output{
        std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x9c}};
    append_le_u64(output, id);
    if (!append_variable1(output, "")) return {};
    output.push_back(std::byte{}); // file path
    output.push_back(std::byte{}); // delete on completion
    output.push_back(std::byte{}); // use big packets
    append_uuid(output, asset_id);
    append_le_u16(output, static_cast<std::uint16_t>(asset_type));
    return output;
}

std::optional<XferPacket> decode_send_xfer_packet(std::span<const std::byte> payload) {
    if (payload.size() < 15 || payload[0] != std::byte{18}) return std::nullopt;
    const auto size = std::to_integer<std::size_t>(payload[13]) |
                      (std::to_integer<std::size_t>(payload[14]) << 8);
    if (payload.size() != 15 + size) return std::nullopt;
    XferPacket result;
    result.id = read_le_u32(payload, 1) |
                (static_cast<std::uint64_t>(read_le_u32(payload, 5)) << 32);
    result.packet = read_le_u32(payload, 9);
    result.data.assign(payload.begin() + 15, payload.end());
    return result;
}

std::vector<std::byte> encode_confirm_xfer_packet(std::uint64_t id, std::uint32_t packet) {
    std::vector<std::byte> output{std::byte{19}};
    append_le_u64(output, id);
    append_le_u32(output, packet);
    return output;
}

std::optional<XferPacket> decode_confirm_xfer_packet(std::span<const std::byte> payload) {
    if (payload.size() != 13 || payload[0] != std::byte{19}) return std::nullopt;
    XferPacket result;
    result.id = read_le_u32(payload, 1) |
                (static_cast<std::uint64_t>(read_le_u32(payload, 5)) << 32);
    result.packet = read_le_u32(payload, 9);
    return result;
}

std::optional<RequestImage> decode_request_image(std::span<const std::byte> payload) {
    constexpr std::size_t header_size = 34;
    constexpr std::size_t block_size = 26;
    if (payload.size() < header_size || payload[0] != std::byte{8}) return std::nullopt;
    const auto count = std::to_integer<std::size_t>(payload[33]);
    if (count == 0 || payload.size() != header_size + count * block_size) return std::nullopt;
    RequestImage result;
    std::copy_n(payload.begin() + 1, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 17, 16, result.session_id.begin());
    result.requests.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto offset = header_size + index * block_size;
        ImageRequestBlock block;
        std::copy_n(payload.begin() + offset, 16, block.image_id.begin());
        block.discard_level = static_cast<std::int8_t>(std::to_integer<std::uint8_t>(payload[offset + 16]));
        block.download_priority = read_f32(payload, offset + 17);
        block.packet = read_le_u32(payload, offset + 21);
        block.type = std::to_integer<std::uint8_t>(payload[offset + 25]);
        result.requests.push_back(block);
    }
    return result;
}

std::vector<std::vector<std::byte>> encode_image_transfer(
    const Uuid& image_id, std::span<const std::byte> content, std::uint32_t start_packet) {
    constexpr std::size_t first_packet_size = 600;
    constexpr std::size_t image_packet_size = 1000;
    if (content.empty()) return {};
    const auto remaining = content.size() > first_packet_size ? content.size() - first_packet_size : 0;
    const auto packet_count = 1 + (remaining + image_packet_size - 1) / image_packet_size;
    if (packet_count > std::numeric_limits<std::uint16_t>::max() || start_packet >= packet_count) return {};
    std::vector<std::vector<std::byte>> output;
    if (start_packet == 0) {
        std::vector<std::byte> header{std::byte{9}};
        append_uuid(header, image_id);
        header.push_back(std::byte{2});
        append_le_u32(header, static_cast<std::uint32_t>(content.size()));
        append_le_u16(header, static_cast<std::uint16_t>(packet_count));
        const auto first_size = std::min(first_packet_size, content.size());
        append_binary(header, content.first(first_size), 2);
        output.push_back(std::move(header));
        start_packet = 1;
    }
    for (std::size_t packet = start_packet; packet < packet_count; ++packet) {
        const auto offset = first_packet_size + (packet - 1) * image_packet_size;
        const auto size = std::min(image_packet_size, content.size() - offset);
        std::vector<std::byte> payload{std::byte{10}};
        append_uuid(payload, image_id);
        append_le_u16(payload, static_cast<std::uint16_t>(packet));
        append_binary(payload, content.subspan(offset, size), 2);
        output.push_back(std::move(payload));
    }
    return output;
}

std::optional<AgentUpdate> decode_agent_update(std::span<const std::byte> payload) {
    if (payload.size() != 115 || payload[0] != std::byte{4}) return std::nullopt;
    AgentUpdate result;
    std::copy_n(payload.begin() + 1, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 17, 16, result.session_id.begin());
    result.body_rotation = read_vector3(payload, 33);
    result.head_rotation = read_vector3(payload, 45);
    result.state = std::to_integer<std::uint8_t>(payload[57]);
    result.camera_center = read_vector3(payload, 58);
    result.camera_at = read_vector3(payload, 70);
    result.camera_left = read_vector3(payload, 82);
    result.camera_up = read_vector3(payload, 94);
    result.draw_distance = read_f32(payload, 106);
    result.control_flags = read_le_u32(payload, 110);
    result.flags = std::to_integer<std::uint8_t>(payload[114]);
    return result;
}

// Low 88. AgentData only: agent, session, and the one bool.
std::optional<SetAlwaysRun> decode_set_always_run(std::span<const std::byte> payload) {
    constexpr std::array<std::byte, 4> message_id{
        std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x58}};
    if (payload.size() < 37 ||
        !std::equal(message_id.begin(), message_id.end(), payload.begin()))
        return std::nullopt;
    SetAlwaysRun result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    result.always_run = payload[36] != std::byte{0};
    return result;
}

std::optional<ModifyLand> decode_modify_land(std::span<const std::byte> payload) {
    constexpr std::size_t fixed_size = 47;
    constexpr std::size_t area_size = 20;
    if (payload.size() < fixed_size + 1 ||
        !std::equal(modify_land_id.begin(), modify_land_id.end(), payload.begin()))
        return std::nullopt;
    ModifyLand result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    result.action = std::to_integer<std::uint8_t>(payload[36]);
    result.brush_size = std::to_integer<std::uint8_t>(payload[37]);
    result.seconds = read_f32(payload, 38);
    result.height = read_f32(payload, 42);
    if (!std::isfinite(result.seconds) || !std::isfinite(result.height)) return std::nullopt;
    std::size_t offset = fixed_size;
    const auto area_count = std::to_integer<std::size_t>(payload[46]);
    if (area_count == 0 || area_count > 64 ||
        offset + area_count * area_size + 1 > payload.size())
        return std::nullopt;
    result.areas.reserve(area_count);
    for (std::size_t index = 0; index < area_count; ++index) {
        ModifyLandArea area;
        area.local_id = static_cast<std::int32_t>(read_le_u32(payload, offset));
        area.west = read_f32(payload, offset + 4);
        area.south = read_f32(payload, offset + 8);
        area.east = read_f32(payload, offset + 12);
        area.north = read_f32(payload, offset + 16);
        if (!std::isfinite(area.west) || !std::isfinite(area.south) ||
            !std::isfinite(area.east) || !std::isfinite(area.north))
            return std::nullopt;
        result.areas.push_back(area);
        offset += area_size;
    }
    const auto extended_count = std::to_integer<std::size_t>(payload[offset++]);
    if (extended_count > 64 || offset + extended_count * sizeof(float) != payload.size())
        return std::nullopt;
    result.extended_brush_sizes.reserve(extended_count);
    for (std::size_t index = 0; index < extended_count; ++index) {
        const auto radius = read_f32(payload, offset);
        if (!std::isfinite(radius)) return std::nullopt;
        result.extended_brush_sizes.push_back(radius);
        offset += sizeof(float);
    }
    return result;
}

std::optional<ParcelPropertiesRequest> decode_parcel_properties_request(
    std::span<const std::byte> payload) {
    constexpr std::size_t id = 2; // Medium-frequency 2-byte message number
    constexpr std::size_t size = id + 32 + 4 + 16 + 1;
    if (payload.size() < size ||
        !std::equal(parcel_properties_request_id.begin(), parcel_properties_request_id.end(),
                    payload.begin()))
        return std::nullopt;
    ParcelPropertiesRequest result;
    result.agent_id = read_uuid(payload, id);
    result.session_id = read_uuid(payload, id + 16);
    std::size_t offset = id + 32;
    result.sequence_id = static_cast<std::int32_t>(read_le_u32(payload, offset));
    result.west = read_f32(payload, offset + 4);
    result.south = read_f32(payload, offset + 8);
    result.east = read_f32(payload, offset + 12);
    result.north = read_f32(payload, offset + 16);
    result.snap_selection = payload[offset + 20] != std::byte{};
    if (!std::isfinite(result.west) || !std::isfinite(result.south) ||
        !std::isfinite(result.east) || !std::isfinite(result.north))
        return std::nullopt;
    return result;
}

std::optional<ParcelPropertiesRequestById> decode_parcel_properties_request_by_id(
    std::span<const std::byte> payload) {
    constexpr std::size_t id = 4;
    constexpr std::size_t size = id + 32 + 4 + 4;
    if (payload.size() < size ||
        !std::equal(parcel_properties_request_by_id_id.begin(),
                    parcel_properties_request_by_id_id.end(), payload.begin()))
        return std::nullopt;
    ParcelPropertiesRequestById result;
    result.agent_id = read_uuid(payload, id);
    result.session_id = read_uuid(payload, id + 16);
    result.sequence_id = static_cast<std::int32_t>(read_le_u32(payload, id + 32));
    result.local_id = static_cast<std::int32_t>(read_le_u32(payload, id + 36));
    return result;
}

std::optional<ParcelPropertiesUpdate> decode_parcel_properties_update(
    std::span<const std::byte> payload) {
    constexpr std::size_t id = 4;
    if (payload.size() < id + 32 + 12 ||
        !std::equal(parcel_properties_update_id.begin(), parcel_properties_update_id.end(),
                    payload.begin()))
        return std::nullopt;
    ParcelPropertiesUpdate result;
    result.agent_id = read_uuid(payload, id);
    result.session_id = read_uuid(payload, id + 16);
    std::size_t offset = id + 32;
    result.local_id = static_cast<std::int32_t>(read_le_u32(payload, offset)); offset += 4;
    result.flags = read_le_u32(payload, offset); offset += 4;
    result.parcel_flags = read_le_u32(payload, offset); offset += 4;
    result.sale_price = static_cast<std::int32_t>(read_le_u32(payload, offset)); offset += 4;
    const auto name = read_variable1(payload, offset);
    if (!name) return std::nullopt;
    result.name = name->first; offset = name->second;
    const auto description = read_variable1(payload, offset);
    if (!description) return std::nullopt;
    result.description = description->first; offset = description->second;
    const auto music = read_variable1(payload, offset);
    if (!music) return std::nullopt;
    result.music_url = music->first; offset = music->second;
    const auto media = read_variable1(payload, offset);
    if (!media) return std::nullopt;
    result.media_url = media->first; offset = media->second;
    if (offset + 16 + 1 + 16 + 4 + 4 + 1 + 16 + 16 + 12 + 12 + 1 > payload.size())
        return std::nullopt;
    result.media_id = read_uuid(payload, offset); offset += 16;
    result.media_auto_scale = std::to_integer<std::uint8_t>(payload[offset]); offset += 1;
    result.group_id = read_uuid(payload, offset); offset += 16;
    result.pass_price = static_cast<std::int32_t>(read_le_u32(payload, offset)); offset += 4;
    result.pass_hours = read_f32(payload, offset); offset += 4;
    result.category = std::to_integer<std::uint8_t>(payload[offset]); offset += 1;
    result.auth_buyer_id = read_uuid(payload, offset); offset += 16;
    result.snapshot_id = read_uuid(payload, offset); offset += 16;
    result.user_location = read_vector3(payload, offset); offset += 12;
    result.user_look_at = read_vector3(payload, offset); offset += 12;
    result.landing_type = std::to_integer<std::uint8_t>(payload[offset]);
    return result;
}

namespace {
std::optional<ParcelRectRequest> decode_parcel_rect(std::span<const std::byte> payload,
                                                    const std::array<std::byte, 4>& id) {
    constexpr std::size_t offset = 4;
    constexpr std::size_t size = offset + 32 + 16;
    if (payload.size() < size || !std::equal(id.begin(), id.end(), payload.begin()))
        return std::nullopt;
    ParcelRectRequest result;
    result.agent_id = read_uuid(payload, offset);
    result.session_id = read_uuid(payload, offset + 16);
    result.west = read_f32(payload, offset + 32);
    result.south = read_f32(payload, offset + 36);
    result.east = read_f32(payload, offset + 40);
    result.north = read_f32(payload, offset + 44);
    if (!std::isfinite(result.west) || !std::isfinite(result.south) ||
        !std::isfinite(result.east) || !std::isfinite(result.north))
        return std::nullopt;
    return result;
}
} // namespace

std::optional<ParcelRectRequest> decode_parcel_divide(std::span<const std::byte> payload) {
    return decode_parcel_rect(payload, parcel_divide_id);
}

std::optional<ParcelRectRequest> decode_parcel_join(std::span<const std::byte> payload) {
    return decode_parcel_rect(payload, parcel_join_id);
}

std::optional<ParcelAccessListRequest> decode_parcel_access_list_request(
    std::span<const std::byte> payload) {
    constexpr std::size_t id = 4;
    constexpr std::size_t size = id + 32 + 4 + 4 + 4;
    if (payload.size() < size ||
        !std::equal(parcel_access_list_request_id.begin(), parcel_access_list_request_id.end(),
                    payload.begin()))
        return std::nullopt;
    ParcelAccessListRequest result;
    result.agent_id = read_uuid(payload, id);
    result.session_id = read_uuid(payload, id + 16);
    result.sequence_id = static_cast<std::int32_t>(read_le_u32(payload, id + 32));
    result.flags = read_le_u32(payload, id + 36);
    result.local_id = static_cast<std::int32_t>(read_le_u32(payload, id + 40));
    return result;
}

std::optional<ParcelAccessListUpdate> decode_parcel_access_list_update(
    std::span<const std::byte> payload) {
    constexpr std::size_t id = 4;
    constexpr std::size_t header = id + 32 + 4 + 4 + 16 + 4 + 4;
    if (payload.size() < header + 1 ||
        !std::equal(parcel_access_list_update_id.begin(), parcel_access_list_update_id.end(),
                    payload.begin()))
        return std::nullopt;
    ParcelAccessListUpdate result;
    result.agent_id = read_uuid(payload, id);
    result.session_id = read_uuid(payload, id + 16);
    std::size_t offset = id + 32;
    result.flags = read_le_u32(payload, offset); offset += 4;
    result.local_id = static_cast<std::int32_t>(read_le_u32(payload, offset)); offset += 4;
    result.transaction_id = read_uuid(payload, offset); offset += 16;
    result.sequence_id = static_cast<std::int32_t>(read_le_u32(payload, offset)); offset += 4;
    result.sections = static_cast<std::int32_t>(read_le_u32(payload, offset)); offset += 4;
    const auto count = std::to_integer<std::size_t>(payload[offset++]);
    if (offset + count * 24 > payload.size()) return std::nullopt;
    result.entries.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        ParcelAccessListEntry entry;
        entry.id = read_uuid(payload, offset); offset += 16;
        entry.time = static_cast<std::int32_t>(read_le_u32(payload, offset)); offset += 4;
        entry.flags = read_le_u32(payload, offset); offset += 4;
        result.entries.push_back(entry);
    }
    return result;
}

std::vector<std::byte> encode_parcel_access_list_reply(const ParcelAccessListReply& message) {
    std::vector<std::byte> output(parcel_access_list_reply_id.begin(),
                                  parcel_access_list_reply_id.end());
    append_uuid(output, message.agent_id);
    append_le_u32(output, static_cast<std::uint32_t>(message.sequence_id));
    append_le_u32(output, message.flags);
    append_le_u32(output, static_cast<std::uint32_t>(message.local_id));
    // Variable list: a single-byte count. An empty list is transmitted as one
    // zero-UUID entry so the viewer clears its cached list (indra behaviour).
    if (message.entries.empty()) {
        output.push_back(std::byte{1});
        Uuid zero{};
        append_uuid(output, zero);
        append_le_u32(output, 0);
        append_le_u32(output, message.flags);
    } else {
        output.push_back(static_cast<std::byte>(std::min<std::size_t>(message.entries.size(), 255)));
        std::size_t emitted = 0;
        for (const auto& entry : message.entries) {
            if (emitted++ >= 255) break;
            append_uuid(output, entry.id);
            append_le_u32(output, static_cast<std::uint32_t>(entry.time));
            append_le_u32(output, entry.flags);
        }
    }
    return output;
}

std::vector<std::vector<std::byte>> encode_parcel_overlay(std::span<const std::uint8_t> cells) {
    constexpr std::size_t per_packet = 1024;
    std::vector<std::vector<std::byte>> packets;
    const std::size_t total = cells.size();
    if (total == 0) return packets;
    const std::size_t count = (total + per_packet - 1) / per_packet;
    for (std::size_t sequence = 0; sequence < count; ++sequence) {
        const std::size_t begin = sequence * per_packet;
        const std::size_t chunk = std::min(per_packet, total - begin);
        std::vector<std::byte> output(parcel_overlay_id.begin(), parcel_overlay_id.end());
        append_le_u32(output, static_cast<std::uint32_t>(sequence));
        append_le_u16(output, static_cast<std::uint16_t>(chunk)); // Data (Variable 2) length
        for (std::size_t index = 0; index < chunk; ++index)
            output.push_back(static_cast<std::byte>(cells[begin + index]));
        packets.push_back(std::move(output));
    }
    return packets;
}

std::optional<ParcelObjectOwnersRequest> decode_parcel_object_owners_request(
    std::span<const std::byte> payload) {
    constexpr std::size_t id = 4;
    constexpr std::size_t size = id + 32 + 4;
    if (payload.size() < size ||
        !std::equal(parcel_object_owners_request_id.begin(), parcel_object_owners_request_id.end(),
                    payload.begin()))
        return std::nullopt;
    ParcelObjectOwnersRequest result;
    result.agent_id = read_uuid(payload, id);
    result.session_id = read_uuid(payload, id + 16);
    result.local_id = static_cast<std::int32_t>(read_le_u32(payload, id + 32));
    return result;
}

std::vector<std::byte> encode_parcel_object_owners_reply(
    std::span<const ParcelObjectOwner> owners) {
    std::vector<std::byte> output(parcel_object_owners_reply_id.begin(),
                                  parcel_object_owners_reply_id.end());
    output.push_back(static_cast<std::byte>(std::min<std::size_t>(owners.size(), 255)));
    std::size_t emitted = 0;
    for (const auto& owner : owners) {
        if (emitted++ >= 255) break;
        append_uuid(output, owner.owner_id);
        output.push_back(static_cast<std::byte>(owner.is_group_owned ? 1 : 0));
        append_le_u32(output, static_cast<std::uint32_t>(owner.count));
        output.push_back(static_cast<std::byte>(owner.online ? 1 : 0));
    }
    return output;
}

namespace {
std::optional<std::pair<std::vector<Uuid>, std::size_t>> read_uuid_list(
    std::span<const std::byte> payload, std::size_t offset) {
    if (offset + 1 > payload.size()) return std::nullopt;
    const auto count = std::to_integer<std::size_t>(payload[offset++]);
    if (offset + count * 16 > payload.size()) return std::nullopt;
    std::vector<Uuid> ids;
    ids.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        ids.push_back(read_uuid(payload, offset));
        offset += 16;
    }
    return std::pair{std::move(ids), offset};
}
} // namespace

std::optional<ParcelSelectObjects> decode_parcel_select_objects(std::span<const std::byte> payload) {
    constexpr std::size_t id = 4;
    constexpr std::size_t header = id + 32 + 4 + 4;
    if (payload.size() < header + 1 ||
        !std::equal(parcel_select_objects_id.begin(), parcel_select_objects_id.end(),
                    payload.begin()))
        return std::nullopt;
    ParcelSelectObjects result;
    result.agent_id = read_uuid(payload, id);
    result.session_id = read_uuid(payload, id + 16);
    result.local_id = static_cast<std::int32_t>(read_le_u32(payload, id + 32));
    result.return_type = read_le_u32(payload, id + 36);
    const auto ids = read_uuid_list(payload, header);
    if (!ids) return std::nullopt;
    result.return_ids = std::move(ids->first);
    return result;
}

std::vector<std::vector<std::byte>> encode_force_object_select(
    std::span<const std::uint32_t> local_ids) {
    constexpr std::size_t per_packet = 251;
    std::vector<std::vector<std::byte>> packets;
    std::size_t offset = 0;
    bool reset = true;
    do {
        const std::size_t chunk = std::min(per_packet, local_ids.size() - offset);
        std::vector<std::byte> output(force_object_select_id.begin(), force_object_select_id.end());
        output.push_back(static_cast<std::byte>(reset ? 1 : 0)); // Header.ResetList
        output.push_back(static_cast<std::byte>(chunk));         // Data block count
        for (std::size_t index = 0; index < chunk; ++index)
            append_le_u32(output, local_ids[offset + index]);
        packets.push_back(std::move(output));
        offset += chunk;
        reset = false;
    } while (offset < local_ids.size());
    return packets;
}

std::optional<ParcelReturnObjects> decode_parcel_return_objects(std::span<const std::byte> payload) {
    constexpr std::size_t id = 4;
    constexpr std::size_t header = id + 32 + 4 + 4;
    if (payload.size() < header + 2 ||
        !std::equal(parcel_return_objects_id.begin(), parcel_return_objects_id.end(),
                    payload.begin()))
        return std::nullopt;
    ParcelReturnObjects result;
    result.agent_id = read_uuid(payload, id);
    result.session_id = read_uuid(payload, id + 16);
    result.local_id = static_cast<std::int32_t>(read_le_u32(payload, id + 32));
    result.return_type = read_le_u32(payload, id + 36);
    const auto tasks = read_uuid_list(payload, header);
    if (!tasks) return std::nullopt;
    result.task_ids = std::move(tasks->first);
    const auto owners = read_uuid_list(payload, tasks->second);
    if (!owners) return std::nullopt;
    result.owner_ids = std::move(owners->first);
    return result;
}

std::optional<RequestRegionInfo> decode_request_region_info(std::span<const std::byte> payload) {
    if (payload.size() < 4 + 32 ||
        !std::equal(request_region_info_id.begin(), request_region_info_id.end(), payload.begin()))
        return std::nullopt;
    RequestRegionInfo result;
    result.agent_id = read_uuid(payload, 4);
    result.session_id = read_uuid(payload, 20);
    return result;
}

std::vector<std::byte> encode_agent_alert_message(const Uuid& agent_id, bool modal,
                                                  std::string_view message) {
    std::vector<std::byte> output(agent_alert_message_id.begin(), agent_alert_message_id.end());
    append_uuid(output, agent_id);
    output.push_back(static_cast<std::byte>(modal ? 1 : 0));
    if (!append_variable1(output, message)) return {};
    return output;
}

std::optional<AgentMessage> decode_estate_covenant_request(std::span<const std::byte> payload) {
    if (payload.size() < 4 + 32 ||
        !std::equal(estate_covenant_request_id.begin(), estate_covenant_request_id.end(),
                    payload.begin()))
        return std::nullopt;
    AgentMessage result;
    result.agent_id = read_uuid(payload, 4);
    result.session_id = read_uuid(payload, 20);
    return result;
}

std::vector<std::byte> encode_estate_covenant_reply(const EstateCovenantReply& message) {
    std::vector<std::byte> output(estate_covenant_reply_id.begin(), estate_covenant_reply_id.end());
    append_uuid(output, message.covenant_id);
    append_le_u32(output, message.timestamp);
    if (!append_variable1(output, message.estate_name)) return {};
    append_uuid(output, message.estate_owner_id);
    return output;
}

std::vector<std::byte> encode_region_info(const RegionInfoReply& message) {
    std::vector<std::byte> output(region_info_id.begin(), region_info_id.end());
    append_uuid(output, message.agent_id);
    append_uuid(output, message.session_id);
    // RegionInfo block.
    if (!append_variable1(output, message.sim_name)) return {};
    append_le_u32(output, message.estate_id);
    append_le_u32(output, message.parent_estate_id);
    append_le_u32(output, message.region_flags);
    output.push_back(static_cast<std::byte>(message.sim_access));
    output.push_back(static_cast<std::byte>(message.max_agents));
    append_f32(output, message.billable_factor);
    append_f32(output, message.object_bonus_factor);
    append_f32(output, message.water_height);
    append_f32(output, message.terrain_raise_limit);
    append_f32(output, message.terrain_lower_limit);
    append_le_u32(output, static_cast<std::uint32_t>(message.price_per_meter));
    append_le_u32(output, static_cast<std::uint32_t>(message.redirect_grid_x));
    append_le_u32(output, static_cast<std::uint32_t>(message.redirect_grid_y));
    output.push_back(static_cast<std::byte>(message.use_estate_sun ? 1 : 0));
    append_f32(output, message.sun_hour);
    // RegionInfo2 block.
    if (!append_variable1(output, message.product_sku) ||
        !append_variable1(output, message.product_name))
        return {};
    append_le_u32(output, message.max_agents);      // MaxAgents32
    append_le_u32(output, message.max_agents);      // HardMaxAgents
    append_le_u32(output, 15000);                   // HardMaxObjects
    // RegionInfo3 (Variable): one entry with the extended flags.
    output.push_back(std::byte{1});
    append_le_u64(output, message.region_flags_extended != 0 ? message.region_flags_extended
                                                             : message.region_flags);
    // RegionInfo5 and CombatSettings variable blocks: none.
    output.push_back(std::byte{0});
    output.push_back(std::byte{0});
    return output;
}

std::optional<EstateOwnerMessage> decode_estate_owner_message(std::span<const std::byte> payload) {
    constexpr std::size_t id = 4;
    if (payload.size() < id + 48 + 1 ||
        !std::equal(estate_owner_message_id.begin(), estate_owner_message_id.end(), payload.begin()))
        return std::nullopt;
    EstateOwnerMessage result;
    result.agent_id = read_uuid(payload, id);
    result.session_id = read_uuid(payload, id + 16);
    result.transaction_id = read_uuid(payload, id + 32);
    std::size_t offset = id + 48;
    const auto method = read_variable1(payload, offset);
    if (!method) return std::nullopt;
    result.method = method->first;
    offset = method->second;
    if (offset + 16 + 1 > payload.size()) return std::nullopt;
    result.invoice = read_uuid(payload, offset);
    offset += 16;
    const auto count = std::to_integer<std::size_t>(payload[offset++]);
    result.params.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto param = read_variable1(payload, offset);
        if (!param) return std::nullopt;
        result.params.push_back(param->first);
        offset = param->second;
    }
    return result;
}

std::vector<std::byte> encode_estate_owner_message(const Uuid& agent_id, const Uuid& invoice,
                                                   std::string_view method,
                                                   std::span<const std::string> params) {
    std::vector<std::byte> output(estate_owner_message_id.begin(), estate_owner_message_id.end());
    append_uuid(output, agent_id);
    const Uuid zero{};
    append_uuid(output, zero); // SessionID (unused in replies)
    append_uuid(output, zero); // TransactionID
    if (!append_variable1(output, method)) return {};
    append_uuid(output, invoice);
    output.push_back(static_cast<std::byte>(std::min<std::size_t>(params.size(), 255)));
    std::size_t emitted = 0;
    for (const auto& param : params) {
        if (emitted++ >= 255) break;
        if (!append_variable1(output, param)) return {};
    }
    return output;
}

std::optional<ChatFromViewer> decode_chat_from_viewer(std::span<const std::byte> payload) {
    if (payload.size() < 43 || !std::equal(chat_from_viewer_id.begin(), chat_from_viewer_id.end(), payload.begin()))
        return std::nullopt;
    ChatFromViewer result;
    std::copy_n(payload.begin() + 4, 16, result.agent_id.begin());
    std::copy_n(payload.begin() + 20, 16, result.session_id.begin());
    const auto message = read_variable2(payload, 36);
    if (!message || message->second + 5 != payload.size()) return std::nullopt;
    result.message = message->first;
    result.type = std::to_integer<std::uint8_t>(payload[message->second]);
    result.channel = static_cast<std::int32_t>(read_le_u32(payload, message->second + 1));
    return result;
}

std::vector<std::byte> encode_chat_from_simulator(const ChatFromSimulator& message) {
    std::vector<std::byte> output(chat_from_simulator_id.begin(), chat_from_simulator_id.end());
    if (!append_variable1(output, message.from_name)) return {};
    append_uuid(output, message.source_id);
    append_uuid(output, message.owner_id);
    output.push_back(static_cast<std::byte>(message.source_type));
    output.push_back(static_cast<std::byte>(message.chat_type));
    output.push_back(static_cast<std::byte>(message.audible));
    for (const auto value : message.position) append_f32(output, value);
    if (!append_variable2(output, message.message)) return {};
    return output;
}

std::vector<std::byte> encode_flat_terrain(std::span<const TerrainPatch> patches, float height) {
    if (patches.empty() || patches.size() > 32 || !std::isfinite(height)) return {};
    BitWriter bits;
    bits.write_byte(0x08); bits.write_byte(0x01); // stride 264, little endian
    bits.write_byte(16); bits.write_byte(0x4c); // 16x16 land layer
    float offset = height - 0.5F;
    std::uint32_t offset_bits{};
    std::memcpy(&offset_bits, &offset, sizeof(offset_bits));
    for (const auto patch : patches) {
        if (patch.x >= 16 || patch.y >= 16) return {};
        bits.write_byte(0x84); // prequant 10, six-bit coefficient words
        bits.write_byte(static_cast<std::uint8_t>(offset_bits));
        bits.write_byte(static_cast<std::uint8_t>(offset_bits >> 8));
        bits.write_byte(static_cast<std::uint8_t>(offset_bits >> 16));
        bits.write_byte(static_cast<std::uint8_t>(offset_bits >> 24));
        bits.write_byte(1); bits.write_byte(0); // range 1
        // LLBitPack serializes a little-endian integer one byte at a time, so
        // this 10-bit field is its low byte followed by its upper two bits.
        const auto patch_id = (static_cast<std::uint32_t>(patch.x) << 5) | patch.y;
        bits.write_byte(static_cast<std::uint8_t>(patch_id));
        bits.write(patch_id >> 8, 2);
        bits.write(2, 2); // zero end-of-block
    }
    bits.write_byte(97); // end of patches
    const auto encoded = bits.finish();
    if (encoded.size() > 65535) return {};
    std::vector<std::byte> output{std::byte{11}, std::byte{0x4c}}; // LayerData, land
    const auto size = static_cast<std::uint16_t>(encoded.size());
    output.push_back(static_cast<std::byte>(size));
    output.push_back(static_cast<std::byte>(size >> 8));
    output.insert(output.end(), encoded.begin(), encoded.end());
    return output;
}

std::vector<std::byte> encode_terrain(std::span<const TerrainPatch> patches,
                                      std::span<const float> heightmap) {
    // The classic square encode: the whole heightmap is the window.
    const auto terrain_width = static_cast<std::size_t>(std::sqrt(heightmap.size()));
    if (terrain_width * terrain_width != heightmap.size()) return {};
    return encode_terrain_window(patches, heightmap, terrain_width, terrain_width, 0, 0,
                                 terrain_width);
}

std::vector<std::byte> encode_terrain_window(std::span<const TerrainPatch> patches,
                                             std::span<const float> heightmap,
                                             std::size_t heightmap_width,
                                             std::size_t heightmap_height,
                                             std::size_t window_x, std::size_t window_y,
                                             std::size_t window_edge) {
    const bool supported_edge = window_edge == 256 || window_edge == 512 || window_edge == 1024;
    const auto valid_dimension = [window_edge](std::size_t dimension) {
        return dimension % 256 == 0 && dimension >= window_edge;
    };
    if (patches.empty() || patches.size() > 32 || !supported_edge ||
        !valid_dimension(heightmap_width) || !valid_dimension(heightmap_height) ||
        heightmap_width * heightmap_height != heightmap.size() ||
        window_x % 16 != 0 || window_y % 16 != 0 ||
        window_x + window_edge > heightmap_width ||
        window_y + window_edge > heightmap_height ||
        !std::all_of(heightmap.begin(), heightmap.end(), [](float height) { return std::isfinite(height); }))
        return {};
    // The layer form is chosen by the window edge, since that is the region
    // size the viewer believes it is standing in.
    const bool extended = window_edge > 256;
    const auto layer_type = static_cast<std::uint8_t>(extended ? 0x4d : 0x4c);
    const auto patches_per_axis = window_edge / 16;
    BitWriter bits;
    bits.write_byte(0x08);
    bits.write_byte(0x01); // stride 264, little endian
    bits.write_byte(16);
    bits.write_byte(layer_type);
    for (const auto patch : patches) {
        if (patch.x >= patches_per_axis || patch.y >= patches_per_axis) return {};
        TerrainPatchHeader header;
        const auto coefficients = compress_terrain_patch(
            heightmap, patch.x, patch.y, heightmap_width, window_x, window_y, extended, header);
        const auto word_bits = write_terrain_patch_header(bits, header, coefficients, extended);
        write_terrain_coefficients(bits, coefficients, word_bits);
    }
    bits.write_byte(97);
    const auto encoded = bits.finish();
    if (encoded.size() > 65535) return {};
    std::vector<std::byte> output{std::byte{11}, static_cast<std::byte>(layer_type)};
    const auto size = static_cast<std::uint16_t>(encoded.size());
    output.push_back(static_cast<std::byte>(size));
    output.push_back(static_cast<std::byte>(size >> 8));
    output.insert(output.end(), encoded.begin(), encoded.end());
    return output;
}

std::vector<std::byte> encode_static_object_update(std::uint64_t region_handle, const StaticObject& object) {
    std::vector<std::byte> output{std::byte{12}}; // high-frequency ObjectUpdate
    append_le_u64(output, region_handle);
    append_le_u16(output, 65535); // full time dilation
    output.push_back(std::byte{1}); // one ObjectData block
    append_le_u32(output, object.local_id);
    output.push_back(static_cast<std::byte>(object.state));
    append_uuid(output, object.id);
    append_le_u32(output, 0); // CRC
    output.push_back(static_cast<std::byte>(object.pcode));
    output.push_back(static_cast<std::byte>(object.material));
    output.push_back(std::byte{}); // click action
    for (const auto value : object.scale) append_f32(output, value);
    std::vector<std::byte> transform;
    for (const auto value : object.position) append_f32(transform, value);
    for (const auto value : object.velocity) append_f32(transform, value);
    for (const auto value : object.acceleration) append_f32(transform, value);
    for (const auto value : object.rotation) append_f32(transform, value);
    for (int index = 0; index < 3; ++index) append_f32(transform, 0.0F); // angular velocity
    if (!append_binary(output, transform, 1)) return {};
    append_le_u32(output, object.parent_local_id);
    append_le_u32(output, object.update_flags);
    output.push_back(static_cast<std::byte>(object.path_curve));
    output.push_back(static_cast<std::byte>(object.profile_curve));
    append_le_u16(output, object.path_begin);
    append_le_u16(output, object.path_end);
    output.push_back(static_cast<std::byte>(object.path_scale_x));
    output.push_back(static_cast<std::byte>(object.path_scale_y));
    output.push_back(static_cast<std::byte>(object.path_shear_x));
    output.push_back(static_cast<std::byte>(object.path_shear_y));
    output.push_back(static_cast<std::byte>(object.path_twist));
    output.push_back(static_cast<std::byte>(object.path_twist_begin));
    output.push_back(static_cast<std::byte>(object.path_radius_offset));
    output.push_back(static_cast<std::byte>(object.path_taper_x));
    output.push_back(static_cast<std::byte>(object.path_taper_y));
    output.push_back(static_cast<std::byte>(object.path_revolutions));
    output.push_back(static_cast<std::byte>(object.path_skew));
    append_le_u16(output, object.profile_begin);
    append_le_u16(output, object.profile_end);
    append_le_u16(output, object.profile_hollow);
    // NameValue. One line, "Name TYPE CLASS SENDTO Value", newline terminated,
    // exactly as llnamevalue.cpp parses it.
    std::vector<std::byte> name_values;
    if (object.attachment_item_id != Uuid{}) {
        const auto line = "AttachItemID STRING RW SV " +
                          format_uuid(object.attachment_item_id) + "\n";
        name_values.reserve(line.size());
        for (const char character : line) name_values.push_back(static_cast<std::byte>(character));
    }
    if (!append_binary(output, object.texture_entry, 2) || !append_binary(output, {}, 1) ||
        !append_binary(output, name_values, 2)) return {}; // texture, animation, name/value
    const std::array<std::byte, 1> prim_count{std::byte{1}};
    if (!append_binary(output, prim_count, 2) || !append_binary(output, {}, 1)) return {}; // data, text
    output.insert(output.end(), 4, std::byte{}); // text color
    if (!append_binary(output, {}, 1) || !append_binary(output, {}, 1)) return {}; // media, particles
    // ExtraParams names a prim's shaping asset. Viewers require the dedicated
    // mesh parameter (0x60) for mesh prims — a sculpt parameter (0x30)
    // carrying sculpt type 5 is not treated as mesh — while true sculpts use
    // 0x30. Both carry the same 17-byte payload: shaping asset UUID + type
    // byte. Everything else sends an empty parameter list, as before.
    //
    // A second parameter rides here when faces carry glTF materials: the render
    // material block (0x80), which is `u8 count` then `u8 face` + UUID per
    // entry, capped at fourteen by the viewer's own packer. It is the only way a
    // face gets a material — `LLViewerObject::parameterChanged` routes
    // PARAMS_RENDER_MATERIAL to `setRenderMaterialIDs` — and the material is
    // what carries two-sidedness, which the TextureEntry has no field for.
    {
        std::vector<std::byte> extra;
        std::uint8_t parameters = 0;
        if (object.sculpt_id != Uuid{}) ++parameters;
        if (!object.face_materials.empty()) ++parameters;
        extra.push_back(static_cast<std::byte>(parameters));
        if (object.sculpt_id != Uuid{}) {
            const std::uint8_t parameter =
                object.sculpt_type == 5 ? std::uint8_t{0x60} : std::uint8_t{0x30};
            extra.push_back(static_cast<std::byte>(parameter)); // u16, little-endian
            extra.push_back(std::byte{0});
            extra.push_back(std::byte{17}); // payload size, u32 little-endian
            extra.insert(extra.end(), 3, std::byte{});
            append_uuid(extra, object.sculpt_id);
            extra.push_back(static_cast<std::byte>(object.sculpt_type));
        }
        if (!object.face_materials.empty()) {
            const auto entries = static_cast<std::uint8_t>(
                (std::min<std::size_t>)(object.face_materials.size(), 14));
            extra.push_back(std::byte{0x80}); // u16, little-endian
            extra.push_back(std::byte{0});
            const auto payload = static_cast<std::uint32_t>(1 + entries * 17);
            extra.push_back(static_cast<std::byte>(payload & 0xff)); // u32, little-endian
            extra.push_back(static_cast<std::byte>((payload >> 8) & 0xff));
            extra.push_back(static_cast<std::byte>((payload >> 16) & 0xff));
            extra.push_back(static_cast<std::byte>((payload >> 24) & 0xff));
            extra.push_back(static_cast<std::byte>(entries));
            for (std::uint8_t at = 0; at < entries; ++at) {
                const auto& [face, material] = object.face_materials[at];
                extra.push_back(static_cast<std::byte>(face));
                if (const auto id = parse_uuid(material)) append_uuid(extra, *id);
                else extra.insert(extra.end(), 16, std::byte{});
            }
        }
        if (!append_binary(output, extra, 1)) return {};
    }
    Uuid zero{};
    append_uuid(output, zero); append_uuid(output, object.owner_id); // sound and owner
    append_f32(output, 0.0F); output.push_back(std::byte{}); append_f32(output, 0.0F);
    output.push_back(std::byte{}); // joint type
    for (int index = 0; index < 6; ++index) append_f32(output, 0.0F);
    return output;
}

std::vector<std::byte> default_texture_entry(const Uuid& texture_id) {
    std::vector<std::byte> output;
    output.reserve(63);
    output.insert(output.end(), texture_id.begin(), texture_id.end());
    output.push_back(std::byte{}); // no per-face texture UUID overrides
    output.insert(output.end(), 4, std::byte{}); // inverted white RGBA
    output.push_back(std::byte{}); // no per-face color overrides
    append_f32(output, 1.0F);
    output.push_back(std::byte{}); // no per-face U repeat overrides
    append_f32(output, 1.0F);
    output.push_back(std::byte{}); // no per-face V repeat overrides
    for (int field = 0; field < 3; ++field) {
        output.insert(output.end(), 2, std::byte{}); // U offset, V offset, rotation
        output.push_back(std::byte{}); // no per-face overrides
    }
    for (int field = 0; field < 3; ++field) {
        output.push_back(std::byte{}); // material, media, glow
        output.push_back(std::byte{}); // no per-face overrides
    }
    output.insert(output.end(), 16, std::byte{}); // no render material UUID
    return output;
}

bool normalize_primitive_texture_entry(
    std::vector<std::byte>& texture_entry, std::span<const std::byte> default_entry) {
    if (default_entry.size() < 16) return false;
    if (texture_entry.size() < 16) {
        texture_entry.assign(default_entry.begin(), default_entry.end());
        return true;
    }
    constexpr std::array<std::byte, 16> viewer_default{
        std::byte{0xd2}, std::byte{0x11}, std::byte{0x44}, std::byte{0x04},
        std::byte{0xdd}, std::byte{0x59}, std::byte{0x4a}, std::byte{0x4d},
        std::byte{0x8e}, std::byte{0x6c}, std::byte{0x49}, std::byte{0x35},
        std::byte{0x9e}, std::byte{0x91}, std::byte{0xbb}, std::byte{0xf0}};
    const bool null_default = std::all_of(
        texture_entry.begin(), texture_entry.begin() + 16,
        [](std::byte value) { return value == std::byte{}; });
    const bool viewer_local_default = std::equal(
        viewer_default.begin(), viewer_default.end(), texture_entry.begin());
    if (!null_default && !viewer_local_default) return false;
    std::copy_n(default_entry.begin(), 16, texture_entry.begin());
    return true;
}

std::optional<std::array<Uuid, 32>> unpack_texture_entry_faces(
    std::span<const std::byte> texture_entry) {
    if (texture_entry.size() < 16) return std::nullopt;
    std::array<Uuid, 32> faces;
    Uuid default_id;
    std::copy_n(texture_entry.begin(), 16, default_id.begin());
    faces.fill(default_id);
    std::size_t position = 16;
    while (position < texture_entry.size()) {
        std::uint32_t face_bits = 0;
        unsigned bit_count = 0;
        std::uint8_t value{};
        do {
            if (position >= texture_entry.size() || bit_count >= 32) return std::nullopt;
            value = std::to_integer<std::uint8_t>(texture_entry[position++]);
            face_bits = (face_bits << 7) | (value & 0x7f);
            bit_count += 7;
        } while (value & 0x80);
        if (face_bits == 0) break;
        if (position + 16 > texture_entry.size()) return std::nullopt;
        Uuid texture_id;
        std::copy_n(texture_entry.begin() + position, 16, texture_id.begin());
        position += 16;
        for (unsigned face = 0; face < 32; ++face)
            if (face_bits & (std::uint32_t{1} << face)) faces[face] = texture_id;
    }
    return faces;
}

std::vector<std::byte> encode_avatar_texture_entry(const std::array<Uuid, 32>& faces,
                                                   const Uuid& default_id) {
    std::vector<std::byte> output;

    // Variable-length face bitmap: 7 bits per byte, most-significant group
    // first, with the continuation bit (0x80) on every byte but the last. This
    // is the inverse of the accumulation loop in unpack_texture_entry_faces.
    auto append_face_bitmap = [&output](std::uint32_t bits) {
        int chunks = 1;
        for (std::uint32_t rest = bits >> 7; rest != 0; rest >>= 7) ++chunks;
        for (int chunk = chunks - 1; chunk >= 0; --chunk) {
            auto seven = static_cast<std::uint8_t>((bits >> (7 * chunk)) & 0x7f);
            if (chunk != 0) seven |= 0x80;
            output.push_back(static_cast<std::byte>(seven));
        }
    };

    // TextureID section: default UUID, then one grouped override per distinct
    // non-default UUID, then a zero bitmap to terminate the section.
    output.insert(output.end(), default_id.begin(), default_id.end());
    std::array<bool, 32> emitted{};
    for (unsigned face = 0; face < 32; ++face) {
        if (emitted[face] || faces[face] == default_id) {
            emitted[face] = true;
            continue;
        }
        std::uint32_t bitmap = 0;
        for (unsigned other = face; other < 32; ++other) {
            if (!emitted[other] && faces[other] == faces[face]) {
                bitmap |= (std::uint32_t{1} << other);
                emitted[other] = true;
            }
        }
        append_face_bitmap(bitmap);
        output.insert(output.end(), faces[face].begin(), faces[face].end());
    }
    output.push_back(std::byte{});  // terminate TextureID section

    // Remaining attribute sections use canonical defaults, matching the layout
    // of default_texture_entry so viewers parse the whole blob.
    output.insert(output.end(), 4, std::byte{});  // inverted white RGBA
    output.push_back(std::byte{});                // no color overrides
    append_f32(output, 1.0F);                     // repeat U
    output.push_back(std::byte{});
    append_f32(output, 1.0F);  // repeat V
    output.push_back(std::byte{});
    for (int field = 0; field < 3; ++field) {  // offset U, offset V, rotation
        output.insert(output.end(), 2, std::byte{});
        output.push_back(std::byte{});
    }
    for (int field = 0; field < 3; ++field) {  // material, media, glow
        output.push_back(std::byte{});
        output.push_back(std::byte{});
    }
    output.insert(output.end(), 16, std::byte{});  // render material UUID
    return output;
}

std::vector<std::byte> encode_avatar_object_update(std::uint64_t region_handle, std::uint32_t local_id,
                                                   const Uuid& agent_id,
                                                   std::array<float, 3> position,
                                                   std::array<float, 3> velocity,
                                                   std::array<float, 3> rotation) {
    StaticObject avatar;
    avatar.local_id = local_id;
    avatar.id = agent_id;
    avatar.owner_id = agent_id;
    avatar.pcode = 47; // avatar
    avatar.material = 4; // flesh
    avatar.position = position;
    avatar.velocity = velocity;
    avatar.rotation = rotation;
    avatar.scale = {0.45F, 0.60F, 1.90F};
    return encode_static_object_update(region_handle, avatar);
}

std::vector<std::byte> encode_packet_ack(std::span<const std::uint32_t> sequences) {
    if (sequences.empty() || sequences.size() > 255) return {};
    std::vector<std::byte> output(packet_ack_id.begin(), packet_ack_id.end());
    output.reserve(5 + sequences.size() * 4);
    output.push_back(static_cast<std::byte>(sequences.size()));
    for (const auto sequence : sequences) append_le_u32(output, sequence);
    return output;
}

std::optional<std::vector<std::uint32_t>> decode_packet_ack(std::span<const std::byte> payload) {
    if (payload.size() < 9 || !std::equal(packet_ack_id.begin(), packet_ack_id.end(), payload.begin()))
        return std::nullopt;
    const auto count = std::to_integer<std::size_t>(payload[4]);
    if (count == 0 || payload.size() != 5 + count * 4) return std::nullopt;
    std::vector<std::uint32_t> result;
    result.reserve(count);
    for (std::size_t offset = 5; offset < payload.size(); offset += 4) result.push_back(read_le_u32(payload, offset));
    return result;
}

std::vector<std::byte> encode_packet(const Packet& packet) {
    if (packet.extra_header.size() > 255 || packet.acknowledgements.size() > 255) return {};
    auto flags = packet.flags;
    if (!packet.acknowledgements.empty()) flags |= flag_appended_acks;
    std::vector<std::byte> output;
    output.reserve(6 + packet.extra_header.size() + packet.payload.size() + packet.acknowledgements.size() * 4 + 1);
    output.push_back(static_cast<std::byte>(flags));
    append_be_u32(output, packet.sequence);
    output.push_back(static_cast<std::byte>(packet.extra_header.size()));
    output.insert(output.end(), packet.extra_header.begin(), packet.extra_header.end());
    const auto encoded = (flags & flag_zero_coded) ? zero_encode(packet.payload) : packet.payload;
    output.insert(output.end(), encoded.begin(), encoded.end());
    for (const auto acknowledgement : packet.acknowledgements) append_be_u32(output, acknowledgement);
    if (!packet.acknowledgements.empty()) output.push_back(static_cast<std::byte>(packet.acknowledgements.size()));
    return output;
}

std::optional<Packet> decode_packet(std::span<const std::byte> datagram) {
    if (datagram.size() < 6) return std::nullopt;
    Packet packet;
    packet.flags = std::to_integer<std::uint8_t>(datagram[0]);
    packet.sequence = read_be_u32(datagram, 1);
    const auto extra_size = std::to_integer<std::size_t>(datagram[5]);
    if (6 + extra_size > datagram.size()) return std::nullopt;
    packet.extra_header.assign(datagram.begin() + 6, datagram.begin() + 6 + extra_size);
    std::size_t payload_end = datagram.size();
    if (packet.flags & flag_appended_acks) {
        if (payload_end < 7) return std::nullopt;
        const auto count = std::to_integer<std::size_t>(datagram[payload_end - 1]);
        if (count == 0 || count > (payload_end - 7) / 4) return std::nullopt;
        const auto ack_start = payload_end - 1 - count * 4;
        for (std::size_t offset = ack_start; offset < payload_end - 1; offset += 4)
            packet.acknowledgements.push_back(read_be_u32(datagram, offset));
        payload_end = ack_start;
    }
    const auto encoded = datagram.subspan(6 + extra_size, payload_end - 6 - extra_size);
    if (packet.flags & flag_zero_coded) {
        auto decoded = zero_decode(encoded);
        if (!decoded) return std::nullopt;
        packet.payload = std::move(*decoded);
    } else {
        packet.payload.assign(encoded.begin(), encoded.end());
    }
    return packet;
}

Circuit::Circuit(Clock::time_point now, double bytes_per_second, std::chrono::seconds idle_timeout)
    : last_activity_(now), token_time_(now), rate_(std::max(1.0, bytes_per_second)),
      capacity_(std::max(1200.0, bytes_per_second)), tokens_(capacity_), idle_timeout_(idle_timeout) {}

std::optional<std::vector<std::byte>> Circuit::send(std::vector<std::byte> payload, bool reliable,
                                                    Clock::time_point now, bool zero_coded) {
    Packet packet;
    packet.flags = (reliable ? flag_reliable : 0) | (zero_coded ? flag_zero_coded : 0);
    packet.sequence = next_sequence_++;
    packet.payload = std::move(payload);
    packet.acknowledgements = take_acks();
    auto datagram = encode_packet(packet);
    if (datagram.empty() || !consume(datagram.size(), now)) {
        queued_acks_.insert(queued_acks_.begin(), packet.acknowledgements.begin(), packet.acknowledgements.end());
        return std::nullopt;
    }
    if (reliable) pending_.emplace(packet.sequence, Pending{packet, now});
    last_activity_ = now;
    return datagram;
}

std::optional<Packet> Circuit::receive(std::span<const std::byte> datagram, Clock::time_point now) {
    auto packet = decode_packet(datagram);
    if (!packet) return std::nullopt;
    last_activity_ = now;
    for (const auto acknowledgement : packet->acknowledgements) pending_.erase(acknowledgement);
    if (const auto acknowledgements = decode_packet_ack(packet->payload))
        for (const auto acknowledgement : *acknowledgements) pending_.erase(acknowledgement);
    if (packet->flags & flag_reliable) {
        if (std::find(queued_acks_.begin(), queued_acks_.end(), packet->sequence) == queued_acks_.end())
            queued_acks_.push_back(packet->sequence);
        if (!received_reliable_.insert(packet->sequence).second) return std::nullopt;
        if (received_reliable_.size() > 4096) received_reliable_.clear();
    }
    return packet;
}

std::vector<std::vector<std::byte>> Circuit::poll(Clock::time_point now) {
    std::vector<std::vector<std::byte>> output;
    constexpr auto resend_after = std::chrono::milliseconds(500);
    for (auto& [sequence, pending] : pending_) {
        static_cast<void>(sequence);
        if (now - pending.sent_at < resend_after || pending.attempts >= 5) continue;
        auto resent = pending.packet;
        resent.flags |= flag_resent;
        resent.acknowledgements = take_acks();
        auto datagram = encode_packet(resent);
        if (!consume(datagram.size(), now)) {
            queued_acks_.insert(queued_acks_.begin(), resent.acknowledgements.begin(), resent.acknowledgements.end());
            continue;
        }
        pending.sent_at = now;
        ++pending.attempts;
        output.push_back(std::move(datagram));
    }
    if (output.empty() && !queued_acks_.empty()) {
        Packet ack;
        ack.sequence = next_sequence_++;
        const auto acknowledgements = take_acks();
        ack.payload = encode_packet_ack(acknowledgements);
        auto datagram = encode_packet(ack);
        if (consume(datagram.size(), now)) output.push_back(std::move(datagram));
        else queued_acks_.insert(queued_acks_.begin(), acknowledgements.begin(), acknowledgements.end());
    }
    return output;
}

bool Circuit::expired(Clock::time_point now) const { return now - last_activity_ > idle_timeout_; }

bool Circuit::consume(std::size_t bytes, Clock::time_point now) {
    const auto elapsed = std::chrono::duration<double>(now - token_time_).count();
    tokens_ = std::min(capacity_, tokens_ + std::max(0.0, elapsed) * rate_);
    token_time_ = now;
    if (tokens_ < static_cast<double>(bytes)) return false;
    tokens_ -= static_cast<double>(bytes);
    return true;
}

std::vector<std::uint32_t> Circuit::take_acks() {
    const auto count = std::min<std::size_t>(queued_acks_.size(), 255);
    std::vector<std::uint32_t> result(queued_acks_.begin(), queued_acks_.begin() + count);
    queued_acks_.erase(queued_acks_.begin(), queued_acks_.begin() + count);
    return result;
}

std::string facet_endpoint_key(std::string endpoint, int facet) {
    if (facet <= 0 || endpoint.empty()) return endpoint;
    return endpoint + "/f" + std::to_string(facet);
}

int endpoint_facet(std::string_view endpoint) {
    const auto marker = endpoint.rfind("/f");
    if (marker == std::string_view::npos) return 0;
    int facet{};
    const auto text = endpoint.substr(marker + 2);
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), facet);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || facet < 0) return 0;
    return facet;
}

std::string_view endpoint_transport(std::string_view endpoint) {
    const auto marker = endpoint.rfind("/f");
    return marker == std::string_view::npos ? endpoint : endpoint.substr(0, marker);
}

std::optional<Packet> CircuitRegistry::receive(std::string_view endpoint, std::span<const std::byte> datagram,
                                               Clock::time_point now) {
    auto found = circuits_.find(std::string(endpoint));
    if (found == circuits_.end()) {
        const auto packet = decode_packet(datagram);
        if (!packet || !(packet->flags & flag_reliable)) return std::nullopt;
        const auto requested = decode_use_circuit_code(packet->payload);
        if (!requested) return std::nullopt;
        bool authorized = false;
        try {
            authorized = authorizer_ && authorizer_(*requested);
        } catch (...) {
            return std::nullopt;
        }
        if (!authorized) return std::nullopt;
        for (auto iterator = circuits_.begin(); iterator != circuits_.end();) {
            const auto& entry = iterator->second;
            const bool same_identity = entry.identity.circuit_code == requested->circuit_code ||
                entry.identity.session_id == requested->session_id || entry.identity.agent_id == requested->agent_id;
            // A matching identity on the same transport but a different facet is
            // a child circuit (ADR 0036): one viewer holds a circuit per facet,
            // and they coexist. Evict only a genuine relogin from a new address
            // (different transport takes every facet with it) or a reconnect to
            // the same facet.
            const bool coexisting_facet = same_identity &&
                endpoint_transport(iterator->first) == endpoint_transport(endpoint) &&
                endpoint_facet(iterator->first) != endpoint_facet(endpoint);
            if (same_identity && !coexisting_facet) {
                replaced_.push_back(ReplacedCircuit{iterator->first, entry.identity});
                iterator = circuits_.erase(iterator);
            } else {
                ++iterator;
            }
        }
        found = circuits_.emplace(std::string(endpoint), Entry{*requested, Circuit(now)}).first;
    }
    return found->second.circuit.receive(datagram, now);
}

std::optional<std::vector<std::byte>> CircuitRegistry::send(std::string_view endpoint,
                                                            std::vector<std::byte> payload, bool reliable,
                                                            Clock::time_point now, bool zero_coded) {
    const auto found = circuits_.find(std::string(endpoint));
    if (found == circuits_.end()) return std::nullopt;
    return found->second.circuit.send(std::move(payload), reliable, now, zero_coded);
}

std::vector<OutboundDatagram> CircuitRegistry::poll(Clock::time_point now) {
    std::vector<OutboundDatagram> output;
    for (auto iterator = circuits_.begin(); iterator != circuits_.end();) {
        if (iterator->second.circuit.expired(now)) {
            iterator = circuits_.erase(iterator);
            continue;
        }
        auto datagrams = iterator->second.circuit.poll(now);
        for (auto& datagram : datagrams) output.push_back({iterator->first, std::move(datagram)});
        ++iterator;
    }
    return output;
}

std::vector<ReplacedCircuit> CircuitRegistry::take_replaced() {
    auto replaced = std::move(replaced_);
    replaced_.clear();
    return replaced;
}

const UseCircuitCode* CircuitRegistry::identity(std::string_view endpoint) const {
    const auto found = circuits_.find(std::string(endpoint));
    return found == circuits_.end() ? nullptr : &found->second.identity;
}

bool CircuitRegistry::remove(std::string_view endpoint) {
    return circuits_.erase(std::string(endpoint)) != 0;
}

} // namespace homeworldz::viewer
