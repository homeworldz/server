// Does a rigged mesh's skeleton actually stand where the Bento skeleton rests?
//
// A joint bound to the wrong target, given that target's inverse bind matrix,
// produces a correct-looking bind pose: the same wrong choice writes the
// matrices that would otherwise reveal it. So the source's own bind position is
// compared against where the skeleton rests the joint its *name* resolved to,
// with sign, before the matrices can absorb the difference.
//
// Sign matters and is the whole point. A mirrored rig - left joints carrying
// right positions - produces the correct *distances* between joints and the
// correct pose; only comparing each joint to its own named target with sign
// catches it. Comparing to the nearest joint, or comparing magnitudes, agrees
// with a mirrored skeleton.
#ifndef HOMEWORLDZ_RIG_CHECK_H
#define HOMEWORLDZ_RIG_CHECK_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace homeworldz::mesh {

// How far a joint may sit from its rest position and still be called a match.
//
// The bracket this must fall inside is a property of the skeleton, measured
// rather than chosen (client core, 2026-08-08):
//
//   2.00 mm  the natural left/right asymmetry of the skeleton's own leg chain.
//            A threshold at or below this calls a *correct* rig wrong.
//   7.81 mm  mFaceLipLowerCenter to mFaceTongueBase, the closest pair of joints
//            that are distinct at all. A threshold at or above this cannot tell
//            those two apart, so a mesh weighted to one and claiming the other
//            passes.
//
// 5 mm sits between them with room on both sides. The first calibration
// attempted here used ~1.2 m - the distance a mirrored rig displaces a hand -
// which is four orders of magnitude too coarse and would have accepted almost
// any skeleton whose overall proportions were human.
inline constexpr float rig_match_tolerance_m = 0.005f;

// Two joints closer together than this cannot be told apart by position at all,
// so a mesh weighted to either is geometrically identical. The skeleton has 26
// such pairs sitting at *exactly* zero distance - mChest and mSpine3 share a
// position to the micrometre, as do each eye and its three face-joint
// neighbours - covering 27 of the 159 joints. For those, "agrees" would be a
// statement the measurement cannot support, so the check reports that it cannot
// discriminate and names the alternatives instead of guessing.
inline constexpr float rig_coincidence_m = 0.0005f;

// Where an inverse bind matrix says its joint rests: invert the affine and read
// the translation. Shared rather than copied — the converter needs the same
// answer the check does, and the axes note in axes.h records what the last
// duplicated matrix helper cost.
//
// False for a singular basis: a degenerate bind matrix has no position to
// recover, and saying so beats dividing by zero.
bool bind_rest_position(const std::array<float, 16>& inverse_bind,
                        std::array<float, 3>& rest);

enum class JointVerdict {
    Agrees,          // within tolerance of the joint its name resolved to
    Disagrees,       // outside tolerance: the name and the position disagree
    Indiscriminate,  // coincident with another joint; position proves nothing
    Unknown,         // no rest position on record for this name
};

struct JointFinding {
    std::string name;
    JointVerdict verdict{JointVerdict::Unknown};
    float distance_m{};                  // from the *named* target, not the nearest
    std::array<float, 3> expected{};
    std::array<float, 3> observed{};
    std::vector<std::string> coincident_with;
};

// The whole-body result, which needs the same three outcomes the per-joint
// verdict does.
//
// A first version reported a bool: "nothing disagreed". That is a fact about the
// check, not a claim about the body, and the two come apart exactly where it
// matters - a body weighted only to positionally-coincident joints produced
// `true` with nothing having been decided, and that `true` was indistinguishable
// from a body checked thoroughly. Unproven is not a weaker kind of pass; it is
// the third outcome one level up (client core, 2026-08-08).
//
// **Unproven is accepted, decided 2026-08-08, "for now, pending more testing and
// real-world use."** A body whose joints are all positionally coincident is
// unproven rather than wrong, and refusing content on the strength of a
// measurement that could not discriminate would turn a limitation of the check
// into a rejection of the creator's work.
//
// Recorded as provisional because it is the kind of decision that should be
// revisited against real uploads rather than settled once: if bodies that turn
// out to be broken keep arriving as Unproven, the coverage rule earns its keep;
// if none do, it never needed one. Nothing yet distinguishes those futures.
//
// **Corrected 2026-08-10.** This said "note this gates nothing today either way.
// check_rig runs in the diagnostic tool and is *not* wired into validate_glb, so
// the upload path still accepts on names, joint counts and influence sets
// alone." It was accurate when written and stopped being so when the wiring
// landed; validate_glb calls check_rig and refuses on Disagrees. The decision
// above therefore takes effect today rather than pending a later change.
//
// A stale note that under-states a check is worse than one that overstates it:
// this one described the position safety net as disconnected, which is precisely
// the reading that makes mapping a foreign skeleton onto ours by name alone look
// harmless.
enum class RigOutcome {
    Agrees,     // at least one joint was decided, and none disagreed
    Disagrees,  // at least one joint disagreed, or could not be read at all
    Unproven,   // nothing disagreed, and nothing was actually decided
};

struct RigFinding {
    RigOutcome outcome{RigOutcome::Unproven};
    std::uint32_t agreed{};
    std::uint32_t disagreed{};
    std::uint32_t indiscriminate{};
    std::uint32_t unknown{};
    float worst_distance_m{};
    std::string worst_joint;
    std::vector<JointFinding> joints;
};

// `inverse_bind` are the inverse bind matrices as the *converted asset* holds
// them: column-major and already in region axes, because mesh_convert conjugates
// them on ingest alongside the geometry.
//
// This deliberately checks what was written rather than what arrived. An earlier
// version took glTF-frame matrices and applied the axis map itself, which meant
// it measured the converter's intent instead of its output — and could not have
// seen the converter failing to map the matrices at all, which it was.
RigFinding check_rig(const std::vector<std::string>& joints,
                     const std::vector<std::array<float, 16>>& inverse_bind);

// One line per joint, for the diagnostic tool and refusal messages.
std::string describe(const RigFinding& finding);

} // namespace homeworldz::mesh

#endif
