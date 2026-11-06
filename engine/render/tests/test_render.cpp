#include "giant/geometry/mesh.h"
#include "giant/render/renderer.h"

#include "giant_test.h"
#include "shadow_pages.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace giant;
using namespace giant::render;

namespace {

Vec3 project(const Mat4& m, const Vec3& p) {
    const Vec4 c = m * Vec4{p.x, p.y, p.z, 1.0f};
    return {c.x / c.w, c.y / c.w, c.z / c.w};
}

} // namespace

GIANT_TEST("render: the camera projects reverse-Z with no far plane") {
    View view;
    view.near_plane = 0.5f;
    view.vertical_fov_degrees = 90.0f;
    const Mat4 vp = Renderer::view_projection(view, 2.0f);
    // Straight ahead (-Z) lands in the centre; depth is near / distance.
    const Vec3 ahead = project(vp, {0, 0, -10});
    GIANT_CHECK_NEAR(ahead.x, 0.0, 1e-6);
    GIANT_CHECK_NEAR(ahead.y, 0.0, 1e-6);
    GIANT_CHECK_NEAR(ahead.z, 0.05, 1e-6);
    GIANT_CHECK_NEAR(project(vp, {0, 0, -0.5f}).z, 1.0, 1e-6);
    GIANT_CHECK(project(vp, {0, 0, -1e7f}).z > 0.0f); // 10 000 km away is still in front
    // 90 degrees vertically: 45 degrees up is the top edge; aspect 2 widens x.
    GIANT_CHECK_NEAR(project(vp, {0, 10, -10}).y, 1.0, 1e-5);
    GIANT_CHECK_NEAR(project(vp, {20, 0, -10}).x, 1.0, 1e-5);

    // Turning the camera 90 degrees left (yaw +90) makes -X the view direction.
    view.orientation = quat_from_euler({0.0f, 90.0f, 0.0f});
    const Vec3 left = project(Renderer::view_projection(view, 1.0f), {-10, 0, 0});
    GIANT_CHECK_NEAR(left.x, 0.0, 1e-5);
    GIANT_CHECK_NEAR(left.z, 0.05, 1e-5);
}

GIANT_TEST("render: the scene is one raster draw and one shading pass on the null device") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    auto device = rhi::create_device(desc);
    auto renderer = Renderer::create(*device);
    GIANT_CHECK(renderer != nullptr);
    if (renderer == nullptr) {
        return;
    }
    rhi::TextureDesc target_desc;
    target_desc.width = 64;
    target_desc.height = 32;
    target_desc.format = rhi::Format::rgba8_srgb;
    target_desc.usage = rhi::TextureUsage::render_target;
    const rhi::TextureHandle target = device->create_texture(target_desc);

    std::vector<Instance> instances(4);
    instances[0].mesh = renderer->builtin(BuiltinMesh::cube);
    instances[1].mesh = renderer->builtin(BuiltinMesh::sphere);
    instances[2].mesh = renderer->builtin(BuiltinMesh::cube);
    instances[3].mesh = 999; // unknown: skipped
    for (usize i = 0; i < instances.size(); ++i) {
        instances[i].position = {static_cast<double>(i) * 2.0 - 3.0, 0.0, -6.0}; // in view
    }
    FrameInput frame;
    frame.instances = instances;
    frame.lighting.background = {0.5f, 0.0f, 0.0f};
    frame.settings.gpu_culling = false; // exact stats on the CPU

    for (int i = 0; i < 2; ++i) { // the second frame reuses the targets
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        renderer->render(*cmd, target, frame);
        device->end_frame();
    }
    GIANT_CHECK(renderer->last_stats().draws == 2); // clusters, then shading
    GIANT_CHECK(renderer->last_stats().instances == 3);
    GIANT_CHECK(renderer->last_stats().clusters >= 3);
    GIANT_CHECK(!renderer->last_stats().from_gpu);
    GIANT_CHECK(renderer->last_stats().post_draws == 4); // screen reflections; SMAA: edges, weights, blend
    GIANT_CHECK(device->last_frame_stats().draws == 2 + 4);
    // The background is cleared as linear colour: 0.5 is sRGB byte 188.
    const auto pixels = device->read_texture(target);
    GIANT_CHECK(pixels.size() == 64 * 32 * 4 && pixels[0] == 188 && pixels[1] == 0);

    // A resized target gets a matching depth target (the null device checks).
    device->destroy(target);
    target_desc.width = 100;
    const rhi::TextureHandle bigger = device->create_texture(target_desc);
    rhi::CommandList* cmd = device->begin_frame(nullptr);
    renderer->render(*cmd, bigger, frame);
    device->end_frame();
    device->destroy(bigger);
}

namespace {
struct NullScene {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<Renderer> renderer;
    rhi::TextureHandle target;
    NullScene() {
        rhi::DeviceDesc desc;
        desc.backend = rhi::Backend::null;
        device = rhi::create_device(desc);
        renderer = Renderer::create(*device);
        rhi::TextureDesc t;
        t.width = 1920; // LOD depends on the pixel size
        t.height = 1080;
        t.format = rhi::Format::rgba8_srgb;
        t.usage = rhi::TextureUsage::render_target;
        target = device->create_texture(t);
    }
    RenderStats draw(const FrameInput& frame) {
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        renderer->render(*cmd, target, frame);
        device->end_frame();
        return renderer->last_stats();
    }
    // Draws until streaming has loaded what the view asks for (CPU culling:
    // requests load the next frame).
    RenderStats settle(const FrameInput& frame) {
        RenderStats s = draw(frame);
        for (int i = 0; i < 16 && (s.streamed_in > 0 || s.waiting > 0); ++i) {
            s = draw(frame);
        }
        return draw(frame);
    }
};
} // namespace

GIANT_TEST("render: detail follows distance, the error setting and the view") {
    NullScene s;
    GIANT_CHECK(s.renderer != nullptr);
    if (s.renderer == nullptr) return;
    std::vector<Instance> instances(1);
    instances[0].mesh = s.renderer->builtin(BuiltinMesh::sphere);
    instances[0].scale = {10, 10, 10};
    FrameInput frame;
    frame.instances = instances;
    frame.settings.gpu_culling = false; // the CPU cut, with same-frame stats
    const auto at = [&](double distance, float error = 1.0f) {
        instances[0].position = {0.0, 0.0, -distance};
        frame.settings.lod_error_pixels = error;
        return s.settle(frame);
    };
    const RenderStats near = at(8.0);
    const RenderStats mid = at(80.0);
    const RenderStats far = at(2000.0);
    GIANT_CHECK(near.triangles > mid.triangles && mid.triangles > far.triangles && far.triangles > 0);
    GIANT_CHECK(near.draws == 2 && far.draws == 2 && near.instances == 1);
    GIANT_CHECK(at(80.0, 8.0f).triangles < mid.triangles); // more error allowed: fewer triangles
    // Behind the camera: nothing drawn (only the shading pass runs).
    instances[0].position = {0.0, 0.0, 50.0};
    const RenderStats behind = s.draw(frame);
    GIANT_CHECK(behind.draws == 1 && behind.clusters == 0 && behind.triangles == 0);
    // Debug views draw the same clusters.
    instances[0].position = {0.0, 0.0, -80.0};
    frame.settings.lod_error_pixels = 1.0f;
    for (const DebugView v : {DebugView::clusters, DebugView::lod_levels, DebugView::triangles}) {
        frame.settings.debug_view = v;
        GIANT_CHECK(s.draw(frame).clusters == mid.clusters);
    }
}

GIANT_TEST("render: an always-mesh instance is never left to the far field") {
    NullScene s;
    if (s.renderer == nullptr) return;
    std::vector<Instance> instances(2);
    instances[0].mesh = s.renderer->builtin(BuiltinMesh::sphere);
    instances[0].scale = {10, 10, 10};
    instances[0].position = {0.0, 0.0, -1000.0};
    instances[1] = instances[0];
    instances[1].position = {20.0, 0.0, -1000.0};
    instances[1].always_mesh = true; // a terrain tile, say
    FrameInput frame;
    frame.instances = instances;
    frame.settings.gpu_culling = false;
    frame.settings.far_field_distance = 500.0f;
    frame.settings.voxel_ms_per_frame = 50.0f;
    RenderStats st = s.settle(frame);
    for (int i = 0; i < 200 && st.voxel_pending > 0; ++i) st = s.draw(frame);
    st = s.draw(frame);
    // Both are in the voxel world; only the ordinary one is left to it.
    GIANT_CHECK(st.far_field && st.far_culled == 1 && st.instances == 1);
    bool solid = false;
    for (const VoxelProbeLevel& l : s.renderer->probe_voxels({20.0, 0.0, -1000.0})) {
        solid = solid || std::string(l.state) == "solid";
    }
    GIANT_CHECK(solid);
    instances[0].always_mesh = true;
    st = s.draw(frame);
    GIANT_CHECK(st.far_culled == 0 && st.instances == 2);
}

