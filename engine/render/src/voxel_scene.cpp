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

// The instance grid's cells, metres on each axis.
constexpr double k_cell = 32.0;
// Instances over more cells than this are kept in a list of their own.
constexpr u64 k_large_cells = 64;
// Bricks copied to the GPU per frame at most (2.1 MiB).
constexpr u32 k_max_uploads = 1024;
// More queued jobs than this and changes wait (they stay cheap to hold).
constexpr usize k_max_jobs = 64;
// More changes waiting than this, or more in one frame than k_bulk (or an
// eighth of the instances), and every window is rebuilt instead.
constexpr usize k_max_backlog = 4096;
constexpr usize k_bulk = 256;
// Marks (cheap set inserts) between looks at the clock.
constexpr u64 k_marks_per_check = 256;

DVec3 to_d(const Vec3& v) { return {v.x, v.y, v.z}; }

bool overlaps(const DVec3& amin, const DVec3& amax, const DVec3& bmin, const DVec3& bmax) {
    return amin.x <= bmax.x && amax.x >= bmin.x && amin.y <= bmax.y && amax.y >= bmin.y && amin.z <= bmax.z &&
           amax.z >= bmin.z;
}

float max_abs_scale(const Vec3& s) { return std::max({std::fabs(s.x), std::fabs(s.y), std::fabs(s.z)}); }

// The i-th brick of a box, x fastest.
voxel::BrickCoord brick_of(const voxel::BrickBox& b, u64 i) {
    const u64 sx = static_cast<u64>(b.max.x - b.min.x);
    const u64 sy = static_cast<u64>(b.max.y - b.min.y);
    return {b.min.x + static_cast<i32>(i % sx), b.min.y + static_cast<i32>((i / sx) % sy),
            b.min.z + static_cast<i32>(i / (sx * sy))};
}

bool same_box(const voxel::BrickBox& a, const voxel::BrickBox& b) {
    return a.level == b.level && a.min == b.min && a.max == b.max;
}

i32 cell_of(double v) {
    constexpr double limit = 1 << 28;
    const double c = std::floor(v / k_cell);
    if (!(c > -limit)) return -(1 << 28);
    if (!(c < limit)) return 1 << 28;
    return static_cast<i32>(c);
}

} // namespace

const char* to_string(VoxelProbe probe) {
    switch (probe) {
    case VoxelProbe::outside: return "outside";
    case VoxelProbe::building: return "building";
    case VoxelProbe::empty: return "empty";
    case VoxelProbe::solid: return "solid";
    }
    return "outside";
}

usize VoxelScene::KeyHash::operator()(const Key& k) const {
    u64 h = 0x9E3779B97F4A7C15ull * (k.level + 1);
    for (const i32 v : {k.brick.x, k.brick.y, k.brick.z}) {
        h ^= static_cast<u32>(v) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    }
    return static_cast<usize>(h);
}

usize VoxelScene::CellHash::operator()(const Cell& c) const {
    u64 h = 0xC2B2AE3D27D4EB4Full;
    for (const i32 v : {c.x, c.y, c.z}) {
        h ^= static_cast<u32>(v) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    }
    return static_cast<usize>(h);
}

VoxelScene::VoxelScene(rhi::Device& device, const voxel::ClipmapDesc& clipmap, u32 brick_capacity)
    : m_device(device), m_clipmap(clipmap), m_capacity(std::clamp(brick_capacity, 1u, gpu::k_unbuilt - 1)) {}

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
    m_dirty_lo.assign(levels, slots);
    m_dirty_hi.assign(levels, 0);
    m_unbuilt.assign(levels, 0);
    m_filled.assign(levels, {});
    m_loose.assign(levels, false);
    m_order.assign(levels, {});
    m_occupancy.assign(m_capacity, {});
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

// --- Instances ------------------------------------------------------------

VoxelScene::MeshCache* VoxelScene::mesh_cache(MeshId id, const MeshLookup& meshes) {
    const auto found = m_meshes.find(id);
    if (found != m_meshes.end()) {
        return found->second.mesh != nullptr ? &found->second : nullptr;
    }
    MeshCache cache;
    cache.mesh = meshes ? meshes(id) : nullptr;
    if (cache.mesh != nullptr && !cache.mesh->empty()) {
        // Around the original clusters' spheres: one pass over the clusters,
        // not the vertices.
        constexpr float inf = std::numeric_limits<float>::infinity();
        cache.min = {inf, inf, inf};
        cache.max = {-inf, -inf, -inf};
        for (const geometry::Cluster& c : cache.mesh->clusters) {
            if (c.refined >= 0) {
                continue;
            }
            const Vec3 p = c.bounds.center;
            const float r = c.bounds.radius;
            cache.min = {std::min(cache.min.x, p.x - r), std::min(cache.min.y, p.y - r),
                         std::min(cache.min.z, p.z - r)};
            cache.max = {std::max(cache.max.x, p.x + r), std::max(cache.max.y, p.y + r),
                         std::max(cache.max.z, p.z + r)};
        }
        if (!(cache.min.x <= cache.max.x)) {
            const geometry::Sphere& s = cache.mesh->bounds;
            cache.min = s.center - Vec3{s.radius, s.radius, s.radius};
            cache.max = s.center + Vec3{s.radius, s.radius, s.radius};
        }
    } else {
        cache.mesh = nullptr; // remembered as missing until the meshes change
    }
    MeshCache& stored = m_meshes.emplace(id, std::move(cache)).first->second;
    return stored.mesh != nullptr ? &stored : nullptr;
}

