// The voxel copy of the scene around the camera (the shared voxel world,
// docs/architecture.md), kept on the GPU for far geometry and lighting.
//
// It keeps its own copy of the instances it was built from (with their
// bounds and resolved colours) in a coarse spatial grid, and each frame
// compares the renderer's instances against it. What changed becomes work:
// - the slabs the clipmap windows move into: their slots read "unbuilt"
//   (the shaders fall back to the next coarser level there) until the
//   instances over them are found and their bricks built;
// - an instance that moved, changed, was added or removed: the bricks under
//   where it was and where it is are built again;
// - many changes at once (or a removal from the middle that is not one
//   contiguous run) rebuild every window, never more than one at a time.
// Everything is a queue worked through under a time budget per frame
// (RenderSettings::voxel_ms_per_frame): finding bricks is split into
// resumable steps, and bricks are built coarse levels first, so the view is
// covered roughly before it sharpens; a coarse brick over thousands of
// instances is built over several frames. Until a brick is rebuilt, its old
// version stays.
//
// A brick is built on the CPU from the meshes' clusters at a detail matching
// its voxel size (a cut through the cluster LOD, so a million-triangle mesh
// voxelizes from a few hundred triangles at 16 m voxels), then copied into a
// fixed GPU pool. When the pool is full, bricks that did not fit wait on a
// retry list (VoxelStats::missing) and are built as room frees up.
//
// Known costs left: comparing the instances is a pass over all of them each
// frame, and a full rebuild copies them all in one frame (fine for tens of
// thousands; world streaming will hand over changes instead).
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/geometry/clusters.h"
#include "giant/render/renderer.h"
#include "giant/rhi/rhi.h"
#include "giant/voxel/brick.h"
#include "giant/voxel/clipmap.h"

#include <array>
#include <deque>
#include <functional>
#include <limits>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace giant::render {

namespace gpu {
struct VoxelLevel;
}

struct VoxelStats {
    u32 bricks = 0;    // in the GPU pool
    u32 capacity = 0;  // the pool's size, bricks
    u32 pending = 0;   // waiting: bricks to build, plus regions and changes not yet looked at
    u32 built = 0;     // built this frame (including ones that came out empty)
    u32 uploaded = 0;  // copied to the GPU this frame
    u32 missing = 0;   // bricks that did not fit in the pool (waiting for room)
    bool overflow = false; // missing > 0: the pool is full
    float cpu_ms = 0.0f;   // voxel work this frame
};

// What the voxel world holds at a point, on one level.
enum class VoxelProbe : u8 {
    outside,  // not in the level's window
    building, // in it, not built yet (the shaders use the next coarser level)
    empty,    // the voxel there is empty
    solid,    // the voxel there is filled
};
const char* to_string(VoxelProbe probe);

// An instance's bounding radius as the far field judges it: the mesh's
// sphere times the largest |scale|, in float (as the GPU computes it).
float stand_in_radius_of(const geometry::ClusterMesh& mesh, const Vec3& scale);

class VoxelScene {
public:
    // A mesh by id, or null when it does not exist (any more). Ids are never
    // reused, and a mesh never changes once added.
    using MeshLookup = std::function<const geometry::ClusterMesh*(MeshId)>;

    VoxelScene(rhi::Device& device, const voxel::ClipmapDesc& clipmap, u32 brick_capacity);
    ~VoxelScene();
    VoxelScene(const VoxelScene&) = delete;
    VoxelScene& operator=(const VoxelScene&) = delete;

    // Creates the pool and slot tables (outside a frame). False if out of memory.
    bool init();

    // Inside a frame, before anything reads the voxels. `meshes_version`
    // changes whenever a mesh is added or removed. Works for about
    // `budget_ms` (at least one step and one brick, if any wait).
    // With `cells` (FrameInput::cells), only the cells that changed since
    // the last update are compared.
    void update(rhi::CommandList& cmd, const DVec3& camera, std::span<const Instance> instances,
                std::span<const CellRange> cells, std::span<const Material> materials, u64 meshes_version,
                const MeshLookup& meshes, float budget_ms);
    // Instances whose bounding radius (the mesh's sphere times the largest
    // |scale|) is at most this are stand-ins: their voxels carry the
    // stand-in bit the far field shows (it stands in for them; bigger
    // ones stay meshes, whose own voxels it must not draw over them).
    // A change rebuilds the voxel world. Default 0: none.
    void set_stand_in_radius(float radius);
    // A mesh was removed (its id may come back as another mesh): its cached
    // cuts go, and instances that used it are looked at again.
    void forget_mesh(MeshId mesh);
    // A frame without update() (the voxel world switched off): this
    // frame's stats are zero, the bricks stay.
    void idle();

    // Per level, for the shaders: `out` holds levels() entries.
    void write_levels(gpu::VoxelLevel* out, const DVec3& camera) const;
    const voxel::Clipmap& clipmap() const { return m_clipmap; }
    u32 levels() const { return m_clipmap.levels(); }
    rhi::BufferHandle pool() const { return m_pool; }
    rhi::BufferHandle table() const { return m_table; }
    const VoxelStats& stats() const { return m_stats; }

