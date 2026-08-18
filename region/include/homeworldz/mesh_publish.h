// Turning an accepted mesh into something a creator owns.
//
// A mesh asset alone is not usable: viewers cannot rez a bare mesh, so an upload
// yields an OBJECT item exactly as it does in Second Life — the canonical mesh
// wrapped by a one-prim object whose sculpt entry names it, its textures split
// out as assets of their own, and an inventory item pointing at the wrapper.
//
// This was a single inline run inside the upload handler while there was one
// caller. FBX import (ADR 0035) is the second: one source file becomes one asset
// *per mesh*, because the ADR 0033 gate's limits are per asset and a Character
// Creator body is already authored as body, eyes, teeth, tongue, tearline and
// eye occlusion. Publishing six of those means calling this six times, which is
// only possible if it is a function.
#ifndef HOMEWORLDZ_MESH_PUBLISH_H
#define HOMEWORLDZ_MESH_PUBLISH_H

#include "homeworldz/grid_client.h"
#include "homeworldz/region_storage.h"
#include "homeworldz/viewer_protocol.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace homeworldz::mesh {

// The blank texture a face falls back to, and the same texture as a bare id for
// a model that textured some faces and not others.
//
// IMG_WHITE rather than a null id: the viewer names this UUID as a real asset
// ("dataserver" in indra_constants.cpp) and this grid serves it — checked, not
// assumed, because plywood is also a dataserver asset and renders here largely
// because viewers cache it from Second Life. A texture that resolves only for
// people who have been to another grid is not a default. Blank rather than
// plywood because plywood obscures the surface being inspected and invites the
// question "why is my model wood?".
const std::vector<std::byte>& blank_prim_texture_entry();
viewer::Uuid blank_texture_id();

// The asset envelope a viewer's material asset carries, checked exactly:
// `version` must be an accepted one and `type` must match, or
// `LLGLTFMaterialList::onAssetLoadComplete` gives up and the face keeps the
// default material.
inline constexpr std::string_view material_asset_version = "1.1";
inline constexpr std::string_view material_asset_type = "GLTF 2.0";

// One `AT_MATERIAL` (type 57) asset, as a viewer stores it (ADR 0033 M3).
//
// **The asset is not the glTF document.** It is an LLSD map with `version`,
// `type` and a `data` string, and the glTF lives inside `data`. Serving the bare
// document instead is silent: the viewer answers 200, fails the envelope check,
// and falls back to its default material — opaque white with no textures — so
// the model renders flat grey and nothing anywhere reports an error.
//
// Within `data`, the one departure from glTF is Second Life's: **an image's
// `uri` is a bare asset UUID** rather than a path or a data URI.
// `LLGLTFMaterial::setFromTexture` reads it with `texture_id.set(uri)`, so the
// string is parsed as a UUID and nothing fetches it as a URL.
//
// It exists because a TextureEntry has nowhere to say "draw both sides". A face
// that names a material is drawn from that material — textures included — so the
// document must carry the base-colour texture too, or a hair card would gain its
// back faces and lose its picture in the same step.
std::string material_document(const std::optional<viewer::Uuid>& base_colour_texture,
                              const std::array<float, 4>& base_colour_factor,
                              std::string_view alpha_mode, float alpha_cutoff,
                              bool double_sided);

struct PublishedMesh {
    // The canonical mesh blob, as stored.
    std::string asset_id;
    // The one-prim object that wraps it, which is what a viewer can rez.
    std::string object_asset_id;
    // The creator's inventory item, pointing at the wrapper.
    std::string item_id;
    std::size_t textures{};
    std::uint32_t triangles{};
    std::uint32_t materials{};
};

// Publish one accepted GLB. Throws with an actionable reason on any failure;
// the caller turns that into the creator's error, since a partial publish is
// not a state worth returning.
//
// `glb` must already have passed the acceptance gate: this stores and registers
// rather than validates, and handing it unvalidated bytes is how the gate gets
// bypassed.
// The folder an import's parts go into, named after the file they came from.
// The creator named the export and will look for that name, so the only things
// removed are what they did not choose: any directory the client sent, and the
// extension, whose case varies between Character Creator's own exporters
// (".Fbx" and ".fbx" both occur in one corpus).
std::string source_folder_name(std::string_view file_name);

