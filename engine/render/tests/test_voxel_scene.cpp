// The voxel world on its own (src/voxel_scene.h), on the null device: LOD
// cuts, which bricks get built, rebuilt and dropped, budgets and the pool.
#include "giant/geometry/clusters.h"
#include "giant/geometry/mesh.h"
#include "giant/render/renderer.h"
#include "giant/rhi/rhi.h"
#include "giant_test.h"
#include "gpu_layout.h"
#include "voxel_scene.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

using namespace giant;
using namespace giant::render;

namespace {

float area_of(const geometry::ClusterMesh& m, const std::vector<u32>& clusters) {
    double total = 0.0;
    for (const u32 ci : clusters) {
        const geometry::Cluster& c = m.clusters[ci];
        for (u32 t = 0; t < c.triangle_count; ++t) {
            const u32* i = &m.indices[c.first_index + t * 3];
            const Vec3 a = m.vertices[i[0]].position;
            total += 0.5 * length(cross(m.vertices[i[1]].position - a, m.vertices[i[2]].position - a));
        }
    }
    return static_cast<float>(total);
}

u32 triangles_of(const geometry::ClusterMesh& m, const std::vector<u32>& clusters) {
    u32 n = 0;
    for (const u32 ci : clusters) n += m.clusters[ci].triangle_count;
    return n;
}

struct Bench {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<VoxelScene> voxels;
    geometry::ClusterMesh cube = geometry::build_clusters(geometry::make_cube());
    std::vector<Instance> instances;
    std::vector<Material> materials;
    u64 version = 1;
    DVec3 camera{0.0, 1.0, 0.0};

    explicit Bench(u32 capacity = 1024) {
        rhi::DeviceDesc desc;
        desc.backend = rhi::Backend::null;
        device = rhi::create_device(desc);
        voxel::ClipmapDesc clip;
        clip.levels = 2;
        clip.voxel_size = 0.25f; // bricks of 2 m and 4 m
        clip.bricks_per_axis = 8;
        voxels = std::make_unique<VoxelScene>(*device, clip, capacity);
    }
    std::vector<CellRange> cells; // empty: none handed over
    void step(float budget_ms = 1000.0f) {
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        voxels->update(*cmd, camera, instances, cells, materials, version,
                       [this](MeshId id) { return id == 0 ? &cube : nullptr; }, budget_ms);
        device->end_frame();
    }
    // A voxel of the brick holding world point `p` at level 0, read back
    // from the GPU copy: occupied, and its packed value.
    bool voxel_at(const DVec3& p, u32& packed) {
        const voxel::Clipmap& clip = voxels->clipmap();
        const voxel::BrickCoord b = clip.brick_at(0, p);
        const u32 index = voxels->brick_index(0, b);
        if (index == gpu::k_no_brick) return false;
        device->wait_idle();
        const std::vector<u8> pool = device->read_buffer(voxels->pool());
        const std::vector<u8> table = device->read_buffer(voxels->table());
        u32 slot_value = 0;
        std::memcpy(&slot_value, table.data() + clip.slot(0, b) * 4, 4);
        if (slot_value != index) return false; // the GPU table agrees
        voxel::Brick brick;
        std::memcpy(&brick, pool.data() + u64{index} * sizeof(voxel::Brick), sizeof(brick));
        const DVec3 lo = clip.brick_min(0, b);
        const double vs = clip.voxel_size(0);
        const u32 x = static_cast<u32>((p.x - lo.x) / vs);
        const u32 y = static_cast<u32>((p.y - lo.y) / vs);
        const u32 z = static_cast<u32>((p.z - lo.z) / vs);
        const u32 i = voxel::voxel_index(x, y, z);
        packed = brick.voxels[i];
        stand_in = brick.stands_in(i);
        return brick.occupied(i);
    }
    bool stand_in = false; // the last voxel_at's stand-in bit
};

} // namespace

GIANT_TEST("voxel scene: LOD cuts cover the whole surface at every error") {
    const geometry::ClusterMesh sphere = geometry::build_clusters(geometry::make_sphere(128, 64));
    const std::vector<u32> finest = VoxelScene::cut(sphere, 0.0f);
    GIANT_CHECK(triangles_of(sphere, finest) == sphere.source_triangles);
    const float area = area_of(sphere, finest);
    u32 previous = triangles_of(sphere, finest);
    bool covers = true;
    bool fewer = true;
    for (float e = 1e-4f; e < 10.0f; e *= 2.0f) {
        const std::vector<u32> c = VoxelScene::cut(sphere, e);
        covers = covers && !c.empty() && std::fabs(area_of(sphere, c) - area) < area * 0.1f;
        const u32 t = triangles_of(sphere, c);
        fewer = fewer && t <= previous;
        previous = t;
    }
    GIANT_CHECK(covers);
    GIANT_CHECK(fewer);
    GIANT_CHECK(previous < sphere.source_triangles / 8); // coarse cuts really are coarse
}

