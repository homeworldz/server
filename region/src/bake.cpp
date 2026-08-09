#include "homeworldz/bake.h"

#include <algorithm>
#include <array>
#include <memory>

namespace homeworldz::viewer {
namespace {

// Hair color is an SL "global color" (LLTexGlobalColor "hair_color"): several
// color params, each mapping its 0..1 value across a color ramp, summed (the
// Add operation) into one tint. Ramps are from avatar_lad.xml. The default hair
// is Blonde (114) = .5, which lands mid-ramp at a medium brown (90,53,16).
struct HairColorParam {
    std::uint32_t id;
    std::vector<std::array<int, 3>> ramp;  // ordered stops, value 0..1 across them
};

const std::array<HairColorParam, 4>& hair_color_params() {
    static const std::array<HairColorParam, 4> params = {{
        {114,  // Blonde
         {{0, 0, 0}, {22, 6, 6}, {29, 9, 6}, {45, 21, 11}, {78, 39, 11}, {90, 53, 16},
          {136, 92, 21}, {150, 106, 33}, {198, 156, 74}, {233, 192, 103}, {238, 205, 136}}},
        {113, {{0, 0, 0}, {118, 47, 19}}},                          // Red
        {115, {{0, 0, 0}, {255, 255, 255}}},                        // White
        {112,                                                        // Rainbow
         {{0, 0, 0}, {255, 0, 255}, {255, 0, 0}, {255, 255, 0}, {0, 255, 0}, {0, 255, 255},
          {0, 0, 255}, {255, 0, 255}}},
    }};
    return params;
}

// Interpolate a color ramp at value in [0,1], matching LMV's GetColorFromParams.
std::array<double, 3> ramp_color(const std::vector<std::array<int, 3>>& ramp, double value) {
    const int n = static_cast<int>(ramp.size());
    if (n == 0) return {0, 0, 0};
    if (n == 1) return {double(ramp[0][0]), double(ramp[0][1]), double(ramp[0][2])};
    const double step = 1.0 / (n - 1);
    int a = 0;
    for (int i = 0; i < n; ++i) {
        if (i * step <= value) a = i; else break;
    }
    const int b = (a == n - 1) ? a : a + 1;
    const double distance = value - a * step;
    const auto& c1 = ramp[a];
    const auto& c2 = ramp[b];
    if (distance < 1e-5 || a == b) return {double(c1[0]), double(c1[1]), double(c1[2])};
    const double t = distance / step;
    return {c1[0] + (c2[0] - c1[0]) * t, c1[1] + (c2[1] - c1[1]) * t, c1[2] + (c2[2] - c1[2]) * t};
}

std::array<std::uint8_t, 3> hair_tint(const Wearable& w) {
    std::array<double, 3> res = {0, 0, 0};
    for (const auto& p : hair_color_params()) {
        const auto it = w.parameters.find(p.id);
        if (it == w.parameters.end()) continue;
        double v = it->second;
        v = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
        const auto c = ramp_color(p.ramp, v);
        res[0] += c[0];  // Add operation (global hair color)
        res[1] += c[1];
        res[2] += c[2];
    }
    const auto clamp8 = [](double x) -> std::uint8_t {
        x = x < 0.0 ? 0.0 : (x > 255.0 ? 255.0 : x);
        return static_cast<std::uint8_t>(x + 0.5);
    };
    return {clamp8(res[0]), clamp8(res[1]), clamp8(res[2])};
}

// SL clothing color is carried as three visual params (red, green, blue), each
// 0..1, that tint the layer's texture. The default outfit relies on this: its
// shirt and pants both use the opaque white "Blank" texture and are colored
// entirely by these params (shirt grey 803/804/805 = .5/.5/.6, pants reddish
// 806/807/808 = .8/.2/.2). Hair uses the global-color ramp above; other body
// parts (skin/shape/eyes) carry no such params and render untinted.
// Clothing alpha masks. The default shirt/pants use the opaque Blank texture,
// so their coverage is defined entirely by parametrized alpha masks (from
// avatar_lad.xml). Each entry names a TGA mask resource, the wearable param
// whose value thresholds it, and whether it multiplies (carves) rather than
// unions. Mirrors LibreMetaverse's bake: normal masks union their coverage,
// multiply masks remove it; the threshold per texel is
// mask <= (1-value)*255 -> transparent, else opaque.
struct ClothingMask {
    const char* name;
    std::uint32_t param;
    bool multiply;
};

const std::vector<ClothingMask>* clothing_masks(WearableType type) {
    static const std::vector<ClothingMask> shirt = {
        {"shirt_sleeve_alpha", 800, false},  // Sleeve Length: base upper coverage
        {"shirt_bottom_alpha", 801, true},   // Shirt Bottom: carve the hem
        {"shirt_collar_alpha", 802, true},   // Collar Front: carve the neck
    };
    static const std::vector<ClothingMask> pants = {
        {"pants_length_alpha", 815, false},  // Pants Length: base lower coverage
        {"pants_waist_alpha", 814, false},   // Waist Height
    };
    switch (type) {
        case WearableType::Shirt: return &shirt;
        case WearableType::Pants: return &pants;
        default: return nullptr;
    }
}

// Threshold a grayscale mask by a param value into 0/255 (LMV ApplyAlpha).
std::uint8_t threshold_texel(std::uint8_t texel, double value) {
    return (static_cast<double>(texel) <= (1.0 - value) * 255.0) ? std::uint8_t{0}
                                                                  : std::uint8_t{255};
}

// Build the combined alpha mask (1-channel L Image) for a clothing wearable, or
// nullopt if it has no masks defined or none could be loaded.
std::optional<image::Image> clothing_alpha(const Wearable& w, const MaskFetch& mask_fetch) {
    if (!mask_fetch) return std::nullopt;
    const std::vector<ClothingMask>* entries = clothing_masks(w.type);
    if (entries == nullptr) return std::nullopt;

    const auto load = [&](const char* name) -> std::optional<image::Image> {
        auto bytes = mask_fetch(name);
        if (!bytes || bytes->empty()) return std::nullopt;
        auto img = image::decode_tga(*bytes);
        if (!img || img->empty()) return std::nullopt;
        return img;
    };

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> combined;
    int normal_added = 0;

    // Pass 1: normal-blend masks union their coverage.
    for (const auto& e : *entries) {
        if (e.multiply) continue;
        const auto it = w.parameters.find(e.param);
        if (it == w.parameters.end()) continue;
        auto img = load(e.name);
        if (!img) continue;
        if (combined.empty()) {
            width = img->width;
            height = img->height;
            combined.assign(static_cast<std::size_t>(width) * height, 0);
        } else if (img->width != width || img->height != height) {
            *img = image::resize_nearest(*img, width, height);
        }
        const double v = std::clamp(it->second, 0.0, 1.0);
        const std::uint8_t ch = img->channels;
        const std::size_t n = static_cast<std::size_t>(width) * height;
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint8_t a = threshold_texel(img->pixels[i * ch], v);
            if (a > combined[i]) combined[i] = a;
        }
        ++normal_added;
    }
    if (combined.empty()) return std::nullopt;
    if (normal_added == 0) std::fill(combined.begin(), combined.end(), 255);