GIANT_TEST("render: small instances past the far-field distance are left to the voxel world") {
    NullScene s;
    if (s.renderer == nullptr) return;
    std::vector<Instance> instances(2);
    instances[0].mesh = s.renderer->builtin(BuiltinMesh::sphere);
    instances[0].scale = {10, 10, 10};
    instances[0].position = {0.0, 0.0, -1000.0};
    instances[1] = instances[0];
    instances[1].position = {0.0, 0.0, -100.0};
    FrameInput frame;
    frame.instances = instances;
    frame.settings.gpu_culling = false;
    frame.settings.far_field_distance = 500.0f;
    frame.settings.voxel_ms_per_frame = 50.0f;
    // Draws until streaming and the voxel world have caught up.
    const auto settle = [&] {
        RenderStats st = s.settle(frame);
        for (int i = 0; i < 200 && st.voxel_pending > 0; ++i) {
            st = s.draw(frame);
        }
        return s.draw(frame);
    };
    // The first frame: its voxels are not built yet, so it stays a mesh.
    frame.settings.voxel_ms_per_frame = 0.0f;
    RenderStats st = s.draw(frame);
    GIANT_CHECK(st.far_field && st.far_culled == 0);
    frame.settings.voxel_ms_per_frame = 50.0f;
    st = settle();
    GIANT_CHECK(st.far_field && st.far_culled == 1 && st.instances == 1);
    // Straddling the distance: still a mesh.
    instances[0].position = {0.0, 0.0, -503.0};
    st = s.draw(frame);
    GIANT_CHECK(st.far_culled == 0 && st.instances == 2);
    // Moved far out: a mesh until its voxels are rebuilt where it went
    // (with no time to spare, that takes frames).
    instances[0].position = {0.0, 0.0, -1000.0};
    frame.settings.voxel_ms_per_frame = 0.0f;
    GIANT_CHECK(s.draw(frame).far_culled == 0);
    frame.settings.voxel_ms_per_frame = 50.0f;
    GIANT_CHECK(settle().far_culled == 1);
    // Too large to be left to the voxels: a mesh however far.
    instances[0].scale = {120, 120, 120}; // radius 60 m (the sphere's is 0.5)
    st = settle();
    GIANT_CHECK(st.far_culled == 0 && st.instances == 2);
    frame.settings.far_field_max_radius = 100.0f; // rebuilds the voxel world with it a stand-in
    frame.settings.voxel_ms_per_frame = 0.0f; // (which takes frames)
    GIANT_CHECK(s.draw(frame).far_culled == 0);
    frame.settings.voxel_ms_per_frame = 50.0f;
    GIANT_CHECK(settle().far_culled == 1);
    // Past the voxel world's coarsest window (about 30 km): a mesh, or it would vanish.
    instances[0].scale = {10, 10, 10};
    instances[0].position = {0.0, 0.0, -40000.0};
    st = settle();
    GIANT_CHECK(st.far_culled == 0 && st.instances == 2);
    // Off, with the voxel world frozen or in a debug view: every instance is a mesh.
    instances[0].position = {0.0, 0.0, -1000.0};
    GIANT_CHECK(settle().far_culled == 1);
    frame.settings.voxel_far_field = false;
    st = s.draw(frame);
    GIANT_CHECK(!st.far_field && st.far_culled == 0 && st.instances == 2);
    frame.settings.voxel_far_field = true;
    frame.settings.voxel_world = false;
    GIANT_CHECK(s.draw(frame).far_culled == 0);
    frame.settings.voxel_world = true;
    for (const DebugView v : {DebugView::clusters, DebugView::lod_levels, DebugView::triangles, DebugView::voxels}) {
        frame.settings.debug_view = v;
        GIANT_CHECK(s.draw(frame).far_culled == 0);
    }
    // GPU culling leaves the same instances out (it never sees them).
    frame.settings.debug_view = DebugView::lit;
    frame.settings.gpu_culling = true;
    GIANT_CHECK(settle().far_culled == 1);
}

GIANT_TEST("render: with the voxel pool full, far instances stay meshes") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    std::unique_ptr<rhi::Device> device = rhi::create_device(desc);
    RendererDesc rd;
    rd.voxel_bricks = 4; // far too few
    std::unique_ptr<Renderer> renderer = Renderer::create(*device, rd);
    GIANT_CHECK(renderer != nullptr);
    if (renderer == nullptr) return;
    rhi::TextureDesc t;
    t.width = 640;
    t.height = 360;
    t.format = rhi::Format::rgba8_srgb;
    t.usage = rhi::TextureUsage::render_target;
    const rhi::TextureHandle target = device->create_texture(t);
    std::vector<Instance> instances(30);
    for (usize i = 0; i < instances.size(); ++i) {
        instances[i].mesh = renderer->builtin(BuiltinMesh::sphere);
        instances[i].scale = {10, 10, 10};
        instances[i].position = {static_cast<double>(i) * 300.0 - 4500.0, 0.0, -5000.0};
    }
    FrameInput frame;
    frame.instances = instances;
    frame.settings.gpu_culling = false;
    frame.settings.far_field_distance = 500.0f;
    frame.settings.voxel_ms_per_frame = 50.0f;
    RenderStats st;
    for (int i = 0; i < 100; ++i) {
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        renderer->render(*cmd, target, frame);
        device->end_frame();
        st = renderer->last_stats();
    }
    GIANT_CHECK(st.voxel_overflow && st.voxel_missing > 0);
    // Only instances whose bricks all made it into the pool are left out.
    GIANT_CHECK(st.far_culled < instances.size());
    device->destroy(target);
}

GIANT_TEST("render: detail streams in as the camera nears") {
    NullScene s;
    if (s.renderer == nullptr) return;
    const geometry::ClusterMesh* sphere = s.renderer->mesh(s.renderer->builtin(BuiltinMesh::sphere));
    GIANT_CHECK(sphere != nullptr);
    if (sphere == nullptr) return;
    std::vector<Instance> instances(1);
    instances[0].mesh = s.renderer->builtin(BuiltinMesh::sphere);
    instances[0].scale = {10, 10, 10};
    instances[0].position = {0.0, 0.0, -2000.0};
    FrameInput frame;
    frame.instances = instances;
    frame.settings.gpu_culling = false;
    const RenderStats far = s.settle(frame);
    GIANT_CHECK(far.resident_groups < far.total_groups); // far away, little is loaded
    GIANT_CHECK(far.pool_used_bytes > 0 && far.pool_used_bytes <= far.pool_bytes);
    // Close up: the first frame draws what is loaded, coarse but whole...
    instances[0].position = {0.0, 0.0, -8.0};
    const RenderStats first = s.draw(frame);
    GIANT_CHECK(first.clusters > 0 && first.waiting == 0);
    // ...then the detail arrives, matching the cut with everything loaded.
    const RenderStats near = s.settle(frame);
    GIANT_CHECK(near.triangles > first.triangles && near.resident_groups > far.resident_groups);
    geometry::LodView lod;
    lod.position = {0.0f, 0.0f, -8.0f};
    lod.scale = {10, 10, 10};
    lod.pixels_per_unit = 0.5f / std::tan(degrees_to_radians(frame.view.vertical_fov_degrees) * 0.5f) * 1080.0f;
    lod.near_plane = std::max(frame.view.near_plane, 1e-4f);
    lod.threshold_pixels = 1.0f;
    const Mat4 vp = Renderer::view_projection(frame.view, 1920.0f / 1080.0f);
    const geometry::Frustum frustum = geometry::frustum_from_view_projection(vp);
    lod.frustum = &frustum;
    std::vector<u32> reference;
    geometry::select_clusters(*sphere, lod, reference);
    GIANT_CHECK(near.clusters == reference.size());
}

GIANT_TEST("render: meshes can be added and removed") {
    NullScene s;
    if (s.renderer == nullptr) return;
    GIANT_CHECK(s.renderer->add_mesh(geometry::ClusterMesh{}) == k_no_mesh);
    const MeshId id = s.renderer->add_mesh(geometry::build_clusters(geometry::make_sphere(64, 32)));
    GIANT_CHECK(id != k_no_mesh && s.renderer->mesh(id) != nullptr);
    std::vector<Instance> instances(1);
    instances[0].mesh = id;
    instances[0].position = {0.0, 0.0, -5.0};
    FrameInput frame;
    frame.instances = instances;
    frame.settings.gpu_culling = false;
    GIANT_CHECK(s.draw(frame).draws == 2);
    s.renderer->remove_mesh(id);
    GIANT_CHECK(s.renderer->mesh(id) == nullptr);
    GIANT_CHECK(s.draw(frame).draws == 1); // a removed id draws nothing; shading still runs
    const MeshId next = s.renderer->add_mesh(geometry::build_clusters(geometry::make_cube()));
    GIANT_CHECK(next == id && s.renderer->mesh(next) != nullptr); // the slot is reused
    GIANT_CHECK(s.draw(frame).draws == 2);                       // so the old instance draws the new mesh
    // Built-in meshes cannot be removed.
    s.renderer->remove_mesh(s.renderer->builtin(BuiltinMesh::cube));
    GIANT_CHECK(s.renderer->mesh(s.renderer->builtin(BuiltinMesh::cube)) != nullptr);
}