    // Whether the voxel world shows instance `index` (of the last update's
    // instances) as it is now: a stand-in, inside the coarsest window, with every brick
    // under it on every level built from it, none waiting to be found,
    // built again or for room in the pool. Until then (just added or
    // moved, a slab just come into view, a rebuild, a full pool) it must
    // stay a mesh, or it would vanish.
    bool holds(u32 index) const;

    // Pool brick `index`'s level and brick; false when it holds none.
    bool pool_brick(u32 index, u32& level, voxel::BrickCoord& brick) const;
    u32 capacity() const { return m_capacity; }
    // This frame's copies into the pool (by the last update): bricks put in
    // a pool index of their own (new there: whatever it held before was
    // another brick), and bricks built again in place.
    std::span<const u32> placed() const { return m_placed; }
    std::span<const u32> rebuilt() const { return m_rebuilt; }
    // Bricks put in a pool index of their own so far, and when pool index
    // `index` last had one put in (that count then; 0: never), so a reader
    // that skipped frames finds what was placed meanwhile.
    u64 placements() const { return m_placements; }
    u64 placed_at(u32 index) const { return index < m_placed_at.size() ? m_placed_at[index] : 0; }

    // The pool index of a brick, or gpu::k_no_brick (none, or not built yet).
    u32 brick_index(u32 level, const voxel::BrickCoord& brick) const;
    // What level `level` holds at a world position (from a CPU copy of the
    // occupancy; tests, the editor's probe_voxels).
    VoxelProbe probe(u32 level, const DVec3& position) const;

    // The clusters of `mesh` whose error is at most `error` while their
    // group's is above it: one cut through its LOD covering the whole
    // surface. Exposed for tests.
    static std::vector<u32> cut(const geometry::ClusterMesh& mesh, float error);

private:
    struct Box { // empty by default
        DVec3 min{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
                  std::numeric_limits<double>::infinity()};
        DVec3 max{-std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                  -std::numeric_limits<double>::infinity()};
        bool empty() const { return !(min.x <= max.x && min.y <= max.y && min.z <= max.z); }
    };
    struct Key {
        u32 level = 0;
        voxel::BrickCoord brick;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        usize operator()(const Key& k) const;
    };
    struct Cell {
        i32 x = 0;
        i32 y = 0;
        i32 z = 0;
        bool operator==(const Cell&) const = default;
    };
    struct CellHash {
        usize operator()(const Cell& c) const;
    };
    // An instance as the bricks were (or are being) built from it.
    struct Copy {
        Instance instance;
        Vec3 color{}; // its material's, resolved
        Box box;      // world bounds (empty: no mesh)
        bool large = false; // in m_large, not the grid
        bool stand_in = false; // small enough for the far field to stand in for
    };
    struct MeshCache {
        const geometry::ClusterMesh* mesh = nullptr; // null: missing
        Vec3 min{}; // mesh-space bounds of its original clusters
        Vec3 max{};
        std::unordered_map<i32, std::vector<u32>> cuts; // by log2 of the error
    };
    enum class JobKind : u8 {
        find,    // mark the bricks under the instances over the box, then
                 // empty the box's unbuilt slots nothing is over
        present, // mark the box's bricks that are in the pool
        all,     // mark every brick of the box
    };
    struct Job {
        voxel::BrickBox box;
        JobKind kind = JobKind::find;
        bool rebuild = false;   // part of a full rebuild
        bool started = false;
        std::vector<u32> found; // find: instances over the box
        u32 next = 0;           // find: the next of found (found.size(): sweeping)
        u64 cursor = 0;         // bricks done of the current instance, sweep or box
    };

    MeshCache* mesh_cache(MeshId id, const MeshLookup& meshes);
    Box bounds_of(const Instance& in, const MeshLookup& meshes);
    Vec3 color_of(const Instance& in, std::span<const Material> materials) const;
    bool differs(const Instance& in, std::span<const Material> materials, const Copy& copy) const;

    // The grid of instance copies.
    void grid_insert(u32 index);
    void grid_erase(u32 index);
    void grid_rebuild();
    void find(const Box& box); // into m_found, each instance once

    // `only`: the cells to compare (the rest are as last time); null: all.
    void see_changes(std::span<const Instance> instances, std::span<const Material> materials,
                     const std::vector<CellRange>* only, u64 meshes_version, const MeshLookup& meshes);
    void set_copy(u32 index, const Instance& in, std::span<const Material> materials, const MeshLookup& meshes);
    void change(u32 index, const Instance& in, std::span<const Material> materials, const MeshLookup& meshes);
    void copy_all(std::span<const Instance> instances, std::span<const Material> materials,
                  const MeshLookup& meshes);
    void remove_range(u32 first, u32 count);
    void request_rebuild();
    void push_rebuild();
    void add_jobs(const Box& box, JobKind kind);
    void enter(const voxel::BrickBox& box);