VoxelScene::Box VoxelScene::bounds_of(const Instance& in, const MeshLookup& meshes) {
    constexpr double inf = std::numeric_limits<double>::infinity();
    const Box none{{inf, inf, inf}, {-inf, -inf, -inf}}; // empty: overlaps nothing
    const MeshCache* m = mesh_cache(in.mesh, meshes);
    if (m == nullptr) {
        return none;
    }
    Box box = none;
    for (u32 corner = 0; corner < 8; ++corner) {
        const Vec3 local{corner & 1 ? m->max.x : m->min.x, corner & 2 ? m->max.y : m->min.y,
                         corner & 4 ? m->max.z : m->min.z};
        const Vec3 scaled{local.x * in.scale.x, local.y * in.scale.y, local.z * in.scale.z};
        const DVec3 p = in.position + to_d(rotate(in.rotation, scaled));
        box.min = {std::min(box.min.x, p.x), std::min(box.min.y, p.y), std::min(box.min.z, p.z)};
        box.max = {std::max(box.max.x, p.x), std::max(box.max.y, p.y), std::max(box.max.z, p.z)};
    }
    const bool finite = std::isfinite(box.min.x) && std::isfinite(box.min.y) && std::isfinite(box.min.z) &&
                        std::isfinite(box.max.x) && std::isfinite(box.max.y) && std::isfinite(box.max.z);
    return finite && !box.empty() ? box : none; // NaN or infinity in the transform
}

Vec3 VoxelScene::color_of(const Instance& in, std::span<const Material> materials) const {
    return in.material < materials.size() ? materials[in.material].base_color : Material{}.base_color;
}

bool VoxelScene::differs(const Instance& in, std::span<const Material> materials, const Copy& copy) const {
    const Instance& c = copy.instance;
    return !(in.position == c.position && in.rotation == c.rotation && in.scale == c.scale && in.mesh == c.mesh &&
             in.material == c.material && color_of(in, materials) == copy.color);
}

void VoxelScene::set_copy(u32 index, const Instance& in, std::span<const Material> materials,
                          const MeshLookup& meshes) {
    Copy& c = m_copies[index];
    c.instance = in;
    c.color = color_of(in, materials);
    c.box = bounds_of(in, meshes);
    c.large = false;
}

// --- The grid -------------------------------------------------------------

void VoxelScene::grid_insert(u32 index) {
    Copy& c = m_copies[index];
    c.large = false;
    if (c.box.empty()) {
        return; // no mesh: nothing to find
    }
    const Cell lo{cell_of(c.box.min.x), cell_of(c.box.min.y), cell_of(c.box.min.z)};
    const Cell hi{cell_of(c.box.max.x), cell_of(c.box.max.y), cell_of(c.box.max.z)};
    const u64 cells = u64(hi.x - lo.x + 1) * u64(hi.y - lo.y + 1) * u64(hi.z - lo.z + 1);
    if (cells > k_large_cells) {
        c.large = true;
        m_large.push_back(index);
        return;
    }
    for (i32 z = lo.z; z <= hi.z; ++z) {
        for (i32 y = lo.y; y <= hi.y; ++y) {
            for (i32 x = lo.x; x <= hi.x; ++x) {
                m_grid[Cell{x, y, z}].push_back(index);
            }
        }
    }
}

void VoxelScene::grid_erase(u32 index) {
    const Copy& c = m_copies[index];
    const auto drop = [index](std::vector<u32>& list) {
        const auto it = std::find(list.begin(), list.end(), index);
        if (it != list.end()) {
            *it = list.back();
            list.pop_back();
        }
    };
    if (c.large) {
        drop(m_large);
        return;
    }
    if (c.box.empty()) {
        return;
    }
    const Cell lo{cell_of(c.box.min.x), cell_of(c.box.min.y), cell_of(c.box.min.z)};
    const Cell hi{cell_of(c.box.max.x), cell_of(c.box.max.y), cell_of(c.box.max.z)};
    for (i32 z = lo.z; z <= hi.z; ++z) {
        for (i32 y = lo.y; y <= hi.y; ++y) {
            for (i32 x = lo.x; x <= hi.x; ++x) {
                const auto it = m_grid.find(Cell{x, y, z});
                if (it != m_grid.end()) {
                    drop(it->second);
                    if (it->second.empty()) {
                        m_grid.erase(it);
                    }
                }
            }
        }
    }
}

void VoxelScene::grid_rebuild() {
    // The cells' lists are kept (emptied), so refilling them allocates
    // little; dropped when far more cells than instances piled up.
    if (m_grid.size() > 4 * m_copies.size() + 1024) {
        m_grid.clear();
    } else {
        for (auto& [cell, list] : m_grid) {
            list.clear();
        }
    }
    m_large.clear();
    for (u32 i = 0; i < m_copies.size(); ++i) {
        grid_insert(i);
    }
}