GIANT_TEST("render: GPU culling records valid work on the null device") {
    // The null device checks every barrier, pipeline and argument buffer
    // (an assert fails the test); it cannot run shaders, so counts are 0.
    NullScene s;
    if (s.renderer == nullptr) return;
    std::vector<Instance> instances(3);
    instances[0].mesh = s.renderer->builtin(BuiltinMesh::sphere);
    instances[1].mesh = s.renderer->builtin(BuiltinMesh::cube);
    instances[2].mesh = s.renderer->add_mesh(geometry::build_clusters(geometry::make_sphere(64, 32)));
    for (usize i = 0; i < instances.size(); ++i) {
        instances[i].position = {static_cast<double>(i) * 3.0 - 3.0, 0.0, -10.0};
    }
    FrameInput frame;
    frame.instances = instances;
    for (const bool occlusion : {true, false}) {
        for (const bool mesh_shaders : {true, false}) {
            frame.settings.occlusion_culling = occlusion;
            frame.settings.mesh_shaders = mesh_shaders;
            for (int f = 0; f < 2; ++f) { // the second frame tests against the first's HZB
                const RenderStats stats = s.draw(frame);
                GIANT_CHECK(stats.from_gpu);
                // A raster pass per culling pass, then shading.
                GIANT_CHECK(stats.draws == (occlusion ? 3u : 2u));
                GIANT_CHECK(s.device->last_frame_stats().draws == stats.draws + stats.post_draws);
                GIANT_CHECK(s.device->last_frame_stats().dispatches > 4);
            }
        }
    }
    // Switching to the CPU and back.
    frame.settings.gpu_culling = false;
    GIANT_CHECK(!s.draw(frame).from_gpu);
    frame.settings.gpu_culling = true;
    GIANT_CHECK(s.draw(frame).from_gpu);
    // A removed mesh, an empty scene, and the GPU counts read after a wait.
    s.renderer->remove_mesh(instances[2].mesh);
    GIANT_CHECK(s.draw(frame).draws == 2); // occlusion is off after the loop
    frame.instances = {};
    GIANT_CHECK(s.draw(frame).draws == 1);
    s.device->wait_idle();
    s.renderer->read_gpu_stats();
    GIANT_CHECK(s.renderer->last_stats().clusters == 0); // the null device ran no shaders
}

GIANT_TEST("render: passes are timed on the GPU, in order") {
    NullScene s;
    if (s.renderer == nullptr) return;
    std::vector<Instance> instances(1);
    instances[0].mesh = s.renderer->builtin(BuiltinMesh::sphere);
    instances[0].position = {0.0, 0.0, -5.0};
    FrameInput frame;
    frame.instances = instances;
    const auto names = [&s] {
        std::vector<std::string> out;
        for (const rhi::GpuTiming& t : s.device->gpu_timings()) out.push_back(t.name);
        return out;
    };
    frame.settings.gpu_culling = false;
    s.draw(frame);
    GIANT_CHECK(names() == (std::vector<std::string>{"streaming", "voxels", "gi", "raster", "shading", "anti-aliasing"}));
    frame.settings.gpu_culling = true;
    frame.settings.occlusion_culling = true;
    frame.settings.antialiasing = false;
    s.draw(frame);
    GIANT_CHECK(names() == (std::vector<std::string>{"streaming", "voxels", "gi", "culling", "raster", "HZB", "culling",
                                                     "raster", "HZB", "shadows", "shading"}));
}

GIANT_TEST("render: reflections trace voxel cones with gi, and the screen in a pass after shading") {
    NullScene s;
    GIANT_CHECK(s.renderer != nullptr);
    if (s.renderer == nullptr) return;
    std::vector<Instance> instances(2);
    instances[0].mesh = s.renderer->builtin(BuiltinMesh::plane);
    instances[0].scale = {20.0f, 1.0f, 20.0f};
    instances[1].mesh = s.renderer->builtin(BuiltinMesh::cube);
    instances[1].position = {0.0, 1.0, -5.0};
    FrameInput frame;
    frame.instances = instances;
    frame.view.position = {0.0, 1.5, 0.0};
    frame.settings.antialiasing = false;
    RenderStats st = s.draw(frame);
    GIANT_CHECK(st.reflections && st.screen_reflections);
    // Shading, then the pass that reflects and tone maps its image.
    GIANT_CHECK(st.post_draws == 1);
    GIANT_CHECK(s.device->last_frame_stats().draws == st.draws + st.post_draws + st.shadow_draws);
    // Without global illumination, only the screen's.
    frame.settings.gi = false;
    st = s.draw(frame);
    GIANT_CHECK(!st.reflections && st.screen_reflections);
    frame.settings.gi = true;
    frame.settings.screen_space_reflections = false;
    st = s.draw(frame);
    GIANT_CHECK(st.reflections && !st.screen_reflections && st.post_draws == 0); // tone mapped in shading
    frame.settings.screen_space_reflections = true;
    frame.settings.debug_view = DebugView::lighting;
    st = s.draw(frame);
    GIANT_CHECK(st.reflections && st.screen_reflections);
    // Views of the geometry have none.
    frame.settings.debug_view = DebugView::clusters;
    st = s.draw(frame);
    GIANT_CHECK(!st.reflections && !st.screen_reflections && st.post_draws == 0);
    frame.settings.debug_view = DebugView::voxels;
    st = s.draw(frame);
    GIANT_CHECK(!st.reflections && !st.screen_reflections && st.post_draws == 0);
    frame.settings.debug_view = DebugView::lit;
    frame.settings.reflections = false;
    st = s.draw(frame);
    GIANT_CHECK(!st.reflections && !st.screen_reflections);
}

GIANT_TEST("render: shadow pages asked for are drawn, a pass per level, and cached") {
    // The null device runs no shaders, so shading asks for nothing: the
    // test asks for pages itself, as the read-back requests would.
    NullScene s;
    if (s.renderer == nullptr) return;
    std::vector<Instance> instances(2);
    instances[0].mesh = s.renderer->builtin(BuiltinMesh::plane);
    instances[0].scale = {40.0f, 1.0f, 40.0f};
    instances[1].mesh = s.renderer->builtin(BuiltinMesh::cube);
    instances[1].position = {0.0, 1.0, -4.0};
    FrameInput frame;
    frame.instances = instances;
    frame.view.position = {0.0, 2.0, 0.0};
    frame.lighting.sun_direction = {0.3f, 0.9f, 0.2f};
    frame.settings.antialiasing = false;
    RenderStats st = s.draw(frame);
    GIANT_CHECK(st.shadows && st.shadow_capacity == 1024);
    GIANT_CHECK(st.shadow_pages == 0 && st.shadow_draws == 0);
    ShadowPages& pages = s.renderer->shadow_pages();
    const auto ask = [&](u32 level, const DVec3& world) {
        std::vector<u32> bits(pages.levels() * 8, 0u);
        const DVec3 l = pages.to_light(world);
        const double size = pages.page_size(level);
        const u32 slot = pages.slot_of(i32(std::floor(l.x / size)), i32(std::floor(l.y / size)));
        bits[level * 8 + slot / 32] |= 1u << (slot % 32);
        pages.request(bits);
    };
    ask(0, instances[1].position);
    ask(0, instances[1].position + DVec3{3.0, 0.0, 0.0});
    ask(4, instances[1].position);
    st = s.draw(frame);
    GIANT_CHECK(st.shadow_pages == 3 && st.shadow_drawn == 3 && st.shadow_waiting == 0);
    GIANT_CHECK(st.shadow_draws == 2); // levels 0 and 4
    GIANT_CHECK(st.draws == 3);        // the view's two raster passes and shading: shadows apart
    GIANT_CHECK(s.device->last_frame_stats().draws == st.draws + st.post_draws + st.shadow_draws);
    // Cached: nothing to draw while nothing changes.
    st = s.draw(frame);
    GIANT_CHECK(st.shadow_drawn == 0 && st.shadow_draws == 0 && st.shadow_pages == 3);
    // The cube moves: the pages under it are drawn again.
    instances[1].position.x += 0.5;
    st = s.draw(frame);
    GIANT_CHECK(st.shadow_drawn >= 2 && st.shadow_draws == 2);
    // The budget: one page a frame (coarse first), the rest wait.
    frame.settings.shadow_pages_per_frame = 1;
    pages.invalidate_all();
    st = s.draw(frame);
    GIANT_CHECK(st.shadow_drawn == 1 && st.shadow_waiting == 2 && st.shadow_draws == 1);
    // Off: nothing drawn, nothing asked; back on, the pages are drawn again.
    frame.settings.shadows = false;
    st = s.draw(frame);
    GIANT_CHECK(!st.shadows && st.shadow_draws == 0);
    frame.settings.shadows = true;
    frame.settings.shadow_pages_per_frame = 64;
    st = s.draw(frame);
    GIANT_CHECK(st.shadows && st.shadow_drawn == 3);
    // The CPU path and the debug views have no shadows.
    frame.settings.gpu_culling = false;
    GIANT_CHECK(!s.draw(frame).shadows);
    frame.settings.gpu_culling = true;
    frame.settings.debug_view = DebugView::clusters;
    GIANT_CHECK(!s.draw(frame).shadows);
}