GIANT_TEST("voxel scene: a cube becomes a shell of coloured voxels facing out") {
    Bench b;
    GIANT_CHECK(b.voxels->init());
    Instance cube;
    cube.mesh = 0;
    cube.position = {0.3, 1.1, 0.2}; // off the voxel grid
    cube.scale = {2.0f, 2.0f, 2.0f};
    b.instances = {cube};
    b.materials = {Material{{0.8f, 0.1f, 0.1f}}};
    b.step();
    const VoxelStats& s = b.voxels->stats();
    GIANT_CHECK(s.pending == 0 && s.bricks > 0 && s.built > 0 && !s.overflow);
    u32 packed = 0;
    // The +X face is at x = 1.3: occupied, facing +X, red.
    GIANT_CHECK(b.voxel_at({1.3, 1.0, 0.0}, packed));
    GIANT_CHECK(dot(voxel::unpack_normal(packed), {1, 0, 0}) > 0.9f);
    const Vec3 c = voxel::unpack_color(packed);
    GIANT_CHECK(c.x > 0.6f && c.y < 0.2f && c.z < 0.2f);
    GIANT_CHECK(b.voxel_at({0.3, 2.1, 0.2}, packed)); // the top, facing up
    GIANT_CHECK(dot(voxel::unpack_normal(packed), {0, 1, 0}) > 0.9f);
    // Inside and outside: empty.
    GIANT_CHECK(!b.voxel_at({0.3, 1.1, 0.2}, packed));
    GIANT_CHECK(!b.voxel_at({2.0, 1.0, 0.0}, packed));
    // Nothing changes: nothing is built again.
    b.step();
    GIANT_CHECK(b.voxels->stats().built == 0 && b.voxels->stats().uploaded == 0);
}

GIANT_TEST("voxel scene: moving, recolouring and removing rebuild what they touch") {
    Bench b;
    GIANT_CHECK(b.voxels->init());
    Instance cube;
    cube.mesh = 0;
    cube.position = {0.3, 1.1, 0.2};
    b.instances = {cube, cube};
    b.instances[1].position = {-3.0, 1.1, -3.0};
    b.step();
    const u32 bricks = b.voxels->stats().bricks;
    GIANT_CHECK(bricks > 0);
    u32 packed = 0;
    GIANT_CHECK(b.voxel_at({0.8, 1.0, 0.2}, packed)); // the first cube's +X face (unit cube)
    // Move the first: its old bricks empty, new ones fill; the second is untouched.
    b.instances[0].position = {3.3, 1.1, 0.2};
    b.step();
    GIANT_CHECK(b.voxels->stats().pending == 0);
    GIANT_CHECK(!b.voxel_at({0.8, 1.0, 0.2}, packed));
    GIANT_CHECK(b.voxel_at({3.8, 1.0, 0.2}, packed));
    GIANT_CHECK(b.voxel_at({-2.5, 1.0, -3.0}, packed));
    // A new material colour rebuilds what uses it.
    b.materials = {Material{{0.1f, 0.1f, 0.9f}}};
    b.step();
    GIANT_CHECK(b.voxel_at({3.8, 1.0, 0.2}, packed));
    GIANT_CHECK(voxel::unpack_color(packed).z > 0.6f);
    // Removing every instance empties the pool.
    b.instances.clear();
    b.step();
    GIANT_CHECK(b.voxels->stats().bricks == 0);
    GIANT_CHECK(!b.voxel_at({3.8, 1.0, 0.2}, packed));
}

