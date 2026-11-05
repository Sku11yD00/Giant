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
// in (ready), so every mesh added can be drawn a frame or a few later.
//
// Each frame culling asks for groups it needed but found missing, and marks
// the loaded ones it used. The requests come back to the CPU a couple of
// frames later; the streamer loads them within a per-frame budget, coarse
// before fine, making room by evicting groups no frame has used for a while
// (least recently used first). Everything a frame loads, terminal groups
// included, fits the per-frame budget, so adding many meshes at once spreads
// over frames instead of making one frame upload them all (rule 9). Geometry
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

struct StreamingStats {
    u64 pool_bytes = 0;
    u64 used_bytes = 0;
    u32 resident_groups = 0; // loaded, of every mesh
    u32 total_groups = 0;
    u32 loaded = 0;  // this frame
    u32 evicted = 0; // this frame
    u32 waiting = 0; // asked for but not loaded this frame (budget, or the pool is full)
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
    bool add(u32 mesh, const geometry::ClusterMesh& data, std::vector<gpu::Cluster>& clusters);
    void remove(u32 mesh);

    // True once a mesh's terminal groups are loaded: only then can it draw.
    bool ready(u32 mesh) const;
    // Per group of a mesh: nonzero while loaded (CPU culling).
    const std::vector<u8>& resident(u32 mesh) const;
    rhi::BufferHandle residency_table(u32 mesh) const;
    u32 group_base(u32 mesh) const;

    rhi::BufferHandle pool() const { return m_pool; }
    rhi::BufferHandle requests() const { return m_requests; }
    rhi::BufferHandle usage() const { return m_usage; }

    // Wanted and used groups found by CPU culling this frame.
    void want(u32 mesh, std::span<const u32> groups);
    void touch(u32 mesh, std::span<const u32> groups, u32 frame_number);
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
    struct MeshState {
        bool live = false;
        bool roots_loaded = false;
        u32 group_base = 0;
        std::vector<u8> blob;          // every group's geometry, back to back
        std::vector<u64> blob_offsets; // per group, plus the end: bytes into blob
        geometry::GroupLinks links;
        std::vector<u32> depth;
        std::vector<u8> root;
        std::vector<u8> resident;
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
    bool make_room(u64 units, u32 frame_number);
    static bool evictable(const MeshState& m, u32 group, u32 frame_number);
    void read_usage();
    bool grow_usage(rhi::CommandList* cmd); // null: at init, nothing to copy

    rhi::Device& m_device;
    u64 m_pool_bytes = 0;
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
    u32 m_total_groups = 0;
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