// Whether an imported mesh is part of the avatar itself rather than something
// worn over it. The two go to different folders, because they are changed at
// different rates: a body is imported once and an outfit is swapped.
//
// Character Creator names the avatar's own meshes `CC_Base_*` and leaves the
// rest named by whoever authored them — "Slim_Jeans", "Classic_short",
// "Boots" — so the prefix carries the distinction and a name list would not.
// The face details it names separately (brows, lashes) are the exception and
// are listed, because they belong to the face rather than to an outfit.
//
// Hair and facial hair deliberately count as outfit: they are separately
// wearable and separately changed, which is the distinction this draws.
bool is_avatar_body_mesh(std::string_view mesh_name);

// `folder_id` places the resulting object item; empty means the owner's Objects
// folder, which is where a single upload belongs. An import passes a folder of
// its own, because one source file yields one asset per mesh and a fifteen-part
// character otherwise arrives as fifteen loose items with no sign they belong
// together.
PublishedMesh publish_glb(std::span<const std::byte> glb, std::string name,
                          const std::string& creator_user_id,
                          storage::RegionStorage& storage, grid::Client& grid,
                          const std::string& region_public_endpoint,
                          std::string_view folder_id = {});

// Publishing, moved off the region's loop.
//
// The region runs one thread. The `while (running)` loop in main.cpp services
// the HTTP listener, the viewer's UDP socket, and the simulation, and an HTTP
// handler that blocks does not merely delay a response — it stops physics and
// stops avatars moving. `publish_glb` is the worst offender on that loop by a
// wide margin: it makes **7 + 3T** blocking grid round trips for T textures, so
// sixteen textures is fifty-five, and grid_client.h's own deadline bounds one
// send or recv rather than a transfer. One upload could stall the region for as
// long as the grid took to answer all of them.
//
// So the loop hands the work here and defers its HTTP response, exactly as it
// already does for the viewer's long-poll event queue. The client sees the same
// 201 it always did; it simply arrives when the work is done rather than
// holding the region still until then.
//
// Two things this deliberately does *not* share with the loop:
//
//   - **the grid client**, which owns a socket. The worker builds its own from
//     the factory, on the worker thread, and the loop's client is untouched.
//   - **the region's storage handle**, which is one sqlite connection with no
//     lock and sixty-odd callers on the loop. The worker opens its own, which
//     WAL supports, rather than putting a mutex around all of them.
//
// It also removes a hazard rather than adding one. `store_vault_asset` is
// documented as load-bearing because "this region's single HTTP thread is busy
// with the upload and cannot answer a fetch-back until it returns" — with the
// publish off the loop, the loop *can* answer. The write-through stays, because
// it is right on its own merits, but it is no longer propping up a deadlock.
class PublishQueue {
public:
    enum class Kind {
        // A GLB, published straight through: one asset, one object, one item.
        Publish,
        // A source file stored as the canonical blob (ADR 0035). Answered 202
        // rather than 201, because 201 names an inventory item and there is not
        // one yet — the file is safe and being worked on, which is a different
        // promise. The import follows on its own.
        StoreSource,
        // The import of a stored source: one asset per mesh. Nobody is waiting
        // on a socket for this — it reports to the log, and the creator sees it
        // as items appearing in inventory.
        Import,
    };