GIANT_TEST("render: many meshes added in one frame draw once their coarsest levels are in") {
    // A per-frame budget of a few meshes: the rest wait, undrawn, on both
    // culling paths, and every mesh draws a few frames later.
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    auto device = rhi::create_device(desc);
    RendererDesc renderer_desc;
    renderer_desc.stream_bytes_per_frame = 8 * 1024;
    auto renderer = Renderer::create(*device, renderer_desc);
    GIANT_CHECK(renderer != nullptr);
    if (renderer == nullptr) return;
    rhi::TextureDesc t;
    t.width = 320;
    t.height = 180;
    t.format = rhi::Format::rgba8_srgb;
    t.usage = rhi::TextureUsage::render_target;
    const rhi::TextureHandle target = device->create_texture(t);
    const auto draw = [&](const FrameInput& frame) {
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        renderer->render(*cmd, target, frame);
        device->end_frame();
        return renderer->last_stats();
    };
    FrameInput frame;
    frame.settings.gpu_culling = false; // exact counts on the CPU
    draw(frame); // the built-in meshes' coarsest levels

    constexpr u32 k_meshes = 48;
    std::vector<Instance> instances(k_meshes);
    rhi::CommandList* cmd = device->begin_frame(nullptr);
    for (u32 i = 0; i < k_meshes; ++i) { // added inside a frame: the staged, unwaited path
        instances[i].mesh = renderer->add_mesh(geometry::build_clusters(geometry::make_sphere(96, 48)));
        GIANT_CHECK(instances[i].mesh != k_no_mesh);
        instances[i].position = {static_cast<double>(i % 8) * 2.5 - 9.0, static_cast<double>(i / 8) * 2.5 - 6.0,
                                 -30.0};
    }
    renderer->render(*cmd, target, frame);
    device->end_frame();
    GIANT_CHECK(device->last_frame_stats().upload_bytes > 0); // the meshes' metadata, staged
    frame.instances = instances;

    u32 drawn = 0;
    u32 frames = 0;
    bool within = true;
    while (frames < 200) {
        const RenderStats stats = draw(frame);
        ++frames;
        GIANT_CHECK(stats.instances >= drawn); // never back
        drawn = stats.instances;
        within = within && device->last_frame_stats().transient_bytes < 8u << 20;
        if (drawn == k_meshes && stats.waiting == 0) break;
    }
    GIANT_CHECK(within);
    GIANT_CHECK(drawn == k_meshes);
    GIANT_CHECK(frames > 3); // spread over frames, not all at once
    // The GPU path draws only meshes that are in, too (the null device
    // checks what it records).
    frame.settings.gpu_culling = true;
    GIANT_CHECK(draw(frame).from_gpu);
    device->destroy(target);
}

#if GIANT_RHI_HAS_D3D12
GIANT_TEST("render: d3d12 lights a scene on the software adapter") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::d3d12;
    desc.software_adapter = true;
    desc.debug_layer = true;
    auto device = rhi::create_device(desc);
    GIANT_CHECK(device != nullptr);
    if (device == nullptr) {
        return;
    }
#if defined(GIANT_REQUIRE_VALIDATION)
    GIANT_CHECK(device->validation_enabled());
#endif
    auto renderer = Renderer::create(*device);
    GIANT_CHECK(renderer != nullptr);
    if (renderer == nullptr) {
        return;
    }
    constexpr u32 k_w = 96, k_h = 64;
    rhi::TextureDesc target_desc;
    target_desc.width = k_w;
    target_desc.height = k_h;
    target_desc.format = rhi::Format::rgba8_srgb;
    target_desc.usage = rhi::TextureUsage::render_target;
    const rhi::TextureHandle target = device->create_texture(target_desc);

    // Far from the origin on purpose: camera-relative rendering keeps it exact.
    const DVec3 camera{250'000.0, 10.0, -125'000.0};
    std::vector<Instance> instances(2);
    // A white sphere 3 m ahead, lit from the right (+X).
    instances[0].mesh = renderer->builtin(BuiltinMesh::sphere);
    instances[0].position = camera + DVec3{0.0, 0.0, -3.0};
    instances[0].scale = {2.0f, 2.0f, 2.0f};
    std::vector<Material> materials(2);
    materials[0].base_color = {0.9f, 0.9f, 0.9f};
    materials[0].roughness = 0.6f;
    instances[0].material = 0;
    // A green emissive cube behind it, partly hidden: depth testing.
    instances[1].mesh = renderer->builtin(BuiltinMesh::cube);
    instances[1].position = camera + DVec3{0.0, 0.0, -8.0};
    instances[1].scale = {20.0f, 0.5f, 1.0f};
    materials[1].base_color = {0.0f, 0.0f, 0.0f};
    materials[1].emissive = {0.0f, 0.6f, 0.0f};
    instances[1].material = 1;

    FrameInput frame;
    frame.view.position = camera;
    frame.instances = instances;
    frame.materials = materials;
    frame.lighting.sun_direction = {1.0f, 0.0f, 0.0f};
    frame.lighting.sun_light = {3.0f, 3.0f, 3.0f};
    frame.lighting.sky_light = {0.02f, 0.02f, 0.02f};
    frame.lighting.ground_light = {0.02f, 0.02f, 0.02f};
    frame.lighting.background = {0.0f, 0.0f, 1.0f};
    frame.settings.shadows = false; // the CPU path has none: compared below
    frame.settings.gi = false;      // checked apart (it lights over many frames)

    // Renders frames until streaming settles (occlusion uses the last frame's depth) and reads
    // the image back, with the GPU's counts for the last frame.
    const auto draw = [&](bool gpu, bool occlusion, bool mesh) {
        frame.settings.gpu_culling = gpu;
        frame.settings.occlusion_culling = occlusion;
        frame.settings.mesh_shaders = mesh;
        // Enough frames for streaming to load what the view asks for: GPU
        // requests reach the CPU two frames late. Done once nothing has
        // loaded or waited for a few frames in a row.
        int quiet = 0;
        for (int i = 0; i < 64 && (i < 3 || quiet < 4); ++i) {
            rhi::CommandList* cmd = device->begin_frame(nullptr);
            GIANT_CHECK(cmd != nullptr);
            if (cmd != nullptr) {
                renderer->render(*cmd, target, frame);
                device->end_frame();
            }
            const RenderStats& st = renderer->last_stats();
            quiet = st.streamed_in == 0 && st.waiting == 0 ? quiet + 1 : 0;
        }
        device->wait_idle();
        renderer->read_gpu_stats();
        return device->read_texture(target);
    };
    const auto pixels = draw(false, false, false);
    const RenderStats cpu = renderer->last_stats();
    GIANT_CHECK(cpu.draws == 2 && !cpu.from_gpu && cpu.instances == 2);
    GIANT_CHECK(pixels.size() == k_w * k_h * 4);
    if (pixels.size() == k_w * k_h * 4) {
        const auto at = [&pixels](u32 x, u32 y) {
            const usize i = (static_cast<usize>(y) * k_w + x) * 4;
            return std::array<int, 4>{pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]};
        };
        // Sphere: about 18 degrees wide each side of the centre at this fov.
        const auto lit = at(k_w / 2 + 12, k_h / 2);
        const auto dark = at(k_w / 2 - 12, k_h / 2);
        GIANT_CHECK(lit[0] > 150 && lit[0] == lit[1]);  // grey, brightly lit
        GIANT_CHECK(dark[0] < 60);                       // facing away from the sun
        const auto bar = at(4, k_h / 2);                 // the cube, beside the sphere
        GIANT_CHECK(bar[1] > 100 && bar[0] < 30 && bar[2] < 30);
        const auto sky = at(2, 2);
        GIANT_CHECK(sky[0] == 0 && sky[1] == 0 && sky[2] == 255);
    }
    // Every GPU path draws the same picture as the CPU path.
    const auto differing = [&pixels](const std::vector<u8>& other) {
        usize n = 0;
        for (usize i = 0; i < pixels.size() && i < other.size(); ++i) {
            n += std::abs(int{pixels[i]} - int{other[i]}) > 2 ? 1 : 0;
        }
        return other.size() == pixels.size() ? n : pixels.size();
    };
    const bool mesh_shaders = device->adapter().mesh_shaders;
    for (const bool mesh : {false, true}) {
        if (mesh && !mesh_shaders) {
            continue; // WARP may lack mesh shaders: the vertex path stands in
        }
        const auto plain = draw(true, false, mesh);
        const RenderStats g = renderer->last_stats();
        GIANT_CHECK(g.from_gpu && !g.overflow);
        GIANT_CHECK(g.instances == cpu.instances);
        // The same cut, give or take a group on a float rounding edge.
        GIANT_CHECK(std::abs(int(g.clusters) - int(cpu.clusters)) <= int(cpu.clusters / 20 + 1));
        GIANT_CHECK(std::abs(int(g.triangles) - int(cpu.triangles)) <= int(cpu.triangles / 20 + 1));
        GIANT_CHECK(g.draws == 2);
        GIANT_CHECK(differing(plain) <= pixels.size() / 200);
        const auto culled = draw(true, true, mesh);
        const RenderStats o = renderer->last_stats();
        GIANT_CHECK(o.from_gpu && !o.overflow && o.draws == 3);
        GIANT_CHECK(o.clusters <= g.clusters && o.clusters > 0);
        GIANT_CHECK(differing(culled) <= pixels.size() / 200); // nothing visible lost
        // The GPU timed each pass of the newest frame (read after the wait).
        const auto timings = device->gpu_timings();
        GIANT_CHECK(timings.size() == 10); // streaming, voxels, 2 x (culling, raster, HZB), shading, AA
        bool timed = true;
        for (const rhi::GpuTiming& t : timings) {
            timed = timed && t.ms >= 0.0f && t.ms <= device->gpu_frame_ms() + 1e-4f;
        }
        GIANT_CHECK(timed && device->gpu_frame_ms() > 0.0f);
        GIANT_CHECK(timings.size() == 10 && timings[2].name == "culling" && timings[8].name == "shading");
    }
    // A glowing red decal stamped on the sphere's front, facing the camera
    // (its +Y turned to +Z), on the CPU and GPU paths alike.
    std::vector<Decal> decals(1);
    decals[0].position = camera + DVec3{0.0, 0.0, -2.0};
    decals[0].rotation = Quat{0.70710678f, 0.0f, 0.0f, 0.70710678f};
    decals[0].size = {0.6f, 1.0f, 0.6f};
    decals[0].base_color = {0.0f, 0.0f, 0.0f};
    decals[0].emissive = {1.0f, 0.0f, 0.0f};
    frame.decals = decals;
    for (const bool gpu : {false, true}) {
        const auto stamped = draw(gpu, false, false);
        GIANT_CHECK(renderer->last_stats().decals == 1);
        if (stamped.size() == k_w * k_h * 4) {
            const usize centre = (static_cast<usize>(k_h / 2) * k_w + k_w / 2) * 4;
            GIANT_CHECK(stamped[centre] > 150 && stamped[centre + 1] < 40);
            const usize lit = (static_cast<usize>(k_h / 2) * k_w + k_w / 2 + 12) * 4;
            GIANT_CHECK(std::abs(int{stamped[lit]} - int{pixels[lit]}) <= 2); // outside the stamp: unchanged
        }
    }
    // Anti-aliasing (on by default) only softens edges: with it off the
    // silhouettes are hard steps, and every pixel SMAA changed sits on one.
    frame.decals = {};
    frame.settings.antialiasing = false;
    const auto aliased = draw(false, false, false);
    GIANT_CHECK(renderer->last_stats().post_draws == 1); // the screen reflections pass alone
    frame.settings.antialiasing = true;
    const auto smoothed = draw(false, false, false);
    GIANT_CHECK(renderer->last_stats().post_draws == 4);
    if (aliased.size() == k_w * k_h * 4 && smoothed.size() == aliased.size()) {
        const auto luma = [](const std::vector<u8>& p, u32 x, u32 y) {
            const usize i = (static_cast<usize>(y) * k_w + x) * 4;
            return 0.2126 * p[i] + 0.7152 * p[i + 1] + 0.0722 * p[i + 2];
        };
        usize changed = 0;
        usize off_edge = 0;
        for (u32 y = 1; y + 1 < k_h; ++y) {
            for (u32 x = 1; x + 1 < k_w; ++x) {
                const usize i = (static_cast<usize>(y) * k_w + x) * 4;
                bool differs = false;
                for (usize c = 0; c < 3; ++c) {
                    differs = differs || std::abs(int{aliased[i + c]} - int{smoothed[i + c]}) > 2;
                }
                if (!differs) {
                    continue;
                }
                ++changed;
                const double l = luma(aliased, x, y);
                const double step = std::max({std::abs(l - luma(aliased, x - 1, y)), std::abs(l - luma(aliased, x + 1, y)),
                                              std::abs(l - luma(aliased, x, y - 1)), std::abs(l - luma(aliased, x, y + 1))});
                off_edge += step < 8.0 ? 1 : 0;
            }
        }
        GIANT_CHECK(changed > 10);                     // the silhouettes were smoothed
        GIANT_CHECK(changed < aliased.size() / 4 / 8); // ...and only they
        GIANT_CHECK(off_edge == 0);
        const usize sky = (2 * k_w + 2) * 4;
        GIANT_CHECK(smoothed[sky] == aliased[sky] && smoothed[sky + 2] == aliased[sky + 2]);
    }
    // Specular anti-aliasing only widens highlights where normals bend
    // faster than a pixel shows: on this large smooth sphere, barely at all.
    frame.settings.specular_antialiasing = false;
    const auto unfiltered = draw(false, false, false);
    frame.settings.specular_antialiasing = true;
    GIANT_CHECK(differing(unfiltered) <= pixels.size() / 50);
    // A small, mirror-like sphere a few pixels wide: its normals turn fast
    // across each pixel, so the filter must widen its highlight (here it
    // lights pixels whose centres the sharp highlight missed).
    std::vector<Instance> shiny(1);
    shiny[0].mesh = renderer->builtin(BuiltinMesh::sphere);
    shiny[0].position = camera + DVec3{0.0, 0.0, -12.0};
    shiny[0].scale = {0.8f, 0.8f, 0.8f};
    std::vector<Material> mirror(1);
    mirror[0].base_color = {0.05f, 0.05f, 0.05f};
    mirror[0].roughness = 0.05f;
    mirror[0].metallic = 1.0f;
    frame.instances = shiny;
    frame.materials = mirror;
    frame.lighting.sun_direction = {0.0f, 0.0f, 1.0f}; // behind the camera: the highlight faces it
    frame.settings.specular_antialiasing = false;
    const auto sharp = draw(false, false, false);
    frame.settings.specular_antialiasing = true;
    const auto filtered = draw(false, false, false);
    usize widened = 0;
    for (usize i = 0; i < sharp.size() && i < filtered.size(); ++i) {
        widened += std::abs(int{sharp[i]} - int{filtered[i]}) > 2 ? 1 : 0;
    }
    GIANT_CHECK(widened > 0);
    device->destroy(target);
    renderer.reset();
    device->wait_idle();
    GIANT_CHECK(!device->device_lost());
    GIANT_CHECK(device->validation_errors() == 0);
}
#endif

