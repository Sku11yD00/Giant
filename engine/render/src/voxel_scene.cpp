#include "voxel_scene.h"

#include "giant/core/assert.h"
#include "giant/core/log.h"
#include "gpu_layout.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

namespace giant::render {

namespace {

// More changed instances than this in one frame rebuild everything (finding
// the bricks under each one costs a pass over every instance per level).
constexpr usize k_max_changes = 32;
// Bricks copied to the GPU per frame at most (4.3 MiB).
constexpr u32 k_max_uploads = 2048;
// Instances a job looks at per step (it checks the time between steps).
constexpr u32 k_instances_per_step = 1024;

DVec3 to_d(const Vec3& v) { return {v.x, v.y, v.z}; }

bool overlaps(const DVec3& amin, const DVec3& amax, const DVec3& bmin, const DVec3& bmax) {
    return amin.x <= bmax.x && amax.x >= bmin.x && amin.y <= bmax.y && amax.y >= bmin.y && amin.z <= bmax.z &&
           amax.z >= bmin.z;
}

float max_abs_scale(const Vec3& s) { return std::max({std::fabs(s.x), std::fabs(s.y), std::fabs(s.z)}); }

Vec3 material_color(std::span<const Material> materials, u32 index) {
    return index < materials.size() ? materials[index].base_color : Material{}.base_color;
}

} // namespace

usize VoxelScene::KeyHash::operator()(const Key& k) const {
    u64 h = 0x9E3779B97F4A7C15ull * (k.level + 1);
    for (const i32 v : {k.brick.x, k.brick.y, k.brick.z}) {
        h ^= static_cast<u32>(v) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    }
    return static_cast<usize>(h);
}

VoxelScene::VoxelScene(rhi::Device& device, const voxel::ClipmapDesc& clipmap, u32 brick_capacity)
    : m_device(device), m_clipmap(clipmap), m_capacity(std::max(brick_capacity, 1u)) {}

VoxelScene::~VoxelScene() {
    m_device.destroy(m_pool);
    m_device.destroy(m_table);
}

bool VoxelScene::init() {
    const u32 levels = m_clipmap.levels();
    const u32 slots = m_clipmap.slots_per_level();
    rhi::BufferDesc pool;
    pool.size = u64{m_capacity} * sizeof(voxel::Brick);
    pool.usage = rhi::BufferUsage::shader_read;
    pool.name = "voxel bricks";
    m_pool = m_device.create_buffer(pool);
    m_slots.assign(usize{levels} * slots, gpu::k_no_brick);
    m_slot_bricks.assign(m_slots.size(), {});
    rhi::BufferDesc table;
    table.size = m_slots.size() * 4;
    table.usage = rhi::BufferUsage::shader_read;
    table.name = "voxel slot tables";
    m_table = m_device.create_buffer(
        table, {reinterpret_cast<const u8*>(m_slots.data()), m_slots.size() * sizeof(u32)});
    if (!m_pool.valid() || !m_table.valid()) {
        log_error("render", "could not create the voxel world's GPU buffers (out of GPU memory?)");
        return false;
    }
    m_level_dirty.assign(levels, false);
    m_order.assign(levels, {});
    m_free.clear();
    for (u32 i = m_capacity; i > 0; --i) {
        m_free.push_back(i - 1);
    }
    m_stats.capacity = m_capacity;
    return true;
}

std::vector<u32> VoxelScene::cut(const geometry::ClusterMesh& mesh, float error) {
    std::vector<u32> out;
    for (u32 ci = 0; ci < mesh.clusters.size(); ++ci) {
        const geometry::Cluster& c = mesh.clusters[ci];
        const float own = c.refined >= 0 ? mesh.groups[static_cast<usize>(c.refined)].simplified.error : 0.0f;
        const float replacement = mesh.groups[c.group].simplified.error;
        if (own <= error && replacement > error) {
            out.push_back(ci);
        }
    }
    return out;
}

bool VoxelScene::same_instance(const Instance& a, const Instance& b) const {
    return a.position == b.position && a.rotation == b.rotation && a.scale == b.scale && a.mesh == b.mesh &&
           a.material == b.material;
}

VoxelScene::MeshCache* VoxelScene::mesh_cache(MeshId id, const MeshLookup& meshes) {
    const auto found = m_meshes.find(id);
    if (found != m_meshes.end()) {
        return found->second.mesh != nullptr ? &found->second : nullptr;
    }
    MeshCache cache;
    cache.mesh = meshes ? meshes(id) : nullptr;
    if (cache.mesh != nullptr && !cache.mesh->vertices.empty()) {
        constexpr float inf = std::numeric_limits<float>::infinity();
        cache.min = {inf, inf, inf};
        cache.max = {-inf, -inf, -inf};
        for (const geometry::Vertex& v : cache.mesh->vertices) {
            cache.min = {std::min(cache.min.x, v.position.x), std::min(cache.min.y, v.position.y),
                         std::min(cache.min.z, v.position.z)};
            cache.max = {std::max(cache.max.x, v.position.x), std::max(cache.max.y, v.position.y),
                         std::max(cache.max.z, v.position.z)};
        }
    } else {
        cache.mesh = nullptr; // remembered as missing until the meshes change
    }
    MeshCache& stored = m_meshes.emplace(id, std::move(cache)).first->second;
    return stored.mesh != nullptr ? &stored : nullptr;
}

VoxelScene::Box VoxelScene::bounds_of(const Instance& in, const MeshLookup& meshes) {
    constexpr double inf = std::numeric_limits<double>::infinity();
    Box box{{inf, inf, inf}, {-inf, -inf, -inf}}; // empty: overlaps nothing
    const MeshCache* m = mesh_cache(in.mesh, meshes);
    if (m == nullptr) {
        return box;
    }
    for (u32 corner = 0; corner < 8; ++corner) {
        const Vec3 local{corner & 1 ? m->max.x : m->min.x, corner & 2 ? m->max.y : m->min.y,
                         corner & 4 ? m->max.z : m->min.z};
        const Vec3 scaled{local.x * in.scale.x, local.y * in.scale.y, local.z * in.scale.z};
        const DVec3 p = in.position + to_d(rotate(in.rotation, scaled));
        box.min = {std::min(box.min.x, p.x), std::min(box.min.y, p.y), std::min(box.min.z, p.z)};
        box.max = {std::max(box.max.x, p.x), std::max(box.max.y, p.y), std::max(box.max.z, p.z)};
    }
    if (!(box.min.x <= box.max.x && box.min.y <= box.max.y && box.min.z <= box.max.z)) {
        return Box{{inf, inf, inf}, {-inf, -inf, -inf}}; // NaN in the transform
    }
    return box;
}

bool VoxelScene::queue(Job& job, std::span<const Instance> instances, const MeshLookup& meshes, u32 count) {
    // Only what is still in the window (it may have moved since the job was made).
    const voxel::BrickCoord o = m_clipmap.origin(job.box.level);
    const i32 n = m_clipmap.desc().bricks_per_axis;
    const voxel::BrickBox box = voxel::intersect(job.box, {job.box.level, o, {o.x + n, o.y + n, o.z + n}});
    if (box.count() == 0) {
        return true;
    }
    const u32 level = box.level;
    const DVec3 lo = m_clipmap.brick_min(level, box.min);
    const DVec3 hi = m_clipmap.brick_min(level, box.max);
    // Every brick of the box gets its list made again from scratch (the
    // job's stamp marks lists it already restarted): an instance touching
    // a brick in the box touches the box, so the pass below finds all of
    // them. A pass may take several calls (frames), `count` instances each.
    if (job.stamp == 0) {
        job.stamp = ++m_stamp;
    }
    const u32 stamp = job.stamp;
    const auto entry = [&](const voxel::BrickCoord& b) -> Pending& {
        const Key key{level, b};
        auto [it, inserted] = m_pending.try_emplace(key);
        if (inserted) {
            m_order[level].push_back(key);
        }
        if (it->second.stamp != stamp) {
            it->second.stamp = stamp;
            it->second.instances.clear();
        }
        return it->second;
    };
    const u32 end = static_cast<u32>(std::min<u64>(instances.size(), u64{job.next} + count));
    for (; m_bounds_ready < end; ++m_bounds_ready) {
        m_bounds[m_bounds_ready] = bounds_of(instances[m_bounds_ready], meshes);
    }
    for (u32 i = job.next; i < end; ++i) {
        const Box& b = m_bounds[i];
        if (!overlaps(b.min, b.max, lo, hi)) {
            continue;
        }
        const voxel::BrickBox under = voxel::intersect(m_clipmap.bricks_touching(level, b.min, b.max), box);
        for (i32 z = under.min.z; z < under.max.z; ++z) {
            for (i32 y = under.min.y; y < under.max.y; ++y) {
                for (i32 x = under.min.x; x < under.max.x; ++x) {
                    entry({x, y, z}).instances.push_back(i);
                }
            }
        }
    }
    job.next = end;
    if (end < instances.size()) {
        return false; // more next time
    }
    // Bricks built before that nothing touches now are built again (empty:
    // removed). Pending bricks in the box that nothing touches now are
    // emptied the same way.
    const u32 slots = m_clipmap.slots_per_level();
    const auto in_box = [&box](const voxel::BrickCoord& b) {
        return b.x >= box.min.x && b.x < box.max.x && b.y >= box.min.y && b.y < box.max.y && b.z >= box.min.z &&
               b.z < box.max.z;
    };
    const auto restart = [&](Pending& pending) {
        if (pending.stamp != stamp) {
            pending.stamp = stamp;
            pending.instances.clear();
        }
    };
    if (box.count() * 4 <= slots + m_pending.size()) {
        // A small box: look its bricks up (a hash lookup costs a few scanned entries).
        for (i32 z = box.min.z; z < box.max.z; ++z) {
            for (i32 y = box.min.y; y < box.max.y; ++y) {
                for (i32 x = box.min.x; x < box.max.x; ++x) {
                    const voxel::BrickCoord b{x, y, z};
                    const u32 s = level * slots + m_clipmap.slot(level, b);
                    if (m_slots[s] != gpu::k_no_brick && m_slot_bricks[s] == b) {
                        (void)entry(b);
                    }
                    const auto it = m_pending.find(Key{level, b});
                    if (it != m_pending.end()) {
                        restart(it->second);
                    }
                }
            }
        }
        return true;
    }
    for (u32 slot = 0; slot < slots; ++slot) {
        const u32 s = level * slots + slot;
        if (m_slots[s] != gpu::k_no_brick && in_box(m_slot_bricks[s])) {
            (void)entry(m_slot_bricks[s]);
        }
    }
    for (auto& [key, pending] : m_pending) {
        if (key.level == level && in_box(key.brick)) {
            restart(pending);
        }
    }
    return true;
}

void VoxelScene::add_job(const Box& box) {
    if (!(box.min.x <= box.max.x)) {
        return; // empty
    }
    for (u32 level = m_clipmap.levels(); level-- > 0;) {
        const voxel::BrickBox bricks = m_clipmap.bricks_touching(level, box.min, box.max);
        if (bricks.count() > 0) {
            m_jobs.push_back({bricks});
        }
    }
}

void VoxelScene::remove(u32 level, u32 slot) {
    const u32 s = level * m_clipmap.slots_per_level() + slot;
    if (m_slots[s] == gpu::k_no_brick) {
        return;
    }
    m_free.push_back(m_slots[s]);
    m_slots[s] = gpu::k_no_brick;
    --m_used;
    m_level_dirty[level] = true;
}

void VoxelScene::evict_outside() {
    const u32 slots = m_clipmap.slots_per_level();
    for (u32 level = 0; level < m_clipmap.levels(); ++level) {
        for (u32 slot = 0; slot < slots; ++slot) {
            const u32 s = level * slots + slot;
            if (m_slots[s] != gpu::k_no_brick && !m_clipmap.contains(level, m_slot_bricks[s])) {
                remove(level, slot);
            }
        }
    }
}

bool VoxelScene::build(rhi::CommandList& cmd, const Key& key, const Pending& pending,
                       std::span<const Instance> instances, std::span<const Material> materials,
                       const MeshLookup& meshes, bool& pool_writable) {
    const u32 level = key.level;
    const double vs = m_clipmap.voxel_size(level);
    const DVec3 origin = m_clipmap.brick_min(level, key.brick);
    const float voxel = static_cast<float>(vs);
    const float edge = voxel * static_cast<float>(voxel::k_brick_edge);
    m_builder.reset(voxel);
    for (const u32 i : pending.instances) {
        if (i >= instances.size()) {
            continue;
        }
        const Instance& in = instances[i];
        MeshCache* m = mesh_cache(in.mesh, meshes);
        if (m == nullptr) {
            continue;
        }
        const float s = max_abs_scale(in.scale);
        if (!(s > 0.0f) || !std::isfinite(s)) {
            continue;
        }
        // Detail to half a voxel: the cut whose error is the power of two
        // at or under that (shared by instances of similar scale).
        const float wanted = 0.5f * voxel / s;
        const i32 exponent = static_cast<i32>(std::clamp(std::floor(std::log2(wanted)), -40.0f, 40.0f));
        auto found = m->cuts.find(exponent);
        if (found == m->cuts.end()) {
            found = m->cuts.emplace(exponent, cut(*m->mesh, std::ldexp(1.0f, exponent))).first;
        }
        const Vec3 rel = to_camera_relative(in.position, origin); // the brick's frame
        const Vec3 color = material_color(materials, in.material);
        const bool mirrored = in.scale.x * in.scale.y * in.scale.z < 0.0f;
        const auto place = [&](const Vec3& p) {
            return rel + rotate(in.rotation, Vec3{p.x * in.scale.x, p.y * in.scale.y, p.z * in.scale.z});
        };
        const geometry::ClusterMesh& mesh = *m->mesh;
        for (const u32 ci : found->second) {
            const geometry::Cluster& c = mesh.clusters[ci];
            const Vec3 center = place(c.bounds.center);
            const float r = c.bounds.radius * s;
            // Sphere against the brick's box.
            const float dx = center.x - std::clamp(center.x, 0.0f, edge);
            const float dy = center.y - std::clamp(center.y, 0.0f, edge);
            const float dz = center.z - std::clamp(center.z, 0.0f, edge);
            if (dx * dx + dy * dy + dz * dz > r * r) {
                continue;
            }
            for (u32 t = 0; t < c.triangle_count; ++t) {
                const u32* idx = &mesh.indices[c.first_index + t * 3];
                const Vec3 a = place(mesh.vertices[idx[0]].position);
                Vec3 b = place(mesh.vertices[idx[1]].position);
                Vec3 d = place(mesh.vertices[idx[2]].position);
                if (mirrored) {
                    std::swap(b, d); // keep the facing outward
                }
                m_builder.add_triangle(a, b, d, color);
            }
        }
    }

    const u32 slot = m_clipmap.slot(level, key.brick);
    const u32 s = level * m_clipmap.slots_per_level() + slot;
    const bool here = m_slots[s] != gpu::k_no_brick && m_slot_bricks[s] == key.brick;
    if (m_slots[s] != gpu::k_no_brick && !here) {
        remove(level, slot); // a brick that left the window (never kept: evicted on moves)
    }
    if (m_builder.empty()) {
        remove(level, slot);
        return true;
    }
    u32 index = here ? m_slots[s] : gpu::k_no_brick;
    if (index == gpu::k_no_brick) {
        if (m_free.empty()) {
            m_stats.overflow = true;
            if (!m_overflow_logged) {
                m_overflow_logged = true;
                log_warning("render", "the voxel brick pool is full; some of the voxel world is missing "
                                      "(raise RendererDesc::voxel_bricks)");
            }
            return true;
        }
        index = m_free.back();
        m_free.pop_back();
        ++m_used;
    }
    const voxel::Brick brick = m_builder.build();
    const rhi::TransientMemory upload =
        cmd.upload({reinterpret_cast<const u8*>(&brick), sizeof(brick)});
    if (upload.data == nullptr) {
        if (!here) {
            m_free.push_back(index);
            --m_used;
        }
        return false; // out of memory: the device is lost
    }
    if (!pool_writable) {
        cmd.barrier(m_pool, rhi::ResourceState::copy_dest);
        pool_writable = true;
    }
    cmd.copy_buffer(m_pool, u64{index} * sizeof(voxel::Brick), upload.buffer, upload.offset, sizeof(brick));
    ++m_stats.uploaded;
    if (!here) {
        m_slots[s] = index;
        m_slot_bricks[s] = key.brick;
        m_level_dirty[level] = true;
    }
    return true;
}

void VoxelScene::update(rhi::CommandList& cmd, const DVec3& camera, std::span<const Instance> instances,
                        std::span<const Material> materials, u64 meshes_version, const MeshLookup& meshes,
                        float budget_ms) {
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    m_stats.built = 0;
    m_stats.uploaded = 0;
    m_stats.overflow = false;
    if (!m_pool.valid()) {
        return;
    }

    // What changed since the bricks were built.
    bool full = meshes_version != m_meshes_version;
    if (full) {
        m_meshes.clear(); // the cached pointers may be gone
        m_meshes_version = meshes_version;
    }
    if (materials.size() != m_colors.size()) {
        full = true;
    } else {
        for (usize i = 0; i < materials.size() && !full; ++i) {
            full = !(materials[i].base_color == m_colors[i]);
        }
    }
    std::vector<u32> changed;
    if (instances.size() < m_instances.size()) {
        full = true; // removed (indices shift)
    }
    for (u32 i = 0; i < instances.size() && !full; ++i) {
        if (i >= m_instances.size() || !same_instance(instances[i], m_instances[i])) {
            changed.push_back(i);
            full = changed.size() > k_max_changes;
        }
    }

    const std::vector<voxel::BrickBox> entered = m_clipmap.recenter(camera);
    if (!entered.empty()) {
        evict_outside();
    }
    if (full) {
        m_pending.clear();
        for (std::deque<Key>& order : m_order) {
            order.clear();
        }
        m_jobs.clear();
        m_bounds.resize(instances.size());
        m_bounds_ready = 0; // worked out as the first job goes through them
        m_instances.assign(instances.begin(), instances.end());
        // Every window, coarsest first; the slabs just entered are inside them.
        const i32 n = m_clipmap.desc().bricks_per_axis;
        for (u32 level = m_clipmap.levels(); level-- > 0;) {
            const voxel::BrickCoord o = m_clipmap.origin(level);
            m_jobs.push_back({{level, o, {o.x + n, o.y + n, o.z + n}}});
        }
    } else if (!changed.empty() || !entered.empty()) {
        // Bricks under where changed instances were, then where they are.
        // Instances no job has reached yet (a rebuild under way) have no
        // bricks to clear, and the jobs to come find them where they are.
        std::vector<Box> old;
        std::vector<u32> placed;
        for (const u32 i : changed) {
            if (i < m_bounds_ready) {
                old.push_back(m_bounds[i]);
                placed.push_back(i);
            }
        }
        m_bounds.resize(instances.size());
        if (m_bounds_ready == m_instances.size()) {
            m_bounds_ready = static_cast<u32>(instances.size()); // appended ones are worked out here
        }
        for (const u32 i : changed) {
            if (i < m_bounds_ready) {
                m_bounds[i] = bounds_of(instances[i], meshes);
                if (i >= m_instances.size()) {
                    placed.push_back(i); // new
                }
            }
        }
        if (!changed.empty()) {
            m_instances.assign(instances.begin(), instances.end());
        }
        for (const voxel::BrickBox& box : entered) {
            m_jobs.push_back({box});
        }
        for (const Box& box : old) {
            add_job(box);
        }
        for (const u32 i : placed) {
            add_job(m_bounds[i]);
        }
    }
    m_colors.resize(materials.size());
    for (usize i = 0; i < materials.size(); ++i) {
        m_colors[i] = materials[i].base_color;
    }

    // Find the bricks under each queued box (at least one box a frame),
    // then build, coarse levels first, within the budget (at least one
    // brick a frame).
    const double budget = std::max(0.0, static_cast<double>(budget_ms));
    const auto elapsed = [&start] {
        return std::chrono::duration<double, std::milli>(clock::now() - start).count();
    };
    for (bool first = true; !m_jobs.empty() && (first || elapsed() < budget); first = false) {
        if (queue(m_jobs.front(), instances, meshes, k_instances_per_step)) {
            m_jobs.pop_front();
        }
    }
    // A job part-way through has made partial lists for its level's bricks:
    // they wait until it is done.
    const u32 busy = !m_jobs.empty() && m_jobs.front().next > 0 ? m_jobs.front().box.level : ~0u;
    bool pool_writable = false;
    bool stop = false;
    for (u32 l = m_clipmap.levels(); l-- > 0 && !stop;) {
        if (l == busy) {
            continue;
        }
        std::deque<Key>& order = m_order[l];
        while (!order.empty()) {
            if (m_stats.built > 0 && (elapsed() >= budget || m_stats.uploaded >= k_max_uploads)) {
                stop = true;
                break;
            }
            const Key key = order.front();
            order.pop_front();
            const auto it = m_pending.find(key);
            if (it == m_pending.end()) {
                continue;
            }
            const Pending pending = std::move(it->second);
            m_pending.erase(it);
            if (!m_clipmap.contains(key.level, key.brick)) {
                continue; // left the window while waiting
            }
            ++m_stats.built;
            if (!build(cmd, key, pending, instances, materials, meshes, pool_writable)) {
                stop = true;
                break;
            }
        }
    }
    if (pool_writable) {
        cmd.barrier(m_pool, rhi::ResourceState::shader_read);
    }

    // Changed slot tables, a level at a time.
    const u32 slots = m_clipmap.slots_per_level();
    bool table_writable = false;
    for (u32 level = 0; level < m_clipmap.levels(); ++level) {
        if (!m_level_dirty[level]) {
            continue;
        }
        const rhi::TransientMemory upload =
            cmd.upload_array(std::span<const u32>(m_slots.data() + usize{level} * slots, slots));
        if (upload.data == nullptr) {
            break; // out of memory: the device is lost
        }
        if (!table_writable) {
            cmd.barrier(m_table, rhi::ResourceState::copy_dest);
            table_writable = true;
        }
        cmd.copy_buffer(m_table, u64{level} * slots * 4, upload.buffer, upload.offset, u64{slots} * 4);
        m_level_dirty[level] = false;
    }
    if (table_writable) {
        cmd.barrier(m_table, rhi::ResourceState::shader_read);
    }
    m_stats.bricks = m_used;
    m_stats.pending = static_cast<u32>(m_pending.size() + m_jobs.size());
    m_stats.cpu_ms = static_cast<float>(std::chrono::duration<double, std::milli>(clock::now() - start).count());
}

u32 VoxelScene::brick_index(u32 level, const voxel::BrickCoord& brick) const {
    if (level >= m_clipmap.levels() || !m_clipmap.contains(level, brick)) {
        return gpu::k_no_brick;
    }
    const u32 s = level * m_clipmap.slots_per_level() + m_clipmap.slot(level, brick);
    return m_slot_bricks[s] == brick ? m_slots[s] : gpu::k_no_brick;
}

void VoxelScene::write_levels(gpu::VoxelLevel* out, const DVec3& camera) const {
    for (u32 level = 0; level < m_clipmap.levels(); ++level) {
        const voxel::BrickCoord o = m_clipmap.origin(level);
        const Vec3 rel = to_camera_relative(m_clipmap.brick_min(level, o), camera);
        gpu::VoxelLevel& l = out[level];
        l.origin[0] = rel.x;
        l.origin[1] = rel.y;
        l.origin[2] = rel.z;
        l.origin[3] = static_cast<float>(m_clipmap.brick_size(level));
        l.brick[0] = o.x;
        l.brick[1] = o.y;
        l.brick[2] = o.z;
        l.brick[3] = 0;
    }
}

} // namespace giant::render
