#include "homeworldz/image.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

using homeworldz::image::composite_rgba;
using homeworldz::image::decode_j2c;
using homeworldz::image::decode_png_or_jpeg;
using homeworldz::image::encode_j2c;
using homeworldz::image::encode_jpeg;
using homeworldz::image::opaque_fraction;
using homeworldz::image::resize_box;
using homeworldz::image::Image;
using homeworldz::image::Layer;
using homeworldz::image::resize_nearest;
using homeworldz::image::to_rgba;

namespace {

// A solid width x height image with the given channels, every pixel = value.
Image make_solid(std::uint32_t w, std::uint32_t h, std::uint8_t channels,
                 std::array<std::uint8_t, 4> value) {
    Image img;
    img.width = w;
    img.height = h;
    img.channels = channels;
    img.pixels.resize(img.expected_size());
    for (std::size_t i = 0; i < img.pixel_count(); ++i)
        for (std::uint8_t c = 0; c < channels; ++c) img.pixels[i * channels + c] = value[c];
    return img;
}

bool near(int a, int b, int tol = 1) { return (a - b <= tol) && (b - a <= tol); }


// A small RGBA gradient so a lossless round-trip has real per-channel content
// to reproduce exactly.
Image make_gradient(std::uint32_t w, std::uint32_t h) {
    Image img;
    img.width = w;
    img.height = h;
    img.channels = 4;
    img.pixels.resize(img.expected_size());
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
            img.pixels[i + 0] = static_cast<std::uint8_t>(x * 255 / (w - 1));
            img.pixels[i + 1] = static_cast<std::uint8_t>(y * 255 / (h - 1));
            img.pixels[i + 2] = static_cast<std::uint8_t>((x + y) & 0xFF);
            img.pixels[i + 3] = 255;
        }
    }
    return img;
}

}  // namespace