void VoxelScene::find(const Box& q) {
    m_found.clear();
    if (q.empty()) {
        return;
    }
    m_seen.resize(m_copies.size(), 0);
    if (++m_find_stamp == 0) {
        std::fill(m_seen.begin(), m_seen.end(), 0);
        m_find_stamp = 1;
    }
    const auto look = [&](u32 i) {
        if (m_seen[i] != m_find_stamp) {
            m_seen[i] = m_find_stamp;
            const Box& b = m_copies[i].box;
            if (overlaps(b.min, b.max, q.min, q.max)) {
                m_found.push_back(i);
            }
        }
    };
    const Cell lo{cell_of(q.min.x), cell_of(q.min.y), cell_of(q.min.z)};
    const Cell hi{cell_of(q.max.x), cell_of(q.max.y), cell_of(q.max.z)};
    const u64 cells = u64(hi.x - lo.x + 1) * u64(hi.y - lo.y + 1) * u64(hi.z - lo.z + 1);
    if (cells > m_grid.size()) {
        // A big box (a coarse brick, a whole window): every occupied cell.
        for (const auto& [cell, list] : m_grid) {
            if (cell.x >= lo.x && cell.x <= hi.x && cell.y >= lo.y && cell.y <= hi.y && cell.z >= lo.z &&
                cell.z <= hi.z) {
                for (const u32 i : list) {
                    look(i);
                }
            }
        }
    } else {
        for (i32 z = lo.z; z <= hi.z; ++z) {
            for (i32 y = lo.y; y <= hi.y; ++y) {
                for (i32 x = lo.x; x <= hi.x; ++x) {
                    const auto it = m_grid.find(Cell{x, y, z});
                    if (it != m_grid.end()) {
                        for (const u32 i : it->second) {
                            look(i);
                        }
                    }
                }
            }
        }
    }
    for (const u32 i : m_large) {
        look(i);
    }
}

// --- Seeing changes -------------------------------------------------------

void VoxelScene::change(u32 index, const Instance& in, std::span<const Material> materials,
                        const MeshLookup& meshes) {
    if (index < m_copies.size()) {
        grid_erase(index);
        if (!m_copies[index].box.empty()) {
            m_old.push_back(m_copies[index].box); // its bricks there are built again
        }
    } else {
        m_copies.resize(usize{index} + 1);
    }
    touch_building(m_copies[index].box); // where it was (empty if new)
    set_copy(index, in, materials, meshes);
    touch_building(m_copies[index].box);
    grid_insert(index);
    if (m_changed.insert(index).second) {
        m_changed_order.push_back(index);
    }
}

void VoxelScene::copy_all(std::span<const Instance> instances, std::span<const Material> materials,
                          const MeshLookup& meshes) {
    const bool same_count = instances.size() == m_copies.size();
    m_copies.resize(instances.size());
    for (u32 i = 0; i < instances.size(); ++i) {
        set_copy(i, instances[i], materials, meshes);
    }
    grid_rebuild();
    // With the same count every index still names the same instance: find
    // jobs under way keep going (they read the current boxes, and the
    // rebuild that follows covers the rest). Otherwise their lists are
    // stale. Restarting them on every bulk change would starve them.
    if (!same_count) {
        for (Job& job : m_jobs) {
            job.started = job.started && job.kind != JobKind::find;
        }
    }
    m_building.again = m_building.again || m_building.active; // so is the brick under way's
}

void VoxelScene::remove_range(u32 first, u32 count) {
    for (u32 i = first; i < first + count; ++i) {
        if (!m_copies[i].box.empty()) {
            m_old.push_back(m_copies[i].box);
        }
    }
    m_copies.erase(m_copies.begin() + first, m_copies.begin() + first + count);
    grid_rebuild();
    // Indices past the run shift down.
    std::deque<u32> order;
    m_changed.clear();
    for (const u32 i : m_changed_order) {
        if (i >= first && i < first + count) {
            continue;
        }
        const u32 j = i < first ? i : i - count;
        if (m_changed.insert(j).second) {
            order.push_back(j);
        }
    }
    m_changed_order = std::move(order);
    for (Job& job : m_jobs) {
        job.started = job.started && job.kind != JobKind::find;
    }
    m_building.again = m_building.again || m_building.active;
}

