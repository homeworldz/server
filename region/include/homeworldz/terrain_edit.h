#pragma once

#include "homeworldz/viewer_protocol.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace homeworldz::terrain {

class Heightmap {
public:
    // Height 0 means square (width). Each dimension must be a multiple of 256,
    // at least 256 and at most 4096. A macro region of ADR 0036 can be
    // rectangular (1024x512, 512x2560); the facet shape rule is enforced at
    // registration, not here.
    explicit Heightmap(std::size_t width = 256, std::size_t height = 0)
        : width_(width), height_(height == 0 ? width : height),
          samples_(width_ * height_) {
        const auto valid = [](std::size_t dimension) {
            return dimension >= 256 && dimension <= 4096 && dimension % 256 == 0;
        };
        if (!valid(width_) || !valid(height_))
            throw std::invalid_argument(
                "terrain dimensions must be multiples of 256 between 256 and 4096");
    }

    std::size_t width() const noexcept { return width_; }
    std::size_t height() const noexcept { return height_; }
    std::size_t size() const noexcept { return samples_.size(); }
    float* data() noexcept { return samples_.data(); }
    const float* data() const noexcept { return samples_.data(); }
    auto begin() noexcept { return samples_.begin(); }
    auto end() noexcept { return samples_.end(); }
    auto begin() const noexcept { return samples_.begin(); }
    auto end() const noexcept { return samples_.end(); }
    float& operator[](std::size_t index) noexcept { return samples_[index]; }
    const float& operator[](std::size_t index) const noexcept { return samples_[index]; }
    void fill(float value) { std::fill(samples_.begin(), samples_.end(), value); }
    operator std::span<const float>() const noexcept {
        return {samples_.data(), samples_.size()};
    }
    bool operator==(const Heightmap&) const = default;

private:
    std::size_t width_;
    std::size_t height_;
    std::vector<float> samples_;
};

// Expected height 0 means square (expected_width). The file is raw f32
// samples; its size must equal width * height * 4.
std::unique_ptr<Heightmap> load_state(const std::filesystem::path& path,
                                      std::size_t expected_width = 256,
                                      std::size_t expected_height = 0);
bool save_state(const std::filesystem::path& path, const Heightmap& heightmap);
// How fast the smooth brush converges on the local average per application.
// It is a feel constant, not a correctness one - smoothing lerps toward a
// bounded target, so the rate only decides how many applications level a
// feature. Inherited at 0.03 against flatten's 0.25, which meant seconds of
// held mouse button per peak; raised on the operator's judgement in three
// steps (0.20, 0.26, 0.36, 0.50) and made overridable per region
// (region.smooth_strength_percent) so tuning it no longer needs a deployment.
// The operator's argument for the top of that range is that a strong default
// is recoverable - a viewer's own strength slider turns it down - while a weak
// one leaves nowhere to go.
inline constexpr float default_smooth_strength = 0.50F;

// How far an edit may move the ground from the region's original height, in
// metres, above and below. The bound is measured against `revert` rather than
// against the current height, which is what makes it a limit rather than a rate:
// otherwise repeated edits walk past it a little at a time.
//
// The viewer's Region/Estate -> Terrain tab sets both and sends them in
// `setregionterrain`. Before that was handled the region announced 100 and -100
// in `RegionInfo` and enforced neither, so the fields read as settings and were
// decoration - the shape of advertising a control that does nothing.
inline constexpr float default_terrain_raise_limit = 100.0F;
inline constexpr float default_terrain_lower_limit = -100.0F;

std::vector<viewer::TerrainPatch> apply(Heightmap& heightmap, const Heightmap& revert,
                                        const viewer::ModifyLand& edit,
                                        float smooth_strength = default_smooth_strength,
                                        float raise_limit = default_terrain_raise_limit,
                                        float lower_limit = default_terrain_lower_limit);

} // namespace homeworldz::terrain
