#include "homeworldz/image.h"

#include <algorithm>
#include <cstring>

#if defined(HOMEWORLDZ_OPENJPEG)
#include <openjpeg.h>
#endif

// stb_image, compiled here once. Only PNG and JPEG are enabled: those are the
// two formats glTF permits embedded (ADR 0033 M3), and every other decoder stb
// carries would be attack surface reachable from an upload for no benefit.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_FAILURE_USERMSG
#include <stb_image.h>

// stb_image_write, for the reverse direction: a texture a viewer uploaded is
// canonically JPEG2000, which the first-party client refuses by rule, so it needs
// a modern rendition derived from it. Only PNG is used — it is lossless, so the
// rendition adds no loss of its own on top of whatever the viewer's uploader
// already did.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

namespace homeworldz::image {

#if defined(HOMEWORLDZ_OPENJPEG)
namespace {

// OpenJPEG has no public in-memory stream, so we drive one with our own read /
// write / skip / seek callbacks over a plain byte buffer.

struct ReadCtx {
    const std::uint8_t* data;
    OPJ_SIZE_T size;
    OPJ_SIZE_T pos;
};

OPJ_SIZE_T mem_read(void* buffer, OPJ_SIZE_T nb, void* user) {
    auto* c = static_cast<ReadCtx*>(user);
    if (c->pos >= c->size) return static_cast<OPJ_SIZE_T>(-1);  // EOF
    OPJ_SIZE_T remaining = c->size - c->pos;
    OPJ_SIZE_T n = nb < remaining ? nb : remaining;
    std::memcpy(buffer, c->data + c->pos, n);
    c->pos += n;
    return n;
}

OPJ_OFF_T mem_skip(OPJ_OFF_T nb, void* user) {
    auto* c = static_cast<ReadCtx*>(user);
    if (nb < 0) return -1;
    OPJ_SIZE_T remaining = c->size - c->pos;
    OPJ_SIZE_T n = static_cast<OPJ_SIZE_T>(nb) < remaining ? static_cast<OPJ_SIZE_T>(nb)
                                                          : remaining;
    c->pos += n;
    return static_cast<OPJ_OFF_T>(n);
}

OPJ_BOOL mem_seek(OPJ_OFF_T pos, void* user) {
    auto* c = static_cast<ReadCtx*>(user);
    if (pos < 0 || static_cast<OPJ_SIZE_T>(pos) > c->size) return OPJ_FALSE;
    c->pos = static_cast<OPJ_SIZE_T>(pos);
    return OPJ_TRUE;
}

struct WriteCtx {
    std::vector<std::uint8_t>* out;
    OPJ_SIZE_T pos;
};

OPJ_SIZE_T mem_write(void* buffer, OPJ_SIZE_T nb, void* user) {
    auto* c = static_cast<WriteCtx*>(user);
    if (c->out->size() < c->pos + nb) c->out->resize(c->pos + nb);
    std::memcpy(c->out->data() + c->pos, buffer, nb);
    c->pos += nb;
    return nb;
}

OPJ_OFF_T write_skip(OPJ_OFF_T nb, void* user) {
    auto* c = static_cast<WriteCtx*>(user);
    if (nb < 0) return -1;
    c->pos += static_cast<OPJ_SIZE_T>(nb);
    if (c->out->size() < c->pos) c->out->resize(c->pos);
    return nb;
}

OPJ_BOOL write_seek(OPJ_OFF_T pos, void* user) {
    auto* c = static_cast<WriteCtx*>(user);
    if (pos < 0) return OPJ_FALSE;
    c->pos = static_cast<OPJ_SIZE_T>(pos);
    if (c->out->size() < c->pos) c->out->resize(c->pos);
    return OPJ_TRUE;
}

void quiet(const char*, void*) {}

OPJ_CODEC_FORMAT detect_format(const std::vector<std::uint8_t>& d) {
    // JP2 files open with the signature box; anything else is treated as a raw
    // J2K codestream (Second Life .j2c starts with the SOC marker 0xFF4F).
    static const std::uint8_t jp2_sig[] = {0x00, 0x00, 0x00, 0x0C, 0x6A, 0x50,
                                           0x20, 0x20, 0x0D, 0x0A, 0x87, 0x0A};
    if (d.size() >= sizeof(jp2_sig) &&
        std::memcmp(d.data(), jp2_sig, sizeof(jp2_sig)) == 0) {
        return OPJ_CODEC_JP2;
    }
    return OPJ_CODEC_J2K;
}

// Target compression for derived JPEG2000, as a ratio against the raw samples.
// Chosen to sit where the peak signal-to-noise ratio of a decode against its
// own canonical stays above the ~40 dB that is conventionally taken as
// visually lossless for photographic content, which the terrain layers and
// avatar bakes both are; the figure this achieves in practice is asserted in
// the image tests rather than trusted here.
constexpr float j2c_compression_ratio = 20.0F;

// ...but never aim below this many bytes. A ratio is meaningless on a small
// image: 20:1 on an 8x8 RGBA targets thirteen bytes, which destroys it to save
// nothing. Below the floor the encode stays lossless, so masks, swatches and
// the small fixtures come back exactly.
constexpr float j2c_minimum_target_bytes = 2048.0F;

// The ratio to actually hand the encoder for an image of this many raw bytes,
// or zero for lossless (OpenJPEG's own spelling of it).
float j2c_rate_for(std::size_t raw_bytes) {
    const auto raw = static_cast<float>(raw_bytes);
    if (raw / j2c_compression_ratio >= j2c_minimum_target_bytes)
        return j2c_compression_ratio;
    const auto reduced = raw / j2c_minimum_target_bytes;
    return reduced > 1.0F ? reduced : 0.0F;
}

}  // namespace

std::optional<Image> decode_j2c(const std::vector<std::uint8_t>& data) {
    if (data.size() < 4) return std::nullopt;

    ReadCtx ctx{data.data(), static_cast<OPJ_SIZE_T>(data.size()), 0};
    opj_stream_t* stream = opj_stream_default_create(OPJ_TRUE);
    if (!stream) return std::nullopt;
    opj_stream_set_user_data(stream, &ctx, nullptr);
    opj_stream_set_user_data_length(stream, data.size());
    opj_stream_set_read_function(stream, mem_read);
    opj_stream_set_skip_function(stream, mem_skip);
    opj_stream_set_seek_function(stream, mem_seek);

    opj_codec_t* codec = opj_create_decompress(detect_format(data));
    if (!codec) {
        opj_stream_destroy(stream);
        return std::nullopt;
    }
    opj_set_error_handler(codec, quiet, nullptr);
    opj_set_warning_handler(codec, quiet, nullptr);
    opj_set_info_handler(codec, quiet, nullptr);

    opj_dparameters_t dparams;
    opj_set_default_decoder_parameters(&dparams);

    opj_image_t* img = nullptr;
    std::optional<Image> result;
    if (opj_setup_decoder(codec, &dparams) && opj_read_header(stream, codec, &img) &&
        opj_decode(codec, stream, img) && opj_end_decompress(codec, stream)) {
        const OPJ_UINT32 n = img->numcomps;
        if (n >= 1 && n <= 4 && img->comps[0].data) {
            const OPJ_UINT32 w = img->comps[0].w;
            const OPJ_UINT32 h = img->comps[0].h;
            bool ok = w > 0 && h > 0;
            for (OPJ_UINT32 c = 0; c < n && ok; ++c) {
                const auto& comp = img->comps[c];
                if (comp.w != w || comp.h != h || comp.dx != 1 || comp.dy != 1 ||
                    !comp.data) {
                    ok = false;
                }
            }
            if (ok) {
                Image out;
                out.width = w;
                out.height = h;
                out.channels = static_cast<std::uint8_t>(n);
                out.pixels.resize(out.expected_size());
                const std::size_t count = out.pixel_count();
                for (OPJ_UINT32 c = 0; c < n; ++c) {
                    const auto& comp = img->comps[c];
                    const int prec = static_cast<int>(comp.prec);
                    const int shift = prec > 8 ? prec - 8 : 0;
                    const int adjust = comp.sgnd ? (1 << (prec - 1)) : 0;
                    const OPJ_INT32* src = comp.data;
                    for (std::size_t i = 0; i < count; ++i) {
                        int v = (src[i] + adjust) >> shift;
                        if (v < 0) v = 0;
                        if (v > 255) v = 255;
                        out.pixels[i * n + c] = static_cast<std::uint8_t>(v);
                    }
                }
                result = std::move(out);
            }
        }
    }

    if (img) opj_image_destroy(img);
    opj_destroy_codec(codec);
    opj_stream_destroy(stream);
    return result;
}

std::optional<std::vector<std::uint8_t>> encode_j2c(const Image& image) {
    if (image.empty() || image.channels < 1 || image.channels > 4) return std::nullopt;
    if (image.pixels.size() != image.expected_size()) return std::nullopt;

    const OPJ_UINT32 n = image.channels;

    std::vector<opj_image_cmptparm_t> cmpt(n);
    for (OPJ_UINT32 c = 0; c < n; ++c) {
        cmpt[c] = opj_image_cmptparm_t{};
        cmpt[c].dx = 1;
        cmpt[c].dy = 1;
        cmpt[c].w = image.width;
        cmpt[c].h = image.height;
        cmpt[c].x0 = 0;
        cmpt[c].y0 = 0;
        cmpt[c].prec = 8;
        cmpt[c].bpp = 8;
        cmpt[c].sgnd = 0;
    }

    const OPJ_COLOR_SPACE cs = (n >= 3) ? OPJ_CLRSPC_SRGB : OPJ_CLRSPC_GRAY;
    opj_image_t* img = opj_image_create(n, cmpt.data(), cs);
    if (!img) return std::nullopt;
    img->x0 = 0;
    img->y0 = 0;
    img->x1 = image.width;
    img->y1 = image.height;

    const std::size_t count = image.pixel_count();
    for (OPJ_UINT32 c = 0; c < n; ++c) {
        OPJ_INT32* dst = img->comps[c].data;
        for (std::size_t i = 0; i < count; ++i) {
            dst[i] = image.pixels[i * n + c];
        }
    }

    opj_cparameters_t params;
    opj_set_default_encoder_parameters(&params);
    params.tcp_numlayers = 1;
    params.cp_disto_alloc = 1;
    // Every JPEG2000 this produces is a derived form for a viewer, and no
    // texture a viewer has ever loaded was lossless - the Second Life terrain
    // layers this grid shipped until today were 6 KB for 128x128, about 8:1.
    // Encoding losslessly made a 1024 layer 2 MB, so four of them were 8 MB of
    // ground at every login (measured 2026-07-31), and avatar bakes were
    // running a quarter of a megabyte each for the same reason.
    //
    // The canonical stays lossless: it is a PNG or JPEG in the vault and this
    // never touches it. Only the regenerable rendition is compressed, which is
    // what the rendition layer is for - if this ratio is ever wrong, every
    // affected asset reconverts from a canonical that never lost anything.
    params.tcp_rates[0] = j2c_rate_for(image.pixels.size());
    // Wavelet decomposition levels must fit the image: the smallest dimension
    // has to survive numresolution-1 halvings. Clamp so small images (and
    // non-power-of-two bake slots) still encode.
    const std::uint32_t min_dim = std::min(image.width, image.height);
    int numres = 1;
    while (numres < params.numresolution && (min_dim >> numres) >= 1) ++numres;
    params.numresolution = numres;

    opj_codec_t* codec = opj_create_compress(OPJ_CODEC_J2K);
    if (!codec) {
        opj_image_destroy(img);
        return std::nullopt;
    }
    opj_set_error_handler(codec, quiet, nullptr);
    opj_set_warning_handler(codec, quiet, nullptr);
    opj_set_info_handler(codec, quiet, nullptr);

    std::vector<std::uint8_t> out;
    WriteCtx ctx{&out, 0};
    opj_stream_t* stream = opj_stream_default_create(OPJ_FALSE);
    std::optional<std::vector<std::uint8_t>> result;
    if (stream && opj_setup_encoder(codec, &params, img)) {
        opj_stream_set_user_data(stream, &ctx, nullptr);
        opj_stream_set_write_function(stream, mem_write);
        opj_stream_set_skip_function(stream, write_skip);
        opj_stream_set_seek_function(stream, write_seek);
        if (opj_start_compress(codec, img, stream) && opj_encode(codec, stream) &&
            opj_end_compress(codec, stream)) {
            out.resize(ctx.pos);
            result = std::move(out);
        }
    }

    if (stream) opj_stream_destroy(stream);
    opj_destroy_codec(codec);
    opj_image_destroy(img);
    return result;
}

#else  // no JPEG2000 codec compiled in

std::optional<Image> decode_j2c(const std::vector<std::uint8_t>&) { return std::nullopt; }

std::optional<std::vector<std::uint8_t>> encode_j2c(const Image&) { return std::nullopt; }

#endif

// ---- codec-independent image operations (compositing pixel engine) ----

std::optional<Image> decode_tga(const std::vector<std::uint8_t>& data) {
    if (data.size() < 18) return std::nullopt;
    const std::uint8_t id_length = data[0];
    const std::uint8_t color_map_type = data[1];
    const std::uint8_t image_type = data[2];
    const auto color_map_length = static_cast<std::uint16_t>(data[5] | (data[6] << 8));
    const std::uint8_t color_map_entry_size = data[7];
    const auto width = static_cast<std::uint32_t>(data[12] | (data[13] << 8));
    const auto height = static_cast<std::uint32_t>(data[14] | (data[15] << 8));
    const std::uint8_t bpp = data[16];
    const std::uint8_t descriptor = data[17];
    if (width == 0 || height == 0) return std::nullopt;

    const bool rle = (image_type == 10 || image_type == 11);
    const std::uint8_t base_type = (image_type == 10) ? 2 : (image_type == 11) ? 3 : image_type;
    if (base_type != 2 && base_type != 3) return std::nullopt;  // truecolor / grayscale only
    const std::uint8_t bytes_per_pixel = bpp / 8;
    if (base_type == 3 && bytes_per_pixel != 1) return std::nullopt;
    if (base_type == 2 && bytes_per_pixel != 3 && bytes_per_pixel != 4) return std::nullopt;

    std::size_t offset = std::size_t{18} + id_length;
    if (color_map_type == 1)
        offset += static_cast<std::size_t>(color_map_length) * ((color_map_entry_size + 7u) / 8u);
    if (offset > data.size()) return std::nullopt;

    const std::size_t pixel_count = static_cast<std::size_t>(width) * height;
    std::vector<std::uint8_t> raw(pixel_count * bytes_per_pixel);
    if (!rle) {
        if (offset + raw.size() > data.size()) return std::nullopt;
        std::memcpy(raw.data(), data.data() + offset, raw.size());
    } else {
        std::size_t out = 0;
        std::size_t in = offset;
        while (out < raw.size()) {
            if (in >= data.size()) return std::nullopt;
            const std::uint8_t packet = data[in++];
            const std::size_t count = (packet & 0x7Fu) + 1u;
            if (packet & 0x80u) {  // RLE packet: one pixel repeated `count` times
                if (in + bytes_per_pixel > data.size()) return std::nullopt;
                for (std::size_t k = 0; k < count && out < raw.size(); ++k) {
                    std::memcpy(raw.data() + out, data.data() + in, bytes_per_pixel);
                    out += bytes_per_pixel;
                }
                in += bytes_per_pixel;
            } else {  // raw packet: `count` literal pixels
                const std::size_t bytes = count * bytes_per_pixel;
                if (in + bytes > data.size()) return std::nullopt;
                const std::size_t copy = std::min(bytes, raw.size() - out);
                std::memcpy(raw.data() + out, data.data() + in, copy);
                out += copy;
                in += bytes;
            }
        }
    }

    Image image;
    image.width = width;
    image.height = height;
    image.channels = (base_type == 3) ? 1 : bytes_per_pixel;
    image.pixels.resize(pixel_count * image.channels);
    for (std::size_t i = 0; i < pixel_count; ++i) {
        const std::uint8_t* s = &raw[i * bytes_per_pixel];
        std::uint8_t* d = &image.pixels[i * image.channels];
        if (base_type == 3) {
            d[0] = s[0];
        } else if (bytes_per_pixel == 3) {  // BGR -> RGB
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
        } else {  // BGRA -> RGBA
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            d[3] = s[3];
        }
    }

    // Descriptor bit 5 set = origin top-left (rows top->bottom); clear =
    // bottom-left, so flip vertically to top-down.
    if ((descriptor & 0x20u) == 0) {
        const std::size_t row = static_cast<std::size_t>(width) * image.channels;
        std::vector<std::uint8_t> tmp(row);
        for (std::uint32_t y = 0; y < height / 2; ++y) {
            std::uint8_t* top = &image.pixels[static_cast<std::size_t>(y) * row];
            std::uint8_t* bot = &image.pixels[static_cast<std::size_t>(height - 1 - y) * row];
            std::memcpy(tmp.data(), top, row);
            std::memcpy(top, bot, row);
            std::memcpy(bot, tmp.data(), row);
        }
    }
    return image;
}

Image to_rgba(const Image& src) {
    if (src.empty() || src.channels < 1 || src.channels > 4 ||
        src.pixels.size() != src.expected_size()) {
        return {};
    }
    if (src.channels == 4) return src;

    Image out;
    out.width = src.width;
    out.height = src.height;
    out.channels = 4;
    out.pixels.resize(out.expected_size());
    const std::size_t count = src.pixel_count();
    const std::uint8_t sc = src.channels;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t* s = &src.pixels[i * sc];
        std::uint8_t* d = &out.pixels[i * 4];
        switch (sc) {
            case 1:  // luminance
                d[0] = d[1] = d[2] = s[0];
                d[3] = 255;
                break;
            case 2:  // luminance + alpha
                d[0] = d[1] = d[2] = s[0];
                d[3] = s[1];
                break;
            default:  // 3: RGB
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
                d[3] = 255;
                break;
        }
    }
    return out;
}

