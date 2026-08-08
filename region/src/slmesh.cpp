#include "homeworldz/slmesh.h"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>

namespace homeworldz::slmesh {
namespace {

// --- binary LLSD, the subset the mesh asset uses -----------------------------
//
// Tags are single bytes; sizes and integers are big-endian 32-bit; reals are
// big-endian IEEE doubles; the payload bytes of "binary" values are opaque
// (the mesh format packs little-endian 16-bit quantities inside them, per the
// viewer's readers).

void put_u32(std::vector<std::byte>& out, std::uint32_t value) {
    out.push_back(static_cast<std::byte>(value >> 24));
    out.push_back(static_cast<std::byte>(value >> 16));
    out.push_back(static_cast<std::byte>(value >> 8));
    out.push_back(static_cast<std::byte>(value));
}

void put_real(std::vector<std::byte>& out, double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    out.push_back(static_cast<std::byte>('r'));
    for (int shift = 56; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::byte>(bits >> shift));
}

void put_integer(std::vector<std::byte>& out, std::int32_t value) {
    out.push_back(static_cast<std::byte>('i'));
    put_u32(out, static_cast<std::uint32_t>(value));
}

void put_key(std::vector<std::byte>& out, std::string_view key) {
    out.push_back(static_cast<std::byte>('k'));
    put_u32(out, static_cast<std::uint32_t>(key.size()));
    for (const auto character : key) out.push_back(static_cast<std::byte>(character));
}

// 's' then a length then the bytes, the same shape as a key. Distinct from a
// key because LLSD's binary form distinguishes the two, and the viewer's reader
// switches on the tag.
void put_string(std::vector<std::byte>& out, std::string_view value) {
    out.push_back(static_cast<std::byte>('s'));
    put_u32(out, static_cast<std::uint32_t>(value.size()));
    for (const auto character : value) out.push_back(static_cast<std::byte>(character));
}

// A bare tag byte and no payload: '1' is true and '0' is false.
void put_boolean(std::vector<std::byte>& out, bool value) {
    out.push_back(static_cast<std::byte>(value ? '1' : '0'));
}

void put_binary(std::vector<std::byte>& out, std::span<const std::byte> payload) {
    out.push_back(static_cast<std::byte>('b'));
    put_u32(out, static_cast<std::uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
}

void open_map(std::vector<std::byte>& out, std::uint32_t entries) {
    out.push_back(static_cast<std::byte>('{'));
    put_u32(out, entries);
}
void close_map(std::vector<std::byte>& out) { out.push_back(static_cast<std::byte>('}')); }
void open_array(std::vector<std::byte>& out, std::uint32_t entries) {
    out.push_back(static_cast<std::byte>('['));
    put_u32(out, entries);
}
void close_array(std::vector<std::byte>& out) { out.push_back(static_cast<std::byte>(']')); }

// A tolerant reader over the same subset. It carries a cursor and fails by
// returning nullopt/false; the callers treat any failure as "not a mesh".
struct Reader {
    std::span<const std::byte> data;
    std::size_t at{};

    std::optional<std::uint8_t> peek() const {
        if (at >= data.size()) return std::nullopt;
        return std::to_integer<std::uint8_t>(data[at]);
    }
    std::optional<std::uint32_t> u32() {
        if (at + 4 > data.size()) return std::nullopt;
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index)
            value = (value << 8) | std::to_integer<std::uint32_t>(data[at + index]);
        at += 4;
        return value;
    }
    bool expect(char tag) {
        const auto next = peek();
        if (!next || *next != static_cast<std::uint8_t>(tag)) return false;
        ++at;
        return true;
    }
    std::optional<double> real() {
        if (!expect('r') || at + 8 > data.size()) return std::nullopt;
        std::uint64_t bits = 0;
        for (int index = 0; index < 8; ++index)
            bits = (bits << 8) | std::to_integer<std::uint64_t>(data[at + index]);
        at += 8;
        double value = 0;
        std::memcpy(&value, &bits, sizeof value);
        return value;
    }
    std::optional<std::int32_t> integer() {
        if (!expect('i')) return std::nullopt;
        const auto value = u32();
        if (!value) return std::nullopt;
        return static_cast<std::int32_t>(*value);
    }
    std::optional<std::string> key() {
        if (!expect('k')) return std::nullopt;
        const auto size = u32();
        if (!size || at + *size > data.size()) return std::nullopt;
        std::string value(reinterpret_cast<const char*>(data.data() + at), *size);
        at += *size;
        return value;
    }
    // 's' then a length then the bytes. Same shape as a key, different tag,
    // because LLSD's binary form distinguishes the two.
    std::optional<std::string> string() {
        if (!expect('s')) return std::nullopt;
        const auto size = u32();
        if (!size || at + *size > data.size()) return std::nullopt;
        std::string value(reinterpret_cast<const char*>(data.data() + at), *size);
        at += *size;
        return value;
    }
    std::optional<bool> boolean() {
        const auto next = peek();
        if (!next || (*next != '1' && *next != '0')) return std::nullopt;
        ++at;
        return *next == '1';
    }
    std::optional<std::span<const std::byte>> binary() {
        if (!expect('b')) return std::nullopt;
        const auto size = u32();
        if (!size || at + *size > data.size()) return std::nullopt;
        const auto payload = data.subspan(at, *size);
        at += *size;
        return payload;
    }
    // skip consumes one value of any subset type, for unknown map entries.
    bool skip() {
        const auto next = peek();
        if (!next) return false;
        switch (*next) {
        case 'r': return real().has_value();
        case 'i': return integer().has_value();
        case 'b': return binary().has_value();
        case '1': case '0': case '!': ++at; return true;
        case '{': {
            ++at;
            const auto entries = u32();
            if (!entries) return false;
            for (std::uint32_t index = 0; index < *entries; ++index)
                if (!key() || !skip()) return false;
            return expect('}');
        }
        case '[': {
            ++at;
            const auto entries = u32();
            if (!entries) return false;
            for (std::uint32_t index = 0; index < *entries; ++index)
                if (!skip()) return false;
            return expect(']');
        }
        default: return false;
        }
    }
};

// --- quantization -------------------------------------------------------------

void put_u16_le(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>(value & 0xff));
    out.push_back(static_cast<std::byte>(value >> 8));
}

std::uint16_t quantize(float value, float low, float high) {
    if (!(high > low)) return 0;
    const float unit = (value - low) / (high - low);
    const float clamped = std::clamp(unit, 0.0f, 1.0f);
    return static_cast<std::uint16_t>(std::lround(clamped * 65535.0f));
}

float dequantize(std::uint16_t value, float low, float high) {
    return low + (static_cast<float>(value) / 65535.0f) * (high - low);
}

std::uint16_t read_u16_le(std::span<const std::byte> data, std::size_t index) {
    return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(data[index]) |
                                      (std::to_integer<std::uint16_t>(data[index + 1]) << 8));
}

