// Cluster streaming: only the geometry the view needs is in GPU memory.
//
// Every mesh keeps its metadata (clusters, groups, LOD tree) on the GPU, but
// the geometry itself (vertices and triangles) lives group by group in one
// shared pool of fixed size. Each mesh has a residency table: per group,
// where its geometry is in the pool, or not loaded.
//
// Culling treats a group that is not loaded as precise enough, so the
// coarser clusters made from it are drawn instead; that keeps the cut whole
// as long as a loaded group's parents are loaded too, which the streamer
// guarantees (parents load first, and a group is only evicted once none of
// its children are loaded). The terminal groups (the coarsest) load before
// anything else and are never evicted; a mesh is drawn once all of its are
// in (ready), so every mesh added can be drawn a frame or a few later, if
// the pool can hold it.
//
// Each frame culling asks for groups it needed but found missing, and marks
// the loaded ones it used. The requests come back to the CPU a couple of
// frames later; the streamer loads them within a per-frame budget, coarse
// before fine, making room by evicting groups no frame has used for a while
// (least recently used first). Everything a frame loads, terminal groups
// included, fits the per-frame budget, so adding many meshes at once spreads
// over frames instead of making one frame upload them all (rule 9). New
// meshes' terminal groups load in the order the meshes were added (except
// that a mesh the pool cannot hold yet does not block the rest), and detail
// waits while any of them waits for the budget. Geometry
// comes from a CPU copy for now; the world's streaming (cells, from disk)
// will feed the same path.
//
// Pool and residency writes are copies recorded at the start of a frame, on
// the one queue: the GPU finishes earlier frames (which read the old state)
// before it runs them, so space can be reused at once.
#pragma once

#include "giant/core/types.h"
#include "giant/geometry/clusters.h"
#include "giant/rhi/rhi.h"
#include "gpu_layout.h"

#include <map>
#include <memory>
#include <span>
#include <vector>

namespace giant::render {

// Best-fit allocator over `capacity` units, with neighbouring free ranges
// merged. CPU bookkeeping only.
class PoolAllocator {
public:
    static constexpr u64 k_none = ~u64{0};

    explicit PoolAllocator(u64 capacity = 0);
    // The first unit of a free range of `units`, or k_none.
    u64 allocate(u64 units);
    // `offset` and `units` as allocate gave and was asked.
    void free(u64 offset, u64 units);
    u64 capacity() const { return m_capacity; }
    u64 used() const { return m_used; }
    u64 largest_free() const;
    usize free_ranges() const { return m_by_offset.size(); }

private:
    void insert(u64 offset, u64 units);
    void erase(std::map<u64, u64>::iterator at);

    u64 m_capacity = 0;
    u64 m_used = 0;
    std::map<u64, u64> m_by_offset;    // free ranges: offset -> units
    std::multimap<u64, u64> m_by_size; // the same: units -> offset
};

// How a cluster's geometry is laid out in the pool: its distinct vertices
// (indices into ClusterMesh::vertices) in the order of first use, and its
// triangles as three local indices 8 bits apart. Its scratch is reused
// between calls (sized to the mesh's vertices).
struct ClusterPacking {
    std::vector<u32> order;
    std::vector<u32> triangles;
    void pack(const geometry::ClusterMesh& data, u32 cluster);

private:
    const geometry::ClusterMesh* mesh = nullptr; // the scratch's
    std::vector<u32> seen; // the cluster that last numbered a vertex
    std::vector<u32> local;
};

struct StreamingStats {
    u64 pool_bytes = 0;
    u64 used_bytes = 0;
    u32 resident_groups = 0; // loaded, of every mesh
    u32 total_groups = 0;
    u32 loaded = 0;  // this frame
    u32 evicted = 0; // this frame
    u32 waiting = 0; // asked for, or a new mesh's terminal groups, not loaded this frame (budget, or the pool is full)
};

class GeometryStreamer {
public:
    static constexpr u64 k_unit = 16;            // pool allocation granule, bytes
    static constexpr u32 k_request_capacity = 1u << 14;
    static constexpr u32 k_usage_interval = 8;   // frames between usage read-backs
    static constexpr u32 k_keep_frames = 30;     // unused this long before eviction

