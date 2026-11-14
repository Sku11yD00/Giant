// Triangle meshes as plain data, and the built-in shapes. Front faces wind
// counter-clockwise seen from outside (the rhi's convention). Metres, +Y up.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace giant::geometry {

// A texture coordinate: (0, 0) is the image's top-left corner, v runs down
// (glTF's and D3D's convention). Repeats past 0..1.
struct TexCoord {
    float u = 0.0f;
    float v = 0.0f;
    constexpr bool operator==(const TexCoord&) const = default;
};

// The tangent frame maps a normal map onto the surface, as glTF defines it:
// `tangent` points along +u, the bitangent is bitangent_sign *
// cross(normal, tangent) and points up the image (toward smaller v), so
// green-up normal maps read right. Meaningful once generate_tangents (or a
// file) set it; the GPU form keeps it perpendicular to the normal whatever
// it holds.
struct Vertex {
    Vec3 position;
    Vec3 normal; // unit
    TexCoord uv{};
    Vec3 tangent{1.0f, 0.0f, 0.0f}; // unit
    float bitangent_sign = 1.0f;    // +1 or -1 (mirrored texture mapping)
};
static_assert(sizeof(Vertex) == 48, "the simplifier reads the normal and uv as 5 floats in a row");

// A vertex as the GPU reads it (k_packed_vertex_bytes, the geometry pool's
// layout, engine/render/shaders/common.slang): the position as floats, the
// normal and tangent as octahedral snorm16 pairs (the tangent's lowest y bit
// holding the bitangent sign: 1 for -1), the uv as floats.
struct PackedVertex {
    float position[3];
    u32 normal;
    u32 tangent;
    float uv[2];
};
inline constexpr u32 k_packed_vertex_bytes = 28;
static_assert(sizeof(PackedVertex) == k_packed_vertex_bytes);

// A unit vector as two snorm16 octahedral coordinates (x low, y high);
// about 0.003 degrees apart. Zero or non-finite vectors pack as +Z.
u32 pack_octahedral(const Vec3& unit);
Vec3 unpack_octahedral(u32 packed);
// The tangent is first made perpendicular to the normal (any perpendicular
// when it is parallel or zero); a non-unit normal is normalised (+Y if zero).
PackedVertex pack_vertex(const Vertex& vertex);
Vertex unpack_vertex(const PackedVertex& packed);

struct TriangleMesh {
    std::vector<Vertex> vertices;
    std::vector<u32> indices; // triangles
};

// Which joints move a vertex, and how much (in 255ths, summing to 255;
// unused slots weigh 0).
struct SkinWeights {
    std::array<u16, 4> joints{};
    std::array<u8, 4> weights{};
    bool operator==(const SkinWeights&) const = default;
};

// One vertex's move under a morph target (blend shape) at full weight:
// added to its position, and to its normal (renormalised after).
struct MorphDelta {
    u32 vertex = 0;
    Vec3 position{};
    Vec3 normal{};
};

// A morph target: the vertices it moves (sparse, in increasing vertex
// order, each once), its name (unique in its mesh) and the weight the mesh
// rests at (glTF's default weights).
struct MorphTarget {
    std::string name;
    float weight = 0.0f;
    std::vector<MorphDelta> deltas;
};
inline constexpr u32 k_max_morph_targets = 256;     // per mesh
inline constexpr usize k_max_morph_name_bytes = 128; // UTF-8

// A mesh in its bind space (giant/anim/skeleton.h), one weight per vertex,
// and its morph targets (deltas in bind space, applied before skinning).
struct SkinnedTriangleMesh {
    TriangleMesh mesh;
    std::vector<SkinWeights> weights;
    std::vector<MorphTarget> morphs;
};

TriangleMesh make_cube();                                      // 1 m, centred, flat faces
TriangleMesh make_sphere(u32 segments = 48, u32 rings = 24);   // 1 m wide, centred
TriangleMesh make_plane();                                     // 1 m square, y = 0, facing +Y

// Area-weighted smooth normals from the triangles (for meshes without any).
// Vertices no triangle uses get +Y.
void compute_normals(TriangleMesh& mesh);

// Tangents from the uvs, the MikkTSpace way: each triangle's +u and +v
// directions, weighted by the corner's angle, summed per vertex (vertices
// alike in position, normal and uv summed together; triangles without area
// left out) and made
// perpendicular to its normal; the bitangent sign says whether +v follows
// cross(normal, tangent). A vertex whose triangles disagree on that sign (a
// mirrored uv seam) is split in two, the copy appended (with its weights,
// when `weights` holds one per vertex). Where the uvs give no direction
// (none, or degenerate) the tangent is any perpendicular of the normal.
// Vertices marked non-zero in `keep` (one per vertex, or empty) keep the
// tangent they have (a file's own) and are never split. `split_from`, when
// given, gets the vertex each appended copy was split from, in order.
void generate_tangents(TriangleMesh& mesh, std::vector<SkinWeights>* weights = nullptr,
                       std::span<const u8> keep = {}, std::vector<u32>* split_from = nullptr);

} // namespace giant::geometry
