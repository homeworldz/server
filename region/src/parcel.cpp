#include "homeworldz/parcel.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace homeworldz::parcel {
namespace {

int cells_for(int region_size_metres) {
    if (region_size_metres <= 0 || region_size_metres % 4 != 0)
        throw std::invalid_argument("region size must be a positive multiple of 4 metres");
    return region_size_metres / 4;
}

// Clamp a world-metre coordinate to a cell index within [0, cells).
int metre_to_cell(float metre, int cells) {
    int cell = static_cast<int>(std::floor(metre / 4.0F));
    if (cell < 0) cell = 0;
    if (cell >= cells) cell = cells - 1;
    return cell;
}

} // namespace

std::int32_t Parcel::area(int cells_x, int cells_y) const {
    std::int32_t set = 0;
    for (int y = 0; y < cells_y; ++y)
        for (int x = 0; x < cells_x; ++x)
            if (contains_cell(cells_x, cells_y, x, y)) ++set;
    return set * 16;
}

bool Parcel::contains_cell(int cells_x, int cells_y, int cell_x, int cell_y) const {
    return ParcelSet::bit_get(bitmap, cells_x, cells_y, cell_x, cell_y);
}

bool Parcel::cell_bounds(int cells_x, int cells_y, int& min_x, int& min_y, int& max_x,
                         int& max_y) const {
    bool any = false;
    min_x = cells_x;
    min_y = cells_y;
    max_x = max_y = -1;
    for (int y = 0; y < cells_y; ++y)
        for (int x = 0; x < cells_x; ++x)
            if (contains_cell(cells_x, cells_y, x, y)) {
                any = true;
                min_x = std::min(min_x, x);
                min_y = std::min(min_y, y);
                max_x = std::max(max_x, x + 1);
                max_y = std::max(max_y, y + 1);
            }
    return any;
}

namespace {

bool is_region_owner(std::string_view agent, std::string_view region_owner) {
    return !region_owner.empty() && agent == region_owner;
}

bool has_access(const Parcel& parcel, std::string_view agent, std::uint32_t flag) {
    for (const auto& entry : parcel.access)
        if (entry.agent_id == agent && (entry.flags & flag) != 0) return true;
    return false;
}

} // namespace

bool can_build(const Parcel& parcel, std::string_view agent, std::string_view region_owner) {
    if (is_region_owner(agent, region_owner)) return true;
    if (!parcel.owner_id.empty() && agent == parcel.owner_id) return true;
    return (parcel.flags & flag_create_objects) != 0;
}

bool can_enter(const Parcel& parcel, std::string_view agent, std::string_view region_owner) {
    if (is_region_owner(agent, region_owner)) return true;
    if (!parcel.owner_id.empty() && agent == parcel.owner_id) return true;
    if ((parcel.flags & flag_use_ban_list) != 0 && has_access(parcel, agent, access_ban))
        return false;
    if ((parcel.flags & flag_use_access_list) != 0 && !has_access(parcel, agent, access_allowed))
        return false;
    return true;
}

bool can_run_scripts(const Parcel& parcel, std::string_view owner, std::string_view region_owner) {
    if ((parcel.flags & flag_allow_other_scripts) != 0) return true;
    if (is_region_owner(owner, region_owner)) return true;
    if (!parcel.owner_id.empty() && owner == parcel.owner_id) return true;
    return false;
}

bool ParcelSet::bit_get(const std::vector<std::uint8_t>& bitmap, int cells_x, int cells_y,
                        int cell_x, int cell_y) {
    if (cell_x < 0 || cell_y < 0 || cell_x >= cells_x || cell_y >= cells_y) return false;
    const std::size_t index = static_cast<std::size_t>(cell_y) * cells_x + cell_x;
    const std::size_t byte = index >> 3;
    if (byte >= bitmap.size()) return false;
    return (bitmap[byte] & (1U << (index & 7U))) != 0;
}

