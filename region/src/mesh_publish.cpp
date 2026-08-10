#include "homeworldz/mesh_publish.h"

#include "homeworldz/fbx_import.h"
#include "homeworldz/mesh_acceptance.h"
#include "homeworldz/mesh_convert.h"
#include "homeworldz/mesh_model_upload.h"
#include "homeworldz/object_asset.h"
#include "homeworldz/scene.h"
#include "homeworldz/viewer_capabilities.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace homeworldz::mesh {

std::string source_folder_name(std::string_view file_name) {
    const auto slash = file_name.find_last_of("/\\");
    if (slash != std::string_view::npos) file_name.remove_prefix(slash + 1);
    const auto dot = file_name.find_last_of('.');
    if (dot != std::string_view::npos && dot > 0) file_name = file_name.substr(0, dot);
    std::string name{file_name};
    if (name.empty()) name = "Imported";
    if (name.size() > 255) name.resize(255);
    return name;
}

const std::vector<std::byte>& blank_prim_texture_entry() {
    static const auto entry = [] {
        const auto blank = viewer::parse_uuid("5748decc-f629-461c-9a36-a35a221fe21f");
        if (!blank) throw std::logic_error("blank texture UUID is invalid");
        return viewer::default_texture_entry(*blank);
    }();
    return entry;
}

viewer::Uuid blank_texture_id() {
    static const auto id =
        viewer::parse_uuid("5748decc-f629-461c-9a36-a35a221fe21f").value();
    return id;
}