struct Bounds3 {
    std::array<float, 3> low{};
    std::array<float, 3> high{};
};

Bounds3 position_bounds(const Level& level) {
    Bounds3 bounds{{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                    std::numeric_limits<float>::max()},
                   {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                    std::numeric_limits<float>::lowest()}};
    for (const auto& submesh : level)
        for (const auto& position : submesh.positions)
            for (int axis = 0; axis < 3; ++axis) {
                bounds.low[axis] = (std::min)(bounds.low[axis], position[axis]);
                bounds.high[axis] = (std::max)(bounds.high[axis], position[axis]);
            }
    for (int axis = 0; axis < 3; ++axis)
        if (!(bounds.high[axis] > bounds.low[axis])) bounds.high[axis] = bounds.low[axis] + 0.001f;
    return bounds;
}

// --- blocks --------------------------------------------------------------------

std::vector<std::byte> compress(std::span<const std::byte> raw) {
    uLongf capacity = compressBound(static_cast<uLong>(raw.size()));
    std::vector<std::byte> out(capacity);
    if (::compress(reinterpret_cast<Bytef*>(out.data()), &capacity,
                   reinterpret_cast<const Bytef*>(raw.data()),
                   static_cast<uLong>(raw.size())) != Z_OK)
        return {};
    out.resize(capacity);
    return out;
}