    struct Result {
        std::uint64_t id{};
        Kind kind{Kind::Publish};
        bool ok{};
        // The publisher's own reason, for showing the creator verbatim.
        std::string error;
        // Publish and StoreSource: the one asset each produced.
        PublishedMesh published;
        // Import: the source it came from, and one entry per mesh published.
        std::string source_asset_id;
        std::vector<PublishedMesh> parts;
        // Import: the folder the parts were placed in, named after the source
        // file. Reported alongside the item ids rather than instead of them:
        // the ids say what arrived, and the folder is what a client re-reads to
        // see it in the shape a person will — including the Body and Outfit
        // subfolders, which the flat id list cannot show.
        std::string folder_id;
        // Import: what the importer reported about the file, for the log line
        // that is the only place this surfaces.
        std::size_t textures{};
        std::size_t opacity_composited{};
        std::size_t influences_pruned{};
        // Images the importer re-encoded smaller to fit the gate. Reported
        // because it is the one thing an import does that a creator could
        // otherwise only discover by looking closely at their own texture.
        std::size_t textures_downscaled{};
        // Import: joint names the skeleton did not recognise, deduplicated
        // across every part. Non-empty means the parts are geometry and
        // textures a creator can use and a body nobody can wear yet — the rig
        // question asked and recorded rather than answered (ADR 0035).
        std::vector<std::string> unresolved_joints;
        // Import: parts the gate refused, as "name: reason", one per part.
        //
        // A refused part used to take the whole import with it, on the reasoning
        // that half a body with no way to tell which half is worse than nothing.
        // The second half of that stopped being true once an import reports to
        // its uploader: the casualty can be *named*. Meanwhile the first half
        // was costing whole characters — one 133.9 MiB body part sank a
        // nine-part import whose other eight were fine. So the parts that pass
        // are published and the ones that do not are listed here.
        //
        // An import where *nothing* passed is still a failure rather than an
        // empty success: there is no inventory for the creator to look at, so
        // the reason has to arrive as an error.
        std::vector<std::string> refused_parts;
        // Who uploaded it. Carried back so the region can tell that one person
        // what became of their file: an import's answer arrives long after the
        // 202 that acknowledged it, so the log line was the only place it
        // surfaced and a creator found out by walking into the result.
        std::string creator_user_id;
    };

    // `open_storage` and `open_grid` are both called on the worker thread, once,
    // so nothing either returns is ever touched by two threads.
    PublishQueue(std::function<std::unique_ptr<storage::RegionStorage>()> open_storage,
                 std::function<std::unique_ptr<grid::Client>()> open_grid,
                 std::string region_public_endpoint);
    ~PublishQueue();
    PublishQueue(const PublishQueue&) = delete;
    PublishQueue& operator=(const PublishQueue&) = delete;

    // Takes a copy of the bytes: the caller's request buffer does not outlive
    // the handler, and the work does.
    std::uint64_t submit(std::vector<std::byte> glb, std::string name,
                         std::string creator_user_id);

    // A source-format upload (ADR 0035): store the creator's own file as the
    // canonical blob, register it, write it through to the vault, and queue its
    // import. Grid I/O throughout, which is why it belongs here and not on the
    // loop.
    //
    // The store is reported as soon as it is done, and the import follows as a
    // second job this queue raises for itself. That split is what lets the
    // creator hear "your file is stored" in seconds while the import takes as
    // long as it takes: a Character Creator body is six meshes, and publishing
    // six is six times 7 + 3T grid round trips.
    //
    // The import runs *here*, on this worker, rather than in meshsmith. ADR
    // 0035 puts conversion in the worker because "import is unbounded CPU on
    // attacker-supplied input and must not sit on any serving path" — this
    // thread is not a serving path, so the reason is satisfied even though the
    // letter names meshsmith. The alternative was meshsmith emitting one
    // combined glTF and the region splitting it back into meshes, since a
    // rendition is one blob per (asset, kind) and cannot be N; that costs a
    // glTF-to-glTF splitter re-deriving what gltf_from_fbx already does, and a
    // second parse of the same geometry, for no result the creator can tell
    // apart.
    std::uint64_t submit_source(std::vector<std::byte> source, std::string name,
                                std::string creator_user_id);

    // Everything finished since the last call. Called from the loop, which then
    // writes each result to the socket it kept.
    std::vector<Result> take_completed();

    // Work submitted and not yet reported, for the shutdown path and for
    // deciding whether a drain is worth the call.
    std::size_t outstanding() const;

    // Upload bytes this queue is holding — everything queued plus whatever the
    // worker has in hand. A job owns a copy of the file until it reports, so
    // these bytes outlive the socket they arrived on, and a memory budget that
    // counted only sockets would let the same 256 MiB be admitted twice over.
    std::uint64_t bytes_held() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace homeworldz::mesh

#endif
