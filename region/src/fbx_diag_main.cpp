// What an FBX actually contains, and what the ADR 0033 gate would make of it,
// without uploading anything.
//
// The first step of ADR 0035, and deliberately a diagnostic rather than an
// importer: the corpus it was written against (Character Creator bodies) is
// local-only for licence reasons, so no fixture can carry it into the test
// suite, and a claim about that content is worth exactly as much as the run that
// produced it. This prints the run.
//
// It answers the questions the importer has to be built around:
//
//   - does ufbx read these files at all, and what does it warn about
//   - what space are they in - FBX defaults to centimetres and the axes vary
//   - how many meshes, and how many material parts within each mesh, since that
//     is the per-face TextureEntry the pipeline has never exercised
//   - where the textures live, and whether those paths resolve, which is the
//     whole reason ADR 0035 makes the upload unit a bundle
//   - what the skin binds to, and how much of that the skeleton would recognise
//
// It then runs the import itself and reports what each mesh became, through the
// same `gltf_from_fbx` the conversion worker calls and the same `validate_glb`
// the upload path applies — not a reimplementation of either, which would be a
// second copy able to disagree with the first. The load options likewise come
// from fbx_load.h rather than a copy here: a diagnostic that opened files more
// permissively than the importer would answer a question nobody asked.
//
//   homeworldz-fbx-diag <file.fbx> [more.fbx ...]
#include "homeworldz/avatar_joints.h"
#include "homeworldz/axes.h"
#include "homeworldz/fbx_import.h"
#include "homeworldz/mesh_acceptance.h"
#include "homeworldz/mesh_convert.h"
#include "homeworldz/rig_retarget.h"

#include "fbx_load.h"
#include "ufbx.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string_view text(const ufbx_string& value) { return {value.data, value.length}; }

std::string mebibytes(std::uintmax_t bytes) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.1f MiB",
                  static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buffer;
}

std::string metres(float value) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.3f", value);
    return buffer;
}

const char* axis_name(ufbx_coordinate_axis axis) {
    switch (axis) {
    case UFBX_COORDINATE_AXIS_POSITIVE_X: return "+X";
    case UFBX_COORDINATE_AXIS_NEGATIVE_X: return "-X";
    case UFBX_COORDINATE_AXIS_POSITIVE_Y: return "+Y";
    case UFBX_COORDINATE_AXIS_NEGATIVE_Y: return "-Y";
    case UFBX_COORDINATE_AXIS_POSITIVE_Z: return "+Z";
    case UFBX_COORDINATE_AXIS_NEGATIVE_Z: return "-Z";
    default: return "unknown";
    }
}

const char* exporter_name(ufbx_exporter exporter) {
    switch (exporter) {
    case UFBX_EXPORTER_FBX_SDK: return "FBX SDK";
    case UFBX_EXPORTER_BLENDER_BINARY: return "Blender (binary)";
    case UFBX_EXPORTER_BLENDER_ASCII: return "Blender (ASCII)";
    case UFBX_EXPORTER_MOTION_BUILDER: return "MotionBuilder";
    case UFBX_EXPORTER_UFBX_WRITE: return "ufbx";
    default: return "unrecorded";
    }
}

// A row of the gate table: a measured value against a published limit.
void limit_row(const char* label, std::uintmax_t measured, std::uintmax_t limit,
               bool& any_over) {
    const bool over = measured > limit;
    if (over) any_over = true;
    std::cout << "    " << std::left << std::setw(11) << label << std::right << std::setw(9)
              << measured << " / " << std::setw(9) << limit << "  " << (over ? "OVER" : "ok")
              << '\n';
}