void VoxelScene::see_changes(std::span<const Instance> instances, std::span<const Material> materials,
                             u64 meshes_version, const MeshLookup& meshes) {
    // Meshes added or removed: the cached ones that are gone (or came back)
    // and the instances using them.
    std::vector<u32> forced;
    if (meshes_version != m_meshes_version) {
        m_meshes_version = meshes_version;
        std::unordered_set<MeshId> stale;
        for (auto it = m_meshes.begin(); it != m_meshes.end();) {
            const geometry::ClusterMesh* now = meshes ? meshes(it->first) : nullptr;
            if (now != nullptr && now->empty()) {
                now = nullptr;
            }
            if (now != it->second.mesh) {
                stale.insert(it->first);
                it = m_meshes.erase(it);
            } else {
                ++it;
            }
        }
        if (!stale.empty()) {
            for (u32 i = 0; i < m_copies.size(); ++i) {
                if (stale.count(m_copies[i].instance.mesh) != 0) {
                    forced.push_back(i);
                }
            }
        }
    }

    const u32 n = static_cast<u32>(instances.size());
    const u32 m = static_cast<u32>(m_copies.size());
    if (n < m) {
        // One run removed from the middle (or the end) shifts the rest down:
        // see whether that is all that happened.
        u32 first = 0;
        while (first < n && !differs(instances[first], materials, m_copies[first])) {
            ++first;
        }
        const u32 k = m - n;
        bool tail = forced.empty();
        for (u32 j = first; j < n && tail; ++j) {
            tail = !differs(instances[j], materials, m_copies[j + k]);
        }
        if (tail) {
            remove_range(first, k);
        } else {
            copy_all(instances, materials, meshes);
            request_rebuild();
        }
        return;
    }
    std::vector<u32> changed = std::move(forced);
    for (u32 i = 0; i < m; ++i) {
        if (differs(instances[i], materials, m_copies[i])) {
            changed.push_back(i);
        }
    }
    for (u32 i = m; i < n; ++i) {
        changed.push_back(i);
    }
    if (changed.size() > std::max<usize>(k_bulk, n / 8)) {
        copy_all(instances, materials, meshes);
        request_rebuild();
        return;
    }
    std::sort(changed.begin(), changed.end());
    changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
    for (const u32 i : changed) {
        change(i, instances[i], materials, meshes);
    }
    if (m_old.size() + m_changed.size() > k_max_backlog) {
        request_rebuild(); // changing faster than they are worked through
    }
}

void VoxelScene::request_rebuild() {
    // A full rebuild covers every change made before it starts.
    m_old.clear();
    m_changed.clear();
    m_changed_order.clear();
    if (m_rebuild_jobs > 0) {
        m_rebuild_again = true; // after the one under way (restarting it would never finish)
    } else {
        push_rebuild();
    }
}

void VoxelScene::push_rebuild() {
    m_rebuild_again = false;
    m_old.clear();
    m_changed.clear();
    m_changed_order.clear();
    const i32 n = m_clipmap.desc().bricks_per_axis;
    const u32 slots = m_clipmap.slots_per_level();
    for (u32 level = m_clipmap.levels(); level-- > 0;) {
        const voxel::BrickCoord o = m_clipmap.origin(level);
        const voxel::BrickBox window{level, o, {o.x + n, o.y + n, o.z + n}};
        // The whole window just came into view and its find job has not
        // started: that job finds everything as it is now. (One under way
        // may have passed instances that changed since.)
        const bool covered = m_unbuilt[level] == slots &&
                             std::any_of(m_jobs.begin(), m_jobs.end(), [&](const Job& j) {
                                 return j.kind == JobKind::find && !j.started && same_box(j.box, window);
                             });
        if (covered) {
            continue;
        }
        Job present;
        present.box = window;
        present.kind = JobKind::present;
        present.rebuild = true;
        Job found = present;
        found.kind = JobKind::find;
        m_jobs.push_back(std::move(present));
        m_jobs.push_back(std::move(found));
        m_rebuild_jobs += 2;
    }
}

void VoxelScene::add_jobs(const Box& box, JobKind kind) {
    if (box.empty()) {
        return;
    }
    for (u32 level = m_clipmap.levels(); level-- > 0;) {
        const voxel::BrickBox bricks = m_clipmap.bricks_touching(level, box.min, box.max);
        if (bricks.count() > 0) {
            Job job;
            job.box = bricks;
            job.kind = kind;
            m_jobs.push_back(std::move(job));
        }
    }
}

void VoxelScene::enter(const voxel::BrickBox& box) {
    // The slots of a slab coming into view held bricks that just left it.
    const u32 level = box.level;
    const i32 n = m_clipmap.desc().bricks_per_axis;
    const u32 base = level * m_clipmap.slots_per_level();
    const auto wrap = [n](i32 v) { return static_cast<u32>(((v % n) + n) % n); };
    for (i32 z = box.min.z; z < box.max.z; ++z) {
        for (i32 y = box.min.y; y < box.max.y; ++y) {
            const u32 row = static_cast<u32>(n) * (wrap(y) + static_cast<u32>(n) * wrap(z));
            for (i32 x = box.min.x; x < box.max.x; ++x) {
                const u32 slot = row + wrap(x);
                u32& value = m_slots[base + slot];
                if (value < gpu::k_unbuilt) {
                    m_free.push_back(value);
                    --m_used;
                    m_loose[level] = true;
                }
                if (value != gpu::k_unbuilt) {
                    ++m_unbuilt[level];
                    value = gpu::k_unbuilt;
                    m_dirty_lo[level] = std::min(m_dirty_lo[level], slot);
                    m_dirty_hi[level] = std::max(m_dirty_hi[level], slot + 1);
                }
                m_slot_bricks[base + slot] = {x, y, z};
            }
        }
    }
    Job job;
    job.box = box;
    job.kind = JobKind::find;
    m_jobs.push_back(std::move(job));
}

// --- Jobs -----------------------------------------------------------------

