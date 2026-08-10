// The correspondence table of AUTO-RIGGING.md Case 1, and the counting that
// decides whether a body fits the per-mesh joint budget.
//
// The most valuable case here is the last one, and it is not about behaviour: it
// checks that every *target* in the table is a joint the skeleton actually has.
// A typo there does not fail — `retarget_joint` returns the misspelling, the
// converter writes it, and the viewer looks for a joint of that name, finds
// none, and silently drops both the binding and its position override. The body
// arrives with a limb that does not move and nothing anywhere says why.
#include "homeworldz/avatar_joints.h"
#include "homeworldz/rig_retarget.h"

#include <set>
#include <string>
#include <vector>

int main() {
    using namespace homeworldz::mesh;

    // A rig that already speaks Bento is not retargeted; alias resolution wins.
    if (retarget_joint("mPelvis") != "mPelvis") return 1;
    // Including the spellings other tools emit, which resolve through the
    // skeleton's own alias table rather than through this one.
    if (retarget_joint("hip") != "mPelvis") return 2;
    if (retarget_joint("abdomen") != "mTorso") return 3;

    // Character Creator names map.
    if (retarget_joint("CC_Base_Pelvis") != "mPelvis") return 4;
    if (retarget_joint("CC_Base_Head") != "mHead") return 5;
    if (retarget_joint("CC_Base_L_Hand") != "mWristLeft") return 6;

    // The folds that make it many-to-one. CC skins to twist bones, so both
    // halves of the upper arm must land on the same Bento joint or their
    // weights compete instead of summing.
    if (retarget_joint("CC_Base_L_UpperarmTwist01") != "mShoulderLeft") return 7;
    if (retarget_joint("CC_Base_L_UpperarmTwist02") != "mShoulderLeft") return 8;
    // And all five toes onto the one toe joint Bento has.
    for (const auto* toe : {"CC_Base_R_BigToe1", "CC_Base_R_IndexToe1", "CC_Base_R_MidToe1",
                            "CC_Base_R_RingToe1", "CC_Base_R_PinkyToe1"})
        if (retarget_joint(toe) != "mToeRight") return 9;

    // Left stays left. A mirrored correspondence produces a body whose bind
    // pose looks right and whose every animation is inside out, which is the
    // failure rig_check.h exists to catch and which is cheaper to prevent here.
    if (retarget_joint("CC_Base_L_Clavicle") != "mCollarLeft") return 10;
    if (retarget_joint("CC_Base_R_Clavicle") != "mCollarRight") return 11;
    if (retarget_joint("CC_Base_L_ThighTwist01") != "mHipLeft") return 12;
    if (retarget_joint("CC_Base_R_ThighTwist01") != "mHipRight") return 13;

    // A name from no skeleton we know maps to nothing, rather than to something
    // plausible-looking.
    if (!retarget_joint("Bip01_Spine").empty()) return 14;
    if (!retarget_joint("").empty()) return 15;

    // Counting: the merge is reported, because it is the lossy part.
    {
        const std::vector<std::string> sources{
            "CC_Base_L_UpperarmTwist01", "CC_Base_L_UpperarmTwist02", "CC_Base_Head"};
        const auto finding = describe_retarget(sources);
        if (finding.mapped != 3) return 16;
        if (finding.targets != 2) return 17;   // mShoulderLeft, mHead
        if (finding.merged != 1) return 18;    // the second twist bone
        if (!finding.unmapped.empty()) return 19;
    }
    {
        const std::vector<std::string> sources{"CC_Base_Head", "Bip01_Spine"};
        const auto finding = describe_retarget(sources);
        if (finding.mapped != 1) return 20;
        if (finding.unmapped.size() != 1 || finding.unmapped.front() != "Bip01_Spine") return 21;
    }

    // Every target names a joint the skeleton resolves, and every source is
    // listed once. Both are properties of the table rather than of the code,
    // and both fail silently in a viewer rather than loudly here.
    {
        std::set<std::string_view> sources;
        for (const auto& entry : character_creator_to_bento) {
            if (!is_riggable_joint(entry.target)) return 22;
            if (!sources.insert(entry.source).second) return 23;
            // A source that the alias table already resolves would never reach
            // this table, since retarget_joint consults aliases first — an
            // entry like that is dead and means the map was built against the
            // wrong assumption.
            if (!canonical_joint(entry.source).empty()) return 24;
        }
    }

    // The skeleton root weights to the pelvis but must never say where the
    // pelvis is. It rests at the character's ground origin, so letting it
    // answer puts the pelvis at the feet and stretches everything bound above
    // it — and a belt and a pair of boots in one export both bind it, so this
    // is reached by ordinary content rather than by a contrived rig.
    {
        if (retarget_joint("CC_Base_BoneRoot") != "mPelvis") return 25;
        if (retarget_supplies_position("CC_Base_BoneRoot")) return 26;
        // The bones that do carry the pelvis still answer for it, or the rule
        // above would leave the joint with no position at all.
        if (!retarget_supplies_position("CC_Base_Pelvis")) return 27;
        if (!retarget_supplies_position("CC_Base_Hip")) return 28;
        // Anything unlisted, and anything the skeleton resolves itself, speaks
        // for its own position: this only ever subtracts from the table.
        if (!retarget_supplies_position("mPelvis")) return 29;
        if (!retarget_supplies_position("CC_Base_L_CalfTwist01")) return 30;
        // Exactly one exception today. A second added without thought would
        // silently drop a joint's position on every body that binds it.
        int barred = 0;
        for (const auto& entry : character_creator_to_bento)
            if (!entry.supplies_position) ++barred;
        if (barred != 1) return 31;
    }

    return 0;
}
