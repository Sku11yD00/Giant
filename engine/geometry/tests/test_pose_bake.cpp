#include "giant/geometry/pose_bake.h"

#include "giant/anim/skeleton.h"
#include "giant/geometry/clusters.h"
#include "giant/geometry/mesh.h"

#include "giant_test.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <span>
#include <tuple>
#include <vector>

using namespace giant;
using namespace giant::geometry;

namespace {

bool close(const Vec3& a, const Vec3& b, float within = 1e-5f) { return length(a - b) <= within; }

// A sphere whose top is moved by joint 1 and bottom by joint 0, blended
// across the middle.
SkinnedTriangleMesh two_joint_sphere(u32 segments, u32 rings) {
    SkinnedTriangleMesh s;
    s.mesh = make_sphere(segments, rings);
    generate_tangents(s.mesh);
    for (const Vertex& v : s.mesh.vertices) {
        SkinWeights w;
        w.joints = {0, 1, 0, 0};
        const float up = std::clamp(v.position.y * 4.0f + 0.5f, 0.0f, 1.0f); // 0 below -0.125, 1 above 0.125
        const u8 top = static_cast<u8>(std::lround(up * 255.0f));
        w.weights = {static_cast<u8>(255 - top), top, 0, 0};
        s.weights.push_back(w);
    }
    return s;
}

using Key = std::tuple<float, float, float, float, float, float, float, float>;
Key key_of(const Vertex& v) {
    return {v.position.x, v.position.y, v.position.z, v.normal.x, v.normal.y, v.normal.z, v.uv.u, v.uv.v};
}

} // namespace

GIANT_TEST("pose bake: a baked pose is each vertex skinned by its joints (anim's skinning, the shader's twin)") {
    const SkinnedTriangleMesh s = two_joint_sphere(24, 12);
    const Quat turn = quat_from_axis_angle({0.0f, 0.0f, 1.0f}, 0.7f);
    const std::vector<Mat4> skin = {Mat4{}, mat4_from_transform({0.25f, 0.5f, -0.125f}, turn, {1.0f, 1.0f, 1.0f})};
    const TriangleMesh baked = bake_pose(s, skin);
    GIANT_CHECK(baked.indices == s.mesh.indices);
    GIANT_CHECK(baked.vertices.size() == s.mesh.vertices.size());
    bool places = true, normals = true, tangents = true, rest = true;
    usize moved = 0, blended = 0;
    for (usize v = 0; v < baked.vertices.size(); ++v) {
        const Vertex& bind = s.mesh.vertices[v];
        const Vertex& posed = baked.vertices[v];
        const SkinWeights& w = s.weights[v];
        places = places && close(posed.position, anim::skin_point(skin, w.joints, w.weights, bind.position), 0.0f);
        normals = normals && close(posed.normal, anim::skin_direction(skin, w.joints, w.weights, bind.normal)) &&
                  std::abs(length(posed.normal) - 1.0f) < 1e-5f;
        tangents = tangents && std::abs(dot(posed.tangent, posed.normal)) < 1e-4f &&
                   std::abs(length(posed.tangent) - 1.0f) < 1e-5f && posed.bitangent_sign == bind.bitangent_sign;
        rest = rest && posed.uv == bind.uv;
        if (w.weights[1] == 255) ++moved;
        if (w.weights[1] != 0 && w.weights[1] != 255) ++blended;
        if (w.weights[0] == 255) rest = rest && posed.position == bind.position && close(posed.normal, bind.normal);
    }
    GIANT_CHECK(places);
    GIANT_CHECK(normals);
    GIANT_CHECK(tangents);
    GIANT_CHECK(rest);
    GIANT_CHECK(moved > 0 && blended > 0);

    // Every joint at bind: the mesh as it was bound.
    const std::vector<Mat4> still(2);
    const TriangleMesh same = bake_pose(s, still);
    bool alike = same.vertices.size() == s.mesh.vertices.size();
    for (usize v = 0; alike && v < same.vertices.size(); ++v) {
        alike = close(same.vertices[v].position, s.mesh.vertices[v].position, 1e-6f) &&
                close(same.vertices[v].normal, s.mesh.vertices[v].normal) &&
                close(same.vertices[v].tangent, s.mesh.vertices[v].tangent, 1e-4f);
    }
    GIANT_CHECK(alike);
    // It goes to the renderer as any mesh does.
    const ClusterMesh clusters = build_clusters(baked);
    GIANT_CHECK(!clusters.empty() && clusters.source_triangles == baked.indices.size() / 3);
}