std::vector<std::byte> decompress(std::span<const std::byte> packed) {
    // Blocks are small by construction (u16 geometry); grow-and-retry keeps
    // the reader free of a size header the format does not carry.
    for (std::size_t capacity = (std::max<std::size_t>)(packed.size() * 8, 1 << 16);
         capacity <= (1u << 26); capacity *= 4) {
        std::vector<std::byte> out(capacity);
        uLongf produced = static_cast<uLongf>(capacity);
        const auto result = ::uncompress(reinterpret_cast<Bytef*>(out.data()), &produced,
                                         reinterpret_cast<const Bytef*>(packed.data()),
                                         static_cast<uLong>(packed.size()));
        if (result == Z_OK) {
            out.resize(produced);
            return out;
        }
        if (result != Z_BUF_ERROR) break;
    }
    return {};
}

std::vector<std::byte> encode_level(const Level& level) {
    std::vector<std::byte> out;
    open_array(out, static_cast<std::uint32_t>(level.size()));
    for (const auto& submesh : level) {
        const auto has_normals = !submesh.normals.empty();
        const auto has_texcoords = !submesh.texcoords.empty();
        Bounds3 bounds = position_bounds({submesh});
        std::array<float, 2> texcoord_low{0.0f, 0.0f}, texcoord_high{1.0f, 1.0f};
        if (has_texcoords) {
            texcoord_low = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
            texcoord_high = {std::numeric_limits<float>::lowest(),
                             std::numeric_limits<float>::lowest()};
            for (const auto& texcoord : submesh.texcoords)
                for (int axis = 0; axis < 2; ++axis) {
                    texcoord_low[axis] = (std::min)(texcoord_low[axis], texcoord[axis]);
                    texcoord_high[axis] = (std::max)(texcoord_high[axis], texcoord[axis]);
                }
            for (int axis = 0; axis < 2; ++axis)
                if (!(texcoord_high[axis] > texcoord_low[axis]))
                    texcoord_high[axis] = texcoord_low[axis] + 0.001f;
        }
        const bool has_influences = !submesh.influences.empty();
        std::uint32_t entries = 3; // PositionDomain, Position, TriangleList
        if (has_normals) ++entries;
        if (has_texcoords) entries += 2;
        if (has_influences) ++entries; // Weights
        open_map(out, entries);

        put_key(out, "PositionDomain");
        open_map(out, 2);
        put_key(out, "Min");
        open_array(out, 3);
        for (int axis = 0; axis < 3; ++axis) put_real(out, bounds.low[axis]);
        close_array(out);
        put_key(out, "Max");
        open_array(out, 3);
        for (int axis = 0; axis < 3; ++axis) put_real(out, bounds.high[axis]);
        close_array(out);
        close_map(out);

        std::vector<std::byte> packed;
        packed.reserve(submesh.positions.size() * 6);
        for (const auto& position : submesh.positions)
            for (int axis = 0; axis < 3; ++axis)
                put_u16_le(packed, quantize(position[axis], bounds.low[axis], bounds.high[axis]));
        put_key(out, "Position");
        put_binary(out, packed);

        if (has_influences) {
            // One vertex at a time: a byte of joint index and a 16-bit weight
            // per influence, then 0xFF to end the list when fewer than four
            // were written. The terminator is omitted at exactly four because
            // the reader stops counting there, which is the format's own rule
            // and not a saving worth making.
            std::vector<std::byte> weights;
            for (std::size_t vertex = 0; vertex < submesh.positions.size(); ++vertex) {
                const auto& list = vertex < submesh.influences.size() ?
                    submesh.influences[vertex] : std::vector<Influence>{};
                std::size_t written = 0;
                for (const auto& influence : list) {
                    if (written == 4) break;
                    if (influence.joint >= 255) continue;
                    weights.push_back(static_cast<std::byte>(influence.joint));
                    const auto scaled = static_cast<std::uint16_t>(
                        (std::max)(0.0F, (std::min)(1.0F, influence.weight)) * 65535.0F);
                    weights.push_back(static_cast<std::byte>(scaled & 0xff));
                    weights.push_back(static_cast<std::byte>(scaled >> 8));
                    ++written;
                }
                if (written < 4) weights.push_back(static_cast<std::byte>(0xff));
            }
            put_key(out, "Weights");
            put_binary(out, weights);
        }

        if (has_normals) {
            packed.clear();
            for (const auto& normal : submesh.normals)
                for (int axis = 0; axis < 3; ++axis)
                    put_u16_le(packed, quantize(normal[axis], -1.0f, 1.0f));
            put_key(out, "Normal");
            put_binary(out, packed);
        }
        if (has_texcoords) {
            put_key(out, "TexCoord0Domain");
            open_map(out, 2);
            put_key(out, "Min");
            open_array(out, 2);
            for (int axis = 0; axis < 2; ++axis) put_real(out, texcoord_low[axis]);
            close_array(out);
            put_key(out, "Max");
            open_array(out, 2);
            for (int axis = 0; axis < 2; ++axis) put_real(out, texcoord_high[axis]);
            close_array(out);
            close_map(out);
            packed.clear();
            for (const auto& texcoord : submesh.texcoords)
                for (int axis = 0; axis < 2; ++axis)
                    put_u16_le(packed, quantize(texcoord[axis], texcoord_low[axis],
                                                texcoord_high[axis]));
            put_key(out, "TexCoord0");
            put_binary(out, packed);
        }

        packed.clear();
        for (const auto index : submesh.indices) put_u16_le(packed, index);
        put_key(out, "TriangleList");
        put_binary(out, packed);
        close_map(out);
    }
    close_array(out);
    return compress(out);
}

