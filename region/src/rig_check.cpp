#include "homeworldz/rig_check.h"

#include "homeworldz/avatar_joints.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string_view>

namespace homeworldz::mesh {

// glTF matrices are column-major, so the translation is elements 12..14 and the
// upper-left 3x3 reads down the columns.
bool bind_rest_position(const std::array<float, 16>& m, std::array<float, 3>& translation) {
    const double a = m[0], b = m[4], c = m[8];
    const double d = m[1], e = m[5], f = m[9];
    const double g = m[2], h = m[6], i = m[10];
    const double determinant =
        a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    // A singular basis means the joint has a degenerate bind matrix; there is no
    // position to recover and saying so beats dividing by zero.
    if (std::abs(determinant) < 1e-12) return false;
    const double inv = 1.0 / determinant;
    // Inverse of the 3x3, then t' = -R^-1 * t. Written out rather than looped
    // because a general matrix class here would be one more thing to verify.
    const double r00 = (e * i - f * h) * inv, r01 = (c * h - b * i) * inv,
                 r02 = (b * f - c * e) * inv;
    const double r10 = (f * g - d * i) * inv, r11 = (a * i - c * g) * inv,
                 r12 = (c * d - a * f) * inv;
    const double r20 = (d * h - e * g) * inv, r21 = (b * g - a * h) * inv,
                 r22 = (a * e - b * d) * inv;
    const double tx = m[12], ty = m[13], tz = m[14];
    translation[0] = static_cast<float>(-(r00 * tx + r01 * ty + r02 * tz));
    translation[1] = static_cast<float>(-(r10 * tx + r11 * ty + r12 * tz));
    translation[2] = static_cast<float>(-(r20 * tx + r21 * ty + r22 * tz));
    return true;
}

namespace {

const JointRest* rest_of(std::string_view name) {
    for (const auto& rest : joint_rest_positions)
        if (rest.name == name) return &rest;
    return nullptr;
}

float distance_between(const std::array<float, 3>& lhs, const std::array<float, 3>& rhs) {
    const float dx = lhs[0] - rhs[0], dy = lhs[1] - rhs[1], dz = lhs[2] - rhs[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

RigFinding check_rig(const std::vector<std::string>& joints,
                     const std::vector<std::array<float, 16>>& inverse_bind) {
    RigFinding finding;
    finding.joints.reserve(joints.size());
    for (std::size_t index = 0; index < joints.size(); ++index) {
        JointFinding entry;
        entry.name = joints[index];
        const auto* rest = rest_of(entry.name);
        if (rest == nullptr) {
            entry.verdict = JointVerdict::Unknown;
            ++finding.unknown;
            finding.joints.push_back(std::move(entry));
            continue;
        }
        entry.expected = {rest->x, rest->y, rest->z};

        // Every joint sharing this rest position is an equally good explanation
        // of the same measurement, so collect them before judging.
        for (const auto& other : joint_rest_positions)
            if (other.name != entry.name &&
                distance_between({other.x, other.y, other.z}, entry.expected) <
                    rig_coincidence_m)
                entry.coincident_with.emplace_back(other.name);

        if (index >= inverse_bind.size()) {
            entry.verdict = JointVerdict::Unknown;
            ++finding.unknown;
            finding.joints.push_back(std::move(entry));
            continue;
        }
        std::array<float, 3> observed{};
        if (!bind_rest_position(inverse_bind[index], observed)) {
            entry.verdict = JointVerdict::Unknown;
            ++finding.unknown;
            finding.joints.push_back(std::move(entry));
            continue;
        }
        entry.observed = observed;
        entry.distance_m = distance_between(observed, entry.expected);

        if (entry.distance_m > rig_match_tolerance_m) {
            // Far from its named target - and, since the coincident set shares
            // that target's position, far from every alternative too. Being
            // unable to say *which* joint was meant does not prevent saying that
            // none of them was: a first cut reported these as merely
            // indiscriminate, which let a rig 21 mm from all its candidates pass
            // as an honest tie.
            entry.verdict = JointVerdict::Disagrees;
            ++finding.disagreed;
        } else if (!entry.coincident_with.empty()) {
            // Within tolerance of its target, but equally within tolerance of
            // joints sharing that position, so "agrees" claims more than the
            // measurement supports.
            entry.verdict = JointVerdict::Indiscriminate;
            ++finding.indiscriminate;
        } else if (entry.distance_m <= rig_match_tolerance_m) {
            entry.verdict = JointVerdict::Agrees;
            ++finding.agreed;
        } else {
            entry.verdict = JointVerdict::Disagrees;
            ++finding.disagreed;
        }
        if (entry.distance_m > finding.worst_distance_m &&
            entry.verdict != JointVerdict::Indiscriminate) {
            finding.worst_distance_m = entry.distance_m;
            finding.worst_joint = entry.name;
        }
        finding.joints.push_back(std::move(entry));
    }
    if (finding.disagreed > 0 || finding.unknown > 0)
        finding.outcome = RigOutcome::Disagrees;
    else if (finding.agreed > 0)
        finding.outcome = RigOutcome::Agrees;
    else
        finding.outcome = RigOutcome::Unproven;
    return finding;
}

std::string describe(const RigFinding& finding) {
    std::ostringstream out;
    switch (finding.outcome) {
    case RigOutcome::Agrees: out << "rig agrees with the skeleton"; break;
    case RigOutcome::Disagrees: out << "rig does NOT agree with the skeleton"; break;
    // Said at this length because the short form of it reads as a pass.
    case RigOutcome::Unproven:
        out << "rig is UNPROVEN: nothing disagreed, and nothing could be decided";
        break;
    }
    out << ": " << finding.agreed << " agreed, " << finding.disagreed << " disagreed, "
        << finding.indiscriminate << " indiscriminate, " << finding.unknown << " unknown";
    if (!finding.worst_joint.empty())
        out << "; worst " << finding.worst_joint << " at " << finding.worst_distance_m * 1000.0f
            << " mm";
    out << "\n";
    for (const auto& joint : finding.joints) {
        out << "  ";
        switch (joint.verdict) {
        case JointVerdict::Agrees: out << "agrees        "; break;
        case JointVerdict::Disagrees: out << "DISAGREES     "; break;
        case JointVerdict::Indiscriminate: out << "cannot tell   "; break;
        case JointVerdict::Unknown: out << "unknown       "; break;
        }
        out << joint.name;
        if (joint.verdict != JointVerdict::Unknown)
            out << "  " << joint.distance_m * 1000.0f << " mm";
        // Both positions, because the distance alone cannot distinguish a
        // wrongly-named joint from a correctly-named one in a different pose,
        // and those want opposite responses.
        if (joint.verdict == JointVerdict::Disagrees)
            out << "  observed (" << joint.observed[0] << ", " << joint.observed[1] << ", "
                << joint.observed[2] << ")  expected (" << joint.expected[0] << ", "
                << joint.expected[1] << ", " << joint.expected[2] << ")";
        if (!joint.coincident_with.empty()) {
            out << "  (coincident with";
            for (const auto& other : joint.coincident_with) out << " " << other;
            out << " - position cannot discriminate)";
        }
        out << "\n";
    }
    return out.str();
}

} // namespace homeworldz::mesh
