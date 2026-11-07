// Cluster LOD: the virtual-geometry form of a mesh (the Nanite method).
//
// On import a mesh is cut into clusters of up to 128 triangles. Clusters are
// grouped, each group is simplified to about half its triangles with its
// outer border locked, and the result is cut into new clusters, level after
// level, until one group is left. That gives a DAG: coarse clusters cover the
// same surface as the finer ones they came from. Each frame a "cut" through
// it is chosen so every cluster's simplification error is under about one
// pixel; because group borders are locked, neighbouring clusters from
// different levels still meet without cracks.
//
// Built with meshoptimizer and its clusterlod.h (third_party/README.md).
// Everything is in mesh space; LodView carries the instance transform.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/geometry/mesh.h"

#include <array>
#include <span>
#include <vector>

namespace giant::geometry {

inline constexpr u32 k_cluster_max_triangles = 128;
inline constexpr u32 k_cluster_max_vertices = 128;

struct Sphere {
    Vec3 center{};
    float radius = 0.0f;
};

// A sphere with the simplification error (metres) of the geometry it
// bounds. k_terminal_error marks the coarsest clusters: never replaced.
struct LodBounds {
    Vec3 center{};
    float radius = 0.0f;
    float error = 0.0f;
};
inline constexpr float k_terminal_error = 3.402823466e+38f; // FLT_MAX

struct Cluster {
    u32 first_index = 0;    // into ClusterMesh::indices
    u32 triangle_count = 0; // 1 to k_cluster_max_triangles
    u32 vertex_count = 0;   // distinct vertices, at most k_cluster_max_vertices
    u32 group = 0;          // the group it belongs to
    i32 refined = -1;       // the group it was simplified from; -1 for original triangles
    Sphere bounds;          // tight, for culling
};

// Clusters of one group are contiguous. `simplified` is the error and
// bounds of the group's simplified replacement (its clusters are drawn
// while that replacement would be too coarse).
struct ClusterGroup {
    u32 depth = 0; // 0 holds the original triangles
    u32 first_cluster = 0;
    u32 cluster_count = 0;
    LodBounds simplified;
};

// Spatial tree over the groups, for fast cuts: one tree per depth (nodes
// 0 to levels - 1 are the roots). A node's bounds cover the worst error of
// every group below it, so a node that is precise enough is skipped whole.
struct ClusterNode {
    LodBounds bounds;
    i32 group = -1; // leaf: the group; internal: -1
    u32 child_offset = 0;
    u32 child_count = 0;
};

struct ClusterMesh {
    std::vector<Vertex> vertices; // shared by every level
    std::vector<u32> indices;     // per cluster, into vertices
    std::vector<Cluster> clusters;
    std::vector<ClusterGroup> groups; // in increasing depth
    std::vector<ClusterNode> nodes;
    u32 levels = 0;
    Sphere bounds; // the whole mesh
    u32 source_triangles = 0;

    bool empty() const { return clusters.empty(); }
};

// Builds the hierarchy. Indices must be in range (asserted); degenerate
// triangles and duplicate vertices are fine. An empty mesh gives an empty
// result. Single-threaded: a few seconds per million triangles in release.
// Vertices marked non-zero in `locked` (one per mesh vertex, or empty) keep
// their place at every level: the edges of terrain tiles, which must meet
// their neighbours' at any detail.
ClusterMesh build_clusters(const TriangleMesh& mesh, std::span<const u8> locked = {});

// A skinned mesh's clusters: its original triangles alone, one level (every
// group terminal: a pose moves the vertices, so simplification errors
// measured at bind would not hold), vertices merged only where their
// weights match too. `weights` is one per ClusterMesh vertex. Empty when the
// mesh is, or it has not one weight per vertex.
struct SkinnedClusterMesh {
    ClusterMesh mesh;
    std::vector<SkinWeights> weights;
};
SkinnedClusterMesh build_skinned_clusters(const SkinnedTriangleMesh& mesh);

// Planes (xyz normal pointing inside, w offset) in camera-relative space.
struct Frustum {
    std::array<Vec4, 5> planes{}; // left, right, bottom, top, near (no far plane)
};
// From a reverse-Z, infinite-far view-projection matrix (Renderer).
Frustum frustum_from_view_projection(const Mat4& view_projection);
bool sphere_in_frustum(const Frustum& frustum, const Vec3& center, float radius);

// How one instance of a mesh is seen.
struct LodView {
    // Mesh space to camera-relative space.
    Vec3 position{};
    Quat rotation{};
    Vec3 scale{1.0f, 1.0f, 1.0f};
    // An error of e metres at distance d covers e / d * pixels_per_unit
    // pixels: 0.5 * cot(vertical_fov / 2) * viewport height in pixels.
    float pixels_per_unit = 1000.0f;
    float near_plane = 0.1f;
    float threshold_pixels = 1.0f;
    const Frustum* frustum = nullptr; // null: no culling
};

// The error of `bounds` on screen, in pixels; larger for coarser geometry
// (which the cut relies on). Non-uniform scale uses the largest axis.
float projected_error(const LodBounds& bounds, const LodView& view);

// Streaming: which groups' geometry is loaded. A group that is not loaded
// counts as precise enough, so the coarser clusters made from it are drawn
// instead; the cut stays whole as long as every loaded group's parents are
// loaded too (GroupLinks), which holds from the terminal groups down.
struct LodResidency {
    const std::vector<u8>* resident = nullptr; // per group: nonzero when loaded
    std::vector<u32>* wanted = nullptr;        // appends groups needed but not loaded
    std::vector<u32>* used = nullptr;          // appends loaded groups the cut reached
};

// Appends the clusters of the cut for `view` (indices into mesh.clusters),
// skipping clusters outside the frustum. Every part of the surface is
// covered by exactly one chosen cluster.
void select_clusters(const ClusterMesh& mesh, const LodView& view, std::vector<u32>& out,
                     const LodResidency* residency = nullptr);

// A group's parents are the coarser groups holding clusters simplified from
// it; its children the finer groups its clusters were simplified from.
// Terminal groups (no replacement) have no parents: they are the roots.
struct GroupLinks {
    std::vector<u32> parent_offsets; // groups + 1 entries: parents of g are
    std::vector<u32> parents;        // parents[parent_offsets[g] .. parent_offsets[g + 1])
    std::vector<u32> child_offsets;
    std::vector<u32> children;

    std::span<const u32> parents_of(u32 group) const {
        return {parents.data() + parent_offsets[group], parent_offsets[group + 1] - parent_offsets[group]};
    }
    std::span<const u32> children_of(u32 group) const {
        return {children.data() + child_offsets[group], child_offsets[group + 1] - child_offsets[group]};
    }
};
GroupLinks link_groups(const ClusterMesh& mesh);
inline bool is_root_group(const ClusterGroup& group) { return group.simplified.error >= k_terminal_error; }

} // namespace giant::geometry