    // Pass 2: multiply-blend masks carve coverage away.
    for (const auto& e : *entries) {
        if (!e.multiply) continue;
        const auto it = w.parameters.find(e.param);
        if (it == w.parameters.end()) continue;
        auto img = load(e.name);
        if (!img) continue;
        if (img->width != width || img->height != height)
            *img = image::resize_nearest(*img, width, height);
        const double v = std::clamp(it->second, 0.0, 1.0);
        const std::uint8_t ch = img->channels;
        const std::size_t n = static_cast<std::size_t>(width) * height;
        for (std::size_t i = 0; i < n; ++i)
            if (threshold_texel(img->pixels[i * ch], v) == 0) combined[i] = 0;
    }

    image::Image mask;
    mask.width = width;
    mask.height = height;
    mask.channels = 1;
    mask.pixels = std::move(combined);
    return mask;
}

std::array<std::uint8_t, 3> wearable_tint(const Wearable& w) {
    if (w.type == WearableType::Hair) return hair_tint(w);
    struct ColorParams {
        WearableType type;
        std::uint32_t r, g, b;
    };
    static const std::array<ColorParams, 9> table = {{
        {WearableType::Shirt, 803, 804, 805},
        {WearableType::Pants, 806, 807, 808},
        {WearableType::Shoes, 812, 813, 817},
        {WearableType::Socks, 818, 819, 820},
        {WearableType::Jacket, 834, 835, 836},
        {WearableType::Gloves, 827, 829, 830},
        {WearableType::Undershirt, 821, 822, 823},
        {WearableType::Underpants, 824, 825, 826},
        {WearableType::Skirt, 921, 922, 923},
    }};
    const auto channel = [&](std::uint32_t id) -> std::uint8_t {
        const auto it = w.parameters.find(id);
        double v = (it == w.parameters.end()) ? 1.0 : it->second;
        v = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
        return static_cast<std::uint8_t>(v * 255.0 + 0.5);
    };
    for (const auto& c : table) {
        if (c.type == w.type) return {channel(c.r), channel(c.g), channel(c.b)};
    }
    return {255, 255, 255};
}

}  // namespace