    void mark(u32 level, const voxel::BrickCoord& brick);
    bool pending(u32 level, const voxel::BrickCoord& brick) const;
    // Works on the job until done (true) or out of time.
    template <typename OutOfTime>
    bool run_job(Job& job, const OutOfTime& out_of_time);
    void job_done(const Job& job);

    // Builds the brick under way (m_building), a few instances at a time
    // until `out_of_time`: done (true), or more next frame (false).
    template <typename OutOfTime>
    bool build_step(const MeshLookup& meshes, const OutOfTime& out_of_time);
    // Copies the finished brick to the pool; false if out of memory.
    bool finish_build(rhi::CommandList& cmd, bool& pool_writable);
    void touch_building(const Box& box); // a change there: the brick under way is built again after
    void set_slot(u32 level, u32 slot, u32 value, const voxel::BrickCoord& brick);
    void remove(u32 level, u32 slot, const voxel::BrickCoord& brick); // to k_no_brick, freeing its brick
    void forget_stand_in(u32 level, u32 index); // a pool brick freed or rebuilt
    void grow_filled(u32 level, const voxel::BrickCoord& brick);
    void tighten_filled(u32 level);
    void upload_tables(rhi::CommandList& cmd);

    rhi::Device& m_device;
    voxel::Clipmap m_clipmap;
    u32 m_capacity = 0;
    rhi::BufferHandle m_pool;
    rhi::BufferHandle m_table;

    // Per level and slot: the pool index there (k_no_brick, k_unbuilt) and
    // the brick it is for.
    std::vector<u32> m_slots;
    std::vector<voxel::BrickCoord> m_slot_bricks;
    std::vector<u32> m_dirty_lo; // per level: slots [lo, hi) to upload
    std::vector<u32> m_dirty_hi;
    std::vector<u32> m_unbuilt;  // per level: k_unbuilt slots
    std::vector<voxel::BrickBox> m_filled; // per level: around its bricks
    std::vector<bool> m_loose;   // per level: m_filled may be loose
    std::vector<u32> m_free;     // pool indices
    std::vector<u32> m_owner;    // per pool index: the slot (level * slots + slot) it was put in last
    std::vector<u32> m_placed;   // this frame's (placed())
    std::vector<u32> m_rebuilt;
    std::vector<u64> m_placed_at; // per pool index (placed_at())
    u64 m_placements = 0;
    u32 m_used = 0;
    std::vector<std::array<u32, voxel::k_brick_voxels / 32>> m_occupancy; // per pool index (probe)
    std::vector<u8> m_has_stand_in;      // per pool index: any stand-in voxel
    std::vector<u32> m_stand_in_bricks;  // per level: pool bricks with stand-in voxels

    // Work. Bricks to build, coarsest level first:
    std::unordered_set<Key, KeyHash> m_pending;
    std::vector<std::deque<Key>> m_order; // per level (may hold stale keys)
    std::unordered_set<Key, KeyHash> m_retry; // did not fit in the pool
    // Boxes whose bricks are still to be found or marked, oldest first:
    std::deque<Job> m_jobs;
    u32 m_rebuild_jobs = 0;    // of a full rebuild, still queued
    bool m_rebuild_again = false;
    std::vector<voxel::BrickBox> m_job_bounds; // per level: around its queued jobs (count 0: none), for holds()
    std::vector<std::vector<voxel::BrickBox>> m_job_boxes; // per level: the queued jobs' boxes
    float m_stand_in_radius = 0.0f;
    // Changes not turned into jobs yet: where instances were (their bricks
    // are built again) and which instances changed (the bricks where they
    // are now are).
    std::deque<Box> m_old;
    std::unordered_set<u32> m_changed;
    std::deque<u32> m_changed_order;

    // What the bricks are built from.
    std::vector<Copy> m_copies;
    std::vector<CellRange> m_seen_cells;    // FrameInput::cells as last seen
    std::vector<CellRange> m_changed_cells; // this update's (scratch)
    std::unordered_map<Cell, std::vector<u32>, CellHash> m_grid;
    std::vector<u32> m_large; // instances spanning too many cells
    std::vector<u32> m_seen;  // per copy: the find() that last saw it
    u32 m_find_stamp = 0;
    std::vector<u32> m_found;
    u64 m_meshes_version = ~u64{0};
    std::unordered_map<MeshId, MeshCache> m_meshes;
    std::vector<MeshId> m_forgotten; // by forget_mesh, for the next see_changes

    // The brick being built (a coarse one over thousands of instances
    // takes several frames).
    struct Building {
        bool active = false;
        Key key;
        Box box;                // its world box
        std::vector<u32> found; // instances over it when started
        u32 next = 0;
        bool again = false;     // something over it changed meanwhile
    };
    Building m_building;
    voxel::BrickBuilder m_builder;
    VoxelStats m_stats;
    bool m_overflow_logged = false;
};

} // namespace giant::render