void ParcelSet::bit_set(std::vector<std::uint8_t>& bitmap, int cells_x, int cells_y,
                        int cell_x, int cell_y, bool value) {
    if (cell_x < 0 || cell_y < 0 || cell_x >= cells_x || cell_y >= cells_y) return;
    const std::size_t index = static_cast<std::size_t>(cell_y) * cells_x + cell_x;
    const std::size_t byte = index >> 3;
    if (byte >= bitmap.size()) bitmap.resize(byte + 1, 0);
    if (value) bitmap[byte] |= static_cast<std::uint8_t>(1U << (index & 7U));
    else bitmap[byte] &= static_cast<std::uint8_t>(~(1U << (index & 7U)));
}

std::vector<std::uint8_t> ParcelSet::full_bitmap(int cells_x, int cells_y) {
    return rectangle_bitmap(cells_x, cells_y, 0, 0, cells_x * 4, cells_y * 4);
}

std::vector<std::uint8_t> ParcelSet::rectangle_bitmap(int cells_x, int cells_y, int west,
                                                      int south, int east, int north) {
    std::vector<std::uint8_t> bitmap((cells_x * cells_y + 7) / 8, 0);
    const int start_x = std::max(0, west / 4);
    const int start_y = std::max(0, south / 4);
    const int end_x = std::min(cells_x, east / 4);
    const int end_y = std::min(cells_y, north / 4);
    for (int y = start_y; y < end_y; ++y)
        for (int x = start_x; x < end_x; ++x)
            bit_set(bitmap, cells_x, cells_y, x, y, true);
    return bitmap;
}

ParcelSet::ParcelSet(int region_size_x_metres, int region_size_y_metres, std::string global_id,
                     std::string owner_id, std::int32_t claim_date)
    : region_size_x_(region_size_x_metres), region_size_y_(region_size_y_metres),
      cells_x_(cells_for(region_size_x_metres)), cells_y_(cells_for(region_size_y_metres)) {
    Parcel parcel;
    parcel.global_id = std::move(global_id);
    parcel.local_id = next_local_id();
    parcel.name = "Homeworldz";
    parcel.owner_id = std::move(owner_id);
    parcel.flags = default_parcel_flags;
    parcel.claim_date = claim_date;
    parcel.bitmap = full_bitmap(cells_x_, cells_y_);
    parcels_.push_back(std::move(parcel));
}

ParcelSet::ParcelSet(int region_size_x_metres, int region_size_y_metres,
                     std::vector<Parcel> parcels)
    : region_size_x_(region_size_x_metres), region_size_y_(region_size_y_metres),
      cells_x_(cells_for(region_size_x_metres)), cells_y_(cells_for(region_size_y_metres)),
      parcels_(std::move(parcels)) {
    const int bytes = bitmap_bytes();
    for (auto& parcel : parcels_) {
        if (static_cast<int>(parcel.bitmap.size()) < bytes) parcel.bitmap.resize(bytes, 0);
        last_local_id_ = std::max(last_local_id_, parcel.local_id);
    }
}

std::int32_t ParcelSet::next_local_id() { return ++last_local_id_; }

Parcel* ParcelSet::find_by_local_id(std::int32_t local_id) {
    for (auto& parcel : parcels_)
        if (parcel.local_id == local_id) return &parcel;
    return nullptr;
}

const Parcel* ParcelSet::find_by_local_id(std::int32_t local_id) const {
    for (const auto& parcel : parcels_)
        if (parcel.local_id == local_id) return &parcel;
    return nullptr;
}

const Parcel* ParcelSet::parcel_at(float x, float y) const {
    if (x < 0.0F || y < 0.0F || x >= static_cast<float>(region_size_x_) ||
        y >= static_cast<float>(region_size_y_))
        return nullptr;
    const int cell_x = metre_to_cell(x, cells_x_);
    const int cell_y = metre_to_cell(y, cells_y_);
    for (const auto& parcel : parcels_)
        if (parcel.contains_cell(cells_x_, cells_y_, cell_x, cell_y)) return &parcel;
    return nullptr;
}

const Parcel* ParcelSet::parcel_at_cell(int cell_x, int cell_y) const {
    if (cell_x < 0 || cell_y < 0 || cell_x >= cells_x_ || cell_y >= cells_y_) return nullptr;
    for (const auto& parcel : parcels_)
        if (parcel.contains_cell(cells_x_, cells_y_, cell_x, cell_y)) return &parcel;
    return nullptr;
}

