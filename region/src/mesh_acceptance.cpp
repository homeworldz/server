#include "homeworldz/mesh_acceptance.h"
#include "homeworldz/avatar_joints.h"
#include "homeworldz/axes.h"
#include "homeworldz/rig_check.h"
#include "homeworldz/rig_retarget.h"

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>
#include <string_view>

namespace homeworldz::mesh {
namespace {

bool extension_allowed(std::string_view name) {
    for (const auto allowed : allowed_extensions)
        if (name == allowed) return true;
    return false;
}

Acceptance refuse(std::string reason) { return {false, std::move(reason), 0, 0, 0}; }

// data: URIs are self-contained; anything else reaches outside the file the
// creator uploaded, which the canonical blob must never do (ADR 0033).
bool external_uri(const char* uri) {
    if (uri == nullptr || *uri == '\0') return false;
    return std::strncmp(uri, "data:", 5) != 0;
}

} // namespace

std::string acceptance_policy_json() {
    std::string extensions;
    for (const auto allowed : allowed_extensions) {
        if (!extensions.empty()) extensions += ',';
        extensions += '"';
        extensions += allowed;
        extensions += '"';
    }
    std::string imported;
    for (const auto format : imported_formats) {
        if (!imported.empty()) imported += ',';
        imported += '"';
        imported += format;
        imported += '"';
    }
    return "{\"format\":\"glb\",\"uploadPath\":\"" + std::string(upload_path) +
        // Source formats the server imports (ADR 0035). `format` stays "glb"
        // because that is still what the limits below describe and what an
        // import produces; these are additional things the upload path accepts.
        "\",\"importedFormats\":[" + imported + "]" +
        // How a client learns what became of a file it uploaded. A source format
        // is answered 202 — stored, and being worked on — because there is no
        // inventory item yet; the answer arrives minutes later as this event.
        // Named here for the same reason terrain and water name theirs: a client
        // reading the block that says "you may send FBX" is exactly the client
        // that needs to know how it will be told the FBX landed, and it should
        // not have to find that in a document.
        //
        // The event reports failure too, so a client that handles it never has
        // to treat silence as either outcome.
        ",\"importedEvent\":\"sourceImported\"" +
        ",\"maxSourceBytes\":" + std::to_string(max_source_bytes) +
        ",\"maxFileBytes\":" + std::to_string(max_glb_bytes) +
        ",\"maxTriangles\":" + std::to_string(max_triangles) +
        ",\"maxMaterials\":" + std::to_string(max_materials) +
        ",\"maxTextures\":" + std::to_string(max_textures) +
        ",\"maxImageBytes\":" + std::to_string(max_image_bytes) +
        ",\"maxRigInfluences\":" + std::to_string(max_rig_influences) +
        // Which limits are in force now and which describe a capability not yet
        // switched on. maxRigInfluences was published ahead of M4 so importers
        // read the number rather than encode it, but a reader seeing a rig limit
        // beside "rigged": false reasonably calls that a contradiction - the
        // client core did, 2026-08-04. Saying so in the payload costs one key and
        // removes the guess.
        ",\"skeleton\":\"" + std::string(rigged_skeleton) + "\"" +
        ",\"skeletonJoints\":" + std::to_string(rigged_skeleton_joints) +
        ",\"maxJointsPerMesh\":" + std::to_string(max_joints_per_mesh) +
        ",\"forwardLooking\":[" +
        std::string(rigged_accepted ? ""
            : "\"maxRigInfluences\",\"skeleton\",\"skeletonJoints\","
              "\"maxJointsPerMesh\"") + "]" +
        ",\"draco\":" + (draco_accepted ? "true" : "false") +
        ",\"rigged\":" + (rigged_accepted ? "true" : "false") +
        ",\"allowedExtensions\":[" + extensions + "]}";
}

Acceptance validate_glb(std::span<const std::byte> content, Origin origin) {
    // Joint names this file binds that resolve to nothing in the skeleton. An
    // upload is refused at the first one; an import collects them and carries
    // them out in the result.
    std::vector<std::string> unresolved;
    // Set when any joint reached its Bento name through the retarget table
    // rather than by already being one.
    bool retargeted = false;
    if (content.size() > max_glb_bytes)
        return refuse("file is " + std::to_string(content.size()) +
                      " bytes; the limit is " + std::to_string(max_glb_bytes));
    if (content.size() < 12 || std::memcmp(content.data(), "glTF", 4) != 0)
        return refuse("not a GLB container");

    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse(&options, content.data(), content.size(), &data) != cgltf_result_success)
        return refuse("the GLB does not parse as glTF 2.0");
    struct Free {
        cgltf_data* data;
        ~Free() { cgltf_free(data); }
    } freer{data};
    if (data->file_type != cgltf_file_type_glb)
        return refuse("only the GLB container is accepted");
    if (cgltf_validate(data) != cgltf_result_success)
        return refuse("the glTF structure fails validation");