GIANT_TEST("voxel scene: a texture's average colour and a cut-out's coverage reach the voxels") {
    Bench b;
    GIANT_CHECK(b.voxels->init());
    Instance cube;
    cube.mesh = 0;
    cube.position = {0.3, 1.1, 0.2};
    cube.scale = {2.0f, 2.0f, 2.0f};
    b.instances = {cube};
    // In a cell that never changes: the material rows are compared anyway.
    b.cells = {CellRange{.first = 0, .count = 1, .version = 7}};
    Material m{{1.0f, 1.0f, 1.0f}};
    b.materials = {m};
    b.step();
    u32 packed = 0;
    GIANT_CHECK(b.voxel_at({1.3, 1.0, 0.0}, packed));
    GIANT_CHECK(voxel::unpack_color(packed).z > 0.8f); // white
    // Its texture arrives: the voxels take its average (green here).
    m.base_color_texture = 3;
    m.base_color_average = {0.1f, 0.7f, 0.1f};
    b.materials = {m};
    b.step();
    GIANT_CHECK(b.voxels->stats().built > 0);
    GIANT_CHECK(b.voxel_at({1.3, 1.0, 0.0}, packed));
    Vec3 c = voxel::unpack_color(packed);
    GIANT_CHECK(c.y > 0.5f && c.x < 0.2f && c.z < 0.2f);
    // Coverage alone does nothing without a cutoff...
    m.coverage = 0.0f;
    b.materials = {m};
    b.step();
    GIANT_CHECK(b.voxels->stats().built == 0);
    GIANT_CHECK(b.voxel_at({1.3, 1.0, 0.0}, packed));
    // ...with one, a surface its texture cuts wholly away leaves no voxels,
    m.alpha_cutoff = 0.5f;
    b.materials = {m};
    b.step();
    GIANT_CHECK(b.voxels->stats().pending == 0);
    GIANT_CHECK(b.voxels->stats().bricks == 0);
    GIANT_CHECK(!b.voxel_at({1.3, 1.0, 0.0}, packed));
    // Partly covered: some voxels; barely covered, the dither leaves none,
    // and a brick thinned to nothing takes no room in the pool.
    m.coverage = 0.5f;
    b.materials = {m};
    b.step();
    GIANT_CHECK(b.voxels->stats().pending == 0 && b.voxels->stats().bricks > 0);
    m.coverage = 1.0e-7f;
    b.materials = {m};
    b.step();
    GIANT_CHECK(b.voxels->stats().pending == 0);
    GIANT_CHECK(b.voxels->stats().bricks == 0);
    // ...and without the texture (gone, or not there yet) it is solid.
    m.base_color_texture = k_no_texture;
    m.base_color_average = {1.0f, 1.0f, 1.0f};
    m.coverage = 1.0f;
    b.materials = {m};
    b.step();
    GIANT_CHECK(b.voxel_at({1.3, 1.0, 0.0}, packed));
    // A NaN coverage counts as solid too.
    m.base_color_texture = 3;
    m.coverage = std::nanf("");
    b.materials = {m};
    b.step();
    GIANT_CHECK(b.voxel_at({1.3, 1.0, 0.0}, packed));
    // Nothing changes: nothing is built again (NaN is no change).
    b.step();
    GIANT_CHECK(b.voxels->stats().built == 0);
    m.base_color_average = {std::nanf(""), 0.5f, 0.5f};
    b.materials = {m};
    b.step();
    b.step();
    GIANT_CHECK(b.voxels->stats().built == 0);
}

GIANT_TEST("voxel scene: a canopy material's voxels face out from the instance's middle") {
    Bench b;
    GIANT_CHECK(b.voxels->init());
    Instance cube;
    cube.mesh = 0;
    cube.position = {0.3, 1.1, 0.2};
    cube.scale = {2.0f, 2.0f, 2.0f};
    b.instances = {cube};
    b.cells = {CellRange{.first = 0, .count = 1, .version = 3}};
    Material m{{0.3f, 0.6f, 0.2f}};
    b.materials = {m};
    b.step();
    // Near the top of the +X face: as modelled it faces +X; as a crown,
    // out from the middle, so up as well.
    const DVec3 high{1.3, 1.95, 0.2};
    u32 packed = 0;
    GIANT_CHECK(b.voxel_at(high, packed));
    GIANT_CHECK(voxel::unpack_normal(packed).y < 0.1f);
    m.canopy = 1.0f;
    b.materials = {m};
    b.step(); // a changed row, in an unchanged cell
    GIANT_CHECK(b.voxels->stats().built > 0);
    GIANT_CHECK(b.voxel_at(high, packed));
    // Out from the cube's middle through each triangle's centroid (a leaf
    // card's is its place; the cube's two big triangles per face lean the
    // voxel towards their own): turned up, off the face's own +x.
    const Vec3 full = voxel::unpack_normal(packed);
    GIANT_CHECK(full.x > 0.7f && full.y > 0.2f);
    // Half bent: between its own facing (+x) and the full bend.
    m.canopy = 0.5f;
    b.materials = {m};
    b.step();
    GIANT_CHECK(b.voxel_at(high, packed));
    const Vec3 half = voxel::unpack_normal(packed);
    GIANT_CHECK(half.x > full.x && half.y > 0.05f && half.y < full.y);
    // Mirrored (its triangles wound the other way, its middle placed by
    // the negative scale too): the same.
    b.instances[0].scale = {-2.0f, 2.0f, 2.0f};
    b.step();
    GIANT_CHECK(b.voxel_at(high, packed));
    GIANT_CHECK(dot(voxel::unpack_normal(packed), half) > 0.97f);
    m.canopy = 1.0f;
    b.materials = {m};
    b.step();
    // Out of range is kept to 0 to 1 (as the renderer reads it): no rebuild.
    m.canopy = 4.0f;
    b.materials = {m};
    b.step();
    GIANT_CHECK(b.voxels->stats().built == 0);
}