bool report(const fs::path& path, const fs::path& write_to) {
    std::error_code size_error;
    const auto size_on_disk = fs::file_size(path, size_error);

    auto opts = homeworldz::mesh::fbx_load_options();
    ufbx_error error{};
    ufbx_scene* scene = ufbx_load_file(path.string().c_str(), &opts, &error);
    if (!scene) {
        char described[1024];
        ufbx_format_error(described, sizeof described, &error);
        std::cout << path.filename().string() << ": REFUSED by ufbx\n" << described << '\n';
        return false;
    }

    const auto& meta = scene->metadata;
    std::cout << path.filename().string() << ": FBX " << (meta.version / 1000) << '.'
              << (meta.version % 1000) / 100 << ' ' << (meta.ascii ? "ASCII" : "binary");
    if (!size_error) std::cout << ", " << mebibytes(size_on_disk);
    std::cout << '\n';
    std::cout << "  creator:  \"" << text(meta.creator) << "\" (" << exporter_name(meta.exporter)
              << ' ' << meta.exporter_version << ")\n";
    std::cout << "  space:    file was " << axis_name(scene->settings.original_axis_up)
              << " up at 1 unit = " << scene->settings.original_unit_meters
              << " m; loaded as glTF axes at 1 unit = 1 m\n";

    // Collapsed by kind. ufbx reports one entry per offending element, and the
    // first run of this against the corpus printed the same sentence 98 times —
    // which buries the two warnings that were not the same sentence.
    if (meta.warnings.count > 0) {
        std::map<std::string_view, std::size_t> by_kind;
        for (std::size_t at = 0; at < meta.warnings.count; ++at) {
            const auto& warning = meta.warnings.data[at];
            by_kind[text(warning.description)] += warning.count;
        }
        std::cout << "  warnings: " << meta.warnings.count << " in " << by_kind.size()
                  << " kind(s)\n";
        for (const auto& [description, count] : by_kind) {
            std::cout << "    - " << description;
            if (count > 1) std::cout << " (x" << count << ')';
            std::cout << '\n';
        }
    }

    // Meshes. material_parts is the interesting number: one part is one material
    // over a subset of the faces, which is one TextureEntry face downstream.
    //
    // Tracked per mesh as well as per file, because the gate's limits are
    // per *asset* and nothing says one FBX has to become one asset. If the worst
    // single mesh fits, an import that emits one asset per mesh fits, and the
    // whole-file totals are a fact about the packaging rather than the content.
    std::uintmax_t total_triangles = 0;
    std::uintmax_t total_parts = 0;
    std::size_t max_influences = 0;
    std::size_t worst_mesh_parts = 0, worst_mesh_textures = 0;
    std::map<std::string, std::array<float, 3>> posed_size;
    std::cout << "  meshes:   " << scene->meshes.count << '\n';
    for (std::size_t at = 0; at < scene->meshes.count; ++at) {
        const ufbx_mesh* mesh = scene->meshes.data[at];
        total_triangles += mesh->num_triangles;
        // A mesh with no materials still reports one part, covering every face.
        // Counting it as a face is right: it is one draw either way.
        const auto parts = std::max<std::size_t>(mesh->material_parts.count, 1);
        total_parts += parts;
        worst_mesh_parts = std::max(worst_mesh_parts, parts);

        // Distinct image files this mesh alone would need. Two materials sharing
        // a map cost one texture, not two, so counting bindings would overstate
        // it — and this number is the one deciding whether a per-mesh asset
        // clears the gate.
        std::set<std::uint32_t> mesh_textures;
        for (std::size_t m = 0; m < mesh->materials.count; ++m) {
            const ufbx_material* material = mesh->materials.data[m];
            for (std::size_t t = 0; t < material->textures.count; ++t) {
                const ufbx_texture* texture = material->textures.data[t].texture;
                if (texture && texture->has_file) mesh_textures.insert(texture->file_index);
            }
        }
        worst_mesh_textures = std::max(worst_mesh_textures, mesh_textures.size());

        // The bind pose as a renderer computes it: for each vertex, the weighted
        // sum over its influences of (joint world transform x inverse bind) —
        // which for ufbx is precomputed per cluster as `geometry_to_world`, and
        // is exactly the composition the emitted glTF encodes as a joint node's
        // matrix times its inverseBindMatrix. Measured here rather than asserted,
        // because bind-space accessor bounds cannot answer it and a body a
        // hundred times too large or lying on its side looks identical to a
        // correct one in every check that does not do this arithmetic.
        if (mesh->skin_deformers.count > 0 && mesh->vertex_position.exists) {
            const ufbx_skin_deformer* skin = mesh->skin_deformers.data[0];
            std::array<float, 3> low{}, high{};
            bool any = false;
            for (std::size_t vertex = 0; vertex < mesh->num_vertices; ++vertex) {
                if (vertex >= skin->vertices.count) break;
                const auto& entry = skin->vertices.data[vertex];
                if (entry.num_weights == 0) continue;
                const auto source = mesh->vertices.data[vertex];
                ufbx_vec3 accumulated{};
                double total = 0.0;
                const auto taken = (std::min<std::size_t>)(entry.num_weights, 4);
                for (std::size_t at = 0; at < taken; ++at) {
                    const auto& weight = skin->weights.data[entry.weight_begin + at];
                    if (weight.cluster_index >= skin->clusters.count) continue;
                    const auto& matrix = skin->clusters.data[weight.cluster_index]->geometry_to_world;
                    const auto placed = ufbx_transform_position(&matrix, source);
                    accumulated.x += placed.x * weight.weight;
                    accumulated.y += placed.y * weight.weight;
                    accumulated.z += placed.z * weight.weight;
                    total += weight.weight;
                }
                if (total <= 0.0) continue;
                std::array<float, 3> point{static_cast<float>(accumulated.x / total),
                                           static_cast<float>(accumulated.y / total),
                                           static_cast<float>(accumulated.z / total)};
                homeworldz::mesh::to_region_axes(point);
                if (!any) {
                    low = point;
                    high = point;
                    any = true;
                }
                for (int axis = 0; axis < 3; ++axis) {
                    low[axis] = (std::min)(low[axis], point[axis]);
                    high[axis] = (std::max)(high[axis], point[axis]);
                }
            }
            if (any) {
                std::array<float, 3> size{};
                for (int axis = 0; axis < 3; ++axis) size[axis] = high[axis] - low[axis];
                auto name = std::string(text(mesh->name));
                if (name.empty()) name = "mesh " + std::to_string(at);
                posed_size.emplace(std::move(name), size);
            }
        }

        std::cout << "    " << text(mesh->name) << ": " << mesh->num_vertices << " vertices, "
                  << mesh->num_faces << " faces, " << mesh->num_triangles << " triangles, "
                  << mesh->uv_sets.count << " uv set(s), " << mesh->materials.count
                  << " material(s) over " << mesh->material_parts.count << " part(s), "
                  << mesh_textures.size() << " texture(s)";
        if (mesh->skin_deformers.count > 0) {
            std::size_t influences = 0;
            for (std::size_t d = 0; d < mesh->skin_deformers.count; ++d)
                influences = std::max(influences,
                                      mesh->skin_deformers.data[d]->max_weights_per_vertex);
            max_influences = std::max(max_influences, influences);
            std::cout << ", skinned (" << influences << " influences)";
        }
        if (mesh->blend_deformers.count > 0)
            std::cout << ", " << mesh->blend_deformers.count << " blend deformer(s)";
        std::cout << '\n';
        for (std::size_t p = 0; p < mesh->material_parts.count; ++p) {
            const auto& part = mesh->material_parts.data[p];
            if (part.num_faces == 0) continue;
            const auto material = p < mesh->materials.count ? text(mesh->materials.data[p]->name)
                                                            : std::string_view{"<no material>"};
            std::cout << "      part " << p << ": " << material << ", " << part.num_faces
                      << " faces, " << part.num_triangles << " triangles\n";
        }
    }

    // Materials, and which texture each map binds. The map name is the FBX
    // property, which is what an importer has to translate into glTF's fixed
    // set - and the corpus is the evidence for how much of it there is.
    std::cout << "  materials: " << scene->materials.count << '\n';
    for (std::size_t at = 0; at < scene->materials.count; ++at) {
        const ufbx_material* material = scene->materials.data[at];
        std::cout << "    " << text(material->name) << ": " << material->textures.count
                  << " texture binding(s), shading model \"" << text(material->shading_model_name)
                  << "\"\n";
        for (std::size_t t = 0; t < material->textures.count; ++t) {
            const auto& binding = material->textures.data[t];
            // The leaf name only: which image this map binds is the question,
            // and the full path is both noise and, in this corpus, someone's
            // home directory repeated once per binding.
            std::cout << "      " << text(binding.material_prop) << " -> "
                      << (binding.texture
                              ? fs::path(text(binding.texture->relative_filename)).filename().string()
                              : std::string{"<none>"})
                      << '\n';
        }
    }

    // Textures, reported by *where the bytes are* and *what path the file
    // claims*, separately. They are separate questions and the corpus is the
    // reason it is worth saying so: these bodies carry every image inside the
    // FBX and still record an absolute path on the exporter's own machine, so a
    // reader that keys on the path finds nothing and a reader that keys on the
    // content needs no path at all.
    //
    // An external path is resolved against the FBX's own directory and confined
    // to it, which is the rule ADR 0035 states for archive entries, applied here
    // to the references rather than the entries.
    const auto root = fs::weakly_canonical(path.parent_path());
    std::size_t resolved = 0, missing = 0, escaping = 0, embedded = 0, absolute_paths = 0;
    std::uintmax_t texture_bytes = 0;
    std::cout << "  textures: " << scene->texture_files.count << " distinct file(s)\n";
    for (std::size_t at = 0; at < scene->texture_files.count; ++at) {
        const auto& file = scene->texture_files.data[at];
        // `relative_filename` is a claim, not a guarantee — ufbx's own note says
        // it "may be absolute if the file is saved in a different drive", and
        // every entry in this corpus is exactly that. Classify by what the path
        // *is*, because an importer that trusted the field name would try to
        // open a path on the exporting machine.
        auto relative = std::string(text(file.relative_filename));
        const auto absolute = std::string(text(file.absolute_filename));
        const bool relative_is_absolute = !relative.empty() && fs::path(relative).is_absolute();
        if (relative_is_absolute) relative.clear();

        std::string where;
        if (file.content.size > 0) {
            ++embedded;
            texture_bytes += file.content.size;
            where = "embedded";
        } else if (relative.empty()) {
            // Nothing to resolve against: no bytes and no relative path is a
            // texture an import cannot obtain by any means the bundle provides.
            ++missing;
            where = "NO SOURCE";
        } else {
            std::error_code resolve_error;
            const auto candidate = fs::weakly_canonical(root / fs::path(relative), resolve_error);
            const auto inside =
                !resolve_error &&
                std::mismatch(root.begin(), root.end(), candidate.begin(), candidate.end()).first ==
                    root.end();
            if (!inside) {
                ++escaping;
                where = "OUTSIDE ROOT";
            } else if (fs::exists(candidate)) {
                ++resolved;
                std::error_code file_size_error;
                const auto bytes = fs::file_size(candidate, file_size_error);
                if (!file_size_error) texture_bytes += bytes;
                where = "on disk";
            } else {
                ++missing;
                where = "MISSING";
            }
        }

        // The recorded path, whatever the bytes did. An absolute one is both
        // useless to us and someone else's filesystem: the corpus records a
        // named user's home directory in every entry, and that must not travel
        // any further than this diagnostic.
        std::string claimed = "no path";
        if (!relative.empty()) {
            claimed = "relative " + relative;
        } else if (relative_is_absolute || !absolute.empty()) {
            ++absolute_paths;
            claimed = "ABSOLUTE " +
                      std::string(relative_is_absolute ? text(file.relative_filename) : absolute);
        }
        std::cout << "    [" << std::left << std::setw(12) << where << std::right << "] "
                  << claimed;
        if (file.content.size > 0) std::cout << "  (" << mebibytes(file.content.size) << ')';
        std::cout << '\n';

        // With --write, the embedded images land beside the GLBs. Looking at
        // one is how a question like "does white mean opaque in this map"
        // gets answered, and it is not answerable from the file's own
        // vocabulary: FBX calls the slot TransparentColor and Reallusion names
        // the file Opacity, which are opposite conventions wearing each
        // other's clothes.
        if (!write_to.empty() && file.content.size > 0) {
            auto leaf = fs::path(std::string(text(file.filename))).filename().string();
            if (leaf.empty()) leaf = "texture_" + std::to_string(at);
            std::ofstream image(write_to / leaf, std::ios::binary);
            image.write(static_cast<const char*>(file.content.data),
                        static_cast<std::streamsize>(file.content.size));
        }
    }
    std::cout << "    on disk " << resolved << ", missing " << missing << ", outside root "
              << escaping << ", embedded " << embedded << "; " << mebibytes(texture_bytes)
              << " of image data\n";
    if (absolute_paths > 0)
        std::cout << "    " << absolute_paths
                  << " entr(ies) record only an absolute path from the exporting machine\n";

    // The skin, by name. Nothing here checks position: rig_check does that
    // against inverse bind matrices the converter has written, and there is no
    // converter yet. What can be decided from names alone is whether the
    // skeleton would recognise any of these at all, and for a Character Creator
    // rig the answer is the point (ADR 0035, "What this does not solve").
    std::set<std::string> joints;
    // Where each *retargeted* joint rests, in region axes and metres, for the
    // pose check below. Taken from the bone's world transform rather than its
    // inverse bind, since that is the frame a joint position override is
    // written in.
    std::map<std::string, std::array<float, 3>> joint_positions;
    std::map<std::string, std::array<float, 3>> bind_positions;
    for (std::size_t at = 0; at < scene->skin_clusters.count; ++at) {
        const ufbx_skin_cluster* cluster = scene->skin_clusters.data[at];
        if (!cluster->bone_node) continue;
        const auto name = std::string(text(cluster->bone_node->name));
        joints.insert(name);
        const auto target = homeworldz::mesh::retarget_joint(name);
        if (target.empty()) continue;
        const auto& translation = cluster->bone_node->node_to_world.cols[3];
        std::array<float, 3> position{static_cast<float>(translation.x),
                                      static_cast<float>(translation.y),
                                      static_cast<float>(translation.z)};
        homeworldz::mesh::to_region_axes(position);
        joint_positions.emplace(std::string(target), position);

        // The same joint as the file's *bind* pose puts it, which FBX stores
        // separately from the node transforms. A tool can export a character
        // standing one way while its skin was bound in another, and skinning
        // follows the bind — so if these two disagree, reading node_to_world is
        // reading the wrong pose.
        if (const auto* pose = cluster->bone_node->bind_pose) {
            if (const auto* bone_pose = ufbx_get_bone_pose(pose, cluster->bone_node)) {
                const auto& bound = bone_pose->bone_to_world.cols[3];
                std::array<float, 3> at{static_cast<float>(bound.x),
                                        static_cast<float>(bound.y),
                                        static_cast<float>(bound.z)};
                homeworldz::mesh::to_region_axes(at);
                bind_positions.emplace(std::string(target), at);
            }
        }
    }
    std::size_t riggable = 0;
    for (const auto& joint : joints)
        if (homeworldz::mesh::is_riggable_joint(joint)) ++riggable;
    std::cout << "  skin:     " << scene->skin_deformers.count << " deformer(s), " << joints.size()
              << " distinct joint(s), " << max_influences << " max influences per vertex\n";
    if (!joints.empty()) {
        // Every name, not a sample. Correspondence for retargeting
        // (docs/AUTO-RIGGING.md, Case 1) is decided joint by joint, and a
        // truncated list is exactly the thing that cannot be used to decide it.
        std::cout << "    joints:\n";
        for (const auto& joint : joints)
            std::cout << "      " << std::left << std::setw(34) << joint << std::right
                      << (homeworldz::mesh::is_riggable_joint(joint) ? "resolves" : "-") << '\n';
        std::cout << "    " << riggable << " of " << joints.size()
                  << " resolve to a skeleton joint";
        if (riggable == 0)
            std::cout << " - the rig is not wearable without retargeting, as ADR 0035 expects";
        std::cout << '\n';

        // And what a retarget would make of them (AUTO-RIGGING.md Case 1). The
        // number that decides whether a body can be worn is `targets`, not
        // `mapped`: the per-mesh budget counts distinct Bento joints, and the
        // folds are what bring a dense foreign rig under it.
        const std::vector<std::string> source_joints(joints.begin(), joints.end());
        const auto retarget = homeworldz::mesh::describe_retarget(source_joints);
        std::cout << "    retarget: " << retarget.mapped << " mapped onto "
                  << retarget.targets << " Bento joint(s), " << retarget.merged
                  << " merged, " << retarget.unmapped.size() << " unmapped";
        if (retarget.targets > homeworldz::mesh::max_joints_per_mesh)
            std::cout << "  OVER the " << homeworldz::mesh::max_joints_per_mesh
                      << "-joint per-mesh budget";
        std::cout << '\n';
        for (const auto& joint : retarget.unmapped)
            std::cout << "      unmapped: " << joint << '\n';

        // A-pose or T-pose, from the arm the rig actually has.
        //
        // This matters more than it sounds. Joint position overrides move a
        // joint's *rest* position, and SL joints are translation-only, so
        // animations rotate them from wherever rest is. Writing an A-posed
        // body's positions as overrides leaves the skeleton A-posed at rest and
        // every animation authored against T-pose lands the arms low — which
        // reads as an animation fault rather than a rig one, and is the
        // expensive kind of wrong to chase.
        //
        // Measured shoulder-to-wrist, because that is the segment the two poses
        // disagree about: the lateral reach is nearly identical either way and
        // only the height differs.
        if (const auto shoulder = joint_positions.find("mShoulderLeft"),
            wrist = joint_positions.find("mWristLeft");
            shoulder != joint_positions.end() && wrist != joint_positions.end()) {
            const auto reach = wrist->second[1] - shoulder->second[1];
            const auto drop = shoulder->second[2] - wrist->second[2];
            const auto degrees =
                static_cast<float>(std::atan2(drop, std::abs(reach)) * 180.0 / 3.14159265358979);
            std::cout << "    pose:     arm " << metres(degrees)
                      << " degrees below horizontal - "
                      << (degrees > 10.0f ? "A-POSE, resave in T-pose before rigging"
                          : degrees < -10.0f ? "arms raised; not a pose this maps"
                                             : "T-pose")
                      << '\n';
        }
        // And the bind pose, where the file records one. FBX stores it apart
        // from the node transforms, and skinning follows the bind — so if the
        // two disagree, the bind is what a retarget has to work from and the
        // node transforms are the wrong thing to have measured.
        if (const auto shoulder = bind_positions.find("mShoulderLeft"),
            wrist = bind_positions.find("mWristLeft");
            shoulder != bind_positions.end() && wrist != bind_positions.end()) {
            const auto reach = wrist->second[1] - shoulder->second[1];
            const auto drop = shoulder->second[2] - wrist->second[2];
            const auto degrees =
                static_cast<float>(std::atan2(drop, std::abs(reach)) * 180.0 / 3.14159265358979);
            std::cout << "    bind:     arm " << metres(degrees) << " degrees below horizontal"
                      << (degrees > 10.0f ? " - the bind pose is A-pose as well"
                                          : " - THE BIND POSE IS T")
                      << '\n';
        } else if (!joint_positions.empty()) {
            std::cout << "    bind:     the file records no bind pose; node transforms are all "
                         "there is\n";
        }
    }

    // Against the published gate. These are the ADR 0033 numbers the upload path
    // enforces on a GLB; an import that produces one has to satisfy them too, so
    // what the corpus does to them is the finding the importer is designed
    // around rather than a surprise found later.
    namespace gate = homeworldz::mesh;
    bool any_over = false;
    std::cout << "  against the ADR 0033 acceptance gate, whole file as one asset:\n";
    limit_row("triangles", total_triangles, gate::max_triangles, any_over);
    limit_row("materials", scene->materials.count, gate::max_materials, any_over);
    limit_row("textures", scene->texture_files.count, gate::max_textures, any_over);
    limit_row("faces", total_parts, gate::max_materials, any_over);
    limit_row("influences", max_influences, gate::max_rig_influences, any_over);

    // The same limits against the worst single mesh. Where this passes and the
    // block above fails, the file is not too big for the gate — the packaging
    // is, and one asset per mesh is the difference.
    bool split_over = false;
    std::cout << "  the same limits against the worst single mesh:\n";
    limit_row("materials", worst_mesh_parts, gate::max_materials, split_over);
    limit_row("textures", worst_mesh_textures, gate::max_textures, split_over);
    limit_row("faces", worst_mesh_parts, gate::max_materials, split_over);
    limit_row("influences", max_influences, gate::max_rig_influences, split_over);

    std::cout << "  verdict:  " << (any_over ? "over the gate as one asset" : "within the gate")
              << ", " << (split_over ? "still over it per mesh" : "within it per mesh") << '\n';

    ufbx_free_scene(scene);

    // And now the import itself, through the code the worker runs. Everything
    // above is a description of the file; this is the answer to whether it
    // becomes assets, which is the only question that finally matters.
    std::vector<std::byte> source;
    {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            std::cout << "  import:   the file cannot be re-read\n";
            return false;
        }
        const std::vector<char> raw{std::istreambuf_iterator<char>(input),
                                    std::istreambuf_iterator<char>()};
        source.resize(raw.size());
        for (std::size_t at = 0; at < raw.size(); ++at)
            source[at] = static_cast<std::byte>(raw[at]);
    }
    const auto imported = homeworldz::mesh::gltf_from_fbx(source);
    if (!imported.ok) {
        std::cout << "  import:   FAILED - " << imported.error << '\n';
        return false;
    }
    std::cout << "  import:   " << imported.meshes.size() << " asset(s), "
              << imported.textures_embedded << " of " << imported.textures_referenced
              << " textures carried";
    if (imported.influences_pruned > 0)
        std::cout << ", " << imported.influences_pruned
                  << " vertex influence list(s) pruned to four";
    if (imported.opacity_composited > 0)
        std::cout << ", " << imported.opacity_composited
                  << " opacity map(s) composited into base-colour alpha";
    if (imported.bindings_dropped > 0)
        std::cout << ", " << imported.bindings_dropped << " texture binding(s) glTF cannot carry";
    std::cout << '\n';

    bool import_clean = true;
    for (const auto& asset : imported.meshes) {
        // Measured height, in region axes and metres. This is the one number
        // that catches the two mistakes an importer makes silently: FBX writes
        // centimetres, so a unit left unconverted gives a body a hundred metres
        // tall, and a Y-up to Z-up map with the lateral axis wrong stands a
        // model upright and points it sideways (axes.h, which records what that
        // one cost). A human body reads about 1.7 m here or something is wrong
        // that no parse check would notice.
        const auto bounds = homeworldz::mesh::declared_world_bounds(asset.glb);
        // Put each asset through the gate it would meet on upload. A GLB this
        // importer wrote and the gate refuses is a defect in the importer, not
        // a property of the source — with one expected exception, which the
        // gate names for itself: joints the skeleton does not know.
        // Both verdicts, because they answer different questions and the
        // difference is the whole of ADR 0035's position on rigs. As an upload
        // a Character Creator part is refused for binding a skeleton that is
        // not ours; as an import the same bytes are accepted with the skeleton
        // recorded, and the geometry and textures are usable while wearing it
        // waits on retargeting.
        const auto accepted =
            homeworldz::mesh::validate_glb(asset.glb, homeworldz::mesh::Origin::Upload);
        const auto importable =
            homeworldz::mesh::validate_glb(asset.glb, homeworldz::mesh::Origin::Import);
        std::cout << "    " << std::left << std::setw(22) << asset.name << std::right << ' '
                  << mebibytes(asset.glb.size()) << ", " << asset.primitives << " primitive(s), "
                  << asset.triangles << " triangles, " << asset.textures << " texture(s)"
                  << (asset.skinned ? ", skinned" : "") << '\n';
        if (bounds.ok)
            std::cout << "      bind: " << metres(bounds.extent[0]) << " x "
                      << metres(bounds.extent[1]) << " x " << metres(bounds.extent[2])
                      << " m of bind-space geometry (x forward, y lateral, z up)\n";
        else
            std::cout << "      bind: the asset declares no readable bounds\n";
        // For a skinned asset the line above is *not* the shape anyone sees.
        // glTF ignores a skinned node's own transform and poses the vertices
        // through the joints, so bind-space geometry may legitimately lie on its
        // side while the posed body stands up — which is exactly what a
        // Character Creator body does. The number worth checking is the posed
        // one, measured back in the mesh loop where the matrices live.
        if (const auto posed = posed_size.find(asset.name); posed != posed_size.end())
            std::cout << "      posed: " << metres(posed->second[0]) << " x "
                      << metres(posed->second[1]) << " x " << metres(posed->second[2])
                      << " m once skinned\n";
        std::cout << "      as upload: " << (accepted.accepted ? "accepted" : "REFUSED");
        if (!accepted.accepted) std::cout << " - " << accepted.reason;
        std::cout << '\n';
        std::cout << "      as import: " << (importable.accepted ? "accepted" : "REFUSED");
        if (!importable.accepted) {
            std::cout << " - " << importable.reason;
            // Only the import verdict decides this tool's exit status. A part
            // refused as an upload for its skeleton is expected and is not a
            // fault in the importer; a part refused as an import is.
            import_clean = false;
        } else if (!importable.unresolved_joints.empty()) {
            std::cout << " (rig unresolved, not wearable: "
                      << importable.unresolved_joints.size() << " joint(s), e.g. "
                      << importable.unresolved_joints.front() << ')';
        }
        std::cout << '\n';

        if (!write_to.empty()) {
            // A GLB on disk is the only check that does not run through this
            // repository's own reader. Opening one in Blender or a viewer is
            // what tells us the geometry is right rather than merely parseable.
            auto name = asset.name;
            for (auto& character : name)
                if (character == '/' || character == '\\' || character == ':') character = '_';
            const auto target = write_to / (name + ".glb");
            std::ofstream output(target, std::ios::binary);
            output.write(reinterpret_cast<const char*>(asset.glb.data()),
                         static_cast<std::streamsize>(asset.glb.size()));
            std::cout << "      wrote: " << target.string() << '\n';

            // And the base-colour images back out of it, read by the same
            // extract_textures the upload path uses. These are the *composited*
            // maps, so opening one is how the opacity merge gets checked by
            // eye — arithmetic that inverts a mask produces a file that is
            // valid, the right size, and wrong in the only way that matters.
            const auto extracted = homeworldz::mesh::extract_textures(asset.glb);
            for (std::size_t at = 0; at < extracted.textures.size(); ++at) {
                const auto& texture = extracted.textures[at];
                const auto suffix = texture.mime == "image/png" ? ".png" : ".jpg";
                std::ofstream image(
                    write_to / (name + "_base" + std::to_string(at) + suffix), std::ios::binary);
                image.write(reinterpret_cast<const char*>(texture.bytes.data()),
                            static_cast<std::streamsize>(texture.bytes.size()));
            }
        }
    }
    // Only the import decides this. The whole-file gate table above is a fact
    // about packaging, not a verdict: nothing ever gates an FBX as one asset,
    // because import emits one asset per mesh and it is those the gate sees.
    // Failing on it would report every Character Creator body as broken.
    return import_clean;
}

} // namespace

int main(int argc, char** argv) {
    fs::path write_to;
    std::vector<fs::path> sources;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--write" && index + 1 < argc) write_to = argv[++index];
        else sources.emplace_back(argument);
    }
    if (sources.empty()) {
        std::cerr << "usage: homeworldz-fbx-diag [--write <directory>] <file.fbx> [more.fbx ...]\n";
        return 2;
    }
    if (!write_to.empty()) {
        std::error_code create_error;
        fs::create_directories(write_to, create_error);
        if (create_error) {
            std::cerr << "cannot write to " << write_to.string() << ": " << create_error.message()
                      << '\n';
            return 2;
        }
    }
    bool all_clear = true;
    for (std::size_t index = 0; index < sources.size(); ++index) {
        if (index > 0) std::cout << '\n';
        if (!report(sources[index], write_to)) all_clear = false;
    }
    return all_clear ? 0 : 1;
}
