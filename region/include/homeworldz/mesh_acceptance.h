// The mesh acceptance gate of ADR 0033: what this region accepts as an
// uploaded GLB, with the policy itself published to clients rather than
// mirrored by them — an importing client must refuse exactly what upload
// would refuse, and two hand-maintained copies of one policy drift. The
// numbers here are therefore the single definition: the validator enforces
// them and the session hello serves them, from the same symbols.
#ifndef HOMEWORLDZ_MESH_ACCEPTANCE_H
#define HOMEWORLDZ_MESH_ACCEPTANCE_H

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <span>
#include <string>
#include <vector>

namespace homeworldz::mesh {

// A GLB larger than this is refused before parsing. Well under the vault
// blob cap: a mesh source this size is a packaging problem, not content.
inline constexpr std::uint64_t max_glb_bytes = 32ull << 20;
// Visual triangles across every primitive of every mesh in the file.
inline constexpr std::uint32_t max_triangles = 262144;
// Materials per file: matches the per-prim face limit viewers render.
inline constexpr std::uint32_t max_materials = 8;
// Distinct textures per file.
inline constexpr std::uint32_t max_textures = 16;
// Bytes of any single embedded image.
inline constexpr std::uint64_t max_image_bytes = 8ull << 20;
// Bento skinning allows at most this many influences per vertex. Published
// now even though rigged mesh lands with M4 (ADR 0033), so importing clients
// already read it rather than encode it.
inline constexpr std::uint32_t max_rig_influences = 4;
// The skeleton rigged mesh must be weighted to, and how many joints it has.
//
// Published ahead of M4 because it is the one fact a re-rig has to target, and
// getting it wrong is not recoverable by any amount of server work: a viewer uses
// its own skeleton and no other.
//
// **Corrected 2026-08-08, from 71.** The old figure counted the legacy
// pre-Bento skeleton, which is not what any current viewer runs. Firestorm's
// own `character/avatar_skeleton.xml` defines 133 bones and 26 collision
// volumes, and `LLVOAvatar::getJoint(name)` resolves both — a collision volume
// is a legal rig target, not merely a physics shape — so 159 names may appear
// in a skin. Publishing 71 told re-riggers to target a skeleton half the size
// of the real one, and the whole point of publishing it is that a re-rig cannot
// recover from being told the wrong target.
//
// A single mesh may still bind at most `max_joints_per_mesh` of those; that is
// the viewer's own `LL_MAX_JOINTS_PER_MESH_OBJECT`, a per-object budget rather
// than a property of the skeleton, which is why it is a separate number from
// the count above.
//
// glTF binds skin joints by node *index* and never by name, so a client that
// draws arbitrary skeletons is unconstrained while a viewer is not (client core,
// 2026-08-04). The constraint is therefore one-sided, and one body rigged to these
// names serves both families - which is why naming the skeleton is worth more than
// naming a joint budget.
//
// The name is ours and matches nothing: no exporter writes a skeleton identifier
// into a file, and "second-life-avatar", which this held until 2026-08-08, appears
// nowhere in Firestorm. Renamed once it was established that nothing keyed on it,
// while `rigged` is still false and no upload has been validated against it.
// "bento" names the skeleton generation, which is the distinction a re-rig
// actually has to get right; the joint count beside it says the same thing as a
// number. Firestorm's own file calls itself `linden_skeleton` version 2.0 — not
// adopted here, since that is a vendor's name for it rather than a description.
inline constexpr std::string_view rigged_skeleton = "bento-avatar";
inline constexpr std::uint32_t rigged_skeleton_joints = 159;
inline constexpr std::uint32_t max_joints_per_mesh = 110;
// Draco-compressed GLBs are refused in v1 rather than half supported.
inline constexpr bool draco_accepted = false;
// Morph targets (glTF blend shapes) are accepted only while every default weight
// is zero.
//
// At zero the base geometry *is* the intended default appearance, which is
// exactly what the converter emits, so nothing is lost but the ability to
// animate it. With a non-zero default weight the author's intended shape is the
// morphed one and we would serve the base - a different body, silently, at a
// different size.
//
// Two findings behind this, both from the client core (2026-08-05). A glTF can
// be valid, conformant, and carry a shape nobody sees because the shape is in
// morph targets rather than POSITION - which is how two Library "bodies" turned
// out to be one neutral mesh with the gender in a morph weight. And separately:
// an engine's bounds for a mesh with blend shapes are inflated by the full
// displacement a morph could reach *while it sits at zero*, proven on three
// one-metre cubes where the morphed one measured 3 m tall and its vertices
// measured 1 m. Skinning does not do this; morphs do. `declared_world_bounds`
// is safe because it reads accessor min/max rather than asking an engine, and
// that is now a property worth keeping deliberately rather than by luck.
inline constexpr bool nonzero_morph_weights_accepted = false;
// Rigged mesh (skins) is accepted as of 2026-08-08. Refusing was honest while
// the mapping was a guess; it is now measured. A skin's joint names resolve
// through the skeleton's own alias table or the upload is refused naming the
// joint, unused joints compact away, and the bind geometry is checked against
// the skeleton's rest pose with a tolerance bracketed by the skeleton itself
// (rig_check.h).
//
// Turned on together with the two corrections that made it worth turning on:
// the axis map's yaw, and the inverse bind matrices being conjugated rather than
// ignored. Accepting before those would have taken uploads into a conversion
// known to be wrong in two specific ways, which is not a test.
//
// A rig whose joints are all positionally coincident (RigOutcome::Unproven) is
// accepted, decided 2026-08-08 and provisional pending real-world use
// (rig_check.h).
//
// **Corrected 2026-08-10.** This paragraph used to end "it gates nothing yet
// regardless: the geometric check is not wired into validate_glb, so this path
// still accepts on names, joint counts and influence sets alone." That is false
// and was false when read: check_rig *is* called here and RigOutcome::Disagrees
// *is* a refusal. The note survived the change that wired it in, and it is the
// worse kind of stale — it describes a safety net as disconnected when it is
// connected, which invites exactly the "just add the aliases" shortcut it was
// written to warn against.
//
// max_rig_influences is enforced and unit-tested, and no natural file will ever
// trip it - which is a property of the number rather than a gap in coverage.
// glTF carries four influences per JOINTS_n set by convention, and no exporter
// emits a JOINTS_1 without deliberate authoring, so four *is* the default an
// ordinary export produces (client core, 2026-08-08, measured across three
// rigged bodies including the reference). A conforming file therefore cannot
// exceed the limit by accident, and "no upload has exercised it" is evidence of
// nothing. The synthetic fixture asserting the refusal proves the branch runs
// and cannot tell us real content will ever reach it.
inline constexpr bool rigged_accepted = true;

// The glTF extensions this gate accepts. Anything else — used or required —
// is refused, not ignored, so content never renders differently on the
// client that understands more.
inline constexpr std::string_view allowed_extensions[] = {
    "KHR_materials_emissive_strength",
    "KHR_materials_ior",
    "KHR_materials_specular",
    "KHR_materials_unlit",
    "KHR_texture_transform",
};

// Where a session client uploads: POST, body is the mesh, authorized by the
// same region ticket the WebSocket authenticates with, as a bearer token —
// one credential, both transports.
inline constexpr std::string_view upload_path = "/session/uploads/mesh";

// Source formats this server imports in addition to GLB (ADR 0035), by the
// extension a creator knows them by. Published rather than mirrored, for the
// reason the rest of this policy is: a client that guesses the accepted set is
// a second copy of it.
//
// The limits above apply to what an import *produces*, not to the file that
// arrives. One source file becomes one asset per mesh, so a Character Creator
// body — 17 materials and 30 textures taken whole, over three of these limits —
// imports as six assets each of which is comfortably inside them.
inline constexpr std::string_view imported_formats[] = {"fbx"};
// A source file may be larger than a GLB of the same content, because it
// carries its textures uncompressed by any container and, for FBX, often
// embeds them. Character Creator bodies measure 19-41 MB.
inline constexpr std::uint64_t max_source_bytes = 96ull << 20;

// The acceptance policy as the JSON object served in the session hello
// (the read-never-encode contract of ADR 0033).
std::string acceptance_policy_json();

// Where the bytes being gated came from. The rules are the same either way with
// exactly one exception, stated here rather than in a second copy of the gate.
enum class Origin {
    // A creator's own GLB, arriving at the upload capability. Gated in full.
    Upload,
    // A part produced by importing a source file (ADR 0035). Everything the
    // gate protects still applies — geometry, textures, extensions, sizes,
    // morph weights — because import must not be a side door into the asset
    // store, which that ADR says in as many words.
    //
    // The one difference is the rig, and it is a difference of *question*
    // rather than of strictness. An FBX carries whatever skeleton its author
    // used, and Character Creator's `CC_Base_*` resolves to nothing here. For
    // an upload that is a refusal, because the creator chose to send a rig
    // claiming to be ours. For an import it is simply an unanswered question:
    // the geometry and textures are good and useful now, and whether the body
    // can be *worn* waits on retargeting — which is exactly the position ADR
    // 0035 takes ("an imported static mesh is useful immediately").
    //
    // So an unresolved skeleton is recorded in `unresolved_joints` instead of
    // refused. A rig whose names *do* resolve is still checked against the
    // skeleton's rest pose and still refused when it disagrees: that one is a
    // rig claiming to be ours and misplacing itself, which is the mirrored-rig
    // hazard rig_check.h exists for, and importing it would ship it broken.
    Import,
};

struct Acceptance {
    bool accepted{};
    // Actionable when refused: names the rule and the offending value, since
    // the creator hearing this may be several tools away from the file.
    std::string reason;
    std::uint32_t triangles{};
    std::uint32_t materials{};
    std::uint32_t textures{};
    // Joint names that resolved to nothing in the skeleton. Always empty for an
    // accepted upload, since one is refused; populated for an accepted import,
    // where it is the record that the rig question was asked and not answered.
    // A non-empty list means the asset is not wearable yet.
    std::vector<std::string> unresolved_joints;
};

// validate_glb applies the gate to an uploaded GLB: container and version,
// self-containment (no external buffer or image URIs), the extension
// allowlist, and the caps above.
Acceptance validate_glb(std::span<const std::byte> content,
                        Origin origin = Origin::Upload);

} // namespace homeworldz::mesh

#endif