Image resize_nearest(const Image& src, std::uint32_t width, std::uint32_t height) {
    if (src.empty() || width == 0 || height == 0 || src.channels < 1 ||
        src.pixels.size() != src.expected_size()) {
        return {};
    }
    if (src.width == width && src.height == height) return src;

    Image out;
    out.width = width;
    out.height = height;
    out.channels = src.channels;
    out.pixels.resize(out.expected_size());
    const std::uint8_t c = src.channels;
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint32_t sy = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(y) * src.height) / height);
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t sx = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(x) * src.width) / width);
            const std::uint8_t* s = &src.pixels[(static_cast<std::size_t>(sy) * src.width + sx) * c];
            std::uint8_t* d = &out.pixels[(static_cast<std::size_t>(y) * width + x) * c];
            for (std::uint8_t k = 0; k < c; ++k) d[k] = s[k];
        }
    }
    return out;
}

Image resize_box(const Image& src, std::uint32_t width, std::uint32_t height) {
    if (src.empty() || width == 0 || height == 0 || src.channels < 1 ||
        src.pixels.size() != src.expected_size()) {
        return {};
    }
    if (src.width == width && src.height == height) return src;
    // A box smaller than a source pixel has nothing to average; nearest is both
    // the honest answer and the one already tested.
    if (width > src.width || height > src.height) return resize_nearest(src, width, height);

    Image out;
    out.width = width;
    out.height = height;
    out.channels = src.channels;
    out.pixels.resize(out.expected_size());
    const std::uint8_t c = src.channels;
    for (std::uint32_t y = 0; y < height; ++y) {
        // Half-open source rows [y0, y1) for this destination row, so the boxes
        // tile the source exactly and no pixel is read twice or skipped.
        const auto y0 = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(y) * src.height) / height);
        auto y1 = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(y + 1) * src.height) / height);
        if (y1 <= y0) y1 = y0 + 1;
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto x0 = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(x) * src.width) / width);
            auto x1 = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(x + 1) * src.width) / width);
            if (x1 <= x0) x1 = x0 + 1;
            std::uint32_t total[4] = {0, 0, 0, 0};
            std::uint32_t counted = 0;
            for (std::uint32_t sy = y0; sy < y1; ++sy) {
                for (std::uint32_t sx = x0; sx < x1; ++sx) {
                    const std::uint8_t* s =
                        &src.pixels[(static_cast<std::size_t>(sy) * src.width + sx) * c];
                    for (std::uint8_t k = 0; k < c; ++k) total[k] += s[k];
                    ++counted;
                }
            }
            std::uint8_t* d = &out.pixels[(static_cast<std::size_t>(y) * width + x) * c];
            for (std::uint8_t k = 0; k < c; ++k)
                d[k] = static_cast<std::uint8_t>((total[k] + counted / 2) / counted);
        }
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> encode_jpeg(const Image& image, int quality) {
    if (image.empty() || image.channels < 1 || image.channels > 4 ||
        image.pixels.size() != image.expected_size())
        return std::nullopt;
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    // stb writes an alpha channel's bytes as if they were colour, so a 4-channel
    // image has to lose its alpha here rather than silently corrupt.
    const Image* source = &image;
    Image opaque;
    if (image.channels == 2 || image.channels == 4) {
        const std::uint8_t keep = image.channels == 2 ? 1 : 3;
        opaque.width = image.width;
        opaque.height = image.height;
        opaque.channels = keep;
        opaque.pixels.resize(opaque.expected_size());
        for (std::size_t at = 0; at < image.pixel_count(); ++at)
            for (std::uint8_t k = 0; k < keep; ++k)
                opaque.pixels[at * keep + k] = image.pixels[at * image.channels + k];
        source = &opaque;
    }
    std::vector<std::uint8_t> out;
    const auto sink = [](void* context, void* data, int size) {
        auto* target = static_cast<std::vector<std::uint8_t>*>(context);
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        target->insert(target->end(), bytes, bytes + size);
    };
    if (stbi_write_jpg_to_func(sink, &out, static_cast<int>(source->width),
                               static_cast<int>(source->height), source->channels,
                               source->pixels.data(), quality) == 0)
        return std::nullopt;
    if (out.empty()) return std::nullopt;
    return out;
}