std::vector<std::byte> encode_physics(const std::vector<std::array<float, 3>>& hull) {
    Bounds3 bounds{{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                    std::numeric_limits<float>::max()},
                   {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                    std::numeric_limits<float>::lowest()}};
    for (const auto& vertex : hull)
        for (int axis = 0; axis < 3; ++axis) {
            bounds.low[axis] = (std::min)(bounds.low[axis], vertex[axis]);
            bounds.high[axis] = (std::max)(bounds.high[axis], vertex[axis]);
        }
    for (int axis = 0; axis < 3; ++axis)
        if (!(bounds.high[axis] > bounds.low[axis])) bounds.high[axis] = bounds.low[axis] + 0.001f;
    std::vector<std::byte> out;
    open_map(out, 3);
    put_key(out, "Min");
    open_array(out, 3);
    for (int axis = 0; axis < 3; ++axis) put_real(out, bounds.low[axis]);
    close_array(out);
    put_key(out, "Max");
    open_array(out, 3);
    for (int axis = 0; axis < 3; ++axis) put_real(out, bounds.high[axis]);
    close_array(out);
    std::vector<std::byte> packed;
    for (const auto& vertex : hull)
        for (int axis = 0; axis < 3; ++axis)
            put_u16_le(packed, quantize(vertex[axis], bounds.low[axis], bounds.high[axis]));
    put_key(out, "BoundingVerts");
    put_binary(out, packed);
    close_map(out);
    return compress(out);
}

bool valid_level(const Level& level) {
    for (const auto& submesh : level) {
        if (submesh.positions.empty() || submesh.indices.empty() ||
            submesh.indices.size() % 3 != 0)
            return false;
        if (submesh.positions.size() > 65535) return false;
        if (!submesh.normals.empty() && submesh.normals.size() != submesh.positions.size())
            return false;
        if (!submesh.texcoords.empty() && submesh.texcoords.size() != submesh.positions.size())
            return false;
        for (const auto index : submesh.indices)
            if (index >= submesh.positions.size()) return false;
    }
    return !level.empty();
}

// --- reading -------------------------------------------------------------------

std::optional<std::array<float, 3>> read_vec3(Reader& reader) {
    if (!reader.expect('[')) return std::nullopt;
    const auto entries = reader.u32();
    if (!entries || *entries != 3) return std::nullopt;
    std::array<float, 3> value{};
    for (int axis = 0; axis < 3; ++axis) {
        const auto component = reader.real();
        if (!component) return std::nullopt;
        value[axis] = static_cast<float>(*component);
    }
    if (!reader.expect(']')) return std::nullopt;
    return value;
}

std::optional<std::array<float, 2>> read_vec2(Reader& reader) {
    if (!reader.expect('[')) return std::nullopt;
    const auto entries = reader.u32();
    if (!entries || *entries != 2) return std::nullopt;
    std::array<float, 2> value{};
    for (int axis = 0; axis < 2; ++axis) {
        const auto component = reader.real();
        if (!component) return std::nullopt;
        value[axis] = static_cast<float>(*component);
    }
    if (!reader.expect(']')) return std::nullopt;
    return value;
}