GIANT_TEST("render: d3d12 casts sun shadows from shadow pages") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::d3d12;
    desc.software_adapter = true;
    desc.debug_layer = true;
    auto device = rhi::create_device(desc);
    if (device == nullptr) {
        return; // not on Windows
    }
    auto renderer = Renderer::create(*device);
    GIANT_CHECK(renderer != nullptr);
    if (renderer == nullptr) {
        return;
    }
    constexpr u32 k_w = 96, k_h = 64;
    rhi::TextureDesc target_desc;
    target_desc.width = k_w;
    target_desc.height = k_h;
    target_desc.format = rhi::Format::rgba8_srgb;
    target_desc.usage = rhi::TextureUsage::render_target;
    const rhi::TextureHandle target = device->create_texture(target_desc);

    // Ground, and a cube floating over it; the camera 6 m up looking
    // straight down (screen right is +X, screen up is -Z), far from the origin.
    const DVec3 origin{-40'000.0, 120.0, 70'000.0};
    std::vector<Instance> instances(2);
    instances[0].mesh = renderer->builtin(BuiltinMesh::plane);
    instances[0].position = origin;
    instances[0].scale = {40.0f, 1.0f, 40.0f};
    instances[1].mesh = renderer->builtin(BuiltinMesh::cube);
    instances[1].position = origin + DVec3{0.0, 1.5, 0.0};
    std::vector<Material> materials(1);
    materials[0].base_color = {0.7f, 0.7f, 0.7f};
    materials[0].roughness = 0.9f;
    FrameInput frame;
    frame.view.position = origin + DVec3{0.0, 6.0, 0.0};
    frame.view.orientation = quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -k_pi * 0.5f);
    frame.instances = instances;
    frame.materials = materials;
    // The sun to the +X side: the cube's shadow falls towards -X, from
    // about x = -2 to -0.25 (the cube hides x in [-0.75, 0.75] from above).
    frame.lighting.sun_direction = normalize_or_zero(Vec3{0.6f, 0.8f, 0.0f});
    frame.lighting.sun_light = {3.0f, 3.0f, 3.0f};
    frame.lighting.sky_light = {0.1f, 0.1f, 0.1f};
    frame.lighting.ground_light = {0.1f, 0.1f, 0.1f};
    frame.settings.antialiasing = false;
    frame.settings.gi = false; // the light here is the sun's alone (gi is tested below)
    // Shading asks for pages; they are read back two frames on and drawn.
    const auto draw = [&](bool shadows) {
        frame.settings.shadows = shadows;
        for (int i = 0; i < 16; ++i) {
            rhi::CommandList* cmd = device->begin_frame(nullptr);
            GIANT_CHECK(cmd != nullptr);
            if (cmd != nullptr) {
                renderer->render(*cmd, target, frame);
                device->end_frame();
            }
        }
        device->wait_idle();
        return device->read_texture(target);
    };
    const auto plain = draw(false);
    const auto shadowed = draw(true);
    const RenderStats st = renderer->last_stats();
    GIANT_CHECK(st.shadows && st.shadow_pages > 0 && st.shadow_waiting == 0 && st.shadow_missing == 0);
    GIANT_CHECK(plain.size() == k_w * k_h * 4 && shadowed.size() == plain.size());
    if (plain.size() == k_w * k_h * 4 && shadowed.size() == plain.size()) {
        const auto at = [](const std::vector<u8>& p, u32 x, u32 y) { return int{p[(static_cast<usize>(y) * k_w + x) * 4]}; };
        // About 0.108 m a pixel on the ground; the centre is x = 0.
        const u32 shade_x = k_w / 2 - 13; // x = -1.4 m: in the shadow
        const u32 open_x = k_w / 2 + 16;  // x = +1.7 m: in the sun
        GIANT_CHECK(at(shadowed, shade_x, k_h / 2) < at(plain, shade_x, k_h / 2) / 2);
        // No shadow acne: open ground is as bright as without shadows.
        for (u32 y = 4; y < k_h - 4; y += 6) {
            GIANT_CHECK(std::abs(at(shadowed, open_x, y) - at(plain, open_x, y)) <= 3);
            GIANT_CHECK(std::abs(at(shadowed, k_w - 3, y) - at(plain, k_w - 3, y)) <= 3);
        }
        // The cube's lit top is unchanged too.
        GIANT_CHECK(std::abs(at(shadowed, k_w / 2, k_h / 2) - at(plain, k_w / 2, k_h / 2)) <= 3);
    }
    // Moving the cube moves its shadow (its pages are drawn again).
    instances[1].position.z += 3.0; // 28 pixels down the screen
    const auto moved = draw(true);
    if (moved.size() == k_w * k_h * 4 && plain.size() == moved.size()) {
        const auto at = [](const std::vector<u8>& p, u32 x, u32 y) { return int{p[(static_cast<usize>(y) * k_w + x) * 4]}; };
        const u32 shade_x = k_w / 2 - 13;
        GIANT_CHECK(std::abs(at(moved, shade_x, k_h / 2) - at(plain, shade_x, k_h / 2)) <= 3); // sunlit again
    }
    GIANT_CHECK(!device->device_lost());
    GIANT_CHECK(device->validation_errors() == 0);
    device->destroy(target);
}

