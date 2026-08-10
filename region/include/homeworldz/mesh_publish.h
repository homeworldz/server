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
PublishedMesh publish_glb(std::span<const std::byte> glb, std::string name,
                          const std::string& creator_user_id,
                          storage::RegionStorage& storage, grid::Client& grid,
                          const std::string& region_public_endpoint);

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
    struct Result {
        std::uint64_t id{};
        bool ok{};
        // The publisher's own reason, for showing the creator verbatim.
        std::string error;
        PublishedMesh published;
        // True for a source-format upload (ADR 0035): the creator's file has
        // been stored and its import queued, and no object or inventory item
        // exists yet. The two answer with different status codes because they
        // are different promises — 201 is "here is your item", 202 is "your
        // file is safe and being worked on".
        bool source_only{};
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
    // It stops there deliberately. What turns an imported file into objects a
    // creator owns is one asset per mesh, and where that runs is still open —
    // the parts are produced either by this worker or by meshsmith, and the two
    // differ in whether a glTF splitter has to exist. Storing the upload is the
    // half that is identical under both, and it is the half that must not be
    // lost: ADR 0035 makes import failure a property of the asset rather than a
    // failed upload, which is only true if the upload was kept.
    std::uint64_t submit_source(std::vector<std::byte> source, std::string name,
                                std::string creator_user_id);

    // Everything finished since the last call. Called from the loop, which then
    // writes each result to the socket it kept.
    std::vector<Result> take_completed();

    // Work submitted and not yet reported, for the shutdown path and for
    // deciding whether a drain is worth the call.
    std::size_t outstanding() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace homeworldz::mesh

#endif
