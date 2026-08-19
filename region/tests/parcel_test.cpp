#include "homeworldz/parcel.h"

#include <cstdio>

using homeworldz::parcel::Parcel;
using homeworldz::parcel::ParcelSet;

namespace {

int failures = 0;

void check(bool condition, const char* label) {
    if (!condition) {
        std::printf("FAIL: %s\n", label);
        ++failures;
    }
}

} // namespace

int main() {
    // Default region-wide parcel for a 256 m region.
    {
        ParcelSet set(256, "11111111-1111-4111-8111-111111111111",
                      "22222222-2222-4222-8222-222222222222", 1000);
        check(set.edge_cells() == 64, "256m region has 64 cells per edge");
        check(set.bitmap_bytes() == 512, "256m region bitmap is 512 bytes");
        check(set.parcels().size() == 1, "fresh region has one parcel");
        const Parcel& parcel = set.parcels().front();
        check(parcel.local_id == 1, "default parcel local id is 1");
        check(parcel.owner_id == "22222222-2222-4222-8222-222222222222", "default owner set");
        check(parcel.area(set.edge_cells()) == 65536, "whole 256m parcel is 65536 m^2");
        check(set.parcel_at(128.0F, 128.0F) == &parcel, "point in region resolves to parcel");
        check(set.parcel_at(300.0F, 10.0F) == nullptr, "point outside region resolves to null");
        check(set.parcel_covering(0.0F, 0.0F, 256.0F, 256.0F) == &parcel, "whole-region rect covered");
    }

    // Variable region size: 512 m -> 128 cells, 2048-byte bitmap.
    {
        ParcelSet set(512, "aaaa1111-1111-4111-8111-111111111111",
                      "bbbb2222-2222-4222-8222-222222222222", 0);
        check(set.edge_cells() == 128, "512m region has 128 cells per edge");
        check(set.bitmap_bytes() == 2048, "512m region bitmap is 2048 bytes");
        check(set.parcels().front().area(set.edge_cells()) == 262144, "whole 512m parcel is 262144 m^2");
    }

    // Subdivide the SW 64x64 m corner out of the default parcel.
    {
        ParcelSet set(256, "11111111-1111-4111-8111-111111111111",
                      "22222222-2222-4222-8222-222222222222", 1000);
        const auto carved = set.divide(0.0F, 0.0F, 64.0F, 64.0F,
                                       "33333333-3333-4333-8333-333333333333",
                                       "44444444-4444-4444-8444-444444444444", 2000);
        check(carved.has_value(), "divide returns a new local id");
        check(set.parcels().size() == 2, "region now has two parcels");
        if (carved) {
            const Parcel* small = set.find_by_local_id(*carved);
            check(small != nullptr, "carved parcel is findable");
            check(small && small->area(set.edge_cells()) == 64 * 64, "carved parcel is 4096 m^2");
            check(small && small->owner_id == "44444444-4444-4444-8444-444444444444",
                  "carved parcel keeps requested owner");
            check(set.parcel_at(10.0F, 10.0F) == small, "SW point resolves to carved parcel");
        }
        const Parcel* original = set.find_by_local_id(1);
        check(original && original->area(set.edge_cells()) == 65536 - 4096,
              "original parcel shrank by the carved area");
        check(set.parcel_at(200.0F, 200.0F) == original, "NE point still resolves to original");

        // Dividing along a boundary that spans both parcels must fail.
        const auto spanning = set.divide(0.0F, 0.0F, 128.0F, 128.0F,
                                         "55555555-5555-4555-8555-555555555555",
                                         "44444444-4444-4444-8444-444444444444", 3000);
        check(!spanning.has_value(), "divide spanning two parcels is rejected");
    }

    // Join two same-owner parcels back into one.
    {
        ParcelSet set(256, "11111111-1111-4111-8111-111111111111",
                      "22222222-2222-4222-8222-222222222222", 1000);
        // Carve, but keep the same owner so join is permitted.
        const auto carved = set.divide(0.0F, 0.0F, 64.0F, 64.0F,
                                       "33333333-3333-4333-8333-333333333333",
                                       "22222222-2222-4222-8222-222222222222", 2000);
        check(carved.has_value() && set.parcels().size() == 2, "prepared two same-owner parcels");
        const auto merged = set.join(0.0F, 0.0F, 256.0F, 256.0F,
                                     "22222222-2222-4222-8222-222222222222");
        check(merged.has_value(), "join returns surviving local id");
        check(set.parcels().size() == 1, "join collapses back to one parcel");
        check(merged && set.find_by_local_id(*merged) &&
                  set.find_by_local_id(*merged)->area(set.edge_cells()) == 65536,
              "joined parcel spans the whole region again");
    }

    // Join must refuse parcels with different owners.
    {
        ParcelSet set(256, "11111111-1111-4111-8111-111111111111",
                      "22222222-2222-4222-8222-222222222222", 1000);
        set.divide(0.0F, 0.0F, 64.0F, 64.0F, "33333333-3333-4333-8333-333333333333",
                   "44444444-4444-4444-8444-444444444444", 2000);
        const auto merged = set.join(0.0F, 0.0F, 256.0F, 256.0F,
                                     "22222222-2222-4222-8222-222222222222");
        check(!merged.has_value(), "join across different owners is rejected");
        check(set.parcels().size() == 2, "parcels unchanged after rejected join");
    }

    // Enforcement predicates.
    {
        using namespace homeworldz::parcel;
        const std::string region_owner = "99999999-9999-4999-8999-999999999999";
        const std::string owner = "22222222-2222-4222-8222-222222222222";
        const std::string stranger = "55555555-5555-4555-8555-555555555555";
        Parcel parcel;
        parcel.owner_id = owner;
        parcel.flags = default_parcel_flags; // CreateObjects + AllowOtherScripts set

        check(can_build(parcel, owner, region_owner), "owner can build");
        check(can_build(parcel, region_owner, region_owner), "region owner can build");
        check(can_build(parcel, stranger, region_owner),
              "stranger can build when CreateObjects set");
        parcel.flags &= ~flag_create_objects;
        check(!can_build(parcel, stranger, region_owner),
              "stranger cannot build when CreateObjects clear");
        check(can_build(parcel, owner, region_owner), "owner still builds without CreateObjects");

        check(can_run_scripts(parcel, stranger, region_owner),
              "other scripts run when AllowOtherScripts set");
        parcel.flags &= ~flag_allow_other_scripts;
        check(!can_run_scripts(parcel, stranger, region_owner),
              "other scripts blocked when AllowOtherScripts clear");
        check(can_run_scripts(parcel, owner, region_owner), "owner scripts always run");

        check(can_enter(parcel, stranger, region_owner), "open parcel admits strangers");
        parcel.flags |= flag_use_ban_list;
        parcel.access.push_back({stranger, 0, access_ban});
        check(!can_enter(parcel, stranger, region_owner), "banned agent cannot enter");
        check(can_enter(parcel, owner, region_owner), "owner enters despite ban list");
        check(can_enter(parcel, region_owner, region_owner), "region owner enters despite ban list");
        parcel.flags |= flag_use_access_list;
        const std::string guest = "66666666-6666-4666-8666-666666666666";
        check(!can_enter(parcel, guest, region_owner),
              "access list excludes non-listed agent");
        parcel.access.push_back({guest, 0, access_allowed});
        check(can_enter(parcel, guest, region_owner), "listed agent enters");
    }

    // ParcelOverlay per-cell colouring and borders.
    {
        using namespace homeworldz::parcel;
        const std::string owner = "22222222-2222-4222-8222-222222222222";
        const std::string other = "77777777-7777-4777-8777-777777777777";
        ParcelSet set(256, "11111111-1111-4111-8111-111111111111", owner, 0);
        set.divide(0.0F, 0.0F, 64.0F, 64.0F, "33333333-3333-4333-8333-333333333333", other, 0);
        const int edge = set.edge_cells();
        const auto self_view = set.overlay_for(owner, "");
        check(static_cast<int>(self_view.size()) == edge * edge, "overlay covers every cell");
        // Cell (0,0): SW corner of the carved parcel, owned by `other`, on both region edges.
        const auto sw = self_view[0];
        check((sw & 0x07) == overlay_owned_by_other, "SW cell owned by other from owner's view");
        check((sw & overlay_border_west) != 0, "region west edge marks a border");
        check((sw & overlay_border_south) != 0, "region south edge marks a border");
        // Cell just north of the carved parcel's top edge belongs to the owner's parcel
        // and must carry a south border where it meets the carved parcel (y = 16 cells).
        const auto boundary = self_view[static_cast<std::size_t>(16) * edge + 0];
        check((boundary & 0x07) == overlay_owned_by_self, "cell above carve is owner's from owner view");
        check((boundary & overlay_border_south) != 0, "internal parcel edge marks a south border");
        // From the other resident's view, their carved parcel colours as self.
        const auto other_view = set.overlay_for(other, "");
        check((other_view[0] & 0x07) == overlay_owned_by_self, "carved parcel is self from other's view");
    }

    // Rectangular region (ADR 0036): 1024 x 512 m -> 256 x 128 cells.
    {
        using namespace homeworldz::parcel;
        const std::string owner = "22222222-2222-4222-8222-222222222222";
        ParcelSet set(1024, 512, "11111111-1111-4111-8111-111111111111", owner, 1000);
        check(set.region_size_x_metres() == 1024, "rect region x size is 1024 m");
        check(set.region_size_y_metres() == 512, "rect region y size is 512 m");
        check(set.region_size_metres() == 1024, "compat region size is the x size");
        check(set.cells_x() == 256, "rect region has 256 cells in x");
        check(set.cells_y() == 128, "rect region has 128 cells in y");
        check(set.edge_cells() == 256, "compat edge_cells is cells_x");
        check(set.bitmap_bytes() == (256 * 128 + 7) / 8, "rect bitmap is cells_x*cells_y bits");
        check(set.parcels().size() == 1, "fresh rect region has one parcel");
        const Parcel& whole = set.parcels().front();
        check(whole.area(set.cells_x(), set.cells_y()) == 1024 * 512,
              "default rect parcel covers the whole region");
        check(set.parcel_at(1000.0F, 500.0F) == &whole, "far NE point resolves to parcel");
        check(set.parcel_at(1000.0F, 520.0F) == nullptr, "point beyond y extent is outside");
        check(set.parcel_at(1030.0F, 100.0F) == nullptr, "point beyond x extent is outside");
        check(set.parcel_at_cell(255, 127) == &whole, "last cell resolves to parcel");
        check(set.parcel_at_cell(255, 128) == nullptr, "cell beyond cells_y is null");
        check(set.parcel_at_cell(128, 127) == &whole, "tall column cell resolves (no square clamp)");

        // Divide a rectangle entirely beyond x = 512 m (the second facet's territory).
        const auto carved = set.divide(640.0F, 128.0F, 768.0F, 256.0F,
                                       "33333333-3333-4333-8333-333333333333",
                                       "44444444-4444-4444-8444-444444444444", 2000);
        check(carved.has_value(), "divide beyond x=512 succeeds");
        const Parcel* east_parcel = carved ? set.find_by_local_id(*carved) : nullptr;
        check(east_parcel != nullptr, "east parcel is findable");
        check(east_parcel && east_parcel->area(set.cells_x(), set.cells_y()) == 128 * 128,
              "east parcel is 128x128 m");
        check(set.parcel_at(700.0F, 200.0F) == east_parcel, "point in east carve resolves");
        check(set.parcel_at_cell(160, 32) == east_parcel, "cell in east carve resolves");
        check(set.parcel_at_cell(159, 32) != east_parcel, "cell west of carve is not the carve");
        int min_x = 0, min_y = 0, max_x = 0, max_y = 0;
        check(east_parcel &&
                  east_parcel->cell_bounds(set.cells_x(), set.cells_y(), min_x, min_y, max_x,
                                           max_y),
              "east parcel has cell bounds");
        check(min_x == 160 && min_y == 32 && max_x == 192 && max_y == 64,
              "east parcel bounds are (160,32)-(192,64)");

        // The full overlay covers the rectangular grid; a facet window matches its slice.
        const auto full = set.overlay_for(owner, "");
        check(static_cast<int>(full.size()) == set.cells_x() * set.cells_y(),
              "rect overlay covers every cell");
        const int window = 128; // one 512 m facet, in cells
        const auto second = set.overlay_window_for(owner, "", window, 0, window);
        check(static_cast<int>(second.size()) == window * window,
              "facet window overlay is window_cells^2");
        bool slice_matches = true;
        for (int y = 0; y < window && slice_matches; ++y)
            for (int x = 0; x < window; ++x)
                if (second[static_cast<std::size_t>(y) * window + x] !=
                    full[static_cast<std::size_t>(y) * set.cells_x() + (window + x)]) {
                    slice_matches = false;
                    break;
                }
        check(slice_matches, "facet window equals the corresponding overlay_for slice");
    }

    // Facet-edge borders (ADR 0036): a parcel line lying on the internal facet
    // seam shows a border; the seam itself is not a region edge.
    {
        using namespace homeworldz::parcel;
        const std::string owner = "22222222-2222-4222-8222-222222222222";
        ParcelSet set(1024, 512, "11111111-1111-4111-8111-111111111111", owner, 1000);
        // Carve everything east of x = 512 m into a second parcel: the parcel
        // line lies exactly on the facet seam.
        const auto carved = set.divide(512.0F, 0.0F, 1024.0F, 512.0F,
                                       "33333333-3333-4333-8333-333333333333",
                                       "44444444-4444-4444-8444-444444444444", 2000);
        check(carved.has_value(), "carving the east half succeeds");

        const int window = 128;
        const auto second = set.overlay_window_for(owner, "", window, 0, window);
        // Window cell x=0 is region cell x=128, immediately east of the parcel line.
        check((second[0] & overlay_border_west) != 0,
              "parcel line on the seam shows a west border in the second window");
        check((second[0] & overlay_border_south) != 0,
              "region south edge borders the second window's first row");
        // A row off the south edge: still a west border (parcel line), no south border.
        check((second[static_cast<std::size_t>(1) * window + 0] & overlay_border_west) != 0,
              "west border continues up the parcel line");
        check((second[static_cast<std::size_t>(1) * window + 0] & overlay_border_south) == 0,
              "no south border inside the region interior");
        // With no parcel line on the seam, the internal edge shows no border.
        ParcelSet undivided(1024, 512, "11111111-1111-4111-8111-111111111111", owner, 1000);
        const auto seam = undivided.overlay_window_for(owner, "", window, 1, window);
        check((seam[0] & overlay_border_west) == 0,
              "internal facet edge without a parcel line shows no west border");
        check((seam[0] & overlay_border_south) == 0,
              "region-edge borders do not appear on an interior window row");
        // The true region west edge still borders the first window.
        const auto first = undivided.overlay_window_for(owner, "", 0, 0, window);
        check((first[0] & overlay_border_west) != 0, "region west edge borders the first window");
        check((first[0] & overlay_border_south) != 0, "region south edge borders the first window");
        // Square region: overlay_for and the whole-region window agree.
        ParcelSet square(256, "11111111-1111-4111-8111-111111111111", owner, 0);
        check(square.overlay_for(owner, "") ==
                  square.overlay_window_for(owner, "", 0, 0, square.cells_x()),
              "square overlay_for equals its whole-region window");
    }

    if (failures != 0) {
        std::printf("%d parcel test check(s) failed\n", failures);
        return 1;
    }
    std::printf("all parcel tests passed\n");
    return 0;
}