    // What one update may load (terminal groups and detail together). The
    // first group of an update always loads, so nothing waits forever.
    struct Budget {
        u64 bytes_per_frame = 32ull << 20;
        u32 groups_per_frame = 4096;
    };

    GeometryStreamer(rhi::Device& device, u64 pool_bytes);
    ~GeometryStreamer();
    GeometryStreamer(const GeometryStreamer&) = delete;
    GeometryStreamer& operator=(const GeometryStreamer&) = delete;

    // Creates the pool and request buffers; false (logged) without memory.
    // The pool is at most 4 GiB (residency offsets are 32-bit).
    bool init();
    void set_budget(const Budget& budget) { m_budget = budget; }

    // Lays out `data`'s geometry for streaming, writing each cluster's
    // vertex and triangle offsets into `clusters` (one per data.clusters).
    // Its terminal groups load from the next update on, within the per-frame
    // budget; it draws once they are all in (ready). False (logged) when a
    // cluster is over the limits or the GPU is out of memory.
    //
    // A posed mesh (skinned: a pass moves its loaded vertices to a pose)
    // shows only its groups from a floor depth up, plus its terminal ones:
    // the rest count as not loaded for culling, so the pose pass need not
    // move detail no view draws. See aim_floor.
    bool add(u32 mesh, const geometry::ClusterMesh& data, std::vector<gpu::Cluster>& clusters, bool posed = false);
    // Another mesh streaming the same geometry as `of` (a posed copy: a
    // character drawn in its own pose), sharing its laid-out geometry; its
    // own residency, floor and place in the pool. The clusters' offsets are
    // those `of` was given. False when `of` is not live.
    bool add_copy(u32 mesh, u32 of);
    void remove(u32 mesh);

    // True once a mesh's terminal groups are loaded: only then can it draw.
    bool ready(u32 mesh) const;
    // Per group of a mesh: nonzero while loaded and shown (CPU culling;
    // the residency table on the GPU holds the same).
    const std::vector<u8>& shown(u32 mesh) const;
    // Per group of a mesh: nonzero while loaded.
    const std::vector<u8>& resident(u32 mesh) const;
    // A posed mesh's floor: groups finer (shallower) than this depth are
    // hidden, terminal groups excepted. A request for a hidden group lowers
    // it at the next update (coarser clusters stand in meanwhile, as for any
    // group not loaded); it rises to the aim once no group below the aim
    // was used or asked for in k_keep_frames. The aim is the coarsest floor
    // the renderer's own estimate (the camera's) allows: below the floor, it
    // lowers it at the next update too. Unposed meshes: always 0.
    void aim_floor(u32 mesh, u32 depth);
    u32 floor(u32 mesh) const;
    // Changes whenever one of a mesh's groups starts showing (loaded, or
    // the floor lowered): a posed mesh's new groups hold the bind pose then.
    u32 shown_version(u32 mesh) const;
    rhi::BufferHandle residency_table(u32 mesh) const;
    u32 group_base(u32 mesh) const;

    rhi::BufferHandle pool() const { return m_pool; }
    rhi::BufferHandle requests() const { return m_requests; }
    rhi::BufferHandle usage() const { return m_usage; }

    // Wanted and used groups found by CPU culling this frame.
    void want(u32 mesh, std::span<const u32> groups);
    void touch(u32 mesh, std::span<const u32> groups, u32 frame_number);
    // A usage mark as GPU culling leaves it (read back): `mark` is the
    // frame that reached the group, loaded or not. read_usage feeds these;
    // public for tests.
    void mark_used(u32 mesh, u32 group, u32 mark);
    // Requests GPU culling recorded in a frame whose counters were read back
    // in `slot` (read when the GPU is done with it), `count` as counted.
    void take_gpu_requests(u32 slot, u32 count);

    // At the start of a frame, before culling: loads, evicts, and updates
    // the residency tables; leaves the pool and tables ready to read.
    void update(rhi::CommandList& cmd, u32 frame_number);
    // After GPU culling: copies its requests (and every few frames the usage
    // marks) for reading back. `slot` is the frame's read-back slot.
    void after_cull(rhi::CommandList& cmd, u32 slot, u64 frame_index);

