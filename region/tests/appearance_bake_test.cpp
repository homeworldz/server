#include "homeworldz/appearance_bake.h"

#include "homeworldz/image.h"
#include "homeworldz/viewer_protocol.h"

#include <array>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

using homeworldz::image::Image;
using homeworldz::viewer::bake_worn_outfit;
using homeworldz::viewer::format_uuid;
using homeworldz::viewer::parse_uuid;
using homeworldz::viewer::unpack_texture_entry_faces;
using homeworldz::viewer::Uuid;

namespace {

std::vector<std::byte> bytes_of(const std::string& s) {
    std::vector<std::byte> out(s.size());
    if (!s.empty()) std::memcpy(out.data(), s.data(), s.size());
    return out;
}

std::vector<std::byte> bytes_of(const std::vector<std::uint8_t>& v) {
    std::vector<std::byte> out(v.size());
    if (!v.empty()) std::memcpy(out.data(), v.data(), v.size());
    return out;
}

Image solid(std::uint32_t size, std::array<std::uint8_t, 4> rgba) {
    Image img;
    img.width = size;
    img.height = size;
    img.channels = 4;
    img.pixels.resize(img.expected_size());
    for (std::size_t i = 0; i < img.pixel_count(); ++i) {
        for (int c = 0; c < 4; ++c) img.pixels[i * 4 + c] = rgba[c];
    }
    return img;
}

bool expect(bool ok, const char* what) {
    if (!ok) std::cerr << "FAIL: " << what << '\n';
    return ok;
}

}  // namespace

int main() {
    bool ok = true;

    const Uuid skin_id = *parse_uuid("aaaaaaaa-0000-4000-8000-000000000001");
    const Uuid tex_head = *parse_uuid("cccccccc-0000-4000-8000-000000000012");
    const Uuid tex_upper = *parse_uuid("cccccccc-0000-4000-8000-000000000010");
    const Uuid tex_lower = *parse_uuid("cccccccc-0000-4000-8000-000000000011");
    const Uuid default_id = *parse_uuid("5748decc-f629-461c-9a36-a35a221fe21f");

    // Skin body part supplies the three body-region skin textures (TE indices
    // 0 = head, 5 = upper, 6 = lower), mirroring the bundled default skin.
    const std::string skin_text = std::string("LLWearable version 22\nSexy Skin\ntype 1\n") +
                                  "parameters 0\n" + "textures 3\n" + "\t0 " +
                                  format_uuid(tex_head) + "\n" + "\t5 " + format_uuid(tex_upper) +
                                  "\n" + "\t6 " + format_uuid(tex_lower) + "\n";

    auto encoded_texture = homeworldz::image::encode_j2c(solid(64, {200, 180, 160, 255}));
    if (!encoded_texture) {
        std::cerr << "test setup: failed to encode a J2C texture\n";
        return 1;
    }
    const std::vector<std::byte> texture_j2c = bytes_of(*encoded_texture);

    auto fetch = [&](const Uuid& id) -> std::optional<std::vector<std::byte>> {
        if (id == skin_id) return bytes_of(skin_text);
        if (id == tex_head || id == tex_upper || id == tex_lower) return texture_j2c;
        return std::nullopt;
    };

    auto bake = bake_worn_outfit({skin_id}, default_id, fetch);
    if (!bake) {
        std::cerr << "bake_worn_outfit returned nullopt\n";
        return 1;
    }

    // Head/upper/lower slots baked (TEX_*_BAKED indices 8/9/10); the rest stay
    // at the default face.
    ok &= expect(bake->faces[8] != default_id, "head slot (index 8) baked");
    ok &= expect(bake->faces[9] != default_id, "upper slot (index 9) baked");
    ok &= expect(bake->faces[10] != default_id, "lower slot (index 10) baked");
    ok &= expect(bake->faces[11] == default_id, "eyes slot (index 11) unbaked");
    ok &= expect(bake->faces[19] == default_id, "skirt slot (index 19) unbaked");
    ok &= expect(bake->assets.size() == 3, "three baked assets produced");

    // TextureEntry round-trips to the same faces.
    auto decoded_faces = unpack_texture_entry_faces(bake->texture_entry);
    ok &= expect(decoded_faces.has_value() && *decoded_faces == bake->faces,
                 "texture_entry decodes back to faces");

    // Each baked asset is a decodable 512x512 JPEG2000 image.
    for (const auto& asset : bake->assets) {
        std::vector<std::uint8_t> raw(asset.j2c.size());
        if (!asset.j2c.empty()) std::memcpy(raw.data(), asset.j2c.data(), asset.j2c.size());
        auto decoded = homeworldz::image::decode_j2c(raw);
        ok &= expect(decoded.has_value() && decoded->width == 512 && decoded->height == 512,
                     "baked asset is a 512x512 J2C");
        // The stored id must be the face at its texture index.
        ok &= expect(bake->faces[asset.texture_index] == asset.id,
                     "asset id matches its face slot");
        // The SIZ marker's component count (Csiz, bytes 40-41 of the
        // codestream): the upper and lower bakes carry the clothing morph mask
        // as a fifth component, which a viewer reads to place clothing morphs.
        // Without it, a sleeve flare reshaped bare forearms in-world.
        const bool body = asset.texture_index == 9 || asset.texture_index == 10;
        const int components = raw.size() > 41 ? (raw[40] << 8) | raw[41] : 0;
        ok &= expect(components == (body ? 5 : 4),
                     body ? "upper and lower bakes carry a fifth, mask component"
                          : "other bakes stay four components");
        ok &= expect(!decoded || decoded->channels == 4, "decoding keeps red, green, blue, alpha");
    }

    if (!ok) return 1;
    std::cerr << "appearance bake orchestration OK\n";
    return 0;
}