GIANT_TEST("render: d3d12 global illumination bounces colour and shuts out the sky") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::d3d12;
    desc.software_adapter = true;
    desc.debug_layer = true;
    auto device = rhi::create_device(desc);
    if (device == nullptr) {
        return; // not on Windows
    }
    // A small voxel world, so the software GPU lights it quickly.
    RendererDesc rd;
    rd.voxel_clipmap.levels = 6;
    rd.voxel_clipmap.bricks_per_axis = 16;
    rd.voxel_bricks = 4096;
    auto renderer = Renderer::create(*device, rd);
    GIANT_CHECK(renderer != nullptr);
    if (renderer == nullptr) {
        return;
    }
    constexpr u32 k_w = 96, k_h = 64;
    rhi::TextureDesc target_desc;
    target_desc.width = k_w;
    target_desc.height = k_h;
    target_desc.format = rhi::Format::rgba8_srgb;
    target_desc.usage = rhi::TextureUsage::render_target;
    const rhi::TextureHandle target = device->create_texture(target_desc);

    // White ground and a red wall standing on it (x from 1.8 to 2.2 m, 2 m
    // tall); the camera 6 m up looking straight down (screen right is +X),
    // the sun low from -X: it lights the wall's -X face and the ground
    // before it, and the wall shades the ground behind it out to x = 4.9.
    const DVec3 origin{-40'000.0, 120.0, 70'000.0};
    std::vector<Instance> instances(2);
    instances[0].mesh = renderer->builtin(BuiltinMesh::plane);
    instances[0].position = origin;
    instances[0].scale = {12.0f, 1.0f, 12.0f};
    instances[0].material = 0;
    instances[1].mesh = renderer->builtin(BuiltinMesh::cube);
    instances[1].position = origin + DVec3{2.0, 1.0, 0.0};
    instances[1].scale = {0.4f, 2.0f, 8.0f};
    instances[1].material = 1;
    std::vector<Material> materials(2);
    materials[0].base_color = {0.8f, 0.8f, 0.8f};
    materials[0].roughness = 0.9f;
    materials[1].base_color = {0.9f, 0.05f, 0.05f};
    materials[1].roughness = 0.9f;
    FrameInput frame;
    frame.view.position = origin + DVec3{0.0, 6.0, 0.0};
    frame.view.orientation = quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -k_pi * 0.5f);
    frame.instances = instances;
    frame.materials = materials;
    frame.lighting.sun_direction = normalize_or_zero(Vec3{-0.8f, 0.6f, 0.0f});
    frame.lighting.sun_light = {3.0f, 3.0f, 3.0f};
    frame.lighting.sky_light = {0.4f, 0.4f, 0.4f};
    frame.lighting.ground_light = {0.4f, 0.4f, 0.4f};
    frame.settings.antialiasing = false;
    frame.settings.voxel_ms_per_frame = 1000.0f;
    frame.settings.gi_bricks_per_frame = 4096;
    const auto draw = [&](bool gi) {
        frame.settings.gi = gi;
        for (int i = 0; i < 12; ++i) {
            rhi::CommandList* cmd = device->begin_frame(nullptr);
            GIANT_CHECK(cmd != nullptr);
            if (cmd != nullptr) {
                renderer->render(*cmd, target, frame);
                device->end_frame();
            }
        }
        device->wait_idle();
        return device->read_texture(target);
    };
    const auto flat = draw(false);
    const auto lit = draw(true);
    const RenderStats st = renderer->last_stats();
    GIANT_CHECK(st.gi && st.gi_lit > 0 && st.gi_waiting == 0 && st.voxel_pending == 0);
    GIANT_CHECK(flat.size() == k_w * k_h * 4 && lit.size() == flat.size());
    if (flat.size() == k_w * k_h * 4 && lit.size() == flat.size()) {
        const auto at = [](const std::vector<u8>& p, u32 x, u32 y, u32 c) {
            return int{p[(static_cast<usize>(y) * k_w + x) * 4 + c]};
        };
        // About 0.108 m a pixel on the ground; the centre is x = 0.
        const auto column = [](double x) { return static_cast<u32>(static_cast<int>(k_w / 2) + static_cast<int>(x / 0.108)); };
        const u32 y = k_h / 2;
        // Before the wall (x = 1.2): red light bounced off its lit face.
        const u32 before = column(1.2);
        GIANT_CHECK(at(flat, before, y, 0) == at(flat, before, y, 1)); // grey without
        GIANT_CHECK(at(lit, before, y, 0) > at(lit, before, y, 1) + 6);
        // Behind it, in its shadow (x = 3.5): the wall hides much of the sky.
        const u32 behind = column(3.5);
        GIANT_CHECK(at(lit, behind, y, 1) < at(flat, behind, y, 1) * 19 / 20);
        // Open ground far from the wall (x = -4): about the same either way.
        const u32 open = column(-4.0);
        GIANT_CHECK(std::abs(at(lit, open, y, 1) - at(flat, open, y, 1)) <= at(flat, open, y, 1) / 8 + 2);
    }
    // The light alone: plain white-grey surfaces, open ground grey and lit.
    frame.settings.debug_view = DebugView::lighting;
    const auto light_only = draw(true);
    if (light_only.size() == k_w * k_h * 4) {
        const usize i = (static_cast<usize>(k_h / 2) * k_w + (k_w / 2 - 37)) * 4; // x = -4: open ground
        // Grey and lit (green and blue alike: the wall's bounce may add a
        // little red even this far off, its cones being wide).
        std::printf("lighting view at x = -4: %d %d %d\n", int{light_only[i]}, int{light_only[i + 1]},
                    int{light_only[i + 2]});
        GIANT_CHECK(light_only[i + 1] > 60 && std::abs(int{light_only[i + 1]} - int{light_only[i + 2]}) <= 3);
        GIANT_CHECK(light_only[i] >= light_only[i + 1]);
    }
    GIANT_CHECK(!device->device_lost());
    GIANT_CHECK(device->validation_errors() == 0);
    device->destroy(target);
}

