// The Second Life mesh asset format (type 49): a binary-LLSD header naming
// zlib-compressed LOD and physics blocks. This is the `sl-mesh` rendition of
// ADR 0033 — derived from the canonical GLB by the conversion worker, served
// to viewers, never touched by the modern client.
//
// The module carries both the writer and a reader: the reader is what makes
// the writer testable as a round trip today, and it is the parser the M2
// direction (deriving gltf from a Firestorm-uploaded sl-mesh) needs anyway.
//
// Geometry constraints mirror the wire format: positions and texture
// coordinates quantize to 16-bit values over a published domain, normals over
// the fixed [-1, 1] domain, and indices are 16-bit — so a submesh holds at
// most 65,535 vertices, one submesh per material face.
#ifndef HOMEWORLDZ_SLMESH_H
#define HOMEWORLDZ_SLMESH_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <span>
#include <string>
#include <vector>

namespace homeworldz::slmesh {

// One vertex's binding to one joint. The wire form is a byte index into the
// asset's joint table and a 16-bit fixed-point weight, so a joint index above
// 254 cannot be expressed at all — 255 is the end-of-list marker.
struct Influence {
    std::uint8_t joint{};
    float weight{};
};

struct Submesh {
    std::vector<std::array<float, 3>> positions;
    // Empty means "not carried"; when present, sized like positions.
    std::vector<std::array<float, 3>> normals;
    std::vector<std::array<float, 2>> texcoords;
    // Triangle list, three indices per triangle, into this submesh's vertices.
    std::vector<std::uint16_t> indices;
    // Empty means unrigged; when present, sized like positions. At most four
    // per vertex, which is what the format and the viewer both allow.
    std::vector<std::vector<Influence>> influences;
};

// The `skin` block of a rigged mesh: which joints it binds and how its bind
// pose relates to the skeleton's.
//
// `joints` names them — the same names a viewer resolves, so aliases and
// attachment points are legal here as well as canonical bones. Each entry has
// one inverse bind matrix, row-major, sixteen values in the order the format
// writes them (row * 4 + column).
//
// `alternate_inverse_bind` is the joint position override: a body whose
// proportions differ from the default skeleton ships its own joint positions
// here rather than deforming wrongly. Empty means "use the skeleton's", which
// is what a body rigged to the standard proportions wants.
// Matrices here are glTF's flat 16 floats — column-major, column vector,
// translation at 12..14 — and they are written to the asset unchanged.
//
// That looks wrong and is not, which is why it is written down. The viewer
// reads them row-major into a row-vector LLMatrix4 (llmodel.cpp:
// mMatrix[j][k] = flat[j*4+k]). Those two conventions are duals: both compute
// result[i] = sum_k flat[k*4+i] * p[k] + flat[12+i]. Transposing on the way out
// to "correct" the convention introduces exactly the defect it looks like it is
// preventing. A test in slmesh_test asserts the duality; it was written while
// making that mistake, and it caught it.
struct Skin {
    std::vector<std::string> joints;
    std::vector<std::array<float, 16>> inverse_bind;
    std::array<float, 16> bind_shape{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::vector<std::array<float, 16>> alternate_inverse_bind;
    float pelvis_offset{};
    bool lock_scale_if_joint_position{};
};

// One level of detail: a submesh per material face, in material order. The
// order must be identical across levels — the viewer matches faces by index.
using Level = std::vector<Submesh>;

struct Mesh {
    // high is mandatory; the others fall back to the next-present level when
    // a serializer chooses to omit them. This converter always writes all
    // four.
    Level high;
    Level medium;
    Level low;
    Level lowest;
    // The convex physics hull's vertices (a single hull, the format's
    // "BoundingVerts" shape).
    std::vector<std::array<float, 3>> physics_hull;
    // Present only for a rigged mesh, and then it is the asset-wide joint
    // table every submesh's influences index into.
    std::optional<Skin> skin;
};

// serialize renders the asset bytes: binary-LLSD header, then the compressed
// blocks the header's offsets name. Empty on invalid input (no high level, a
// submesh with no triangles, or an index out of range).
std::vector<std::byte> serialize(const Mesh& mesh);

// parse reads asset bytes back into the model, quantization applied — a
// round trip through serialize/parse reproduces geometry to quantization
// precision. nullopt when the bytes are not a well-formed mesh asset.
std::optional<Mesh> parse(std::span<const std::byte> content);

} // namespace homeworldz::slmesh

#endif