bool write_alpha_from_luminance(Image& rgba, const Image& mask) {
    if (rgba.empty() || mask.empty() || rgba.channels != 4 ||
        rgba.pixels.size() != rgba.expected_size())
        return false;
    const Image resampled = resize_nearest(mask, rgba.width, rgba.height);
    if (resampled.empty()) return false;

    const auto pixels = rgba.pixel_count();
    for (std::size_t at = 0; at < pixels; ++at) {
        const std::uint8_t* sample = &resampled.pixels[at * resampled.channels];
        // A one- or two-channel mask is already the value; a colour one is read
        // as luminance (Rec. 601 weights, fixed point) rather than by taking red.
        const std::uint32_t value =
            resampled.channels >= 3
                ? (sample[0] * 77u + sample[1] * 151u + sample[2] * 28u) >> 8
                : sample[0];
        rgba.pixels[at * 4 + 3] = static_cast<std::uint8_t>(value);
    }
    return true;
}

Image solid_with_alpha(std::array<std::uint8_t, 3> colour, const Image& mask) {
    if (mask.empty()) return {};
    Image out;
    out.width = mask.width;
    out.height = mask.height;
    out.channels = 4;
    out.pixels.resize(out.expected_size());
    const std::array<std::uint8_t, 4> fill{colour[0], colour[1], colour[2], 255};
    for (std::size_t at = 0; at < out.pixel_count(); ++at)
        std::memcpy(&out.pixels[at * 4], fill.data(), fill.size());
    if (!write_alpha_from_luminance(out, mask)) return {};
    return out;
}