GIANT_TEST("voxel scene: building keeps to its budget and coarse levels go first") {
    Bench b;
    GIANT_CHECK(b.voxels->init());
    for (int i = 0; i < 6; ++i) {
        Instance cube;
        cube.mesh = 0;
        cube.position = {static_cast<double>(i) * 2.5 - 6.0, 1.1, 0.0};
        b.instances.push_back(cube);
    }
    // A zero budget still makes progress: one step of finding bricks and
    // one brick a frame, coarse level first; the fine level reads
    // "building" until its bricks are done (the coarse one stands in).
    const DVec3 face{-5.5, 1.0, 0.0};
    b.step(0.0f);
    GIANT_CHECK(b.voxels->stats().built == 1);
    GIANT_CHECK(b.voxels->stats().pending > 1);
    GIANT_CHECK(b.voxels->probe(0, face) == VoxelProbe::building);
    const voxel::BrickCoord coarse = b.voxels->clipmap().brick_at(1, {-6.0, 1.1, 0.0});
    const voxel::BrickCoord fine = b.voxels->clipmap().brick_at(0, {-6.0, 1.1, 0.0});
    GIANT_CHECK(b.voxels->brick_index(0, fine) == gpu::k_no_brick);
    bool at_most_one = true;
    int frames = 0;
    while (b.voxels->stats().pending > 0 && frames < 2000) {
        b.step(0.0f);
        at_most_one = at_most_one && b.voxels->stats().built <= 1;
        ++frames;
    }
    GIANT_CHECK(at_most_one);
    GIANT_CHECK(b.voxels->stats().pending == 0);
    GIANT_CHECK(b.voxels->brick_index(0, fine) != gpu::k_no_brick);
    GIANT_CHECK(b.voxels->brick_index(1, coarse) != gpu::k_no_brick);
    GIANT_CHECK(b.voxels->probe(0, face) == VoxelProbe::solid);
    GIANT_CHECK(b.voxels->probe(0, {-6.0, 1.1, 0.0}) == VoxelProbe::empty); // inside the cube
    GIANT_CHECK(b.voxels->probe(0, {500.0, 1.0, 0.0}) == VoxelProbe::outside);
}

GIANT_TEST("voxel scene: a full pool says so, keeps what it has and fills as room frees up") {
    Bench b(3);
    GIANT_CHECK(b.voxels->init());
    Instance ground;
    ground.mesh = 0;
    ground.scale = {40.0f, 0.1f, 40.0f};
    b.instances = {ground};
    b.step();
    GIANT_CHECK(b.voxels->stats().overflow && b.voxels->stats().missing > 0);
    GIANT_CHECK(b.voxels->stats().bricks == 3 && b.voxels->stats().capacity == 3);
    b.step();
    GIANT_CHECK(b.voxels->stats().overflow); // lasts while bricks are missing
    // The ground goes and a small cube comes (one brick on each level):
    // room for it, nothing missing.
    Instance cube;
    cube.mesh = 0;
    cube.position = {1.0, 1.1, 1.0};
    b.instances = {cube};
    for (int i = 0; i < 100 && (b.voxels->stats().pending > 0 || b.voxels->stats().missing > 0); ++i) b.step();
    GIANT_CHECK(b.voxels->stats().pending == 0);
    GIANT_CHECK(!b.voxels->stats().overflow && b.voxels->stats().missing == 0);
    u32 packed = 0;
    GIANT_CHECK(b.voxel_at({1.5, 1.0, 1.0}, packed));
    GIANT_CHECK(b.voxels->stats().bricks == 2);
}

GIANT_TEST("voxel scene: instances moving every frame never stall the rest") {
    Bench b;
    GIANT_CHECK(b.voxels->init());
    Instance cube;
    cube.mesh = 0;
    cube.position = {0.3, 1.1, 0.2};
    b.instances = {cube, cube};
    b.instances[1].position = {-3.0, 1.1, -3.0};
    b.step();
    for (int i = 0; i < 60; ++i) {
        b.instances[0].position.x = 0.3 + 0.05 * i;
        b.step(0.2f);
    }
    for (int i = 0; i < 200 && b.voxels->stats().pending > 0; ++i) b.step();
    GIANT_CHECK(b.voxels->stats().pending == 0);
    u32 packed = 0;
    const double x = b.instances[0].position.x;
    GIANT_CHECK(b.voxel_at({x + 0.5, 1.0, 0.2}, packed));  // where it ended up
    GIANT_CHECK(!b.voxel_at({0.8, 1.0, 0.2}, packed));     // not where it started
    GIANT_CHECK(b.voxel_at({-2.5, 1.0, -3.0}, packed));    // the other one, untouched
}

