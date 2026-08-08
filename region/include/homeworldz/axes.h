// The one definition of the glTF/region axis change (ADR 0033 "Coordinates").
//
// glTF is +Y up with X lateral; a region is +Z up with X forward and Y lateral.
// A Y-up to Z-up conversion has two degrees of freedom - which axis becomes up,
// and where the lateral axis goes - and getting the second wrong is a 90 degree
// yaw that stands a model upright and points it sideways. That is not
// hypothetical: it shipped, survived nine days behind a fixture that could not
// fail on yaw, and was found only when a skeleton with a canonical facing
// arrived (2026-08-08).
//
// So it lives in one place. It was previously private to mesh_convert.cpp with a
// hand-copied duplicate in the rig check, on the reasoning that two copies which
// must agree are safer visible than hidden. That reasoning was wrong in the way
// duplication usually is: the copies agreed with each other and both were wrong,
// and a third caller needing the matrix form is what makes the cost obvious.
#ifndef HOMEWORLDZ_AXES_H
#define HOMEWORLDZ_AXES_H

#include <array>

namespace homeworldz::mesh {

// (x, y, z)_glTF -> (z, x, y)_region.
inline void to_region_axes(std::array<float, 3>& value) {
    const float x = value[0], y = value[1], z = value[2];
    value[0] = z;
    value[1] = x;
    value[2] = y;
}

// The inverse, for a derived glTF that must open upright in any tool.
inline void to_gltf_axes(std::array<float, 3>& value) {
    const float x = value[0], y = value[1], z = value[2];
    value[0] = y;
    value[1] = z;
    value[2] = x;
}

// `to_region_axes` as a matrix and its inverse. Column-major, as glTF stores
// them: column c is where the map sends that basis vector.
inline constexpr std::array<float, 16> region_from_gltf{
    0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 1};
inline constexpr std::array<float, 16> gltf_from_region{
    0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1};

inline std::array<float, 16> multiply(const std::array<float, 16>& lhs,
                                      const std::array<float, 16>& rhs) {
    std::array<float, 16> result{};
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += lhs[k * 4 + row] * rhs[column * 4 + k];
            result[column * 4 + row] = sum;
        }
    return result;
}

// An inverse bind matrix maps world space into a joint's local space, so an axis
// change on the world it reads must be undone on the space it writes: the map is
// a **conjugation**, not an application.
//
// Applying `to_region_axes` to the matrix's translation alone is the tempting
// shorthand and is wrong in a way that hides. It puts every joint in the right
// place with its local frame still in glTF orientation, so the body measures
// correctly at rest and rotates about the wrong axes the moment a joint moves.
inline std::array<float, 16> to_region_axes_matrix(const std::array<float, 16>& inverse_bind) {
    return multiply(multiply(region_from_gltf, inverse_bind), gltf_from_region);
}

} // namespace homeworldz::mesh

#endif