Image composite_rgba(std::uint32_t width, std::uint32_t height,
                     const std::vector<Layer>& layers) {
    if (width == 0 || height == 0) return {};

    Image acc;
    acc.width = width;
    acc.height = height;
    acc.channels = 4;
    acc.pixels.assign(acc.expected_size(), 0);  // transparent

    const std::size_t count = acc.pixel_count();
    for (const Layer& layer : layers) {
        Image rgba = to_rgba(layer.image);
        if (rgba.empty()) continue;
        rgba = resize_nearest(rgba, width, height);
        if (rgba.empty()) continue;

        for (std::size_t i = 0; i < count; ++i) {
            const std::uint8_t* s = &rgba.pixels[i * 4];
            // Tint multiplies the source color (255 == identity).
            const int sr = s[0] * layer.tint[0] / 255;
            const int sg = s[1] * layer.tint[1] / 255;
            const int sb = s[2] * layer.tint[2] / 255;
            const int sa = s[3];
            if (sa == 0) continue;

            std::uint8_t* d = &acc.pixels[i * 4];
            const int da = d[3];
            // Straight-alpha source-over: out_a = sa + da*(1-sa).
            const int out_a = sa + da * (255 - sa) / 255;
            if (out_a == 0) {
                d[0] = d[1] = d[2] = d[3] = 0;
                continue;
            }
            const int inv = 255 - sa;
            d[0] = static_cast<std::uint8_t>((sr * sa + d[0] * da * inv / 255) / out_a);
            d[1] = static_cast<std::uint8_t>((sg * sa + d[1] * da * inv / 255) / out_a);
            d[2] = static_cast<std::uint8_t>((sb * sa + d[2] * da * inv / 255) / out_a);
            d[3] = static_cast<std::uint8_t>(out_a);
        }
    }
    return acc;
}


