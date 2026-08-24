#pragma once

#include "homeworldz/parcel.h"
#include "homeworldz/scene.h"

#include <cstdint>
#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct sqlite3;

namespace homeworldz::storage {

struct SnapshotMetadata {
    std::uint64_t revision{};
    std::string path;
};

struct AssetMetadata {
    std::string viewer_id;
    std::string creator_id;
    std::string sha256;
    std::uint64_t size{};
};

// A read-only view of one region's asset store, safe to use from a thread that
// is not the region's.
//
// It exists so serving asset bytes cannot be blocked by the region's own work.
// The grid fetches an asset from the region that holds it, and when that fetch
// is the durability check inside an inventory commit, the region's single
// thread is already waiting on that very commit: neither side moves until the
// grid client's deadline expires and the commit is refused. Bytes are the one
// thing a region can hand out without consulting anything the sim thread owns —
// the blob files are content-addressed and therefore immutable, and the id
// mapping is a single-row read — so this takes its own read-only connection and
// leaves the rest of the store alone.
//
// Read-only is enforced by the connection, not by convention: nothing reached
// through here can write.
class AssetReader {
public:
    explicit AssetReader(const std::filesystem::path& data_path);
    ~AssetReader();
    AssetReader(const AssetReader&) = delete;
    AssetReader& operator=(const AssetReader&) = delete;

    // The asset's verified bytes, or nothing when this region does not hold it
    // or the blob fails its content hash. Safe to call concurrently.
    std::optional<std::vector<std::byte>> read(std::string_view viewer_id) const;

private:
    std::filesystem::path data_path_;
    sqlite3* database_{};
};

class RegionStorage {
public:
    explicit RegionStorage(std::filesystem::path data_path);
    ~RegionStorage();
    RegionStorage(const RegionStorage&) = delete;
    RegionStorage& operator=(const RegionStorage&) = delete;

    void save_snapshot(const scene::Scene& scene);
    bool load_snapshot(scene::Scene& scene) const;
    SnapshotMetadata snapshot_metadata() const;
    AssetMetadata store_asset(std::string viewer_id, std::string creator_id,
                              std::span<const std::byte> content);
    AssetMetadata reconcile_asset_creator(std::string_view viewer_id, std::string_view creator_id,
                                          std::string_view sha256, std::uint64_t size);
    // Legacy Blinn-Phong material definitions, keyed by the id a face carries.
    // Storing one already present is a no-op: the id is the definition's own
    // hash, so the row cannot differ from what is being written.
    void store_render_material(std::string material_id, std::span<const std::byte> definition);
    std::vector<std::pair<std::string, std::vector<std::byte>>> load_render_materials() const;
    void store_baked_texture(std::string cache_id, std::uint8_t texture_index, std::string asset_id);
    std::optional<std::string> find_baked_texture(std::string_view cache_id,
                                                  std::uint8_t texture_index) const;
    std::size_t import_asset_directory(const std::filesystem::path& directory,
                                       std::string_view creator_id);
    std::vector<AssetMetadata> list_assets() const;
    std::optional<AssetMetadata> find_asset(std::string_view viewer_id) const;
    std::vector<std::byte> read_asset(std::string_view viewer_id) const;

    // The region's terrain layer settings, as an operator changed them through
    // the viewer's Region/Estate tab. Nothing stored means the region still
    // holds the shipped defaults.
    void save_terrain_settings(const std::array<std::string, 4>& assets,
                               const std::array<float, 4>& low,
                               const std::array<float, 4>& high);
    bool load_terrain_settings(std::array<std::string, 4>& assets,
                               std::array<float, 4>& low,
                               std::array<float, 4>& high) const;

    // The region's own settings from the viewer's Region/Estate form, as
    // distinct from the terrain layers above: water height, the terrain edit
    // limits, and region sun. Nothing stored means the region still runs on its
    // configured defaults, which is a different state from having been set to
    // values that happen to equal them.
    struct RegionSettings {
        double water_height{};
        double terrain_raise{};
        double terrain_lower{};
        bool use_estate_sun{true};
        bool fixed_sun{};
        double sun_hour{};
    };
    void save_region_settings(const RegionSettings& settings);
    std::optional<RegionSettings> load_region_settings() const;

    // Replace all persisted parcels (and their access lists) atomically.
    void save_parcels(const std::vector<parcel::Parcel>& parcels);
    // Load persisted parcels; nullopt when the region has never stored any.
    std::optional<std::vector<parcel::Parcel>> load_parcels() const;

private:
    std::filesystem::path data_path_;
    sqlite3* database_{};
};

} // namespace homeworldz::storage