int main() {
    const Image src = make_gradient(32, 24);

    auto encoded = encode_j2c(src);
    if (!encoded || encoded->empty()) {
        std::cerr << "encode_j2c failed\n";
        return 1;
    }

    auto decoded = decode_j2c(*encoded);
    if (!decoded) {
        std::cerr << "decode_j2c failed\n";
        return 1;
    }
    if (decoded->width != src.width || decoded->height != src.height ||
        decoded->channels != src.channels) {
        std::cerr << "dimensions changed across round-trip: " << decoded->width << "x"
                  << decoded->height << "x" << static_cast<int>(decoded->channels) << '\n';
        return 1;
    }
    // Small images stay lossless: a compression ratio aims at a byte count, and
    // below a floor that count destroys the image to save nothing. This 32x24
    // is under the floor, so it must come back bit-exact.
    if (decoded->pixels != src.pixels) {
        std::cerr << "small-image round-trip did not reproduce pixels exactly\n";
        return 1;
    }

    // A four-channel image round-trips with its alpha intact. This is the
    // path a GLB texture takes (ADR 0033 M3): PNGs are commonly RGBA even
    // when fully opaque, and an encoder that dropped or corrupted the fourth
    // component would turn an opaque face transparent in a viewer while every
    // byte-level check upstream still passed.
    {
        Image rgba;
        rgba.width = 8;
        rgba.height = 8;
        rgba.channels = 4;
        rgba.pixels.resize(rgba.expected_size());
        for (std::uint32_t y = 0; y < rgba.height; ++y)
            for (std::uint32_t x = 0; x < rgba.width; ++x) {
                const auto index = (static_cast<std::size_t>(y) * rgba.width + x) * 4;
                const bool red = ((x + y) % 2) == 0;
                rgba.pixels[index] = 255;
                rgba.pixels[index + 1] = red ? 0 : 255;
                rgba.pixels[index + 2] = red ? 0 : 255;
                rgba.pixels[index + 3] = 255;  // fully opaque throughout
            }
        const auto coded = encode_j2c(rgba);
        if (!coded || coded->empty()) {
            std::cerr << "encode_j2c refused a four-channel image\n";
            return 1;
        }
        const auto back = decode_j2c(*coded);
        if (!back) {
            std::cerr << "decode_j2c refused its own four-channel output\n";
            return 1;
        }
        if (back->channels != 4) {
            std::cerr << "four-channel round-trip lost a component: "
                      << static_cast<int>(back->channels) << '\n';
            return 1;
        }
        for (std::size_t pixel = 0; pixel < back->pixel_count(); ++pixel)
            if (back->pixels[pixel * 4 + 3] != 255) {
                std::cerr << "alpha did not survive the round-trip at pixel " << pixel
                          << ": " << static_cast<int>(back->pixels[pixel * 4 + 3]) << '\n';
                return 1;
            }
    }

    // Above the small-image floor the encode is lossy, because a viewer's
    // JPEG2000 is a derived form and lossless made a 1024 terrain layer 2 MB
    // (measured 2026-07-31). Two things have to hold together, and neither
    // alone is worth anything: it must actually compress, and what comes back
    // must still be the picture. So assert the size *and* the peak
    // signal-to-noise ratio, on a photographic-ish gradient rather than a
    // synthetic checkerboard, since hard edges are the one thing a wavelet
    // coder flatters least and terrain is not made of them.
    {
        Image big;
        big.width = 256;
        big.height = 256;
        big.channels = 3;
        big.pixels.resize(big.expected_size());
        for (std::uint32_t y = 0; y < big.height; ++y)
            for (std::uint32_t x = 0; x < big.width; ++x) {
                const auto index = (static_cast<std::size_t>(y) * big.width + x) * 3;
                // Smooth ramps plus a low-amplitude ripple: continuous tone
                // with real local variation, which is what a ground texture is.
                const auto ripple = static_cast<int>(12.0 * std::sin(x * 0.15) * std::cos(y * 0.11));
                big.pixels[index + 0] = static_cast<std::uint8_t>(
                    std::clamp(static_cast<int>(x) / 2 + 40 + ripple, 0, 255));
                big.pixels[index + 1] = static_cast<std::uint8_t>(
                    std::clamp(static_cast<int>(y) / 2 + 60 - ripple, 0, 255));
                big.pixels[index + 2] = static_cast<std::uint8_t>(
                    std::clamp(static_cast<int>(x + y) / 4 + 30 + ripple, 0, 255));
            }
        const auto coded = encode_j2c(big);
        if (!coded || coded->empty()) {
            std::cerr << "encode_j2c refused a 256x256 image\n";
            return 1;
        }
        // 20:1 against the raw samples, with slack for header and rounding.
        const auto raw = big.pixels.size();
        if (coded->size() > raw / 10) {
            std::cerr << "lossy encode did not compress: " << coded->size() << " bytes from "
                      << raw << " raw (expected near " << raw / 20 << ")\n";
            return 1;
        }
        const auto back = decode_j2c(*coded);
        if (!back || back->pixels.size() != big.pixels.size()) {
            std::cerr << "lossy round-trip did not return the same shape\n";
            return 1;
        }
        double squared = 0.0;
        for (std::size_t i = 0; i < big.pixels.size(); ++i) {
            const double difference =
                static_cast<int>(back->pixels[i]) - static_cast<int>(big.pixels[i]);
            squared += difference * difference;
        }
        const auto mean_squared = squared / static_cast<double>(big.pixels.size());
        const auto psnr = mean_squared > 0.0 ? 10.0 * std::log10(255.0 * 255.0 / mean_squared) : 99.0;
        // 40 dB is the conventional threshold for visually lossless
        // photographic content; measured 2026-07-31 at well above it.
        if (psnr < 40.0) {
            std::cerr << "lossy round-trip lost too much: PSNR " << psnr << " dB\n";
            return 1;
        }
        std::cerr << "image j2c lossy encode OK (" << raw << " raw -> " << coded->size()
                  << " bytes, PSNR " << psnr << " dB)\n";
    }


    // PNG encoding, the reverse rendition direction. A texture a viewer uploaded
    // is canonically JPEG2000 and the first-party client refuses that by rule, so
    // it needs a modern copy. PNG is lossless, so the rendition must reproduce
    // the canonical's pixels *exactly* — anything less would be a second
    // generation of loss on top of the viewer's own.
    {
        const Image source = make_gradient(48, 32);
        const auto png = homeworldz::image::encode_png(source);
        if (!png || png->empty()) {
            std::cerr << "encode_png failed\n";
            return 1;
        }
        // A real PNG, by its signature, not merely some bytes.
        static const std::uint8_t signature[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
        if (png->size() < sizeof(signature) ||
            !std::equal(std::begin(signature), std::end(signature), png->begin())) {
            std::cerr << "encode_png did not produce a PNG signature\n";
            return 1;
        }
        const auto back = homeworldz::image::decode_png_or_jpeg(*png);
        if (!back) {
            std::cerr << "the PNG we wrote could not be decoded\n";
            return 1;
        }
        if (back->width != source.width || back->height != source.height ||
            back->channels != source.channels) {
            std::cerr << "PNG round-trip changed the shape\n";
            return 1;
        }
        if (back->pixels != source.pixels) {
            std::cerr << "PNG round-trip was not lossless\n";
            return 1;
        }
        // And the path a viewer-uploaded texture actually takes: JPEG2000
        // canonical in, PNG rendition out, both readable.
        const auto j2c = homeworldz::image::encode_j2c(source);
        if (!j2c) {
            std::cerr << "encode_j2c failed for the reverse-direction fixture\n";
            return 1;
        }
        const auto decoded_j2c = homeworldz::image::decode_j2c(*j2c);
        if (!decoded_j2c) {
            std::cerr << "decode_j2c failed for the reverse-direction fixture\n";
            return 1;
        }
        const auto rendition = homeworldz::image::encode_png(*decoded_j2c);
        if (!rendition) {
            std::cerr << "encode_png refused a JPEG2000 decode\n";
            return 1;
        }
        const auto final_image = homeworldz::image::decode_png_or_jpeg(*rendition);
        if (!final_image || final_image->pixels != decoded_j2c->pixels) {
            std::cerr << "the png-texture rendition did not preserve the canonical decode\n";
            return 1;
        }
        std::cerr << "image png encode OK (" << source.pixels.size() << " raw -> "
                  << png->size() << " bytes, lossless; j2c->png rendition preserved)\n";
        // An empty image is refused rather than producing a zero-pixel PNG.
        if (homeworldz::image::encode_png(Image{})) {
            std::cerr << "encode_png accepted an empty image\n";
            return 1;
        }
    }

    // Garbage input must be rejected, not crash.
    if (decode_j2c(std::vector<std::uint8_t>{0x00, 0x01, 0x02, 0x03, 0x04}).has_value()) {
        std::cerr << "decode_j2c accepted non-JPEG2000 input\n";
        return 1;
    }

    // to_rgba expands RGB to opaque RGBA.
    {
        Image rgb = make_solid(2, 2, 3, {10, 20, 30, 0});
        Image rgba = to_rgba(rgb);
        if (rgba.channels != 4 || rgba.pixels[0] != 10 || rgba.pixels[1] != 20 ||
            rgba.pixels[2] != 30 || rgba.pixels[3] != 255) {
            std::cerr << "to_rgba(RGB) did not expand to opaque RGBA\n";
            return 1;
        }
    }

    // resize_nearest doubles dimensions and preserves the solid color.
    {
        Image src = make_solid(2, 2, 4, {5, 6, 7, 8});
        Image big = resize_nearest(src, 4, 4);
        if (big.width != 4 || big.height != 4 || big.pixels.size() != 4 * 4 * 4 ||
            big.pixels[0] != 5 || big.pixels[3] != 8) {
            std::cerr << "resize_nearest failed\n";
            return 1;
        }
    }

    // Tint multiplies the source color: white tinted red -> red.
    {
        std::vector<Layer> layers{{make_solid(1, 1, 4, {255, 255, 255, 255}), {255, 0, 0}}};
        Image out = composite_rgba(1, 1, layers);
        if (!near(out.pixels[0], 255) || !near(out.pixels[1], 0) ||
            !near(out.pixels[2], 0) || out.pixels[3] != 255) {
            std::cerr << "tinted composite wrong: " << int(out.pixels[0]) << ','
                      << int(out.pixels[1]) << ',' << int(out.pixels[2]) << '\n';
            return 1;
        }
    }

    // Source-over: 50% blue over opaque red -> ~(127,0,128,255).
    {
        std::vector<Layer> layers{
            {make_solid(1, 1, 4, {255, 0, 0, 255}), {255, 255, 255}},
            {make_solid(1, 1, 4, {0, 0, 255, 128}), {255, 255, 255}},
        };
        Image out = composite_rgba(1, 1, layers);
        if (!near(out.pixels[0], 127, 2) || !near(out.pixels[1], 0) ||
            !near(out.pixels[2], 128, 2) || out.pixels[3] != 255) {
            std::cerr << "alpha blend wrong: " << int(out.pixels[0]) << ','
                      << int(out.pixels[1]) << ',' << int(out.pixels[2]) << ','
                      << int(out.pixels[3]) << '\n';
            return 1;
        }
    }

    // An opacity map becoming base-colour alpha. White is opaque: an eyelash map
    // is white lashes on a black card, and inverting it draws a black rectangle
    // where a lash should be — which reads as a geometry fault, not a channel
    // one, so it is worth a test rather than a comment.
    {
        Image rgba = make_solid(2, 2, 4, {10, 20, 30, 255});
        Image mask = make_solid(2, 2, 1, {255});
        mask.pixels[0] = 0;    // one transparent corner
        mask.pixels[1] = 128;  // and one half-way
        if (!write_alpha_from_luminance(rgba, mask)) {
            std::cerr << "write_alpha_from_luminance refused a good pair\n";
            return 1;
        }
        if (rgba.pixels[3] != 0 || rgba.pixels[7] != 128 || rgba.pixels[11] != 255) {
            std::cerr << "alpha from mask wrong: " << int(rgba.pixels[3]) << ','
                      << int(rgba.pixels[7]) << ',' << int(rgba.pixels[11]) << '\n';
            return 1;
        }
        // The colour is untouched — only alpha is written.
        if (rgba.pixels[0] != 10 || rgba.pixels[1] != 20 || rgba.pixels[2] != 30) {
            std::cerr << "alpha write disturbed the colour channels\n";
            return 1;
        }
    }

    // A colour mask is read as luminance, not as its red channel: a mask
    // authored as an RGB image must not lose two thirds of itself. Pure green at
    // Rec. 601 weights is 151/256 of full.
    {
        Image rgba = make_solid(1, 1, 4, {0, 0, 0, 255});
        const Image green = make_solid(1, 1, 3, {0, 255, 0});
        if (!write_alpha_from_luminance(rgba, green)) return 1;
        if (!near(rgba.pixels[3], 151, 1)) {
            std::cerr << "colour mask not read as luminance: " << int(rgba.pixels[3]) << '\n';
            return 1;
        }
    }

    // A mask of a different size than the surface is resampled rather than
    // refused: Character Creator authors these at whatever size it likes.
    {
        Image rgba = make_solid(4, 4, 4, {1, 2, 3, 255});
        const Image mask = make_solid(2, 2, 1, {64});
        if (!write_alpha_from_luminance(rgba, mask)) return 1;
        for (std::size_t at = 0; at < rgba.pixel_count(); ++at)
            if (rgba.pixels[at * 4 + 3] != 64) {
                std::cerr << "resampled mask lost a pixel at " << at << '\n';
                return 1;
            }
    }

    // A material with an opacity map and no colour map: one colour everywhere,
    // the mask carrying the whole of the shape. Character Creator's eye
    // occlusion is exactly this, and treating it as textureless published the
    // part with no texture at all — which the viewer draws as opaque white, two
    // white shells over the eyes (in-world, 2026-08-11).
    {
        Image mask = make_solid(2, 1, 1, {255});
        mask.pixels[1] = 0;
        const Image out = solid_with_alpha({0, 0, 0}, mask);
        if (out.width != 2 || out.height != 1 || out.channels != 4) {
            std::cerr << "solid_with_alpha did not take the mask's shape\n";
            return 1;
        }
        if (out.pixels[0] != 0 || out.pixels[1] != 0 || out.pixels[2] != 0 ||
            out.pixels[3] != 255 || out.pixels[7] != 0) {
            std::cerr << "solid_with_alpha wrong: " << int(out.pixels[3]) << ','
                      << int(out.pixels[7]) << '\n';
            return 1;
        }
        // An empty mask has no shape to take, so there is nothing to publish.
        if (!solid_with_alpha({0, 0, 0}, Image{}).empty()) {
            std::cerr << "solid_with_alpha invented an image from an empty mask\n";
            return 1;
        }
    }

    // resize_box averages what it covers rather than picking one of them, which
    // is the whole reason it exists next to resize_nearest. A 2x2 of four known
    // values down to 1x1 has exactly one right answer, and nearest gets it
    // wrong: it would return a corner.
    {
        Image quad;
        quad.width = 2;
        quad.height = 2;
        quad.channels = 1;
        quad.pixels = {0, 100, 200, 255};
        const Image one = resize_box(quad, 1, 1);
        if (one.width != 1 || one.height != 1 || one.channels != 1) {
            std::cerr << "resize_box did not produce a 1x1\n";
            return 1;
        }
        // (0 + 100 + 200 + 255 + 2) / 4 == 139
        if (one.pixels[0] != 139) {
            std::cerr << "resize_box averaged wrong: " << int(one.pixels[0]) << " (want 139)\n";
            return 1;
        }
        // Every source pixel contributes exactly once: a 4x1 halved must be the
        // two pairwise means, not two of the four originals.
        Image row;
        row.width = 4;
        row.height = 1;
        row.channels = 1;
        row.pixels = {10, 20, 30, 40};
        const Image half = resize_box(row, 2, 1);
        if (half.width != 2 || half.pixels[0] != 15 || half.pixels[1] != 35) {
            std::cerr << "resize_box pairwise wrong: " << int(half.pixels[0]) << ','
                      << int(half.pixels[1]) << " (want 15,35)\n";
            return 1;
        }
        // Enlarging has nothing to average and must not come back empty.
        if (resize_box(quad, 4, 4).width != 4) {
            std::cerr << "resize_box refused to enlarge\n";
            return 1;
        }
        if (!resize_box(Image{}, 2, 2).empty()) {
            std::cerr << "resize_box invented an image from an empty one\n";
            return 1;
        }
    }

    // encode_jpeg has to drop alpha rather than write it as colour, and has to
    // actually shrink a photograph — the reason it is used instead of PNG.
    {
        Image rgba = make_solid(64, 64, 4, {200, 50, 25, 128});
        const auto jpeg = encode_jpeg(rgba, 90);
        if (!jpeg) {
            std::cerr << "encode_jpeg refused an RGBA image\n";
            return 1;
        }
        const auto back = decode_png_or_jpeg(*jpeg);
        if (!back) {
            std::cerr << "encode_jpeg produced something undecodable\n";
            return 1;
        }
        if (back->channels != 3) {
            std::cerr << "encode_jpeg kept " << int(back->channels)
                      << " channels; alpha must be dropped, not written as colour\n";
            return 1;
        }
        if (back->width != 64 || back->height != 64) {
            std::cerr << "encode_jpeg changed the dimensions\n";
            return 1;
        }
        if (encode_jpeg(Image{}, 90)) {
            std::cerr << "encode_jpeg accepted an empty image\n";
            return 1;
        }
    }

    // opaque_fraction is what separates a cutout mask from a soft one, and the
    // separation is only meaningful if the count is exact at the boundary.
    {
        Image half = make_solid(2, 2, 4, {0, 0, 0, 255});
        half.pixels[3] = 255;   // one pixel opaque
        half.pixels[7] = 230;   // one exactly at the threshold: counts
        half.pixels[11] = 229;  // one just below: does not
        half.pixels[15] = 0;    // clear
        const auto share = opaque_fraction(half, 230);
        if (share < 0.49 || share > 0.51) {
            std::cerr << "opaque_fraction wrong: " << share << " (want 0.5)\n";
            return 40;
        }
        // An image with no alpha channel cannot be less than opaque.
        if (opaque_fraction(make_solid(2, 2, 3, {1, 2, 3, 0}), 230) != 1.0) {
            std::cerr << "opaque_fraction treated an RGB image as transparent\n";
            return 41;
        }
        if (opaque_fraction(Image{}, 230) != 0.0) {
            std::cerr << "opaque_fraction invented coverage for an empty image\n";
            return 42;
        }
    }

    std::cerr << "image j2c lossless round-trip OK (" << encoded->size() << " bytes)\n";
    std::cerr << "image box resample and jpeg encode OK\n";
    std::cerr << "image composite/resize/tint OK\n";
    std::cerr << "image opacity-to-alpha OK\n";
    return 0;
}