std::optional<std::vector<std::uint8_t>> encode_png(const Image& image) {
    if (image.empty() || image.channels < 1 || image.channels > 4) return std::nullopt;
    if (image.pixels.size() != image.expected_size()) return std::nullopt;
    std::vector<std::uint8_t> out;
    const auto sink = [](void* context, void* data, int size) {
        auto* target = static_cast<std::vector<std::uint8_t>*>(context);
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        target->insert(target->end(), bytes, bytes + size);
    };
    const auto stride = static_cast<int>(image.width) * image.channels;
    if (stbi_write_png_to_func(sink, &out, static_cast<int>(image.width),
                               static_cast<int>(image.height), image.channels,
                               image.pixels.data(), stride) == 0)
        return std::nullopt;
    if (out.empty()) return std::nullopt;
    return out;
}

std::optional<Image> decode_png_or_jpeg(const std::vector<std::uint8_t>& data) {
    if (data.empty() || data.size() > (std::size_t{1} << 31)) return std::nullopt;
    int width = 0;
    int height = 0;
    int channels = 0;
    // Ask for whatever the file carries rather than forcing RGBA: a greyscale
    // mask stays one channel, so the J2C the viewer receives is the size the
    // content warrants. The bake path's to_rgba() widens where it must.
    auto* pixels = stbi_load_from_memory(data.data(), static_cast<int>(data.size()),
                                         &width, &height, &channels, 0);
    if (pixels == nullptr) return std::nullopt;
    if (width <= 0 || height <= 0 || channels < 1 || channels > 4) {
        stbi_image_free(pixels);
        return std::nullopt;
    }
    Image image;
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    image.channels = static_cast<std::uint8_t>(channels);
    const auto count = image.expected_size();
    image.pixels.assign(pixels, pixels + count);
    stbi_image_free(pixels);
    return image;
}

}  // namespace homeworldz::image