GIANT_TEST("render: d3d12 reflects a block in a mirror floor, from the voxels and the screen") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::d3d12;
    desc.software_adapter = true;
    desc.debug_layer = true;
    auto device = rhi::create_device(desc);
    if (device == nullptr) {
        return; // not on Windows
    }
    RendererDesc rd;
    rd.voxel_clipmap.levels = 6;
    rd.voxel_clipmap.bricks_per_axis = 16;
    rd.voxel_bricks = 4096;
    auto renderer = Renderer::create(*device, rd);
    GIANT_CHECK(renderer != nullptr);
    if (renderer == nullptr) {
        return;
    }
    constexpr u32 k_w = 96, k_h = 64;
    rhi::TextureDesc target_desc;
    target_desc.width = k_w;
    target_desc.height = k_h;
    target_desc.format = rhi::Format::rgba8_srgb;
    target_desc.usage = rhi::TextureUsage::render_target;
    const rhi::TextureHandle target = device->create_texture(target_desc);

    // A mirror-smooth metal floor and a green block on it, 5 to 7 m ahead
    // (-Z); the camera 1.5 m up, looking 15 degrees down; the sun behind
    // and above it lights the block's face. The floor between 2.1 and 5 m
    // ahead shows the block's reflection: at the screen's centre column,
    // rows 34 to 52 or so (row 44: 2.9 m ahead, the block 1.1 m up).
    const DVec3 origin{25'000.0, 40.0, -60'000.0};
    std::vector<Instance> instances(2);
    instances[0].mesh = renderer->builtin(BuiltinMesh::plane);
    instances[0].position = origin;
    instances[0].scale = {20.0f, 1.0f, 20.0f};
    instances[0].material = 0;
    instances[1].mesh = renderer->builtin(BuiltinMesh::cube);
    instances[1].position = origin + DVec3{0.0, 1.0, -6.0};
    instances[1].scale = {2.0f, 2.0f, 2.0f};
    instances[1].material = 1;
    std::vector<Material> materials(2);
    materials[0].base_color = {0.9f, 0.9f, 0.9f};
    materials[0].roughness = 0.05f;
    materials[0].metallic = 1.0f;
    materials[1].base_color = {0.05f, 0.8f, 0.05f};
    materials[1].roughness = 0.9f;
    FrameInput frame;
    frame.view.position = origin + DVec3{0.0, 1.5, 0.0};
    frame.view.orientation = quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -k_pi / 12.0f);
    frame.instances = instances;
    frame.materials = materials;
    frame.lighting.sun_direction = normalize_or_zero(Vec3{0.0f, 0.7f, 0.7f});
    frame.lighting.sun_light = {3.0f, 3.0f, 3.0f};
    frame.lighting.sky_light = {0.4f, 0.4f, 0.4f};
    frame.lighting.ground_light = {0.4f, 0.4f, 0.4f};
    frame.settings.antialiasing = false;
    frame.settings.voxel_ms_per_frame = 1000.0f;
    frame.settings.gi_bricks_per_frame = 4096;
    const auto draw = [&] {
        for (int i = 0; i < 12; ++i) {
            rhi::CommandList* cmd = device->begin_frame(nullptr);
            GIANT_CHECK(cmd != nullptr);
            if (cmd != nullptr) {
                renderer->render(*cmd, target, frame);
                device->end_frame();
            }
        }
        device->wait_idle();
        return device->read_texture(target);
    };
    frame.settings.reflections = false;
    const auto off = draw();
    frame.settings.reflections = true;
    frame.settings.screen_space_reflections = false;
    const auto voxel = draw();
    GIANT_CHECK(renderer->last_stats().reflections && !renderer->last_stats().screen_reflections);
    frame.settings.screen_space_reflections = true;
    const auto both = draw();
    GIANT_CHECK(renderer->last_stats().reflections && renderer->last_stats().screen_reflections);
    // The screen alone (no voxel world to trace).
    frame.settings.gi = false;
    const auto screen = draw();
    GIANT_CHECK(!renderer->last_stats().reflections && renderer->last_stats().screen_reflections);
    // Moving: the screen's reflection is this frame's, wherever the camera
    // is (one frame drawn after the move).
    frame.view.position = frame.view.position + DVec3{0.4, 0.0, 0.0};
    const auto moved = [&] {
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        GIANT_CHECK(cmd != nullptr);
        if (cmd != nullptr) {
            renderer->render(*cmd, target, frame);
            device->end_frame();
        }
        device->wait_idle();
        return device->read_texture(target);
    }();
    const usize size = usize{k_w} * k_h * 4;
    GIANT_CHECK(off.size() == size && voxel.size() == size && both.size() == size && screen.size() == size &&
                moved.size() == size);
    if (off.size() == size && voxel.size() == size && both.size() == size && screen.size() == size &&
        moved.size() == size) {
        const auto px = [](const std::vector<u8>& p, u32 y, u32 c) {
            return int{p[(static_cast<usize>(y) * k_w + k_w / 2) * 4 + c]};
        };
        for (const u32 y : {40u, 44u, 48u}) {
            std::printf("mirror floor row %u: off %d %d %d, voxel %d %d %d, both %d %d %d, screen %d %d %d\n", y,
                        px(off, y, 0), px(off, y, 1), px(off, y, 2), px(voxel, y, 0), px(voxel, y, 1),
                        px(voxel, y, 2), px(both, y, 0), px(both, y, 1), px(both, y, 2), px(screen, y, 0),
                        px(screen, y, 1), px(screen, y, 2));
        }
        const u32 y = 44;
        // Without reflections the floor shows the grey sky; with them, the
        // green block.
        GIANT_CHECK(std::abs(px(off, y, 1) - px(off, y, 0)) <= 3);
        GIANT_CHECK(px(voxel, y, 1) > px(voxel, y, 0) + 10);
        GIANT_CHECK(px(both, y, 1) > px(both, y, 0) + 10);
        GIANT_CHECK(px(screen, y, 1) > px(screen, y, 0) + 10);
        GIANT_CHECK(px(moved, y, 1) > px(moved, y, 0) + 10);
        // Well to the side, the mirror direction misses the block: the sky
        // reflects (column 78: about 3 m to the side where it passes it).
        const auto side = [](const std::vector<u8>& p, u32 c) {
            return int{p[(static_cast<usize>(44) * k_w + 78) * 4 + c]};
        };
        std::printf("mirror floor to the side: %d %d %d\n", side(both, 0), side(both, 1), side(both, 2));
        GIANT_CHECK(std::abs(side(both, 1) - side(both, 0)) <= 4);
    }
    GIANT_CHECK(!device->device_lost());
    GIANT_CHECK(device->validation_errors() == 0);
    device->destroy(target);
}

GIANT_TEST("render: the sky's passes run before shading in lit views, the clouds only with some") {
    NullScene s;
    if (s.renderer == nullptr) return;
    std::vector<Instance> instances(1);
    instances[0].mesh = s.renderer->builtin(BuiltinMesh::plane);
    instances[0].scale = {100.0f, 1.0f, 100.0f};
    FrameInput frame;
    frame.instances = instances;
    frame.view.position = {0.0, 2.0, 0.0};
    frame.lighting.sun_direction = normalize_or_zero(Vec3{0.3f, 0.8f, -0.4f});
    frame.settings.gpu_culling = false;
    frame.settings.antialiasing = false;
    const auto names = [&s] {
        std::vector<std::string> out;
        for (const rhi::GpuTiming& t : s.device->gpu_timings()) out.push_back(t.name);
        return out;
    };
    RenderStats st = s.draw(frame);
    GIANT_CHECK(!st.sky && !st.clouds); // off: the flat background
    frame.sky.on = true;
    st = s.draw(frame); // no cover: the air alone
    GIANT_CHECK(st.sky && !st.clouds && !st.cloud_shadows);
    GIANT_CHECK(names() == (std::vector<std::string>{"streaming", "voxels", "gi", "raster", "sky", "shading"}));
    frame.sky.cloud_cover = 0.6f;
    st = s.draw(frame);
    GIANT_CHECK(st.sky && st.clouds && st.cloud_shadows);
    GIANT_CHECK(names() ==
                (std::vector<std::string>{"streaming", "voxels", "gi", "raster", "sky", "clouds", "shading"}));
    // Settings turn them off; a sun below the horizon casts no cloud shadows.
    frame.settings.cloud_shadows = false;
    st = s.draw(frame);
    GIANT_CHECK(st.clouds && !st.cloud_shadows);
    frame.settings.cloud_shadows = true;
    frame.lighting.sun_direction = {0.0f, -1.0f, 0.0f};
    st = s.draw(frame);
    GIANT_CHECK(st.clouds && !st.cloud_shadows);
    frame.settings.clouds = false;
    st = s.draw(frame);
    GIANT_CHECK(st.sky && !st.clouds);
    // Views of the geometry have no sky; nor do odd values break it.
    frame.settings.clouds = true;
    frame.settings.debug_view = DebugView::triangles;
    st = s.draw(frame);
    GIANT_CHECK(!st.sky);
    frame.settings.debug_view = DebugView::lit;
    frame.sky.cloud_base = NAN;
    frame.sky.cloud_tile = -5.0f;
    frame.sky.cloud_offset = {1e300, 0.0, -1e300};
    frame.view.position = {-3.0e7, 2.0, 1.0e9};
    st = s.draw(frame);
    GIANT_CHECK(st.sky);
}