GIANT_TEST("voxel scene: an instance that moves is never held, so moving it costs nothing") {
    Bench b;
    GIANT_CHECK(b.voxels->init());
    Instance cube;
    cube.mesh = 0;
    cube.position = {0.3, 1.1, 0.2};
    b.instances = {cube, cube};
    b.instances[1].position = {-3.0, 1.1, -3.0};
    b.instances[0].moves = true;
    b.step();
    u32 packed = 0;
    GIANT_CHECK(!b.voxel_at({0.8, 1.0, 0.2}, packed));  // not held
    GIANT_CHECK(b.voxel_at({-2.5, 1.0, -3.0}, packed)); // the still one is
    const u32 bricks = b.voxels->stats().bricks;
    for (int i = 0; i < 30; ++i) {
        b.instances[0].position.x = 0.3 + 0.05 * i;
        b.step(0.0f); // no budget: nothing would be built anyway
        GIANT_CHECK(b.voxels->stats().pending == 0);
    }
    GIANT_CHECK(b.voxels->stats().bricks == bricks);
    // Still again, it is held.
    b.instances[0].moves = false;
    for (int i = 0; i < 20 && (i == 0 || b.voxels->stats().pending > 0); ++i) b.step();
    GIANT_CHECK(b.voxel_at({b.instances[0].position.x + 0.5, 1.0, 0.2}, packed));
}

GIANT_TEST("voxel scene: many changes at once rebuild everything, and it settles") {
    Bench b(4096);
    GIANT_CHECK(b.voxels->init());
    for (int i = 0; i < 400; ++i) {
        Instance cube;
        cube.mesh = 0;
        cube.scale = {0.2f, 0.2f, 0.2f};
        cube.position = {static_cast<double>(i % 20) * 0.7 - 7.0, 1.1, static_cast<double>(i / 20) * 0.7 - 7.0};
        b.instances.push_back(cube);
    }
    b.step();
    // Every one moves up, every frame for a while (a rebuild at a time).
    for (int f = 0; f < 10; ++f) {
        for (Instance& in : b.instances) in.position.y += 0.1;
        b.step(0.5f);
    }
    for (int i = 0; i < 500 && b.voxels->stats().pending > 0; ++i) b.step();
    GIANT_CHECK(b.voxels->stats().pending == 0);
    const double y = b.instances[0].position.y;
    GIANT_CHECK(b.voxels->probe(0, {-7.0, y + 0.1, -7.0}) == VoxelProbe::solid);
    GIANT_CHECK(b.voxels->probe(0, {-7.0, 1.2, -7.0}) == VoxelProbe::empty);
}

// Scatter arrives a cell at a time: thousands of placements over a few
// frames, each frame under the bulk limit. They wait gathered by region, so
// the backlog never overflows into rebuilding every window.
GIANT_TEST("voxel scene: thousands of placements arriving wait by region, no rebuild") {
    Bench b(4096);
    GIANT_CHECK(b.voxels->init());
    b.step();
    const u32 rebuilds = b.voxels->stats().rebuilds;
    for (int f = 0; f < 40; ++f) {
        for (int i = 0; i < 200; ++i) {
            Instance cube;
            cube.mesh = 0;
            cube.scale = {0.05f, 0.05f, 0.05f};
            const int k = f * 200 + i;
            cube.position = {static_cast<double>(k % 90) * 0.13 - 6.0, 0.6 + 0.01 * (k % 7),
                             static_cast<double>((k / 90) % 90) * 0.13 - 6.0};
            b.instances.push_back(cube);
        }
        b.step(0.2f);
    }
    GIANT_CHECK(b.voxels->stats().rebuilds == rebuilds); // 8000 changes, never over the backlog
    // A run from the middle goes while its region's jobs are still queued:
    // the indices after it shift down under them.
    b.instances.erase(b.instances.begin() + 7000, b.instances.begin() + 7400);
    b.step(0.2f);
    for (int i = 0; i < 2000 && b.voxels->stats().pending > 0; ++i) b.step();
    GIANT_CHECK(b.voxels->stats().pending == 0);
    // The last one placed is in the voxel world.
    const Instance& last = b.instances.back();
    GIANT_CHECK(b.voxels->probe(0, last.position) == VoxelProbe::solid);
}