std::optional<Level> decode_level(std::span<const std::byte> packed) {
    const auto raw = decompress(packed);
    if (raw.empty()) return std::nullopt;
    Reader reader{raw, 0};
    if (!reader.expect('[')) return std::nullopt;
    const auto submesh_count = reader.u32();
    if (!submesh_count) return std::nullopt;
    Level level;
    for (std::uint32_t index = 0; index < *submesh_count; ++index) {
        if (!reader.expect('{')) return std::nullopt;
        const auto entries = reader.u32();
        if (!entries) return std::nullopt;
        Submesh submesh;
        Bounds3 bounds;
        std::array<float, 2> texcoord_low{}, texcoord_high{1.0f, 1.0f};
        std::span<const std::byte> positions, normals, texcoords, triangles;
        std::span<const std::byte> weights;
        for (std::uint32_t entry = 0; entry < *entries; ++entry) {
            const auto name = reader.key();
            if (!name) return std::nullopt;
            if (*name == "PositionDomain" || *name == "TexCoord0Domain") {
                if (!reader.expect('{')) return std::nullopt;
                const auto domain_entries = reader.u32();
                if (!domain_entries) return std::nullopt;
                for (std::uint32_t domain_entry = 0; domain_entry < *domain_entries;
                     ++domain_entry) {
                    const auto bound = reader.key();
                    if (!bound) return std::nullopt;
                    if (*name == "PositionDomain") {
                        const auto value = read_vec3(reader);
                        if (!value) return std::nullopt;
                        (*bound == "Min" ? bounds.low : bounds.high) = *value;
                    } else {
                        const auto value = read_vec2(reader);
                        if (!value) return std::nullopt;
                        (*bound == "Min" ? texcoord_low : texcoord_high) = *value;
                    }
                }
                if (!reader.expect('}')) return std::nullopt;
            } else if (*name == "Position" || *name == "Normal" || *name == "TexCoord0" ||
                       *name == "TriangleList" || *name == "Weights") {
                const auto payload = reader.binary();
                if (!payload) return std::nullopt;
                if (*name == "Position") positions = *payload;
                else if (*name == "Normal") normals = *payload;
                else if (*name == "TexCoord0") texcoords = *payload;
                else if (*name == "Weights") weights = *payload;
                else triangles = *payload;
            } else if (!reader.skip()) {
                return std::nullopt;
            }
        }
        if (!reader.expect('}')) return std::nullopt;
        if (positions.size() % 6 != 0 || triangles.size() % 6 != 0) return std::nullopt;
        const auto vertex_count = positions.size() / 6;
        for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
            std::array<float, 3> value{};
            for (int axis = 0; axis < 3; ++axis)
                value[axis] = dequantize(read_u16_le(positions, vertex * 6 + axis * 2),
                                         bounds.low[axis], bounds.high[axis]);
            submesh.positions.push_back(value);
        }
        if (!normals.empty()) {
            if (normals.size() != positions.size()) return std::nullopt;
            for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
                std::array<float, 3> value{};
                for (int axis = 0; axis < 3; ++axis)
                    value[axis] = dequantize(read_u16_le(normals, vertex * 6 + axis * 2),
                                             -1.0f, 1.0f);
                submesh.normals.push_back(value);
            }
        }
        if (!texcoords.empty()) {
            if (texcoords.size() != vertex_count * 4) return std::nullopt;
            for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
                std::array<float, 2> value{};
                for (int axis = 0; axis < 2; ++axis)
                    value[axis] = dequantize(read_u16_le(texcoords, vertex * 4 + axis * 2),
                                             texcoord_low[axis], texcoord_high[axis]);
                submesh.texcoords.push_back(value);
            }
        }
        // Per-vertex skin weights, in the format encode writes: a run of
        // (u8 joint, u16 weight) per vertex, terminated by 0xff when the vertex
        // uses fewer than four. Read back because nothing else could verify the
        // weights survived - the round-trip test previously searched the
        // serialized bytes for the string "Weights", which sits inside a
        // zlib-compressed block and was found only when deflate happened to
        // leave it intact.
        if (!weights.empty()) {
            std::size_t at = 0;
            for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
                std::vector<Influence> bound;
                while (bound.size() < 4) {
                    if (at >= weights.size()) return std::nullopt;
                    const auto joint = static_cast<std::uint8_t>(weights[at]);
                    if (joint == 0xff) {
                        ++at;
                        break;
                    }
                    if (at + 2 >= weights.size()) return std::nullopt;
                    const auto scaled = read_u16_le(weights, at + 1);
                    bound.push_back({joint, static_cast<float>(scaled) / 65535.0F});
                    at += 3;
                }
                submesh.influences.push_back(std::move(bound));
            }
        }
        for (std::size_t position = 0; position + 1 < triangles.size(); position += 2)
            submesh.indices.push_back(read_u16_le(triangles, position));
        level.push_back(std::move(submesh));
    }
    if (!reader.expect(']')) return std::nullopt;
    return level;
}

