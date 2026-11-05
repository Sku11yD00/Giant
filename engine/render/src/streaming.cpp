#include "streaming.h"

#include "giant/core/assert.h"
#include "giant/core/log.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <queue>
#include <string>

namespace giant::render {

// --- PoolAllocator -------------------------------------------------------------------

PoolAllocator::PoolAllocator(u64 capacity) : m_capacity(capacity) {
    if (capacity > 0) {
        insert(0, capacity);
    }
}

void PoolAllocator::insert(u64 offset, u64 units) {
    m_by_offset.emplace(offset, units);
    m_by_size.emplace(units, offset);
}

void PoolAllocator::erase(std::map<u64, u64>::iterator at) {
    const auto [first, last] = m_by_size.equal_range(at->second);
    for (auto it = first; it != last; ++it) {
        if (it->second == at->first) {
            m_by_size.erase(it);
            break;
        }
    }
    m_by_offset.erase(at);
}

u64 PoolAllocator::allocate(u64 units) {
    if (units == 0) {
        return k_none;
    }
    const auto best = m_by_size.lower_bound(units); // the smallest range that fits
    if (best == m_by_size.end()) {
        return k_none;
    }
    const u64 size = best->first;
    const u64 offset = best->second;
    m_by_size.erase(best);
    m_by_offset.erase(offset);
    if (size > units) {
        insert(offset + units, size - units);
    }
    m_used += units;
    return offset;
}

void PoolAllocator::free(u64 offset, u64 units) {
    GIANT_ASSERT(units > 0 && offset + units <= m_capacity && units <= m_used, "freeing what was not allocated");
    m_used -= units;
    u64 start = offset;
    u64 size = units;
    // Merge with the free range after, then the one before.
    auto next = m_by_offset.lower_bound(offset);
    GIANT_ASSERT(next == m_by_offset.end() || next->first >= offset + units, "double free");
    if (next != m_by_offset.end() && next->first == offset + units) {
        size += next->second;
        auto gone = next++;
        erase(gone);
    }
    if (next != m_by_offset.begin()) {
        auto prev = std::prev(next);
        GIANT_ASSERT(prev->first + prev->second <= offset, "double free");
        if (prev->first + prev->second == offset) {
            start = prev->first;
            size += prev->second;
            erase(prev);
        }
    }
    insert(start, size);
}

u64 PoolAllocator::largest_free() const { return m_by_size.empty() ? 0 : m_by_size.rbegin()->first; }

// --- GeometryStreamer ----------------------------------------------------------------

namespace {

constexpr u64 k_no_unit = ~u64{0};

// Whether frame number `a` is after `b` (wrapping).
bool later(u32 a, u32 b) { return static_cast<i32>(a - b) > 0; }

} // namespace

namespace {
// Residency tables hold byte offsets as u32.
constexpr u64 k_max_pool_bytes = (u64{1} << 32) - GeometryStreamer::k_unit;
u64 pool_size(u64 bytes) { return std::min(bytes, k_max_pool_bytes) / GeometryStreamer::k_unit * GeometryStreamer::k_unit; }
} // namespace

GeometryStreamer::GeometryStreamer(rhi::Device& device, u64 pool_bytes)
    : m_device(device), m_pool_bytes(pool_size(pool_bytes)), m_alloc(pool_size(pool_bytes) / k_unit) {}

GeometryStreamer::~GeometryStreamer() {
    for (MeshState& m : m_meshes) {
        m_device.destroy(m.table);
    }
    for (const rhi::BufferHandle b : {m_pool, m_requests, m_request_readback[0], m_request_readback[1],
                                      m_request_readback[2], m_usage, m_usage_readback}) {
        m_device.destroy(b);
    }
}

bool GeometryStreamer::init() {
    const auto make = [this](u64 size, rhi::BufferUsage usage, rhi::Memory memory, const char* name) {
        rhi::BufferDesc desc;
        desc.size = size;
        desc.usage = usage;
        desc.memory = memory;
        desc.name = name;
        return m_device.create_buffer(desc);
    };
    m_pool = make(std::max<u64>(m_pool_bytes, k_unit), rhi::BufferUsage::shader_read, rhi::Memory::gpu,
                  "geometry pool (streamed clusters)");
    m_requests = make(u64{k_request_capacity} * sizeof(gpu::Entry),
                      rhi::BufferUsage::shader_read | rhi::BufferUsage::shader_write, rhi::Memory::gpu,
                      "streaming requests");
    for (rhi::BufferHandle& r : m_request_readback) {
        r = make(u64{k_request_capacity} * sizeof(gpu::Entry), rhi::BufferUsage::none, rhi::Memory::readback,
                 "streaming requests (read back)");
    }
    for (const rhi::BufferHandle b : {m_pool, m_requests, m_request_readback[0], m_request_readback[1],
                                      m_request_readback[2]}) {
        if (!b.valid()) {
            log_error("render", "could not create the geometry pool (out of GPU memory?)");
            return false;
        }
    }
    m_stats.pool_bytes = m_pool_bytes;
    return grow_usage(nullptr);
}

bool GeometryStreamer::add(u32 mesh, const geometry::ClusterMesh& data, std::vector<gpu::Cluster>& clusters) {
    GIANT_ASSERT(clusters.size() == data.clusters.size(), "one gpu cluster per cluster");
    MeshState m;
    const usize groups = data.groups.size();
    m.blob_offsets.reserve(groups + 1);
    std::vector<u32> seen(data.vertices.size(), ~0u); // cluster that last numbered a vertex
    std::vector<u32> local(data.vertices.size(), 0);
    const auto append = [&m](const void* bytes, usize size) {
        const auto* p = static_cast<const u8*>(bytes);
        m.blob.insert(m.blob.end(), p, p + size);
    };
    for (u32 g = 0; g < groups; ++g) {
        const u64 start = m.blob.size();
        m.blob_offsets.push_back(start);
        const geometry::ClusterGroup& group = data.groups[g];
        for (u32 i = 0; i < group.cluster_count; ++i) {
            const u32 ci = group.first_cluster + i;
            const geometry::Cluster& c = data.clusters[ci];
            gpu::Cluster& out = clusters[ci];
            // Its distinct vertices, then its triangles as local indices.
            std::vector<u32> order;
            std::vector<u32> triangles;
            triangles.reserve(c.triangle_count);
            for (u32 t = 0; t < c.triangle_count; ++t) {
                u32 corner[3];
                for (u32 k = 0; k < 3; ++k) {
                    const u32 v = data.indices[c.first_index + t * 3 + k];
                    if (seen[v] != ci) {
                        seen[v] = ci;
                        local[v] = static_cast<u32>(order.size());
                        order.push_back(v);
                    }
                    corner[k] = local[v];
                }
                triangles.push_back(corner[0] | (corner[1] << 8) | (corner[2] << 16));
            }
            if (order.size() > geometry::k_cluster_max_vertices ||
                c.triangle_count > geometry::k_cluster_max_triangles) {
                log_error("render", "a cluster is over 128 vertices or triangles; mesh not added");
                return false;
            }
            out.vertex_count = static_cast<u32>(order.size());
            out.triangle_count = c.triangle_count;
            out.vertex_offset = static_cast<u32>(m.blob.size() - start);
            for (const u32 v : order) {
                append(&data.vertices[v], sizeof(geometry::Vertex));
            }
            out.triangle_offset = static_cast<u32>(m.blob.size() - start);
            append(triangles.data(), triangles.size() * 4);
        }
        m.blob.resize((m.blob.size() + k_unit - 1) / k_unit * k_unit, 0);
    }
    m.blob_offsets.push_back(m.blob.size());
    m.links = geometry::link_groups(data);
    m.depth.resize(groups);
    m.root.resize(groups);
    for (u32 g = 0; g < groups; ++g) {
        m.depth[g] = data.groups[g].depth;
        m.root[g] = geometry::is_root_group(data.groups[g]) ? 1 : 0;
    }
    m.resident.assign(groups, 0);
    m.pool_unit.assign(groups, k_no_unit);
    m.last_used.assign(groups, 0);
    m.loaded_children.assign(groups, 0);
    m.wanted_mark.assign(groups, 0);
    const std::vector<u32> empty(std::max<usize>(groups, 1), gpu::k_not_resident);
    rhi::BufferDesc desc;
    desc.size = empty.size() * 4;
    desc.usage = rhi::BufferUsage::shader_read;
    desc.name = "mesh residency table";
    m.table = m_device.create_buffer(desc, {reinterpret_cast<const u8*>(empty.data()), empty.size() * 4});
    if (!m.table.valid()) {
        log_error("render", "could not create a mesh's residency table (out of GPU memory?)");
        return false;
    }
    m.group_base = m_total_groups;
    m_total_groups += static_cast<u32>(groups);
    m_stats.total_groups += static_cast<u32>(groups);
    m.live = true;
    m.table_dirty = false; // made all "not loaded"
    if (mesh >= m_meshes.size()) {
        m_meshes.resize(mesh + 1);
    }
    m_device.destroy(m_meshes[mesh].table);
    m_meshes[mesh] = std::move(m);
    return true;
}

void GeometryStreamer::remove(u32 mesh) {
    if (mesh >= m_meshes.size() || !m_meshes[mesh].live) {
        return;
    }
    MeshState& m = m_meshes[mesh];
    for (u32 g = 0; g < m.resident.size(); ++g) {
        if (m.resident[g] != 0) {
            m_alloc.free(m.pool_unit[g], units_of(m, g));
            --m_stats.resident_groups;
        }
    }
    m_stats.total_groups -= static_cast<u32>(m.resident.size());
    m_stats.used_bytes = m_alloc.used() * k_unit;
    m_device.destroy(m.table); // deferred by the device past frames in flight
    m = {};
}

bool GeometryStreamer::ready(u32 mesh) const { return mesh < m_meshes.size() && m_meshes[mesh].roots_loaded; }

const std::vector<u8>& GeometryStreamer::resident(u32 mesh) const {
    static const std::vector<u8> none;
    return mesh < m_meshes.size() ? m_meshes[mesh].resident : none;
}

rhi::BufferHandle GeometryStreamer::residency_table(u32 mesh) const {
    return mesh < m_meshes.size() ? m_meshes[mesh].table : rhi::BufferHandle{};
}

u32 GeometryStreamer::group_base(u32 mesh) const { return mesh < m_meshes.size() ? m_meshes[mesh].group_base : 0; }

void GeometryStreamer::want(u32 mesh, std::span<const u32> groups) {
    for (const u32 g : groups) {
        m_wanted.push_back({mesh, g});
    }
}

void GeometryStreamer::touch(u32 mesh, std::span<const u32> groups, u32 frame_number) {
    if (mesh >= m_meshes.size()) {
        return;
    }
    MeshState& m = m_meshes[mesh];
    for (const u32 g : groups) {
        if (g < m.last_used.size()) {
            m.last_used[g] = frame_number;
        }
    }
}

void GeometryStreamer::take_gpu_requests(u32 slot, u32 count) {
    const u8* bytes = m_device.mapped(m_request_readback[slot % 3]);
    if (bytes == nullptr) {
        return;
    }
    const u32 n = std::min(count, k_request_capacity);
    for (u32 i = 0; i < n; ++i) {
        u32 pair[2];
        std::memcpy(pair, bytes + u64{i} * sizeof(pair), sizeof(pair));
        m_wanted.push_back({pair[0], pair[1]}); // checked in update
    }
}

u64 GeometryStreamer::units_of(const MeshState& m, u32 group) const {
    return (m.blob_offsets[group + 1] - m.blob_offsets[group]) / k_unit;
}

bool GeometryStreamer::load(rhi::CommandList& cmd, MeshState& m, u32 mesh, u32 group, u32 frame_number,
                            bool& pool_writable) {
    (void)mesh;
    const u64 units = units_of(m, group);
    if (units == 0) {
        return false; // a group always holds a cluster, so never
    }
    m_room_capped = false;
    u64 at = m_alloc.allocate(units);
    if (at == PoolAllocator::k_none && make_room(units, frame_number)) {
        at = m_alloc.allocate(units);
    }
    if (at == PoolAllocator::k_none) {
        return false;
    }
    for (const u32 p : m.links.parents_of(group)) {
        if (m.resident[p] == 0) { // never (parents were in use, so kept): stay whole regardless
            m_alloc.free(at, units);
            return false;
        }
    }
    const u64 bytes = units * k_unit;
    const rhi::TransientMemory upload =
        cmd.upload({m.blob.data() + m.blob_offsets[group], static_cast<usize>(bytes)});
    if (upload.data == nullptr) {
        m_alloc.free(at, units);
        return false;
    }
    if (!pool_writable) {
        cmd.barrier(m_pool, rhi::ResourceState::copy_dest);
        pool_writable = true;
    }
    cmd.copy_buffer(m_pool, at * k_unit, upload.buffer, upload.offset, bytes);
    m.resident[group] = 1;
    ++m_stats.resident_groups;
    m.pool_unit[group] = at;
    m.last_used[group] = frame_number;
    for (const u32 p : m.links.parents_of(group)) {
        ++m.loaded_children[p];
    }
    m.table_dirty = true;
    ++m_stats.loaded;
    return true;
}

void GeometryStreamer::evict(MeshState& m, u32 group) {
    GIANT_ASSERT(m.resident[group] != 0 && m.root[group] == 0 && m.loaded_children[group] == 0,
                 "evicting a group that must stay");
    m_alloc.free(m.pool_unit[group], units_of(m, group));
    m.resident[group] = 0;
    --m_stats.resident_groups;
    m.pool_unit[group] = k_no_unit;
    for (const u32 p : m.links.parents_of(group)) {
        --m.loaded_children[p];
    }
    m.table_dirty = true;
    ++m_stats.evicted;
}

bool GeometryStreamer::evictable(const MeshState& m, u32 g, u32 frame_number) {
    return m.resident[g] != 0 && m.root[g] == 0 && m.loaded_children[g] == 0 &&
           frame_number - m.last_used[g] > k_keep_frames && later(frame_number, m.last_used[g]);
}

bool GeometryStreamer::make_room(u64 units, u32 frame_number) {
    // Leaves of what is loaded (no loaded children), not terminal, and unused
    // for a while: oldest first. Evicting one can make its parents leaves.
    // The candidates are gathered once per update (entries that stopped
    // qualifying are skipped when they come up).
    if (!m_candidates_built) {
        m_candidates_built = true;
        m_candidates.clear();
        for (u32 mi = 0; mi < m_meshes.size(); ++mi) {
            const MeshState& m = m_meshes[mi];
            if (!m.live) continue;
            for (u32 g = 0; g < m.resident.size(); ++g) {
                if (evictable(m, g, frame_number)) m_candidates.push_back({frame_number - m.last_used[g], mi, g});
            }
        }
        std::make_heap(m_candidates.begin(), m_candidates.end(), candidate_order);
    }
    // Free holes need not touch: give up after freeing a few times what was
    // asked, rather than flush the pool for one load.
    u64 freed = 0;
    m_room_capped = false;
    while (m_alloc.largest_free() < units && !m_candidates.empty() && freed < 4 * units + 4096) {
        std::pop_heap(m_candidates.begin(), m_candidates.end(), candidate_order);
        const Candidate c = m_candidates.back();
        m_candidates.pop_back();
        MeshState& m = m_meshes[c.mesh];
        if (!m.live || c.group >= m.resident.size() || !evictable(m, c.group, frame_number)) continue;
        freed += units_of(m, c.group);
        evict(m, c.group);
        for (const u32 p : m.links.parents_of(c.group)) {
            if (evictable(m, p, frame_number)) {
                m_candidates.push_back({frame_number - m.last_used[p], c.mesh, p});
                std::push_heap(m_candidates.begin(), m_candidates.end(), candidate_order);
            }
        }
    }
    const bool room = m_alloc.largest_free() >= units;
    m_room_capped = !room && !m_candidates.empty(); // more could go next frame
    return room;
}

bool GeometryStreamer::grow_usage(rhi::CommandList* cmd) {
    if (m_total_groups <= m_usage_capacity && m_usage.valid()) {
        return true;
    }
    const u32 capacity = std::bit_ceil(std::max(m_total_groups, 4096u));
    const std::vector<u8> zeros(u64{capacity} * 4, 0);
    rhi::BufferDesc desc;
    desc.size = zeros.size();
    desc.usage = rhi::BufferUsage::shader_read | rhi::BufferUsage::shader_write;
    desc.name = "streaming usage marks";
    const rhi::BufferHandle grown = m_device.create_buffer(desc, zeros);
    desc.usage = rhi::BufferUsage::none;
    desc.memory = rhi::Memory::readback;
    desc.name = "streaming usage marks (read back)";
    const rhi::BufferHandle readback = m_device.create_buffer(desc);
    if (!grown.valid() || !readback.valid()) {
        m_device.destroy(grown);
        m_device.destroy(readback);
        return false;
    }
    if (m_usage.valid() && cmd != nullptr) {
        cmd->barrier(m_usage, rhi::ResourceState::copy_source);
        cmd->barrier(grown, rhi::ResourceState::copy_dest);
        cmd->copy_buffer(grown, 0, m_usage, 0, u64{m_usage_capacity} * 4);
    }
    m_device.destroy(m_usage);
    m_device.destroy(m_usage_readback);
    m_usage = grown;
    m_usage_readback = readback;
    m_usage_capacity = capacity;
    m_usage_readback_frame = ~u64{0}; // a pending copy went to the old buffer
    return true;
}

void GeometryStreamer::read_usage() {
    if (m_usage_readback_frame == ~u64{0} ||
        m_usage_readback_frame + rhi::Device::k_frames_in_flight > m_device.frames_completed()) {
        return;
    }
    const u8* bytes = m_device.mapped(m_usage_readback);
    m_usage_readback_frame = ~u64{0};
    if (bytes == nullptr) {
        return;
    }
    for (MeshState& m : m_meshes) {
        if (!m.live) continue;
        for (u32 g = 0; g < m.last_used.size(); ++g) {
            const u64 global = u64{m.group_base} + g;
            if (global >= m_usage_readback_groups) break;
            u32 mark;
            std::memcpy(&mark, bytes + global * 4, 4);
            if (mark != 0 && later(mark, m.last_used[g])) {
                m.last_used[g] = mark;
            }
        }
    }
}

void GeometryStreamer::update(rhi::CommandList& cmd, u32 frame_number) {
    m_stats.loaded = 0;
    m_stats.evicted = 0;
    m_stats.waiting = 0;
    m_tables_current = true;
    m_candidates_built = false;
    read_usage();
    if (!grow_usage(&cmd) && !m_usage_logged) {
        m_usage_logged = true; // the old buffer stays: new meshes' marks are dropped
        log_error("render", "could not grow the streaming usage buffer (out of GPU memory?)");
    }
    bool pool_writable = false;

    // What culling asked for, with any missing parents (which must load
    // first), coarse before fine. Gathered before the terminal groups load,
    // so the ancestors are marked in use before anything makes room.
    std::vector<Request> todo;
    std::vector<Request> stack;
    const u32 mark = ++m_updates; // per update, even if a frame number repeats
    for (const Request& r : m_wanted) {
        if (r.mesh >= m_meshes.size()) continue;
        MeshState& m = m_meshes[r.mesh];
        if (!m.live || r.group >= m.resident.size()) continue;
        stack.push_back(r);
        while (!stack.empty()) {
            const Request q = stack.back();
            stack.pop_back();
            if (m.wanted_mark[q.group] == mark) continue;
            m.wanted_mark[q.group] = mark;
            // Culling went through the ancestors of what it asked for: they
            // are in use, whatever the (later) usage marks say, so making
            // room never evicts the parent of a group about to load.
            if (m.resident[q.group] != 0) {
                m.last_used[q.group] = frame_number;
            } else {
                todo.push_back(q);
            }
            for (const u32 p : m.links.parents_of(q.group)) {
                stack.push_back({q.mesh, p});
            }
        }
    }
    m_wanted.clear();
    // This frame's loads, terminal groups and detail alike, stay within the
    // budget; the first load of a frame always goes, so a group larger than
    // the budget still loads (alone).
    u64 bytes = 0;
    u32 groups = 0;
    const auto fits = [&](u64 size) {
        return groups == 0 || (groups < m_budget.groups_per_frame && bytes + size <= m_budget.bytes_per_frame);
    };
    // Terminal groups first: without them a mesh is not drawn (ready() stays
    // false, so neither culling path sees its instances). Many meshes added
    // at once take several frames, in the order they were added: once one
    // does not fit the budget, the rest wait (a mesh part loaded continues
    // next frame). A mesh the pool cannot hold yet does not block the rest.
    // While any terminal group waits for the budget, detail waits too.
    bool roots_full = false;
    for (u32 mi = 0; mi < m_meshes.size(); ++mi) {
        MeshState& m = m_meshes[mi];
        if (!m.live || m.roots_loaded) continue;
        bool all = true;
        bool no_room = false;
        for (u32 g = 0; g < m.root.size(); ++g) {
            if (m.root[g] == 0 || m.resident[g] != 0) continue;
            const u64 size = units_of(m, g) * k_unit;
            if (roots_full || !fits(size)) {
                roots_full = true;
                all = false;
                ++m_stats.waiting;
                continue;
            }
            if (load(cmd, m, mi, g, frame_number, pool_writable)) {
                bytes += size;
                ++groups;
            } else {
                all = false;
                ++m_stats.waiting;
                no_room = no_room || !m_room_capped;
                // Room was made but capped: keep it for this terminal group
                // next frame instead of handing it to detail now.
                roots_full = roots_full || m_room_capped;
            }
        }
        m.roots_loaded = all;
        if (no_room && !m_roots_logged) {
            m_roots_logged = true;
            log_error("render", "the geometry pool cannot hold a mesh's coarsest level; it is not drawn "
                                "(raise the pool size)");
        }
    }

    std::sort(todo.begin(), todo.end(), [this](const Request& a, const Request& b) {
        const u32 da = m_meshes[a.mesh].depth[a.group];
        const u32 db = m_meshes[b.mesh].depth[b.group];
        return da != db ? da > db : (a.mesh != b.mesh ? a.mesh < b.mesh : a.group < b.group);
    });
    bool full = roots_full;
    for (const Request& r : todo) {
        MeshState& m = m_meshes[r.mesh];
        if (m.resident[r.group] != 0) continue; // a terminal group the loop above loaded
        const u64 size = units_of(m, r.group) * k_unit;
        bool parents_in = true;
        for (const u32 p : m.links.parents_of(r.group)) {
            parents_in = parents_in && m.resident[p] != 0;
        }
        if (full || !m.roots_loaded || !parents_in || !fits(size)) {
            ++m_stats.waiting;
            continue;
        }
        if (load(cmd, m, r.mesh, r.group, frame_number, pool_writable)) {
            bytes += size;
            ++groups;
        } else if (m_room_capped) {
            ++m_stats.waiting; // made what room one load may this frame; smaller ones may fit
        } else {
            ++m_stats.waiting;
            full = true; // nothing old enough to evict: try again next frame
            if (!m_full_logged) {
                m_full_logged = true;
                log_warning("render", "the geometry pool is full of geometry in use; some detail waits "
                                      "(raise the pool size)");
            }
        }
    }

    // Residency tables of meshes whose groups changed.
    std::vector<u32> table;
    for (MeshState& m : m_meshes) {
        if (!m.live || !m.table_dirty) continue;
        table.assign(m.resident.size(), gpu::k_not_resident);
        for (u32 g = 0; g < m.resident.size(); ++g) {
            if (m.resident[g] != 0) {
                table[g] = static_cast<u32>(m.pool_unit[g] * k_unit);
            }
        }
        const rhi::TransientMemory upload = cmd.upload_array(std::span<const u32>(table));
        if (upload.data == nullptr) {
            // Out of memory (the device is as good as lost): the GPU's table
            // no longer matches the pool, so nothing may draw this frame.
            m_tables_current = false;
            continue;
        }
        cmd.barrier(m.table, rhi::ResourceState::copy_dest);
        cmd.copy_buffer(m.table, 0, upload.buffer, upload.offset, table.size() * 4);
        cmd.barrier(m.table, rhi::ResourceState::shader_read);
        m.table_dirty = false;
    }
    cmd.barrier(m_pool, rhi::ResourceState::shader_read);

    m_stats.used_bytes = m_alloc.used() * k_unit;
}

void GeometryStreamer::after_cull(rhi::CommandList& cmd, u32 slot, u64 frame_index) {
    cmd.barrier(m_requests, rhi::ResourceState::copy_source);
    cmd.copy_buffer(m_request_readback[slot % 3], 0, m_requests, 0, u64{k_request_capacity} * sizeof(gpu::Entry));
    ++m_culls_since_usage;
    const u32 groups = std::min(m_total_groups, m_usage_capacity);
    if (m_usage.valid() && m_usage_readback.valid() && m_usage_readback_frame == ~u64{0} &&
        m_culls_since_usage >= k_usage_interval && groups > 0) {
        cmd.barrier(m_usage, rhi::ResourceState::copy_source);
        cmd.copy_buffer(m_usage_readback, 0, m_usage, 0, u64{groups} * 4);
        m_usage_readback_frame = frame_index;
        m_usage_readback_groups = groups;
        m_culls_since_usage = 0;
    }
}

} // namespace giant::render
