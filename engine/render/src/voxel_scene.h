// The voxel copy of the scene around the camera (the shared voxel world,
// docs/architecture.md), kept on the GPU for far geometry and lighting.
//
// Each frame it moves the clipmap with the camera and works out which bricks
// need building: those that came into view, and those under instances that
// changed (moved, added, removed; materials or meshes changing rebuild
// everything). A brick is built on the CPU from the meshes' clusters at a
// detail matching its voxel size (a cut through the cluster LOD, so a
// million-triangle mesh voxelizes from a few hundred triangles at 16 m
// voxels), then copied into a fixed GPU pool. Building has a time budget per
// frame, coarse levels first, so the whole view is covered roughly before
// the near levels sharpen; until a brick is rebuilt, its old version stays.
//
// Built from plain renderer data: instances, materials and cluster meshes.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/geometry/clusters.h"
#include "giant/render/renderer.h"
#include "giant/rhi/rhi.h"
#include "giant/voxel/brick.h"
#include "giant/voxel/clipmap.h"

#include <deque>
#include <functional>
#include <span>
#include <unordered_map>
#include <vector>

namespace giant::render {

namespace gpu {
struct VoxelLevel;
}

struct VoxelStats {
    u32 bricks = 0;    // in the GPU pool
    u32 capacity = 0;  // the pool's size, bricks
    u32 pending = 0;   // waiting to be built (bricks, plus regions not yet looked at)
    u32 built = 0;     // built this frame (including ones that came out empty)
    u32 uploaded = 0;  // copied to the GPU this frame
    bool overflow = false; // the pool is full: some bricks are missing
    float cpu_ms = 0.0f;   // building this frame
};

class VoxelScene {
public:
    // A mesh by id, or null when it does not exist (any more).
    using MeshLookup = std::function<const geometry::ClusterMesh*(MeshId)>;

    VoxelScene(rhi::Device& device, const voxel::ClipmapDesc& clipmap, u32 brick_capacity);
    ~VoxelScene();
    VoxelScene(const VoxelScene&) = delete;
    VoxelScene& operator=(const VoxelScene&) = delete;

    // Creates the pool and slot tables (outside a frame). False if out of memory.
    bool init();

    // Inside a frame, before anything reads the voxels. `meshes_version`
    // changes whenever a mesh is added or removed. Builds for at most
    // `budget_ms` (at least one brick, if any wait).
    void update(rhi::CommandList& cmd, const DVec3& camera, std::span<const Instance> instances,
                std::span<const Material> materials, u64 meshes_version, const MeshLookup& meshes,
                float budget_ms);

    // Per level, for the shaders: `out` holds levels() entries.
    void write_levels(gpu::VoxelLevel* out, const DVec3& camera) const;
    const voxel::Clipmap& clipmap() const { return m_clipmap; }
    u32 levels() const { return m_clipmap.levels(); }
    rhi::BufferHandle pool() const { return m_pool; }
    rhi::BufferHandle table() const { return m_table; }
    const VoxelStats& stats() const { return m_stats; }

    // The pool index of a brick, or gpu::k_no_brick (tests, queries).
    u32 brick_index(u32 level, const voxel::BrickCoord& brick) const;

    // The clusters of `mesh` whose error is at most `error` while their
    // group's is above it: one cut through its LOD covering the whole
    // surface. Exposed for tests.
    static std::vector<u32> cut(const geometry::ClusterMesh& mesh, float error);

private:
    struct Box {
        DVec3 min{};
        DVec3 max{};
    };
    struct Key {
        u32 level = 0;
        voxel::BrickCoord brick;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        usize operator()(const Key& k) const;
    };
    struct Pending {
        std::vector<u32> instances; // into this frame's instances
        u32 stamp = 0;              // the queue() call that last restarted the list
    };
    struct MeshCache {
        const geometry::ClusterMesh* mesh = nullptr;
        Vec3 min{}; // mesh-space bounds of its vertices
        Vec3 max{};
        std::unordered_map<i32, std::vector<u32>> cuts; // by log2 of the error
    };

    bool same_instance(const Instance& a, const Instance& b) const;
    Box bounds_of(const Instance& in, const MeshLookup& meshes);
    MeshCache* mesh_cache(MeshId id, const MeshLookup& meshes);
    // A box whose bricks are to be found (queue()), a step at a time.
    struct Job {
        voxel::BrickBox box;
        u32 next = 0;  // the next instance to look at
        u32 stamp = 0; // marks the lists it restarted (0: not started)
    };
    // Finds the instances over each brick of the job's box, `count`
    // instances at a time; true when done.
    bool queue(Job& job, std::span<const Instance> instances, const MeshLookup& meshes, u32 count);
    void add_job(const Box& box); // the bricks under a world box, every level
    void evict_outside();
    bool build(rhi::CommandList& cmd, const Key& key, const Pending& pending, std::span<const Instance> instances,
               std::span<const Material> materials, const MeshLookup& meshes, bool& pool_writable);
    void remove(u32 level, u32 slot);

    rhi::Device& m_device;
    voxel::Clipmap m_clipmap;
    u32 m_capacity = 0;
    rhi::BufferHandle m_pool;
    rhi::BufferHandle m_table;

    // Per level and slot: the pool index there (or k_no_brick) and the
    // brick it holds.
    std::vector<u32> m_slots;
    std::vector<voxel::BrickCoord> m_slot_bricks;
    std::vector<bool> m_level_dirty;
    std::vector<u32> m_free; // pool indices
    u32 m_used = 0;

    // Work: bricks to build, coarsest level first.
    std::unordered_map<Key, Pending, KeyHash> m_pending;
    std::vector<std::deque<Key>> m_order; // per level
    // Boxes whose bricks are still to be found (queue()), oldest first.
    std::deque<Job> m_jobs;
    u32 m_stamp = 0;

    // What the bricks were built from, to see what changed.
    std::vector<Instance> m_instances;
    std::vector<Box> m_bounds;
    u32 m_bounds_ready = 0; // m_bounds is worked out for instances below this
    std::vector<Vec3> m_colors;
    u64 m_meshes_version = ~u64{0};
    std::unordered_map<MeshId, MeshCache> m_meshes;

    voxel::BrickBuilder m_builder;
    VoxelStats m_stats;
    bool m_overflow_logged = false;
};

} // namespace giant::render