void VoxelScene::mark(u32 level, const voxel::BrickCoord& brick) {
    const Key key{level, brick};
    if (m_clipmap.contains(level, brick) && m_pending.insert(key).second) {
        m_order[level].push_back(key);
    }
}

bool VoxelScene::pending(u32 level, const voxel::BrickCoord& brick) const {
    return m_pending.count(Key{level, brick}) != 0;
}

template <typename OutOfTime>
bool VoxelScene::run_job(Job& job, const OutOfTime& out_of_time) {
    // Walks the job's own box (fixed, so a window move never restarts it);
    // bricks that left the window meanwhile are skipped (mark() ignores
    // them, and their slots hold other bricks).
    const u32 level = job.box.level;
    const voxel::BrickCoord o = m_clipmap.origin(level);
    const i32 n = m_clipmap.desc().bricks_per_axis;
    if (voxel::intersect(job.box, {level, o, {o.x + n, o.y + n, o.z + n}}).count() == 0) {
        return true; // all of it left the window
    }
    const voxel::BrickBox& box = job.box;
    if (!job.started) {
        job.started = true;
        job.next = 0;
        job.cursor = 0;
        job.found.clear();
        if (job.kind == JobKind::find) {
            find({m_clipmap.brick_min(level, box.min), m_clipmap.brick_min(level, box.max)});
            job.found = m_found;
        }
    }
    const u32 slots = m_clipmap.slots_per_level();
    const u64 count = box.count();
    if (job.kind == JobKind::find) {
        while (job.next < job.found.size()) {
            const u32 i = job.found[job.next];
            const Box& b = i < m_copies.size() ? m_copies[i].box : Box{};
            const voxel::BrickBox under =
                b.empty() ? voxel::BrickBox{} : voxel::intersect(m_clipmap.bricks_touching(level, b.min, b.max), box);
            const u64 bricks = under.count();
            while (job.cursor < bricks) {
                mark(level, brick_of(under, job.cursor++));
                if (job.cursor % k_marks_per_check == 0 && out_of_time()) {
                    return false;
                }
            }
            job.cursor = 0;
            ++job.next;
            if (job.next % 64 == 0 && out_of_time()) {
                return false;
            }
        }
        // Unbuilt slots nothing is over: empty.
        while (job.cursor < count) {
            const voxel::BrickCoord b = brick_of(box, job.cursor++);
            const u32 slot = m_clipmap.slot(level, b);
            const u32 s = level * slots + slot;
            if (m_slots[s] == gpu::k_unbuilt && m_slot_bricks[s] == b && !pending(level, b) &&
                m_retry.count(Key{level, b}) == 0) {
                set_slot(level, slot, gpu::k_no_brick, b);
            }
            if (job.cursor % (k_marks_per_check * 4) == 0 && out_of_time()) {
                return false;
            }
        }
        return true;
    }
    const bool every = job.kind == JobKind::all;
    while (job.cursor < count) {
        const voxel::BrickCoord b = brick_of(box, job.cursor++);
        const u32 s = level * slots + m_clipmap.slot(level, b);
        if (every || (m_slots[s] < gpu::k_unbuilt && m_slot_bricks[s] == b)) {
            mark(level, b);
        }
        if (job.cursor % k_marks_per_check == 0 && out_of_time()) {
            return false;
        }
    }
    return true;
}

void VoxelScene::job_done(const Job& job) {
    if (job.rebuild && m_rebuild_jobs > 0 && --m_rebuild_jobs == 0 && m_rebuild_again) {
        push_rebuild();
    }
}

// --- Slots and bricks -----------------------------------------------------

void VoxelScene::set_slot(u32 level, u32 slot, u32 value, const voxel::BrickCoord& brick) {
    const u32 s = level * m_clipmap.slots_per_level() + slot;
    if (m_slots[s] == gpu::k_unbuilt) {
        --m_unbuilt[level];
    }
    if (value == gpu::k_unbuilt) {
        ++m_unbuilt[level];
    }
    if (m_slots[s] != value) {
        m_dirty_lo[level] = std::min(m_dirty_lo[level], slot);
        m_dirty_hi[level] = std::max(m_dirty_hi[level], slot + 1);
    }
    m_slots[s] = value;
    m_slot_bricks[s] = brick;
}

void VoxelScene::remove(u32 level, u32 slot, const voxel::BrickCoord& brick) {
    const u32 s = level * m_clipmap.slots_per_level() + slot;
    if (m_slots[s] < gpu::k_unbuilt) {
        m_free.push_back(m_slots[s]);
        --m_used;
        m_loose[level] = true;
    }
    set_slot(level, slot, gpu::k_no_brick, brick);
}

void VoxelScene::grow_filled(u32 level, const voxel::BrickCoord& b) {
    voxel::BrickBox& f = m_filled[level];
    if (f.count() == 0) {
        f = {level, b, {b.x + 1, b.y + 1, b.z + 1}};
        return;
    }
    f.min = {std::min(f.min.x, b.x), std::min(f.min.y, b.y), std::min(f.min.z, b.z)};
    f.max = {std::max(f.max.x, b.x + 1), std::max(f.max.y, b.y + 1), std::max(f.max.z, b.z + 1)};
}