namespace {

// The ParcelOverlay byte for one absolute cell: ownership colour relative to
// `agent`, plus west/south borders computed against the full parcel grid.
// Region-edge borders apply at the region edge (cell 0), never a window edge.
std::uint8_t overlay_cell_byte(const ParcelSet& set, std::string_view agent, int cell_x,
                               int cell_y) {
    const Parcel* parcel = set.parcel_at_cell(cell_x, cell_y);
    std::uint8_t byte = overlay_public;
    if (parcel != nullptr) {
        if (!parcel->owner_id.empty() && parcel->owner_id == agent)
            byte = overlay_owned_by_self;
        else if ((parcel->flags & flag_for_sale) != 0)
            byte = overlay_for_sale;
        else if (parcel->owner_id.empty())
            byte = overlay_public;
        else
            byte = overlay_owned_by_other;
    }
    // West/south borders: region edge, or a different parcel neighbour.
    if (cell_x == 0 || set.parcel_at_cell(cell_x - 1, cell_y) != parcel)
        byte |= overlay_border_west;
    if (cell_y == 0 || set.parcel_at_cell(cell_x, cell_y - 1) != parcel)
        byte |= overlay_border_south;
    return byte;
}

} // namespace

std::vector<std::uint8_t> ParcelSet::overlay_for(std::string_view agent,
                                                 std::string_view region_owner) const {
    if (cells_x_ == cells_y_) return overlay_window_for(agent, region_owner, 0, 0, cells_x_);
    static_cast<void>(region_owner);
    std::vector<std::uint8_t> cells(static_cast<std::size_t>(cells_x_) * cells_y_, 0);
    for (int y = 0; y < cells_y_; ++y)
        for (int x = 0; x < cells_x_; ++x)
            cells[static_cast<std::size_t>(y) * cells_x_ + x] =
                overlay_cell_byte(*this, agent, x, y);
    return cells;
}

std::vector<std::uint8_t> ParcelSet::overlay_window_for(std::string_view agent,
                                                        std::string_view region_owner,
                                                        int cell_x0, int cell_y0,
                                                        int window_cells) const {
    static_cast<void>(region_owner);
    std::vector<std::uint8_t> cells(
        static_cast<std::size_t>(window_cells) * window_cells, 0);
    for (int wy = 0; wy < window_cells; ++wy)
        for (int wx = 0; wx < window_cells; ++wx) {
            const int x = cell_x0 + wx;
            const int y = cell_y0 + wy;
            std::uint8_t byte = overlay_public;
            if (x >= 0 && y >= 0 && x < cells_x_ && y < cells_y_)
                byte = overlay_cell_byte(*this, agent, x, y);
            cells[static_cast<std::size_t>(wy) * window_cells + wx] = byte;
        }
    return cells;
}

const Parcel* ParcelSet::parcel_covering(float west, float south, float east, float north) const {
    const int start_x = std::max(0, static_cast<int>(std::floor(west / 4.0F)));
    const int start_y = std::max(0, static_cast<int>(std::floor(south / 4.0F)));
    const int end_x = std::min(cells_x_, static_cast<int>(std::ceil(east / 4.0F)));
    const int end_y = std::min(cells_y_, static_cast<int>(std::ceil(north / 4.0F)));
    if (start_x >= end_x || start_y >= end_y) return nullptr;
    const Parcel* found = nullptr;
    for (int y = start_y; y < end_y; ++y)
        for (int x = start_x; x < end_x; ++x) {
            const Parcel* here = nullptr;
            for (const auto& parcel : parcels_)
                if (parcel.contains_cell(cells_x_, cells_y_, x, y)) {
                    here = &parcel;
                    break;
                }
            if (here == nullptr) return nullptr;
            if (found == nullptr) found = here;
            else if (found != here) return nullptr;
        }
    return found;
}