// The inverse of encode_skin. Reads the keys in whatever order they appear,
// since a map has no order and a reader that depended on one would break on any
// writer but this file's.
std::optional<Skin> decode_skin(std::span<const std::byte> packed) {
    const auto raw = decompress(packed);
    if (raw.empty()) return std::nullopt;
    Reader reader{raw, 0};
    if (!reader.expect('{')) return std::nullopt;
    const auto entries = reader.u32();
    if (!entries) return std::nullopt;
    Skin skin;
    const auto read_matrices = [&](std::vector<std::array<float, 16>>& target) {
        if (!reader.expect('[')) return false;
        const auto count = reader.u32();
        if (!count) return false;
        for (std::uint32_t index = 0; index < *count; ++index) {
            if (!reader.expect('[')) return false;
            const auto cells = reader.u32();
            if (!cells || *cells != 16) return false;
            std::array<float, 16> matrix{};
            for (auto& cell : matrix) {
                const auto value = reader.real();
                if (!value) return false;
                cell = static_cast<float>(*value);
            }
            if (!reader.expect(']')) return false;
            target.push_back(matrix);
        }
        return reader.expect(']');
    };
    for (std::uint32_t entry = 0; entry < *entries; ++entry) {
        const auto name = reader.key();
        if (!name) return std::nullopt;
        if (*name == "joint_names") {
            if (!reader.expect('[')) return std::nullopt;
            const auto count = reader.u32();
            if (!count) return std::nullopt;
            for (std::uint32_t index = 0; index < *count; ++index) {
                auto joint = reader.string();
                if (!joint) return std::nullopt;
                skin.joints.push_back(std::move(*joint));
            }
            if (!reader.expect(']')) return std::nullopt;
        } else if (*name == "inverse_bind_matrix") {
            if (!read_matrices(skin.inverse_bind)) return std::nullopt;
        } else if (*name == "alt_inverse_bind_matrix") {
            if (!read_matrices(skin.alternate_inverse_bind)) return std::nullopt;
        } else if (*name == "bind_shape_matrix") {
            if (!reader.expect('[')) return std::nullopt;
            const auto cells = reader.u32();
            if (!cells || *cells != 16) return std::nullopt;
            for (auto& cell : skin.bind_shape) {
                const auto value = reader.real();
                if (!value) return std::nullopt;
                cell = static_cast<float>(*value);
            }
            if (!reader.expect(']')) return std::nullopt;
        } else if (*name == "pelvis_offset") {
            const auto value = reader.real();
            if (!value) return std::nullopt;
            skin.pelvis_offset = static_cast<float>(*value);
        } else if (*name == "lock_scale_if_joint_position") {
            const auto value = reader.boolean();
            if (!value) return std::nullopt;
            skin.lock_scale_if_joint_position = *value;
        } else if (!reader.skip()) {
            return std::nullopt;
        }
    }
    if (skin.joints.empty() || skin.joints.size() != skin.inverse_bind.size())
        return std::nullopt;
    return skin;
}