    // False when this frame's residency tables could not be uploaded (out
    // of memory): drawing would read stale places in the pool.
    bool tables_current() const { return m_tables_current; }

    // This frame's numbers (update fills them).
    const StreamingStats& stats() const { return m_stats; }

private:
    // A mesh's geometry laid out for the pool, and its group structure:
    // the same for every copy of it.
    struct Layout {
        std::vector<u8> blob;          // every group's geometry, back to back
        std::vector<u64> blob_offsets; // per group, plus the end: bytes into blob
        geometry::GroupLinks links;
        std::vector<u32> depth;
        std::vector<u8> root;
        u32 coarsest = 0; // the deepest group's depth
    };
    struct MeshState {
        bool live = false;
        bool roots_loaded = false;
        u32 group_base = 0;
        std::shared_ptr<const Layout> lay; // shared by copies
        std::vector<u8> resident;
        std::vector<u8> shown;      // resident, and not hidden under the floor
        bool posed = false;
        u32 floor = 0;
        u32 aim = 0;
        u32 held_until = 0;         // update count: the floor stays this low until then
        u32 shown_version = 0;
        std::vector<u64> pool_unit; // per loaded group: first unit in the pool
        std::vector<u32> last_used; // frame number
        std::vector<u32> loaded_children;
        std::vector<u32> wanted_mark; // update count when last wanted
        rhi::BufferHandle table;
        bool table_dirty = true;
    };
    struct Request {
        u32 mesh;
        u32 group;
    };
    struct Candidate {
        u32 age; // frames since last used
        u32 mesh;
        u32 group;
    };
    static bool candidate_order(const Candidate& a, const Candidate& b) { return a.age < b.age; } // oldest on top

    u64 units_of(const MeshState& m, u32 group) const;
    bool load(rhi::CommandList& cmd, MeshState& m, u32 mesh, u32 group, u32 frame_number, bool& pool_writable);
    void evict(MeshState& m, u32 group);
    bool start(u32 mesh, MeshState m, bool posed); // its own state, on m.lay
    static bool shows(const MeshState& m, u32 group) {
        return m.resident[group] != 0 && (m.lay->root[group] != 0 || m.lay->depth[group] >= m.floor);
    }
    void set_floor(MeshState& m, u32 floor);
    void move_floors(u32 frame_number);
    bool make_room(u64 units, u32 frame_number);
    static bool evictable(const MeshState& m, u32 group, u32 frame_number);
    void read_usage();
    void note_usage(MeshState& m, u32 group, u32 mark);
    bool grow_usage(rhi::CommandList* cmd); // null: at init, nothing to copy

    rhi::Device& m_device;
    u64 m_pool_bytes = 0;
    // Global group numbers for a mesh's groups (u32 each in the usage buffer).
    u32 take_groups(u32 count);
    void give_groups(u32 base, u32 count);

    Budget m_budget;
    PoolAllocator m_alloc;
    rhi::BufferHandle m_pool;
    rhi::BufferHandle m_requests;
    rhi::BufferHandle m_request_readback[3];
    rhi::BufferHandle m_usage;          // u32 per global group
    u32 m_usage_capacity = 0;           // groups
    rhi::BufferHandle m_usage_readback;
    u32 m_usage_readback_groups = 0;
    u64 m_usage_readback_frame = ~u64{0}; // pending copy, tagged; ~0 when none
    u32 m_total_groups = 0; // global group numbers handed out: the end of the last range
    // Ranges of global group numbers removed meshes left (first -> count),
    // joined, reused first-fit, so meshes coming and going do not grow the
    // usage buffer.
    std::map<u32, u32> m_free_groups;
    std::vector<MeshState> m_meshes;
    std::vector<Request> m_wanted;
    StreamingStats m_stats;
    u32 m_culls_since_usage = 0;
    u32 m_updates = 0;
    std::vector<Candidate> m_candidates; // eviction heap, built once per update when needed
    bool m_candidates_built = false;
    bool m_room_capped = false; // the last make_room stopped at its cap, candidates left
    bool m_tables_current = true;
    bool m_usage_logged = false;
    bool m_full_logged = false;
    bool m_roots_logged = false;
};

} // namespace giant::render