GIANT_TEST("voxel scene: removing one from the middle keeps the rest") {
    Bench b;
    GIANT_CHECK(b.voxels->init());
    Instance cube;
    cube.mesh = 0;
    b.instances = {cube, cube, cube};
    b.instances[0].position = {-3.0, 1.1, 0.0};
    b.instances[1].position = {0.0, 1.1, 0.0};
    b.instances[2].position = {3.0, 1.1, 0.0};
    b.step();
    b.instances.erase(b.instances.begin() + 1);
    b.step();
    GIANT_CHECK(b.voxels->stats().pending == 0);
    u32 packed = 0;
    GIANT_CHECK(b.voxel_at({-2.5, 1.0, 0.0}, packed));
    GIANT_CHECK(!b.voxel_at({0.5, 1.0, 0.0}, packed));
    GIANT_CHECK(b.voxel_at({3.5, 1.0, 0.0}, packed));
    // Moving the last one now (its index shifted) still finds it.
    b.instances[1].position = {3.0, 4.1, 0.0};
    b.step();
    GIANT_CHECK(!b.voxel_at({3.5, 1.0, 0.0}, packed));
    GIANT_CHECK(b.voxel_at({3.5, 4.0, 0.0}, packed));
}

GIANT_TEST("voxel scene: bricks left behind by the camera are dropped, new ones built") {
    Bench b;
    GIANT_CHECK(b.voxels->init());
    Instance ground;
    ground.mesh = 0;
    ground.position = {0.0, -0.05, 0.0};
    ground.scale = {1000.0f, 0.1f, 1000.0f};
    b.instances = {ground};
    b.step();
    GIANT_CHECK(b.voxels->stats().pending == 0 && !b.voxels->stats().overflow);
    const u32 full = b.voxels->stats().bricks;
    const voxel::BrickCoord start = b.voxels->clipmap().brick_at(0, {-7.0, -0.05, 0.0});
    GIANT_CHECK(b.voxels->brick_index(0, start) != gpu::k_no_brick);
    b.camera = {40.0, 1.0, 0.0};
    b.step(0.0f);
    GIANT_CHECK(b.voxels->probe(0, {40.0, -0.05, 0.0}) == VoxelProbe::building); // the coarse level stands in
    for (int i = 0; i < 2000 && b.voxels->stats().pending > 0; ++i) b.step(0.0f);
    GIANT_CHECK(b.voxels->stats().pending == 0);
    GIANT_CHECK(b.voxels->brick_index(0, start) == gpu::k_no_brick);
    GIANT_CHECK(b.voxels->brick_index(0, b.voxels->clipmap().brick_at(0, {40.0, -0.05, 0.0})) != gpu::k_no_brick);
    GIANT_CHECK(b.voxels->stats().bricks == full); // a flat ground fills each window alike
}

GIANT_TEST("voxel scene: the renderer draws the voxel view in one pass") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    auto device = rhi::create_device(desc);
    RendererDesc rd;
    auto renderer = Renderer::create(*device, rd);
    GIANT_CHECK(renderer != nullptr);
    if (renderer == nullptr) {
        return;
    }
    rhi::TextureDesc target_desc;
    target_desc.width = 32;
    target_desc.height = 16;
    target_desc.format = rhi::Format::rgba8_srgb;
    target_desc.usage = rhi::TextureUsage::render_target;
    const rhi::TextureHandle target = device->create_texture(target_desc);
    std::vector<Instance> instances(1);
    instances[0].mesh = renderer->builtin(BuiltinMesh::sphere);
    instances[0].position = {0.0, 0.0, -5.0};
    FrameInput frame;
    frame.instances = instances;
    frame.settings.debug_view = DebugView::voxels;
    frame.settings.gpu_culling = false; // one raster pass
    frame.settings.voxel_ms_per_frame = 1000.0f;
    for (int i = 0; i < 2; ++i) {
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        renderer->render(*cmd, target, frame);
        device->end_frame();
    }
    const RenderStats s = renderer->last_stats();
    GIANT_CHECK(s.voxel_bricks > 0 && s.voxel_pending == 0 && s.voxel_capacity == rd.voxel_bricks);
    GIANT_CHECK(s.draws == 2); // the raster pass, then the voxel view in place of shading
    GIANT_CHECK(s.post_draws == 0);
    // Off, the voxel world stays as it was.
    frame.settings.voxel_world = false;
    instances.clear();
    frame.instances = instances;
    rhi::CommandList* cmd = device->begin_frame(nullptr);
    renderer->render(*cmd, target, frame);
    device->end_frame();
    GIANT_CHECK(renderer->last_stats().voxel_bricks == s.voxel_bricks);
}

