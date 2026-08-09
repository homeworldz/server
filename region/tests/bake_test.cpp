#include "homeworldz/bake.h"

#include <array>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

using homeworldz::image::Image;
using homeworldz::viewer::bake_outfit;
using homeworldz::viewer::baked_texture_index;
using homeworldz::viewer::BakeSlot;
using homeworldz::viewer::format_uuid;
using homeworldz::viewer::parse_uuid;
using homeworldz::viewer::Wearable;
using homeworldz::viewer::WearableType;
namespace tx = homeworldz::viewer::tex_index;

namespace {

const char* const kSkinHead = "aaaaaaaa-0000-0000-0000-000000000001";
const char* const kSkinUpper = "aaaaaaaa-0000-0000-0000-000000000002";
const char* const kSkinLower = "aaaaaaaa-0000-0000-0000-000000000003";
const char* const kPantsTex = "bbbbbbbb-0000-0000-0000-000000000001";

Image solid(std::uint32_t size, std::array<std::uint8_t, 4> rgba) {
    Image img;
    img.width = size;
    img.height = size;
    img.channels = 4;
    img.pixels.resize(img.expected_size());
    for (std::size_t i = 0; i < img.pixel_count(); ++i) {
        img.pixels[i * 4 + 0] = rgba[0];
        img.pixels[i * 4 + 1] = rgba[1];
        img.pixels[i * 4 + 2] = rgba[2];
        img.pixels[i * 4 + 3] = rgba[3];
    }
    return img;
}

Wearable make_wearable(WearableType type,
                       std::vector<std::pair<std::uint32_t, const char*>> textures) {
    Wearable w;
    w.type = type;
    for (auto& [index, uuid] : textures) w.textures[index] = *parse_uuid(uuid);
    return w;
}

bool expect(bool ok, const char* what) {
    if (!ok) std::cerr << "FAIL: " << what << '\n';
    return ok;
}

}  // namespace

