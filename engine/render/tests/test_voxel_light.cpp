// Which voxel bricks get lit each frame (src/voxel_light.h), on the null
// device: new bricks cleared and lit first, the rest in turn, budgets, and
// starting afresh after global illumination was off. (What the light is
// runs on the GPU: test_render.cpp's d3d12 test.)
#include "giant/geometry/clusters.h"
#include "giant/geometry/mesh.h"
#include "giant/render/renderer.h"
#include "giant/rhi/rhi.h"
#include "giant_test.h"
#include "gpu_layout.h"
#include "voxel_light.h"
#include "voxel_scene.h"

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace giant;
using namespace giant::render;

namespace {

struct Bench {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<VoxelScene> voxels;
    geometry::ClusterMesh cube = geometry::build_clusters(geometry::make_cube());
    std::vector<Instance> instances;
    std::vector<Material> materials;
    DVec3 camera{0.0, 1.0, 0.0};

    Bench() {
        rhi::DeviceDesc desc;
        desc.backend = rhi::Backend::null;
        device = rhi::create_device(desc);
        voxel::ClipmapDesc clip;
        clip.levels = 2;
        clip.voxel_size = 0.25f;
        clip.bricks_per_axis = 8;
        voxels = std::make_unique<VoxelScene>(*device, clip, 1024);
    }
    void step() {
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        voxels->update(*cmd, camera, instances, materials, 1,
                       [this](MeshId id) { return id == 0 ? &cube : nullptr; }, 1000.0f);
        device->end_frame();
    }
    std::set<u32> owned() const {
        std::set<u32> out;
        u32 level = 0;
        voxel::BrickCoord brick;
        for (u32 i = 0; i < voxels->capacity(); ++i) {
            if (voxels->pool_brick(i, level, brick)) out.insert(i);
        }
        return out;
    }
};

std::vector<u32> indices_of(std::span<const gpu::LightJob> jobs) {
    std::vector<u32> out;
    for (const gpu::LightJob& j : jobs) out.push_back(j.index);
    return out;
}

} // namespace

GIANT_TEST("gi light: pool bricks know their slot, and say what was placed or rebuilt") {
    Bench b;
    GIANT_CHECK(b.voxels->init());
    Instance cube;
    cube.mesh = 0;
    cube.position = {0.3, 1.1, 0.2};
    b.instances = {cube};
    b.step();
    const std::set<u32> owned = b.owned();
    GIANT_CHECK(!owned.empty() && owned.size() == b.voxels->stats().bricks);
    GIANT_CHECK(b.voxels->placed().size() == owned.size() && b.voxels->rebuilt().empty());
    for (const u32 i : owned) {
        u32 level = 0;
        voxel::BrickCoord brick;
        GIANT_CHECK(b.voxels->pool_brick(i, level, brick));
        GIANT_CHECK(b.voxels->brick_index(level, brick) == i);
    }
    // Nothing changes: nothing placed or rebuilt.
    b.step();
    GIANT_CHECK(b.voxels->placed().empty() && b.voxels->rebuilt().empty());
    // Recoloured: rebuilt in place.
    b.materials = {Material{{0.1f, 0.1f, 0.9f}}};
    b.step();
    GIANT_CHECK(b.voxels->placed().empty() && !b.voxels->rebuilt().empty());
    // Removed: the bricks are free, and say so.
    b.instances.clear();
    b.step();
    GIANT_CHECK(b.owned().empty());
    u32 level = 0;
    voxel::BrickCoord brick;
    GIANT_CHECK(!b.voxels->pool_brick(*owned.begin(), level, brick));
    GIANT_CHECK(!b.voxels->pool_brick(b.voxels->capacity(), level, brick)); // out of range
}

