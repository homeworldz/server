#ifndef HOMEWORLDZ_IMAGE_H
#define HOMEWORLDZ_IMAGE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// Raster image + JPEG2000 codec wrapper for server-side appearance baking
// (ADR 0029). The codec is isolated here so the underlying library (OpenJPEG
// today) is a swappable implementation detail; the bake compositor works only
// on the RGBA Image type below.
namespace homeworldz::image {

// A decoded raster: 1..4 8-bit channels (1=L, 2=LA, 3=RGB, 4=RGBA), row-major,
// tightly packed (no row padding). pixels.size() == width*height*channels.
struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t channels = 0;
    std::vector<std::uint8_t> pixels;

    bool empty() const { return width == 0 || height == 0 || channels == 0; }
    std::size_t pixel_count() const {
        return static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    }
    std::size_t expected_size() const { return pixel_count() * channels; }
};

// Decode a JPEG2000 blob (raw J2K codestream, e.g. Second Life .j2c, or a JP2
// wrapper) into 8-bit channels. Returns nullopt on malformed input, an
// unsupported layout (subsampled/mismatched components), or if no codec is
// compiled in.
std::optional<Image> decode_j2c(const std::vector<std::uint8_t>& data);

// Encode an Image as a lossless JPEG2000 codestream (.j2c). Returns nullopt on
// invalid input or encoder failure.
std::optional<std::vector<std::uint8_t>> encode_j2c(const Image& image);

// Decode a PNG or JPEG blob into an Image, keeping the channel count the file
// carries. These are the only two formats glTF allows for embedded images, so
// this is the entry point for extracting a GLB's textures (ADR 0033 M3).
// Returns nullopt on malformed or unsupported input.
// PNG, for the modern rendition of a texture a viewer uploaded as JPEG2000.
// Lossless, so it adds nothing to whatever the viewer's uploader already
// discarded.
std::optional<std::vector<std::uint8_t>> encode_png(const Image& image);

std::optional<Image> decode_png_or_jpeg(const std::vector<std::uint8_t>& data);

// Decode a TGA blob into an Image. Supports uncompressed and RLE, grayscale
// (types 3/11 -> 1-channel L) and truecolor (types 2/10, 24/32 bpp -> RGB/RGBA,
// BGRA reordered to RGBA), honoring the image-descriptor vertical origin.
// Returns nullopt on malformed or unsupported input. Used to load the clothing
// alpha masks for the bake (ADR 0029).
std::optional<Image> decode_tga(const std::vector<std::uint8_t>& data);

// Expand any 1..4-channel Image to a 4-channel RGBA copy (L->grey+opaque,
// LA->grey+alpha, RGB->opaque). An RGBA input is returned unchanged. An empty
// or malformed Image yields an empty Image.
Image to_rgba(const Image& src);

// Nearest-neighbor resample to width x height, preserving channel count.
// Returns an empty Image on invalid input.
Image resize_nearest(const Image& src, std::uint32_t width, std::uint32_t height);

// Box-filter (area-average) resample to width x height, preserving channel
// count. Returns an empty Image on invalid input.
//
// Separate from resize_nearest rather than replacing it, because the two are
// wanted for different things. Nearest is right for reading a mask, where the
// values mean something and averaging two of them invents a third. This is right
// for *shrinking a photograph*: dropping three of every four pixels of a skin
// map aliases into visible speckle, and averaging them does not. Every source
// pixel contributes exactly once, so a 4:1 reduction reads all 16 pixels rather
// than one of them.
//
// Enlarging is not what this is for and falls back to nearest, since a box that
// covers less than one source pixel has nothing to average.
Image resize_box(const Image& src, std::uint32_t width, std::uint32_t height);

// Encode an Image as a baseline JPEG at `quality` (1..100).
//
// The counterpart to encode_png, and the reason both exist: glTF permits PNG and
// JPEG only, PNG is the one that can carry alpha, and JPEG is the one that does
// not inflate a photograph. Re-encoding an opaque JPEG skin map as PNG can make
// it *larger* than the original it was meant to shrink, which defeats a
// downscale entirely. Alpha is dropped — JPEG cannot carry it — so this must not
// be handed an image whose alpha matters.
std::optional<std::vector<std::uint8_t>> encode_jpeg(const Image& image, int quality);

// Replace `rgba`'s alpha channel with `mask`'s luminance, resampling the mask to
// fit. `rgba` must be 4-channel; false is returned and nothing written if either
// image is empty or the resample fails.
//
// This is how an opacity map authored as its own image becomes glTF's
// base-colour alpha, and it lives here rather than in the importer because the
// convention it encodes is worth stating once and testing once:
//
//   - **White is opaque.** The two names disagree — FBX files these images under
//     `TransparentColor`, whose semantics are the opposite, while Reallusion
//     names the file `_Opacity`. An eyelash map settles it: white lashes on a
//     black card, and the lashes are the part you can see. Inverting this
//     renders a solid black rectangle where a lash should be, which looks like a
//     geometry fault rather than a channel one.
//   - **A colour mask is read as luminance**, not as its red channel, so a mask
//     authored as an RGB image does not silently lose two thirds of itself.
bool write_alpha_from_luminance(Image& rgba, const Image& mask);

// The share of pixels whose alpha is at least `at_least`, 0..1. A 3-channel or
// 1-channel image has no alpha and counts as fully opaque. Returns 0 for an
// empty image.
//
// This is how a *cutout* mask is told from a *soft* one, which decides whether a
// surface is alpha-tested or alpha-blended. Measured across the reference corpus
// the two populations do not overlap: eye occlusion, tearlines, a scalp cap and
// a soft eyelash map all sit at or below 0.004 above alpha 0.9, while hair,
// beards, brows and a crisp eyelash map sit at 0.044 and above — a tenfold gap
// with nothing in between.
double opaque_fraction(const Image& image, std::uint8_t at_least);

// An RGBA image of one colour everywhere, with `mask`'s luminance as its alpha.
//
// For a material that has an opacity map and no colour map: the colour is then a
// single value on the material and the mask is the whole of the surface's shape.
// Returns an empty Image if the mask is empty.
Image solid_with_alpha(std::array<std::uint8_t, 3> colour, const Image& mask);

// One contribution to a composite: a source image plus an RGB tint multiplied
// into its color channels (255,255,255 = no tint).
struct Layer {
    Image image;
    std::array<std::uint8_t, 3> tint{255, 255, 255};
};

// Composite layers bottom-to-top into a width x height RGBA image using
// straight-alpha source-over blending. Each layer is converted to RGBA, tinted,
// and resized to the target first. This is the bake step's pixel engine
// (ADR 0029): the first layer is the base (e.g. skin), later layers stack on
// top (clothing). Returns an empty Image on invalid dimensions.
Image composite_rgba(std::uint32_t width, std::uint32_t height,
                     const std::vector<Layer>& layers);

}  // namespace homeworldz::image

#endif  // HOMEWORLDZ_IMAGE_H