int main() {
    bool ok = true;

    // A skin body part supplies the three body-region skin textures; opaque
    // blue pants clothe the lower body on top of red lower skin.
    std::vector<Wearable> worn = {
        make_wearable(WearableType::Skin,
                      {{tx::kHeadBodypaint, kSkinHead},
                       {tx::kUpperBodypaint, kSkinUpper},
                       {tx::kLowerBodypaint, kSkinLower}}),
        make_wearable(WearableType::Pants, {{tx::kLowerPants, kPantsTex}}),
    };

    auto fetch = [&](const homeworldz::viewer::Uuid& id) -> std::optional<Image> {
        const std::string s = format_uuid(id);
        if (s == kSkinHead) return solid(64, {200, 180, 160, 255});
        if (s == kSkinUpper) return solid(64, {200, 180, 160, 255});
        if (s == kSkinLower) return solid(64, {255, 0, 0, 255});
        if (s == kPantsTex) return solid(64, {0, 0, 255, 255});
        return std::nullopt;
    };

    auto baked = bake_outfit(worn, fetch);

    // Head, upper, and lower slots should bake; eyes/skirt/hair have no worn
    // texture and must be absent.
    ok &= expect(baked.count(BakeSlot::Head) == 1, "head baked");
    ok &= expect(baked.count(BakeSlot::Upper) == 1, "upper baked");
    ok &= expect(baked.count(BakeSlot::Lower) == 1, "lower baked");
    ok &= expect(baked.count(BakeSlot::Eyes) == 0, "no eyes bake without iris texture");
    ok &= expect(baked.count(BakeSlot::Skirt) == 0, "no skirt bake");
    ok &= expect(baked.count(BakeSlot::Hair) == 0, "no hair bake");

    if (baked.count(BakeSlot::Lower)) {
        const Image& lower = baked.at(BakeSlot::Lower);
        ok &= expect(lower.width == 512 && lower.height == 512, "lower bake is 512x512");
        // Opaque pants sit on top of skin, so the lower bake is blue.
        ok &= expect(lower.pixels[2] > 200 && lower.pixels[0] < 50,
                     "lower bake shows opaque pants over skin");
    }
    if (baked.count(BakeSlot::Head)) {
        const Image& head = baked.at(BakeSlot::Head);
        ok &= expect(head.pixels[0] > 150 && head.pixels[2] < 200, "head bake is skin");
    }

    ok &= expect(baked_texture_index(BakeSlot::Lower) == tx::kLowerBaked,
                 "lower -> TEX_LOWER_BAKED");
    ok &= expect(baked_texture_index(BakeSlot::Eyes) == tx::kEyesBaked,
                 "eyes -> TEX_EYES_BAKED");

    // An Alpha wearable hides the body region it masks. This is what makes a
    // mesh body wearable: the default body renders underneath one and pushes
    // through wherever the two disagree, worst at the head.
    {
        const char* const kAlphaHead = "cccccccc-0000-0000-0000-000000000001";
        auto alpha_worn = worn;
        alpha_worn.push_back(make_wearable(WearableType::Alpha, {{tx::kHeadAlpha, kAlphaHead}}));
        auto alpha_fetch = [&](const homeworldz::viewer::Uuid& id) -> std::optional<Image> {
            const std::string s = format_uuid(id);
            if (s == kAlphaHead) return solid(64, {0, 0, 0, 0});
            if (s == kSkinHead) return solid(64, {200, 180, 160, 255});
            if (s == kSkinUpper) return solid(64, {200, 180, 160, 255});
            if (s == kSkinLower) return solid(64, {255, 0, 0, 255});
            if (s == kPantsTex) return solid(64, {0, 0, 255, 255});
            return std::nullopt;
        };
        const auto masked = bake_outfit(alpha_worn, alpha_fetch);
        ok &= expect(masked.count(BakeSlot::Head) == 1, "head still bakes with an alpha worn");
        if (masked.count(BakeSlot::Head)) {
            const Image& head = masked.at(BakeSlot::Head);
            // Transparent where the mask is transparent...
            ok &= expect(head.pixels[3] == 0, "a transparent head alpha hides the head bake");
            // ...and the colour beneath is untouched. The viewer masks alpha
            // only (colour mask false,true); tinting the pixels black instead
            // would look identical on a body that is fully hidden and wrong on
            // every partial mask, which is what real alpha layers are.
            ok &= expect(head.pixels[0] > 150, "masking leaves the colour channels alone");
        }
        // A region with no alpha texture is unaffected: the lower body still
        // shows its pants.
        if (masked.count(BakeSlot::Lower)) {
            const Image& lower = masked.at(BakeSlot::Lower);
            ok &= expect(lower.pixels[3] == 255, "an unmasked region stays opaque");
        }
        ok &= expect(!homeworldz::viewer::alpha_texture_index(BakeSlot::Skirt).has_value(),
                     "the skirt has no alpha channel");
        ok &= expect(homeworldz::viewer::alpha_texture_index(BakeSlot::Head) == tx::kHeadAlpha,
                     "head -> TEX_HEAD_ALPHA");
    }

    // The alpha above is an in-memory image. What a region actually masks with
    // is IMG_INVISIBLE, and the region synthesizes it as a J2C at startup
    // (main.cpp) rather than bundling one, so the mask only reaches the bake if
    // that image survives the codec with its alpha channel intact. It is worth
    // asserting separately: J2C alpha has been lost in this pipeline before,
    // and the failure is silent — the bake fetches a mask, finds it opaque, and
    // hides nothing while reporting success.
    {
        Image transparent = solid(32, {0, 0, 0, 0});
        const auto encoded = homeworldz::image::encode_j2c(transparent);
        ok &= expect(encoded.has_value() && !encoded->empty(),
                     "the invisible texture encodes to J2C");
        if (encoded && !encoded->empty()) {
            const auto decoded = homeworldz::image::decode_j2c(*encoded);
            ok &= expect(decoded.has_value(), "the invisible texture decodes again");
            if (decoded) {
                ok &= expect(decoded->channels == 4, "it keeps an alpha channel");
                bool all_transparent = decoded->channels == 4;
                for (std::size_t i = 0; all_transparent && i < decoded->pixel_count(); ++i)
                    all_transparent = decoded->pixels[i * decoded->channels + 3] == 0;
                ok &= expect(all_transparent, "every texel survives fully transparent");

                // And drive the bake with it, at 32x32 against 64x64 skin, so
                // the mask is resized on the way in exactly as a real one is.
                const char* const kInvisible = "3a367d1c-bef1-6d43-7595-e88c1e3aadb3";
                auto alpha_worn = worn;
                alpha_worn.push_back(
                    make_wearable(WearableType::Alpha, {{tx::kHeadAlpha, kInvisible}}));
                auto invisible_fetch = [&](const homeworldz::viewer::Uuid& id)
                    -> std::optional<Image> {
                    const std::string s = format_uuid(id);
                    if (s == kInvisible) return *decoded;
                    if (s == kSkinHead) return solid(64, {200, 180, 160, 255});
                    if (s == kSkinUpper) return solid(64, {200, 180, 160, 255});
                    if (s == kSkinLower) return solid(64, {255, 0, 0, 255});
                    if (s == kPantsTex) return solid(64, {0, 0, 255, 255});
                    return std::nullopt;
                };
                const auto masked = bake_outfit(alpha_worn, invisible_fetch);
                ok &= expect(masked.count(BakeSlot::Head) == 1,
                             "the head bakes with IMG_INVISIBLE worn");
                if (masked.count(BakeSlot::Head)) {
                    const Image& head = masked.at(BakeSlot::Head);
                    bool hidden = true;
                    for (std::size_t i = 0; hidden && i < head.pixel_count(); ++i)
                        hidden = head.pixels[i * 4 + 3] == 0;
                    ok &= expect(hidden, "a synthesized IMG_INVISIBLE hides the whole head bake");
                }
            }
        }
    }

    if (!ok) return 1;
    std::cerr << "bake outfit OK\n";
    return 0;
}