const std::vector<BakeSlotLayout>& bake_slot_layouts() {
    namespace tx = tex_index;
    // Composite order is bottom -> top: skin, then tattoo, then under layers,
    // the main garment, and finally the jacket on top. Only layers a user
    // actually wears contribute; the default six-wearable outfit exercises skin
    // plus one garment per body region.
    static const std::vector<BakeSlotLayout> layouts = {
        {BakeSlot::Head, 512, {tx::kHeadBodypaint, tx::kHeadTattoo}},
        {BakeSlot::Upper,
         512,
         {tx::kUpperBodypaint, tx::kUpperTattoo, tx::kUpperUndershirt, tx::kUpperGloves,
          tx::kUpperShirt, tx::kUpperJacket}},
        {BakeSlot::Lower,
         512,
         {tx::kLowerBodypaint, tx::kLowerTattoo, tx::kLowerUnderpants, tx::kLowerSocks,
          tx::kLowerShoes, tx::kLowerPants, tx::kLowerJacket}},
        {BakeSlot::Eyes, 128, {tx::kEyesIris}},
        {BakeSlot::Skirt, 512, {tx::kSkirt}},
        {BakeSlot::Hair, 512, {tx::kHair}},
    };
    return layouts;
}

std::uint32_t baked_texture_index(BakeSlot slot) {
    switch (slot) {
        case BakeSlot::Head:
            return tex_index::kHeadBaked;
        case BakeSlot::Upper:
            return tex_index::kUpperBaked;
        case BakeSlot::Lower:
            return tex_index::kLowerBaked;
        case BakeSlot::Eyes:
            return tex_index::kEyesBaked;
        case BakeSlot::Skirt:
            return tex_index::kSkirtBaked;
        case BakeSlot::Hair:
            return tex_index::kHairBaked;
    }
    return tex_index::kHeadBaked;
}

std::optional<std::uint32_t> alpha_texture_index(BakeSlot slot) {
    switch (slot) {
        case BakeSlot::Head:
            return tex_index::kHeadAlpha;
        case BakeSlot::Upper:
            return tex_index::kUpperAlpha;
        case BakeSlot::Lower:
            return tex_index::kLowerAlpha;
        case BakeSlot::Eyes:
            return tex_index::kEyesAlpha;
        case BakeSlot::Hair:
            return tex_index::kHairAlpha;
        case BakeSlot::Skirt:
            // The Alpha wearable has no skirt channel; indra has no
            // TEX_SKIRT_ALPHA. A skirt is removed by taking it off.
            return std::nullopt;
    }
    return std::nullopt;
}