GIANT_TEST("pose bake: mirrors turn the bitangent, bad joints and matrices are left out") {
    SkinnedTriangleMesh s;
    s.mesh = make_plane();
    generate_tangents(s.mesh);
    s.weights.resize(s.mesh.vertices.size());
    // Vertex 0: the mirroring joint first. 1: an unweighted slot before it. 2: a joint out of
    // range with half the weight. 3: a joint whose matrix is not a number.
    s.weights[0] = {{1, 0, 0, 0}, {255, 0, 0, 0}};
    s.weights[1] = {{0, 1, 0, 0}, {0, 255, 0, 0}};
    s.weights[2] = {{9, 0, 0, 0}, {128, 127, 0, 0}};
    s.weights[3] = {{2, 0, 0, 0}, {255, 0, 0, 0}};
    Mat4 broken;
    broken.at(0, 3) = std::numeric_limits<float>::quiet_NaN();
    const std::vector<Mat4> skin = {Mat4{}, mat4_from_transform({}, Quat{}, {-1.0f, 1.0f, 1.0f}), broken};
    const TriangleMesh baked = bake_pose(s, skin);
    GIANT_CHECK(baked.vertices.size() == 4);
    for (usize v = 0; v < 2; ++v) {
        const Vertex& bind = s.mesh.vertices[v];
        GIANT_CHECK(baked.vertices[v].bitangent_sign == -bind.bitangent_sign);
        GIANT_CHECK(close(baked.vertices[v].position, {-bind.position.x, bind.position.y, bind.position.z}));
    }
    GIANT_CHECK(close(baked.vertices[2].position, s.mesh.vertices[2].position * (127.0f / 255.0f)));
    GIANT_CHECK(baked.vertices[2].bitangent_sign == s.mesh.vertices[2].bitangent_sign);
    GIANT_CHECK(baked.vertices[3].position == s.mesh.vertices[3].position); // left where it was bound
    for (const Vertex& v : baked.vertices) {
        GIANT_CHECK(std::abs(length(v.normal) - 1.0f) < 1e-5f && std::abs(length(v.tangent) - 1.0f) < 1e-5f);
        GIANT_CHECK(std::abs(dot(v.normal, v.tangent)) < 1e-4f);
    }
    // No joints at all: every normal +Y, nothing that is not a number. Weights not one per vertex: nothing.
    const TriangleMesh none = bake_pose(s, {});
    for (const Vertex& v : none.vertices) {
        GIANT_CHECK(v.normal == Vec3(0.0f, 1.0f, 0.0f) && std::abs(length(v.tangent) - 1.0f) < 1e-5f);
    }
    s.weights.pop_back();
    GIANT_CHECK(bake_pose(s, skin).vertices.empty() && bake_pose(s, skin).indices.empty());
}