PublishedMesh publish_glb(std::span<const std::byte> glb, std::string name,
                          const std::string& creator_user_id,
                          storage::RegionStorage& storage, grid::Client& grid,
                          const std::string& region_public_endpoint,
                          std::string_view folder_id) {
    if (name.empty()) name = "Mesh";
    if (name.size() > 255) name.resize(255);

    PublishedMesh published;

    // As in Second Life, a mesh upload yields an OBJECT item: viewers cannot
    // rez a bare mesh asset. The mesh asset (the canonical GLB) is wrapped by a
    // one-prim object whose sculpt entry names it, and whose scale is the
    // model's declared world bounds -- the same bounds the converter normalizes
    // by, so it renders at authored size (ADR 0033).
    const auto bounds = declared_world_bounds(glb);
    if (!bounds.ok) throw std::runtime_error("the GLB declares no position bounds");
    const auto stored = storage.store_asset(viewer::random_uuid(), creator_user_id, glb);
    if (!grid.register_asset(stored.viewer_id, stored.creator_id, stored.sha256, stored.size,
                             region_public_endpoint, true))
        throw std::runtime_error("mesh asset registration failed");
    // Write-through before the commit. Load-bearing here, not just the ADR 0026
    // optimization: the calling thread is the one that would serve the grid's
    // fetch-back, so the commit must find the blob already vault-held.
    if (!grid.store_vault_asset(stored.viewer_id, glb))
        throw std::runtime_error("vault write-through failed");

    // The GLB's textures become assets of their own (ADR 0033 M3). A viewer
    // cannot read a PNG embedded in a GLB, so each image is stored canonically
    // as the creator's own bytes - a format the modern client reads directly -
    // and a j2c-texture rendition is queued for the viewer pipeline. The same
    // canonical/derived split the mesh uses, pointed at images, rather than
    // storing JPEG2000 at rest and inverting it.
    const auto extracted = extract_textures(glb);
    if (!extracted.ok) throw std::runtime_error(extracted.error);
    std::vector<std::string> texture_assets;
    for (const auto& texture : extracted.textures) {
        const auto image =
            storage.store_asset(viewer::random_uuid(), creator_user_id, texture.bytes);
        if (!grid.register_asset(image.viewer_id, image.creator_id, image.sha256, image.size,
                                 region_public_endpoint, true) ||
            !grid.store_vault_asset(image.viewer_id, texture.bytes))
            throw std::runtime_error("texture asset registration failed");
        static_cast<void>(grid.request_asset_rendition(image.viewer_id, "j2c-texture"));
        texture_assets.push_back(image.viewer_id);
    }
    std::cout << "{\"level\":\"info\",\"message\":\"mesh textures extracted\",\"images\":"
              << extracted.textures.size()
              << ",\"faces\":" << extracted.face_textures.size() << ",\"textured\":"
              << std::count_if(extracted.face_textures.begin(), extracted.face_textures.end(),
                               [](int value) { return value >= 0; })
              << "}" << std::endl;
    published.textures = extracted.textures.size();

    scene::Entity wrapper;
    wrapper.name = name;
    wrapper.creator_id = creator_user_id;
    wrapper.owner_id = creator_user_id;
    wrapper.sculpt_id = stored.viewer_id;
    wrapper.sculpt_type = 5; // mesh
    // A face with no texture entry renders transparent (verified live on
    // Firestorm, 2026-07-29); the default entry is a bundled asset the startup
    // write-through keeps vault-held, so the commit closure stays deadlock-free.
    //
    // Where the GLB carried images, the faces name them instead: the extraction
    // reports a texture per face in the same order the converter emits faces,
    // from one shared traversal, so face N means the same face to both
    // (ADR 0033 M3). Until the j2c-texture rendition exists a viewer asking for
    // one of these gets not-yet, which is the same contract mesh has.
    if (texture_assets.empty()) {
        wrapper.texture_entry = blank_prim_texture_entry();
    } else {
        std::vector<mesh_model::Face> faces;
        std::vector<std::optional<viewer::Uuid>> images;
        for (const auto& asset : texture_assets) images.push_back(viewer::parse_uuid(asset));
        for (const auto index : extracted.face_textures)
            faces.push_back({index, {1.0f, 1.0f, 1.0f, 1.0f}});
        wrapper.texture_entry =
            mesh_model::instance_texture_entry(blank_texture_id(), faces, images);
    }
    wrapper.scale.x = std::clamp(bounds.extent[0], 0.01f, 64.0f);
    wrapper.scale.y = std::clamp(bounds.extent[1], 0.01f, 64.0f);
    wrapper.scale.z = std::clamp(bounds.extent[2], 0.01f, 64.0f);

    const auto wrapped = asset::serialize_linkset_asset(wrapper);
    const auto wrapped_bytes =
        std::span(reinterpret_cast<const std::byte*>(wrapped.data()), wrapped.size());
    const auto object_stored =
        storage.store_asset(viewer::random_uuid(), creator_user_id, wrapped_bytes);
    if (!grid.register_asset(object_stored.viewer_id, object_stored.creator_id,
                             object_stored.sha256, object_stored.size, region_public_endpoint,
                             true))
        throw std::runtime_error("object asset registration failed");
    if (!grid.store_vault_asset(object_stored.viewer_id, wrapped_bytes))
        throw std::runtime_error("object vault write-through failed");

    std::string destination{folder_id};
    if (destination.empty()) {
        const auto folder = grid.find_system_inventory_folder(creator_user_id, 6);
        if (!folder) throw std::runtime_error("objects folder unavailable");
        destination = *folder;
    }
    grid::InventoryItem item;
    item.item_id = viewer::random_uuid();
    item.creator_id = creator_user_id;
    item.owner_id = creator_user_id;
    item.folder_id = destination;
    item.asset_id = object_stored.viewer_id;
    item.asset_type = 6;
    item.inventory_type = 6;
    item.name = name;
    item.base_permissions = 0x7fffffff;
    item.current_permissions = 0x7fffffff;
    item.everyone_permissions = 0;
    item.next_permissions = 581632;
    // The commit's closure walk finds the wrapper and, through its sculptId,
    // the GLB -- both already vault-held by write-through, so no fetch-back can
    // deadlock the calling thread.
    if (!grid.create_inventory_item(creator_user_id, item))
        throw std::runtime_error("inventory commit was refused");
    static_cast<void>(grid.request_asset_rendition(stored.viewer_id, "sl-mesh"));

    published.asset_id = stored.viewer_id;
    published.object_asset_id = object_stored.viewer_id;
    published.item_id = item.item_id;
    return published;
}

namespace {

// Store a creator's source file and queue its import (ADR 0035). The upload is
// canonical and is never rewritten, so this is the step that must succeed even
// when the import later cannot: a file we could not read is a reportable state
// of a stored asset, not an upload that vanished.
PublishedMesh store_source(std::span<const std::byte> source, const std::string& creator_user_id,
                           storage::RegionStorage& storage, grid::Client& grid,
                           const std::string& region_public_endpoint) {
    PublishedMesh published;
    const auto stored = storage.store_asset(viewer::random_uuid(), creator_user_id, source);
    if (!grid.register_asset(stored.viewer_id, stored.creator_id, stored.sha256, stored.size,
                             region_public_endpoint, true))
        throw std::runtime_error("source asset registration failed");
    if (!grid.store_vault_asset(stored.viewer_id, source))
        throw std::runtime_error("source vault write-through failed");
    // Deliberately no rendition request. An earlier version queued a `gltf`
    // rendition here, from when meshsmith was going to do the conversion; the
    // import now runs on this worker instead, and nothing fetches a glTF of the
    // source anyway — the objects a creator ends up with reference the *parts*,
    // each its own asset with its own renditions, and the source is kept as the
    // canonical original rather than as something rendered.
    //
    // Leaving the request in was not merely wasted work. meshsmith refuses a
    // multi-mesh FBX by design, so every character uploaded would have left a
    // permanently failed job behind it, and a queue full of expected failures
    // is one nobody reads.
    published.asset_id = stored.viewer_id;
    return published;
}

} // namespace

