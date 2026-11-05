// Triangle meshes as plain data, and the built-in shapes. Front faces wind
// counter-clockwise seen from outside (the rhi's convention).
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"

#include <vector>

namespace giant::render {

struct MeshVertex {
    Vec3 position;
    Vec3 normal; // unit
};
static_assert(sizeof(MeshVertex) == 24, "the shader reads 24-byte vertices");

struct MeshData {
    std::vector<MeshVertex> vertices;
    std::vector<u32> indices; // triangles
};

MeshData make_cube();                                     // 1 m, centred, flat faces
MeshData make_sphere(u32 segments = 48, u32 rings = 24);  // 1 m wide, centred
MeshData make_plane();                                    // 1 m square, y = 0, facing +Y

} // namespace giant::render