    // Refused, not ignored: an extension only one client understands renders
    // differently on the client that understands more (ADR 0033).
    for (cgltf_size index = 0; index < data->extensions_used_count; ++index) {
        const std::string_view name = data->extensions_used[index];
        if (!extension_allowed(name))
            return refuse("extension " + std::string(name) + " is not accepted");
    }
    for (cgltf_size index = 0; index < data->extensions_required_count; ++index) {
        const std::string_view name = data->extensions_required[index];
        if (!extension_allowed(name))
            return refuse("required extension " + std::string(name) + " is not accepted");
    }

    // Self-containment: the canonical blob must never reach outside itself.
    for (cgltf_size index = 0; index < data->buffers_count; ++index)
        if (external_uri(data->buffers[index].uri))
            return refuse("buffers must be embedded; external buffer URIs are not accepted");
    for (cgltf_size index = 0; index < data->images_count; ++index) {
        const auto& image = data->images[index];
        if (external_uri(image.uri))
            return refuse("images must be embedded; external image URIs are not accepted");
        if (image.buffer_view != nullptr && image.buffer_view->size > max_image_bytes)
            return refuse("an embedded image is " + std::to_string(image.buffer_view->size) +
                          " bytes; the limit is " + std::to_string(max_image_bytes));
    }

    // Buffer data, needed from here on. Everything above inspects structure
    // only, which is why this gate never loaded it before: the per-mesh joint
    // budget is the first rule that has to read vertices. Safe to do after the
    // self-containment checks above and not before — those refuse external
    // URIs, so nothing here can reach outside the file.
    if (cgltf_load_buffers(&options, data, nullptr) != cgltf_result_success)
        return refuse("the GLB's buffer data could not be read");

