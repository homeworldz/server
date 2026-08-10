// How this server opens an FBX, in one place.
//
// These options are the security-relevant part of reading a source format and
// there is exactly one correct set of them, so the importer and the diagnostic
// read the same way. A diagnostic that opened files more permissively than the
// importer would answer a question nobody asked.
#ifndef HOMEWORLDZ_FBX_LOAD_H
#define HOMEWORLDZ_FBX_LOAD_H

#include "ufbx.h"

namespace homeworldz::mesh {

inline ufbx_load_opts fbx_load_options() {
    ufbx_load_opts opts{};

    // Normalize into glTF's space, because glTF is what this pipeline converts
    // *from* (ADR 0033) and the axis map in axes.h starts there. FBX writes
    // centimetres far more often than not, and a body imported a hundred times
    // too large is the kind of wrong that reads as a modelling mistake.
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1.0f;
    // Bake the conversion into the vertices rather than leave it on the root
    // node: a reader that takes positions and ignores a root transform gets the
    // wrong answer silently, and there is no reason to keep that trap alive.
    opts.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;

    // **Not** load_external_files. Its own documentation calls it risky for
    // untrusted data because the file names the paths, which is precisely the
    // reach ADR 0035 confines to an extraction root. Nothing here follows a
    // path out of the file.
    opts.load_external_files = false;
    opts.ignore_missing_external_files = true;

    // Bounds on attacker-supplied input. The depth limit is the one that
    // matters for a recursive reader; the rest keep a small file from costing a
    // large amount of memory to discover it was hostile.
    opts.node_depth_limit = 256;
    opts.temp_allocator.memory_limit = 512u << 20;
    opts.result_allocator.memory_limit = 512u << 20;

    // Normals are an attribute a converter needs and an exporter may omit;
    // generating them beats emitting a mesh that shades flat.
    opts.generate_missing_normals = true;
    // Negative, zero and NaN weights removed before anything reads them, so
    // influence pruning sorts a list that means something.
    opts.clean_skin_weights = true;
    return opts;
}

} // namespace homeworldz::mesh

#endif