std::map<BakeSlot, image::Image> bake_outfit(const std::vector<Wearable>& worn,
                                             const TextureFetch& fetch,
                                             const MaskFetch& mask_fetch,
                                             std::vector<Uuid>* unfetchable_masks) {
    // Merge worn textures by texture-entry index (later wearable wins), carrying
    // each source wearable's color tint and (for clothing) its combined alpha
    // mask so the layer is colored and shaped on composite.
    struct WornTexture {
        Uuid id;
        std::array<std::uint8_t, 3> tint;
        std::shared_ptr<const image::Image> mask;
    };
    std::map<std::uint32_t, WornTexture> worn_textures;
    for (const Wearable& w : worn) {
        const auto tint = wearable_tint(w);
        std::shared_ptr<const image::Image> mask;
        if (auto m = clothing_alpha(w, mask_fetch))
            mask = std::make_shared<const image::Image>(std::move(*m));
        for (const auto& [index, id] : w.textures) worn_textures[index] = {id, tint, mask};
    }

    std::map<BakeSlot, image::Image> baked;
    for (const BakeSlotLayout& layout : bake_slot_layouts()) {
        std::vector<image::Layer> layers;
        for (std::uint32_t source : layout.source_texture_indices) {
            const auto it = worn_textures.find(source);
            if (it == worn_textures.end()) continue;
            std::optional<image::Image> texture = fetch(it->second.id);
            if (!texture || texture->empty()) continue;
            image::Layer layer{std::move(*texture), it->second.tint};
            if (it->second.mask) {
                // Apply the clothing alpha mask: upsize the (often tiny "Blank")
                // texture to the mask resolution and set its alpha to the mask,
                // so the garment only covers where the mask is opaque.
                const image::Image& m = *it->second.mask;
                image::Image rgba = image::resize_nearest(image::to_rgba(layer.image), m.width, m.height);
                const std::size_t n = static_cast<std::size_t>(m.width) * m.height;
                if (!rgba.empty() && rgba.pixels.size() == n * 4 && m.pixels.size() == n) {
                    for (std::size_t i = 0; i < n; ++i) rgba.pixels[i * 4 + 3] = m.pixels[i];
                    layer.image = std::move(rgba);
                }
            }
            layers.push_back(std::move(layer));
        }
        if (layers.empty()) continue;
        image::Image slot_image =
            image::composite_rgba(layout.resolution, layout.resolution, layers);
        if (slot_image.empty()) continue;
        // The Alpha wearable, applied exactly as the viewer applies it: its
        // texture's alpha channel multiplied into the baked alpha, with the
        // colour channels untouched (lltexlayer.cpp renders mask layers with
        // BT_MULT_ALPHA under a colour mask of alpha only). Where the mask is
        // transparent the body region stops rendering, which is how a mesh body
        // stops having the default one pushing through it.
        //
        // The alpha channel and not the luminance, even though a black-and-white
        // mask is what a creator draws: matching the viewer matters more than
        // being accommodating, or a bake done here and a bake done there differ
        // and the difference shows up as a body that is fine until someone else
        // looks at it.
        if (const auto alpha_index = alpha_texture_index(layout.slot)) {
            if (const auto it = worn_textures.find(*alpha_index); it != worn_textures.end()) {
                auto mask = fetch(it->second.id);
                if (!mask || mask->empty()) {
                    // Asked to hide a region and unable to: recorded, never
                    // silently skipped.
                    if (unfetchable_masks) unfetchable_masks->push_back(it->second.id);
                }
                if (mask && !mask->empty()) {
                    const auto sized = image::resize_nearest(
                        image::to_rgba(*mask), slot_image.width, slot_image.height);
                    const std::size_t count =
                        static_cast<std::size_t>(slot_image.width) * slot_image.height;
                    if (sized.pixels.size() == count * 4 && slot_image.pixels.size() == count * 4)
                        for (std::size_t pixel = 0; pixel < count; ++pixel) {
                            const std::uint32_t baked_alpha = slot_image.pixels[pixel * 4 + 3];
                            const std::uint32_t mask_alpha = sized.pixels[pixel * 4 + 3];
                            slot_image.pixels[pixel * 4 + 3] =
                                static_cast<std::uint8_t>((baked_alpha * mask_alpha + 127) / 255);
                        }
                }
            }
        }
        baked.emplace(layout.slot, std::move(slot_image));
    }
    return baked;
}

}  // namespace homeworldz::viewer