void VoxelScene::tighten_filled(u32 level) {
    const u32 slots = m_clipmap.slots_per_level();
    m_filled[level] = {level, {}, {}};
    for (u32 slot = 0; slot < slots; ++slot) {
        const u32 s = level * slots + slot;
        if (m_slots[s] < gpu::k_unbuilt) {
            grow_filled(level, m_slot_bricks[s]);
        }
    }
    m_loose[level] = false;
}

void VoxelScene::touch_building(const Box& box) {
    if (m_building.active && !box.empty() &&
        overlaps(box.min, box.max, m_building.box.min, m_building.box.max)) {
        m_building.again = true;
    }
}

template <typename OutOfTime>
bool VoxelScene::build_step(const MeshLookup& meshes, const OutOfTime& out_of_time) {
    Building& job = m_building;
    const u32 level = job.key.level;
    const DVec3 origin = job.box.min;
    const float voxel = static_cast<float>(m_clipmap.voxel_size(level));
    const float edge = voxel * static_cast<float>(voxel::k_brick_edge);
    while (job.next < job.found.size()) {
        const u32 i = job.found[job.next++];
        if (i >= m_copies.size()) {
            continue; // removed meanwhile (job.again is set)
        }
        const Copy& copy = m_copies[i];
        const Instance& in = copy.instance;
        MeshCache* m = mesh_cache(in.mesh, meshes);
        const float s = max_abs_scale(in.scale);
        if (m != nullptr && s > 0.0f && std::isfinite(s)) {
            // Detail to half a voxel: the cut whose error is the power of
            // two at or under that (shared by instances of similar scale).
            const float wanted = 0.5f * voxel / s;
            const i32 exponent = static_cast<i32>(std::clamp(std::floor(std::log2(wanted)), -40.0f, 40.0f));
            auto cut_it = m->cuts.find(exponent);
            if (cut_it == m->cuts.end()) {
                cut_it = m->cuts.emplace(exponent, cut(*m->mesh, std::ldexp(1.0f, exponent))).first;
            }
            const Vec3 rel = to_camera_relative(in.position, origin); // the brick's frame
            const bool mirrored = in.scale.x * in.scale.y * in.scale.z < 0.0f;
            const auto place = [&](const Vec3& p) {
                return rel + rotate(in.rotation, Vec3{p.x * in.scale.x, p.y * in.scale.y, p.z * in.scale.z});
            };
            const geometry::ClusterMesh& mesh = *m->mesh;
            for (const u32 ci : cut_it->second) {
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
                    m_builder.add_triangle(a, b, d, copy.color);
                }
            }
        }
        if (job.next % 16 == 0 && job.next < job.found.size() && out_of_time()) {
            return false;
        }
    }
    return true;
}