GIANT_TEST("gi light: new bricks are cleared and lit first, then every brick in turn") {
    Bench b;
    GIANT_CHECK(b.voxels->init());
    Instance ground;
    ground.mesh = 0;
    ground.position = {0.0, -0.05, 0.0};
    ground.scale = {1000.0f, 0.1f, 1000.0f};
    b.instances = {ground};
    b.step();
    const std::set<u32> owned = b.owned();
    GIANT_CHECK(owned.size() > 20);
    VoxelLight light(b.voxels->capacity());
    constexpr u32 k_budget = 8;
    light.plan(*b.voxels, k_budget);
    // The first frame clears every brick and starts lighting them.
    GIANT_CHECK(std::set<u32>(light.clears().begin(), light.clears().end()) == owned);
    GIANT_CHECK(light.stats().lit == k_budget && light.stats().waiting == owned.size() - k_budget);
    for (const gpu::LightJob& j : light.jobs()) {
        u32 level = 0;
        voxel::BrickCoord brick;
        GIANT_CHECK(b.voxels->pool_brick(j.index, level, brick));
        GIANT_CHECK(j.level == level && j.brick[0] == brick.x && j.brick[1] == brick.y && j.brick[2] == brick.z);
    }
    // Each one lit once while the new ones drain, never twice in a frame.
    std::set<u32> lit;
    for (const u32 i : indices_of(light.jobs())) lit.insert(i);
    for (int frame = 0; frame < 1000 && light.stats().waiting > 0; ++frame) {
        b.step();
        light.plan(*b.voxels, k_budget);
        GIANT_CHECK(light.clears().empty());
        const std::vector<u32> now = indices_of(light.jobs());
        GIANT_CHECK(std::set<u32>(now.begin(), now.end()).size() == now.size());
        for (const u32 i : now) lit.insert(i);
    }
    GIANT_CHECK(lit == owned);
    // Then the round: every brick again within bricks / budget frames.
    std::set<u32> round;
    const u32 frames = static_cast<u32>((owned.size() + k_budget - 1) / k_budget);
    for (u32 frame = 0; frame < frames; ++frame) {
        b.step();
        light.plan(*b.voxels, k_budget);
        GIANT_CHECK(light.stats().lit == k_budget || frame + 1 == frames);
        for (const u32 i : indices_of(light.jobs())) round.insert(i);
    }
    GIANT_CHECK(round == owned);
}

GIANT_TEST("gi light: changed bricks go before the round; after a pause what was placed meanwhile is cleared") {
    Bench b;
    GIANT_CHECK(b.voxels->init());
    Instance ground;
    ground.mesh = 0;
    ground.position = {0.0, -0.05, 0.0};
    ground.scale = {1000.0f, 0.1f, 1000.0f};
    Instance cube;
    cube.mesh = 0;
    cube.position = {0.3, 1.1, 0.2};
    b.instances = {ground, cube};
    b.step();
    VoxelLight light(b.voxels->capacity());
    for (int i = 0; i < 1000; ++i) {
        light.plan(*b.voxels, 64);
        if (light.stats().waiting == 0) break;
        b.step();
    }
    GIANT_CHECK(light.stats().waiting == 0);
    // The cube moves: its new bricks are cleared, and they and the rebuilt
    // ones are lit first.
    b.instances[1].position.x += 1.7;
    b.step();
    const std::vector<u32> placed(b.voxels->placed().begin(), b.voxels->placed().end());
    const std::vector<u32> rebuilt(b.voxels->rebuilt().begin(), b.voxels->rebuilt().end());
    GIANT_CHECK(!placed.empty() || !rebuilt.empty());
    light.plan(*b.voxels, 64);
    GIANT_CHECK(std::vector<u32>(light.clears().begin(), light.clears().end()) == placed);
    const std::vector<u32> jobs = indices_of(light.jobs());
    std::set<u32> first;
    u32 level = 0;
    voxel::BrickCoord brick;
    for (const u32 i : placed) if (b.voxels->pool_brick(i, level, brick)) first.insert(i);
    for (const u32 i : rebuilt) if (b.voxels->pool_brick(i, level, brick)) first.insert(i);
    GIANT_CHECK(jobs.size() >= first.size());
    GIANT_CHECK(std::set<u32>(jobs.begin(), jobs.begin() + static_cast<std::ptrdiff_t>(first.size())) == first);
    // Off for a while (the world changes meanwhile): back on, the bricks
    // put in the pool meanwhile are cleared and lit first, the rest keep
    // their light.
    light.off();
    GIANT_CHECK(light.stats().lit == 0 && light.jobs().empty());
    const u64 before = b.voxels->placements();
    b.instances[1].position.y += 3.0; // up into bricks of its own (new in the pool)
    b.step();
    b.step();
    std::set<u32> meanwhile;
    for (const u32 i : b.owned()) if (b.voxels->placed_at(i) > before) meanwhile.insert(i);
    GIANT_CHECK(!meanwhile.empty() && meanwhile.size() < b.owned().size());
    light.plan(*b.voxels, 2); // one of them, and one for the round
    GIANT_CHECK(std::set<u32>(light.clears().begin(), light.clears().end()) == meanwhile);
    GIANT_CHECK(light.stats().lit == 2 && light.stats().waiting == meanwhile.size() - 1);
    // Clears lost (out of memory): the next plan clears them again.
    light.lost();
    b.step();
    light.plan(*b.voxels, 1);
    GIANT_CHECK(std::set<u32>(light.clears().begin(), light.clears().end()) == meanwhile);
    // A zero budget still lights one.
    b.step();
    light.plan(*b.voxels, 0);
    GIANT_CHECK(light.stats().lit == 1);
}

