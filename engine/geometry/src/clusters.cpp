#include "giant/geometry/clusters.h"

#include "giant/core/assert.h"

#include <clusterlod.h>
#include <meshoptimizer.h>

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <limits>

namespace giant::geometry {

namespace {

static_assert(k_terminal_error == std::numeric_limits<float>::max());

LodBounds to_bounds(const clodBounds& b) {
    return {{b.center[0], b.center[1], b.center[2]}, b.radius, b.error};
}

float max_abs_scale(const Vec3& s) {
    return std::max({std::fabs(s.x), std::fabs(s.y), std::fabs(s.z)});
}

Vec3 to_view(const LodView& view, const Vec3& p) {
    return view.position +
           rotate(view.rotation, Vec3{p.x * view.scale.x, p.y * view.scale.y, p.z * view.scale.z});
}

} // namespace

namespace {

// build_clusters, and with `weights` (one per source vertex) the skinned
// form: vertices told apart by their weights too (`out_weights` gets one
// per output vertex), and without `levels` the original triangles alone.
ClusterMesh build(const TriangleMesh& source, std::span<const u8> locked, const SkinWeights* weights,
                  std::vector<SkinWeights>* out_weights, bool levels) {
    ClusterMesh out;
    if (source.indices.size() < 3 || source.vertices.empty()) {
        return out;
    }
    const usize index_count = source.indices.size() - source.indices.size() % 3;
    for (usize i = 0; i < index_count; ++i) {
        GIANT_ASSERT(source.indices[i] < source.vertices.size(), "triangle index out of range");
    }
    out.source_triangles = static_cast<u32>(index_count / 3);

    // Merge duplicate vertices (exporters write many), so clusters and the
    // simplifier see the real connectivity.
    std::vector<u32> remap(source.vertices.size());
    usize unique = 0;
    if (weights == nullptr) {
        unique = meshopt_generateVertexRemap(remap.data(), source.indices.data(), index_count, source.vertices.data(),
                                             source.vertices.size(), sizeof(Vertex));
    } else {
        static_assert(sizeof(SkinWeights) == 12);
        const meshopt_Stream streams[2] = {{source.vertices.data(), sizeof(Vertex), sizeof(Vertex)},
                                           {weights, sizeof(SkinWeights), sizeof(SkinWeights)}};
        unique = meshopt_generateVertexRemapMulti(remap.data(), source.indices.data(), index_count,
                                                  source.vertices.size(), streams, 2);
        out_weights->resize(unique);
        meshopt_remapVertexBuffer(out_weights->data(), weights, source.vertices.size(), sizeof(SkinWeights),
                                  remap.data());
    }
    out.vertices.resize(unique);
    meshopt_remapVertexBuffer(out.vertices.data(), source.vertices.data(), source.vertices.size(),
                              sizeof(Vertex), remap.data());
    std::vector<u32> indices(index_count);
    meshopt_remapIndexBuffer(indices.data(), source.indices.data(), index_count, remap.data());

    GIANT_ASSERT(locked.empty() || locked.size() == source.vertices.size(), "one lock flag per vertex");
    std::vector<unsigned char> locks;
    if (!locked.empty()) {
        locks.assign(unique, 0);
        for (usize i = 0; i < locked.size(); ++i) {
            if (locked[i] != 0 && remap[i] != ~0u) locks[remap[i]] = meshopt_SimplifyVertex_Lock;
        }
    }

    // A skinned mesh's weight seams (corners at one place with other
    // weights) protected at every level: a coarse triangle never takes a
    // corner from across one, which a pose would tear away.
    if (weights != nullptr && levels) {
        std::vector<u32> twin(unique);
        meshopt_generatePositionRemap(twin.data(), &out.vertices[0].position.x, unique, sizeof(Vertex));
        for (usize i = 0; i < unique; ++i) {
            const SkinWeights& a = (*out_weights)[i];
            const SkinWeights& b = (*out_weights)[twin[i]];
            if (a.joints == b.joints && a.weights == b.weights) continue;
            if (locks.empty()) locks.assign(unique, 0);
            locks[i] |= meshopt_SimplifyVertex_Protect;
            locks[twin[i]] |= meshopt_SimplifyVertex_Protect;
        }
    }

    clodConfig config = clodDefaultConfig(k_cluster_max_triangles);
    config.max_vertices = k_cluster_max_vertices;
    config.optimize_bounds = true; // tight culling spheres
    // Every level indexes the one vertex array, unmoved (skinned meshes'
    // weights stay each vertex's own): nothing may move vertices.
    GIANT_ASSERT(!config.simplify_dilate_borders, "simplification must not move vertices");
    if (weights != nullptr) {
        config.simplify_fallback_sloppy = false; // (sloppy simplification ignores the seams' protection)
    }
    const bool base_only = weights != nullptr && !levels;
    if (base_only) {
        // Every first group counts as stuck: nothing is simplified (a ratio
        // of 1 makes the attempt cheap), so the original triangles are the
        // only level.
        config.simplify_ratio = 1.0f;
        config.simplify_threshold = 0.0f;
    }

    // Normals take part in simplification, as in meshoptimizer's own demo,
    // and uvs lightly (uv seams are kept anyway: the vertices either side
    // differ). Simplification only drops vertices, so the survivors keep
    // their uvs and tangents.
    const float attribute_weights[5] = {0.5f, 0.5f, 0.5f, 0.1f, 0.1f};
    static_assert(offsetof(Vertex, uv) == offsetof(Vertex, normal) + 12, "normal then uv, 5 floats");
    clodMesh mesh{};
    mesh.indices = indices.data();
    mesh.index_count = indices.size();
    mesh.vertex_count = out.vertices.size();
    mesh.vertex_positions = &out.vertices[0].position.x;
    mesh.vertex_positions_stride = sizeof(Vertex);
    mesh.vertex_lock = locks.empty() ? nullptr : locks.data();
    mesh.vertex_attributes = &out.vertices[0].normal.x;
    mesh.vertex_attributes_stride = sizeof(Vertex);
    mesh.attribute_weights = attribute_weights;
    mesh.attribute_count = 5;

    std::vector<clodGroup> groups;
    clodBuild(config, mesh, [&](clodGroup group, const clodCluster* clusters, usize count) -> int {
        if (base_only && group.depth > 0) {
            return -1; // (not expected: every first group is terminal) only the original triangles
        }
        if (base_only) {
            group.simplified.error = k_terminal_error;
        }
        ClusterGroup g;
        g.depth = static_cast<u32>(group.depth);
        g.first_cluster = static_cast<u32>(out.clusters.size());
        g.cluster_count = static_cast<u32>(count);
        g.simplified = to_bounds(group.simplified);
        const u32 group_index = static_cast<u32>(out.groups.size());
        for (usize i = 0; i < count; ++i) {
            const clodCluster& c = clusters[i];
            Cluster cluster;
            cluster.first_index = static_cast<u32>(out.indices.size());
            cluster.triangle_count = static_cast<u32>(c.index_count / 3);
            cluster.vertex_count = static_cast<u32>(c.vertex_count);
            cluster.group = group_index;
            cluster.refined = c.refined;
            cluster.bounds = {{c.bounds.center[0], c.bounds.center[1], c.bounds.center[2]},
                              c.bounds.radius};
            out.indices.insert(out.indices.end(), c.indices, c.indices + c.index_count);
            out.clusters.push_back(cluster);
        }
        out.groups.push_back(g);
        groups.push_back(group);
        return static_cast<int>(group_index);
    });
    if (out.groups.empty()) {
        out.clusters.clear();
        out.indices.clear();
        return out;
    }

    out.levels = out.groups.back().depth + 1;
    const usize node_width = 8;
    std::vector<clodNode> nodes(clodBuildHierarchyBound(groups.size(), node_width, out.levels));
    nodes.resize(
        clodBuildHierarchy(nodes.data(), groups.data(), groups.size(), node_width, out.levels));
    out.nodes.reserve(nodes.size());
    for (const clodNode& n : nodes) {
        out.nodes.push_back({to_bounds(n.bounds), n.group, n.child_offset, n.child_count});
    }

    const meshopt_Bounds all = meshopt_computeSphereBounds(
        &out.vertices[0].position.x, out.vertices.size(), sizeof(Vertex), nullptr, 0);
    out.bounds = {{all.center[0], all.center[1], all.center[2]}, all.radius};
    return out;
}

} // namespace

ClusterMesh build_clusters(const TriangleMesh& source, std::span<const u8> locked) {
    return build(source, locked, nullptr, nullptr, true);
}

SkinnedClusterMesh build_skinned_clusters(const SkinnedTriangleMesh& source, bool levels) {
    SkinnedClusterMesh out;
    if (source.weights.size() != source.mesh.vertices.size()) {
        return out; // one weight per vertex, or nothing
    }
    out.mesh = build(source.mesh, {}, source.weights.data(), &out.weights, levels);
    if (out.mesh.empty()) {
        out.weights.clear();
    }
    return out;
}

Frustum frustum_from_view_projection(const Mat4& m) {
    // Gribb and Hartmann: clip-space -w <= x, y <= w and z <= w (reverse-Z
    // near; the far plane is at infinity), as rows of the matrix.
    const auto row = [&m](int r) { return Vec4{m.at(r, 0), m.at(r, 1), m.at(r, 2), m.at(r, 3)}; };
    const Vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    const auto add = [](const Vec4& a, const Vec4& b) { return Vec4{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; };
    const auto sub = [](const Vec4& a, const Vec4& b) { return Vec4{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; };
    Frustum f;
    f.planes = {add(r3, r0), sub(r3, r0), add(r3, r1), sub(r3, r1), sub(r3, r2)};
    for (Vec4& p : f.planes) {
        const float len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        if (len > 0.0f) {
            p = {p.x / len, p.y / len, p.z / len, p.w / len};
        }
    }
    return f;
}

bool sphere_in_frustum(const Frustum& frustum, const Vec3& c, float radius) {
    for (const Vec4& p : frustum.planes) {
        if (p.x * c.x + p.y * c.y + p.z * c.z + p.w < -radius) {
            return false;
        }
    }
    return true;
}

float projected_error(const LodBounds& bounds, const LodView& view) {
    if (bounds.error >= k_terminal_error) {
        return std::numeric_limits<float>::infinity();
    }
    const float s = max_abs_scale(view.scale);
    const Vec3 c = to_view(view, bounds.center);
    const float d = length(c) - (bounds.radius + view.pose_reach) * s;
    // A positive floor: with 0 the camera inside a zero-error sphere gives
    // 0 / 0, and NaN would pass neither side of the cut (drawing both).
    const float near_plane = std::max(1e-6f, view.near_plane); // NaN gives the floor too
    return bounds.error * s / std::max(d, near_plane) * view.pixels_per_unit;
}

void select_clusters(const ClusterMesh& mesh, const LodView& view, std::vector<u32>& out,
                     const LodResidency* residency) {
    if (mesh.empty()) {
        return;
    }
    const float s = max_abs_scale(view.scale);
    if (view.frustum != nullptr &&
        !sphere_in_frustum(*view.frustum, to_view(view, mesh.bounds.center), (mesh.bounds.radius + view.pose_reach) * s)) {
        return;
    }
    const std::vector<u8>* resident =
        residency != nullptr && residency->resident != nullptr && residency->resident->size() >= mesh.groups.size()
            ? residency->resident
            : nullptr;
    const auto loaded = [resident](usize group) { return resident == nullptr || (*resident)[group] != 0; };
    const float threshold = view.threshold_pixels;
    // Depth-first over every per-level tree; a node precise enough for the
    // view holds nothing to draw (its groups' replacements are fine).
    std::vector<u32> stack;
    stack.reserve(64);
    for (u32 root = 0; root < mesh.levels && root < mesh.nodes.size(); ++root) {
        stack.push_back(root);
    }
    while (!stack.empty()) {
        const ClusterNode& node = mesh.nodes[stack.back()];
        stack.pop_back();
        if (projected_error(node.bounds, view) <= threshold) {
            continue;
        }
        if (node.group < 0) {
            for (u32 i = 0; i < node.child_count; ++i) {
                stack.push_back(node.child_offset + i);
            }
            continue;
        }
        const usize gi = static_cast<usize>(node.group);
        const ClusterGroup& group = mesh.groups[gi];
        if (projected_error(group.simplified, view) <= threshold) {
            continue; // the leaf's bounds are the group's, but do not rely on it
        }
        if (!loaded(gi)) {
            // Needed but not loaded: the clusters made from it draw instead.
            if (residency->wanted != nullptr) {
                residency->wanted->push_back(static_cast<u32>(gi));
            }
            continue;
        }
        if (resident != nullptr && residency->used != nullptr) {
            residency->used->push_back(static_cast<u32>(gi));
        }
        // The group's replacement is too coarse here. Draw each of its
        // clusters whose own detail is enough (or that cannot get finer, or
        // whose finer version is not loaded).
        for (u32 i = 0; i < group.cluster_count; ++i) {
            const u32 index = group.first_cluster + i;
            const Cluster& c = mesh.clusters[index];
            if (c.refined >= 0 && loaded(static_cast<usize>(c.refined)) &&
                projected_error(mesh.groups[static_cast<usize>(c.refined)].simplified, view) > threshold) {
                continue; // a finer version is drawn instead
            }
            if (view.frustum != nullptr &&
                !sphere_in_frustum(*view.frustum, to_view(view, c.bounds.center), (c.bounds.radius + view.pose_reach) * s)) {
                continue;
            }
            out.push_back(index);
        }
    }
}

GroupLinks link_groups(const ClusterMesh& mesh) {
    const usize n = mesh.groups.size();
    // (child, parent) pairs, one per distinct link.
    std::vector<std::pair<u32, u32>> links;
    for (const Cluster& c : mesh.clusters) {
        if (c.refined >= 0) {
            links.push_back({static_cast<u32>(c.refined), c.group});
        }
    }
    std::sort(links.begin(), links.end());
    links.erase(std::unique(links.begin(), links.end()), links.end());
    GroupLinks out;
    out.parent_offsets.assign(n + 1, 0);
    out.child_offsets.assign(n + 1, 0);
    for (const auto& [child, parent] : links) {
        ++out.parent_offsets[child + 1];
        ++out.child_offsets[parent + 1];
    }
    for (usize g = 0; g < n; ++g) {
        out.parent_offsets[g + 1] += out.parent_offsets[g];
        out.child_offsets[g + 1] += out.child_offsets[g];
    }
    out.parents.resize(links.size());
    out.children.resize(links.size());
    std::vector<u32> parent_fill(out.parent_offsets.begin(), out.parent_offsets.end() - 1);
    std::vector<u32> child_fill(out.child_offsets.begin(), out.child_offsets.end() - 1);
    for (const auto& [child, parent] : links) {
        out.parents[parent_fill[child]++] = parent;
        out.children[child_fill[parent]++] = child;
    }
    return out;
}

} // namespace giant::geometry
