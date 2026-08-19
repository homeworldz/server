#include "homeworldz/terrain_edit.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <memory>
#include <numbers>
#include <set>

namespace homeworldz::terrain {
namespace {

constexpr float minimum_height = 0.0F;
constexpr float maximum_height = 4096.0F;

float neighbor_average(const Heightmap& source, int x, int y) {
    float total{};
    int count{};
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const auto sample_x = std::clamp(x + dx, 0, static_cast<int>(source.width() - 1));
            const auto sample_y = std::clamp(y + dy, 0, static_cast<int>(source.height() - 1));
            total += source[static_cast<std::size_t>(sample_y) * source.width() + sample_x];
            ++count;
        }
    return total / static_cast<float>(count);
}

float deterministic_noise(int x, int y) {
    auto value = static_cast<std::uint32_t>(x) * 0x9e3779b9U ^
                 static_cast<std::uint32_t>(y) * 0x85ebca6bU;
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    return static_cast<float>(value & 0xffffU) / 32767.5F - 1.0F;
}

} // namespace

std::unique_ptr<Heightmap> load_state(const std::filesystem::path& path,
                                      std::size_t expected_width,
                                      std::size_t expected_height) {
    if (expected_height == 0) expected_height = expected_width;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto expected_bytes = expected_width * expected_height * sizeof(float);
    if (!input || input.tellg() != static_cast<std::streamoff>(expected_bytes)) return {};
    input.seekg(0);
    auto result = std::make_unique<Heightmap>(expected_width, expected_height);
    input.read(reinterpret_cast<char*>(result->data()), static_cast<std::streamsize>(expected_bytes));
    if (!input || std::any_of(result->begin(), result->end(), [](float height) {
            return !std::isfinite(height) || height < minimum_height || height > maximum_height;
        }))
        return {};
    return result;
}

bool save_state(const std::filesystem::path& path, const Heightmap& heightmap) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output.write(reinterpret_cast<const char*>(heightmap.data()),
                     static_cast<std::streamsize>(heightmap.size() * sizeof(float)));
        if (!output) return false;
    }
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
    return !error;
}

std::vector<viewer::TerrainPatch> apply(Heightmap& heightmap, const Heightmap& revert,
                                        const viewer::ModifyLand& edit,
                                        float smooth_strength, float raise_limit,
                                        float lower_limit) {
    if (edit.action > 5 || edit.areas.empty()) return {};
    if (heightmap.width() != revert.width() || heightmap.height() != revert.height()) return {};
    // Normalised so a reversed or same-signed pair cannot invert the window and
    // reject every edit. The viewer sends raise positive and lower negative; an
    // operator typing them the other way round should get a usable region, not a
    // frozen one.
    const auto upper_offset = std::max(raise_limit, lower_limit);
    const auto lower_offset = std::min(raise_limit, lower_limit);
    // Smooth reads the pre-edit neighbourhood, so it needs a snapshot; nothing
    // else does. This copy was unconditional, which meant every raise, lower,
    // flatten, noise and revert packet also allocated and copied the whole
    // heightmap - 4 MB on a 1024 region, per ModifyLand, at brush rates
    // (operator report of edits arriving twenty seconds late, 2026-07-30).
    std::unique_ptr<Heightmap> original;
    if (edit.action == 3) original = std::make_unique<Heightmap>(heightmap);
    std::set<std::pair<std::uint8_t, std::uint8_t>> patches;
    for (std::size_t area_index = 0; area_index < edit.areas.size(); ++area_index) {
        const auto& area = edit.areas[area_index];
        const auto center_x = (area.west + area.east) * 0.5F;
        const auto center_y = (area.south + area.north) * 0.5F;
        float radius = static_cast<float>(std::max<std::uint8_t>(1, edit.brush_size));
        if (area_index < edit.extended_brush_sizes.size() && edit.extended_brush_sizes[area_index] > 0.0F)
            radius = edit.extended_brush_sizes[area_index];
        radius = std::clamp(radius, 0.5F, 64.0F);
        const auto maximum_x = static_cast<int>(heightmap.width() - 1);
        const auto maximum_y = static_cast<int>(heightmap.height() - 1);
        const auto x_from = std::clamp(static_cast<int>(std::floor(center_x - radius)), 0, maximum_x);
        const auto x_to = std::clamp(static_cast<int>(std::ceil(center_x + radius)), 0, maximum_x);
        const auto y_from = std::clamp(static_cast<int>(std::floor(center_y - radius)), 0, maximum_y);
        const auto y_to = std::clamp(static_cast<int>(std::ceil(center_y + radius)), 0, maximum_y);
        const auto duration = std::clamp(edit.seconds, 0.01F, 4.0F);
        for (int y = y_from; y <= y_to; ++y)
            for (int x = x_from; x <= x_to; ++x) {
                const auto distance = std::hypot(static_cast<float>(x) - center_x,
                                                 static_cast<float>(y) - center_y);
                if (distance > radius) continue;
                const auto weight = std::max(0.0F, std::cos(distance * std::numbers::pi_v<float> /
                                                            (radius * 2.0F)));
                const auto index = static_cast<std::size_t>(y) * heightmap.width() + x;
                float next = heightmap[index];
                switch (edit.action) {
                case 0: next += (edit.height - next) * std::min(1.0F, weight * duration * 0.25F); break;
                case 1: next += weight * duration; break;
                case 2: next -= weight * duration; break;
                // Smooth converges on the local average, so its rate only
                // decides how many applications that takes. At 0.03 it took
                // several seconds of held mouse button to level one peak while
                // flatten (0.25) felt immediate - an eight-fold difference no
                // one had chosen, only inherited. Raised to a rate in the same
                // family as the other brushes; still a lerp toward a bounded
                // target, so it cannot overshoot.
                case 3: next += (neighbor_average(*original, x, y) - next) *
                                     std::min(1.0F, weight * duration * smooth_strength); break;
                case 4: next += deterministic_noise(x, y) * weight * duration * 0.25F; break;
                case 5: next += (revert[index] - next) * std::min(1.0F, weight * duration * 0.25F); break;
                default: break;
                }
                next = std::clamp(next, minimum_height, maximum_height);
                // The edit limits, measured from the region's original height so
                // that repeated edits cannot walk past them a little at a time.
                // Revert is exempt: it moves the ground back toward the baseline,
                // so it can only ever reduce the distance being bounded, and
                // clamping it would strand terrain that predates a tightened
                // limit outside a window it can no longer leave.
                if (edit.action != 5)
                    next = std::clamp(next, revert[index] + lower_offset,
                                      revert[index] + upper_offset);
                if (std::abs(next - heightmap[index]) < 0.0001F) continue;
                heightmap[index] = next;
                patches.emplace(static_cast<std::uint8_t>(x / 16), static_cast<std::uint8_t>(y / 16));
            }
    }
    std::vector<viewer::TerrainPatch> result;
    result.reserve(patches.size());
    for (const auto [x, y] : patches) result.push_back({x, y});
    return result;
}

} // namespace homeworldz::terrain
