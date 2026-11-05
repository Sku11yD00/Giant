#include "giant/geometry/mesh.h"
#include "giant/render/renderer.h"

#include "giant_test.h"

#include <array>
#include <cmath>
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
    GIANT_CHECK(renderer->last_stats().post_draws == 3); // SMAA: edges, weights, blend
    GIANT_CHECK(device->last_frame_stats().draws == 2 + 3);
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
    GIANT_CHECK(next != id); // never reused
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
        GIANT_CHECK(renderer->last_stats().decals == 1 || gpu);
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
    GIANT_CHECK(renderer->last_stats().post_draws == 0);
    frame.settings.antialiasing = true;
    const auto smoothed = draw(false, false, false);
    GIANT_CHECK(renderer->last_stats().post_draws == 3);
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
    // across each pixel, so the filter must widen (dim) its highlight.
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
