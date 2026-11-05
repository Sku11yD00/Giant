#include "giant/render/meshes.h"

#include "giant/core/assert.h"

#include <cmath>

namespace giant::render {

MeshData make_cube() {
    MeshData mesh;
    // One quad per face: normal, and two axes spanning the face such that
    // u x v == normal, so (0,1,2) (0,2,3) wind counter-clockwise outside.
    struct Face {
        Vec3 normal, u, v;
    };
    const Face faces[6] = {
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},  {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},  {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},   {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
    };
    for (const Face& f : faces) {
        const u32 base = static_cast<u32>(mesh.vertices.size());
        const Vec3 c = f.normal * 0.5f;
        mesh.vertices.push_back({c - f.u * 0.5f - f.v * 0.5f, f.normal});
        mesh.vertices.push_back({c + f.u * 0.5f - f.v * 0.5f, f.normal});
        mesh.vertices.push_back({c + f.u * 0.5f + f.v * 0.5f, f.normal});
        mesh.vertices.push_back({c - f.u * 0.5f + f.v * 0.5f, f.normal});
        for (const u32 i : {0u, 1u, 2u, 0u, 2u, 3u}) {
            mesh.indices.push_back(base + i);
        }
    }
    return mesh;
}

MeshData make_sphere(u32 segments, u32 rings) {
    GIANT_ASSERT(segments >= 3 && rings >= 2, "a sphere needs at least 3 segments and 2 rings");
    MeshData mesh;
    // Rows from the north pole (+Y) to the south pole; the seam column is
    // duplicated so each row has segments + 1 vertices.
    for (u32 r = 0; r <= rings; ++r) {
        const float theta = k_pi * static_cast<float>(r) / static_cast<float>(rings);
        for (u32 s = 0; s <= segments; ++s) {
            const float phi = 2.0f * k_pi * static_cast<float>(s) / static_cast<float>(segments);
            const Vec3 n{std::sin(theta) * std::cos(phi), std::cos(theta),
                         -std::sin(theta) * std::sin(phi)};
            mesh.vertices.push_back({n * 0.5f, n});
        }
    }
    const u32 row = segments + 1;
    for (u32 r = 0; r < rings; ++r) {
        for (u32 s = 0; s < segments; ++s) {
            const u32 a = r * row + s;     // top left
            const u32 b = a + row;         // bottom left
            const u32 c = b + 1;           // bottom right
            const u32 d = a + 1;           // top right
            if (r != 0) {                  // the pole rows are single triangles
                mesh.indices.insert(mesh.indices.end(), {a, b, d});
            }
            if (r != rings - 1) {
                mesh.indices.insert(mesh.indices.end(), {d, b, c});
            }
        }
    }
    return mesh;
}

MeshData make_plane() {
    MeshData mesh;
    const Vec3 up{0, 1, 0};
    mesh.vertices = {{{-0.5f, 0, 0.5f}, up}, {{0.5f, 0, 0.5f}, up},
                     {{0.5f, 0, -0.5f}, up}, {{-0.5f, 0, -0.5f}, up}};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    return mesh;
}

} // namespace giant::render
