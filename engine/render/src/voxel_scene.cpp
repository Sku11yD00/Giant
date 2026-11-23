#include "voxel_scene.h"

#include "cells.h"

#include "giant/core/assert.h"
#include "giant/core/log.h"
#include "gpu_layout.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <thread>

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
// Waiting changes are gathered in squares of this many metres; a box wider
// than two of them waits on its own.
constexpr double k_region = 64.0;
// Marks (cheap set inserts) between looks at the clock.
constexpr u64 k_marks_per_check = 256;
// Bricks being built at once (on the workers or waiting for one): enough to keep them busy.
constexpr usize k_most_builds = 32;
// A brick over at most this many instances and water triangles is built on the main thread as it is gathered (as the
// rest were, between looks at the clock): most are, and a frame finishes them without waiting.
constexpr u32 k_inline_items = 16;
// ...while such builds are cheap: averaging over this (a debug build, a starved machine), they
// go to the workers like the rest, which have the whole frame, bar one in this many to keep
// the average current.
constexpr double k_inline_ms = 0.05;
constexpr u32 k_inline_probe = 64;
// A build no worker has taken after this many frames is built on the main thread (one a frame):
// the workers may get no core (a software GPU on every core, say), and it would wait for ever.
constexpr u64 k_most_waiting_updates = 30;
// With at least this budget a frame (behind a loading screen, in tests), the main thread builds
// bricks too, a whole brick at a time, and waits on the workers' rather than leave time unused.
constexpr double k_help_ms = 4.0;
// Bricks under way at once when the workers prepare them (waiting for a worker, most of them),
// how many are handed over in one job, and the most instances over one a worker copies out while
// it holds its share of the scene (the main thread waits for that share to change anything).
constexpr usize k_most_under_way = 512;
constexpr usize k_batch = 16;
constexpr usize k_worker_items = 2048;
// A brick a worker left for a mesh never looked up goes back to the workers this many times (the
// meshes forgotten again meanwhile, each time), then is the main thread's.
constexpr u32 k_most_sent_back = 2;
// An instance whose sphere's radius is at most this share of a voxel is added as one point
// (BrickBuilder::add_point), not triangle by triangle: a quarter, so it spans a voxel or two.
constexpr float k_point_share = 0.25f;