std::optional<std::vector<std::array<float, 3>>> decode_physics(std::span<const std::byte> packed) {
    const auto raw = decompress(packed);
    if (raw.empty()) return std::nullopt;
    Reader reader{raw, 0};
    if (!reader.expect('{')) return std::nullopt;
    const auto entries = reader.u32();
    if (!entries) return std::nullopt;
    Bounds3 bounds;
    std::span<const std::byte> vertices;
    for (std::uint32_t entry = 0; entry < *entries; ++entry) {
        const auto name = reader.key();
        if (!name) return std::nullopt;
        if (*name == "Min" || *name == "Max") {
            const auto value = read_vec3(reader);
            if (!value) return std::nullopt;
            (*name == "Min" ? bounds.low : bounds.high) = *value;
        } else if (*name == "BoundingVerts") {
            const auto payload = reader.binary();
            if (!payload) return std::nullopt;
            vertices = *payload;
        } else if (!reader.skip()) {
            return std::nullopt;
        }
    }
    if (vertices.size() % 6 != 0) return std::nullopt;
    std::vector<std::array<float, 3>> hull;
    for (std::size_t vertex = 0; vertex < vertices.size() / 6; ++vertex) {
        std::array<float, 3> value{};
        for (int axis = 0; axis < 3; ++axis)
            value[axis] = dequantize(read_u16_le(vertices, vertex * 6 + axis * 2),
                                     bounds.low[axis], bounds.high[axis]);
        hull.push_back(value);
    }
    return hull;
}

} // namespace

// The `skin` block: the joint table every submesh's influences index into, and
// how this mesh's bind pose relates to the skeleton's. Keys and their exact
// spellings come from the viewer's own reader (llmodel.cpp) rather than from a
// specification, since that reader is what has to accept the result.
std::vector<std::byte> encode_skin(const Skin& skin) {
    if (skin.joints.empty() || skin.joints.size() != skin.inverse_bind.size()) return {};
    // alt_inverse_bind_matrix is optional, but a partial one would silently
    // override some joints and not others.
    if (!skin.alternate_inverse_bind.empty() &&
        skin.alternate_inverse_bind.size() != skin.joints.size())
        return {};
    std::vector<std::byte> out;
    std::uint32_t entries = 5; // joint_names, inverse_bind, bind_shape, pelvis, lock
    if (!skin.alternate_inverse_bind.empty()) ++entries;
    open_map(out, entries);

    put_key(out, "joint_names");
    open_array(out, static_cast<std::uint32_t>(skin.joints.size()));
    for (const auto& joint : skin.joints) put_string(out, joint);
    close_array(out);

    const auto put_matrices = [&](std::string_view key,
                                  const std::vector<std::array<float, 16>>& matrices) {
        put_key(out, key);
        open_array(out, static_cast<std::uint32_t>(matrices.size()));
        for (const auto& matrix : matrices) {
            open_array(out, 16);
            for (const auto value : matrix) put_real(out, value);
            close_array(out);
        }
        close_array(out);
    };
    put_matrices("inverse_bind_matrix", skin.inverse_bind);

    put_key(out, "bind_shape_matrix");
    open_array(out, 16);
    for (const auto value : skin.bind_shape) put_real(out, value);
    close_array(out);

    if (!skin.alternate_inverse_bind.empty())
        put_matrices("alt_inverse_bind_matrix", skin.alternate_inverse_bind);

    put_key(out, "pelvis_offset");
    put_real(out, skin.pelvis_offset);
    put_key(out, "lock_scale_if_joint_position");
    put_boolean(out, skin.lock_scale_if_joint_position);
    close_map(out);
    // Deflated like every other block. The header's offsets name compressed
    // extents, so a block written plain is unreadable by anything that trusts
    // the format — including this file's own reader, which is how it was
    // caught.
    return compress(out);
}