GIANT_TEST("pose bake: morph targets move bind vertices by their weights") {
    std::vector<Vertex> vertices(4);
    for (usize v = 0; v < 4; ++v) {
        vertices[v].position = {static_cast<float>(v), 0.0f, 0.0f};
        vertices[v].normal = {0.0f, 1.0f, 0.0f};
    }
    std::vector<MorphTarget> morphs;
    morphs.push_back({"up", 0.0f, {{0, {0, 2, 0}, {1, 0, 0}}, {1, {0, 4, 0}, {0, -1, 0}}}});
    morphs.push_back({"along", 0.0f, {{0, {1, 0, 0}, {}}, {7, {1, 0, 0}, {}}}}); // one names no vertex
    morphs.push_back({"never", 0.0f, {{2, {0, 9, 0}, {}}}});                     // no weight given
    morphs.push_back({"bad", 0.0f, {{3, {0, 9, 0}, {}}}});
    const std::vector<float> weights = {0.5f, 2.0f};
    apply_morphs(vertices, morphs, weights);
    GIANT_CHECK(close(vertices[0].position, {2.0f, 1.0f, 0.0f}));
    GIANT_CHECK(close(vertices[0].normal, normalize_or_zero(Vec3{0.5f, 1.0f, 0.0f})));
    GIANT_CHECK(close(vertices[1].position, {1.0f, 2.0f, 0.0f}));
    GIANT_CHECK(close(vertices[1].normal, {0.0f, 1.0f, 0.0f})); // (0, 0.5, 0) made unit
    GIANT_CHECK(vertices[2].position == Vec3(2.0f, 0.0f, 0.0f) && vertices[3].position == Vec3(3.0f, 0.0f, 0.0f));
    // A normal brought to nothing is +Y; a weight that is not a number does nothing.
    const std::vector<float> more = {2.0f, 0.0f, 0.0f, std::numeric_limits<float>::quiet_NaN()};
    apply_morphs(vertices, morphs, more);
    GIANT_CHECK(close(vertices[1].position, {1.0f, 10.0f, 0.0f}));
    GIANT_CHECK(close(vertices[1].normal, {0.0f, -1.0f, 0.0f}));
    GIANT_CHECK(vertices[3].position == Vec3(3.0f, 0.0f, 0.0f));
    std::vector<MorphTarget> flat;
    flat.push_back({"flat", 0.0f, {{2, {}, {0, -1, 0}}}});
    const std::vector<float> one = {1.0f};
    apply_morphs(vertices, flat, one);
    GIANT_CHECK(vertices[2].normal == Vec3(0.0f, 1.0f, 0.0f));
}

GIANT_TEST("pose bake: seams are where two parts have a vertex at one place") {
    // Two squares sharing the edge x = 1, a third apart; one place written as -0.
    const auto square = [](float x0) {
        std::vector<Vertex> v(4);
        v[0].position = {x0, 0.0f, 0.0f};
        v[1].position = {x0 + 1.0f, 0.0f, 0.0f};
        v[2].position = {x0, 0.0f, 1.0f};
        v[3].position = {x0 + 1.0f, 0.0f, 1.0f};
        return v;
    };
    std::vector<Vertex> a = square(0.0f), b = square(1.0f), c = square(5.0f);
    a[1].position.y = -0.0f;
    a.push_back(a[1]);                                                 // a twin inside its own part
    a.push_back(a[0]);                                                 // likewise, on no seam
    c[0].position.x = std::numeric_limits<float>::quiet_NaN();
    b.push_back(c[0]);                                                 // not a number in two parts: no seam
    const std::vector<std::span<const Vertex>> parts = {a, b, c, {}};
    const std::vector<std::vector<u8>> seams = seams_between(parts);
    GIANT_CHECK(seams.size() == 4 && seams[3].empty());
    GIANT_CHECK(seams[0] == std::vector<u8>({0, 1, 0, 1, 1, 0}));
    GIANT_CHECK(seams[1] == std::vector<u8>({1, 0, 1, 0, 0}));
    GIANT_CHECK(seams[2] == std::vector<u8>({0, 0, 0, 0}));
    GIANT_CHECK(seams_between({}).empty());
}

