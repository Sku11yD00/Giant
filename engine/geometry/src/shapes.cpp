#include "giant/geometry/mesh.h"

#include "giant/core/assert.h"

#include <cmath>

namespace giant::geometry {

TriangleMesh make_cube() {
    TriangleMesh mesh;
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
        // Each face shows the whole image, its top along +v.
        mesh.vertices.push_back({c - f.u * 0.5f - f.v * 0.5f, f.normal, {0, 1}});
        mesh.vertices.push_back({c + f.u * 0.5f - f.v * 0.5f, f.normal, {1, 1}});
        mesh.vertices.push_back({c + f.u * 0.5f + f.v * 0.5f, f.normal, {1, 0}});
        mesh.vertices.push_back({c - f.u * 0.5f + f.v * 0.5f, f.normal, {0, 0}});
        for (const u32 i : {0u, 1u, 2u, 0u, 2u, 3u}) {
            mesh.indices.push_back(base + i);
        }
    }
    generate_tangents(mesh);
    return mesh;
}

TriangleMesh make_sphere(u32 segments, u32 rings) {
    GIANT_ASSERT(segments >= 3 && rings >= 2, "a sphere needs at least 3 segments and 2 rings");
    TriangleMesh mesh;
    // Rows from the north pole (+Y) to the south pole; the seam column is
    // duplicated so each row has segments + 1 vertices.
    for (u32 r = 0; r <= rings; ++r) {
        const float theta = k_pi * static_cast<float>(r) / static_cast<float>(rings);
        for (u32 s = 0; s <= segments; ++s) {
            // The seam column repeats the first one's positions exactly, and
            // the poles are exact, so the vertices there meet bit for bit:
            // the simplifier takes them for uv seams, not open borders, and
            // coarser levels keep them closed.
            const float phi = 2.0f * k_pi * static_cast<float>(s % segments) / static_cast<float>(segments);
            Vec3 n{std::sin(theta) * std::cos(phi), std::cos(theta), -std::sin(theta) * std::sin(phi)};
            if (r == 0 || r == rings) n = {0.0f, r == 0 ? 1.0f : -1.0f, 0.0f};
            // The image wraps round once, its top at the north pole.
            mesh.vertices.push_back({n * 0.5f, n,
                                     {static_cast<float>(s) / static_cast<float>(segments),
                                      static_cast<float>(r) / static_cast<float>(rings)}});
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
    generate_tangents(mesh);
    return mesh;
}

TriangleMesh make_plane() {
    TriangleMesh mesh;
    const Vec3 up{0, 1, 0};
    // The image's top toward -Z.
    mesh.vertices = {{{-0.5f, 0, 0.5f}, up, {0, 1}}, {{0.5f, 0, 0.5f}, up, {1, 1}},
                     {{0.5f, 0, -0.5f}, up, {1, 0}}, {{-0.5f, 0, -0.5f}, up, {0, 0}}};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    generate_tangents(mesh);
    return mesh;
}

void compute_normals(TriangleMesh& mesh) {
    std::vector<Vec3> sums(mesh.vertices.size(), Vec3{});
    for (usize i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const u32 a = mesh.indices[i], b = mesh.indices[i + 1], c = mesh.indices[i + 2];
        GIANT_ASSERT(a < sums.size() && b < sums.size() && c < sums.size(), "index out of range");
        const Vec3 pa = mesh.vertices[a].position;
        // The cross product's length is twice the area: area weighting.
        const Vec3 n = cross(mesh.vertices[b].position - pa, mesh.vertices[c].position - pa);
        sums[a] = sums[a] + n;
        sums[b] = sums[b] + n;
        sums[c] = sums[c] + n;
    }
    for (usize i = 0; i < sums.size(); ++i) {
        const Vec3 n = normalize_or_zero(sums[i]);
        mesh.vertices[i].normal = length_squared(n) > 0.0f ? n : Vec3{0, 1, 0};
    }
}

} // namespace giant::geometry