bool VoxelScene::finish_build(rhi::CommandList& cmd, bool& pool_writable) {
    Building& job = m_building;
    job.active = false;
    const Key key = job.key;
    const u32 level = key.level;
    if (job.again) {
        mark(level, key.brick); // built again with what changed
    }
    if (!m_clipmap.contains(level, key.brick)) {
        return true; // left the window meanwhile
    }
    const u32 slot = m_clipmap.slot(level, key.brick);
    const u32 s = level * m_clipmap.slots_per_level() + slot;
    const bool here = m_slots[s] < gpu::k_unbuilt && m_slot_bricks[s] == key.brick;
    if (m_slots[s] < gpu::k_unbuilt && !here) {
        remove(level, slot, key.brick); // a brick that left the window (entering frees them; never kept)
    }
    if (m_builder.empty()) {
        remove(level, slot, key.brick);
        m_retry.erase(key);
        return true;
    }
    u32 index = here ? m_slots[s] : gpu::k_no_brick;
    if (index == gpu::k_no_brick) {
        if (m_free.empty()) {
            // Waits for room; the coarser level shows there meanwhile.
            m_retry.insert(key);
            set_slot(level, slot, gpu::k_unbuilt, key.brick);
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
    const rhi::TransientMemory upload = cmd.upload({reinterpret_cast<const u8*>(&brick), sizeof(brick)});
    if (upload.data == nullptr) {
        if (!here) {
            m_free.push_back(index);
            --m_used;
        }
        mark(level, key.brick); // again later (out of memory: the device is likely lost)
        return false;
    }
    if (!pool_writable) {
        cmd.barrier(m_pool, rhi::ResourceState::copy_dest);
        pool_writable = true;
    }
    cmd.copy_buffer(m_pool, u64{index} * sizeof(voxel::Brick), upload.buffer, upload.offset, sizeof(brick));
    m_occupancy[index] = brick.occupancy;
    m_retry.erase(key);
    ++m_stats.uploaded;
    if (!here) {
        set_slot(level, slot, index, key.brick);
        grow_filled(level, key.brick);
    }
    return true;
}

void VoxelScene::upload_tables(rhi::CommandList& cmd) {
    const u32 slots = m_clipmap.slots_per_level();
    bool table_writable = false;
    for (u32 level = 0; level < m_clipmap.levels(); ++level) {
        const u32 lo = m_dirty_lo[level];
        const u32 hi = m_dirty_hi[level];
        if (lo >= hi) {
            continue;
        }
        const u32 first = level * slots + lo;
        const rhi::TransientMemory upload = cmd.upload_array(std::span<const u32>(m_slots.data() + first, hi - lo));
        if (upload.data == nullptr) {
            break; // out of memory: the device is lost (stays dirty)
        }
        if (!table_writable) {
            cmd.barrier(m_table, rhi::ResourceState::copy_dest);
            table_writable = true;
        }
        cmd.copy_buffer(m_table, u64{first} * 4, upload.buffer, upload.offset, u64{hi - lo} * 4);
        m_dirty_lo[level] = slots;
        m_dirty_hi[level] = 0;
    }
    if (table_writable) {
        cmd.barrier(m_table, rhi::ResourceState::shader_read);
    }
}

// --- The frame ------------------------------------------------------------

void VoxelScene::update(rhi::CommandList& cmd, const DVec3& camera, std::span<const Instance> instances,
                        std::span<const Material> materials, u64 meshes_version, const MeshLookup& meshes,
                        float budget_ms) {
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    m_stats.built = 0;
    m_stats.uploaded = 0;
    if (!m_pool.valid()) {
        return;
    }
    const double budget = std::max(0.0, static_cast<double>(budget_ms));
    const auto elapsed = [&start] {
        return std::chrono::duration<double, std::milli>(clock::now() - start).count();
    };

    // Coarse levels' slabs first (recenter lists them finest first).
    const std::vector<voxel::BrickBox> entered = m_clipmap.recenter(camera);
    for (usize i = entered.size(); i-- > 0;) {
        enter(entered[i]);
    }
    // Bricks left behind still waiting to be built: dropped (finer levels'
    // queues may not be reached for a while).
    for (usize i = 0; i < entered.size(); ++i) {
        const u32 level = entered[i].level;
        if (i > 0 && entered[i - 1].level == level) {
            continue; // once per level
        }
        std::deque<Key>& order = m_order[level];
        std::deque<Key> kept;
        for (const Key& key : order) {
            if (m_clipmap.contains(level, key.brick)) {
                kept.push_back(key);
            } else {
                m_pending.erase(key);
            }
        }
        order = std::move(kept);
    }
    see_changes(instances, materials, meshes_version, meshes);
    // Waiting changes become jobs while the queue is short.
    while (m_jobs.size() < k_max_jobs && (!m_old.empty() || !m_changed_order.empty())) {
        if (!m_old.empty()) {
            const Box box = m_old.front();
            m_old.pop_front();
            add_jobs(box, JobKind::present);
        } else {
            const u32 i = m_changed_order.front();
            m_changed_order.pop_front();
            m_changed.erase(i);
            if (i < m_copies.size()) {
                add_jobs(m_copies[i].box, JobKind::all);
            }
        }
    }
    // Bricks that did not fit, as room frees up (dropping those nothing is
    // over any more, a few hundred looks a frame at most).
    if (!m_retry.empty() && !m_free.empty()) {
        usize room = m_free.size();
        u32 looks = 256;
        for (auto it = m_retry.begin(); it != m_retry.end() && room > 0 && looks > 0 && elapsed() < budget * 0.25;
             --looks) {
            const Key key = *it;
            if (!m_clipmap.contains(key.level, key.brick)) {
                it = m_retry.erase(it); // left the view
                continue;
            }
            find({m_clipmap.brick_min(key.level, key.brick),
                  m_clipmap.brick_min(key.level, {key.brick.x + 1, key.brick.y + 1, key.brick.z + 1})});
            if (m_found.empty()) {
                const u32 slot = m_clipmap.slot(key.level, key.brick);
                const u32 s = key.level * m_clipmap.slots_per_level() + slot;
                if (m_slots[s] == gpu::k_unbuilt && m_slot_bricks[s] == key.brick && !pending(key.level, key.brick)) {
                    set_slot(key.level, slot, gpu::k_no_brick, key.brick);
                }
                it = m_retry.erase(it);
                continue;
            }
            mark(key.level, key.brick);
            ++it;
            --room;
        }
    }

    // Finding bricks takes up to half the budget while bricks wait to be
    // built (at least one step a frame), then building the rest (at least
    // one brick), coarse levels first.
    const double jobs_budget = m_pending.empty() ? budget : budget * 0.5;
    const auto jobs_out_of_time = [&] { return elapsed() >= jobs_budget; };
    for (bool first = true; !m_jobs.empty() && (first || !jobs_out_of_time()); first = false) {
        if (!run_job(m_jobs.front(), jobs_out_of_time)) {
            break;
        }
        const Job done = std::move(m_jobs.front());
        m_jobs.pop_front();
        job_done(done);
    }
    // A brick at a time: the one under way first, then the coarsest waiting.
    bool pool_writable = false;
    bool progress = false; // at least one step a frame
    const auto build_out_of_time = [&] { return elapsed() >= budget; };
    for (;;) {
        if (progress && (build_out_of_time() || m_stats.uploaded >= k_max_uploads)) {
            break;
        }
        if (!m_building.active) {
            bool started = false;
            for (u32 l = m_clipmap.levels(); l-- > 0 && !started;) {
                std::deque<Key>& order = m_order[l];
                while (!order.empty()) {
                    const Key key = order.front();
                    order.pop_front();
                    if (m_pending.erase(key) == 0 || !m_clipmap.contains(key.level, key.brick)) {
                        continue; // left the window while waiting
                    }
                    m_building.active = true;
                    m_building.key = key;
                    m_building.box = {m_clipmap.brick_min(key.level, key.brick),
                                      m_clipmap.brick_min(key.level, {key.brick.x + 1, key.brick.y + 1,
                                                                      key.brick.z + 1})};
                    find(m_building.box);
                    m_building.found = m_found;
                    m_building.next = 0;
                    m_building.again = false;
                    m_builder.reset(static_cast<float>(m_clipmap.voxel_size(key.level)));
                    started = true;
                    break;
                }
            }
            if (!started) {
                break; // nothing waits
            }
        }
        progress = true;
        if (!build_step(meshes, build_out_of_time)) {
            break; // more next frame
        }
        ++m_stats.built;
        if (!finish_build(cmd, pool_writable)) {
            break;
        }
    }
    if (pool_writable) {
        cmd.barrier(m_pool, rhi::ResourceState::shader_read);
    }
    // One level's bounds tightened a frame, once it has settled.
    for (u32 level = 0; level < m_clipmap.levels(); ++level) {
        if (m_loose[level] && m_unbuilt[level] == 0) {
            tighten_filled(level);
            break;
        }
    }
    upload_tables(cmd);

    m_stats.bricks = m_used;
    m_stats.pending = static_cast<u32>(m_pending.size() + m_jobs.size() + m_old.size() + m_changed.size() +
                                       (m_rebuild_again ? 1 : 0) + (m_building.active ? 1 : 0));
    m_stats.missing = static_cast<u32>(m_retry.size());
    m_stats.overflow = m_stats.missing > 0;
    m_stats.cpu_ms = static_cast<float>(elapsed());
}

void VoxelScene::idle() {
    m_stats.built = 0;
    m_stats.uploaded = 0;
    m_stats.cpu_ms = 0.0f;
}

// --- Queries --------------------------------------------------------------

u32 VoxelScene::brick_index(u32 level, const voxel::BrickCoord& brick) const {
    if (level >= m_clipmap.levels() || !m_clipmap.contains(level, brick)) {
        return gpu::k_no_brick;
    }
    const u32 s = level * m_clipmap.slots_per_level() + m_clipmap.slot(level, brick);
    return m_slot_bricks[s] == brick && m_slots[s] < gpu::k_unbuilt ? m_slots[s] : gpu::k_no_brick;
}

VoxelProbe VoxelScene::probe(u32 level, const DVec3& position) const {
    if (level >= m_clipmap.levels() || m_slots.empty()) {
        return VoxelProbe::outside;
    }
    const voxel::BrickCoord b = m_clipmap.brick_at(level, position);
    if (!m_clipmap.contains(level, b)) {
        return VoxelProbe::outside;
    }
    const u32 s = level * m_clipmap.slots_per_level() + m_clipmap.slot(level, b);
    if (!(m_slot_bricks[s] == b) || m_slots[s] == gpu::k_unbuilt) {
        return VoxelProbe::building;
    }
    if (m_slots[s] == gpu::k_no_brick) {
        return VoxelProbe::empty;
    }
    const DVec3 lo = m_clipmap.brick_min(level, b);
    const double vs = m_clipmap.voxel_size(level);
    const auto coord = [vs](double v) {
        return static_cast<u32>(std::clamp(std::floor(v / vs), 0.0, double(voxel::k_brick_edge - 1)));
    };
    const u32 i = voxel::voxel_index(coord(position.x - lo.x), coord(position.y - lo.y), coord(position.z - lo.z));
    return (m_occupancy[m_slots[s]][i / 32] >> (i % 32) & 1u) != 0 ? VoxelProbe::solid : VoxelProbe::empty;
}

void VoxelScene::write_levels(gpu::VoxelLevel* out, const DVec3& camera) const {
    const i32 n = m_clipmap.desc().bricks_per_axis;
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
        // While part of the level is unbuilt, rays must look at every slot
        // there (to fall back to the coarser level).
        const voxel::BrickBox window{level, o, {o.x + n, o.y + n, o.z + n}};
        const voxel::BrickBox f =
            m_unbuilt.empty() || m_unbuilt[level] > 0 ? window : voxel::intersect(m_filled[level], window);
        if (f.count() == 0) {
            for (int i = 0; i < 3; ++i) {
                l.filled_min[i] = 1.0f;
                l.filled_max[i] = -1.0f;
            }
        } else {
            const Vec3 lo = to_camera_relative(m_clipmap.brick_min(level, f.min), camera);
            const Vec3 hi = to_camera_relative(m_clipmap.brick_min(level, f.max), camera);
            l.filled_min[0] = lo.x;
            l.filled_min[1] = lo.y;
            l.filled_min[2] = lo.z;
            l.filled_max[0] = hi.x;
            l.filled_max[1] = hi.y;
            l.filled_max[2] = hi.z;
        }
        l.filled_min[3] = 0.0f;
        l.filled_max[3] = 0.0f;
    }
}

} // namespace giant::render