GIANT_TEST("gi light: a quarter of the budget goes round while new bricks keep coming") {
    Bench b;
    GIANT_CHECK(b.voxels->init());
    Instance ground;
    ground.mesh = 0;
    ground.position = {0.0, -0.05, 0.0};
    ground.scale = {1000.0f, 0.1f, 1000.0f};
    b.instances = {ground};
    b.step();
    VoxelLight light(b.voxels->capacity());
    // Many new bricks every frame (the camera flying on): the urgent ones
    // never drain, yet the round still reaches every brick.
    std::set<u32> round;
    u32 most_waiting = 0;
    for (int frame = 0; frame < 400; ++frame) {
        b.camera.x += 1.0; // a new slab of bricks every other frame (2 m bricks)
        b.step();
        light.plan(*b.voxels, 8);
        GIANT_CHECK(light.stats().lit == 8);
        most_waiting = std::max(most_waiting, light.stats().waiting);
        for (const u32 i : indices_of(light.jobs())) round.insert(i);
    }
    GIANT_CHECK(most_waiting > 64); // the new ones did pile up
    u32 owned_lit = 0;
    for (const u32 i : b.owned()) owned_lit += round.count(i) != 0 ? 1u : 0u;
    GIANT_CHECK(owned_lit == b.owned().size());
}

GIANT_TEST("gi light: the renderer lights voxels in the lit views and the voxel view only") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    auto device = rhi::create_device(desc);
    auto renderer = Renderer::create(*device);
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
    std::vector<Instance> instances(2);
    instances[0].mesh = renderer->builtin(BuiltinMesh::plane);
    instances[0].scale = {40.0f, 1.0f, 40.0f};
    instances[1].mesh = renderer->builtin(BuiltinMesh::cube);
    instances[1].position = {0.0, 1.0, -4.0};
    FrameInput frame;
    frame.instances = instances;
    frame.view.position = {0.0, 2.0, 0.0};
    frame.settings.voxel_ms_per_frame = 1000.0f;
    frame.settings.gi_bricks_per_frame = 4;
    const auto draw = [&] {
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        renderer->render(*cmd, target, frame);
        device->end_frame();
        return renderer->last_stats();
    };
    RenderStats st = draw();
    GIANT_CHECK(st.gi && st.voxel_bricks > 4);
    GIANT_CHECK(st.gi_cleared == st.voxel_bricks && st.gi_lit == 4 && st.gi_waiting == st.voxel_bricks - 4);
    const auto timed = [&] {
        for (const rhi::GpuTiming& t : device->gpu_timings()) if (t.name == std::string("gi")) return true;
        return false;
    };
    GIANT_CHECK(timed());
    st = draw();
    GIANT_CHECK(st.gi && st.gi_cleared == 0 && st.gi_lit == 4);
    frame.settings.debug_view = DebugView::lighting;
    GIANT_CHECK(draw().gi);
    frame.settings.debug_view = DebugView::voxels;
    GIANT_CHECK(draw().gi);
    frame.settings.debug_view = DebugView::clusters;
    st = draw();
    GIANT_CHECK(!st.gi && st.gi_lit == 0 && !timed());
    // Back in the lit view, nothing was placed meanwhile: nothing cleared.
    frame.settings.debug_view = DebugView::lit;
    st = draw();
    GIANT_CHECK(st.gi && st.gi_cleared == 0 && st.gi_lit == 4);
    // Off, or with the voxel world off: none.
    frame.settings.gi = false;
    GIANT_CHECK(!draw().gi);
    frame.settings.gi = true;
    frame.settings.voxel_world = false;
    GIANT_CHECK(!draw().gi);
    device->destroy(target);
}