GIANT_TEST("pose bake: a simplified skin keeps its vertices' own weights and poses like the original") {
    const SkinnedTriangleMesh s = two_joint_sphere(64, 32);
    const u32 source = static_cast<u32>(s.mesh.indices.size() / 3);
    std::map<Key, SkinWeights> weights_of;
    for (usize v = 0; v < s.mesh.vertices.size(); ++v) weights_of[key_of(s.mesh.vertices[v])] = s.weights[v];

    // The equator's ring of vertices locked: all of it is kept.
    std::vector<u8> locked(s.mesh.vertices.size(), 0);
    float ring_y = 10.0f;
    for (const Vertex& v : s.mesh.vertices) ring_y = std::abs(v.position.y) < std::abs(ring_y) ? v.position.y : ring_y;
    usize ring = 0;
    for (usize v = 0; v < locked.size(); ++v) {
        if (s.mesh.vertices[v].position.y == ring_y) {
            locked[v] = 1;
            ++ring;
        }
    }
    GIANT_CHECK(ring >= 64);

    const SimplifiedSkin small = simplify_skinned(s.mesh.vertices, s.mesh.indices, s.weights, 500, locked);
    const usize triangles = small.mesh.mesh.indices.size() / 3;
    GIANT_CHECK(small.source_triangles == source && source > 3000);
    GIANT_CHECK(triangles > 100 && triangles <= 625);
    GIANT_CHECK(small.error > 0.0f && small.error < 0.05f); // a 1 m sphere
    GIANT_CHECK(!small.unlocked);
    GIANT_CHECK(small.mesh.weights.size() == small.mesh.mesh.vertices.size() && small.mesh.morphs.empty());
    GIANT_CHECK(small.mesh.mesh.vertices.size() < s.mesh.vertices.size() / 3);
    bool own = true;
    usize ring_kept = 0;
    std::vector<u8> used(small.mesh.mesh.vertices.size(), 0);
    for (const u32 i : small.mesh.mesh.indices) {
        GIANT_CHECK(i < used.size());
        used[i] = 1;
    }
    for (usize v = 0; v < small.mesh.mesh.vertices.size(); ++v) {
        const auto found = weights_of.find(key_of(small.mesh.mesh.vertices[v]));
        own = own && found != weights_of.end() && found->second == small.mesh.weights[v] && used[v] != 0;
        if (small.mesh.mesh.vertices[v].position.y == ring_y) ++ring_kept;
    }
    GIANT_CHECK(own); // each one kept is an original vertex, with its weights, and a triangle uses it
    GIANT_CHECK(ring_kept == ring);

    // Posed, the small mesh stays on the original's posed surface: every vertex of it is one of the
    // original's, where the original's is.
    const Quat turn = quat_from_axis_angle({1.0f, 0.0f, 0.0f}, 0.5f);
    const std::vector<Mat4> skin = {Mat4{}, mat4_from_transform({0.0f, 0.25f, 0.0f}, turn, {1.0f, 1.0f, 1.0f})};
    const TriangleMesh full = bake_pose(s, skin);
    const TriangleMesh little = bake_pose(small.mesh, skin);
    std::map<std::tuple<float, float, float>, int> posed;
    for (const Vertex& v : full.vertices) posed[{v.position.x, v.position.y, v.position.z}] = 1;
    bool on = true;
    for (const Vertex& v : little.vertices) on = on && posed.count({v.position.x, v.position.y, v.position.z}) == 1;
    GIANT_CHECK(on);

    // No fewer asked for than it has: the whole mesh, nothing lost.
    const SimplifiedSkin whole = simplify_skinned(s.mesh.vertices, s.mesh.indices, s.weights, source);
    GIANT_CHECK(whole.mesh.mesh.indices.size() == s.mesh.indices.size() && whole.error == 0.0f);
    // Nothing from nothing.
    GIANT_CHECK(simplify_skinned({}, {}, {}, 10).mesh.mesh.indices.empty());
    GIANT_CHECK(simplify_skinned(s.mesh.vertices, s.mesh.indices, std::span<const SkinWeights>(s.weights).first(3), 10)
                    .mesh.mesh.indices.empty());
    const std::vector<u32> out_of_range = {0, 1, static_cast<u32>(s.mesh.vertices.size())};
    GIANT_CHECK(simplify_skinned(s.mesh.vertices, out_of_range, s.weights, 10).mesh.mesh.indices.empty());
    // Degenerate triangles and vertices nothing uses are dropped even when nothing is simplified.
    const std::vector<u32> few = {0, 1, 2, 5, 5, 6};
    const SimplifiedSkin tidy = simplify_skinned(s.mesh.vertices, few, s.weights, 10);
    GIANT_CHECK(tidy.mesh.mesh.indices.size() == 3 && tidy.mesh.mesh.vertices.size() == 3 && tidy.source_triangles == 2);
}