GIANT_TEST("voxel scene: constant bulk change still builds the whole view") {
    Bench b(4096);
    GIANT_CHECK(b.voxels->init());
    for (int i = 0; i < 2000; ++i) {
        Instance cube;
        cube.mesh = 0;
        cube.scale = {0.05f, 0.05f, 0.05f};
        cube.position = {static_cast<double>(i % 40) * 0.1 - 2.0, 1.1, static_cast<double>(i / 40) * 0.1 - 2.0};
        b.instances.push_back(cube);
    }
    std::vector<DVec3> fixed;
    for (int i = 0; i < 16; ++i) {
        Instance cube;
        cube.mesh = 0;
        cube.scale = {0.5f, 0.5f, 0.5f};
        cube.position = {static_cast<double>(i % 4) * 6.0 - 9.0 + 0.1, 1.1, static_cast<double>(i / 4) * 6.0 - 9.0 + 0.1};
        fixed.push_back(cube.position);
        b.instances.push_back(cube);
    }
    // Every small cube moves every frame (a bulk change each frame).
    for (int f = 0; f < 400; ++f) {
        for (usize i = 0; i < 2000; ++i) b.instances[i].position.y = 1.1 + 0.01 * (f % 50);
        b.step(0.0f);
    }
    u32 building = 0;
    for (const DVec3& p : fixed) {
        building += b.voxels->probe(1, p) == VoxelProbe::building ? 1u : 0u;
    }
    GIANT_CHECK(building == 0);
}

GIANT_TEST("voxel scene: a rebuild started by a full backlog loses no changes") {
    Bench b(8192);
    GIANT_CHECK(b.voxels->init());
    for (int i = 0; i < 100; ++i) {
        Instance cube;
        cube.mesh = 0;
        cube.scale = {0.05f, 0.05f, 0.05f};
        cube.position = {0.5 + (i % 10) * 0.3, 0.5 + (i / 10) * 0.3, 0.5};
        b.instances.push_back(cube);
    }
    for (int i = 0; i < 16800; ++i) {
        Instance far;
        far.mesh = 0;
        far.scale = {0.05f, 0.05f, 0.05f};
        far.position = {5000.0 + i, 0.0, 5000.0};
        b.instances.push_back(far);
    }
    b.step(0.0f); // the coarse window's find job is part-way through
    b.instances[0].position = {-9.0, -5.0, -9.0};
    b.instances[0].scale = {1.0f, 1.0f, 1.0f};
    for (usize i = 0; i < 2100; ++i) b.instances[100 + i].position.y += 1.0; // a backlog over the limit
    for (int i = 0; i < 5000 && b.voxels->stats().pending > 0; ++i) b.step();
    GIANT_CHECK(b.voxels->stats().pending == 0);
    GIANT_CHECK(b.voxels->probe(1, {-9.45, -5.0, -9.0}) == VoxelProbe::solid); // its -X face, where it went
}

GIANT_TEST("voxel scene: missing bricks are retried even at a zero budget") {
    Bench b(48);
    GIANT_CHECK(b.voxels->init());
    Instance ground;
    ground.mesh = 0;
    ground.scale = {6.0f, 0.1f, 6.0f}; // about 40 bricks: fits on its own, with room to spare
    Instance tower;
    tower.mesh = 0;
    tower.position = {0.0, 6.0, 0.0};
    tower.scale = {3.0f, 12.0f, 3.0f};
    b.instances = {ground, tower};
    b.step();
    GIANT_CHECK(b.voxels->stats().missing > 0);
    b.instances.pop_back(); // room frees up
    for (int i = 0; i < 3000 && (b.voxels->stats().pending > 0 || b.voxels->stats().missing > 0); ++i) b.step(0.0f);
    GIANT_CHECK(b.voxels->stats().missing == 0 && b.voxels->stats().pending == 0);
}

namespace {

// 16 fixed cubes around the window plus 1,000 small ones that `change`
// edits each frame, 200 frames at a zero budget: how many fixed cubes
// still read "building" on level 1 (0 when no search is starved).
template <typename Change>
u32 still_building_under(const Change& change) {
    Bench b(4096);
    GIANT_CHECK(b.voxels->init());
    std::vector<DVec3> fixed;
    for (int i = 0; i < 16; ++i) {
        Instance cube;
        cube.mesh = 0;
        cube.scale = {0.5f, 0.5f, 0.5f};
        cube.position = {static_cast<double>(i % 4) * 6.0 - 9.0 + 0.1, 1.1, static_cast<double>(i / 4) * 6.0 - 9.0 + 0.1};
        fixed.push_back(cube.position);
        b.instances.push_back(cube);
    }
    for (int i = 0; i < 1000; ++i) {
        Instance cube;
        cube.mesh = 0;
        cube.scale = {0.05f, 0.05f, 0.05f};
        cube.position = {static_cast<double>(i % 40) * 0.1 - 2.0, 1.1, static_cast<double>(i / 40) * 0.1 - 2.0};
        b.instances.push_back(cube);
    }
    for (int f = 0; f < 200; ++f) {
        change(b.instances, f);
        b.step(0.0f);
    }
    u32 building = 0;
    for (const DVec3& p : fixed) {
        building += b.voxels->probe(1, p) == VoxelProbe::building ? 1u : 0u;
    }
    return building;
}

} // namespace

