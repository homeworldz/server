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
// so their coverage is defined entirely by parametrized alpha masks, and this
// reproduces how a viewer renders them (avatar_lad.xml, the shirt layer of
// upper_body and the pants layer of lower_body).
//
// A wearable carries the slider a creator moved, which is a driver: Sleeve
// Length (800) drives Sleeve Length Cloth (600), whose range is 0..0.85, and
// the mask is rendered at the driven weight. Thresholding on the slider made a
// 0.73 sleeve render at 0.73 rather than 0.62 - three-quarter length to every
// watcher, half length to its wearer.
struct ClothingMask {
    const char* name;      // TGA in the default-avatar asset directory
    std::uint32_t driver;  // the wearable's slider
    double driver_default; // its value_default, when the wearable omits it
    double max1;           // the driven entry's max1: driver weight at full drive
    double driven_max;     // the driven param's value_max (value_min is 0)
    double domain;         // the param_alpha domain: width of the soft edge
    bool multiply;         // multiply_blend: carve coverage rather than add it
};

const std::vector<ClothingMask>* clothing_masks(WearableType type) {
    // In layer order, which matters: an added mask unions coverage and a
    // multiplied one carves what has accumulated so far.
    static const std::vector<ClothingMask> shirt = {
        {"shirt_sleeve_alpha", 800, 0.89, 1.0, 0.85, 0.01, false},      // 600
        {"shirt_bottom_alpha", 801, 1.0, 1.0, 1.0, 0.05, true},         // 601
        {"shirt_collar_alpha", 802, 0.78, 1.0, 1.0, 0.05, true},        // 602
        {"shirt_collar_back_alpha", 781, 0.78, 1.0, 1.0, 0.05, true},   // 778
    };
    static const std::vector<ClothingMask> pants = {
        {"pants_waist_alpha", 814, 1.0, 1.0, 1.0, 0.05, false},   // 614
        {"pants_length_alpha", 815, 0.8, 0.9, 1.0, 0.01, false},  // 615
    };
    switch (type) {
        case WearableType::Shirt: return &shirt;
        case WearableType::Pants: return &pants;
        default: return nullptr;
    }
}

// LLDriverParam::getDrivenWeight for a 0..1 driver and a driven param whose
// minimum is 0, with min1 = 0 and max2 = min2 = 1 as every entry above has.
double driven_weight(const ClothingMask& e, double driver) {
    if (driver <= 0.0) return 0.0;
    if (driver >= e.max1) return e.driven_max;
    return driver / e.max1 * e.driven_max;
}

// LLImageTGA::decodeAndProcess: a ramp one domain wide, placed by the weight.
std::array<std::uint8_t, 256> mask_ramp(double domain, double weight) {
    const double scale = 1.0 / domain;
    const double bias = -scale * (1.0 - domain) * std::clamp(1.0 - weight, 0.0, 1.0);
    std::array<std::uint8_t, 256> lut{};
    for (int i = 0; i < 256; ++i) {
        const double v = std::clamp(255.0 * (i / 255.0 * scale + bias), 0.0, 255.0);
        lut[i] = static_cast<std::uint8_t>(v);
    }
    return lut;
}

// Build the combined alpha mask (1-channel L Image) for a clothing wearable, or
// nullopt if it has no masks defined or none could be loaded. LLTexLayer::
// renderMorphMasks: coverage starts empty, added masks sum (saturating) and
// multiplied ones scale what is there.
std::optional<image::Image> clothing_alpha(const Wearable& w, const MaskFetch& mask_fetch) {
    if (!mask_fetch) return std::nullopt;
    const std::vector<ClothingMask>* entries = clothing_masks(w.type);
    if (entries == nullptr) return std::nullopt;

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> combined;
    for (const ClothingMask& e : *entries) {
        auto bytes = mask_fetch(e.name);
        if (!bytes || bytes->empty()) continue;
        auto img = image::decode_tga(*bytes);
        if (!img || img->empty()) continue;
        if (combined.empty()) {
            width = img->width;
            height = img->height;
            combined.assign(static_cast<std::size_t>(width) * height, 0);
        } else if (img->width != width || img->height != height) {
            *img = image::resize_nearest(*img, width, height);
        }
        const auto it = w.parameters.find(e.driver);
        const double driver = it == w.parameters.end() ? e.driver_default : it->second;
        const auto lut = mask_ramp(e.domain, driven_weight(e, std::clamp(driver, 0.0, 1.0)));
        const std::uint8_t ch = img->channels;
        for (std::size_t i = 0; i < combined.size(); ++i) {
            const std::uint32_t texel = lut[img->pixels[i * ch]];
            combined[i] = e.multiply
                              ? static_cast<std::uint8_t>(combined[i] * texel / 255)
                              : static_cast<std::uint8_t>(std::min<std::uint32_t>(255, combined[i] + texel));
        }
    }
    if (combined.empty()) return std::nullopt;

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
        // The eyes layer set starts from a bundled image, not a worn texture:
        // avatar_lad.xml paints eyewhite.tga beneath the iris. An iris texture
        // is transparent outside the iris, so without this base the bake is
        // mostly holes and the eye sockets render see-through. A base that
        // cannot be loaded still shows, as the eyes slot in hiddenBySlot.
        if (layout.slot == BakeSlot::Eyes && mask_fetch) {
            if (const auto bytes = mask_fetch("eyewhite"); bytes && !bytes->empty()) {
                if (auto whites = image::decode_tga(*bytes); whites && !whites->empty())
                    layers.insert(layers.begin(), image::Layer{std::move(*whites), {255, 255, 255}});
            }
        }
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

std::optional<image::Image> clothing_morph_mask(const std::vector<Wearable>& worn,
                                                BakeSlot slot, const MaskFetch& mask_fetch) {
    if (slot != BakeSlot::Upper && slot != BakeSlot::Lower) return std::nullopt;
    const BakeSlotLayout* layout = nullptr;
    for (const BakeSlotLayout& candidate : bake_slot_layouts())
        if (candidate.slot == slot) layout = &candidate;
    if (layout == nullptr) return std::nullopt;

    // gatherMorphMaskAlpha: start fully covered and multiply in the coverage
    // of every masked layer in the set, so a sleeve flare reaches the forearm
    // only where the shirt is.
    image::Image mask;
    mask.width = layout->resolution;
    mask.height = layout->resolution;
    mask.channels = 1;
    mask.pixels.assign(mask.pixel_count(), 255);
    for (const Wearable& w : worn) {
        bool in_slot = false;
        for (const auto& [index, id] : w.textures)
            for (std::uint32_t source : layout->source_texture_indices)
                if (index == source) in_slot = true;
        if (!in_slot) continue;
        auto coverage = clothing_alpha(w, mask_fetch);
        if (!coverage) continue;
        const image::Image sized =
            image::resize_nearest(*coverage, layout->resolution, layout->resolution);
        if (sized.pixels.size() != mask.pixels.size()) continue;
        for (std::size_t i = 0; i < mask.pixels.size(); ++i)
            mask.pixels[i] = static_cast<std::uint8_t>(
                (static_cast<std::uint32_t>(mask.pixels[i]) * (sized.pixels[i] + 1u)) >> 8);
    }
    return mask;
}

}  // namespace homeworldz::viewer
