// Writing a GLB container: the little-endian appends and the two-chunk wrapper.
//
// Private to the region's sources — this is mechanism, not policy, and nothing
// outside needs it. It exists because there are now two writers of glTF here
// (the `gltf` rendition derived from a stored type-49 asset, and the FBX import
// of ADR 0035) and a GLB header written two ways is a header written wrong once.
#ifndef HOMEWORLDZ_GLB_WRITE_H
#define HOMEWORLDZ_GLB_WRITE_H

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace homeworldz::mesh::glb {

// Shortest round-trip decimal, so the JSON carries the float that was computed
// rather than a printf approximation of it.
inline std::string number(float value) {
    std::array<char, 32> buffer{};
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (error != std::errc{}) return "0";
    return std::string(buffer.data(), end);
}

inline void append_u32(std::vector<std::byte>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
}

inline void append_u16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>(value & 0xffu));
    out.push_back(static_cast<std::byte>((value >> 8) & 0xffu));
}

inline void append_float(std::vector<std::byte>& out, float value) {
    std::array<std::byte, sizeof value> raw{};
    std::memcpy(raw.data(), &value, sizeof value);
    out.insert(out.end(), raw.begin(), raw.end());
}

// glTF requires an accessor's byteOffset to be a multiple of its component
// size; padding every view to four satisfies both float and u16 attributes.
inline void pad_to_four(std::vector<std::byte>& out) {
    while (out.size() % 4 != 0) out.push_back(std::byte{});
}

// The container: 12-byte header, a JSON chunk, and the binary chunk when there
// is one. `document` is padded with spaces and `binary` with zeroes, which is
// what the specification asks for in each chunk.
inline std::vector<std::byte> wrap(std::string document, std::vector<std::byte> binary) {
    while (document.size() % 4 != 0) document.push_back(' ');
    pad_to_four(binary);

    std::vector<std::byte> out;
    const auto total = 12 + 8 + document.size() + (binary.empty() ? 0 : 8 + binary.size());
    out.reserve(total);
    for (const char character : {'g', 'l', 'T', 'F'})
        out.push_back(static_cast<std::byte>(character));
    append_u32(out, 2);
    append_u32(out, static_cast<std::uint32_t>(total));
    append_u32(out, static_cast<std::uint32_t>(document.size()));
    for (const char character : {'J', 'S', 'O', 'N'})
        out.push_back(static_cast<std::byte>(character));
    for (const char character : document) out.push_back(static_cast<std::byte>(character));
    if (!binary.empty()) {
        append_u32(out, static_cast<std::uint32_t>(binary.size()));
        for (const char character : {'B', 'I', 'N', '\0'})
            out.push_back(static_cast<std::byte>(character));
        out.insert(out.end(), binary.begin(), binary.end());
    }
    return out;
}

} // namespace homeworldz::mesh::glb

#endif