std::optional<std::int32_t> ParcelSet::divide(float west, float south, float east, float north,
                                              std::string new_global_id, std::string owner_id,
                                              std::int32_t claim_date) {
    const int start_x = static_cast<int>(std::floor(std::min(west, east) / 4.0F));
    const int start_y = static_cast<int>(std::floor(std::min(south, north) / 4.0F));
    const int end_x = static_cast<int>(std::ceil(std::max(west, east) / 4.0F));
    const int end_y = static_cast<int>(std::ceil(std::max(south, north) / 4.0F));
    if (start_x < 0 || start_y < 0 || end_x > cells_x_ || end_y > cells_y_) return std::nullopt;
    if (start_x >= end_x || start_y >= end_y) return std::nullopt;

    // The rectangle must lie entirely within exactly one parcel.
    Parcel* source = nullptr;
    for (int y = start_y; y < end_y; ++y)
        for (int x = start_x; x < end_x; ++x) {
            Parcel* here = nullptr;
            for (auto& parcel : parcels_)
                if (parcel.contains_cell(cells_x_, cells_y_, x, y)) {
                    here = &parcel;
                    break;
                }
            if (here == nullptr) return std::nullopt;
            if (source == nullptr) source = here;
            else if (source != here) return std::nullopt;
        }
    if (source == nullptr) return std::nullopt;
    // Refuse to "divide" a rectangle covering the whole source parcel.
    if (source->area(cells_x_, cells_y_) == (end_x - start_x) * (end_y - start_y) * 16)
        return std::nullopt;

    Parcel carved;
    carved.global_id = std::move(new_global_id);
    carved.local_id = next_local_id();
    carved.name = "Parcel";
    carved.owner_id = std::move(owner_id);
    carved.flags = default_parcel_flags;
    carved.claim_date = claim_date;
    carved.bitmap.assign(bitmap_bytes(), 0);
    for (int y = start_y; y < end_y; ++y)
        for (int x = start_x; x < end_x; ++x) {
            bit_set(carved.bitmap, cells_x_, cells_y_, x, y, true);
            bit_set(source->bitmap, cells_x_, cells_y_, x, y, false);
        }
    const std::int32_t local_id = carved.local_id;
    parcels_.push_back(std::move(carved));
    return local_id;
}

std::optional<std::int32_t> ParcelSet::join(float west, float south, float east, float north,
                                            std::string_view owner_id) {
    const int start_x = std::max(0, static_cast<int>(std::floor(std::min(west, east) / 4.0F)));
    const int start_y = std::max(0, static_cast<int>(std::floor(std::min(south, north) / 4.0F)));
    const int end_x = std::min(cells_x_, static_cast<int>(std::ceil(std::max(west, east) / 4.0F)));
    const int end_y = std::min(cells_y_, static_cast<int>(std::ceil(std::max(south, north) / 4.0F)));
    if (start_x >= end_x || start_y >= end_y) return std::nullopt;

    // Collect the distinct parcels intersecting the rectangle.
    std::vector<std::int32_t> touched;
    for (int y = start_y; y < end_y; ++y)
        for (int x = start_x; x < end_x; ++x)
            for (auto& parcel : parcels_)
                if (parcel.contains_cell(cells_x_, cells_y_, x, y)) {
                    if (std::find(touched.begin(), touched.end(), parcel.local_id) == touched.end())
                        touched.push_back(parcel.local_id);
                    break;
                }
    if (touched.size() < 2) return std::nullopt;
    // Every touched parcel must share the requested owner.
    for (const auto local_id : touched) {
        const Parcel* parcel = find_by_local_id(local_id);
        if (parcel == nullptr || parcel->owner_id != owner_id) return std::nullopt;
    }

    const std::int32_t master_id = *std::min_element(touched.begin(), touched.end());
    Parcel* master = find_by_local_id(master_id);
    if (master == nullptr) return std::nullopt;
    for (const auto local_id : touched) {
        if (local_id == master_id) continue;
        const Parcel* other = find_by_local_id(local_id);
        if (other == nullptr) continue;
        for (int y = 0; y < cells_y_; ++y)
            for (int x = 0; x < cells_x_; ++x)
                if (bit_get(other->bitmap, cells_x_, cells_y_, x, y))
                    bit_set(master->bitmap, cells_x_, cells_y_, x, y, true);
    }
    parcels_.erase(std::remove_if(parcels_.begin(), parcels_.end(),
                       [&](const Parcel& parcel) {
                           return parcel.local_id != master_id &&
                                  std::find(touched.begin(), touched.end(), parcel.local_id) !=
                                      touched.end();
                       }),
                   parcels_.end());
    return master_id;
}

} // namespace homeworldz::parcel