struct PublishQueue::State {
    std::function<std::unique_ptr<storage::RegionStorage>()> open_storage;
    std::function<std::unique_ptr<grid::Client>()> open_grid;
    std::string region_public_endpoint;

    struct Job {
        std::uint64_t id{};
        std::vector<std::byte> glb;
        std::string name;
        std::string creator_user_id;
        Kind kind{Kind::Publish};
        // Import only: the stored source this came from.
        std::string source_asset_id;
    };

    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<Job> queue;
    std::vector<Result> completed;
    std::uint64_t next_id{1};
    std::size_t in_flight{};
    // Bytes of upload held by queued jobs and by the one being worked on.
    // Counted on submit and released when the job reports, so it covers the
    // whole time a copy of the file exists here.
    std::uint64_t bytes_held{};
    bool stopping{};
    std::thread worker;

    void run() {
        // Opened here rather than passed in: both own a handle — a sqlite
        // connection and a socket — and a handle created on one thread and used
        // on another is the bug this whole class exists to avoid.
        std::unique_ptr<storage::RegionStorage> storage;
        std::unique_ptr<grid::Client> grid;
        std::string open_error;
        try {
            storage = open_storage();
            grid = open_grid();
        } catch (const std::exception& error) {
            open_error = error.what();
        }
        if (!storage || !grid) {
            if (open_error.empty()) open_error = "the publish worker could not open its own "
                                                 "storage and grid connections";
            std::cout << "{\"level\":\"error\",\"message\":\"publish worker unavailable\",\"error\":\""
                      << open_error << "\"}" << std::endl;
        }

        for (;;) {
            Job job;
            std::size_t job_bytes = 0;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [this] { return stopping || !queue.empty(); });
                // Drain before stopping: a job accepted and dropped is an
                // upload the creator was told nothing about.
                if (queue.empty()) return;
                job = std::move(queue.front());
                queue.pop_front();
                // Still held — the worker has the bytes now instead of the
                // queue, and only reporting releases them.
                job_bytes = job.glb.size();
            }