    // Rig validation runs before the not-yet-accepted refusal, so a creator
    // preparing content against the published policy learns what is actually
    // wrong with their rig rather than only that rigs are not accepted. Same
    // reasoning that published the skeleton and the limits ahead of M4: a
    // re-rig cannot recover from being aimed wrong, and finding out late costs
    // whoever authored it.
    for (cgltf_size skin_index = 0; skin_index < data->skins_count; ++skin_index) {
        const auto& skin = data->skins[skin_index];
        // Names before the count, deliberately. The viewer's own limit counts
        // *recognized* joints (fslocalmeshimportbase.cpp, enforceRigJointLimit),
        // not the declared list, and a body rigged to another skeleton fails
        // both at once: a MakeHuman export declares 163 joints, none of which
        // are ours. Reporting "163 joints; the limit is 110" would send its
        // author to trim joints when the actual problem is the whole skeleton.
        // Refusing unknown names first makes the two counts identical by the
        // time the limit is applied, so this is also correct by construction
        // rather than by coincidence.
        for (cgltf_size joint_index = 0; joint_index < skin.joints_count; ++joint_index) {
            const auto* node = skin.joints[joint_index];
            const std::string_view name = node != nullptr && node->name != nullptr ? node->name : "";
            if (name.empty())
                return refuse("a skin binds an unnamed joint; every joint must name one of the " +
                              std::to_string(rigged_skeleton_joints) + " " +
                              std::string(rigged_skeleton) + " joints");
            // The name is what a viewer resolves, and it resolves aliases and
            // attachment points as well as canonical bones - so this accepts
            // every spelling a viewer would, including the `hip` and `abdomen`
            // that Blender and Avastar emit. Naming the offending joint matters
            // because the creator hearing it may be several tools away from the
            // file.
            // Retargetable, not merely already-Bento. Since the converter maps
            // a foreign skeleton onto ours (AUTO-RIGGING.md Case 1), the
            // question a gate should ask is whether this rig can be *made* to
            // fit, not whether it arrived fitting. Asking the old question made
            // an imported Character Creator body report itself unwearable while
            // the type-49 a viewer actually fetches was correctly Bento-rigged.
            // A name that only the retarget table places means this is a
            // foreign skeleton being mapped onto ours, not a Bento rig. The
            // distinction decides whether the position check below can say
            // anything: see where `retargeted` is read.
            if (canonical_joint(name).empty() && !retarget_joint(name).empty()) retargeted = true;
            // The converter folds a joint it cannot place into the nearest
            // ancestor it can, so the gate has to do the same or it refuses
            // content the pipeline handles. Character Creator rigs an accessory
            // to a bone named after itself — an earring binds
            // `Earring_Flower_0` — and the converter puts it on the head; a gate
            // that had not walked the tree turned that into a refused upload.
            bool placeable = !retarget_joint(name).empty();
            if (!placeable)
                for (const cgltf_node* up = node->parent; up != nullptr; up = up->parent)
                    if (up->name != nullptr && !retarget_joint(up->name).empty()) {
                        placeable = true;
                        retargeted = true;
                        break;
                    }
            if (!placeable) {
                if (origin == Origin::Upload)
                    return refuse("a skin binds joint \"" + std::string(name) +
                                  "\", which is not a joint of the " +
                                  std::string(rigged_skeleton) + " skeleton");
                // An import: the file carries whatever skeleton its author
                // used, and that is a question rather than an offence
                // (mesh_acceptance.h, Origin::Import). Recorded so the asset
                // can say it is not wearable, and so the names are available
                // when retargeting has something to say about them.
                if (std::find(unresolved.begin(), unresolved.end(), name) == unresolved.end())
                    unresolved.emplace_back(name);
            }
        }
    }
    // The per-mesh budget counts joints a mesh *uses*, not what its skin
    // declares, and the difference is not academic: Blender writes every
    // armature bone into the shared skin whatever each mesh touches, so the
    // Second Life reference body exported through the standard pipeline
    // declares 133 joints and uses 21 — 12 in its largest mesh. Counting the
    // declaration refused the official body, and with it every rig produced the
    // ordinary way.
    //
    // This is also what the viewer counts: enforceRigJointLimit takes
    // recognized_joint_count rather than the declared list.
    for (cgltf_size mesh_index = 0; mesh_index < data->meshes_count; ++mesh_index) {
        const auto& mesh_value = data->meshes[mesh_index];
        std::vector<cgltf_uint> used;
        for (cgltf_size primitive_index = 0; primitive_index < mesh_value.primitives_count;
             ++primitive_index) {
            const auto& primitive = mesh_value.primitives[primitive_index];
            const cgltf_accessor* joints = nullptr;
            const cgltf_accessor* weights = nullptr;
            for (cgltf_size attribute = 0; attribute < primitive.attributes_count; ++attribute) {
                const auto& value = primitive.attributes[attribute];
                if (value.type == cgltf_attribute_type_joints && value.index == 0)
                    joints = value.data;
                if (value.type == cgltf_attribute_type_weights && value.index == 0)
                    weights = value.data;
            }
            if (joints == nullptr || weights == nullptr) continue;
            for (cgltf_size vertex = 0; vertex < joints->count; ++vertex) {
                cgltf_uint slots[4] = {};
                float amounts[4] = {};
                if (!cgltf_accessor_read_uint(joints, vertex, slots, 4) ||
                    !cgltf_accessor_read_float(weights, vertex, amounts, 4))
                    return refuse("a joint or weight accessor is unreadable");
                for (int slot = 0; slot < 4; ++slot) {
                    // A zero weight is padding, not a binding. Counting it
                    // would charge a mesh for joints it does not move.
                    if (amounts[slot] <= 0.0F) continue;
                    if (std::find(used.begin(), used.end(), slots[slot]) == used.end())
                        used.push_back(slots[slot]);
                }
            }
        }
        if (used.size() > max_joints_per_mesh)
            return refuse("a mesh binds " + std::to_string(used.size()) +
                          " joints; the limit is " + std::to_string(max_joints_per_mesh));
    }
    // More than four influences per vertex arrives as a second joint/weight
    // set. glTF numbers them JOINTS_0, JOINTS_1 and so on, four to a set, so
    // any index above zero is a fifth influence by definition.
    for (cgltf_size mesh_index = 0; mesh_index < data->meshes_count; ++mesh_index) {
        const auto& mesh_value = data->meshes[mesh_index];
        for (cgltf_size primitive_index = 0; primitive_index < mesh_value.primitives_count;
             ++primitive_index) {
            const auto& primitive = mesh_value.primitives[primitive_index];
            for (cgltf_size attribute = 0; attribute < primitive.attributes_count; ++attribute) {
                const auto& value = primitive.attributes[attribute];
                if ((value.type == cgltf_attribute_type_joints ||
                     value.type == cgltf_attribute_type_weights) && value.index > 0)
                    return refuse("a primitive declares more than " +
                                  std::to_string(max_rig_influences) +
                                  " influences per vertex");
            }
        }
    }
    if (!rigged_accepted && data->skins_count != 0)
        return refuse("rigged mesh is not accepted yet (ADR 0033 M4); upload the static mesh");
    // Does the skeleton these names describe actually stand where Bento rests
    // it? Nothing above can tell: a joint bound to the wrong target, given that
    // target's inverse bind matrix, produces a correct-looking bind pose,
    // because the same wrong choice writes the matrices that would expose it.
    // So the bind positions are compared against the rest pose, with sign,
    // before the matrices absorb the difference (rig_check.h).
    //
    // Runs last of the rig checks deliberately. A name that does not resolve, a
    // mesh over the joint budget, or a fifth influence are all more specific
    // diagnoses, and a creator is better served by "joint \"root\" is not a
    // joint of this skeleton" than by a list of joints in the wrong place -
    // which is what a body rigged to another skeleton produces once every name
    // has been forced to resolve.
    for (cgltf_size skin_index = 0; skin_index < data->skins_count; ++skin_index) {
        const auto& skin = data->skins[skin_index];
        // Only joints a vertex actually moves. A skin declares every armature
        // bone whatever the mesh touches, and an unused joint's bind matrix is
        // whatever the exporter happened to write - the reference body has two
        // sitting 11 mm from the rest pose, moving nothing. Checking the
        // declared list refuses that body for joints it does not use, which is
        // the same declared-versus-used error the budget check above exists to
        // avoid, made one field over.
        std::vector<bool> moved(skin.joints_count, false);
        for (cgltf_size node_index = 0; node_index < data->nodes_count; ++node_index) {
            const auto& node = data->nodes[node_index];
            if (node.mesh == nullptr || node.skin != &skin) continue;
            for (cgltf_size primitive_index = 0; primitive_index < node.mesh->primitives_count;
                 ++primitive_index) {
                const auto& primitive = node.mesh->primitives[primitive_index];
                const cgltf_accessor* joints = nullptr;
                const cgltf_accessor* weights = nullptr;
                for (cgltf_size attribute = 0; attribute < primitive.attributes_count;
                     ++attribute) {
                    const auto& value = primitive.attributes[attribute];
                    if (value.type == cgltf_attribute_type_joints && value.index == 0)
                        joints = value.data;
                    if (value.type == cgltf_attribute_type_weights && value.index == 0)
                        weights = value.data;
                }
                if (joints == nullptr || weights == nullptr) continue;
                for (cgltf_size vertex = 0; vertex < joints->count; ++vertex) {
                    cgltf_uint slots[4] = {};
                    float amounts[4] = {};
                    if (!cgltf_accessor_read_uint(joints, vertex, slots, 4) ||
                        !cgltf_accessor_read_float(weights, vertex, amounts, 4))
                        return refuse("a joint or weight accessor is unreadable");
                    for (int slot = 0; slot < 4; ++slot)
                        if (amounts[slot] > 0.0F && slots[slot] < moved.size())
                            moved[slots[slot]] = true;
                }
            }
        }
        std::vector<std::string> names;
        std::vector<std::array<float, 16>> inverse_bind;
        names.reserve(skin.joints_count);
        inverse_bind.reserve(skin.joints_count);
        for (cgltf_size joint_index = 0; joint_index < skin.joints_count; ++joint_index) {
            if (!moved[joint_index]) continue;
            const auto* node = skin.joints[joint_index];
            const std::string_view name =
                node != nullptr && node->name != nullptr ? node->name : "";
            names.emplace_back(canonical_joint(name));
            std::array<float, 16> matrix{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            if (skin.inverse_bind_matrices != nullptr &&
                !cgltf_accessor_read_float(skin.inverse_bind_matrices, joint_index,
                                           matrix.data(), 16))
                return refuse("an inverse bind matrix is unreadable");
            // Conjugated into region axes, exactly as the converter will store
            // it, so the gate measures what would be written rather than what
            // arrived.
            inverse_bind.push_back(to_region_axes_matrix(matrix));
        }
        // Two cases this check cannot speak to, skipped rather than softened so
        // that what it does say keeps its full strength.
        //
        // A skeleton that resolved to nothing: every name comes back Unknown,
        // which check_rig reports as Disagrees, and refusing on that would
        // refuse an import for exactly the thing already recorded above.
        //
        // And a *retargeted* skeleton, which is the subtler one. This check asks
        // whether a rig stands where the Bento skeleton rests — a fair question
        // of a body that arrived claiming Bento names, where sitting elsewhere
        // means the names are lying. A Character Creator body claims nothing of
        // the sort: it was mapped onto our skeleton by the converter, it keeps
        // its own proportions on purpose, and it declares them through joint
        // position overrides. Measuring it against Linden's rest pose asks a
        // question it never answered. Aaron's shoulder sits 74 mm from Linden's
        // and his knee 48; both are the body he is, not a fault.
        if (!unresolved.empty() || retargeted) continue;
        const auto finding = check_rig(names, inverse_bind);
        if (finding.outcome == RigOutcome::Disagrees)
            return refuse("a skin's joints do not stand where the " +
                          std::string(rigged_skeleton) + " skeleton rests them: " +
                          std::to_string(finding.disagreed) + " of " +
                          std::to_string(finding.joints.size()) + " disagree, worst " +
                          finding.worst_joint + " by " +
                          std::to_string(static_cast<int>(finding.worst_distance_m * 1000.0F)) +
                          " mm");
        // RigOutcome::Unproven is accepted (mesh_acceptance.h): a body weighted
        // only to positionally-coincident joints is unproven rather than wrong,
        // and refusing on a measurement that could not discriminate would turn a
        // limitation of the check into a rejection of the creator's work.
    }


    // A morph target at zero costs nothing: the base geometry is the intended
    // default and is what the converter emits. A non-zero default weight means
    // the intended shape is the morphed one, and serving the base would be
    // serving a different mesh without saying so.
    if (!nonzero_morph_weights_accepted) {
        const auto declared_nonzero = [](const float* weights, std::size_t count) {
            for (std::size_t index = 0; index < count; ++index)
                if (weights[index] < -1e-6F || weights[index] > 1e-6F) return true;
            return false;
        };
        for (std::size_t index = 0; index < data->meshes_count; ++index)
            if (declared_nonzero(data->meshes[index].weights,
                                 data->meshes[index].weights_count))
                return refuse("a mesh declares a non-zero morph target weight; the"
                              " shape served would be the unmorphed base. Bake the"
                              " morphs into the vertices and export again");
        for (std::size_t index = 0; index < data->nodes_count; ++index)
            if (declared_nonzero(data->nodes[index].weights,
                                 data->nodes[index].weights_count))
                return refuse("a node declares a non-zero morph target weight; the"
                              " shape served would be the unmorphed base. Bake the"
                              " morphs into the vertices and export again");
    }

    Acceptance result;
    result.unresolved_joints = std::move(unresolved);
    result.materials = static_cast<std::uint32_t>(data->materials_count);
    result.textures = static_cast<std::uint32_t>(data->textures_count);
    if (result.materials > max_materials)
        return refuse(std::to_string(result.materials) + " materials; the limit is " +
                      std::to_string(max_materials));
    if (result.textures > max_textures)
        return refuse(std::to_string(result.textures) + " textures; the limit is " +
                      std::to_string(max_textures));

    std::uint64_t triangles = 0;
    for (cgltf_size mesh_index = 0; mesh_index < data->meshes_count; ++mesh_index) {
        const auto& mesh_value = data->meshes[mesh_index];
        for (cgltf_size primitive_index = 0; primitive_index < mesh_value.primitives_count;
             ++primitive_index) {
            const auto& primitive = mesh_value.primitives[primitive_index];
            if (primitive.type != cgltf_primitive_type_triangles)
                return refuse("only triangle primitives are accepted");
            cgltf_size vertices = 0;
            if (primitive.indices != nullptr) {
                vertices = primitive.indices->count;
            } else {
                for (cgltf_size attribute = 0; attribute < primitive.attributes_count; ++attribute) {
                    if (primitive.attributes[attribute].type == cgltf_attribute_type_position) {
                        vertices = primitive.attributes[attribute].data->count;
                        break;
                    }
                }
            }
            triangles += vertices / 3;
        }
    }
    if (triangles == 0)
        return refuse("the file contains no triangles");
    if (triangles > max_triangles)
        return refuse(std::to_string(triangles) + " triangles; the limit is " +
                      std::to_string(max_triangles));
    result.triangles = static_cast<std::uint32_t>(triangles);
    result.accepted = true;
    return result;
}

} // namespace homeworldz::mesh
