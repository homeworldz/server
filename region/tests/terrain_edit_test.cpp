#include "homeworldz/terrain_edit.h"

#include <cmath>
#include <filesystem>
#include <memory>
#include <stdexcept>

int main() {
    auto terrain = std::make_unique<homeworldz::terrain::Heightmap>();
    terrain->fill(20.0F);
    auto revert = std::make_unique<homeworldz::terrain::Heightmap>(*terrain);
    homeworldz::viewer::ModifyLand raise;
    raise.action = 1;
    raise.seconds = 1.0F;
    raise.brush_size = 1;
    raise.areas.push_back({-1, 128.0F, 128.0F, 128.0F, 128.0F});
    raise.extended_brush_sizes.push_back(4.0F);
    const auto changed = homeworldz::terrain::apply(*terrain, *revert, raise);
    if (changed.empty() || (*terrain)[128 * 256 + 128] <= 20.9F ||
        (*terrain)[100 * 256 + 100] != 20.0F)
        return 1;

    auto level = raise;
    level.action = 0;
    level.height = 22.0F;
    level.seconds = 4.0F;
    if (homeworldz::terrain::apply(*terrain, *revert, level).empty() ||
        std::abs((*terrain)[128 * 256 + 128] - 22.0F) > 0.001F)
        return 2;

    auto restore = raise;
    restore.action = 5;
    restore.seconds = 4.0F;
    if (homeworldz::terrain::apply(*terrain, *revert, restore).empty() ||
        std::abs((*terrain)[128 * 256 + 128] - 20.0F) > 0.001F)
        return 3;

    const auto path = std::filesystem::temp_directory_path() / "homeworldz-terrain-edit-test.f32";
    if (!homeworldz::terrain::save_state(path, *terrain)) return 4;
    const auto loaded = homeworldz::terrain::load_state(path);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    if (!loaded || *loaded != *terrain) return 5;

    homeworldz::terrain::Heightmap large(512);
    large.fill(20.0F);
    const auto large_revert = large;
    auto large_raise = raise;
    large_raise.areas.clear();
    large_raise.areas.push_back({-1, 400.0F, 300.0F, 400.0F, 300.0F});
    if (homeworldz::terrain::apply(large, large_revert, large_raise).empty() ||
        large[300 * 512 + 400] <= 20.9F || large[128 * 512 + 128] != 20.0F)
        return 6;

    const auto large_path = std::filesystem::temp_directory_path() /
        "homeworldz-terrain-edit-large-test.f32";
    if (!homeworldz::terrain::save_state(large_path, large)) return 7;
    const auto loaded_large = homeworldz::terrain::load_state(large_path, 512);
    const auto wrong_size = homeworldz::terrain::load_state(large_path, 256);
    std::filesystem::remove(large_path, ignored);
    if (!loaded_large || *loaded_large != large || wrong_size) return 8;

    homeworldz::terrain::Heightmap maximum(1024);
    if (maximum.width() != 1024 || maximum.height() != 1024 || maximum.size() != 1024 * 1024)
        return 9;
    if (!homeworldz::terrain::apply(maximum, large_revert, large_raise).empty()) return 10;
    try {
        homeworldz::terrain::Heightmap unsupported(300);
        return 11;
    } catch (const std::invalid_argument&) {
    }
    try {
        homeworldz::terrain::Heightmap oversized(4352);
        return 11;
    } catch (const std::invalid_argument&) {
    }
    try {
        homeworldz::terrain::Heightmap bad_height(512, 100);
        return 11;
    } catch (const std::invalid_argument&) {
    }

    // Rectangular heightmaps (ADR 0036): a macro region is width x height with
    // each dimension its own bound; row-major indexing keeps the width stride.
    {
        homeworldz::terrain::Heightmap rect(512, 256);
        if (rect.width() != 512 || rect.height() != 256 || rect.size() != 512 * 256) return 19;
        rect.fill(20.0F);
        const auto rect_revert = rect;

        // A square revert of the same width is a different shape, not a match.
        homeworldz::terrain::Heightmap square_revert(512);
        square_revert.fill(20.0F);
        homeworldz::viewer::ModifyLand corner_raise;
        corner_raise.action = 1;
        corner_raise.seconds = 1.0F;
        corner_raise.brush_size = 1;
        corner_raise.areas.push_back({-1, 500.0F, 240.0F, 500.0F, 240.0F});
        corner_raise.extended_brush_sizes.push_back(4.0F);
        if (!homeworldz::terrain::apply(rect, square_revert, corner_raise).empty()) return 20;

        // Editing near the far corner: x may run to 511 while y stops at 255,
        // each axis bounded independently. The width()-as-y-bound bug would
        // either index off the end or leave the far column untouched.
        if (homeworldz::terrain::apply(rect, rect_revert, corner_raise).empty()) return 21;
        if (rect[240 * 512 + 500] <= 20.9F) return 22;
        if (rect[100 * 512 + 100] != 20.0F) return 23;

        // A brush centred past the top edge clamps to the last row rather than
        // walking into memory that a square map would have had.
        auto edge_raise = corner_raise;
        edge_raise.areas.clear();
        edge_raise.areas.push_back({-1, 255.0F, 255.0F, 255.0F, 255.0F});
        if (homeworldz::terrain::apply(rect, rect_revert, edge_raise).empty()) return 24;
        if (rect[255 * 512 + 255] <= 20.0F) return 25;

        // Save and load with width != height; a square load of the same width
        // expects a different byte count and is refused.
        const auto rect_path = std::filesystem::temp_directory_path() /
            "homeworldz-terrain-edit-rect-test.f32";
        if (!homeworldz::terrain::save_state(rect_path, rect)) return 26;
        const auto loaded_rect = homeworldz::terrain::load_state(rect_path, 512, 256);
        const auto square_load = homeworldz::terrain::load_state(rect_path, 512);
        std::filesystem::remove(rect_path, ignored);
        if (!loaded_rect || *loaded_rect != rect || square_load) return 27;
    }

    // The Region/Estate terrain raise/lower limits. Before setregionterrain was
    // handled the region announced 100 and -100 in RegionInfo and enforced
    // neither, so the form's fields were decoration - a control that does
    // nothing, which is the defect this project keeps meeting.
    {
        homeworldz::terrain::Heightmap ground(256);
        homeworldz::terrain::Heightmap baseline(256);
        ground.fill(25.0F);
        baseline.fill(25.0F);
        homeworldz::viewer::ModifyLand raise_edit;
        raise_edit.action = 1;                 // raise
        raise_edit.brush_size = 2;
        raise_edit.seconds = 4.0F;
        raise_edit.areas.push_back({0, 120.0F, 120.0F, 130.0F, 130.0F});
        const auto centre = 125u * 256u + 125u;

        // Held down long past the limit: the ground stops at revert + 3, not at
        // whatever the brush would have reached. Measured from `baseline` rather
        // than from the current height on purpose - a bound against the current
        // height is a rate, and repeated edits walk straight past it.
        for (int pass = 0; pass < 40; ++pass)
            homeworldz::terrain::apply(ground, baseline, raise_edit, 0.5F, 3.0F, -3.0F);
        if (ground[centre] > 28.0F + 1e-4F) return 12;
        if (ground[centre] < 28.0F - 1e-4F) return 13;   // and it does reach it

        homeworldz::viewer::ModifyLand lower_edit = raise_edit;
        lower_edit.action = 2;                 // lower
        for (int pass = 0; pass < 80; ++pass)
            homeworldz::terrain::apply(ground, baseline, lower_edit, 0.5F, 3.0F, -3.0F);
        if (ground[centre] < 22.0F - 1e-4F) return 14;
        if (ground[centre] > 22.0F + 1e-4F) return 15;

        // Revert is exempt: it moves the ground back toward the baseline, so it
        // can only reduce the distance being bounded. Clamping it would strand
        // terrain that predates a tightened limit outside a window it can never
        // leave. Raise with a wide limit, then revert under a narrow one.
        homeworldz::terrain::Heightmap tall(256);
        tall.fill(25.0F);
        for (int pass = 0; pass < 40; ++pass)
            homeworldz::terrain::apply(tall, baseline, raise_edit, 0.5F, 50.0F, -50.0F);
        if (tall[centre] <= 30.0F) return 16;  // genuinely far from baseline
        homeworldz::viewer::ModifyLand revert_edit = raise_edit;
        revert_edit.action = 5;
        for (int pass = 0; pass < 200; ++pass)
            homeworldz::terrain::apply(tall, baseline, revert_edit, 0.5F, 1.0F, -1.0F);
        if (std::abs(tall[centre] - 25.0F) > 0.05F) return 17;

        // A reversed pair must not invert the window and freeze the region.
        homeworldz::terrain::Heightmap swapped(256);
        swapped.fill(25.0F);
        for (int pass = 0; pass < 40; ++pass)
            homeworldz::terrain::apply(swapped, baseline, raise_edit, 0.5F, -3.0F, 3.0F);
        if (swapped[centre] <= 25.0F) return 18;
    }
    return 0;
}
