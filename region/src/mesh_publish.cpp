#include "homeworldz/mesh_publish.h"

#include "homeworldz/fbx_import.h"
#include "homeworldz/mesh_acceptance.h"
#include "homeworldz/mesh_convert.h"
#include "homeworldz/mesh_model_upload.h"
#include "homeworldz/object_asset.h"
#include "homeworldz/scene.h"
#include "homeworldz/viewer_capabilities.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace homeworldz::mesh {

bool is_avatar_body_mesh(std::string_view mesh_name) {
    if (mesh_name.starts_with("CC_Base_") || mesh_name.starts_with("CC_Game_")) return true;
    // Face details Character Creator names on their own. Matched by prefix
    // because the trailing part is a variant ("Eyelash_Up", "Eyelash_Down").
    for (const auto* prefix : {"Eyebrow", "Eyelash", "Tearline", "Eye_Occlusion"})
        if (mesh_name.starts_with(prefix)) return true;
    return false;
}

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

std::string material_document(const std::optional<viewer::Uuid>& base_colour_texture,
                              const std::array<float, 4>& base_colour_factor,
                              std::string_view alpha_mode, bool double_sided) {
    const auto number = [](float value) {
        std::ostringstream text;
        text << std::setprecision(6) << std::noshowpoint << value;
        return text.str();
    };
    std::string document = "{\"asset\":{\"version\":\"2.0\"}";
    std::string texture_reference;
    if (base_colour_texture) {
        // The uri *is* the asset id. See material_document's declaration.
        document += ",\"images\":[{\"uri\":\"" + viewer::format_uuid(*base_colour_texture) +
            "\"}],\"textures\":[{\"source\":0}]";
        texture_reference = ",\"baseColorTexture\":{\"index\":0}";
    }
    document += ",\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[" +
        number(base_colour_factor[0]) + ',' + number(base_colour_factor[1]) + ',' +
        number(base_colour_factor[2]) + ',' + number(base_colour_factor[3]) + ']' +
        texture_reference +
        // The converter states these rather than leaving them to the
        // specification's default, for the same reason the derived glTF does:
        // an unstated metallicFactor defaults to 1 and draws the surface as
        // metal.
        ",\"metallicFactor\":0,\"roughnessFactor\":1}";
    if (alpha_mode != "OPAQUE") document += ",\"alphaMode\":\"" + std::string(alpha_mode) + "\"";
    document += ",\"doubleSided\":" + std::string(double_sided ? "true" : "false") + "}]}";

    // The asset is not the glTF document. It is an LLSD map *carrying* the glTF
    // document as a string, and a viewer that cannot read the envelope does not
    // report an error — it applies its default material, which is opaque white
    // with no textures. That is exactly what shipping the bare document looked
    // like: every face of two whole characters drew flat grey, every material
    // was fetched and answered 200, and no texture was ever requested, because
    // the material the viewer ended up holding named none.
    //
    // `LLGLTFMaterialList::onAssetLoadComplete` deserialises the buffer as LLSD,
    // requires `version` to be an accepted one and `type` to be exactly
    // "GLTF 2.0", and only then hands `data` to tinygltf.
    //
    // Written as LLSD XML opening with `<llsd>`, which `LLSDSerialize::deserialize`
    // accepts without any `<?llsd/...?>` header — it is that function's
    // LEGACY_NON_HEADER case.
    const auto escaped = [](std::string_view value) {
        std::string out;
        out.reserve(value.size());
        for (const char character : value) {
            switch (character) {
                case '&': out += "&amp;"; break;
                case '<': out += "&lt;"; break;
                case '>': out += "&gt;"; break;
                default: out += character;
            }
        }
        return out;
    };
    return "<llsd><map>"
           "<key>version</key><string>" + std::string(material_asset_version) + "</string>"
           "<key>type</key><string>" + std::string(material_asset_type) + "</string>"
           "<key>data</key><string>" + escaped(document) + "</string>"
           "</map></llsd>";
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

    // A glTF material asset for every face that needs one (ADR 0033 M3).
    //
    // "Needs one" means the face says something a TextureEntry cannot carry, and
    // today that is `doubleSided`: Character Creator builds hair from
    // single-sided cards and the viewer culls back faces, so half of a head of
    // hair is missing. Firestorm reads two-sidedness from a material and nowhere
    // else.
    //
    // Only those faces get a material, deliberately. A face that a TextureEntry
    // already describes completely is left alone rather than given a material
    // that repeats it — a material overrides the face's textures wholesale, so
    // every one issued is a second place the same surface is written, and a
    // wrong one hides its own cause.
    //
    // Materials are deduplicated by document: a body's fifteen faces sharing one
    // two-sided material is one asset, not fifteen.
    std::vector<std::pair<std::uint8_t, std::string>> face_materials;
    std::map<std::string, std::string> material_assets;
    for (std::size_t face = 0; face < extracted.face_textures.size(); ++face) {
        if (face >= extracted.face_double_sided.size() || !extracted.face_double_sided[face])
            continue;
        // The rule is simply what the source declares, and the narrower rule
        // that suggests itself does not work.
        //
        // Restricting this to *blended* faces looks right — a hair card is a
        // flat alpha sheet and a body is closed — and it is wrong. Kevin's hair
        // blends because its opacity arrived as a separate map; Alika's hair is
        // OPAQUE because hers is baked into the texture's own alpha, and she
        // needs two-sidedness every bit as much. alphaMode does not tell you
        // whether a surface has a back worth drawing.
        //
        // The honest discriminator is geometric — a mesh whose edges do not each
        // join exactly two triangles is open, and only an open surface can show
        // its back — but that is a different piece of work. Until then this
        // follows the file: Character Creator marks its materials two-sided, and
        // taking it at its word is defensible where second-guessing it is not.
        // A prim carries at most 14 render-material entries on the wire, and a
        // face beyond that simply keeps its TextureEntry.
        if (face_materials.size() >= 14 || face > 255) break;
        std::optional<viewer::Uuid> texture;
        const auto index = extracted.face_textures[face];
        if (index >= 0 && static_cast<std::size_t>(index) < texture_assets.size())
            texture = viewer::parse_uuid(texture_assets[static_cast<std::size_t>(index)]);
        const auto colour = face < extracted.face_colours.size()
                                ? extracted.face_colours[face]
                                : std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f};
        const auto alpha_mode = face < extracted.face_alpha_modes.size()
                                    ? extracted.face_alpha_modes[face]
                                    : std::string("OPAQUE");
        const auto document = material_document(texture, colour, alpha_mode, true);
        auto known = material_assets.find(document);
        if (known == material_assets.end()) {
            const auto bytes = std::span(reinterpret_cast<const std::byte*>(document.data()),
                                         document.size());
            const auto asset = storage.store_asset(viewer::random_uuid(), creator_user_id, bytes);
            if (!grid.register_asset(asset.viewer_id, asset.creator_id, asset.sha256, asset.size,
                                     region_public_endpoint, true) ||
                !grid.store_vault_asset(asset.viewer_id, bytes))
                throw std::runtime_error("material asset registration failed");
            known = material_assets.emplace(document, asset.viewer_id).first;
        }
        face_materials.emplace_back(static_cast<std::uint8_t>(face), known->second);
    }
    if (!face_materials.empty())
        std::cout << "{\"level\":\"info\",\"message\":\"mesh materials published\",\"faces\":"
                  << face_materials.size() << ",\"assets\":" << material_assets.size()
                  << "}" << std::endl;

    scene::Entity wrapper;
    wrapper.name = name;
    wrapper.creator_id = creator_user_id;
    wrapper.owner_id = creator_user_id;
    wrapper.sculpt_id = stored.viewer_id;
    wrapper.sculpt_type = 5; // mesh
    wrapper.face_materials = std::move(face_materials);
    // A face with no texture entry renders transparent (verified live on
    // Firestorm, 2026-07-29); the default entry is a bundled asset the startup
    // write-through keeps vault-held, so the commit closure stays deadlock-free.
    //
    // Where the GLB carried images, the faces name them instead: the extraction
    // reports a texture per face in the same order the converter emits faces,
    // from one shared traversal, so face N means the same face to both
    // (ADR 0033 M3). Until the j2c-texture rendition exists a viewer asking for
    // one of these gets not-yet, which is the same contract mesh has.
    //
    // The per-face colour is the material's baseColorFactor, not a fixed white.
    // Where a face has a map the two multiply, exactly as glTF says, and white
    // is the identity — so a textured face is unchanged. Where a face has *no*
    // map the factor is the entire surface, and sending white instead was how a
    // tinted or semitransparent material arrived as an opaque white slab: the
    // fallback texture is opaque white and so was the colour multiplying it, so
    // nothing carried the material at all.
    if (texture_assets.empty() && std::all_of(extracted.face_colours.begin(),
                                              extracted.face_colours.end(),
                                              [](const std::array<float, 4>& colour) {
                                                  return colour == std::array<float, 4>{
                                                             1.0f, 1.0f, 1.0f, 1.0f};
                                              })) {
        wrapper.texture_entry = blank_prim_texture_entry();
    } else {
        std::vector<mesh_model::Face> faces;
        std::vector<std::optional<viewer::Uuid>> images;
        for (const auto& asset : texture_assets) images.push_back(viewer::parse_uuid(asset));
        for (std::size_t at = 0; at < extracted.face_textures.size(); ++at)
            faces.push_back({extracted.face_textures[at],
                             at < extracted.face_colours.size()
                                 ? extracted.face_colours[at]
                                 : std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}});
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
            // Who this is for, recorded before the work rather than after it.
            // It used to be set only on the success path, so a failed import
            // knew the reason and not the person — which is exactly the case
            // that most needs telling someone.
            result.creator_user_id = job.creator_user_id;
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
                        result.folder_id = parts_folder;
                        // Body and outfit apart, because they are changed at
                        // different rates — the point of importing a body with
                        // its hidden faces intact is to put a different outfit
                        // on it later, and that is hard to do when both arrived
                        // as one list of fourteen.
                        //
                        // Made only when something will go in them: a source
                        // that is all body or all clothing should not grow an
                        // empty folder next to the full one.
                        std::string body_folder, outfit_folder;
                        for (const auto& mesh : imported.meshes) {
                            auto& wanted =
                                is_avatar_body_mesh(mesh.name) ? body_folder : outfit_folder;
                            if (!wanted.empty()) continue;
                            wanted = viewer::random_uuid();
                            if (!grid->create_inventory_folder(
                                    job.creator_user_id, wanted, parts_folder,
                                    is_avatar_body_mesh(mesh.name) ? "Body" : "Outfit", -1))
                                throw std::runtime_error("could not create a folder for the parts");
                        }
                        result.textures = imported.textures_embedded;
                        result.opacity_composited = imported.opacity_composited;
                        result.influences_pruned = imported.influences_pruned;
                        result.textures_downscaled = imported.textures_downscaled;
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
                            // A refused part is named and skipped rather than
                            // thrown: the rest of the file is still worth
                            // having, and the creator is told which part they
                            // did not get. See Result::refused_parts.
                            if (!accepted.accepted) {
                                result.refused_parts.push_back(mesh.name + ": " + accepted.reason);
                                continue;
                            }
                            for (const auto& joint : accepted.unresolved_joints)
                                if (std::find(result.unresolved_joints.begin(),
                                              result.unresolved_joints.end(),
                                              joint) == result.unresolved_joints.end())
                                    result.unresolved_joints.push_back(joint);
                            // The mesh's own name, which is the author's naming
                            // and the one they will recognise in inventory.
                            result.parts.push_back(publish_glb(
                                mesh.glb, mesh.name, job.creator_user_id, *storage, *grid,
                                region_public_endpoint,
                                is_avatar_body_mesh(mesh.name) ? body_folder : outfit_folder));
                        }
                        // Nothing survived the gate, so there is no inventory to
                        // find and the refusal has to be the answer.
                        if (result.parts.empty() && !result.refused_parts.empty())
                            throw std::runtime_error("every mesh was refused: " +
                                                     result.refused_parts.front());
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