GIANT_TEST("voxel scene: bulk changes with the count going up and down still build the whole view") {
    GIANT_CHECK(still_building_under([](std::vector<Instance>& in, int f) {
                    for (usize i = 16; i < in.size(); ++i) in[i].position.y = 1.1 + 0.01 * (f % 50);
                    if (f % 2 != 0) {
                        in.pop_back();
                    } else {
                        in.push_back(in.back());
                    }
                }) == 0);
}

GIANT_TEST("voxel scene: one removed every other frame still builds the whole view") {
    // (Every frame at a zero budget only queues removals faster than one
    // job a frame works them off: a throughput limit, not a stall.)
    GIANT_CHECK(still_building_under([](std::vector<Instance>& in, int f) {
                    if (f % 2 == 0) in.pop_back();
                }) == 0);
}

GIANT_TEST("voxel scene: small instances are stand-ins, held once built; big ones never") {
    Bench b;
    GIANT_CHECK(b.voxels->init());
    b.voxels->set_stand_in_radius(1.0f);
    b.instances.resize(2);
    b.instances[0].position = {0.3, 1.1, 0.2}; // off the voxel grid; the cube's sphere: radius about 0.87
    b.instances[1].position = {-5.3, 2.1, -5.2};
    b.instances[1].scale = {3.0f, 3.0f, 3.0f}; // radius about 2.6
    b.step(0.0f);
    GIANT_CHECK(!b.voxels->holds(0)); // not built yet
    for (int i = 0; i < 50 && b.voxels->stats().pending > 0; ++i) b.step();
    GIANT_CHECK(b.voxels->stats().pending == 0);
    GIANT_CHECK(b.voxels->holds(0) && !b.voxels->holds(1) && !b.voxels->holds(2));
    u32 packed = 0;
    GIANT_CHECK(b.voxel_at({0.8, 1.1, 0.2}, packed) && b.stand_in);   // the small cube's +X face
    GIANT_CHECK(b.voxel_at({-3.8, 2.1, -5.2}, packed) && !b.stand_in); // the big one's
    // Moved: not held until rebuilt where it went.
    b.instances[0].position = {1.3, 1.1, 0.2};
    b.step(0.0f);
    GIANT_CHECK(!b.voxels->holds(0));
    for (int i = 0; i < 50 && b.voxels->stats().pending > 0; ++i) b.step();
    GIANT_CHECK(b.voxels->holds(0));
    // A larger radius makes the big one a stand-in too, after a rebuild.
    b.voxels->set_stand_in_radius(5.0f);
    b.step(0.0f);
    GIANT_CHECK(!b.voxels->holds(1));
    for (int i = 0; i < 100 && b.voxels->stats().pending > 0; ++i) b.step();
    GIANT_CHECK(b.voxels->holds(1));
    GIANT_CHECK(b.voxel_at({-3.8, 2.1, -5.2}, packed) && b.stand_in);
}

GIANT_TEST("voxel scene: levels say whether the far field need look at them") {
    Bench b;
    GIANT_CHECK(b.voxels->init());
    b.voxels->set_stand_in_radius(1.0f);
    b.instances.resize(1);
    b.instances[0].position = {-5.3, 2.1, -5.2};
    b.instances[0].scale = {3.0f, 3.0f, 3.0f}; // too big: a mesh, never a stand-in
    const auto settle = [&] {
        for (int i = 0; i < 100 && b.voxels->stats().pending > 0; ++i) b.step();
    };
    const auto stands = [&] {
        std::vector<gpu::VoxelLevel> levels(b.voxels->levels());
        b.voxels->write_levels(levels.data(), b.camera);
        u32 n = 0;
        for (const gpu::VoxelLevel& l : levels) n += l.brick[3] != 0 ? 1u : 0u;
        return n;
    };
    b.step(0.0f);
    settle();
    GIANT_CHECK(b.voxels->stats().bricks > 0 && stands() == 0); // nothing for the far field
    // A small one: every level holding it has stand-in bricks.
    b.instances.push_back({});
    b.instances[1].position = {0.3, 1.1, 0.2};
    b.step();
    settle();
    GIANT_CHECK(stands() == b.voxels->levels());
    // Gone again: the levels empty of stand-ins.
    b.instances.pop_back();
    b.step();
    settle();
    GIANT_CHECK(stands() == 0);
}