GIANT_TEST("render: d3d12 draws a blue sky, grey under overcast clouds that shade the ground, and fog") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::d3d12;
    desc.software_adapter = true;
    desc.debug_layer = true;
    auto device = rhi::create_device(desc);
    if (device == nullptr) {
        return; // not on Windows
    }
    auto renderer = Renderer::create(*device);
    GIANT_CHECK(renderer != nullptr);
    if (renderer == nullptr) {
        return;
    }
    constexpr u32 k_w = 64, k_h = 48;
    rhi::TextureDesc target_desc;
    target_desc.width = k_w;
    target_desc.height = k_h;
    target_desc.format = rhi::Format::rgba8_srgb;
    target_desc.usage = rhi::TextureUsage::render_target;
    const rhi::TextureHandle target = device->create_texture(target_desc);
    // A grey ground to the horizon; the camera looks out level, a little up.
    std::vector<Instance> instances(1);
    instances[0].mesh = renderer->builtin(BuiltinMesh::plane);
    instances[0].scale = {40000.0f, 1.0f, 40000.0f};
    instances[0].always_mesh = true;
    std::vector<Material> materials(1);
    materials[0].base_color = {0.5f, 0.5f, 0.5f};
    materials[0].roughness = 0.9f;
    FrameInput frame;
    frame.view.position = {1000.0, 2.0, -500.0};
    frame.view.orientation = quat_from_axis_angle({1.0f, 0.0f, 0.0f}, degrees_to_radians(10.0f));
    frame.view.vertical_fov_degrees = 70.0f;
    frame.instances = instances;
    frame.materials = materials;
    frame.lighting.sun_direction = normalize_or_zero(Vec3{0.2f, 0.7f, -0.6f});
    frame.lighting.sun_light = {2.6f, 2.5f, 2.3f};
    frame.lighting.sky_light = {0.15f, 0.25f, 0.45f};
    frame.lighting.ground_light = {0.08f, 0.07f, 0.06f};
    frame.sky.on = true;
    frame.sky.sun_top = {3.0f, 3.0f, 3.0f};
    frame.settings.gi = false;
    frame.settings.voxel_world = false;
    frame.settings.shadows = false;
    frame.settings.antialiasing = false;
    frame.settings.cloud_steps = 24;
    const auto draw = [&] {
        for (int i = 0; i < 6; ++i) {
            rhi::CommandList* cmd = device->begin_frame(nullptr);
            GIANT_CHECK(cmd != nullptr);
            if (cmd != nullptr) {
                renderer->render(*cmd, target, frame);
                device->end_frame();
            }
        }
        device->wait_idle();
        return device->read_texture(target);
    };
    const auto clear = draw();
    GIANT_CHECK(renderer->last_stats().sky && !renderer->last_stats().clouds);
    frame.sky.cloud_cover = 1.0f;
    frame.sky.cloud_density = 1.0f;
    frame.sky.cloud_base = 600.0f;
    frame.sky.cloud_top = 2500.0f;
    const auto overcast = draw();
    GIANT_CHECK(renderer->last_stats().clouds && renderer->last_stats().cloud_shadows);
    frame.sky.cloud_cover = 0.0f;
    frame.sky.fog_density = 0.05f;
    frame.sky.fog_falloff = 1.0f / 100.0f;
    const auto fog = draw();
    // Thin ground fog seen from 8 km up, looking down: a finite, misty
    // ground (the fog's two exponents once made 0 * inf, NaN, black).
    frame.sky.fog_density = 0.02f;
    frame.sky.fog_falloff = std::log(2.0f) / 60.0f;
    frame.view.position = {1000.0, 8000.0, -500.0};
    frame.view.orientation = quat_from_axis_angle({1.0f, 0.0f, 0.0f}, degrees_to_radians(-60.0f));
    const auto high = draw();
    const usize size = usize{k_w} * k_h * 4;
    GIANT_CHECK(high.size() == size);
    if (high.size() == size) {
        const usize i = (static_cast<usize>(k_h / 2) * k_w + k_w / 2) * 4;
        std::printf("sky high fog: ground %d %d %d\n", high[i], high[i + 1], high[i + 2]);
        GIANT_CHECK(high[i] + high[i + 1] + high[i + 2] > 60);
    }
    GIANT_CHECK(clear.size() == size && overcast.size() == size && fog.size() == size);
    if (clear.size() == size && overcast.size() == size && fog.size() == size) {
        const auto px = [](const std::vector<u8>& p, u32 x, u32 y) {
            const usize i = (static_cast<usize>(y) * k_w + x) * 4;
            return std::array<int, 3>{p[i], p[i + 1], p[i + 2]};
        };
        const auto print = [&](const char* what, const std::vector<u8>& p) {
            const auto up = px(p, k_w / 2, 2);
            const auto down = px(p, k_w / 2, k_h - 2);
            const auto mid = px(p, k_w / 2, k_h / 2 + 4);
            std::printf("sky %s: up %d %d %d, ground near %d %d %d, ground far %d %d %d\n", what, up[0], up[1], up[2],
                        down[0], down[1], down[2], mid[0], mid[1], mid[2]);
        };
        print("clear", clear);
        print("overcast", overcast);
        print("fog", fog);
        // Clear: the sky overhead is blue.
        const auto up = px(clear, k_w / 2, 2);
        GIANT_CHECK(up[2] > up[0] + 20);
        // Overcast: grey, and the ground in the clouds' shadow is darker.
        const auto grey = px(overcast, k_w / 2, 2);
        GIANT_CHECK(std::abs(grey[2] - grey[0]) < (up[2] - up[0]) / 2);
        GIANT_CHECK(px(overcast, k_w / 2, k_h - 2)[1] < px(clear, k_w / 2, k_h - 2)[1]);
        // Fog: the far ground fades towards the sky; the near ground barely.
        const auto far_clear = px(clear, k_w / 2, k_h / 2 + 4);
        const auto far_fog = px(fog, k_w / 2, k_h / 2 + 4);
        GIANT_CHECK(std::abs(far_fog[1] - far_clear[1]) > 8);
        const auto near_clear = px(clear, k_w / 2, k_h - 2);
        const auto near_fog = px(fog, k_w / 2, k_h - 2);
        GIANT_CHECK(std::abs(near_fog[1] - near_clear[1]) < std::abs(far_fog[1] - far_clear[1]));
    }
    GIANT_CHECK(!device->device_lost());
    GIANT_CHECK(device->validation_errors() == 0);
    device->destroy(target);
}

GIANT_TEST("render: d3d12 snow and wet ground under the weather, kept off where a roof shelters it") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::d3d12;
    desc.software_adapter = true;
    desc.debug_layer = true;
    auto device = rhi::create_device(desc);
    if (device == nullptr) {
        return; // not on Windows
    }
    RendererDesc rd; // a small voxel world (the cover map is made from it)
    rd.voxel_clipmap.levels = 6;
    rd.voxel_clipmap.bricks_per_axis = 16;
    rd.voxel_bricks = 4096;
    auto renderer = Renderer::create(*device, rd);
    GIANT_CHECK(renderer != nullptr);
    if (renderer == nullptr) {
        return;
    }
    constexpr u32 k_w = 64, k_h = 48;
    rhi::TextureDesc target_desc;
    target_desc.width = k_w;
    target_desc.height = k_h;
    target_desc.format = rhi::Format::rgba8_srgb;
    target_desc.usage = rhi::TextureUsage::render_target;
    const rhi::TextureHandle target = device->create_texture(target_desc);
    // Grey ground; a roof 3 m up over the camera (or moved far off); the
    // camera 1.5 m up looking straight down at the ground.
    const DVec3 origin{12'000.0, 40.0, -9'000.0};
    std::vector<Instance> instances(2);
    instances[0].mesh = renderer->builtin(BuiltinMesh::plane);
    instances[0].position = origin;
    instances[0].scale = {16.0f, 1.0f, 16.0f};
    instances[1].mesh = renderer->builtin(BuiltinMesh::cube);
    instances[1].scale = {8.0f, 0.3f, 8.0f};
    std::vector<Material> materials(1);
    materials[0].base_color = {0.35f, 0.35f, 0.35f};
    materials[0].roughness = 0.9f;
    FrameInput frame;
    frame.view.position = origin + DVec3{0.0, 1.5, 0.0};
    frame.view.orientation = quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -k_pi * 0.5f);
    frame.view.vertical_fov_degrees = 50.0f;
    frame.instances = instances;
    frame.materials = materials;
    frame.lighting.sun_direction = normalize_or_zero(Vec3{0.3f, 0.9f, 0.2f});
    frame.lighting.sun_light = {2.5f, 2.5f, 2.5f};
    frame.lighting.sky_light = {0.4f, 0.4f, 0.45f};
    frame.lighting.ground_light = {0.2f, 0.2f, 0.2f};
    frame.sky.on = true;
    frame.settings.antialiasing = false;
    frame.settings.gi = false;
    frame.settings.shadows = false; // the roof's shade is not what this checks
    frame.settings.voxel_ms_per_frame = 1000.0f;
    const auto draw = [&](bool roof) {
        instances[1].position = origin + (roof ? DVec3{0.0, 3.0, 0.0} : DVec3{60.0, 3.0, 0.0});
        for (int i = 0; i < 16; ++i) {
            rhi::CommandList* cmd = device->begin_frame(nullptr);
            GIANT_CHECK(cmd != nullptr);
            if (cmd != nullptr) {
                renderer->render(*cmd, target, frame);
                device->end_frame();
            }
        }
        device->wait_idle();
        return device->read_texture(target);
    };
    const auto dry = draw(false);
    frame.sky.wetness = 1.0f;
    const auto wet = draw(false);
    frame.sky.wetness = 0.0f;
    frame.sky.snow_cover = 1.0f;
    frame.sky.snow = 0.5f;
    const auto snowy = draw(false);
    const RenderStats st = renderer->last_stats();
    GIANT_CHECK(st.sky && st.wet_surfaces && st.weather_cover && st.falling && st.voxel_pending == 0);
    const auto sheltered = draw(true);
    const usize size = usize{k_w} * k_h * 4;
    GIANT_CHECK(dry.size() == size && wet.size() == size && snowy.size() == size && sheltered.size() == size);
    if (dry.size() == size && wet.size() == size && snowy.size() == size && sheltered.size() == size) {
        // The ground a little off centre (no flake on it: falling snow is
        // sparse; three pixels averaged).
        const auto px = [](const std::vector<u8>& p) {
            int sum = 0;
            for (const u32 x : {k_w / 2 - 6, k_w / 2, k_w / 2 + 6}) {
                const usize i = (static_cast<usize>(k_h / 2 + 5) * k_w + x) * 4;
                sum += p[i] + p[i + 1] + p[i + 2];
            }
            return sum / 9;
        };
        std::printf("weather ground: dry %d, wet %d, snow %d, snow under a roof %d\n", px(dry), px(wet), px(snowy),
                    px(sheltered));
        GIANT_CHECK(px(wet) < px(dry) - 8);       // wet ground is darker
        GIANT_CHECK(px(snowy) > px(dry) + 40);    // snow is bright
        GIANT_CHECK(px(sheltered) < px(snowy) - 40); // none under the roof
    }
    GIANT_CHECK(!device->device_lost());
    GIANT_CHECK(device->validation_errors() == 0);
    device->destroy(target);
}
