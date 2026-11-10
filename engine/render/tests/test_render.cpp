#include "giant/geometry/mesh.h"
#include "giant/render/renderer.h"

#include "giant_test.h"
#include "shadow_pages.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <limits>
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
    // Screen reflections; exposure and tone mapping; SMAA: edges, weights, blend.
    GIANT_CHECK(renderer->last_stats().post_draws == 5);
    GIANT_CHECK(device->last_frame_stats().draws == 2 + 5);
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
    GIANT_CHECK(names() == (std::vector<std::string>{"streaming", "voxels", "gi", "raster", "ambient occlusion", "shading", "post",
                                                     "anti-aliasing"}));
    frame.settings.gpu_culling = true;
    frame.settings.occlusion_culling = true;
    frame.settings.antialiasing = false;
    s.draw(frame);
    GIANT_CHECK(names() == (std::vector<std::string>{"streaming", "voxels", "gi", "culling", "raster", "HZB", "culling",
                                                     "raster", "HZB", "shadows", "ambient occlusion", "shading", "post"}));
    GIANT_CHECK(s.renderer->last_stats().ambient_occlusion);
    // Ambient occlusion off, or a debug view: no pass.
    frame.settings.ambient_occlusion = false;
    s.draw(frame);
    GIANT_CHECK(!s.renderer->last_stats().ambient_occlusion);
    GIANT_CHECK(names() == (std::vector<std::string>{"streaming", "voxels", "gi", "culling", "raster", "HZB", "culling",
                                                     "raster", "HZB", "shadows", "shading", "post"}));
    frame.settings.ambient_occlusion = true;
    frame.settings.ambient_occlusion_radius = NAN; // (kept in range)
    frame.settings.debug_view = DebugView::clusters;
    s.draw(frame);
    GIANT_CHECK(!s.renderer->last_stats().ambient_occlusion);
    frame.settings.debug_view = DebugView::lit;
    s.draw(frame);
    GIANT_CHECK(s.renderer->last_stats().ambient_occlusion);
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
    // Shading, then the pass that reflects, then exposure and tone mapping.
    GIANT_CHECK(st.post_draws == 2);
    GIANT_CHECK(s.device->last_frame_stats().draws == st.draws + st.post_draws + st.shadow_draws);
    // Without global illumination, only the screen's.
    frame.settings.gi = false;
    st = s.draw(frame);
    GIANT_CHECK(!st.reflections && st.screen_reflections);
    frame.settings.gi = true;
    frame.settings.screen_space_reflections = false;
    st = s.draw(frame);
    GIANT_CHECK(st.reflections && !st.screen_reflections && st.post_draws == 1); // exposure and tone mapping alone
    frame.settings.screen_space_reflections = true;
    frame.settings.debug_view = DebugView::lighting;
    st = s.draw(frame);
    GIANT_CHECK(st.reflections && st.screen_reflections);
    // Views of the geometry have none.
    frame.settings.debug_view = DebugView::clusters;
    st = s.draw(frame);
    GIANT_CHECK(!st.reflections && !st.screen_reflections && st.post_draws == 1); // shown as they are
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
    frame.settings.auto_exposure = false; // (Lighting::exposure alone, no bloom: the colours checked hold)
    frame.settings.bloom = false;
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
        GIANT_CHECK(timings.size() == 12); // streaming, voxels, 2 x (culling, raster, HZB), AO, shading, post, AA
        bool timed = true;
        for (const rhi::GpuTiming& t : timings) {
            timed = timed && t.ms >= 0.0f && t.ms <= device->gpu_frame_ms() + 1e-4f;
        }
        GIANT_CHECK(timed && device->gpu_frame_ms() > 0.0f);
        GIANT_CHECK(timings.size() == 12 && timings[2].name == "culling" && timings[8].name == "ambient occlusion" &&
                    timings[9].name == "shading" && timings[10].name == "post");
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
    GIANT_CHECK(renderer->last_stats().post_draws == 2); // the screen reflections pass, then tone mapping
    frame.settings.antialiasing = true;
    const auto smoothed = draw(false, false, false);
    GIANT_CHECK(renderer->last_stats().post_draws == 5);
    if (aliased.size() == k_w * k_h * 4 && smoothed.size() == aliased.size()) {
        // How far apart two pixels are: their largest channel difference
        // (luma alone misses a dark grey against the blue background).
        const auto apart = [&aliased](u32 x, u32 y, u32 x2, u32 y2) {
            const usize i = (static_cast<usize>(y) * k_w + x) * 4;
            const usize j = (static_cast<usize>(y2) * k_w + x2) * 4;
            int most = 0;
            for (usize c = 0; c < 3; ++c) most = std::max(most, std::abs(int{aliased[i + c]} - int{aliased[j + c]}));
            return static_cast<double>(most);
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
                const double step = std::max({apart(x, y, x - 1, y), apart(x, y, x + 1, y), apart(x, y, x, y - 1),
                                              apart(x, y, x, y + 1)});
                if (step < 8.0) {
                    ++off_edge;
                    std::printf("smaa changed a pixel off the edges: (%u, %u) %u %u %u -> %u %u %u, step %.1f\n", x, y,
                                aliased[i], aliased[i + 1], aliased[i + 2], smoothed[i], smoothed[i + 1],
                                smoothed[i + 2], step);
                }
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
    frame.settings.auto_exposure = false; // (Lighting::exposure alone, no bloom: the colours checked hold)
    frame.settings.bloom = false;
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

GIANT_TEST("render: d3d12 sways what the wind moves, as the CPU works it out, near the camera only") {
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

    // Grey ground and a red pole 4 m tall (a cube: its middle is its
    // origin, so its top 2 m bend), seen from 8 m straight down (screen
    // right is +X), far from the origin.
    const DVec3 origin{-40'000.0, 120.0, 70'000.0};
    std::vector<Instance> instances(2);
    instances[0].mesh = renderer->builtin(BuiltinMesh::plane);
    instances[0].position = origin;
    instances[0].scale = {40.0f, 1.0f, 40.0f};
    instances[1].mesh = renderer->builtin(BuiltinMesh::cube);
    instances[1].position = origin + DVec3{0.0, 2.0, 0.0};
    instances[1].scale = {0.3f, 4.0f, 0.3f};
    instances[1].material = 1;
    std::vector<Material> materials(2);
    materials[0].base_color = {0.5f, 0.5f, 0.5f};
    materials[1].base_color = {0.9f, 0.05f, 0.05f};
    materials[1].sway = 4.0f;
    FrameInput frame;
    frame.settings.auto_exposure = false; // (Lighting::exposure alone, no bloom: the colours checked hold)
    frame.settings.bloom = false;
    frame.view.position = origin + DVec3{0.0, 8.0, 0.0};
    frame.view.orientation = quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -k_pi * 0.5f);
    frame.instances = instances;
    frame.materials = materials;
    frame.lighting.sun_direction = normalize_or_zero(Vec3{0.0f, 1.0f, 0.2f});
    frame.lighting.sun_light = {3.0f, 3.0f, 3.0f};
    frame.lighting.sky_light = {0.3f, 0.3f, 0.3f};
    frame.lighting.ground_light = {0.1f, 0.1f, 0.1f};
    frame.settings.antialiasing = false;
    frame.settings.gi = false;
    frame.settings.shadows = false;
    // A steady gale along +X: the pole's top leans the most it can, 30%
    // of the 2 m that bend.
    WindClock clock;
    clock.advance({{30.0f, 0.0f, 0.0f}, 0.0f}, 0.5);
    const auto draw = [&](const WindState& wind) {
        frame.wind = wind;
        for (int i = 0; i < 8; ++i) {
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
    // The red pixels' rightmost column (the pole's top, seen from above).
    const auto right_edge = [&](const std::vector<u8>& p) {
        int edge = -1;
        if (p.size() != k_w * k_h * 4) return edge;
        for (u32 y = 0; y < k_h; ++y) {
            for (u32 x = 0; x < k_w; ++x) {
                const u8* c = &p[(static_cast<usize>(y) * k_w + x) * 4];
                if (c[0] > 2 * c[1] + 20) edge = std::max(edge, static_cast<int>(x));
            }
        }
        return edge;
    };
    const int still = right_edge(draw(WindState{}));
    const int blown = right_edge(draw(clock.state()));
    // About 0.072 m a pixel at the top (4 m away): 0.6 m is 8 pixels.
    const Vec3 expect = sway_offset(clock.state(), instances[1].position, 2.0f,
                                    instances[1].position + DVec3{0.15, 2.0, 0.0}, 4.0f, 0.0f);
    GIANT_CHECK(std::abs(expect.x - 0.6f) < 0.01f);
    GIANT_CHECK(still > static_cast<int>(k_w / 2) && blown >= still + 6 && blown <= still + 10);
    // The CPU's cut grows its sphere the same way: the same image.
    frame.settings.gpu_culling = false;
    GIANT_CHECK(right_edge(draw(clock.state())) == blown);
    frame.settings.gpu_culling = true;
    // Past wind_distance (or with it 0) it stands still.
    frame.settings.wind_distance = 0.0f;
    GIANT_CHECK(right_edge(draw(clock.state())) == still);
    frame.settings.wind_distance = 3.0f;
    GIANT_CHECK(right_edge(draw(clock.state())) == still);
    // A still material does not move in the gale.
    frame.settings.wind_distance = 150.0f;
    materials[1].sway = 0.0f;
    GIANT_CHECK(right_edge(draw(clock.state())) == still);
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
    frame.settings.auto_exposure = false; // (Lighting::exposure alone, no bloom: the colours checked hold)
    frame.settings.bloom = false;
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
    frame.settings.auto_exposure = false; // (Lighting::exposure alone, no bloom: the colours checked hold)
    frame.settings.bloom = false;
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
    GIANT_CHECK(st.sky && st.physical_sky && !st.clouds && !st.cloud_shadows);
    GIANT_CHECK(names() == (std::vector<std::string>{"streaming", "voxels", "gi", "raster", "sky", "ambient occlusion", "shading", "post"}));
    // The physical sky's passes run inside "sky"; off, the single-scattered sky and the flat ambient colours.
    frame.settings.physical_sky = false;
    st = s.draw(frame);
    GIANT_CHECK(st.sky && !st.physical_sky);
    GIANT_CHECK(names() == (std::vector<std::string>{"streaming", "voxels", "gi", "raster", "sky", "ambient occlusion", "shading", "post"}));
    frame.settings.physical_sky = true;
    frame.settings.aerial_distance = NAN; // (kept in range)
    st = s.draw(frame);
    GIANT_CHECK(st.physical_sky);
    frame.settings.aerial_distance = 32000.0f;
    frame.sky.cloud_cover = 0.6f;
    st = s.draw(frame);
    GIANT_CHECK(st.sky && st.clouds && st.cloud_shadows);
    GIANT_CHECK(names() ==
                (std::vector<std::string>{"streaming", "voxels", "gi", "raster", "sky", "clouds", "ambient occlusion", "shading", "post"}));
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
    frame.settings.auto_exposure = false; // (Lighting::exposure alone, no bloom: the colours checked hold)
    frame.settings.bloom = false;
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
    frame.settings.physical_sky = false; // (the single-scattered sky and flat ambient this was set for; its own test below)
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

GIANT_TEST("render: d3d12 physical sky: a brighter sky, far ground fading blue through the air, ambient from the sky") {
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
    // A dark ground 20 km out each way; the camera 300 m up looking 10 degrees down: the sky at
    // the top, the ground about 7 km off a little under the middle, about 440 m off at the bottom.
    // (Dark, so the air shows as a blue haze over it: a sunlit ground brighter than the horizon sky
    // would fade toward that dimmer sky, losing as much blue as the air adds.)
    std::vector<Instance> instances(1);
    instances[0].mesh = renderer->builtin(BuiltinMesh::plane);
    instances[0].scale = {40000.0f, 1.0f, 40000.0f};
    instances[0].always_mesh = true;
    std::vector<Material> materials(1);
    materials[0].base_color = {0.1f, 0.1f, 0.1f};
    materials[0].roughness = 0.9f;
    FrameInput frame;
    frame.settings.auto_exposure = false; // (Lighting::exposure alone, no bloom: the colours checked hold)
    frame.settings.bloom = false;
    frame.view.position = {1000.0, 300.0, -500.0};
    frame.view.orientation = quat_from_axis_angle({1.0f, 0.0f, 0.0f}, degrees_to_radians(-10.0f));
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
    const auto draw = [&] {
        for (int i = 0; i < 4; ++i) { // (the ambient table is read from the frame after it is first filled)
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
    frame.settings.physical_sky = false;
    const auto flat = draw();
    GIANT_CHECK(!renderer->last_stats().physical_sky);
    frame.settings.physical_sky = true;
    const auto physical = draw();
    GIANT_CHECK(renderer->last_stats().physical_sky);
    // The same with no light from the sky at all: the ambient is the sky's, not sky_light's.
    frame.lighting.sky_light = {0.0f, 0.0f, 0.0f};
    frame.lighting.ground_light = {0.0f, 0.0f, 0.0f};
    const auto unlit_flat = draw();
    const usize size = usize{k_w} * k_h * 4;
    GIANT_CHECK(flat.size() == size && physical.size() == size && unlit_flat.size() == size);
    if (flat.size() == size && physical.size() == size && unlit_flat.size() == size) {
        const auto px = [](const std::vector<u8>& p, u32 x, u32 y) {
            const usize i = (static_cast<usize>(y) * k_w + x) * 4;
            return std::array<int, 3>{p[i], p[i + 1], p[i + 2]};
        };
        const auto print = [&](const char* what, const std::vector<u8>& p) {
            const auto up = px(p, k_w / 2, 2);
            const auto near = px(p, k_w / 2, k_h - 2);
            const auto far = px(p, k_w / 2, 19);
            std::printf("physical sky %s: up %d %d %d, ground near %d %d %d, ground far %d %d %d\n", what, up[0], up[1],
                        up[2], near[0], near[1], near[2], far[0], far[1], far[2]);
        };
        print("off", flat);
        print("on", physical);
        print("on, no flat colours", unlit_flat);
        const auto sum = [](const std::array<int, 3>& c) { return c[0] + c[1] + c[2]; };
        // The sky overhead: still blue, and no darker for the light scattered many times.
        const auto up_on = px(physical, k_w / 2, 2);
        const auto up_off = px(flat, k_w / 2, 2);
        GIANT_CHECK(up_on[2] > up_on[0] + 20);
        GIANT_CHECK(sum(up_on) >= sum(up_off));
        // The far ground: bluer than the near ground through the air (more than without it).
        const auto near_on = px(physical, k_w / 2, k_h - 2);
        const auto far_on = px(physical, k_w / 2, 19);
        const auto near_off = px(flat, k_w / 2, k_h - 2);
        const auto far_off = px(flat, k_w / 2, 19);
        GIANT_CHECK((far_on[2] - far_on[0]) - (near_on[2] - near_on[0]) >
                    (far_off[2] - far_off[0]) - (near_off[2] - near_off[0]));
        // The ambient light is the sky's: without the flat colours the near ground is as lit as with them.
        const auto near_unlit = px(unlit_flat, k_w / 2, k_h - 2);
        GIANT_CHECK(std::abs(sum(near_unlit) - sum(near_on)) <= 6);
        GIANT_CHECK(sum(near_on) > 60);
    }
    GIANT_CHECK(!device->device_lost());
    GIANT_CHECK(device->validation_errors() == 0);
    device->destroy(target);
}

GIANT_TEST("render: dust puffs given are counted, bad ones and those past the cap left out (null device)") {
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
    std::vector<Instance> instances(1);
    instances[0].mesh = renderer->builtin(BuiltinMesh::cube);
    instances[0].position = {0.0, 0.0, -6.0};
    std::vector<DustPuff> dust(40);
    for (usize i = 0; i < dust.size(); ++i) dust[i].center = {0.0, 0.0, -10.0 - static_cast<double>(i)};
    dust[1].radius = std::numeric_limits<float>::quiet_NaN();
    dust[2].density = 0.0f;
    FrameInput frame;
    frame.instances = instances;
    frame.dust = dust;
    const auto draw = [&] {
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        GIANT_CHECK(cmd != nullptr);
        if (cmd != nullptr) {
            renderer->render(*cmd, target, frame);
            device->end_frame();
        }
    };
    draw();
    GIANT_CHECK(renderer->last_stats().dust_puffs == k_max_dust_puffs - 2); // the first 32, two of them bad
    frame.settings.debug_view = DebugView::clusters; // not lit: no dust
    draw();
    GIANT_CHECK(renderer->last_stats().dust_puffs == 0);
    device->destroy(target);
}

GIANT_TEST("render: d3d12 a dust puff hides what is behind it, lit; one behind the ground shows nothing") {
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
    // A dark ground; the camera 2 m up looks a little down at it.
    std::vector<Instance> instances(1);
    instances[0].mesh = renderer->builtin(BuiltinMesh::plane);
    instances[0].scale = {400.0f, 1.0f, 400.0f};
    instances[0].always_mesh = true;
    std::vector<Material> materials(1);
    materials[0].base_color = {0.05f, 0.05f, 0.05f};
    materials[0].roughness = 0.9f;
    FrameInput frame;
    frame.settings.auto_exposure = false; // (Lighting::exposure alone, no bloom: the colours checked hold)
    frame.settings.bloom = false;
    frame.view.position = {0.0, 2.0, 0.0};
    frame.view.orientation = quat_from_axis_angle({1.0f, 0.0f, 0.0f}, degrees_to_radians(-10.0f));
    frame.instances = instances;
    frame.materials = materials;
    frame.lighting.sun_direction = normalize_or_zero(Vec3{0.2f, 0.7f, -0.6f});
    frame.lighting.sun_light = {2.6f, 2.5f, 2.3f};
    frame.lighting.sky_light = {0.15f, 0.25f, 0.45f};
    frame.lighting.ground_light = {0.08f, 0.07f, 0.06f};
    frame.settings.gi = false;
    frame.settings.voxel_world = false;
    frame.settings.shadows = false;
    frame.settings.antialiasing = false;
    const auto draw = [&] {
        for (int i = 0; i < 3; ++i) {
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
    const auto bare = draw();
    // A pale puff 6 m across about 9 m ahead, low over the ground: its middle hides the ground.
    std::vector<DustPuff> dust(1);
    const Vec3 ahead = rotate(frame.view.orientation, Vec3{0.0f, 0.0f, -1.0f});
    const double along = 2.0 / -ahead.y; // where the view's middle meets the ground
    dust[0].center = {ahead.x * along * 0.8, 2.0 + ahead.y * along * 0.8, ahead.z * along * 0.8};
    dust[0].radius = 3.0f;
    dust[0].density = 2.0f;
    dust[0].color = {0.8f, 0.75f, 0.7f};
    frame.dust = dust;
    const auto dusty = draw();
    GIANT_CHECK(renderer->last_stats().dust_puffs == 1);
    // The same puff under the ground, behind what the pixels show: nothing.
    dust[0].center = {ahead.x * along * 1.5, -20.0, ahead.z * along * 1.5};
    const auto buried = draw();
    const usize size = usize{k_w} * k_h * 4;
    GIANT_CHECK(bare.size() == size && dusty.size() == size && buried.size() == size);
    if (bare.size() == size && dusty.size() == size && buried.size() == size) {
        const auto px = [](const std::vector<u8>& p, u32 x, u32 y) {
            const usize i = (static_cast<usize>(y) * k_w + x) * 4;
            return std::array<int, 3>{p[i], p[i + 1], p[i + 2]};
        };
        const auto mid_bare = px(bare, k_w / 2, k_h / 2);
        const auto mid_dusty = px(dusty, k_w / 2, k_h / 2);
        const auto edge_bare = px(bare, 2, k_h - 2);
        const auto edge_dusty = px(dusty, 2, k_h - 2);
        std::printf("dust: middle %d %d %d -> %d %d %d, corner %d %d %d -> %d %d %d\n", mid_bare[0], mid_bare[1],
                    mid_bare[2], mid_dusty[0], mid_dusty[1], mid_dusty[2], edge_bare[0], edge_bare[1], edge_bare[2],
                    edge_dusty[0], edge_dusty[1], edge_dusty[2]);
        // Lit dust in front of the dark ground: brighter.
        GIANT_CHECK(mid_dusty[0] > mid_bare[0] + 30 && mid_dusty[1] > mid_bare[1] + 30);
        // Away from it, and with it behind the ground, the image as without.
        GIANT_CHECK(std::abs(edge_dusty[1] - edge_bare[1]) <= 2);
        usize differ = 0;
        for (usize i = 0; i < size; ++i) differ += std::abs(int{buried[i]} - int{bare[i]}) > 1 ? 1u : 0u;
        GIANT_CHECK(differ == 0);
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
    frame.settings.auto_exposure = false; // (Lighting::exposure alone, no bloom: the colours checked hold)
    frame.settings.bloom = false;
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
    frame.settings.physical_sky = false; // (the flat ambient these levels were set for)
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

GIANT_TEST("render: exposure, bloom and tone mapping after shading; auto exposure waits for a measurement (null device)") {
    NullScene s;
    GIANT_CHECK(s.renderer != nullptr);
    if (s.renderer == nullptr) return;
    std::vector<Instance> instances(1);
    instances[0].mesh = s.renderer->builtin(BuiltinMesh::cube);
    instances[0].position = {0.0, 0.0, -5.0};
    FrameInput frame;
    frame.instances = instances;
    frame.lighting.exposure = 2.0f;
    RenderStats st = s.draw(frame);
    // On by default; the null device reads nothing back, so Lighting::exposure alone.
    GIANT_CHECK(st.auto_exposure && st.bloom && !st.exposure_measured && st.exposure == 2.0f);
    for (int i = 0; i < 4; ++i) {
        st = s.draw(frame);
    }
    GIANT_CHECK(st.auto_exposure && !st.exposure_measured && st.exposure == 2.0f);
    // Off: the manual exposure; no bloom with it off or at no strength.
    frame.settings.auto_exposure = false;
    frame.settings.bloom = false;
    st = s.draw(frame);
    GIANT_CHECK(!st.auto_exposure && !st.bloom && st.exposure == 2.0f);
    frame.settings.bloom = true;
    frame.settings.bloom_strength = 0.0f;
    GIANT_CHECK(!s.draw(frame).bloom);
    frame.settings.bloom_strength = NAN;
    GIANT_CHECK(!s.draw(frame).bloom);
    // Nonsense kept sane.
    frame.settings.bloom_strength = 0.04f;
    frame.settings.auto_exposure = true;
    frame.settings.exposure_compensation = NAN;
    frame.settings.exposure_min = NAN;
    frame.settings.exposure_speed_up = -INFINITY;
    frame.delta_seconds = NAN;
    frame.lighting.exposure = NAN;
    st = s.draw(frame);
    GIANT_CHECK(st.auto_exposure && st.bloom && st.exposure == 1.0f);
    // Views of the geometry: shown as they are, nothing measured or bloomed.
    frame.settings.debug_view = DebugView::clusters;
    st = s.draw(frame);
    GIANT_CHECK(!st.auto_exposure && !st.bloom && st.post_draws == 1);
    frame.settings.debug_view = DebugView::voxels;
    st = s.draw(frame);
    GIANT_CHECK(!st.auto_exposure && !st.bloom && st.post_draws == 0);
    // A new size: bloom's levels are made again with the frame's targets.
    frame.settings.debug_view = DebugView::lit;
    rhi::TextureDesc t;
    t.width = 7;
    t.height = 3;
    t.format = rhi::Format::rgba8_srgb;
    t.usage = rhi::TextureUsage::render_target;
    s.device->destroy(s.target);
    s.target = s.device->create_texture(t);
    st = s.draw(frame);
    GIANT_CHECK(st.bloom);
    GIANT_CHECK(s.device->validation_errors() == 0);
}

namespace {
// A d3d12 renderer drawing into a small target on the software adapter
// (nullptr off Windows).
struct WarpScene {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<Renderer> renderer;
    rhi::TextureHandle target;
    static constexpr u32 k_w = 64, k_h = 48;
    bool make() {
        rhi::DeviceDesc desc;
        desc.backend = rhi::Backend::d3d12;
        desc.software_adapter = true;
        desc.debug_layer = true;
        device = rhi::create_device(desc);
        if (device == nullptr) {
            return false;
        }
        renderer = Renderer::create(*device);
        GIANT_CHECK(renderer != nullptr);
        if (renderer == nullptr) {
            return false;
        }
        rhi::TextureDesc t;
        t.width = k_w;
        t.height = k_h;
        t.format = rhi::Format::rgba8_srgb;
        t.usage = rhi::TextureUsage::render_target;
        target = device->create_texture(t);
        return true;
    }
    // `frames` frames, each waited for (so measurements come back), then the image.
    std::vector<u8> draw(const FrameInput& frame, int frames) {
        for (int i = 0; i < frames; ++i) {
            rhi::CommandList* cmd = device->begin_frame(nullptr);
            GIANT_CHECK(cmd != nullptr);
            if (cmd != nullptr) {
                renderer->render(*cmd, target, frame);
                device->end_frame();
            }
            device->wait_idle();
        }
        return device->read_texture(target);
    }
    static std::array<int, 3> px(const std::vector<u8>& p, u32 x, u32 y) {
        const usize i = (static_cast<usize>(y) * k_w + x) * 4;
        if (p.size() < i + 4) {
            return {0, 0, 0};
        }
        return {p[i], p[i + 1], p[i + 2]};
    }
};
} // namespace

GIANT_TEST("render: d3d12 auto exposure brings a dim and a bright scene to the same brightness, eased") {
    WarpScene w;
    if (!w.make()) {
        return; // not on Windows
    }
    // A grey ground filling the view from 10 m up, lit by the sun alone.
    std::vector<Instance> instances(1);
    instances[0].mesh = w.renderer->builtin(BuiltinMesh::plane);
    instances[0].scale = {200.0f, 1.0f, 200.0f};
    instances[0].always_mesh = true;
    std::vector<Material> materials(1);
    materials[0].base_color = {0.5f, 0.5f, 0.5f};
    materials[0].roughness = 0.9f;
    FrameInput frame;
    frame.view.position = {0.0, 10.0, 0.0};
    frame.view.orientation = quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -k_pi * 0.5f);
    frame.instances = instances;
    frame.materials = materials;
    frame.lighting.sun_direction = {0.0f, 1.0f, 0.0f};
    frame.lighting.sky_light = {0.0f, 0.0f, 0.0f};
    frame.lighting.ground_light = {0.0f, 0.0f, 0.0f};
    frame.settings.gi = false;
    frame.settings.voxel_world = false;
    frame.settings.shadows = false;
    frame.settings.antialiasing = false;
    frame.settings.bloom = false;
    frame.settings.exposure_speed_up = 100.0f; // (eased at once: a second a frame)
    frame.settings.exposure_speed_down = 100.0f;
    frame.delta_seconds = 1.0f;
    const auto grey = [&](float sun, bool auto_exposure) {
        frame.lighting.sun_light = {sun, sun, sun};
        frame.settings.auto_exposure = auto_exposure;
        return WarpScene::px(w.draw(frame, 5), WarpScene::k_w / 2, WarpScene::k_h / 2)[1];
    };
    const int dim_fixed = grey(0.05f, false);
    const int bright_fixed = grey(5.0f, false);
    const int dim = grey(0.05f, true);
    const RenderStats dim_stats = w.renderer->last_stats();
    const int bright = grey(5.0f, true);
    const RenderStats bright_stats = w.renderer->last_stats();
    std::printf("auto exposure: fixed %d / %d, auto %d / %d (scene %.2f / %.2f stops, exposure %.3f / %.3f)\n",
                dim_fixed, bright_fixed, dim, bright, dim_stats.scene_stops, bright_stats.scene_stops,
                dim_stats.exposure, bright_stats.exposure);
    GIANT_CHECK(bright_fixed - dim_fixed > 100);
    GIANT_CHECK(dim_stats.exposure_measured && bright_stats.exposure_measured);
    // A hundred times the light: about 6.6 stops brighter measured, as much less exposure.
    GIANT_CHECK(std::abs((bright_stats.scene_stops - dim_stats.scene_stops) - std::log2(100.0f)) < 0.75f);
    GIANT_CHECK(std::abs(std::log2(dim_stats.exposure / bright_stats.exposure) - std::log2(100.0f)) < 0.75f);
    GIANT_CHECK(std::abs(bright - dim) <= 20); // (the histogram's bins are 0.38 stops wide)
    // Middle grey shows as middle grey does (about sRGB 105 under PBR Neutral).
    GIANT_CHECK(bright > 80 && bright < 160);
    // Eased: a slow eye has not caught up a frame after the light drops.
    frame.settings.exposure_speed_up = 1.0f;
    frame.delta_seconds = 1.0f / 60.0f;
    frame.lighting.sun_light = {0.05f, 0.05f, 0.05f};
    const int caught = WarpScene::px(w.draw(frame, 4), WarpScene::k_w / 2, WarpScene::k_h / 2)[1];
    GIANT_CHECK(caught < dim - 40);
    // Compensation: a stop brighter.
    frame.settings.exposure_speed_up = 100.0f;
    frame.delta_seconds = 1.0f;
    frame.settings.exposure_compensation = 1.0f;
    w.draw(frame, 5);
    GIANT_CHECK(std::abs(std::log2(w.renderer->last_stats().exposure / dim_stats.exposure) - 1.0f) < 0.1f);
    GIANT_CHECK(!w.device->device_lost());
    GIANT_CHECK(w.device->validation_errors() == 0);
}

GIANT_TEST("render: d3d12 bloom spreads a bright light round it; AgX keeps grey grey") {
    WarpScene w;
    if (!w.make()) {
        return; // not on Windows
    }
    // A small blazing ball on a dark ground, seen from above.
    std::vector<Instance> instances(2);
    instances[0].mesh = w.renderer->builtin(BuiltinMesh::plane);
    instances[0].scale = {200.0f, 1.0f, 200.0f};
    instances[0].always_mesh = true;
    instances[0].material = 0;
    instances[1].mesh = w.renderer->builtin(BuiltinMesh::sphere);
    instances[1].position = {0.0, 0.5, 0.0}; // 1 m wide: about 4 pixels
    instances[1].scale = {1.0f, 1.0f, 1.0f};
    instances[1].always_mesh = true;
    instances[1].material = 1;
    std::vector<Material> materials(2);
    materials[0].base_color = {0.18f, 0.18f, 0.18f};
    materials[0].roughness = 0.9f;
    materials[1].base_color = {0.0f, 0.0f, 0.0f};
    materials[1].emissive = {200.0f, 200.0f, 200.0f};
    FrameInput frame;
    frame.view.position = {0.0, 10.0, 0.0};
    frame.view.orientation = quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -k_pi * 0.5f);
    frame.instances = instances;
    frame.materials = materials;
    frame.lighting.sun_direction = {0.0f, 1.0f, 0.0f};
    frame.lighting.sun_light = {0.0f, 0.0f, 0.0f};
    frame.lighting.sky_light = {0.3f, 0.3f, 0.3f};
    frame.lighting.ground_light = {0.0f, 0.0f, 0.0f};
    frame.settings.gi = false;
    frame.settings.voxel_world = false;
    frame.settings.shadows = false;
    frame.settings.antialiasing = false;
    frame.settings.reflections = false;
    frame.settings.auto_exposure = false;
    frame.settings.bloom = false;
    const auto off = w.draw(frame, 2);
    GIANT_CHECK(!w.renderer->last_stats().bloom);
    frame.settings.bloom = true;
    frame.settings.bloom_strength = 0.2f;
    const auto on = w.draw(frame, 2);
    GIANT_CHECK(w.renderer->last_stats().bloom);
    const u32 cx = WarpScene::k_w / 2, cy = WarpScene::k_h / 2;
    const auto near_off = WarpScene::px(off, cx + 6, cy);
    const auto near_on = WarpScene::px(on, cx + 6, cy);
    const auto corner_off = WarpScene::px(off, 2, 2);
    const auto corner_on = WarpScene::px(on, 2, 2);
    const auto centre_on = WarpScene::px(on, cx, cy);
    std::printf("bloom: near %d -> %d, corner %d -> %d, centre %d\n", near_off[1], near_on[1], corner_off[1],
                corner_on[1], centre_on[1]);
    GIANT_CHECK(near_on[1] > near_off[1] + 10); // the glow round the ball
    GIANT_CHECK(near_on[0] == near_on[2] || std::abs(near_on[0] - near_on[2]) <= 2); // white light, white glow
    GIANT_CHECK(near_on[1] - near_off[1] > corner_on[1] - corner_off[1]); // more near than far
    GIANT_CHECK(centre_on[1] > 240);                                         // the ball itself still blazes
    // AgX: different from PBR Neutral, grey kept grey.
    frame.settings.bloom = false;
    frame.settings.tone_mapper = ToneMapper::agx;
    const auto agx = w.draw(frame, 2);
    const auto ground_agx = WarpScene::px(agx, 2, 2);
    std::printf("AgX: ground %d %d %d (PBR Neutral %d)\n", ground_agx[0], ground_agx[1], ground_agx[2], corner_off[1]);
    GIANT_CHECK(std::abs(ground_agx[0] - ground_agx[1]) <= 2 && std::abs(ground_agx[1] - ground_agx[2]) <= 2);
    GIANT_CHECK(ground_agx[1] != corner_off[1]);
    GIANT_CHECK(ground_agx[1] > 20 && WarpScene::px(agx, cx, cy)[1] > 220);
    GIANT_CHECK(!w.device->device_lost());
    GIANT_CHECK(w.device->validation_errors() == 0);
}

namespace {

// A 4x4 block of one colour per 4x4 texels, as D3D lays them out.
std::vector<u8> solid_bc1(u16 rgb565) {
    return {static_cast<u8>(rgb565 & 0xFF), static_cast<u8>(rgb565 >> 8), static_cast<u8>(rgb565 & 0xFF),
            static_cast<u8>(rgb565 >> 8), 0, 0, 0, 0};
}
std::vector<u8> solid_bc4(u8 value) { return {value, value, 0, 0, 0, 0, 0, 0}; }

rhi::TextureDesc material_desc(u32 width, u32 height, rhi::Format format) {
    rhi::TextureDesc d;
    d.width = width;
    d.height = height;
    d.format = format;
    d.usage = rhi::TextureUsage::sampled;
    return d;
}

} // namespace

GIANT_TEST("render: textures for materials are added, refused, removed and reused") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    auto device = rhi::create_device(desc);
    auto renderer = Renderer::create(*device);
    GIANT_CHECK(renderer != nullptr);
    if (renderer == nullptr) return;

    const std::vector<u8> block = solid_bc1(0xF800);
    const TextureId red = renderer->add_texture(material_desc(4, 4, rhi::Format::bc1_srgb), block);
    GIANT_CHECK(red != k_no_texture);
    GIANT_CHECK(renderer->texture(red) != nullptr && renderer->texture(red)->format == rhi::Format::bc1_srgb);
    // Refused: no contents for a compressed texture, contents that do not
    // fit, an array, a render target, a size rhi refuses.
    GIANT_CHECK(renderer->add_texture(material_desc(4, 4, rhi::Format::bc1_srgb), {}) == k_no_texture);
    GIANT_CHECK(renderer->add_texture(material_desc(8, 4, rhi::Format::bc1_srgb), block) == k_no_texture);
    rhi::TextureDesc array = material_desc(4, 4, rhi::Format::bc1_srgb);
    array.array = true;
    GIANT_CHECK(renderer->add_texture(array, block) == k_no_texture);
    rhi::TextureDesc target = material_desc(4, 4, rhi::Format::rgba8_srgb);
    target.usage = rhi::TextureUsage::render_target;
    GIANT_CHECK(renderer->add_texture(target, {}) == k_no_texture);
    GIANT_CHECK(renderer->add_texture(material_desc(6, 4, rhi::Format::bc4_unorm), solid_bc4(9)) == k_no_texture);
    const std::vector<u8> plain(16 * 4, 200);
    const TextureId rgba = renderer->add_texture(material_desc(4, 4, rhi::Format::rgba8_srgb), plain);
    GIANT_CHECK(rgba != k_no_texture && rgba != red);

    // Materials using a live, a removed and an unknown texture all draw.
    rhi::TextureDesc t;
    t.width = 64;
    t.height = 64;
    t.format = rhi::Format::rgba8_srgb;
    t.usage = rhi::TextureUsage::render_target;
    const rhi::TextureHandle image = device->create_texture(t);
    std::vector<Material> materials(3);
    materials[0].base_color_texture = red;
    materials[0].normal_texture = rgba;
    materials[1].roughness_metal_texture = rgba;
    materials[1].occlusion_texture = 12345;
    std::vector<Instance> instances(3);
    for (u32 i = 0; i < 3; ++i) {
        instances[i].mesh = renderer->builtin(BuiltinMesh::cube);
        instances[i].position = {static_cast<double>(i) * 1.5 - 1.5, 0.0, -4.0};
        instances[i].material = i;
    }
    FrameInput frame;
    frame.instances = instances;
    frame.materials = materials;
    const auto draw = [&] {
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        GIANT_CHECK(cmd != nullptr);
        if (cmd == nullptr) return;
        renderer->render(*cmd, image, frame);
        device->end_frame();
    };
    draw();
    renderer->remove_texture(red);
    GIANT_CHECK(renderer->texture(red) == nullptr);
    renderer->remove_texture(red); // again: ignored
    renderer->remove_texture(k_no_texture);
    draw(); // material 0 now has no base colour texture
    // The removed slot is handed out again.
    // The slot is handed out again under a new id: the old one stays dead.
    const TextureId again = renderer->add_texture(material_desc(4, 4, rhi::Format::bc4_unorm), solid_bc4(64));
    GIANT_CHECK(again != k_no_texture && again != red);
    GIANT_CHECK((again & 0xFFFFFu) == (red & 0xFFFFFu));
    GIANT_CHECK(renderer->texture(red) == nullptr && renderer->texture(again) != nullptr);
    renderer->remove_texture(red); // the old id does not remove the new texture
    GIANT_CHECK(renderer->texture(again) != nullptr);
    // A texture in the wrong colour space is left out (logged once): a
    // linear one as base colour, an sRGB one as a normal map.
    materials[2].base_color_texture = again;
    materials[2].normal_texture = rgba;
    draw();
    GIANT_CHECK(renderer->texture(12345) == nullptr && renderer->texture(k_no_texture) == nullptr);
    device->destroy(image);
}

GIANT_TEST("render: streamed textures change levels under a budget, oldest first, and report where they stand") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    auto device = rhi::create_device(desc);
    RendererDesc rd;
    rd.texture_stream_bytes_per_frame = 64; // two rows of the top level a frame
    rd.texture_changes_per_frame = 1;
    auto renderer = Renderer::create(*device, rd);
    GIANT_CHECK(renderer != nullptr);
    if (renderer == nullptr) return;

    // Sizes: 8x8 rgba8 with 4 levels (256, 64, 16, 4 bytes).
    rhi::TextureDesc d = material_desc(8, 8, rhi::Format::rgba8_unorm);
    d.mips = 4;
    GIANT_CHECK(texture_from_mip(d, 2).width == 2 && texture_from_mip(d, 2).mips == 2);
    GIANT_CHECK(texture_from_mip(d, 9).width == 1 && texture_from_mip(d, 9).mips == 1);
    GIANT_CHECK(coarsest_first_mip(d) == 3);
    rhi::TextureDesc bc = material_desc(16, 16, rhi::Format::bc7_unorm);
    bc.mips = 5;
    GIANT_CHECK(coarsest_first_mip(bc) == 2); // 4x4: whole blocks
    bc.width = 24;                             // 24, 12, 6: level 2 is not whole blocks
    bc.mips = 4;
    GIANT_CHECK(coarsest_first_mip(bc) == 1);
    std::vector<u8> contents(rhi::texture_bytes(d));
    for (usize i = 0; i < contents.size(); ++i) contents[i] = static_cast<u8>(i);
    const std::span<const u8> all(contents);
    {
        // Refused: past the coarsest level, contents not of the levels given.
        GIANT_CHECK(renderer->add_texture(d, all.subspan(320), 4) == k_no_texture);
        GIANT_CHECK(renderer->add_texture(d, all, 2) == k_no_texture);
        GIANT_CHECK(renderer->add_texture(d, {}, 2) == k_no_texture);
    }
    const TextureId a = renderer->add_texture(d, all.subspan(320), 2); // 2x2 and 1x1
    const TextureId b = renderer->add_texture(d, all.subspan(256), 1);
    GIANT_CHECK(a != k_no_texture && b != k_no_texture);
    GIANT_CHECK(renderer->texture(a) != nullptr && renderer->texture(a)->width == 8 && renderer->texture(a)->mips == 4);
    std::optional<TextureResidency> r = renderer->texture_residency(a);
    GIANT_CHECK(r && r->first_mip == 2 && r->target_mip == 2 && r->bytes == 20 &&
                r->wanted_mip == TextureResidency::k_not_seen);

    // Refused changes: not finer, the wrong size, an unknown id, trims
    // past the coarsest level.
    GIANT_CHECK(!renderer->stream_texture(a, 2, {}));
    GIANT_CHECK(!renderer->stream_texture(a, 0, std::vector<u8>(100)));
    GIANT_CHECK(!renderer->stream_texture(12345, 0, {}));
    GIANT_CHECK(!renderer->trim_texture(a, 4) && !renderer->trim_texture(a, 1) && !renderer->trim_texture(777, 3));

    rhi::TextureDesc t;
    t.width = 32;
    t.height = 32;
    t.format = rhi::Format::rgba8_srgb;
    t.usage = rhi::TextureUsage::render_target;
    const rhi::TextureHandle image = device->create_texture(t);
    std::vector<Material> materials(2);
    materials[0].base_color_texture = k_no_texture;
    materials[0].roughness_metal_texture = a;
    materials[1].occlusion_texture = b;
    FrameInput frame;
    frame.materials = materials;
    const auto draw = [&] {
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        GIANT_CHECK(cmd != nullptr);
        if (cmd == nullptr) return RenderStats{};
        renderer->render(*cmd, image, frame);
        device->end_frame();
        return renderer->last_stats();
    };
    RenderStats st = draw();
    GIANT_CHECK(st.textures == 2 && st.texture_bytes == 20 + 84 && st.textures_changing == 0);

    // a: levels 0 and 1 (320 bytes) at 64 a frame; b, queued after it,
    // waits. The top level's rows are 32 bytes, level 1's 16.
    GIANT_CHECK(renderer->stream_texture(a, 0, std::vector<u8>(contents.begin(), contents.begin() + 320)));
    GIANT_CHECK(renderer->stream_texture(b, 0, std::vector<u8>(contents.begin(), contents.begin() + 256)));
    r = renderer->texture_residency(a);
    GIANT_CHECK(r && r->first_mip == 2 && r->target_mip == 0);
    u32 frames = 0;
    u64 most = 0;
    while (renderer->texture_residency(a)->first_mip != 0 && frames < 20) {
        st = draw();
        most = std::max(most, st.texture_stream_bytes);
        ++frames;
        GIANT_CHECK(renderer->texture_residency(b)->first_mip == 1 || renderer->texture_residency(a)->first_mip == 0);
    }
    GIANT_CHECK(frames == 5 && most == 64); // 320 / 64
    r = renderer->texture_residency(a);
    GIANT_CHECK(r && r->first_mip == 0 && r->target_mip == 0 && r->bytes == 340);
    for (u32 i = 0; i < 10 && renderer->texture_residency(b)->first_mip != 0; ++i) draw();
    GIANT_CHECK(renderer->texture_residency(b)->first_mip == 0 && renderer->last_stats().texture_bytes == 680);

    // A trim waits for the next render(), then copies (no upload).
    GIANT_CHECK(renderer->trim_texture(a, 3));
    GIANT_CHECK(renderer->texture_residency(a)->first_mip == 0 && renderer->texture_residency(a)->target_mip == 3);
    st = draw();
    GIANT_CHECK(renderer->texture_residency(a)->first_mip == 3 && renderer->texture_residency(a)->bytes == 4);
    GIANT_CHECK(st.texture_stream_bytes == 0 && st.texture_bytes == 340 + 4);
    // A change given up: a stream replaced by a trim to where it is.
    GIANT_CHECK(renderer->stream_texture(a, 1, std::vector<u8>(contents.begin() + 256, contents.begin() + 336)));
    draw(); // under way: 64 of 80 bytes
    GIANT_CHECK(renderer->texture_residency(a)->first_mip == 3 && renderer->texture_residency(a)->target_mip == 1);
    GIANT_CHECK(renderer->trim_texture(a, 3));
    GIANT_CHECK(renderer->texture_residency(a)->target_mip == 3);
    st = draw();
    GIANT_CHECK(renderer->texture_residency(a)->first_mip == 3 && st.texture_stream_bytes == 0 &&
                st.textures_changing == 0);
    // A texture removed mid-change; its slot's next texture starts clean.
    GIANT_CHECK(renderer->stream_texture(a, 0, std::vector<u8>(contents.begin(), contents.begin() + 336)));
    draw();
    renderer->remove_texture(a);
    GIANT_CHECK(!renderer->texture_residency(a));
    const TextureId c = renderer->add_texture(d, all.subspan(320), 2);
    GIANT_CHECK((c & 0xFFFFFu) == (a & 0xFFFFFu));
    st = draw();
    GIANT_CHECK(renderer->texture_residency(c)->first_mip == 2 && st.texture_stream_bytes == 0);
    GIANT_CHECK(st.texture_bytes == 20 + 340);

    // Past the materials the GPU is asked about, a textured material's
    // textures count as wanting their finest level.
    std::vector<Material> many(65537);
    many.back().base_color_texture = c;
    frame.materials = many;
    draw();
    GIANT_CHECK(renderer->texture_residency(c)->wanted_mip == 0);
    GIANT_CHECK(renderer->texture_residency(b)->wanted_mip == TextureResidency::k_not_seen); // (the null GPU asks nothing)
    // Unused for two windows (64 frames), it is not wanted.
    frame.materials = materials;
    for (int i = 0; i < 70; ++i) draw();
    GIANT_CHECK(renderer->texture_residency(c)->wanted_mip == TextureResidency::k_not_seen);
    device->destroy(image);
}

#if GIANT_RHI_HAS_D3D12
GIANT_TEST("render: d3d12 materials sample colour, normal and occlusion textures") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::d3d12;
    desc.software_adapter = true;
    desc.debug_layer = true;
    auto device = rhi::create_device(desc);
    GIANT_CHECK(device != nullptr);
    if (device == nullptr) return;
    auto renderer = Renderer::create(*device);
    GIANT_CHECK(renderer != nullptr);
    if (renderer == nullptr) return;
    constexpr u32 k_w = 96, k_h = 64;
    rhi::TextureDesc target_desc;
    target_desc.width = k_w;
    target_desc.height = k_h;
    target_desc.format = rhi::Format::rgba8_srgb;
    target_desc.usage = rhi::TextureUsage::render_target;
    const rhi::TextureHandle target = device->create_texture(target_desc);

    const auto bc1_blocks = [](std::initializer_list<u16> colours) {
        std::vector<u8> out;
        for (const u16 c : colours) {
            const auto b = solid_bc1(c);
            out.insert(out.end(), b.begin(), b.end());
        }
        return out;
    };
    // 8x8 BC1: red on the left half (u < 0.5), blue on the right.
    const TextureId colour = renderer->add_texture(material_desc(8, 8, rhi::Format::bc1_srgb),
                                                   bc1_blocks({0xF800, 0x001F, 0xF800, 0x001F}));
    // 16x16 with 3 mips: red, then green, then blue.
    rhi::TextureDesc mipped_desc = material_desc(16, 16, rhi::Format::bc1_srgb);
    mipped_desc.mips = 3;
    std::vector<u8> mipped = bc1_blocks({0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800,
                                         0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, // 16x16
                                         0x07E0, 0x07E0, 0x07E0, 0x07E0,                         // 8x8
                                         0x001F});                                               // 4x4
    const TextureId levels = renderer->add_texture(mipped_desc, mipped);
    // Normal maps: tilted 45 degrees toward +u, -u and +v-up (green).
    const auto bc5 = [](u8 x, u8 y) {
        return std::vector<u8>{x, x, 0, 0, 0, 0, 0, 0, y, y, 0, 0, 0, 0, 0, 0};
    };
    const TextureId toward = renderer->add_texture(material_desc(4, 4, rhi::Format::bc5_unorm), bc5(217, 128));
    const TextureId away = renderer->add_texture(material_desc(4, 4, rhi::Format::bc5_unorm), bc5(38, 128));
    const TextureId up = renderer->add_texture(material_desc(4, 4, rhi::Format::bc5_unorm), bc5(128, 217));
    const TextureId dim = renderer->add_texture(material_desc(4, 4, rhi::Format::bc4_unorm), solid_bc4(64));
    GIANT_CHECK(colour != k_no_texture && levels != k_no_texture && toward != k_no_texture &&
                away != k_no_texture && up != k_no_texture && dim != k_no_texture);

    // A 2 m plane turned to face the camera: +u to the right, the image's
    // top (+v up the image) up.
    const DVec3 camera{1000.0, 5.0, -2000.0};
    std::vector<Instance> instances(1);
    instances[0].mesh = renderer->builtin(BuiltinMesh::plane);
    instances[0].rotation = Quat{0.70710678f, 0.0f, 0.0f, 0.70710678f};
    instances[0].scale = {2.0f, 2.0f, 2.0f};
    std::vector<Material> materials(1);
    Material plain;
    plain.base_color = {1.0f, 1.0f, 1.0f};
    plain.roughness = 0.9f;

    FrameInput frame;
    frame.settings.auto_exposure = false;
    frame.settings.bloom = false;
    frame.settings.shadows = false;
    frame.settings.gi = false;
    frame.settings.physical_sky = false; // the flat sky and ground light below
    frame.settings.reflections = false;
    frame.settings.screen_space_reflections = false;
    frame.settings.specular_antialiasing = false;
    frame.view.position = camera;
    frame.instances = instances;
    frame.materials = materials;
    frame.lighting.background = {0.0f, 0.0f, 0.0f};
    const auto draw = [&](const Material& m, double distance, const Vec3& sun_direction, float sun, float sky) {
        materials[0] = m;
        instances[0].position = camera + DVec3{0.0, 0.0, -distance};
        frame.lighting.sun_direction = sun_direction;
        frame.lighting.sun_light = {sun, sun, sun};
        frame.lighting.sky_light = {sky, sky, sky};
        frame.lighting.ground_light = {sky, sky, sky};
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
        return device->read_texture(target);
    };
    const auto pixel = [](const std::vector<u8>& image, u32 x, u32 y) {
        const usize i = (static_cast<usize>(y) * k_w + x) * 4;
        return i + 3 < image.size() ? std::array<int, 3>{image[i], image[i + 1], image[i + 2]}
                                    : std::array<int, 3>{-1, -1, -1};
    };
    const auto centre = [&](const Material& m, const Vec3& sun_direction, float sun, float sky) {
        return pixel(draw(m, 2.0, sun_direction, sun, sky), k_w / 2, k_h / 2)[0];
    };
    const Vec3 head_on{0.0f, 0.0f, 1.0f};

    // Colour: red left, blue right, sampled a quarter of the plane's width
    // (0.5 m) either side of the centre. 2 m off, the view (60 degrees
    // tall) is 2 tan(30) * 2 m * 96 / 64 = 3.46 m wide.
    Material coloured = plain;
    coloured.base_color_texture = colour;
    const auto halves = draw(coloured, 2.0, head_on, 3.0f, 0.02f);
    const double pixels_per_metre = k_w / (2.0 * std::tan(30.0 * 3.14159265 / 180.0) * 2.0 * k_w / k_h);
    const u32 quarter = static_cast<u32>(std::lround(0.5 * pixels_per_metre));
    const auto left = pixel(halves, k_w / 2 - quarter, k_h / 2);
    const auto right = pixel(halves, k_w / 2 + quarter, k_h / 2);
    std::printf("    quarter %u px: left %d %d %d, right %d %d %d\n", quarter, left[0], left[1], left[2], right[0],
                right[1], right[2]);
    GIANT_CHECK(left[0] > 120 && left[1] < 40 && left[2] < 40);
    GIANT_CHECK(right[2] > 120 && right[0] < 40 && right[1] < 40);

    // Mips from the pixel's footprint: near, the 16 texels span about 55
    // pixels (the full-size red level); 30 m off, about 3.7 (past the
    // smallest, blue).
    Material mip_test = plain;
    mip_test.base_color_texture = levels;
    const auto near = pixel(draw(mip_test, 2.0, head_on, 3.0f, 0.02f), k_w / 2, k_h / 2);
    const auto far = pixel(draw(mip_test, 30.0, head_on, 3.0f, 0.02f), k_w / 2, k_h / 2);
    std::printf("    mips: near %d %d %d, far %d %d %d\n", near[0], near[1], near[2], far[0], far[1], far[2]);
    GIANT_CHECK(near[0] > 120 && near[1] < 40 && near[2] < 40);
    GIANT_CHECK(far[2] > 60 && far[2] > far[0] * 2 && far[2] > far[1]);

    // Normal maps: with the sun 45 degrees right, tilted toward it the plane
    // is brighter than flat, away darker; with it 45 degrees above, a
    // tilt up the image (green) brightens it.
    const Vec3 sun_right{0.70710678f, 0.0f, 0.70710678f};
    const Vec3 sun_above{0.0f, 0.70710678f, 0.70710678f};
    Material bent = plain;
    bent.normal_texture = toward;
    Material turned = plain;
    turned.normal_texture = away;
    Material raised = plain;
    raised.normal_texture = up;
    const int flat_lum = centre(plain, sun_right, 3.0f, 0.02f);
    const int toward_lum = centre(bent, sun_right, 3.0f, 0.02f);
    const int away_lum = centre(turned, sun_right, 3.0f, 0.02f);
    const int flat_above = centre(plain, sun_above, 3.0f, 0.02f);
    const int up_lum = centre(raised, sun_above, 3.0f, 0.02f);
    const int up_right = centre(raised, sun_right, 3.0f, 0.02f);
    std::printf("    normal maps: flat %d, toward %d, away %d; above: flat %d, up %d; up lit from the right %d\n",
                flat_lum, toward_lum, away_lum, flat_above, up_lum, up_right);
    GIANT_CHECK(toward_lum * 100 > flat_lum * 105);
    GIANT_CHECK(away_lum * 2 < flat_lum);
    GIANT_CHECK(up_lum * 100 > flat_above * 105);
    GIANT_CHECK(up_right < flat_lum); // a tilt up is a tilt away from a sun on the right

    // Occlusion dims the ambient light, never the sun's.
    Material occluded = plain;
    occluded.occlusion_texture = dim;
    const int open_lum = centre(plain, sun_right, 0.0f, 0.5f);
    const int shut_lum = centre(occluded, sun_right, 0.0f, 0.5f);
    const int sun_open = centre(plain, sun_right, 3.0f, 0.0f);
    const int sun_shut = centre(occluded, sun_right, 3.0f, 0.0f);
    std::printf("    occlusion: ambient %d -> %d, sun %d -> %d\n", open_lum, shut_lum, sun_open, sun_shut);
    GIANT_CHECK(open_lum > 40 && shut_lum * 100 < open_lum * 75);
    GIANT_CHECK(std::abs(sun_open - sun_shut) <= 3);

    // Streaming: the same three levels, only the smallest (blue) at first.
    const std::span<const u8> all_levels(mipped);
    const TextureId streamed = renderer->add_texture(mipped_desc, all_levels.subspan(20 * 8), 2);
    GIANT_CHECK(streamed != k_no_texture);
    Material streaming = plain;
    streaming.base_color_texture = streamed;
    // The feedback says which level the pixels need: 30 m off, 16 texels
    // over about 3.7 pixels want level 2 (4 texels); 2 m off, level 0.
    const auto far_streamed = pixel(draw(streaming, 30.0, head_on, 3.0f, 0.02f), k_w / 2, k_h / 2);
    const std::optional<TextureResidency> far_wanted = renderer->texture_residency(streamed);
    const auto near_blue = pixel(draw(streaming, 2.0, head_on, 3.0f, 0.02f), k_w / 2, k_h / 2);
    const std::optional<TextureResidency> near_wanted = renderer->texture_residency(streamed);
    GIANT_CHECK(far_wanted && near_wanted);
    if (far_wanted && near_wanted) {
        std::printf("    streaming: wanted far %u, near %u; first mip %u\n", far_wanted->wanted_mip,
                    near_wanted->wanted_mip, near_wanted->first_mip);
        GIANT_CHECK(far_wanted->wanted_mip == 2 && near_wanted->wanted_mip == 0 && near_wanted->first_mip == 2);
    }
    GIANT_CHECK(far_streamed[2] > 60 && near_blue[2] > 120 && near_blue[0] < 40); // only blue so far
    // Its finer levels streamed in: red near; trimmed to level 1: green.
    GIANT_CHECK(renderer->stream_texture(streamed, 0, std::vector<u8>(mipped.begin(), mipped.begin() + 20 * 8)));
    const auto near_red = pixel(draw(streaming, 2.0, head_on, 3.0f, 0.02f), k_w / 2, k_h / 2);
    GIANT_CHECK(renderer->texture_residency(streamed)->first_mip == 0);
    GIANT_CHECK(renderer->trim_texture(streamed, 1));
    const auto near_green = pixel(draw(streaming, 2.0, head_on, 3.0f, 0.02f), k_w / 2, k_h / 2);
    GIANT_CHECK(renderer->texture_residency(streamed)->first_mip == 1);
    std::printf("    streamed: red %d %d %d, trimmed %d %d %d\n", near_red[0], near_red[1], near_red[2],
                near_green[0], near_green[1], near_green[2]);
    GIANT_CHECK(near_red[0] > 120 && near_red[1] < 40 && near_red[2] < 40);
    GIANT_CHECK(near_green[1] > 120 && near_green[0] < 40 && near_green[2] < 40);
    GIANT_CHECK(!device->device_lost() && device->validation_errors() == 0);
    device->destroy(target);
}

GIANT_TEST("render: d3d12 rough metal keeps its light: the split sum with multi-scatter put back") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::d3d12;
    desc.software_adapter = true;
    desc.debug_layer = true;
    auto device = rhi::create_device(desc);
    if (device == nullptr) return; // not on Windows
    auto renderer = Renderer::create(*device);
    GIANT_CHECK(renderer != nullptr);
    if (renderer == nullptr) return;
    constexpr u32 k_w = 64, k_h = 64;
    rhi::TextureDesc target_desc;
    target_desc.width = k_w;
    target_desc.height = k_h;
    target_desc.format = rhi::Format::rgba8_srgb;
    target_desc.usage = rhi::TextureUsage::render_target;
    const rhi::TextureHandle target = device->create_texture(target_desc);
    // A plane facing the camera in an even grey surround (sky and ground
    // alike), no sun: a white furnace for its reflections.
    const DVec3 camera{0.0, 5.0, 0.0};
    std::vector<Instance> instances(1);
    instances[0].mesh = renderer->builtin(BuiltinMesh::plane);
    instances[0].rotation = Quat{0.70710678f, 0.0f, 0.0f, 0.70710678f};
    instances[0].scale = {4.0f, 4.0f, 4.0f};
    instances[0].position = camera + DVec3{0.0, 0.0, -2.0};
    std::vector<Material> materials(1);
    FrameInput frame;
    frame.settings.auto_exposure = false;
    frame.settings.bloom = false;
    frame.settings.shadows = false;
    frame.settings.gi = false;
    frame.settings.physical_sky = false;
    frame.settings.reflections = false;
    frame.settings.screen_space_reflections = false;
    frame.settings.specular_antialiasing = false;
    frame.view.position = camera;
    frame.instances = instances;
    frame.materials = materials;
    frame.lighting.background = {0.0f, 0.0f, 0.0f};
    frame.lighting.sun_direction = {0.0f, 1.0f, 0.0f};
    frame.lighting.sun_light = {0.0f, 0.0f, 0.0f};
    frame.lighting.sky_light = {0.3f, 0.3f, 0.3f};
    frame.lighting.ground_light = {0.3f, 0.3f, 0.3f};
    const auto brightness = [&](float base, float roughness, float metal) {
        materials[0] = Material{};
        materials[0].base_color = {base, base, base};
        materials[0].roughness = roughness;
        materials[0].metallic = metal;
        for (int i = 0; i < 6; ++i) {
            rhi::CommandList* cmd = device->begin_frame(nullptr);
            GIANT_CHECK(cmd != nullptr);
            if (cmd != nullptr) {
                renderer->render(*cmd, target, frame);
                device->end_frame();
            }
        }
        device->wait_idle();
        const std::vector<u8> image = device->read_texture(target);
        const usize i = (static_cast<usize>(k_h / 2) * k_w + k_w / 2) * 4;
        return i + 3 < image.size() ? static_cast<int>(image[i + 1]) : -1;
    };
    const int smooth = brightness(1.0f, 0.1f, 1.0f);
    const int medium = brightness(1.0f, 0.5f, 1.0f);
    const int rough = brightness(1.0f, 1.0f, 1.0f);
    const int white = brightness(1.0f, 1.0f, 0.0f);
    const int grey_metal = brightness(0.5f, 1.0f, 1.0f);
    const int grey_smooth = brightness(0.5f, 0.1f, 1.0f);
    std::printf("    white metal: smooth %d, medium %d, rough %d; rough white dielectric %d; grey metal: rough %d, "
                "smooth %d\n",
                smooth, medium, rough, white, grey_metal, grey_smooth);
    GIANT_CHECK(smooth > 20);
    // A white metal reflects everything at any roughness (one bounce keeps
    // only 31% head on at roughness 1; the rest comes back).
    GIANT_CHECK(std::abs(rough - smooth) * 100 <= smooth * 8 && std::abs(medium - smooth) * 100 <= smooth * 8);
    // A white dielectric keeps about as much (diffuse takes what the
    // highlight leaves), never more.
    GIANT_CHECK(white <= smooth + 3 && white * 100 >= smooth * 85);
    // A grey metal (f0 0.5) loses light with roughness, as the table says:
    // head on at roughness 1 one bounce keeps 0.31 of f0, multi-scatter
    // adds about 0.09 (0.24 of the surround in all, some 51 in sRGB),
    // where smooth it keeps 0.5 (about 93; without the table both would).
    GIANT_CHECK(grey_metal < rough - 10);
    GIANT_CHECK(grey_metal >= 38 && grey_metal <= 66);
    GIANT_CHECK(grey_smooth - grey_metal > 20);
    GIANT_CHECK(!device->device_lost() && device->validation_errors() == 0);
    device->destroy(target);
}
GIANT_TEST("render: d3d12 ambient occlusion darkens the crease where a wall meets the floor, not the open wall") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::d3d12;
    desc.software_adapter = true;
    desc.debug_layer = true;
    auto device = rhi::create_device(desc);
    if (device == nullptr) return; // not on Windows
    auto renderer = Renderer::create(*device);
    GIANT_CHECK(renderer != nullptr);
    if (renderer == nullptr) return;
    constexpr u32 k_w = 128, k_h = 128;
    rhi::TextureDesc target_desc;
    target_desc.width = k_w;
    target_desc.height = k_h;
    target_desc.format = rhi::Format::rgba8_srgb;
    target_desc.usage = rhi::TextureUsage::render_target;
    const rhi::TextureHandle target = device->create_texture(target_desc);
    // The camera 1.5 m up, 6 m from a wall whose foot meets the floor:
    // the crease about row 92 (60 degrees tall view).
    const DVec3 camera{500.0, 1.5, 4.0};
    std::vector<Instance> instances(2);
    instances[0].mesh = renderer->builtin(BuiltinMesh::plane);
    instances[0].position = DVec3{500.0, 0.0, 0.0};
    instances[0].scale = {20.0f, 1.0f, 20.0f};
    instances[1].mesh = renderer->builtin(BuiltinMesh::cube);
    instances[1].position = DVec3{500.0, 5.0, -3.0}; // its front face at z -2
    instances[1].scale = {20.0f, 10.0f, 2.0f};
    std::vector<Material> materials(1);
    materials[0].base_color = {0.8f, 0.8f, 0.8f};
    materials[0].roughness = 0.9f;
    FrameInput frame;
    frame.settings.auto_exposure = false;
    frame.settings.bloom = false;
    frame.settings.shadows = false;
    frame.settings.gi = false;
    frame.settings.physical_sky = false;
    frame.settings.reflections = false;
    frame.settings.screen_space_reflections = false;
    frame.settings.antialiasing = false;
    frame.view.position = camera;
    frame.instances = instances;
    frame.materials = materials;
    frame.lighting.background = {0.0f, 0.0f, 0.0f};
    frame.lighting.sun_direction = {0.0f, 1.0f, 0.0f};
    frame.lighting.sun_light = {0.0f, 0.0f, 0.0f};
    frame.lighting.sky_light = {0.5f, 0.5f, 0.5f};
    frame.lighting.ground_light = {0.5f, 0.5f, 0.5f};
    const auto draw = [&](bool ao) {
        frame.settings.ambient_occlusion = ao;
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
        GIANT_CHECK(renderer->last_stats().ambient_occlusion == ao);
        device->wait_idle();
        return device->read_texture(target);
    };
    const auto green = [](const std::vector<u8>& image, u32 y) {
        const usize i = (static_cast<usize>(y) * k_w + k_w / 2) * 4;
        return i + 3 < image.size() ? static_cast<int>(image[i + 1]) : -1;
    };
    const std::vector<u8> off = draw(false);
    const std::vector<u8> on = draw(true);
    // Rows: 40 the open wall (some 2.8 m above the floor), 88 the wall and
    // 94 the floor each a hand's width from the crease.
    std::printf("    open wall %d -> %d; wall by the crease %d -> %d; floor by it %d -> %d\n", green(off, 40),
                green(on, 40), green(off, 88), green(on, 88), green(off, 94), green(on, 94));
    GIANT_CHECK(green(off, 40) > 60 && green(off, 88) > 60 && green(off, 94) > 60);
    GIANT_CHECK(std::abs(green(on, 40) - green(off, 40)) <= 4);
    GIANT_CHECK(green(on, 88) * 100 < green(off, 88) * 92);
    GIANT_CHECK(green(on, 94) * 100 < green(off, 94) * 92);
    GIANT_CHECK(!device->device_lost() && device->validation_errors() == 0);
    device->destroy(target);
}

#endif
