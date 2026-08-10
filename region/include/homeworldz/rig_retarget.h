// Mapping a foreign humanoid rig onto the Bento skeleton (AUTO-RIGGING.md,
// Case 1 — "the one worth solving").
//
// A Character Creator body is already skinned: it has joints, weights and a bind
// pose. What it lacks is Bento's joint *names* and Bento's rest pose. The
// weights are good; the problem is correspondence, which is why this case is
// tractable where rigging an unrigged mesh is not.
//
// Two things decide the shape of this file.
//
// **Source rigs bind to bones that Bento does not have.** Character Creator
// skins to its *twist* bones rather than the limb bones — a CC body has no
// `Upperarm` in its skin at all, only `UpperarmTwist01` and `UpperarmTwist02`.
// So correspondence is many-to-one and must merge weights, not rename them. The
// same is true of `ShareBone` helpers at the elbow and knee, and of the five
// separate toe bones per foot where Bento has one.
//
// **The bind pose must be carried, not flattened.** The obvious move — shift the
// geometry until the source skeleton sits where Bento rests — is wrong, and
// wrong in the way that matters: it would make every imported body Linden-shaped
// and throw away the proportions the creator built. Second Life already solves
// this with per-body joint position overrides, which is exactly what a mesh body
// uses to not be Linden-shaped. A retarget therefore *writes* the source's own
// joint positions and leaves the mesh alone.
#ifndef HOMEWORLDZ_RIG_RETARGET_H
#define HOMEWORLDZ_RIG_RETARGET_H

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace homeworldz::mesh {

// One source joint and the Bento joint it becomes. Several sources may name the
// same target; their weights add.
struct JointCorrespondence {
    std::string_view source;
    std::string_view target;
};

// Character Creator (CC3/CC4/CC5) to Bento.
//
// Measured against CC5-Kevin and the CC3 base bodies: their skins bind 85
// distinct joints, and every one of them appears here. That is the measurement
// AUTO-RIGGING.md asked for and could not assume — "the alias table may cover
// more of CC/Mixamo than expected, or almost none".
//
// The folds are deliberate and each loses nothing a viewer can show:
//
//   *Twist01/02   -> the limb they twist. Bento has no twist joints, and their
//                    whole purpose is distributing a roll the viewer applies to
//                    the parent anyway.
//   *ShareBone    -> the joint they share. Helper bones for elbow and knee
//                    creasing, with no Bento equivalent.
//   toes          -> mToe*. Bento has one toe joint per foot; CC has five.
//   Spine01       -> mTorso, with Waist. CC's four-segment spine is more
//                    articulated than Bento's two, so the lower two merge.
//   NeckTwist01/02-> mNeck.
//
// Breasts map to the pectoral *collision volumes*, which are legal rig targets
// (avatar_joints.h: findJoint walks collision volumes too), not to bones.
inline constexpr JointCorrespondence character_creator_to_bento[] = {
    // Spine and head
    {"CC_Base_Pelvis", "mPelvis"},
    {"CC_Base_Waist", "mTorso"},
    {"CC_Base_Spine01", "mTorso"},
    {"CC_Base_Spine02", "mChest"},
    {"CC_Base_L_RibsTwist", "mChest"},
    {"CC_Base_R_RibsTwist", "mChest"},
    {"CC_Base_NeckTwist01", "mNeck"},
    {"CC_Base_NeckTwist02", "mNeck"},
    {"CC_Base_Head", "mHead"},
    {"CC_Base_JawRoot", "mFaceJaw"},
    {"CC_Base_L_Eye", "mEyeLeft"},
    {"CC_Base_R_Eye", "mEyeRight"},
    {"CC_Base_Teeth01", "mFaceTeethUpper"},
    {"CC_Base_Teeth02", "mFaceTeethLower"},
    {"CC_Base_Tongue01", "mFaceTongueBase"},
    {"CC_Base_Tongue02", "mFaceTongueBase"},
    {"CC_Base_Tongue03", "mFaceTongueTip"},
    {"CC_Base_L_Breast", "LEFT_PEC"},
    {"CC_Base_R_Breast", "RIGHT_PEC"},

    // Arms. CC skins to the twist bones; the limb bones carry no weight.
    {"CC_Base_L_Clavicle", "mCollarLeft"},
    {"CC_Base_R_Clavicle", "mCollarRight"},
    {"CC_Base_L_UpperarmTwist01", "mShoulderLeft"},
    {"CC_Base_L_UpperarmTwist02", "mShoulderLeft"},
    {"CC_Base_R_UpperarmTwist01", "mShoulderRight"},
    {"CC_Base_R_UpperarmTwist02", "mShoulderRight"},
    {"CC_Base_L_ElbowShareBone", "mElbowLeft"},
    {"CC_Base_R_ElbowShareBone", "mElbowRight"},
    {"CC_Base_L_ForearmTwist01", "mElbowLeft"},
    {"CC_Base_L_ForearmTwist02", "mElbowLeft"},
    {"CC_Base_R_ForearmTwist01", "mElbowRight"},
    {"CC_Base_R_ForearmTwist02", "mElbowRight"},
    {"CC_Base_L_Hand", "mWristLeft"},
    {"CC_Base_R_Hand", "mWristRight"},

    // Fingers map one to one; Bento's hand is as articulated as CC's.
    {"CC_Base_L_Thumb1", "mHandThumb1Left"},
    {"CC_Base_L_Thumb2", "mHandThumb2Left"},
    {"CC_Base_L_Thumb3", "mHandThumb3Left"},
    {"CC_Base_L_Index1", "mHandIndex1Left"},
    {"CC_Base_L_Index2", "mHandIndex2Left"},
    {"CC_Base_L_Index3", "mHandIndex3Left"},
    {"CC_Base_L_Mid1", "mHandMiddle1Left"},
    {"CC_Base_L_Mid2", "mHandMiddle2Left"},
    {"CC_Base_L_Mid3", "mHandMiddle3Left"},
    {"CC_Base_L_Ring1", "mHandRing1Left"},
    {"CC_Base_L_Ring2", "mHandRing2Left"},
    {"CC_Base_L_Ring3", "mHandRing3Left"},
    {"CC_Base_L_Pinky1", "mHandPinky1Left"},
    {"CC_Base_L_Pinky2", "mHandPinky2Left"},
    {"CC_Base_L_Pinky3", "mHandPinky3Left"},
    {"CC_Base_R_Thumb1", "mHandThumb1Right"},
    {"CC_Base_R_Thumb2", "mHandThumb2Right"},
    {"CC_Base_R_Thumb3", "mHandThumb3Right"},
    {"CC_Base_R_Index1", "mHandIndex1Right"},
    {"CC_Base_R_Index2", "mHandIndex2Right"},
    {"CC_Base_R_Index3", "mHandIndex3Right"},
    {"CC_Base_R_Mid1", "mHandMiddle1Right"},
    {"CC_Base_R_Mid2", "mHandMiddle2Right"},
    {"CC_Base_R_Mid3", "mHandMiddle3Right"},
    {"CC_Base_R_Ring1", "mHandRing1Right"},
    {"CC_Base_R_Ring2", "mHandRing2Right"},
    {"CC_Base_R_Ring3", "mHandRing3Right"},
    {"CC_Base_R_Pinky1", "mHandPinky1Right"},
    {"CC_Base_R_Pinky2", "mHandPinky2Right"},
    {"CC_Base_R_Pinky3", "mHandPinky3Right"},

    // Legs, again skinned to the twist bones.
    {"CC_Base_L_ThighTwist01", "mHipLeft"},
    {"CC_Base_L_ThighTwist02", "mHipLeft"},
    {"CC_Base_R_ThighTwist01", "mHipRight"},
    {"CC_Base_R_ThighTwist02", "mHipRight"},
    {"CC_Base_L_KneeShareBone", "mKneeLeft"},
    {"CC_Base_R_KneeShareBone", "mKneeRight"},
    {"CC_Base_L_CalfTwist01", "mKneeLeft"},
    {"CC_Base_L_CalfTwist02", "mKneeLeft"},
    {"CC_Base_R_CalfTwist01", "mKneeRight"},
    {"CC_Base_R_CalfTwist02", "mKneeRight"},
    {"CC_Base_L_Foot", "mAnkleLeft"},
    {"CC_Base_R_Foot", "mAnkleRight"},
    {"CC_Base_L_BigToe1", "mToeLeft"},
    {"CC_Base_L_IndexToe1", "mToeLeft"},
    {"CC_Base_L_MidToe1", "mToeLeft"},
    {"CC_Base_L_RingToe1", "mToeLeft"},
    {"CC_Base_L_PinkyToe1", "mToeLeft"},
    {"CC_Base_R_BigToe1", "mToeRight"},
    {"CC_Base_R_IndexToe1", "mToeRight"},
    {"CC_Base_R_MidToe1", "mToeRight"},
    {"CC_Base_R_RingToe1", "mToeRight"},
    {"CC_Base_R_PinkyToe1", "mToeRight"},
};

// The Bento joint a source name becomes, or empty when nothing corresponds.
// Consults the correspondence table first and then the skeleton's own alias
// resolution, so a rig that already speaks Bento passes through unchanged.
std::string_view retarget_joint(std::string_view source);

struct RetargetFinding {
    // Source joints that found a target, and those that did not.
    std::uint32_t mapped{};
    std::vector<std::string> unmapped;
    // Distinct Bento joints the result binds, which is what the per-mesh budget
    // counts (max_joints_per_mesh) and what a viewer calls "recognized".
    std::uint32_t targets{};
    // Source joints that merged into a target another source also claimed.
    std::uint32_t merged{};
};

// What `sources` would become, without transforming anything. For the
// diagnostic and for deciding whether a body is worth retargeting at all.
RetargetFinding describe_retarget(const std::vector<std::string>& sources);

} // namespace homeworldz::mesh

#endif
