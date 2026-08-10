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

} // namespace homeworldz::mesh

#endif
