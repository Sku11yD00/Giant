#include "giant/geometry/mesh.h"

#include "giant/core/assert.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <span>
#include <unordered_map>
#include <vector>

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

namespace {

constexpr u32 k_not_kept = std::numeric_limits<u32>::max();

// Fills `out` with the part's triangles and vertices; returns where each of
// the mesh's vertices went (k_not_kept: not in the part).
std::vector<u32> take_part(const TriangleMesh& mesh, std::span<const u16> triangle_parts, u16 part, TriangleMesh& out) {
    std::vector<u32> went(mesh.vertices.size(), k_not_kept);
    if (triangle_parts.size() != mesh.indices.size() / 3) return went;
    for (usize t = 0; t < triangle_parts.size(); ++t) {
        if (triangle_parts[t] != part) continue;
        for (usize corner = 0; corner < 3; ++corner) {
            const u32 from = mesh.indices[t * 3 + corner];
            GIANT_ASSERT(from < mesh.vertices.size(), "a triangle names a vertex the mesh lacks");
            if (went[from] == k_not_kept) {
                went[from] = static_cast<u32>(out.vertices.size());
                out.vertices.push_back(mesh.vertices[from]);
            }
            out.indices.push_back(went[from]);
        }
    }
    return went;
}

} // namespace

TriangleMesh part_of(const TriangleMesh& mesh, std::span<const u16> triangle_parts, u16 part) {
    TriangleMesh out;
    take_part(mesh, triangle_parts, part, out);
    return out;
}

std::vector<u8> seam_vertices(const TriangleMesh& mesh, std::span<const u16> triangle_parts) {
    std::vector<u8> seams;
    if (triangle_parts.size() != mesh.indices.size() / 3) return seams;
    // Which part uses each place: one part's number, or "several".
    struct Place {
        float x, y, z;
        bool operator==(const Place&) const = default;
    };
    struct PlaceHash {
        usize operator()(const Place& p) const {
            return (std::hash<float>{}(p.x) * 73856093u) ^ (std::hash<float>{}(p.y) * 19349663u) ^
                   (std::hash<float>{}(p.z) * 83492791u);
        }
    };
    constexpr u32 k_several = std::numeric_limits<u32>::max();
    const auto place_of = [&mesh](u32 vertex) {
        const Vec3& p = mesh.vertices[vertex].position;
        return Place{p.x + 0.0f, p.y + 0.0f, p.z + 0.0f}; // (-0 and 0 are one place)
    };
    std::unordered_map<Place, u32, PlaceHash> users;
    users.reserve(mesh.vertices.size());
    for (usize t = 0; t < triangle_parts.size(); ++t) {
        for (usize corner = 0; corner < 3; ++corner) {
            const u32 vertex = mesh.indices[t * 3 + corner];
            GIANT_ASSERT(vertex < mesh.vertices.size(), "a triangle names a vertex the mesh lacks");
            const auto [found, fresh] = users.try_emplace(place_of(vertex), u32{triangle_parts[t]});
            if (!fresh && found->second != triangle_parts[t]) found->second = k_several;
        }
    }
    seams.assign(mesh.vertices.size(), 0);
    for (u32 vertex = 0; vertex < mesh.vertices.size(); ++vertex) {
        // (A place that is not a number is never found: no seam.)
        const auto found = users.find(place_of(vertex));
        if (found != users.end() && found->second == k_several) seams[vertex] = 1;
    }
    return seams;
}

std::vector<u8> part_seams(const TriangleMesh& mesh, std::span<const u16> triangle_parts, u16 part,
                           std::span<const u8> seam_vertices_of_mesh) {
    std::vector<u8> seams;
    if (triangle_parts.size() != mesh.indices.size() / 3 || seam_vertices_of_mesh.size() != mesh.vertices.size()) {
        return seams;
    }
    // In part_of's order: the part's vertices as first used.
    std::vector<u8> seen(mesh.vertices.size(), 0);
    for (usize t = 0; t < triangle_parts.size(); ++t) {
        if (triangle_parts[t] != part) continue;
        for (usize corner = 0; corner < 3; ++corner) {
            const u32 vertex = mesh.indices[t * 3 + corner];
            GIANT_ASSERT(vertex < mesh.vertices.size(), "a triangle names a vertex the mesh lacks");
            if (seen[vertex] != 0) continue;
            seen[vertex] = 1;
            seams.push_back(seam_vertices_of_mesh[vertex]);
        }
    }
    return seams;
}

std::vector<u8> part_seams(const TriangleMesh& mesh, std::span<const u16> triangle_parts, u16 part) {
    return part_seams(mesh, triangle_parts, part, seam_vertices(mesh, triangle_parts));
}

SkinnedTriangleMesh part_of(const SkinnedTriangleMesh& mesh, std::span<const u16> triangle_parts, u16 part) {
    SkinnedTriangleMesh out;
    GIANT_ASSERT(mesh.weights.size() == mesh.mesh.vertices.size(), "a skinned mesh has a weight per vertex");
    const std::vector<u32> went = take_part(mesh.mesh, triangle_parts, part, out.mesh);
    out.weights.resize(out.mesh.vertices.size());
    for (usize v = 0; v < went.size(); ++v) {
        if (went[v] != k_not_kept) out.weights[went[v]] = mesh.weights[v];
    }
    out.morphs.reserve(mesh.morphs.size());
    for (const MorphTarget& morph : mesh.morphs) {
        MorphTarget& kept = out.morphs.emplace_back();
        kept.name = morph.name;
        kept.weight = morph.weight;
        for (const MorphDelta& delta : morph.deltas) {
            if (delta.vertex < went.size() && went[delta.vertex] != k_not_kept) {
                kept.deltas.push_back({went[delta.vertex], delta.position, delta.normal});
            }
        }
        // In increasing vertex order, as a target's moves are kept (the part's vertices are in the
        // order first used, not the mesh's).
        std::sort(kept.deltas.begin(), kept.deltas.end(),
                  [](const MorphDelta& a, const MorphDelta& b) { return a.vertex < b.vertex; });
    }
    return out;
}

} // namespace giant::geometry
