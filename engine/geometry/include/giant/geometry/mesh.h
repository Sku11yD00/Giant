// Triangle meshes as plain data, and the built-in shapes. Front faces wind
// counter-clockwise seen from outside (the rhi's convention). Metres, +Y up.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"

#include <array>
#include <vector>

namespace giant::geometry {

struct Vertex {
    Vec3 position;
    Vec3 normal; // unit
};
static_assert(sizeof(Vertex) == 24, "shaders read 24-byte vertices");

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

// A mesh in its bind space (giant/anim/skeleton.h), one weight per vertex.
struct SkinnedTriangleMesh {
    TriangleMesh mesh;
    std::vector<SkinWeights> weights;
};

TriangleMesh make_cube();                                      // 1 m, centred, flat faces
TriangleMesh make_sphere(u32 segments = 48, u32 rings = 24);   // 1 m wide, centred
TriangleMesh make_plane();                                     // 1 m square, y = 0, facing +Y

// Area-weighted smooth normals from the triangles (for meshes without any).
// Vertices no triangle uses get +Y.
void compute_normals(TriangleMesh& mesh);

} // namespace giant::geometry