            Result result;
            result.id = job.id;
            result.kind = job.kind;
            // Set when a stored source needs its import raising, which cannot
            // be done while the result lock is held below.
            std::optional<Job> follow_on;
            if (!storage || !grid) {
                result.error = open_error;
            } else {
                try {
                    switch (job.kind) {
                    case Kind::Publish:
                        result.published =
                            publish_glb(job.glb, job.name, job.creator_user_id, *storage, *grid,
                                        region_public_endpoint);
                        break;
                    case Kind::StoreSource: {
                        result.published =
                            store_source(job.glb, job.creator_user_id, *storage, *grid,
                                         region_public_endpoint);
                        // The import, raised now that the file is safe. It
                        // carries the bytes rather than re-reading them: they
                        // are already here, and a re-read would be a second
                        // chance for the two to differ.
                        Job next;
                        next.glb = std::move(job.glb);
                        next.name = job.name;
                        next.creator_user_id = job.creator_user_id;
                        next.kind = Kind::Import;
                        next.source_asset_id = result.published.asset_id;
                        follow_on = std::move(next);
                        break;
                    }
                    case Kind::Import: {
                        result.source_asset_id = job.source_asset_id;
                        const auto imported = gltf_from_fbx(job.glb);
                        if (!imported.ok) throw std::runtime_error(imported.error);
                        // A folder named after the source, holding its parts. A
                        // character imports as one asset per mesh, and fifteen
                        // items landing loose in Objects gives the wearer no
                        // sign which fifteen belong to each other.
                        //
                        // Failing here costs nothing but the retry: the source
                        // is already in the vault by the time an import runs
                        // (Kind::StoreSource precedes it), so refusing is
                        // better than quietly scattering the parts.
                        const auto objects =
                            grid->find_system_inventory_folder(job.creator_user_id, 6);
                        if (!objects) throw std::runtime_error("objects folder unavailable");
                        const auto parts_folder = viewer::random_uuid();
                        if (!grid->create_inventory_folder(job.creator_user_id, parts_folder,
                                                           *objects, source_folder_name(job.name),
                                                           -1))
                            throw std::runtime_error("could not create a folder for the parts");
                        result.textures = imported.textures_embedded;
                        result.opacity_composited = imported.opacity_composited;
                        result.influences_pruned = imported.influences_pruned;
                        for (const auto& mesh : imported.meshes) {
                            // The gate, on what the import produced. ADR 0035:
                            // "import is not a side door into the asset store",
                            // and without this it was exactly that - a part
                            // reached the vault having passed no check an
                            // uploaded GLB has to pass.
                            //
                            // Origin::Import differs in one thing only: a
                            // skeleton that resolves to nothing is recorded
                            // rather than refused, because for imported content
                            // that is an unanswered question and not an
                            // offence (mesh_acceptance.h).
                            const auto accepted = validate_glb(mesh.glb, Origin::Import);
                            if (!accepted.accepted)
                                throw std::runtime_error("mesh \"" + mesh.name +
                                                         "\" was refused: " + accepted.reason);
                            for (const auto& joint : accepted.unresolved_joints)
                                if (std::find(result.unresolved_joints.begin(),
                                              result.unresolved_joints.end(),
                                              joint) == result.unresolved_joints.end())
                                    result.unresolved_joints.push_back(joint);
                            // The mesh's own name, which is the author's naming
                            // and the one they will recognise in inventory.
                            // A part that fails takes the whole import with it
                            // rather than leaving a creator half a body with no
                            // way to tell which half.
                            result.parts.push_back(publish_glb(mesh.glb, mesh.name,
                                                               job.creator_user_id, *storage,
                                                               *grid, region_public_endpoint,
                                                               parts_folder));
                        }
                        break;
                    }
                    }
                    result.ok = true;
                } catch (const std::exception& error) {
                    result.error = error.what();
                } catch (...) {
                    // The loop is waiting on a reply for this job and an
                    // unknown exception must not turn into silence.
                    result.error = "the publish worker failed for an unrecorded reason";
                }
            }
            {
                std::lock_guard lock(mutex);
                completed.push_back(std::move(result));
                --in_flight;
                bytes_held -= (std::min<std::uint64_t>)(bytes_held, job_bytes);
                if (follow_on) {
                    follow_on->id = next_id++;
                    bytes_held += follow_on->glb.size();
                    queue.push_back(std::move(*follow_on));
                    ++in_flight;
                }
            }
        }
    }
};

PublishQueue::PublishQueue(std::function<std::unique_ptr<storage::RegionStorage>()> open_storage,
                           std::function<std::unique_ptr<grid::Client>()> open_grid,
                           std::string region_public_endpoint)
    : state_(std::make_unique<State>()) {
    state_->open_storage = std::move(open_storage);
    state_->open_grid = std::move(open_grid);
    state_->region_public_endpoint = std::move(region_public_endpoint);
    state_->worker = std::thread([state = state_.get()] { state->run(); });
}

PublishQueue::~PublishQueue() {
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
    }
    state_->wake.notify_all();
    if (state_->worker.joinable()) state_->worker.join();
}

std::uint64_t PublishQueue::submit(std::vector<std::byte> glb, std::string name,
                                   std::string creator_user_id) {
    std::uint64_t id = 0;
    {
        std::lock_guard lock(state_->mutex);
        id = state_->next_id++;
        state_->bytes_held += glb.size();
        state_->queue.push_back(
            {id, std::move(glb), std::move(name), std::move(creator_user_id), Kind::Publish, {}});
        ++state_->in_flight;
    }
    state_->wake.notify_one();
    return id;
}

std::uint64_t PublishQueue::submit_source(std::vector<std::byte> source, std::string name,
                                          std::string creator_user_id) {
    std::uint64_t id = 0;
    {
        std::lock_guard lock(state_->mutex);
        id = state_->next_id++;
        state_->bytes_held += source.size();
        state_->queue.push_back({id, std::move(source), std::move(name),
                                 std::move(creator_user_id), Kind::StoreSource, {}});
        ++state_->in_flight;
    }
    state_->wake.notify_one();
    return id;
}

std::vector<PublishQueue::Result> PublishQueue::take_completed() {
    std::lock_guard lock(state_->mutex);
    return std::exchange(state_->completed, {});
}

std::size_t PublishQueue::outstanding() const {
    std::lock_guard lock(state_->mutex);
    return state_->in_flight;
}

std::uint64_t PublishQueue::bytes_held() const {
    std::lock_guard lock(state_->mutex);
    return state_->bytes_held;
}

} // namespace homeworldz::mesh