std::vector<std::byte> serialize(const Mesh& mesh) {
    if (!valid_level(mesh.high) || !valid_level(mesh.medium) || !valid_level(mesh.low) ||
        !valid_level(mesh.lowest) || mesh.physics_hull.empty())
        return {};
    struct Block {
        std::string_view name;
        std::vector<std::byte> bytes;
    };
    std::vector<Block> blocks;
    blocks.push_back({"high_lod", encode_level(mesh.high)});
    blocks.push_back({"medium_lod", encode_level(mesh.medium)});
    blocks.push_back({"low_lod", encode_level(mesh.low)});
    blocks.push_back({"lowest_lod", encode_level(mesh.lowest)});
    blocks.push_back({"physics_convex", encode_physics(mesh.physics_hull)});
    if (mesh.skin) {
        auto encoded = encode_skin(*mesh.skin);
        if (encoded.empty()) return {};
        blocks.push_back({"skin", std::move(encoded)});
    }
    for (const auto& block : blocks)
        if (block.bytes.empty()) return {};

    // Header offsets are relative to the first byte after the header.
    std::vector<std::byte> header;
    open_map(header, static_cast<std::uint32_t>(blocks.size() + 1));
    put_key(header, "version");
    put_integer(header, 1);
    std::uint32_t offset = 0;
    for (const auto& block : blocks) {
        put_key(header, block.name);
        open_map(header, 2);
        put_key(header, "offset");
        put_integer(header, static_cast<std::int32_t>(offset));
        put_key(header, "size");
        put_integer(header, static_cast<std::int32_t>(block.bytes.size()));
        close_map(header);
        offset += static_cast<std::uint32_t>(block.bytes.size());
    }
    close_map(header);

    std::vector<std::byte> out = std::move(header);
    for (const auto& block : blocks)
        out.insert(out.end(), block.bytes.begin(), block.bytes.end());
    return out;
}

std::optional<Mesh> parse(std::span<const std::byte> content) {
    Reader reader{content, 0};
    if (!reader.expect('{')) return std::nullopt;
    const auto entries = reader.u32();
    if (!entries) return std::nullopt;
    struct Extent {
        std::uint32_t offset{};
        std::uint32_t size{};
        bool present{};
    };
    std::map<std::string, Extent> extents;
    for (std::uint32_t entry = 0; entry < *entries; ++entry) {
        const auto name = reader.key();
        if (!name) return std::nullopt;
        const auto next = reader.peek();
        if (next && *next == '{') {
            ++reader.at;
            const auto block_entries = reader.u32();
            if (!block_entries) return std::nullopt;
            Extent extent;
            for (std::uint32_t block_entry = 0; block_entry < *block_entries; ++block_entry) {
                const auto field = reader.key();
                if (!field) return std::nullopt;
                const auto value = reader.integer();
                if (!value) return std::nullopt;
                if (*field == "offset") extent.offset = static_cast<std::uint32_t>(*value);
                if (*field == "size") extent.size = static_cast<std::uint32_t>(*value);
            }
            if (!reader.expect('}')) return std::nullopt;
            extent.present = extent.size != 0;
            extents[*name] = extent;
        } else if (!reader.skip()) {
            return std::nullopt;
        }
    }
    if (!reader.expect('}')) return std::nullopt;
    const auto blocks_begin = reader.at;
    const auto block = [&](const char* name) -> std::optional<std::span<const std::byte>> {
        const auto found = extents.find(name);
        if (found == extents.end() || !found->second.present) return std::nullopt;
        const std::size_t start = blocks_begin + found->second.offset;
        if (start + found->second.size > content.size()) return std::nullopt;
        return content.subspan(start, found->second.size);
    };
    const auto high = block("high_lod");
    if (!high) return std::nullopt;
    Mesh mesh;
    const auto high_level = decode_level(*high);
    if (!high_level) return std::nullopt;
    mesh.high = *high_level;
    const auto read_level = [&](const char* name, Level& target) -> bool {
        const auto bytes = block(name);
        if (!bytes) {
            target = mesh.high;
            return true;
        }
        const auto level = decode_level(*bytes);
        if (!level) return false;
        target = *level;
        return true;
    };
    if (!read_level("medium_lod", mesh.medium) || !read_level("low_lod", mesh.low) ||
        !read_level("lowest_lod", mesh.lowest))
        return std::nullopt;
    if (const auto skin_bytes = block("skin")) {
        const auto skin = decode_skin(*skin_bytes);
        if (!skin) return std::nullopt;
        mesh.skin = *skin;
    }
    if (const auto physics = block("physics_convex")) {
        const auto hull = decode_physics(*physics);
        if (!hull) return std::nullopt;
        mesh.physics_hull = *hull;
    }
    return mesh;
}

} // namespace homeworldz::slmesh
