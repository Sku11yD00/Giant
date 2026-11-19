#include "giant/geometry/pose_bake.h"

#include "giant/anim/skeleton.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <vector>

namespace giant::geometry {

namespace {

bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// Unit `v`, or `fallback` when it has no direction.
Vec3 unit_or(const Vec3& v, const Vec3& fallback) {
    if (!finite(v)) return fallback;
    const Vec3 n = normalize_or_zero(v);
    return length_squared(n) > 0.0f ? n : fallback;
}

// A unit vector perpendicular to unit `n` (the skinning shader's choice).
Vec3 any_perpendicular(const Vec3& n) {
    return std::abs(n.x) < 0.9f ? unit_or(Vec3{1.0f, 0.0f, 0.0f} - n * n.x, {0.0f, 1.0f, 0.0f})
                                : unit_or(Vec3{0.0f, 1.0f, 0.0f} - n * n.y, {1.0f, 0.0f, 0.0f});
}

// Whether the matrix's 3x3 part mirrors.
bool mirrors(const Mat4& m) {
    const Vec3 x{m.at(0, 0), m.at(1, 0), m.at(2, 0)};
    const Vec3 y{m.at(0, 1), m.at(1, 1), m.at(2, 1)};
    const Vec3 z{m.at(0, 2), m.at(1, 2), m.at(2, 2)};
    return dot(x, cross(y, z)) < 0.0f;
}

// A place as three words that compare alike exactly where the floats do
// (-0 is 0).
struct Place {
    u32 x, y, z;
    u32 part;
    u32 vertex;
    bool same_place(const Place& o) const { return x == o.x && y == o.y && z == o.z; }
};
u32 bits_of(float f) {
    f += 0.0f; // -0 becomes +0
    u32 b = 0;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}

// The triangles that still have three different corners, and the vertices
// they use (in the order first used), with their weights.
SkinnedTriangleMesh compacted(std::span<const Vertex> vertices, std::span<const SkinWeights> weights,
                              std::span<const u32> indices) {
    SkinnedTriangleMesh out;
    std::vector<u32> remap(vertices.size(), ~0u);
    out.mesh.indices.reserve(indices.size());
    for (usize i = 0; i + 2 < indices.size(); i += 3) {
        const u32 a = indices[i], b = indices[i + 1], c = indices[i + 2];
        if (a == b || b == c || a == c) continue;
        for (const u32 v : {a, b, c}) {
            if (remap[v] == ~0u) {
                remap[v] = static_cast<u32>(out.mesh.vertices.size());
                out.mesh.vertices.push_back(vertices[v]);
                out.weights.push_back(weights[v]);
            }
            out.mesh.indices.push_back(remap[v]);
        }
    }
    return out;
}

} // namespace

void apply_morphs(std::span<Vertex> vertices, std::span<const MorphTarget> morphs, std::span<const float> weights) {
    std::vector<u8> turned; // vertices whose normal a target moved
    for (usize t = 0; t < morphs.size() && t < weights.size(); ++t) {
        const float w = weights[t];
        if (w == 0.0f || !std::isfinite(w)) continue;
        for (const MorphDelta& d : morphs[t].deltas) {
            if (d.vertex >= vertices.size()) continue;
            Vertex& v = vertices[d.vertex];
            v.position = v.position + d.position * w;
            v.normal = v.normal + d.normal * w;
            if (turned.empty()) turned.assign(vertices.size(), 0);
            turned[d.vertex] = 1;
        }
    }
    for (usize v = 0; v < turned.size(); ++v) {
        if (turned[v] != 0) vertices[v].normal = unit_or(vertices[v].normal, {0.0f, 1.0f, 0.0f});
    }
}

std::vector<std::vector<u8>> seams_between(std::span<const std::span<const Vertex>> parts) {
    std::vector<std::vector<u8>> flags(parts.size());
    std::vector<Place> places;
    usize total = 0;
    for (const std::span<const Vertex>& part : parts) total += part.size();
    places.reserve(total);
    for (usize p = 0; p < parts.size(); ++p) {
        flags[p].assign(parts[p].size(), 0);
        for (usize v = 0; v < parts[p].size(); ++v) {
            const Vec3& at = parts[p][v].position;
            if (!finite(at)) continue;
            places.push_back({bits_of(at.x), bits_of(at.y), bits_of(at.z), static_cast<u32>(p), static_cast<u32>(v)});
        }
    }
    std::sort(places.begin(), places.end(), [](const Place& a, const Place& b) {
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        if (a.z != b.z) return a.z < b.z;
        return a.part < b.part;
    });
    for (usize first = 0; first < places.size();) {
        usize end = first + 1;
        while (end < places.size() && places[end].same_place(places[first])) ++end;
        // (Sorted by part within a place: more than one part when the ends differ.)
        if (places[first].part != places[end - 1].part) {
            for (usize i = first; i < end; ++i) flags[places[i].part][places[i].vertex] = 1;
        }
        first = end;
    }
    return flags;
}

SimplifiedSkin simplify_skinned(std::span<const Vertex> vertices, std::span<const u32> indices,
                                std::span<const SkinWeights> weights, u32 triangles, std::span<const u8> locked) {
    SimplifiedSkin out;
    const usize index_count = indices.size() - indices.size() % 3;
    if (index_count == 0 || vertices.empty() || weights.size() != vertices.size()) return out;
    for (usize i = 0; i < index_count; ++i) {
        if (indices[i] >= vertices.size()) return out;
    }
    out.source_triangles = static_cast<u32>(index_count / 3);
    const usize target = static_cast<usize>(std::max<u32>(triangles, 1)) * 3;
    std::vector<u32> kept(indices.begin(), indices.begin() + static_cast<std::ptrdiff_t>(index_count));
    if (index_count > target) {
        const float* positions = &vertices[0].position.x;
        const float scale = meshopt_simplifyScale(positions, vertices.size(), sizeof(Vertex));
        // Shape first: a figure is seen small, by its outline. Normals count a
        // little (so a crease is not smoothed away for nothing), uvs not at
        // all (their seams are separate vertices already). Heavier weights, as
        // build_clusters has for meshes seen close, cost a person cut to a
        // tenth several times the shape error.
        const float attribute_weights[5] = {0.05f, 0.05f, 0.05f, 0.0f, 0.0f};
        static_assert(offsetof(Vertex, uv) == offsetof(Vertex, normal) + 12, "normal then uv, 5 floats");
        const float* attributes = &vertices[0].normal.x;
        const float no_limit = std::numeric_limits<float>::max();
        const auto near_enough = [&] { return kept.size() <= target + target / 4; };
        const auto note = [&](float relative) {
            // (Each step measures from what it was given: together, their sum.)
            if (std::isfinite(relative)) out.error += relative * scale;
        };

        std::vector<unsigned char> locks;
        if (locked.size() == vertices.size()) {
            locks.resize(vertices.size());
            for (usize v = 0; v < locks.size(); ++v) locks[v] = locked[v] != 0 ? meshopt_SimplifyVertex_Lock : 0;
        }
        std::vector<u32> next(kept.size());
        float error = 0.0f;
        // 1. Its joins and its seams kept.
        next.resize(meshopt_simplifyWithAttributes(next.data(), kept.data(), kept.size(), positions, vertices.size(),
                                                   sizeof(Vertex), attributes, sizeof(Vertex), attribute_weights, 5,
                                                   locks.empty() ? nullptr : locks.data(), target, no_limit, 0,
                                                   &error));
        note(error);
        kept.swap(next);
        if (!near_enough()) {
            // 2. Seams and locks let go, loose pieces dropped.
            next.resize(kept.size());
            error = 0.0f;
            next.resize(meshopt_simplifyWithAttributes(next.data(), kept.data(), kept.size(), positions,
                                                       vertices.size(), sizeof(Vertex), attributes, sizeof(Vertex),
                                                       attribute_weights, 5, nullptr, target, no_limit,
                                                       meshopt_SimplifyPermissive | meshopt_SimplifyPrune, &error));
            note(error);
            kept.swap(next);
            out.unlocked = true;
        }
    }
    out.mesh = compacted(vertices, weights, kept);
    return out;
}

TriangleMesh bake_pose(const SkinnedTriangleMesh& mesh, std::span<const Mat4> skin) {
    TriangleMesh out;
    if (mesh.weights.size() != mesh.mesh.vertices.size()) return out;
    out.indices = mesh.mesh.indices;
    out.vertices.resize(mesh.mesh.vertices.size());
    for (usize v = 0; v < out.vertices.size(); ++v) {
        const Vertex& bind = mesh.mesh.vertices[v];
        const SkinWeights& w = mesh.weights[v];
        Vertex& posed = out.vertices[v];
        posed.uv = bind.uv;
        posed.position = anim::skin_point(skin, w.joints, w.weights, bind.position);
        if (!finite(posed.position)) posed.position = bind.position;
        posed.normal = unit_or(anim::skin_direction(skin, w.joints, w.weights, bind.normal), {0.0f, 1.0f, 0.0f});
        // The tangent stays perpendicular to the posed normal.
        Vec3 t = anim::skin_direction(skin, w.joints, w.weights, bind.tangent);
        if (!finite(t)) t = {};
        t = t - posed.normal * dot(posed.normal, t);
        posed.tangent = length_squared(t) > 1e-24f ? unit_or(t, any_perpendicular(posed.normal))
                                                   : any_perpendicular(posed.normal);
        // The first weighted joint mirroring turns the bitangent.
        posed.bitangent_sign = bind.bitangent_sign < 0.0f ? -1.0f : 1.0f;
        for (usize k = 0; k < 4; ++k) {
            if (w.weights[k] == 0 || w.joints[k] >= skin.size()) continue;
            if (mirrors(skin[w.joints[k]])) posed.bitangent_sign = -posed.bitangent_sign;
            break;
        }
    }
    return out;
}

} // namespace giant::geometry