DVec3 to_d(const Vec3& v) { return {v.x, v.y, v.z}; }
DVec3 lower(const DVec3& a, const DVec3& b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
DVec3 upper(const DVec3& a, const DVec3& b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }

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

VoxelScene::VoxelScene(rhi::Device& device, const voxel::ClipmapDesc& clipmap, u32 brick_capacity, u32 more_workers)
    : m_device(device), m_clipmap(clipmap), m_capacity(std::clamp(brick_capacity, 1u, gpu::k_unbuilt - 1)),
      // A quarter of the cores, two to four: the other systems' workers want theirs.
      m_build_jobs(std::clamp(std::thread::hardware_concurrency() / 4, 2u, 4u)) {
    // Behind a loading screen nothing else wants them: up to half the cores, eight at most.
    const u32 base = m_build_jobs.worker_count();
    const u32 all = std::clamp(std::thread::hardware_concurrency() / 2, 2u, 8u);
    const u32 more = more_workers == k_auto ? (all > base ? all - base : 0u) : std::min(more_workers, 16u);
    if (more > 0) {
        m_more_jobs = std::make_unique<JobSystem>(more);
    }
    static std::atomic<u64> scenes{0};
    m_serial = scenes.fetch_add(1, std::memory_order_relaxed) + 1;
}

VoxelScene::~VoxelScene() {
    cancel_builds(true); // (the workers would otherwise build every queued one as they stop)
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
    m_again.clear();
    m_occupancy.assign(m_capacity, {});
    m_has_stand_in.assign(m_capacity, 0);
    m_owner.assign(m_capacity, ~0u);
    m_placed_at.assign(m_capacity, 0);
    m_stand_in_bricks.assign(levels, 0);
    m_free.clear();
    for (u32 i = m_capacity; i > 0; --i) {
        m_free.push_back(i - 1);
    }
    m_stats.capacity = m_capacity;
    return true;
}

VoxelScene::Cut VoxelScene::cut(const geometry::ClusterMesh& mesh, float error, bool sums) {
    Cut out;
    for (u32 ci = 0; ci < mesh.clusters.size(); ++ci) {
        const geometry::Cluster& c = mesh.clusters[ci];
        const float own = c.refined >= 0 ? mesh.groups[static_cast<usize>(c.refined)].simplified.error : 0.0f;
        const float replacement = mesh.groups[c.group].simplified.error;
        if (own <= error && replacement > error) {
            out.clusters.push_back(ci);
            if (!sums) {
                continue;
            }
            for (u32 t = 0; t < c.triangle_count; ++t) {
                const u32* idx = &mesh.indices[c.first_index + t * 3];
                const Vec3 face = cross(mesh.vertices[idx[1]].position - mesh.vertices[idx[0]].position,
                                        mesh.vertices[idx[2]].position - mesh.vertices[idx[0]].position);
                const float twice = length(face);
                if (twice > 0.0f && std::isfinite(twice)) { // (add_triangle leaves out what has no area)
                    ++out.triangles;
                    out.area += 0.5f * twice;
                    out.normals += face * (1.0f / twice);
                }
            }
        }
    }
    return out;
}

// --- Instances ------------------------------------------------------------

VoxelScene::Writing::Writing(VoxelScene& voxels) : scene(voxels) {
    scene.m_writing.store(true, std::memory_order_release); // (the workers take no new share)
    scene.m_scene_lock.lock();
    scene.m_touchable_ready = false;
}

VoxelScene::Writing::~Writing() {
    scene.m_copy_count.store(scene.m_copies.size(), std::memory_order_relaxed);
    ++scene.m_writes;
    scene.m_scene_lock.unlock();
    scene.m_writing.store(false, std::memory_order_release);
    scene.m_writing.notify_all();
}

VoxelScene::MeshCache* VoxelScene::known_mesh(MeshId id, bool& known, std::shared_ptr<MeshCache>* hold) {
    const std::lock_guard lock(m_mesh_mutex);
    const auto found = m_meshes.find(id);
    known = found != m_meshes.end();
    if (!known || found->second->mesh == nullptr) {
        return nullptr;
    }
    if (hold != nullptr) {
        *hold = found->second;
    }
    return found->second.get();
}

VoxelScene::MeshCache* VoxelScene::mesh_cache(MeshId id, const MeshLookup& meshes) {
    {
        bool known = false;
        MeshCache* const cache = known_mesh(id, known, nullptr);
        if (known) {
            return cache;
        }
    }
    // (Only the main thread adds: looked up outside the lock, the workers not held meanwhile.)
    std::shared_ptr<const geometry::ClusterMesh> mesh = meshes ? meshes(id) : nullptr;
    constexpr float inf = std::numeric_limits<float>::infinity();
    Vec3 min{inf, inf, inf};
    Vec3 max{-inf, -inf, -inf};
    if (mesh != nullptr && !mesh->empty()) {
        // Around the original clusters' spheres: one pass over the clusters,
        // not the vertices.
        for (const geometry::Cluster& c : mesh->clusters) {
            if (c.refined >= 0) {
                continue;
            }
            const Vec3 p = c.bounds.center;
            const float r = c.bounds.radius;
            min = {std::min(min.x, p.x - r), std::min(min.y, p.y - r), std::min(min.z, p.z - r)};
            max = {std::max(max.x, p.x + r), std::max(max.y, p.y + r), std::max(max.z, p.z + r)};
        }
        if (!(min.x <= max.x)) {
            const geometry::Sphere& sphere = mesh->bounds;
            min = sphere.center - Vec3{sphere.radius, sphere.radius, sphere.radius};
            max = sphere.center + Vec3{sphere.radius, sphere.radius, sphere.radius};
        }
    } else {
        mesh = nullptr; // remembered as missing until the meshes change
        m_missing.insert(id);
    }
    const std::lock_guard lock(m_mesh_mutex);
    std::shared_ptr<MeshCache>& entry = m_meshes[id];
    entry = std::make_shared<MeshCache>();
    MeshCache& stored = *entry;
    stored.mesh = std::move(mesh);
    stored.min = min;
    stored.max = max;
    return stored.mesh != nullptr ? &stored : nullptr;
}

std::shared_ptr<const VoxelScene::Cut> VoxelScene::cut_for(MeshCache& cache, i32 exponent, bool point) {
    const i32 key = exponent * 2 + (point ? 1 : 0);
    {
        const std::lock_guard lock(m_mesh_mutex);
        const auto it = cache.cuts.find(key);
        if (it != cache.cuts.end()) {
            return it->second;
        }
    }
    // Made outside the lock (a walk over the mesh): two threads may each make one, and the first
    // filed is the one kept.
    std::shared_ptr<const Cut> made = std::make_shared<const Cut>(cut(*cache.mesh, std::ldexp(1.0f, exponent), point));
    const std::lock_guard lock(m_mesh_mutex);
    return cache.cuts.try_emplace(key, std::move(made)).first->second;
}

namespace {
// The grid cell of `v` on an axis the mesh spans from `from` to `to` (make_grid and may_touch
// number them the same way; a flat mesh has no width on an axis: every cell of it is cell 0).
u32 grid_cell(float v, float from, float to) {
    constexpr u32 n = 16;
    const float at = (v - from) / (to - from) * static_cast<float>(n);
    if (!(at > 0.0f)) {
        return 0; // (also not a number: no width, or a span no float holds)
    }
    return static_cast<u32>(std::min(at, static_cast<float>(n - 1)));
}
bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
} // namespace

void VoxelScene::ensure_grid(MeshCache& cache) {
    static_assert(MeshCache::k_grid == 16);
    if (cache.grid_ready.load(std::memory_order_acquire)) {
        return;
    }
    // Made outside the lock (a walk over the mesh's triangles), filed under it: two threads may
    // each make one, and the first filed is the one kept.
    std::vector<u16> grid;
    Vec3 grid_min{};
    Vec3 grid_max{};
    const auto file = [&] {
        const std::lock_guard lock(m_mesh_mutex);
        if (!cache.grid_ready.load(std::memory_order_relaxed)) {
            cache.grid = std::move(grid);
            cache.grid_min = grid_min;
            cache.grid_max = grid_max;
            cache.grid_ready.store(true, std::memory_order_release);
        }
    };
    const geometry::ClusterMesh& mesh = *cache.mesh;
    u64 triangles = 0;
    for (const geometry::Cluster& c : mesh.clusters) {
        triangles += c.refined < 0 ? c.triangle_count : 0u;
    }
    if (triangles == 0 || triangles > MeshCache::k_grid_triangles) {
        return file();
    }
    constexpr float inf = std::numeric_limits<float>::infinity();
    Vec3 lo{inf, inf, inf};
    Vec3 hi{-inf, -inf, -inf};
    const auto each = [&](const auto& fn) {
        for (const geometry::Cluster& c : mesh.clusters) {
            if (c.refined >= 0) {
                continue;
            }
            for (u32 t = 0; t < c.triangle_count; ++t) {
                const u32* idx = &mesh.indices[c.first_index + t * 3];
                const Vec3& p0 = mesh.vertices[idx[0]].position;
                const Vec3& p1 = mesh.vertices[idx[1]].position;
                const Vec3& p2 = mesh.vertices[idx[2]].position;
                if (!finite(p0) || !finite(p1) || !finite(p2)) {
                    continue; // (a corner not a number: a build adds no voxel for it either)
                }
                fn(Vec3{std::min({p0.x, p1.x, p2.x}), std::min({p0.y, p1.y, p2.y}), std::min({p0.z, p1.z, p2.z})},
                   Vec3{std::max({p0.x, p1.x, p2.x}), std::max({p0.y, p1.y, p2.y}), std::max({p0.z, p1.z, p2.z})});
            }
        }
    };
    each([&](const Vec3& a, const Vec3& b) {
        lo = {std::min(lo.x, a.x), std::min(lo.y, a.y), std::min(lo.z, a.z)};
        hi = {std::max(hi.x, b.x), std::max(hi.y, b.y), std::max(hi.z, b.z)};
    });
    if (!finite(lo) || !finite(hi)) {
        return file(); // (no triangle with every corner a number: nothing ruled out)
    }
    constexpr u32 n = MeshCache::k_grid;
    grid_min = lo;
    grid_max = hi;
    grid.assign(usize{n} * n, 0);
    each([&](const Vec3& a, const Vec3& b) {
        const u32 x0 = grid_cell(a.x, lo.x, hi.x), x1 = grid_cell(b.x, lo.x, hi.x);
        const u32 y0 = grid_cell(a.y, lo.y, hi.y), y1 = grid_cell(b.y, lo.y, hi.y);
        const u32 z0 = grid_cell(a.z, lo.z, hi.z), z1 = grid_cell(b.z, lo.z, hi.z);
        const u16 bits = static_cast<u16>((u32{2} << x1) - (u32{1} << x0));
        for (u32 z = z0; z <= z1; ++z) {
            for (u32 y = y0; y <= y1; ++y) {
                grid[y + usize{n} * z] |= bits;
            }
        }
    });
    file();
}

bool VoxelScene::may_touch(const Copy& copy, MeshCache& cache, const Box& box, double voxel) {
    const Instance& in = copy.instance;
    MeshCache* const m = &cache;
    const float s = max_abs_scale(in.scale);
    if (!(s > 0.0f) || !std::isfinite(s)) {
        return false; // (as a build)
    }
    if (m->mesh->bounds.radius * s <= k_point_share * static_cast<float>(voxel)) {
        return true; // built as a point, wherever its triangles are
    }
    if (!(std::abs(in.scale.x) > 0.0f && std::abs(in.scale.y) > 0.0f && std::abs(in.scale.z) > 0.0f)) {
        return true; // flattened: not turned back into the mesh's space
    }
    ensure_grid(*m);
    if (m->grid.empty()) {
        return true;
    }
    // Not a rotation (its length not one): `rotate` skews with it, and is not undone below.
    const Quat& q = in.rotation;
    if (!(std::abs(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w - 1.0f) <= 1e-4f)) {
        return true;
    }
    // The box, two voxels wider (a build draws a simplified cut, off the original triangles by
    // half a voxel as its error is told, and that error is the largest of its levels', not their
    // sum), in the mesh's space: the box around its eight corners there.
    const double pad = 2.0 * voxel;
    constexpr float inf = std::numeric_limits<float>::infinity();
    Vec3 lo{inf, inf, inf};
    Vec3 hi{-inf, -inf, -inf};
    const Quat back = conjugate(in.rotation);
    for (u32 corner = 0; corner < 8; ++corner) {
        const DVec3 world{(corner & 1) != 0 ? box.max.x + pad : box.min.x - pad,
                          (corner & 2) != 0 ? box.max.y + pad : box.min.y - pad,
                          (corner & 4) != 0 ? box.max.z + pad : box.min.z - pad};
        const Vec3 turned = rotate(back, to_camera_relative(world, in.position));
        const Vec3 p{turned.x / in.scale.x, turned.y / in.scale.y, turned.z / in.scale.z};
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    if (!finite(lo) || !finite(hi)) {
        return true;
    }
    const Vec3& from = m->grid_min;
    const Vec3& to = m->grid_max;
    if (hi.x < from.x || hi.y < from.y || hi.z < from.z || lo.x > to.x || lo.y > to.y || lo.z > to.z) {
        return false;
    }
    constexpr u32 n = MeshCache::k_grid;
    const u32 x0 = grid_cell(lo.x, from.x, to.x), x1 = grid_cell(hi.x, from.x, to.x);
    const u32 y0 = grid_cell(lo.y, from.y, to.y), y1 = grid_cell(hi.y, from.y, to.y);
    const u32 z0 = grid_cell(lo.z, from.z, to.z), z1 = grid_cell(hi.z, from.z, to.z);
    const u16 bits = static_cast<u16>((u32{2} << x1) - (u32{1} << x0));
    for (u32 z = z0; z <= z1; ++z) {
        for (u32 y = y0; y <= y1; ++y) {
            if ((m->grid[y + usize{n} * z] & bits) != 0) {
                return true;
            }
        }
    }
    return false;
}

VoxelScene::Box VoxelScene::bounds_of(const Instance& in, const MeshLookup& meshes) {
    constexpr double inf = std::numeric_limits<double>::infinity();
    const Box none{{inf, inf, inf}, {-inf, -inf, -inf}}; // empty: overlaps nothing
    const MeshCache* m = in.moves ? nullptr : mesh_cache(in.mesh, meshes);
    if (m == nullptr) {
        return none; // no mesh, or not held (it moves)
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
    const Material& m = in.material < materials.size() ? materials[in.material] : Material{};
    // The texture's average only while it has one (as coverage_of).
    const Vec3 a = m.base_color_texture != k_no_texture ? m.base_color_average : Vec3{1.0f, 1.0f, 1.0f};
    // NaN as 0, so it compares equal to itself (else it would build again every frame).
    const auto clean = [](float v) { return v == v ? v : 0.0f; };
    return {clean(m.base_color.x * a.x), clean(m.base_color.y * a.y), clean(m.base_color.z * a.z)};
}

float VoxelScene::coverage_of(const Instance& in, std::span<const Material> materials) const {
    if (in.material >= materials.size()) {
        return 1.0f;
    }
    const Material& m = materials[in.material];
    // Cut out only as the renderer draws it (a cutoff and a texture);
    // NaN counts as solid.
    if (!(m.alpha_cutoff > 0.0f) || m.base_color_texture == k_no_texture || !(m.coverage >= 0.0f)) {
        return 1.0f;
    }
    return std::min(m.coverage, 1.0f);
}

float VoxelScene::canopy_of(const Instance& in, std::span<const Material> materials) const {
    if (in.material >= materials.size()) {
        return 0.0f;
    }
    const float c = materials[in.material].canopy;
    return std::isfinite(c) ? std::clamp(c, 0.0f, 1.0f) : 0.0f; // as the renderer reads it
}

bool VoxelScene::material_changes(std::span<const Material> materials) {
    m_material_changed.assign(materials.size(), 0);
    bool any = false;
    m_material_looks.resize(materials.size());
    for (u32 i = 0; i < materials.size(); ++i) {
        const Instance probe{.material = i};
        const MaterialLook look{color_of(probe, materials), coverage_of(probe, materials), canopy_of(probe, materials)};
        if (!(look == m_material_looks[i])) {
            m_material_looks[i] = look;
            m_material_changed[i] = 1;
            any = true;
        }
    }
    return any;
}

bool VoxelScene::differs(const Instance& in, std::span<const Material> materials, const Copy& copy) const {
    const Instance& c = copy.instance;
    if (in.moves && c.moves) {
        // Not held either way: where it is does not matter.
        return !(in.scale == c.scale && in.mesh == c.mesh && in.material == c.material &&
                 in.always_mesh == c.always_mesh && color_of(in, materials) == copy.color &&
                 coverage_of(in, materials) == copy.coverage && canopy_of(in, materials) == copy.canopy);
    }
    return in.moves != c.moves || !(in.position == c.position && in.rotation == c.rotation && in.scale == c.scale && in.mesh == c.mesh &&
             in.material == c.material && in.always_mesh == c.always_mesh && color_of(in, materials) == copy.color &&
                 coverage_of(in, materials) == copy.coverage && canopy_of(in, materials) == copy.canopy);
}

void VoxelScene::set_copy(u32 index, const Instance& in, std::span<const Material> materials,
                          const MeshLookup& meshes) {
    Copy& c = m_copies[index];
    c.instance = in;
    m_copy_mesh[index] = in.mesh;
    c.color = color_of(in, materials);
    c.coverage = coverage_of(in, materials);
    c.canopy = canopy_of(in, materials);
    c.box = bounds_of(in, meshes);
    c.large = false;
    const MeshCache* m = c.box.empty() ? nullptr : mesh_cache(in.mesh, meshes);
    c.stand_in = !in.always_mesh && m != nullptr && m->mesh != nullptr &&
                 stand_in_radius_of(*m->mesh, in.scale) <= m_stand_in_radius;
}

float stand_in_radius_of(const geometry::ClusterMesh& mesh, const Vec3& scale) {
    // In float, as the GPU works it out (resolve.slang): the two agree exactly.
    const float s = std::max({std::fabs(scale.x), std::fabs(scale.y), std::fabs(scale.z)});
    return mesh.bounds.radius * s;
}

void VoxelScene::forget_mesh(MeshId mesh) {
    { // (nothing of the scene's: the workers are not held for it)
        const std::lock_guard lock(m_mesh_mutex);
        m_meshes.erase(mesh);
    }
    m_missing.erase(mesh);
    m_forgotten.push_back(mesh);
}

void VoxelScene::set_stand_in_radius(float radius) {
    const float r = radius > 0.0f ? radius : 0.0f; // NaN: none
    if (r == m_stand_in_radius) {
        return;
    }
    m_stand_in_radius = r;
    const Writing writing(*this);
    bool any = false;
    for (Copy& c : m_copies) {
        // The same rule as set_copy, from the copy's own instance.
        const auto it = m_meshes.find(c.instance.mesh);
        const geometry::ClusterMesh* mesh = it != m_meshes.end() ? it->second->mesh.get() : nullptr;
        const bool stand_in = !c.instance.always_mesh && !c.box.empty() && mesh != nullptr &&
                              stand_in_radius_of(*mesh, c.instance.scale) <= r;
        any = any || stand_in != c.stand_in;
        c.stand_in = stand_in;
    }
    if (any && m_clipmap.placed()) {
        request_rebuild();
    }
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
                const Cell cell{x, y, z};
                const auto [it, added] = m_grid.try_emplace(cell);
                if (added) {
                    m_coarse[coarse_of(cell)].push_back({cell, &it->second});
                }
                it->second.push_back(index);
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
                        const Cell cell = it->first;
                        const auto co = m_coarse.find(coarse_of(cell));
                        if (co != m_coarse.end()) {
                            std::vector<Fine>& fine = co->second;
                            for (usize k = 0; k < fine.size(); ++k) {
                                if (fine[k].cell == cell) {
                                    fine[k] = fine.back();
                                    fine.pop_back();
                                    break;
                                }
                            }
                            if (fine.empty()) {
                                m_coarse.erase(co);
                            }
                        }
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
        m_coarse.clear();
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

void VoxelScene::find(const Box& q) { find_into(q, m_found, m_seen, m_find_stamp, ~usize{0}, nullptr); }

bool VoxelScene::find_into(const Box& q, std::vector<u32>& found, std::vector<u32>& seen, u32& stamp, usize most,
                           usize* resume) const {
    found.clear();
    // More of a walk an earlier call cut short (else the first of one, or one on its own).
    const bool more = resume != nullptr && *resume != k_from_start;
    const usize skip = more ? *resume : 0;
    if (resume != nullptr) {
        *resume = 0;
    }
    if (q.empty()) {
        return true;
    }
    seen.resize(m_copies.size(), 0);
    if (!more && ++stamp == 0) {
        std::fill(seen.begin(), seen.end(), 0);
        stamp = 1;
    }
    // (`most`: given up once more are found, or sixteen times as many looked at: a worker's.)
    const usize most_looks = most > ~usize{0} / 16 ? ~usize{0} : most * 16;
    usize looks = 0;
    bool over = false;
    const auto look = [&](u32 i) {
        if (seen[i] != stamp) {
            over = ++looks > most_looks || found.size() > most;
            seen[i] = stamp;
            const Box& b = m_copies[i].box;
            if (overlaps(b.min, b.max, q.min, q.max)) {
                found.push_back(i);
            }
        }
    };
    // The lists are walked in the same order every time while the grid is as it was: `at` counts
    // the entries passed, and a call that takes up a walk skips those the calls before it passed
    // (whole lists at a time), so it holds the scene only for what is new to it.
    usize at = 0;
    const auto each = [&](const std::vector<u32>& list) {
        if (over) {
            return;
        }
        if (at + list.size() <= skip) {
            at += list.size();
            return;
        }
        usize k = skip > at ? skip - at : 0;
        at += k;
        while (k < list.size() && !over) {
            look(list[k++]);
            ++at; // (the one that went over is taken too)
        }
    };
    const Cell lo{cell_of(q.min.x), cell_of(q.min.y), cell_of(q.min.z)};
    const Cell hi{cell_of(q.max.x), cell_of(q.max.y), cell_of(q.max.z)};
    const u64 cells = u64(hi.x - lo.x + 1) * u64(hi.y - lo.y + 1) * u64(hi.z - lo.z + 1);
    if (cells > 64) {
        // A big box (a coarse brick, a whole window): the occupied cells of
        // the coarse cells it covers (or of every coarse cell, if fewer).
        const Cell clo = coarse_of(lo);
        const Cell chi = coarse_of(hi);
        const auto fine = [&](const std::vector<Fine>& cells_in) {
            for (const Fine& f : cells_in) {
                const Cell& cell = f.cell;
                if (over) {
                    return;
                }
                if (cell.x >= lo.x && cell.x <= hi.x && cell.y >= lo.y && cell.y <= hi.y && cell.z >= lo.z &&
                    cell.z <= hi.z) {
                    each(*f.list);
                }
            }
        };
        const u64 coarse = u64(chi.x - clo.x + 1) * u64(chi.y - clo.y + 1) * u64(chi.z - clo.z + 1);
        if (coarse > m_coarse.size()) {
            for (const auto& [c, cells_in] : m_coarse) {
                if (c.x >= clo.x && c.x <= chi.x && c.y >= clo.y && c.y <= chi.y && c.z >= clo.z && c.z <= chi.z) {
                    fine(cells_in);
                }
            }
        } else {
            for (i32 z = clo.z; z <= chi.z && !over; ++z) {
                for (i32 y = clo.y; y <= chi.y; ++y) {
                    for (i32 x = clo.x; x <= chi.x; ++x) {
                        const auto it = m_coarse.find(Cell{x, y, z});
                        if (it != m_coarse.end()) {
                            fine(it->second);
                        }
                    }
                }
            }
        }
    } else {
        for (i32 z = lo.z; z <= hi.z; ++z) {
            for (i32 y = lo.y; y <= hi.y; ++y) {
                for (i32 x = lo.x; x <= hi.x; ++x) {
                    const auto it = m_grid.find(Cell{x, y, z});
                    if (it != m_grid.end()) {
                        each(it->second);
                    }
                }
            }
        }
    }
    each(m_large);
    if (resume != nullptr) {
        *resume = at;
    }
    return !over;
}

// --- Seeing changes -------------------------------------------------------

VoxelScene::Region& VoxelScene::region_for(const Box& box) {
    u64 key = 0;
    if (box.max.x - box.min.x > 2.0 * k_region || box.max.z - box.min.z > 2.0 * k_region) {
        key = (u64{1} << 63) | m_next_own_region++; // on its own: merged, it would widen its neighbours'
    } else {
        const auto at = [](double v) {
            return static_cast<u64>(static_cast<u32>(static_cast<i32>(std::floor(v / k_region))));
        };
        key = at(0.5 * (box.min.x + box.max.x)) | at(0.5 * (box.min.z + box.max.z)) << 32;
        key &= ~(u64{1} << 63);
    }
    const auto [it, added] = m_regions.try_emplace(key);
    if (added) {
        m_region_order.push_back(key);
    }
    return it->second;
}

void VoxelScene::queue_old(const Box& box) {
    if (box.empty()) {
        return;
    }
    Region& r = region_for(box);
    if (r.old.empty() || !(r.old.back().min == box.min && r.old.back().max == box.max)) {
        r.old.push_back(box); // (the same box again, as one instance changes frame after frame: once)
    }
}

void VoxelScene::queue_fresh(const Box& box) {
    if (box.empty()) {
        return;
    }
    region_for(box).fresh.push_back(box);
}

namespace {

// Water's pieces: the squares (metres, as the regions) a body's triangles are filed in.
constexpr double k_water_piece = 64.0;

// A 64-bit mix of words (splitmix's finaliser per word, chained).
struct WordHash {
    u64 h = 0x9E3779B97F4A7C15ull;
    void add(u64 w) {
        u64 x = h ^ w;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        h = x ^ (x >> 31);
    }
    void add(double d) {
        u64 w = 0;
        std::memcpy(&w, &d, sizeof(w));
        add(w);
    }
    void add(float f) {
        u32 w = 0;
        std::memcpy(&w, &f, sizeof(w));
        add(u64{w});
    }
};

// The x the triangle's ground plan (x, z) spans within the row z0 <= z <= z1
// (false: it does not reach the row). The plan clipped to the row is
// convex, so it meets every square of the row whose x overlaps the span.
bool plan_span_in_row(const DVec3& a, const DVec3& b, const DVec3& c, double z0, double z1, double& x0,
                      double& x1) {
    x0 = std::numeric_limits<double>::infinity();
    x1 = -std::numeric_limits<double>::infinity();
    const DVec3* p[3] = {&a, &b, &c};
    for (int e = 0; e < 3; ++e) {
        const DVec3& s = *p[e];
        const DVec3& t = *p[(e + 1) % 3];
        if (s.z >= z0 && s.z <= z1) {
            x0 = std::min(x0, s.x);
            x1 = std::max(x1, s.x);
        }
        for (const double z : {z0, z1}) {
            if ((s.z < z && t.z > z) || (s.z > z && t.z < z)) {
                const double x = s.x + (t.x - s.x) * ((z - s.z) / (t.z - s.z));
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
            }
        }
    }
    return x0 <= x1;
}

} // namespace

bool VoxelScene::water_over(const Box& box) const {
    for (const auto& body : m_water) {
        if (!overlaps(body->box.min, body->box.max, box.min, box.max)) {
            continue;
        }
        for (const WaterPiece& piece : body->pieces) {
            if (overlaps(piece.box.min, piece.box.max, box.min, box.max)) {
                return true;
            }
        }
    }
    return false;
}

void VoxelScene::set_water(std::span<const WaterVertex> vertices, std::span<const u32> indices,
                           std::span<const WaterLook> looks, u64 version) {
    // First whether anything changed: by the caller's version, else a hash
    // of the whole (what makes the voxels: positions, the triangles, the
    // looks' colours), as it is the same nearly every frame.
    if (version != 0 && version == m_water_version) {
        return;
    }
    m_water_version = version;
    WordHash whole;
    for (const WaterVertex& v : vertices) {
        whole.add(v.position.x);
        whole.add(v.position.y);
        whole.add(v.position.z);
        whole.add(u64{v.look} | (u64{v.swell ? 1u : 0u} << 32));
    }
    for (usize i = 0; i + 1 < indices.size(); i += 2) {
        whole.add(u64{indices[i]} | (u64{indices[i + 1]} << 32));
    }
    if ((indices.size() & 1u) != 0) {
        whole.add(u64{indices.back()});
    }
    for (const WaterLook& l : looks) {
        whole.add(l.color.x);
        whole.add(l.color.y);
        whole.add(l.color.z);
    }
    whole.add(u64{vertices.size()});
    whole.add(u64{indices.size()} | (u64{looks.size()} << 32));
    if (whole.h == m_water_hash) {
        return;
    }
    m_water_hash = whole.h;
    // The bodies as they are now.
    std::vector<WaterBody> bodies;
    std::unordered_map<u32, usize> by_look;
    for (usize t = 0; t + 2 < indices.size(); t += 3) {
        const u32 a = indices[t], b = indices[t + 1], c = indices[t + 2];
        if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size()) {
            continue;
        }
        const WaterVertex* v[3] = {&vertices[a], &vertices[b], &vertices[c]};
        bool ok = true;
        for (const WaterVertex* p : v) {
            ok = ok && !p->swell && std::isfinite(p->position.x) && std::isfinite(p->position.y) &&
                 std::isfinite(p->position.z) && std::fabs(p->position.x) < 1e7 &&
                 std::fabs(p->position.y) < 1e7 && std::fabs(p->position.z) < 1e7;
        }
        if (!ok) {
            continue;
        }
        const u32 look = v[0]->look;
        auto [it, added] = by_look.try_emplace(look, bodies.size());
        if (added) {
            WaterBody body;
            Vec3 color = WaterLook{}.color;
            if (look < looks.size()) {
                const Vec3& tint = looks[look].color;
                if (std::isfinite(tint.x) && std::isfinite(tint.y) && std::isfinite(tint.z)) {
                    color = {std::clamp(tint.x, 0.0f, 1.0f), std::clamp(tint.y, 0.0f, 1.0f),
                             std::clamp(tint.z, 0.0f, 1.0f)};
                }
            }
            body.color = color;
            bodies.push_back(std::move(body));
        }
        WaterBody& body = bodies[it->second];
        for (const WaterVertex* p : v) {
            body.corners.push_back(p->position);
            body.box.min = lower(body.box.min, p->position);
            body.box.max = upper(body.box.max, p->position);
        }
    }
    for (WaterBody& body : bodies) {
        WordHash h;
        for (const DVec3& p : body.corners) {
            h.add(p.x);
            h.add(p.y);
            h.add(p.z);
        }
        h.add(body.color.x);
        h.add(body.color.y);
        h.add(body.color.z);
        body.hash = h.h;
    }
    const Writing writing(*this);
    // Bodies the same as before are kept (pieces and all); the others go
    // and come, their bricks built again.
    std::unordered_map<u64, usize> before;
    for (usize i = 0; i < m_water.size(); ++i) {
        before.emplace(m_water[i]->hash, i);
    }
    std::vector<u8> kept(m_water.size(), 0);
    std::vector<std::shared_ptr<const WaterBody>> now(bodies.size());
    for (usize i = 0; i < bodies.size(); ++i) {
        const auto it = before.find(bodies[i].hash);
        if (it != before.end() && kept[it->second] == 0 && m_water[it->second]->corners == bodies[i].corners) {
            kept[it->second] = 1;
            now[i] = m_water[it->second];
        }
    }
    for (usize i = 0; i < m_water.size(); ++i) {
        if (kept[i] == 0) {
            for (const WaterPiece& piece : m_water[i]->pieces) {
                queue_old(piece.box);
                touch_building(piece.box);
            }
        }
    }
    for (usize i = 0; i < bodies.size(); ++i) {
        if (now[i] != nullptr) {
            continue;
        }
        WaterBody& body = bodies[i];
        // File each triangle in every square its plan meets (row by row:
        // the squares under its span in each).
        std::unordered_map<u64, usize> square;
        const usize triangles = body.corners.size() / 3;
        for (usize t = 0; t < triangles; ++t) {
            const DVec3& a = body.corners[t * 3];
            const DVec3& b = body.corners[t * 3 + 1];
            const DVec3& c = body.corners[t * 3 + 2];
            const DVec3 lo = lower(lower(a, b), c);
            const DVec3 hi = upper(upper(a, b), c);
            const i64 z0 = static_cast<i64>(std::floor(lo.z / k_water_piece));
            const i64 z1 = static_cast<i64>(std::floor(hi.z / k_water_piece));
            for (i64 z = z0; z <= z1; ++z) {
                const double sz = static_cast<double>(z) * k_water_piece;
                double span0 = 0.0, span1 = 0.0;
                if (!plan_span_in_row(a, b, c, sz, sz + k_water_piece, span0, span1)) {
                    continue;
                }
                const i64 x0 = static_cast<i64>(std::floor(span0 / k_water_piece));
                const i64 x1 = static_cast<i64>(std::floor(span1 / k_water_piece));
                for (i64 x = x0; x <= x1; ++x) {
                    const double sx = static_cast<double>(x) * k_water_piece;
                    const u64 key = (static_cast<u64>(static_cast<u32>(x)) << 32) | static_cast<u32>(z);
                    auto [it, added] = square.try_emplace(key, body.pieces.size());
                    if (added) {
                        body.pieces.emplace_back();
                    }
                    WaterPiece& piece = body.pieces[it->second];
                    piece.triangles.push_back(static_cast<u32>(t));
                    // Its box within the square (as much of it as is there).
                    const DVec3 in_lo{std::max(lo.x, sx), lo.y, std::max(lo.z, sz)};
                    const DVec3 in_hi{std::min(hi.x, sx + k_water_piece), hi.y, std::min(hi.z, sz + k_water_piece)};
                    piece.box.min = lower(piece.box.min, in_lo);
                    piece.box.max = upper(piece.box.max, in_hi);
                }
            }
        }
        for (const WaterPiece& piece : body.pieces) {
            queue_fresh(piece.box);
            touch_building(piece.box);
        }
        now[i] = std::make_shared<const WaterBody>(std::move(body));
    }
    m_water = std::move(now);
    if (m_regions.size() > k_max_backlog) {
        request_rebuild(); // changing faster than they are worked through
    }
}

void VoxelScene::queue_changed(u32 index) {
    const Box& box = m_copies[index].box;
    // An empty box (nothing to voxelize) still waits, for holds().
    Region& r = region_for(box.empty() ? Box{DVec3{}, DVec3{}} : box);
    if (!r.changed.empty() && r.changed.back() == index) {
        return; // changing again, frame after frame, while it waits: once
    }
    r.changed.push_back(index); // (twice if it changes twice apart: harmless, counted twice)
    if (index >= m_changed.size()) {
        m_changed.resize(usize{index} + 1, 0);
    }
    m_changed_count += m_changed[index]++ == 0 ? 1u : 0u;
}

void VoxelScene::change(u32 index, const Instance& in, std::span<const Material> materials,
                        const MeshLookup& meshes) {
    if (index < m_copies.size()) {
        grid_erase(index);
        queue_old(m_copies[index].box); // its bricks there are built again
    } else {
        m_copies.resize(usize{index} + 1);
        m_copy_mesh.resize(m_copies.size(), k_no_mesh);
    }
    touch_building(m_copies[index].box); // where it was (empty if new)
    set_copy(index, in, materials, meshes);
    touch_building(m_copies[index].box);
    grid_insert(index);
    queue_changed(index);
}

void VoxelScene::copy_all(std::span<const Instance> instances, std::span<const Material> materials,
                          const MeshLookup& meshes) {
    m_copies.resize(instances.size());
    m_copy_mesh.resize(instances.size(), k_no_mesh);
    for (u32 i = 0; i < instances.size(); ++i) {
        set_copy(i, instances[i], materials, meshes);
    }
    grid_rebuild();
    // Find jobs under way keep going: they read the current boxes (and
    // skip indices past the end), and the full rebuild that always follows
    // covers whatever they miss. Restarting them on every bulk change
    // would starve them.
    for (const std::shared_ptr<Build>& b : m_builds) {
        b->again = b->again || !b->untouched(); // so are the bricks under way (that have read what was)
    }
}

void VoxelScene::remove_range(u32 first, u32 count) {
    for (u32 i = first; i < first + count; ++i) {
        queue_old(m_copies[i].box);
    }
    m_copies.erase(m_copies.begin() + first, m_copies.begin() + first + count);
    m_copy_mesh.erase(m_copy_mesh.begin() + first, m_copy_mesh.begin() + first + count);
    grid_rebuild();
    // Indices past the run shift down.
    m_changed.assign(m_copies.size(), 0);
    m_changed_count = 0;
    for (auto& [key, region] : m_regions) {
        std::vector<u32> kept;
        for (const u32 i : region.changed) {
            if (i >= first && i < first + count) {
                continue;
            }
            const u32 j = i < first ? i : i - count;
            kept.push_back(j);
            m_changed_count += m_changed[j]++ == 0 ? 1u : 0u;
        }
        region.changed = std::move(kept);
    }
    // Find lists under way drop the removed run and shift the rest down,
    // keeping their place (restarting them would starve them under
    // constant removals).
    const auto remap = [first, count](std::vector<u32>& found, u32& next, u64* cursor) {
        u32 out = 0;
        u32 new_next = 0;
        bool dropped_current = false;
        for (u32 k = 0; k < found.size(); ++k) {
            const u32 i = found[k];
            const bool gone = i >= first && i < first + count;
            if (k == next && gone) {
                dropped_current = true;
            }
            if (k < next && !gone) {
                ++new_next;
            }
            if (!gone) {
                found[out++] = i < first ? i : i - count;
            }
        }
        if (next >= found.size()) {
            new_next = out; // past the end (sweeping): stays there
        }
        found.resize(out);
        next = new_next;
        if (cursor != nullptr && dropped_current) {
            *cursor = 0; // the instance it was part-way through is gone
        }
    };
    for (Job& job : m_jobs) {
        if (job.kind == JobKind::find && job.started) {
            remap(job.found, job.next, &job.cursor);
        } else if (job.kind == JobKind::region) {
            // Its changed instances too (each job its own copy now).
            Region r = *job.region;
            const u32 olds = static_cast<u32>(r.fresh.size() + r.old.size()); // (fresh first, then old)
            if (job.next < olds) {
                u32 none = 0;
                remap(r.changed, none, nullptr);
            } else {
                u32 next = job.next - olds;
                remap(r.changed, next, &job.cursor);
                job.next = olds + next;
            }
            job.region = std::make_shared<const Region>(std::move(r));
        }
    }
    for (const std::shared_ptr<Build>& b : m_builds) {
        b->again = b->again || !b->untouched();
    }
    if (m_gathering != nullptr) {
        // The one still copying holds indices: shifted too (the instances it has yet to copy).
        remap(m_gathering->found, m_gathering->next, nullptr);
    }
}

void VoxelScene::see_changes(std::span<const Instance> instances, std::span<const Material> materials,
                             const std::vector<CellRange>* only, u64 meshes_version, const MeshLookup& meshes) {
    // Meshes added or removed: the cached ones that are gone (or came back)
    // and the instances using them.
    std::vector<u32> forced;
    const bool looks_changed = material_changes(materials);
    if (meshes_version != m_meshes_version) {
        m_meshes_version = meshes_version;
        std::unordered_set<MeshId> stale(m_forgotten.begin(), m_forgotten.end());
        m_forgotten.clear();
        // Those missing that came (the rest are as they were; removed ones were forgotten).
        for (auto it = m_missing.begin(); it != m_missing.end();) {
            const std::shared_ptr<const geometry::ClusterMesh> now = meshes ? meshes(*it) : nullptr;
            if (now != nullptr && !now->empty()) {
                stale.insert(*it);
                {
                    const std::lock_guard lock(m_mesh_mutex);
                    m_meshes.erase(*it);
                }
                it = m_missing.erase(it);
            } else {
                ++it;
            }
        }
        if (!stale.empty()) {
            // By mesh id (a flag each), not a set lookup per instance.
            MeshId top = 0;
            for (const MeshId id : stale) {
                top = std::max(top, id == k_no_mesh ? 0 : id);
            }
            std::vector<u8> is_stale(usize{top} + 1, 0);
            for (const MeshId id : stale) {
                if (id != k_no_mesh) {
                    is_stale[id] = 1;
                }
            }
            for (u32 i = 0; i < m_copy_mesh.size(); ++i) {
                const MeshId id = m_copy_mesh[i];
                if (id <= top && is_stale[id] != 0) {
                    forced.push_back(i);
                }
            }
        }
    }

    const u32 n = static_cast<u32>(instances.size());
    const u32 m = static_cast<u32>(m_copies.size());
    if (only != nullptr && n >= m) {
        // Only the cells listed can differ (instances past the copies are
        // new, and in them), and instances whose material row looks
        // different now (its texture arrived: the cell did not change).
        // The other paths compare every instance, which sees those too.
        std::vector<u32> changed = std::move(forced);
        if (looks_changed) {
            for (u32 i = 0; i < m; ++i) {
                const u32 material = m_copies[i].instance.material;
                if (material < m_material_changed.size() && m_material_changed[material] != 0) {
                    changed.push_back(i);
                }
            }
        }
        for (const CellRange& c : *only) {
            for (u32 i = c.first; i < c.first + c.count && i < n; ++i) {
                if (i >= m || differs(instances[i], materials, m_copies[i])) {
                    changed.push_back(i);
                }
            }
        }
        std::sort(changed.begin(), changed.end());
        changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
        if (changed.size() > std::max<usize>(k_bulk, n / 8)) {
            copy_all(instances, materials, meshes);
            request_rebuild();
            return;
        }
        for (const u32 i : changed) {
            change(i, instances[i], materials, meshes);
        }
        if (m_regions.size() > k_max_backlog) {
            request_rebuild();
        }
        return;
    }
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
    std::sort(changed.begin(), changed.end());
    changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
    if (changed.size() > std::max<usize>(k_bulk, n / 8)) {
        copy_all(instances, materials, meshes);
        request_rebuild();
        return;
    }
    for (const u32 i : changed) {
        change(i, instances[i], materials, meshes);
    }
    if (m_regions.size() > k_max_backlog) {
        request_rebuild(); // changing faster than they are worked through
    }
}

void VoxelScene::request_rebuild() {
    ++m_stats.rebuilds;
    // A full rebuild covers every change made before it starts.
    m_regions.clear();
    m_region_order.clear();
    std::fill(m_changed.begin(), m_changed.end(), 0u);
    m_changed_count = 0;
    if (m_rebuild_jobs > 0) {
        m_rebuild_again = true; // after the one under way (restarting it would never finish)
    } else {
        push_rebuild();
    }
}

void VoxelScene::push_rebuild() {
    m_rebuild_again = false;
    m_regions.clear();
    m_region_order.clear();
    std::fill(m_changed.begin(), m_changed.end(), 0u);
    m_changed_count = 0;
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
                    forget_stand_in(level, value);
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
                // The brick that had this slot is out of the window now (its slot is another's):
                // forgotten here, so that nothing walks the queues of bricks waiting to be built,
                // which hold tens of thousands at speed. They keep the key until it is popped.
                const voxel::BrickCoord left = m_slot_bricks[base + slot];
                if (!m_pending.empty() && left != voxel::BrickCoord{x, y, z} && !m_clipmap.contains(level, left)) {
                    m_pending.erase(Key{level, left});
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
        ++m_stats.marked;
        const u32 s = level * m_clipmap.slots_per_level() + m_clipmap.slot(level, brick);
        const bool built = m_slot_bricks[s] == brick && m_slots[s] != gpu::k_unbuilt;
        (built ? m_again : m_order[level]).push_back(key);
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
    if (job.kind == JobKind::region) {
        // Each old box's bricks in the pool, then every brick under each
        // changed instance where it is now (within the job's box).
        // (Fresh boxes first: every brick under them, as under a changed instance.)
        const Region& r = *job.region;
        if (count <= 8 && m_clipmap.brick_size(level) >= 2.0 * k_region) {
            // A coarse level (bricks at least as wide as a region's widest box) and the box around
            // the whole region (old, fresh and changed) a few bricks: each of them once, not once
            // per instance (the same few bricks thousands of times over). One only an old box
            // touched is built again even if not in the pool (it builds empty).
            for (u64 i = 0; i < count; ++i) {
                mark(level, brick_of(box, i));
            }
            return true;
        }
        const usize fresh = r.fresh.size();
        const usize total = fresh + r.old.size() + r.changed.size();
        while (job.next < total) {
            const bool old = job.next >= fresh && job.next < fresh + r.old.size();
            Box b;
            if (job.next < fresh) {
                b = r.fresh[job.next];
            } else if (old) {
                b = r.old[job.next - fresh];
            } else {
                const u32 i = r.changed[job.next - fresh - r.old.size()];
                b = i < m_copies.size() ? m_copies[i].box : Box{};
            }
            const voxel::BrickBox under =
                b.empty() ? voxel::BrickBox{} : voxel::intersect(m_clipmap.bricks_touching(level, b.min, b.max), box);
            const u64 bricks = under.count();
            while (job.cursor < bricks) {
                const voxel::BrickCoord brick = brick_of(under, job.cursor++);
                const u32 s = level * slots + m_clipmap.slot(level, brick);
                if (!old || (m_slots[s] < gpu::k_unbuilt && m_slot_bricks[s] == brick)) {
                    mark(level, brick);
                }
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
        return true;
    }
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
        // Water over the box (bricks under its bodies; once, as the sweep starts).
        if (job.next == job.found.size() && job.cursor == 0) {
            const Box around{m_clipmap.brick_min(level, box.min),
                             m_clipmap.brick_min(level, box.max)}; // (max: one past)
            for (const auto& body : m_water) {
                if (!overlaps(body->box.min, body->box.max, around.min, around.max)) {
                    continue;
                }
                for (const WaterPiece& piece : body->pieces) {
                    const voxel::BrickBox under =
                        voxel::intersect(m_clipmap.bricks_touching(level, piece.box.min, piece.box.max), box);
                    for (u64 k = 0; k < under.count(); ++k) {
                        mark(level, brick_of(under, k));
                    }
                }
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
        forget_stand_in(level, m_slots[s]);
        m_free.push_back(m_slots[s]);
        --m_used;
        m_loose[level] = true;
    }
    set_slot(level, slot, gpu::k_no_brick, brick);
}

void VoxelScene::forget_stand_in(u32 level, u32 index) {
    if (m_has_stand_in[index] != 0) {
        m_has_stand_in[index] = 0;
        --m_stand_in_bricks[level];
    }
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
    if (box.empty()) {
        return;
    }
    if (!m_touchable_ready) {
        // (Scatter streaming in changes thousands of instances a frame: each looks only at the
        // builds that have read the scene, a few of the hundreds waiting.)
        m_touchable.clear();
        for (const std::shared_ptr<Build>& b : m_builds) {
            if (!b->untouched()) {
                m_touchable.push_back(b.get());
            }
        }
        m_touchable_ready = true;
    }
    for (Build* const b : m_touchable) {
        const double size = b->brick_size;
        if (!b->again && overlaps(box.min, box.max, b->origin, b->origin + DVec3{size, size, size})) {
            b->again = true;
        }
    }
}

bool VoxelScene::Build::run() {
    u32 expected = k_queued;
    if (!state.compare_exchange_strong(expected, k_running, std::memory_order_acq_rel)) {
        return false; // another thread has it
    }
    build_voxels();
    state.store(k_built, std::memory_order_release);
    return true;
}

void VoxelScene::Build::build_voxels() {
    if (items.empty() && water.empty()) {
        meshes.clear();
        cuts.clear();
        return; // nothing over it
    }
    // (One a thread: 20 KB, and reset clears only what the last brick touched.)
    static thread_local voxel::BrickBuilder builder;
    builder.reset(voxel_size, seed);
    const float voxel = voxel_size;
    const float edge = voxel * static_cast<float>(voxel::k_brick_edge);
    for (const Item& item : items) {
        const geometry::ClusterMesh& mesh = *meshes[item.mesh];
        const Cut& chosen = *cuts[item.cut];
        const float s = max_abs_scale(item.scale);
        const Vec3 rel = to_camera_relative(item.position, origin); // the brick's frame
        const bool mirrored = item.scale.x * item.scale.y * item.scale.z < 0.0f;
        const auto place = [&](const Vec3& p) {
            return rel + rotate(item.rotation, Vec3{p.x * item.scale.x, p.y * item.scale.y, p.z * item.scale.z});
        };
        if (mesh.bounds.radius * s <= k_point_share * voxel) {
            // Far smaller than a voxel (a grass tuft or a tree in a coarse level's): one point where
            // it is, added by the brick its middle is in (a few dozen times less work than its
            // triangles, which would all fall in one voxel or two). Its facing as its triangles'
            // summed (a leafy crown's, bent outward on every side, sums to about none: +Y).
            // Which brick, from the world position, so that one on a brick's face is added by
            // exactly one of the two (in each brick's frame, floats can put it just outside both).
            const Vec3 offset = rotate(item.rotation, Vec3{mesh.bounds.center.x * item.scale.x, mesh.bounds.center.y * item.scale.y,
                                                           mesh.bounds.center.z * item.scale.z});
            const DVec3 middle = to_world(offset, item.position);
            if (std::floor(middle.x / brick_size) != static_cast<double>(brick.x) ||
                std::floor(middle.y / brick_size) != static_cast<double>(brick.y) ||
                std::floor(middle.z / brick_size) != static_cast<double>(brick.z)) {
                continue;
            }
            const Vec3 n = chosen.normals;
            const Vec3 normals = rotate(item.rotation, Vec3{std::copysign(1.0f, item.scale.x) * n.x, std::copysign(1.0f, item.scale.y) * n.y,
                                                            std::copysign(1.0f, item.scale.z) * n.z}) *
                                 (1.0f - std::clamp(item.canopy, 0.0f, 1.0f));
            const float inside = std::nextafter(edge, 0.0f);
            const Vec3 at = place(mesh.bounds.center);
            builder.add_point({std::clamp(at.x, 0.0f, inside), std::clamp(at.y, 0.0f, inside), std::clamp(at.z, 0.0f, inside)},
                              chosen.triangles, chosen.area * s * s, normals, item.color, item.stand_in, item.coverage);
            continue;
        }
        // Leafy crowns face out from the middle, as resolve.slang bends them.
        const Vec3 middle = place(mesh.bounds.center);
        for (const u32 ci : chosen.clusters) {
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
                Vec3 facing{};
                if (item.canopy > 0.0f) {
                    // As resolve.slang: the side seen from outside
                    // (a leaf card's either way), bent out.
                    Vec3 own = normalize_or_zero(cross(b - a, d - a));
                    const Vec3 out = normalize_or_zero((a + b + d) * (1.0f / 3.0f) - middle);
                    if (dot(own, out) < 0.0f) {
                        own = own * -1.0f;
                    }
                    // (Never cancels once own is on out's side; out
                    // zero, at the middle: add_triangle keeps own.)
                    facing = own + (out - own) * item.canopy;
                }
                builder.add_triangle(a, b, d, item.color, item.stand_in, item.coverage, facing);
            }
        }
    }
    // Water's surfaces over the brick, after everything else.
    for (const WaterTriangle& w : water) {
        builder.add_water_triangle(w.a, w.b, w.c, w.color);
    }
    // Empty also when cut-outs touched voxels but thinned every one away.
    if (!builder.empty()) {
        auto made = std::make_unique<const voxel::Brick>(builder.build());
        if (!made->empty()) {
            result = std::move(made);
        }
    }
    items.clear();
    items.shrink_to_fit();
    meshes.clear(); // they may go now
    cuts.clear();
    water.clear();
    water.shrink_to_fit();
}

bool VoxelScene::start_build(const std::function<bool()>& out_of_time, usize most) {
    // Every level's never built, coarsest first; then those to build again, oldest first.
    u32 started = 0;
    for (u32 n = m_clipmap.levels() + 1; n-- > 0;) {
        std::deque<Key>& order = n > 0 ? m_order[n - 1] : m_again;
        while (!order.empty()) {
            if (m_builds.size() >= most) {
                submit_batch();
                return started > 0; // the rest wait where they are
            }
            const Key key = order.front();
            order.pop_front();
            if (m_pending.erase(key) == 0 || !m_clipmap.contains(key.level, key.brick)) {
                continue; // left the window while waiting
            }
            if (const auto under_way = m_building.find(key); under_way != m_building.end()) {
                // Built once more when it is done (never two at once), unless it has yet to read
                // the scene: whatever marked it is there to be read.
                if (!under_way->second->untouched()) {
                    under_way->second->again = true;
                }
                continue;
            }
            auto job = std::make_shared<Build>();
            Build& b = *job;
            b.level = key.level;
            b.brick = key.brick;
            b.origin = m_clipmap.brick_min(key.level, key.brick);
            b.brick_size = m_clipmap.brick_size(key.level);
            b.box = {b.origin, b.origin + DVec3{b.brick_size, b.brick_size, b.brick_size}};
            b.voxel_size = static_cast<float>(m_clipmap.voxel_size(key.level));
            b.seed = static_cast<u32>(KeyHash{}(key));
            b.queued_at = m_updates;
            m_builds.push_back(job);
            m_building.emplace(key, job.get());
            ++started;
            if (!m_prepare_on_workers) {
                b.state.store(Build::k_for_main, std::memory_order_release);
                return true; // one a call: update begins it
            }
            b.state.store(Build::k_waiting, std::memory_order_release);
            m_batch.push_back(std::move(job));
            if (m_batch.size() >= k_batch) {
                submit_batch();
            }
            if (started % 64 == 0 && out_of_time()) {
                submit_batch();
                return true;
            }
        }
    }
    submit_batch();
    return started > 0;
}

JobSystem& VoxelScene::next_workers(u32 builds) {
    const u32 base = m_build_jobs.worker_count();
    const u32 all = base + (m_helping && m_more_jobs != nullptr ? m_more_jobs->worker_count() : 0u);
    const bool more = m_submitted++ % all >= base;
    m_more_handed += more ? builds : 0u;
    return more ? *m_more_jobs : m_build_jobs;
}

void VoxelScene::submit_batch() {
    if (m_batch.empty()) {
        return;
    }
    JobSystem& jobs = next_workers(static_cast<u32>(m_batch.size()));
    jobs.submit(m_build_group, [this, batch = std::move(m_batch)] {
        for (const std::shared_ptr<Build>& b : batch) {
            work(*b, false);
        }
    });
    m_batch.clear();
}

bool VoxelScene::work(Build& b, bool here) {
    u32 expected = Build::k_waiting;
    if (!b.state.compare_exchange_strong(expected, Build::k_running, std::memory_order_acq_rel)) {
        return false; // another thread has it, or it was dropped
    }
    static thread_local FindScratch scratch;
    if (scratch.scene != m_serial) {
        scratch.scene = m_serial;
        scratch.seen.clear();
        scratch.stamp = 0;
    }
    // (Room for every copy made ready before the scene is held: megabytes, the first time.)
    if (const usize copies = m_copy_count.load(std::memory_order_relaxed); scratch.seen.size() < copies) {
        scratch.seen.resize(copies, 0);
    }
    // (`here`: the main thread, helping or with the workers starved. A brick over more than one
    // share finds is not its to take whole in a frame: begun as before the workers took them,
    // a slice a frame.)
    const bool prepared = take(b, scratch, here) && prepare(b, scratch);
    if (prepared) {
        gather_water(b, scratch.water, scratch.triangles);
    }
    scratch.water.clear(); // (the bodies may go now)
    if (scratch.copies.capacity() > k_worker_items * 4) {
        // (A coarse brick's tens of thousands, megabytes: not kept by every thread for good.)
        std::vector<Copy>().swap(scratch.copies);
        std::vector<Copy>().swap(scratch.sorted);
        std::vector<u32>().swap(scratch.taken);
    }
    if (!prepared) {
        b.state.store(Build::k_for_main, std::memory_order_release);
        return true;
    }
    b.prepared = true;
    b.build_voxels();
    b.state.store(Build::k_built, std::memory_order_release);
    return true;
}

bool VoxelScene::take(const Build& b, FindScratch& scratch, bool one_share) {
    // A share of the scene, only to find what is over this one brick and copy it out; never taken
    // while the main thread wants the whole (it would wait on one brick after another), and never
    // kept for more than k_worker_items found: a brick over more (a coarse one over a forest) is
    // taken in as many shares, each let go before the next and each taking the walk up where the
    // last left it. Whatever changes over the brick meanwhile sets its `again` (touch_building),
    // as for any build that has read the scene, so a copy that moved between two shares is put
    // right by the build after.
    scratch.copies.clear();
    scratch.taken.clear();
    scratch.water.clear();
    usize place = k_from_start;
    u64 writes = 0;
    for (bool all = false; !all;) {
        for (;;) {
            m_writing.wait(true, std::memory_order_acquire);
            m_scene_lock.lock_shared();
            if (!m_writing.load(std::memory_order_acquire)) {
                break;
            }
            m_scene_lock.unlock_shared();
        }
        const bool first = place == k_from_start;
        if (!first && writes != m_writes) {
            // The scene was written between two shares: the lists may not be as they were, so
            // the walk starts over, passing those it has seen (they are not looked at twice).
            place = 0;
        }
        writes = m_writes;
        all = find_into(b.box, scratch.found, scratch.seen, scratch.stamp, k_worker_items, &place);
        if (!all && one_share) {
            m_scene_lock.unlock_shared();
            return false;
        }
        for (const u32 i : scratch.found) {
            scratch.copies.push_back(m_copies[i]); // (what a share cut short found is over the brick too)
            scratch.taken.push_back(i);
        }
        if (first) {
            for (const auto& body : m_water) {
                if (overlaps(body->box.min, body->box.max, b.box.min, b.box.max)) {
                    scratch.water.push_back(body);
                }
            }
        }
        m_scene_lock.unlock_shared();
    }
    // In the instances' order, however they were found: a brick's voxels depend a little on the
    // order its instances are added in (sums of floats), and it is built the same whether taken
    // in one share, in many, or on the main thread (begin_here).
    if (!std::is_sorted(scratch.taken.begin(), scratch.taken.end())) {
        std::vector<u32>& order = scratch.found;
        order.resize(scratch.taken.size());
        std::iota(order.begin(), order.end(), 0u);
        std::sort(order.begin(), order.end(), [&](u32 l, u32 r) { return scratch.taken[l] < scratch.taken[r]; });
        scratch.sorted.clear();
        scratch.sorted.reserve(order.size());
        for (const u32 at : order) {
            scratch.sorted.push_back(scratch.copies[at]);
        }
        scratch.copies.swap(scratch.sorted);
    }
    return true;
}

bool VoxelScene::prepare(Build& b, FindScratch& scratch) {
    std::vector<Copy>& found = scratch.copies;
    bool ruled = false;
    std::shared_ptr<MeshCache> hold; // (a mesh forgotten meanwhile: its entry stays while looked at)
    if (m_ruling_out && !found.empty()) {
        // Most bricks under an instance's bounds have none of its triangles near (the air over a
        // slope's ground and the earth under it): told from the mesh's grid, far cheaper than
        // building each to find it empty. (Runs of one mesh look it up once.)
        MeshId last = k_no_mesh;
        MeshCache* cache = nullptr;
        bool known = false;
        bool have = false;
        bool unknown = false;
        const double voxel = static_cast<double>(b.voxel_size);
        std::erase_if(found, [&](const Copy& copy) {
            if (!have || copy.instance.mesh != last) {
                cache = known_mesh(copy.instance.mesh, known, &hold);
                last = copy.instance.mesh;
                have = true;
            }
            if (!known) {
                if (!unknown || b.unknown.back() != last) {
                    b.unknown.push_back(last);
                }
                unknown = true;
                return false;
            }
            return cache == nullptr || !may_touch(copy, *cache, b.box, voxel); // (no mesh: a build skips it too)
        });
        if (unknown) {
            // Every mesh never looked up is told at once (each once): one trip to the main thread.
            std::sort(b.unknown.begin(), b.unknown.end());
            b.unknown.erase(std::unique(b.unknown.begin(), b.unknown.end()), b.unknown.end());
            return false;
        }
        ruled = found.empty();
    }
    b.items.reserve(found.size());
    GatherRun run;
    for (const Copy& copy : found) {
        // (False: its mesh was never looked up, or was forgotten since it was ruled in.)
        if (!gather_item(b, copy, run, nullptr) && (b.unknown.empty() || b.unknown.back() != copy.instance.mesh)) {
            b.unknown.push_back(copy.instance.mesh);
        }
    }
    if (!b.unknown.empty()) {
        b.items.clear();
        b.meshes.clear();
        b.cuts.clear();
        std::sort(b.unknown.begin(), b.unknown.end());
        b.unknown.erase(std::unique(b.unknown.begin(), b.unknown.end()), b.unknown.end());
        return false;
    }
    b.ruled = ruled;
    b.count = static_cast<u32>(b.items.size());
    return true;
}

void VoxelScene::begin_here(const std::shared_ptr<Build>& job, const MeshLookup& meshes) {
    Build& b = *job;
    if (!m_clipmap.contains(b.level, b.brick)) {
        b.state.store(Build::k_built, std::memory_order_release); // left the window: dropped as it is placed
        return;
    }
    ++m_begun_here;
    find(b.box);
    std::sort(m_found.begin(), m_found.end()); // (the order a worker adds them in: take)
    bool ruled = false;
    if (m_ruling_out && !m_found.empty()) {
        const double voxel = static_cast<double>(b.voxel_size);
        MeshId last = k_no_mesh;
        MeshCache* cache = nullptr;
        bool have = false;
        std::erase_if(m_found, [&](u32 i) {
            const Copy& copy = m_copies[i];
            if (!have || copy.instance.mesh != last) {
                cache = mesh_cache(copy.instance.mesh, meshes);
                last = copy.instance.mesh;
                have = true;
            }
            return cache == nullptr || !may_touch(copy, *cache, b.box, voxel);
        });
        ruled = m_found.empty();
    }
    b.ruled = ruled;
    if (m_found.empty() && !water_over(b.box)) {
        // Nothing over it: done, with nothing in it.
        b.count = 0;
        b.state.store(Build::k_built, std::memory_order_release);
        return;
    }
    b.found = m_found;
    b.next = 0;
    b.items.reserve(m_found.size());
    gather_water(b, m_water, m_water_triangles);
    b.state.store(Build::k_gathering, std::memory_order_release);
    m_gathering = job; // its instances copied next, a few hundred between looks at the clock
}

void VoxelScene::gather_water(Build& b, std::span<const std::shared_ptr<const WaterBody>> bodies,
                              std::vector<u32>& triangles) {
    // Water's surfaces over it: each triangle once, though it is filed in every piece it reaches
    // (the bodies as they were when the scene was read; a change meanwhile sets `again`).
    const DVec3 far_corner = b.box.max;
    for (const auto& body : bodies) {
        if (!overlaps(body->box.min, body->box.max, b.origin, far_corner)) {
            continue;
        }
        triangles.clear();
        for (const WaterPiece& piece : body->pieces) {
            if (overlaps(piece.box.min, piece.box.max, b.origin, far_corner)) {
                triangles.insert(triangles.end(), piece.triangles.begin(), piece.triangles.end());
            }
        }
        std::sort(triangles.begin(), triangles.end());
        triangles.erase(std::unique(triangles.begin(), triangles.end()), triangles.end());
        for (const u32 t : triangles) {
            const DVec3& p0 = body->corners[usize{t} * 3];
            const DVec3& p1 = body->corners[usize{t} * 3 + 1];
            const DVec3& p2 = body->corners[usize{t} * 3 + 2];
            if (overlaps(lower(lower(p0, p1), p2), upper(upper(p0, p1), p2), b.origin, far_corner)) {
                b.water.push_back({to_camera_relative(p0, b.origin), to_camera_relative(p1, b.origin),
                                   to_camera_relative(p2, b.origin), body->color});
            }
        }
    }
    b.water_count = static_cast<u32>(b.water.size());
}

bool VoxelScene::gather_item(Build& b, const Copy& copy, GatherRun& run, const MeshLookup* meshes) {
    const Instance& in = copy.instance;
    const float s = max_abs_scale(in.scale);
    if (!(s > 0.0f) || !std::isfinite(s)) {
        return true;
    }
    // Detail to half a voxel: the cut whose error is the power of
    // two at or under that (shared by instances of similar scale).
    const float wanted = 0.5f * b.voxel_size / s;
    const i32 exponent = static_cast<i32>(std::clamp(std::floor(std::log2(wanted)), -40.0f, 40.0f));
    const bool fresh = !run.have || in.mesh != run.last_mesh || exponent != run.last_exponent;
    bool known = true;
    std::shared_ptr<MeshCache> hold;
    const auto look_up = [&]() -> MeshCache* {
        return meshes != nullptr ? mesh_cache(in.mesh, *meshes) : known_mesh(in.mesh, known, &hold);
    };
    MeshCache* m = fresh ? look_up() : nullptr;
    if (!known) {
        return false;
    }
    if (fresh && m == nullptr) {
        return true;
    }
    // A point needs the cut's sums (Build::build_voxels), the rest only its clusters: kept apart,
    // so a fine cut of a dense mesh is not walked triangle by triangle for nothing.
    const geometry::ClusterMesh& mesh = fresh ? *m->mesh : *b.meshes.back();
    const bool point = mesh.bounds.radius * s <= k_point_share * b.voxel_size;
    if (fresh || point != run.last_point) {
        if (m == nullptr) {
            m = look_up();
            if (!known) {
                return false;
            }
            if (m == nullptr) {
                return true;
            }
        }
        b.meshes.push_back(m->mesh);
        b.cuts.push_back(cut_for(*m, exponent, point));
        run.have = true;
        run.last_mesh = in.mesh;
        run.last_exponent = exponent;
        run.last_point = point;
    }
    Build::Item item;
    item.position = in.position;
    item.rotation = in.rotation;
    item.scale = in.scale;
    item.color = copy.color;
    item.coverage = copy.coverage;
    item.canopy = copy.canopy;
    item.stand_in = copy.stand_in;
    item.mesh = static_cast<u32>(b.meshes.size() - 1);
    item.cut = static_cast<u32>(b.cuts.size() - 1);
    b.items.push_back(item);
    return true;
}

bool VoxelScene::gather(const MeshLookup& meshes, const std::function<bool()>& out_of_time) {
    Build& b = *m_gathering;
    GatherRun run; // (of this call: what it copied in an earlier one may have changed since)
    bool looked = false;
    while (b.next < b.found.size()) {
        if (b.next % 256 == 0 && looked && out_of_time()) {
            return false;
        }
        looked = true;
        const u32 i = b.found[b.next++];
        if (i < m_copies.size()) { // (else removed meanwhile: `again` is set)
            gather_item(b, m_copies[i], run, &meshes);
        }
    }
    b.found.clear();
    b.found.shrink_to_fit();
    b.count = static_cast<u32>(b.items.size());
    b.state.store(Build::k_queued, std::memory_order_release);
    const bool small = b.items.size() + b.water.size() <= k_inline_items;
    if (small && (m_inline_ms <= k_inline_ms || ++m_inline_skipped >= k_inline_probe)) {
        // A few instances (most bricks): built here at once, as cheap as handing it over, and
        // not left waiting on workers that may have no core to run on.
        m_inline_skipped = 0;
        const auto before = std::chrono::steady_clock::now();
        b.run();
        const double took = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before).count();
        m_inline_ms += (took - m_inline_ms) * 0.125;
    } else {
        b.queued_at = m_updates;
        next_workers(1).submit(m_build_group, [job = m_gathering] { job->run(); });
    }
    m_gathering.reset();
    return true;
}

bool VoxelScene::finish_build(rhi::CommandList& cmd, bool& pool_writable, const Build& build) {
    const bool ok = place_build(cmd, pool_writable, build);
    if (build.again) {
        // Built again with what changed: marked once placed, so a first build counts as built
        // (it waits its turn with the rest, not ahead of every brick never built).
        mark(build.level, build.brick);
    }
    return ok;
}

void VoxelScene::cancel_builds(bool all) {
    m_touchable_ready = false;
    std::erase_if(m_builds, [&](const std::shared_ptr<Build>& held) {
        Build& b = *held;
        if (!all && m_clipmap.contains(b.level, b.brick)) {
            return false;
        }
        // Those no thread has: waiting for one, or the main thread's own.
        u32 expected = Build::k_queued;
        bool dropped = b.state.compare_exchange_strong(expected, Build::k_cancelled, std::memory_order_acq_rel);
        expected = Build::k_waiting;
        dropped = dropped || b.state.compare_exchange_strong(expected, Build::k_cancelled, std::memory_order_acq_rel);
        dropped = dropped || expected == Build::k_for_main || held == m_gathering;
        if (!dropped) {
            return false; // under way or done: finished as usual
        }
        if (held == m_gathering) {
            m_gathering.reset();
        }
        if (!all && b.again) {
            mark(b.level, b.brick); // (no-op: it left the window)
        }
        m_building.erase(Key{b.level, b.brick});
        return true;
    });
}

bool VoxelScene::place_build(rhi::CommandList& cmd, bool& pool_writable, const Build& build) {
    const Key key{build.level, build.brick};
    const u32 level = key.level;
    if (!m_clipmap.contains(level, key.brick)) {
        return true; // left the window meanwhile
    }
    const u32 slot = m_clipmap.slot(level, key.brick);
    const u32 s = level * m_clipmap.slots_per_level() + slot;
    const bool here = m_slots[s] < gpu::k_unbuilt && m_slot_bricks[s] == key.brick;
    if (m_slots[s] < gpu::k_unbuilt && !here) {
        remove(level, slot, key.brick); // a brick that left the window (entering frees them; never kept)
    }
    if (build.result == nullptr) {
        remove(level, slot, key.brick);
        m_retry.erase(key);
        return true;
    }
    const voxel::Brick& brick = *build.result;
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
    forget_stand_in(level, index); // rebuilt in place: what it had
    bool stands = false;
    for (const u32 word : brick.stand_in) {
        stands = stands || word != 0;
    }
    if (stands) {
        m_has_stand_in[index] = 1;
        ++m_stand_in_bricks[level];
    }
    m_retry.erase(key);
    ++m_stats.uploaded;
    if (here) {
        m_rebuilt.push_back(index);
    } else {
        m_placed.push_back(index);
        m_owner[index] = s;
        m_placed_at[index] = ++m_placements;
    }
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
                        std::span<const CellRange> cells, std::span<const Material> materials, u64 meshes_version,
                        const MeshLookup& meshes, float budget_ms) {
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    m_stats.built = 0;
    m_stats.uploaded = 0;
    m_stats.marked = 0;
    m_stats.shift_ms = m_stats.changes_ms = m_stats.find_ms = m_stats.bricks_ms = 0.0f;
    m_stats.begun_here = 0;
    const u64 begun_before = m_begun_here;
    m_placed.clear();
    m_rebuilt.clear();
    if (!m_pool.valid()) {
        return;
    }
    const double budget = std::max(0.0, static_cast<double>(budget_ms));
    const auto elapsed = [&start] {
        return std::chrono::duration<double, std::milli>(clock::now() - start).count();
    };
    // Built bricks leave m_builds for the pool: those with something in them while there is time
    // and uploads left, those with nothing in them (most) always. `for_main`: the first one
    // found waiting for the main thread to begin it.
    bool pool_writable = false;
    std::shared_ptr<Build> for_main;
    const auto finish_ready = [&](bool empty_only, bool& stepped, bool& failed, bool& timed_out) {
        for_main.reset();
        const usize count = m_builds.size();
        usize keep = 0;
        usize i = 0;
        u32 emptied = 0;
        for (; i < count; ++i) {
            std::shared_ptr<Build>& held = m_builds[i];
            const u32 state = held->state.load(std::memory_order_acquire);
            const bool nothing = state == Build::k_built && held->result == nullptr;
            const bool placing = state == Build::k_built && (nothing || (!empty_only && m_stats.uploaded < k_max_uploads));
            // (The clock: before each with something in it, and every 64 with nothing.)
            if (placing && (nothing ? ++emptied % 64 == 0 : stepped) && elapsed() >= budget) {
                timed_out = true;
                break; // one a step, more while there is time
            }
            if (!placing) {
                if (state == Build::k_for_main && for_main == nullptr) {
                    for_main = held;
                }
                if (keep != i) {
                    m_builds[keep] = std::move(held);
                }
                ++keep;
                continue;
            }
            const std::shared_ptr<Build> done = std::move(held);
            m_building.erase(Key{done->level, done->brick});
            ++m_stats.built;
            m_ruled_out += done->ruled ? 1u : 0u;
            m_prepared_on_workers += done->prepared ? 1u : 0u;
            m_most_in_a_brick = std::max(m_most_in_a_brick, done->count);
            m_most_water_in_a_brick = std::max(m_most_water_in_a_brick, done->water_count);
            m_progress_at = done->prepared ? m_updates : m_progress_at;
            stepped = stepped || !nothing;
            if (!finish_build(cmd, pool_writable, *done)) {
                failed = true;
                ++i;
                break;
            }
        }
        for (; i < count; ++i, ++keep) {
            if (keep != i) {
                m_builds[keep] = std::move(m_builds[i]);
            }
        }
        m_builds.resize(keep);
        stepped = stepped || keep != count;
    };
    {
        // Those with nothing in them, before this frame's changes look at every build under way.
        bool stepped = false, failed = false, timed_out = false;
        finish_ready(true, stepped, failed, timed_out);
    }
    // (Where the time goes, for the stats: each part is what passed since the part before.)
    double part_from = 0.0;
    const auto part = [&](float& ms) {
        const double now = elapsed();
        ms += static_cast<float>(now - part_from);
        part_from = now;
    };
    part(m_stats.bricks_ms);

    // Coarse levels' slabs first (recenter lists them finest first).
    const std::vector<voxel::BrickBox> entered = m_clipmap.recenter(camera);
    if (!entered.empty()) {
        cancel_builds(false); // (after a teleport, the new window's bricks start at once)
    }
    for (usize i = entered.size(); i-- > 0;) {
        enter(entered[i]);
    }
    // The keys of bricks left behind are dropped as they are popped (start_build), and `enter`
    // has forgotten them already. A queue grown past twice the bricks waiting (and a level's
    // worth: 32,768 by default) is mostly keys of bricks gone, or of ones that left, came back
    // and were queued again: those are taken out, so that a long flight cannot grow a queue
    // without end. What is kept is at most the bricks waiting, so the walk is paid for by the
    // keys it removes (never every frame).
    const usize most_queued = usize{2} * m_pending.size() + m_clipmap.slots_per_level();
    const auto without_stale = [this](std::deque<Key>& queue) {
        std::deque<Key> kept;
        std::unordered_set<Key, KeyHash> seen;
        for (const Key& key : queue) {
            if (m_pending.count(key) != 0 && seen.insert(key).second) {
                kept.push_back(key);
            }
        }
        queue = std::move(kept);
    };
    for (std::deque<Key>& queue : m_order) {
        if (queue.size() > most_queued) {
            without_stale(queue);
        }
    }
    if (m_again.size() > most_queued) {
        without_stale(m_again);
    }
    part(m_stats.shift_ms);
    {
        const Writing writing(*this); // (the workers stand back while the copies change)
        const bool only = changed_cells(cells, instances.size(), m_seen_cells, m_changed_cells);
        see_changes(instances, materials, only ? &m_changed_cells : nullptr, meshes_version, meshes);
    }
    // Waiting changes become jobs while the queue is short.
    while (m_jobs.size() < k_max_jobs && !m_region_order.empty()) {
        const u64 key = m_region_order.front();
        m_region_order.pop_front();
        const auto it = m_regions.find(key);
        if (it == m_regions.end()) {
            continue;
        }
        auto region = std::make_shared<Region>(std::move(it->second));
        m_regions.erase(it);
        Box around; // the old and fresh boxes and where the changed instances are now
        for (const Box& b : region->fresh) {
            around.min = lower(around.min, b.min);
            around.max = upper(around.max, b.max);
        }
        for (const Box& b : region->old) {
            around.min = lower(around.min, b.min);
            around.max = upper(around.max, b.max);
        }
        for (const u32 i : region->changed) {
            if (i < m_changed.size() && m_changed[i] > 0 && --m_changed[i] == 0) {
                --m_changed_count;
            }
            if (i < m_copies.size() && !m_copies[i].box.empty()) {
                around.min = lower(around.min, m_copies[i].box.min);
                around.max = upper(around.max, m_copies[i].box.max);
            }
        }
        if (around.empty()) {
            continue;
        }
        const std::shared_ptr<const Region> shared = std::move(region);
        for (u32 level = m_clipmap.levels(); level-- > 0;) {
            const voxel::BrickBox bricks = m_clipmap.bricks_touching(level, around.min, around.max);
            if (bricks.count() > 0) {
                Job job;
                job.box = bricks;
                job.kind = JobKind::region;
                job.region = shared;
                m_jobs.push_back(std::move(job));
            }
        }
    }
    // Bricks that did not fit, as room frees up (dropping those nothing is
    // over any more, a few hundred looks a frame at most).
    if (!m_retry.empty() && !m_free.empty()) {
        usize room = m_free.size();
        u32 looks = 256;
        for (auto it = m_retry.begin(); it != m_retry.end() && room > 0 && looks > 0 && (looks == 256 || elapsed() < budget * 0.25);
             --looks) {
            const Key key = *it;
            if (!m_clipmap.contains(key.level, key.brick)) {
                it = m_retry.erase(it); // left the view
                continue;
            }
            const Box around{m_clipmap.brick_min(key.level, key.brick),
                             m_clipmap.brick_min(key.level, {key.brick.x + 1, key.brick.y + 1, key.brick.z + 1})};
            find(around);
            if (m_found.empty() && !water_over(around)) {
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

    part(m_stats.changes_ms);
    // Finding bricks takes up to half of what is left of the budget while
    // bricks wait to be built (at least one step a frame), then building
    // the rest.
    const double seen = elapsed();
    const double jobs_budget = m_pending.empty() ? budget : seen + std::max(0.0, budget - seen) * 0.5;
    const auto jobs_out_of_time = [&] { return elapsed() >= jobs_budget; };
    for (bool first = true; !m_jobs.empty() && (first || !jobs_out_of_time()); first = false) {
        if (!run_job(m_jobs.front(), jobs_out_of_time)) {
            break;
        }
        const Job done = std::move(m_jobs.front());
        m_jobs.pop_front();
        job_done(done);
    }
    part(m_stats.find_ms);
    // Bricks: those built copied to the pool, and more started (coarsest waiting first) while
    // fewer than the most are under way. With time to spare, the main thread builds some itself
    // and waits on the workers' rather than leave it unused.
    bool progress = false; // at least one step a frame
    const bool help = budget >= k_help_ms && m_main_thread_helps;
    m_helping = help;
    // (Prepared on the workers, a brick under way costs the main thread next to nothing. Prepared
    // here: more under way with more workers to build them, eight a worker.)
    const usize most_builds = m_prepare_on_workers ? k_most_under_way
                                                   : k_most_builds + (help && m_more_jobs != nullptr
                                                                          ? usize{8} * m_more_jobs->worker_count()
                                                                          : 0u);
    ++m_updates;
    bool rescued = false; // one build left waiting too long, built here this frame
    const std::function<bool()> out_of_time = [&] { return elapsed() >= budget; };
    for (;;) {
        if (progress && (elapsed() >= budget || m_stats.uploaded >= k_max_uploads)) {
            break;
        }
        bool stepped = false;
        bool failed = false;
        bool timed_out = false;
        finish_ready(false, stepped, failed, timed_out);
        if (failed) {
            break;
        }
        if (timed_out) {
            progress = true;
            continue; // (out of time: ends at the top)
        }
        if (m_gathering != nullptr) {
            gather(meshes, out_of_time);
            stepped = true;
        } else if (for_main != nullptr && !for_main->unknown.empty() && for_main->sent_back < k_most_sent_back) {
            // One a worker left for a mesh never looked up, which only the main thread may do:
            // looked up (a pass over its clusters), and the brick goes back to the workers.
            Build& b = *for_main;
            for (const MeshId id : b.unknown) {
                mesh_cache(id, meshes);
            }
            b.unknown.clear();
            b.again = false; // (it reads the scene afresh)
            ++b.sent_back;
            ++m_sent_back;
            b.queued_at = m_updates;
            b.state.store(Build::k_waiting, std::memory_order_release);
            m_batch.push_back(for_main);
            if (m_batch.size() >= k_batch) {
                submit_batch();
            }
            stepped = true;
        } else if (for_main != nullptr) {
            // The main thread's own (set_preparing_on_workers(false)), one sent back to the
            // workers more than once, or one it met while helping or with the workers starved
            // (work, `here`): found and copied here.
            for_main->unknown.clear();
            begin_here(for_main, meshes);
            if (m_gathering != nullptr) {
                gather(meshes, out_of_time); // (most bricks are over a few instances: done at once)
            }
            stepped = true;
        } else if (start_build(out_of_time, most_builds)) {
            stepped = true;
        } else if (help && !m_builds.empty()) {
            // None more to start: build one here (the smallest copied, else the last handed to
            // the workers, and only in the first half of the budget, as a whole brick is not
            // split), or wait for the workers'.
            Build* smallest = nullptr;
            Build* waiting = nullptr;
            if (elapsed() < budget * 0.5) {
                for (const std::shared_ptr<Build>& b : m_builds) {
                    const u32 state = b->state.load(std::memory_order_acquire);
                    if (state == Build::k_queued && (smallest == nullptr || b->count < smallest->count)) {
                        smallest = b.get();
                    } else if (state == Build::k_waiting) {
                        waiting = b.get();
                    }
                }
            }
            const bool built = smallest != nullptr ? smallest->run() : waiting != nullptr && work(*waiting, true);
            if (!built) {
                std::this_thread::yield();
            }
            stepped = true;
        }
        if (!stepped && !rescued && m_main_thread_helps) {
            for (const std::shared_ptr<Build>& b : m_builds) {
                const u32 state = b->state.load(std::memory_order_acquire);
                // (One waiting to be prepared: only when the workers have finished none for as
                // long, with no core to run on. Else it is behind others, and its turn comes.)
                if ((state == Build::k_queued ||
                     (state == Build::k_waiting && m_updates - m_progress_at > k_most_waiting_updates)) &&
                    m_updates - b->queued_at > k_most_waiting_updates) {
                    rescued = true;
                    stepped = state == Build::k_queued ? b->run() : work(*b, true);
                    break;
                }
            }
        }
        if (!stepped) {
            break; // nothing waits, or all under way
        }
        progress = true;
    }
    submit_batch(); // (those sent back to the workers)
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

    // Each level's queued jobs and a box around them, for holds() (most
    // instances miss the box, so they cost nothing per job).
    m_job_bounds.assign(m_clipmap.levels(), voxel::BrickBox{});
    m_job_boxes.resize(m_clipmap.levels());
    for (std::vector<voxel::BrickBox>& boxes : m_job_boxes) {
        boxes.clear();
    }
    std::vector<bool> any(m_clipmap.levels(), false);
    for (const Job& job : m_jobs) {
        const u32 l = job.box.level;
        m_job_boxes[l].push_back(job.box);
        voxel::BrickBox& b = m_job_bounds[l];
        b.level = l;
        if (!any[l]) {
            b.min = job.box.min;
            b.max = job.box.max;
            any[l] = true;
        } else {
            b.min = {std::min(b.min.x, job.box.min.x), std::min(b.min.y, job.box.min.y), std::min(b.min.z, job.box.min.z)};
            b.max = {std::max(b.max.x, job.box.max.x), std::max(b.max.y, job.box.max.y), std::max(b.max.z, job.box.max.z)};
        }
    }

    m_stats.bricks = m_used;
    m_stats.pending = static_cast<u32>(m_pending.size() + m_jobs.size() + m_regions.size() + m_changed_count +
                                       (m_rebuild_again ? 1 : 0) + m_builds.size());
    m_stats.waiting = static_cast<u32>(m_pending.size());
    m_stats.building = static_cast<u32>(m_builds.size());
    m_stats.changes = static_cast<u32>(m_jobs.size() + m_regions.size() + m_changed_count + (m_rebuild_again ? 1 : 0));
    m_stats.missing = static_cast<u32>(m_retry.size());
    m_stats.overflow = m_stats.missing > 0;
    m_stats.begun_here = static_cast<u32>(m_begun_here - begun_before);
    part(m_stats.bricks_ms);
    m_stats.cpu_ms = static_cast<float>(elapsed());
}

void VoxelScene::idle() {
    if (m_gathering != nullptr) {
        // Off for now: what it was copying (and the meshes it holds) let go; built when back on.
        const std::shared_ptr<Build> b = m_gathering;
        m_gathering.reset();
        m_builds.erase(std::find(m_builds.begin(), m_builds.end(), b));
        m_building.erase(Key{b->level, b->brick});
        mark(b->level, b->brick);
    }
    m_placed.clear();
    m_rebuilt.clear();
    m_stats.built = 0;
    m_stats.uploaded = 0;
    m_stats.marked = 0;
    m_stats.cpu_ms = 0.0f;
    m_stats.shift_ms = m_stats.changes_ms = m_stats.find_ms = m_stats.bricks_ms = 0.0f;
    m_stats.begun_here = 0;
}

usize VoxelScene::longest_queue() const {
    usize longest = m_again.size();
    for (const std::deque<Key>& queue : m_order) {
        longest = std::max(longest, queue.size());
    }
    return longest;
}

// --- Queries --------------------------------------------------------------

bool VoxelScene::pool_brick(u32 index, u32& level, voxel::BrickCoord& brick) const {
    if (index >= m_owner.size()) {
        return false;
    }
    const u32 s = m_owner[index];
    if (s >= m_slots.size() || m_slots[s] != index) {
        return false; // freed since (its slot holds something else)
    }
    level = s / m_clipmap.slots_per_level();
    brick = m_slot_bricks[s];
    return true;
}

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

bool VoxelScene::holds(u32 index) const {
    if (!m_clipmap.placed() || index >= m_copies.size() || m_rebuild_again ||
        (index < m_changed.size() && m_changed[index] != 0)) {
        return false;
    }
    if (!m_copies[index].stand_in) {
        return false;
    }
    const Box& b = m_copies[index].box;
    const u32 top = m_clipmap.levels() - 1;
    if (b.empty() || !m_clipmap.contains(top, m_clipmap.brick_at(top, b.min)) ||
        !m_clipmap.contains(top, m_clipmap.brick_at(top, b.max))) {
        return false;
    }
    constexpr u64 k_most_bricks = 512; // more under one instance on a level: not worth checking
    for (u32 level = 0; level <= top; ++level) {
        const voxel::BrickBox box = m_clipmap.bricks_touching(level, b.min, b.max);
        if (box.count() == 0) {
            continue;
        }
        if (box.count() > k_most_bricks) {
            return false;
        }
        if (level < m_job_bounds.size() && voxel::intersect(m_job_bounds[level], box).count() > 0) {
            for (const voxel::BrickBox& job : m_job_boxes[level]) {
                if (voxel::intersect(job, box).count() > 0) {
                    return false; // bricks there still to be found or marked
                }
            }
        }
        for (i32 z = box.min.z; z < box.max.z; ++z) {
            for (i32 y = box.min.y; y < box.max.y; ++y) {
                for (i32 x = box.min.x; x < box.max.x; ++x) {
                    const voxel::BrickCoord brick{x, y, z};
                    const Key key{level, brick};
                    const u32 s = level * m_clipmap.slots_per_level() + m_clipmap.slot(level, brick);
                    const u32 value = m_slots[s];
                    if (value == gpu::k_unbuilt || (value != gpu::k_no_brick && !(m_slot_bricks[s] == brick)) ||
                        (!m_pending.empty() && m_pending.count(key) != 0) ||
                        (!m_retry.empty() && m_retry.count(key) != 0) ||
                        building(key)) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
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
        // Whether the far field (stand-in voxels only) need look at the
        // level: it has stand-in bricks, or unbuilt slots a coarser level
        // with some stands in for.
        bool stands = false;
        for (u32 up = level; up < m_clipmap.levels() && !stands; ++up) {
            stands = m_stand_in_bricks[up] > 0;
            if (up == level && !stands && m_unbuilt[level] == 0) {
                break;
            }
        }
        l.brick[3] = stands ? 1 : 0;
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
    // The top of everything from each level up (coarser levels are written
    // above, so walked from the coarsest down).
    float top = -1.0e30f;
    for (u32 level = m_clipmap.levels(); level-- > 0;) {
        gpu::VoxelLevel& l = out[level];
        if (l.filled_min[1] <= l.filled_max[1]) {
            top = std::max(top, l.filled_max[1]);
        }
        l.filled_max[3] = top;
    }
}

} // namespace giant::render