GIANT_TEST("pose bake: loose cards that share no edge still come down to the target") {
    // Hair as cards: 2000 small quads, none touching another (every vertex on an open edge).
    std::vector<Vertex> vertices;
    std::vector<u32> indices;
    std::vector<SkinWeights> weights;
    for (u32 q = 0; q < 2000; ++q) {
        const float x = static_cast<float>(q % 50) * 0.01f, z = static_cast<float>(q / 50) * 0.01f;
        const u32 base = static_cast<u32>(vertices.size());
        for (u32 c = 0; c < 4; ++c) {
            Vertex v;
            v.position = {x + (c & 1 ? 0.008f : 0.0f), static_cast<float>(q % 7) * 0.002f, z + (c & 2 ? 0.008f : 0.0f)};
            v.normal = {0.0f, 1.0f, 0.0f};
            vertices.push_back(v);
            weights.push_back({{static_cast<u16>(q % 3), 0, 0, 0}, {255, 0, 0, 0}});
        }
        indices.insert(indices.end(), {base, base + 2, base + 3, base, base + 3, base + 1});
    }
    const SimplifiedSkin small = simplify_skinned(vertices, indices, weights, 400);
    const usize triangles = small.mesh.mesh.indices.size() / 3;
    GIANT_CHECK(small.source_triangles == 4000);
    GIANT_CHECK(!small.unlocked && small.error > 0.0f);
    // Every vertex locked: nothing can go until the locks are let go.
    const std::vector<u8> all_locked(vertices.size(), 1);
    const SimplifiedSkin freed = simplify_skinned(vertices, indices, weights, 400, all_locked);
    GIANT_CHECK(freed.unlocked && freed.mesh.mesh.indices.size() / 3 <= 500 && !freed.mesh.mesh.indices.empty());
    GIANT_CHECK(freed.mesh.weights.size() == freed.mesh.mesh.vertices.size());
    // Triangles alone, each far from the next.
    std::vector<Vertex> lone;
    std::vector<u32> lone_indices;
    std::vector<SkinWeights> lone_weights;
    for (u32 t = 0; t < 600; ++t) {
        for (u32 c = 0; c < 3; ++c) {
            Vertex v;
            v.position = {static_cast<float>(t % 30) + (c == 1 ? 0.3f : 0.0f), static_cast<float>(t / 30), c == 2 ? 0.3f : 0.0f};
            v.normal = {0.0f, 1.0f, 0.0f};
            lone.push_back(v);
            lone_weights.push_back({{0, 0, 0, 0}, {255, 0, 0, 0}});
            lone_indices.push_back(t * 3 + c);
        }
    }
    const SimplifiedSkin soup = simplify_skinned(lone, lone_indices, lone_weights, 50);
    GIANT_CHECK(soup.mesh.mesh.indices.size() / 3 <= 62 && !soup.mesh.mesh.indices.empty() && soup.source_triangles == 600);
    GIANT_CHECK(triangles >= 1 && triangles <= 500);
    GIANT_CHECK(small.mesh.weights.size() == small.mesh.mesh.vertices.size());
    for (const u32 i : small.mesh.mesh.indices) GIANT_CHECK(i < small.mesh.mesh.vertices.size());
}
