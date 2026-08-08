#include "homeworldz/region_storage.h"
#include "homeworldz/sha256.h"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

int main() {
    const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto path = std::filesystem::temp_directory_path() / ("homeworldz-storage-test-" + suffix);
    try {
        const std::array abc{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
        if (homeworldz::crypto::sha256_hex(abc) !=
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") return 1;
        homeworldz::scene::Scene scene;
        const auto first = scene.create("first", {1, 2, 3}, {0.5, 0, 0});
        const auto second = scene.create("second \"line\"\n", {4, 5, 6});
        auto* primitive = scene.find(second);
        if (primitive == nullptr) return 1;
        primitive->object_id = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
        primitive->owner_id = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
        primitive->creator_id = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
        primitive->scale = {0.5, 0.75, 1.25};
        primitive->rotation = {0.25, 0.5, 0.125};
        primitive->description = "storage test primitive";
        primitive->material = 4;
        primitive->physical = true;
        primitive->phantom = true;
        primitive->physics_shape_type = 2;
        primitive->physics_density = 125.0;
        primitive->physics_friction = 0.7;
        primitive->physics_restitution = 0.25;
        primitive->physics_gravity_multiplier = 1.5;
        primitive->texture_entry = {std::byte{0xaa}, std::byte{0xbb}, std::byte{0xcc}};
        primitive->path_curve = 0x20;
        primitive->profile_curve = 0x05;
        primitive->path_begin = 0x1234;
        primitive->path_scale_x = 200;
        primitive->path_scale_y = 100;
        primitive->path_shear_x = 0xce;
        primitive->path_skew = 7;
        primitive->profile_hollow = 0x5678;
        primitive->parent_id = first;
        primitive->local_position = {1.25, -2.5, 3.75};
        primitive->local_rotation = {0.125, -0.25, 0.5};
        primitive->task_inventory_serial = 7;
        primitive->task_inventory.push_back({
            "11111111-2222-4333-8444-555555555555",
            "66666666-7777-4888-8999-aaaaaaaaaaaa",
            "bbbbbbbb-cccc-4ddd-8eee-ffffffffffff",
            primitive->owner_id,
            "12121212-3434-4567-8787-909090909090",
            "00000000-0000-0000-0000-000000000000",
            "Stored Texture", "task inventory persistence",
            0, 0, 0x00000001, 0x0009e000, 0x0008e000, 0x00000000,
            0x00000000, 0x0008e000, 0, 0, 123456789});
        const auto temporary = scene.create("temporary", {7, 8, 9});
        scene.find(temporary)->temporary = true;
        scene.find(first)->task_inventory_serial = 9;
        std::filesystem::create_directories(path);
        sqlite3* legacy_database = nullptr;
        if (sqlite3_open((path / "region.db").string().c_str(), &legacy_database) != SQLITE_OK) return 1;
        const auto legacy_result = sqlite3_exec(
            legacy_database,
            "CREATE TABLE asset_mappings (viewer_id TEXT PRIMARY KEY, sha256 TEXT NOT NULL, "
            "size INTEGER NOT NULL, created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
            "INSERT INTO asset_mappings (viewer_id, sha256, size) VALUES ("
            "'99999999-9999-4999-8999-999999999999', "
            "'0000000000000000000000000000000000000000000000000000000000000000', 0);",
            nullptr, nullptr, nullptr);
        sqlite3_close(legacy_database);
        if (legacy_result != SQLITE_OK) return 1;
        {
            homeworldz::storage::RegionStorage storage(path);
            const auto migrated = storage.find_asset("99999999-9999-4999-8999-999999999999");
            if (!migrated || migrated->creator_id != "00000000-0000-0000-0000-000000000000") return 1;
            storage.save_snapshot(scene);
            auto metadata = storage.snapshot_metadata();
            if (metadata.revision != scene.revision() || metadata.path != "scene/snapshot.json") return 1;
            {
                std::ifstream input(path / metadata.path, std::ios::binary);
                const std::string snapshot((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
                if (snapshot.find(R"("name":"first")") == std::string::npos ||
                    snapshot.find(R"("name":"second \"line\"\n")") == std::string::npos ||
                    snapshot.find(R"("name":"temporary")") != std::string::npos ||
                    snapshot.find(R"("taskInventorySerial":7)") == std::string::npos ||
                    snapshot.find(R"("name":"Stored Texture")") == std::string::npos) return 1;
            }
            auto* entity = scene.find(first);
            if (entity == nullptr) return 1;
            entity->velocity.x = 1.0;
            entity->avatar_flying = true;
            scene.step(1.0);
            storage.save_snapshot(scene);
            metadata = storage.snapshot_metadata();
            if (metadata.revision != scene.revision() || std::filesystem::exists(path / "scene/snapshot.json.tmp")) return 1;

            homeworldz::scene::Scene restored;
            if (!storage.load_snapshot(restored) || restored.revision() != scene.revision() || restored.size() != 2) return 1;
            const auto* restored_first = restored.find(first);
            const auto* restored_second = restored.find(second);
            if (restored_first == nullptr || restored_first->position.x != 2.0 ||
                restored_first->velocity.x != 1.0 || !restored_first->avatar_flying ||
                restored_first->task_inventory_serial != 9 ||
                !restored_first->task_inventory.empty() ||
                restored_second == nullptr ||
                restored_second->name != "second \"line\"\n" ||
                restored_second->object_id != primitive->object_id ||
                restored_second->owner_id != primitive->owner_id ||
                restored_second->creator_id != primitive->creator_id ||
                restored_second->base_permissions != 0x0009e000 ||
                restored_second->next_owner_permissions != 0x0008e000 ||
                restored_second->scale.y != 0.75 || restored_second->rotation.x != 0.25 ||
                restored_second->rotation.y != 0.5 ||
                restored_second->description != "storage test primitive" || restored_second->material != 4 ||
                !restored_second->physical || !restored_second->phantom ||
                restored_second->physics_shape_type != 2 || restored_second->physics_density != 125.0 ||
                restored_second->physics_friction != 0.7 ||
                restored_second->physics_restitution != 0.25 ||
                restored_second->physics_gravity_multiplier != 1.5 ||
                restored_second->texture_entry != primitive->texture_entry ||
                restored_second->path_curve != 0x20 || restored_second->profile_curve != 0x05 ||
                restored_second->path_begin != 0x1234 || restored_second->path_scale_x != 200 ||
                restored_second->path_scale_y != 100 || restored_second->path_shear_x != 0xce ||
                restored_second->path_skew != 7 || restored_second->profile_hollow != 0x5678 ||
                restored_second->parent_id != first || restored_second->local_position.x != 1.25 ||
                restored_second->local_position.y != -2.5 || restored_second->local_position.z != 3.75 ||
                restored_second->local_rotation.x != 0.125 || restored_second->local_rotation.y != -0.25 ||
                restored_second->local_rotation.z != 0.5 ||
                restored_second->task_inventory_serial != 7 ||
                restored_second->task_inventory.size() != 1 ||
                restored_second->task_inventory[0].item_id !=
                    "11111111-2222-4333-8444-555555555555" ||
                restored_second->task_inventory[0].asset_id !=
                    "66666666-7777-4888-8999-aaaaaaaaaaaa" ||
                restored_second->task_inventory[0].creator_id !=
                    "bbbbbbbb-cccc-4ddd-8eee-ffffffffffff" ||
                restored_second->task_inventory[0].name != "Stored Texture" ||
                restored_second->task_inventory[0].current_permissions != 0x0008e000 ||
                restored_second->task_inventory[0].creation_date != 123456789 ||
                restored.create("next") != 3) return 1;

            const std::array content{std::byte{0x00}, std::byte{0x7f}, std::byte{0xff}, std::byte{0x42}};
            const auto first_asset = storage.store_asset(
                "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "11111111-1111-4111-8111-111111111111", content);
            const auto second_asset = storage.store_asset(
                "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", "22222222-2222-4222-8222-222222222222", content);
            if (first_asset.sha256 != second_asset.sha256 || first_asset.size != content.size() ||
                first_asset.creator_id != "11111111-1111-4111-8111-111111111111") return 1;
            const auto mapping = storage.find_asset("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
            if (!mapping || mapping->sha256 != first_asset.sha256 ||
                mapping->creator_id != "11111111-1111-4111-8111-111111111111") return 1;
            const auto migrated_content = std::array{std::byte{0x01}, std::byte{0x02}};
            const auto unknown_asset = storage.store_asset(
                "88888888-8888-4888-8888-888888888888",
                "00000000-0000-0000-0000-000000000000", migrated_content);
            const auto known_asset = storage.store_asset(
                unknown_asset.viewer_id, "33333333-3333-4333-8333-333333333333", migrated_content);
            if (known_asset.creator_id != "33333333-3333-4333-8333-333333333333" ||
                storage.find_asset(unknown_asset.viewer_id)->creator_id != known_asset.creator_id) return 1;
            const auto assets = storage.list_assets();
            const auto contains_asset = [&assets](const homeworldz::storage::AssetMetadata& wanted) {
                return std::any_of(assets.begin(), assets.end(), [&wanted](const auto& asset) {
                    return asset.viewer_id == wanted.viewer_id && asset.creator_id == wanted.creator_id &&
                           asset.sha256 == wanted.sha256 && asset.size == wanted.size;
                });
            };
            if (assets.size() != 4 || !contains_asset(first_asset) || !contains_asset(second_asset) ||
                !contains_asset(known_asset)) return 1;
            bool invalid_creator_rejected = false;
            try {
                storage.store_asset("ffffffff-ffff-4fff-8fff-ffffffffffff", "not-a-uuid", content);
            } catch (const std::invalid_argument&) {
                invalid_creator_rejected = true;
            }
            if (!invalid_creator_rejected) return 1;
            bool conflicting_content_rejected = false;
            try {
                const std::array different{std::byte{0x01}};
                storage.store_asset("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
                                    "11111111-1111-4111-8111-111111111111", different);
            } catch (const std::invalid_argument&) {
                conflicting_content_rejected = true;
            }
            bool conflicting_creator_rejected = false;
            try {
                storage.store_asset("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
                                    "22222222-2222-4222-8222-222222222222", content);
            } catch (const std::invalid_argument&) {
                conflicting_creator_rejected = true;
            }
            const auto repeated = storage.store_asset(
                "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
                "11111111-1111-4111-8111-111111111111", content);
            if (!conflicting_content_rejected || !conflicting_creator_rejected ||
                repeated.sha256 != first_asset.sha256 || repeated.creator_id != first_asset.creator_id)
                return 1;
            const auto reconciled = storage.reconcile_asset_creator(
                first_asset.viewer_id, "44444444-4444-4444-8444-444444444444",
                first_asset.sha256, first_asset.size);
            if (reconciled.creator_id != "44444444-4444-4444-8444-444444444444" ||
                storage.find_asset(first_asset.viewer_id)->creator_id != reconciled.creator_id)
                return 1;
            bool invalid_reconciliation_rejected = false;
            try {
                storage.reconcile_asset_creator(first_asset.viewer_id, first_asset.creator_id,
                                                std::string(64, '0'), first_asset.size);
            } catch (const std::invalid_argument&) {
                invalid_reconciliation_rejected = true;
            }
            if (!invalid_reconciliation_rejected) return 1;
            storage.store_baked_texture("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee", 8,
                                        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
            if (storage.find_baked_texture("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee", 8) !=
                    "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa" ||
                storage.find_baked_texture("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee", 9)) return 1;
            const auto loaded = storage.read_asset("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
            if (loaded.size() != content.size() || !std::equal(loaded.begin(), loaded.end(), content.begin())) return 1;
            const auto source = path / "source" / "nested";
            std::filesystem::create_directories(source);
            {
                std::ofstream output(source / "cccccccc-cccc-4ccc-8ccc-cccccccccccc.j2c", std::ios::binary);
                output.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
            }
            {
                std::ofstream output(source / "ignored.txt", std::ios::binary);
                output << "ignored";
            }
            {
                std::ofstream output(source / "dddddddd-dddd-4ddd-8ddd-dddddddddddd.bodypart", std::ios::binary);
                output.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
            }
            {
                std::ofstream output(source / "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee.clothing", std::ios::binary);
                output.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
            }
            {
                std::ofstream output(source / "ffffffff-ffff-4fff-8fff-ffffffffffff.ogg", std::ios::binary);
                output.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
            }
            {
                std::ofstream output(source / "12345678-1234-4234-8234-123456789abc.settings", std::ios::binary);
                output.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
            }
            // A texture's canonical form is a modern image (ADR 0033 M3), so
            // PNG imports as an asset like any other.
            {
                std::ofstream output(source / "aaaaaaaa-1111-4111-8111-aaaaaaaaaaaa.png", std::ios::binary);
                output.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
            }
            // But the filename *is* the asset id, so a file that merely shares
            // an extension with an asset is not one. Accepting .png without
            // this rule swept in the heightmap source images beside the real
            // assets and crash-looped every region on the store's UUID check
            // (2026-07-31); the extension was never the test that mattered.
            {
                std::ofstream output(source / "plateau-square.png", std::ios::binary);
                output.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
            }
            constexpr std::string_view importer = "33333333-3333-4333-8333-333333333333";
            if (storage.import_asset_directory(path / "missing", importer) != 0 ||
                storage.import_asset_directory(path / "source", importer) != 6 ||
                storage.read_asset("aaaaaaaa-1111-4111-8111-aaaaaaaaaaaa") !=
                    std::vector<std::byte>(content.begin(), content.end()) ||
                storage.read_asset("cccccccc-cccc-4ccc-8ccc-cccccccccccc") !=
                    std::vector<std::byte>(content.begin(), content.end()) ||
                storage.read_asset("dddddddd-dddd-4ddd-8ddd-dddddddddddd") !=
                    std::vector<std::byte>(content.begin(), content.end()) ||
                storage.read_asset("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee") !=
                    std::vector<std::byte>(content.begin(), content.end()) ||
                storage.read_asset("ffffffff-ffff-4fff-8fff-ffffffffffff") !=
                    std::vector<std::byte>(content.begin(), content.end()) ||
                storage.read_asset("12345678-1234-4234-8234-123456789abc") !=
                    std::vector<std::byte>(content.begin(), content.end()) ||
                storage.find_asset("cccccccc-cccc-4ccc-8ccc-cccccccccccc")->creator_id != importer) return 1;
            const auto blob = path / "assets" / first_asset.sha256.substr(0, 2) / first_asset.sha256.substr(2);
            if (!std::filesystem::is_regular_file(blob)) return 1;
            {
                std::ofstream corrupt(blob, std::ios::binary | std::ios::trunc);
                corrupt << "corrupt";
            }
            bool corruption_detected = false;
            try {
                static_cast<void>(storage.read_asset("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"));
            } catch (const std::runtime_error&) {
                corruption_detected = true;
            }
            if (!corruption_detected) return 1;

            // An attachment is not the region's to keep. Removing the filter that
            // excludes one makes this block fail at load_snapshot, not at the
            // find calls below: a worn linkset is two levels deep — child prim,
            // worn root, avatar — and the loader rejects nested links, so a
            // stored attachment costs the entire scene, not one stray prim.
            {
                homeworldz::scene::Scene worn_scene;
                const auto ground = worn_scene.create("ground prim", {10, 11, 12});
                const auto wearer = worn_scene.create("wearer", {20, 21, 22});
                const auto attachment = worn_scene.create("worn root", {20, 21, 22});
                auto* worn_root = worn_scene.find(attachment);
                if (worn_root == nullptr) return 1;
                worn_root->object_id = "dddddddd-1111-4111-8111-dddddddddddd";
                worn_root->parent_id = wearer;
                worn_root->attachment_point = 5;
                worn_root->attachment_item_id = "eeeeeeee-1111-4111-8111-eeeeeeeeeeee";
                const auto worn_child = worn_scene.create("worn child", {20, 21, 22});
                auto* child = worn_scene.find(worn_child);
                if (child == nullptr) return 1;
                child->parent_id = attachment;
                homeworldz::storage::RegionStorage worn_storage(path / "worn");
                worn_storage.save_snapshot(worn_scene);
                homeworldz::scene::Scene restored_worn;
                if (!worn_storage.load_snapshot(restored_worn)) return 1;
                if (restored_worn.find(ground) == nullptr) return 1;
                // The child carries no point of its own — it is excluded because
                // the root it hangs from is worn, which is the case that would
                // otherwise leave half a linkset behind.
                if (restored_worn.find(attachment) != nullptr ||
                    restored_worn.find(worn_child) != nullptr) return 1;
            }

            // Parcel persistence round-trip: a divided region with an access entry.
            if (storage.load_parcels()) return 1; // none stored yet
            homeworldz::parcel::ParcelSet set(256, "aaaa0000-0000-4000-8000-000000000001",
                                              "0b0b0b0b-0000-4000-8000-000000000002", 555);
            const auto carved = set.divide(0.0F, 0.0F, 64.0F, 64.0F,
                                           "aaaa0000-0000-4000-8000-000000000003",
                                           "0c0c0c0c-0000-4000-8000-000000000004", 777);
            if (!carved) return 1;
            auto* edited = set.find_by_local_id(1);
            if (edited == nullptr) return 1;
            edited->name = "Storage Parcel";
            edited->description = "round trip \"desc\"";
            edited->flags = homeworldz::parcel::default_parcel_flags |
                            homeworldz::parcel::flag_use_ban_list;
            edited->landing_type = static_cast<std::uint8_t>(homeworldz::parcel::LandingType::landing_point);
            edited->user_location = {12.0F, 34.0F, 25.0F};
            edited->access.push_back({"0d0d0d0d-0000-4000-8000-000000000005", 0,
                                      homeworldz::parcel::access_ban});
            storage.save_parcels(set.parcels());
            const auto restored_parcels = storage.load_parcels();
            if (!restored_parcels || restored_parcels->size() != 2) return 1;
            const homeworldz::parcel::ParcelSet reopened(256, *restored_parcels);
            const auto* whole = reopened.find_by_local_id(1);
            const auto* small = reopened.find_by_local_id(*carved);
            if (whole == nullptr || small == nullptr) return 1;
            if (whole->name != "Storage Parcel" || whole->description != "round trip \"desc\"" ||
                whole->owner_id != "0b0b0b0b-0000-4000-8000-000000000002" ||
                (whole->flags & homeworldz::parcel::flag_use_ban_list) == 0 ||
                whole->landing_type !=
                    static_cast<std::uint8_t>(homeworldz::parcel::LandingType::landing_point) ||
                whole->user_location.x != 12.0F || whole->user_location.z != 25.0F ||
                whole->access.size() != 1 ||
                whole->access[0].agent_id != "0d0d0d0d-0000-4000-8000-000000000005" ||
                whole->access[0].flags != homeworldz::parcel::access_ban) return 1;
            if (small->owner_id != "0c0c0c0c-0000-4000-8000-000000000004" ||
                small->area(reopened.edge_cells()) != 64 * 64) return 1;
            if (reopened.parcel_at(10.0F, 10.0F) != small ||
                reopened.parcel_at(200.0F, 200.0F) != whole) return 1;

            // Material definitions persist, which is the whole point of the
            // RenderMaterials work: an assignment that survived only until the
            // region restarted would be the same silent loss with a longer
            // fuse. Storing the same id twice is a no-op rather than an error,
            // because the id is the definition's own hash and two viewers
            // assigning the same material must share one row.
            if (!storage.load_render_materials().empty()) return 1; // none stored yet
            const std::vector<std::byte> definition{std::byte{'d'}, std::byte{'e'}, std::byte{'f'}};
            const std::vector<std::byte> other{std::byte{'x'}};
            storage.store_render_material("11112222-3333-4444-5555-666677778888", definition);
            storage.store_render_material("11112222-3333-4444-5555-666677778888", definition);
            storage.store_render_material("99998888-7777-6666-5555-444433332222", other);
            const auto materials = storage.load_render_materials();
            if (materials.size() != 2) return 1;
            bool found_first = false, found_second = false;
            for (const auto& [id, bytes] : materials) {
                if (id == "11112222-3333-4444-5555-666677778888" && bytes == definition)
                    found_first = true;
                if (id == "99998888-7777-6666-5555-444433332222" && bytes == other)
                    found_second = true;
            }
            if (!found_first || !found_second) return 1;

            // Terrain layers an operator set from the viewer's Terrain tab.
            // Absent must be distinguishable from stored, because "never
            // touched" should follow a change of defaults and "set to these
            // values" must not — a load that quietly reported the defaults
            // either way would erase that difference.
            std::array<std::string, 4> layer_ids{};
            std::array<float, 4> layer_low{};
            std::array<float, 4> layer_high{};
            if (storage.load_terrain_settings(layer_ids, layer_low, layer_high)) return 1; // never set
            const std::array<std::string, 4> chosen{
                "aaaaaaaa-0000-4000-8000-000000000001",
                "bbbbbbbb-0000-4000-8000-000000000002",
                "cccccccc-0000-4000-8000-000000000003",
                "dddddddd-0000-4000-8000-000000000004"};
            // Per-corner and asymmetric on purpose: a bug that wrote one value
            // to all four, or swapped low with high, passes any uniform fixture.
            const std::array<float, 4> chosen_low{20.0F, 21.5F, 22.0F, 23.25F};
            const std::array<float, 4> chosen_high{60.0F, 61.5F, 62.0F, 63.25F};
            storage.save_terrain_settings(chosen, chosen_low, chosen_high);
            if (!storage.load_terrain_settings(layer_ids, layer_low, layer_high)) return 1;
            if (layer_ids != chosen || layer_low != chosen_low || layer_high != chosen_high) return 1;
            // The Region/Estate form's own settings, kept separate from the
            // layers because either can be untouched while the other is not.
            if (storage.load_region_settings()) return 54;  // never set
            homeworldz::storage::RegionStorage::RegionSettings wanted{
                22.5, 80.0, -40.0, false, true, 13.25};
            storage.save_region_settings(wanted);
            const auto read_back = storage.load_region_settings();
            if (!read_back) return 55;
            // Every field, and the two booleans set opposite to their defaults:
            // a struct copied field-by-field with one line missing passes any
            // test that only checks the numbers.
            if (read_back->water_height != 22.5 || read_back->terrain_raise != 80.0 ||
                read_back->terrain_lower != -40.0 || read_back->use_estate_sun != false ||
                read_back->fixed_sun != true || read_back->sun_hour != 13.25) return 56;

            // Saving again replaces the single row rather than adding one.
            const std::array<float, 4> raised{30.0F, 30.0F, 30.0F, 30.0F};
            storage.save_terrain_settings(chosen, raised, chosen_high);
            if (!storage.load_terrain_settings(layer_ids, layer_low, layer_high)) return 1;
            if (layer_low != raised || layer_high != chosen_high) return 1;
        }
        // Reopened from disk: the definitions are on the filesystem, not merely
        // in the handle that wrote them.
        {
            homeworldz::storage::RegionStorage reopened(path);
            const auto materials = reopened.load_render_materials();
            if (materials.size() != 2) return 1;
            std::array<std::string, 4> layer_ids{};
            std::array<float, 4> layer_low{};
            std::array<float, 4> layer_high{};
            const auto settings = reopened.load_region_settings();
            if (!settings || settings->water_height != 22.5 || !settings->fixed_sun) return 57;
            if (!reopened.load_terrain_settings(layer_ids, layer_low, layer_high)) return 1;
            if (layer_ids[3] != "dddddddd-0000-4000-8000-000000000004" ||
                layer_low[0] != 30.0F || layer_high[3] != 63.25F) return 1;
        }
        std::filesystem::remove_all(path);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all(path);
        return 1;
    }
}
