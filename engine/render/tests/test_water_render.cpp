// Water's surfaces (water plan W2a): packing what the caller gives (bad
// triangles dropped, looks kept in range, the caps), the waves (the same
// at a place whatever the camera, moving with time), the pass on the null
// device, and on the software adapter: shallow water shows its bed, deep
// water its own colour, and seen flat it mirrors the sky. Under water (W3):
// the camera's water dims the view where the near plane is under it,
// the surface from below shows Snell's window, waves gather caustics.
#include "giant/render/renderer.h"

#include "giant_test.h"
#include "water.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <vector>

using namespace giant;
using namespace giant::render;

namespace {

// A square of water `half` metres each way round (x, z) at `level`.
void square(std::vector<WaterVertex>& v, std::vector<u32>& idx, DVec3 centre, double half, u32 look = 0) {
    const u32 base = static_cast<u32>(v.size());
    for (const auto& [dx, dz] : {std::pair{-half, -half}, std::pair{half, -half}, std::pair{half, half}, std::pair{-half, half}}) {
        WaterVertex w;
        w.position = centre + DVec3{dx, 0.0, dz};
        w.look = look;
        v.push_back(w);
    }
    idx.insert(idx.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
}

// The waves' height at world (x, z), from a set made for `camera`.
double height(const std::array<gpu::WaterWave, k_water_waves>& waves, const DVec3& camera, double x, double z) {
    double h = 0.0;
    for (const gpu::WaterWave& w : waves) {
        const double rx = x - camera.x, rz = z - camera.z;
        h += w.steepness / w.k * std::sin(w.k * (w.dir_x * rx + w.dir_z * rz) + w.phase);
    }
    return h;
}

} // namespace

GIANT_TEST("water render: packed camera-relative; bad triangles dropped, looks kept in range, past the caps left out") {
    std::vector<WaterVertex> v;
    std::vector<u32> idx;
    const DVec3 camera{1000.0, 5.0, -2000.0};
    square(v, idx, {1000.0, 0.0, -2010.0}, 5.0, 1);
    std::vector<WaterLook> looks(2);
    looks[1].color = {2.0f, -1.0f, std::nanf("")}; // out of range, and bad
    looks[1].absorption = {20.0f, 0.5f, 0.1f};
    looks[1].waves = 9.0f;
    WaterBatch b = pack_water(v, idx, looks, camera);
    GIANT_CHECK(b.vertices.size() == 4 && b.indices.size() == 6 && b.dropped == 0 && !b.overflow);
    GIANT_CHECK(b.looks.size() == 3); // the two, and the default last
    GIANT_CHECK(std::fabs(b.vertices[0].position[0] - -5.0f) < 1e-4f && std::fabs(b.vertices[0].position[1] - -5.0f) < 1e-4f);
    GIANT_CHECK(std::fabs(b.vertices[0].position[2] - -15.0f) < 1e-4f && b.vertices[0].look == 1);
    const WaterLook d;
    GIANT_CHECK(b.looks[1].color[0] == 1.0f && b.looks[1].color[1] == 0.0f && b.looks[1].color[2] == d.color.z);
    GIANT_CHECK(b.looks[1].absorption[0] == 10.0f && b.looks[1].waves == 4.0f);
    // A look past those given: the default; an index past the vertices, or a
    // bad vertex: its triangle dropped (the others kept).
    v[0].look = 7;
    idx.insert(idx.end(), {0, 1, 99});
    b = pack_water(v, idx, looks, camera);
    GIANT_CHECK(b.vertices[0].look == 2 && b.indices.size() == 6 && b.dropped == 1);
    v[3].position.y = std::numeric_limits<double>::quiet_NaN();
    b = pack_water(v, idx, looks, camera);
    GIANT_CHECK(b.indices.size() == 3 && b.dropped == 2); // (the second triangle used vertex 3)
    // An incomplete last triangle is no triangle.
    v[3].position.y = 0.0;
    idx.push_back(1);
    b = pack_water(v, idx, looks, camera);
    GIANT_CHECK(b.indices.size() == 6 && b.dropped == 1);
    // Past the vertex cap: those left out, and their triangles, are overflow (not dropped).
    std::vector<WaterVertex> many(k_max_water_vertices + 3);
    std::vector<u32> tri{0, 1, 2, k_max_water_vertices, k_max_water_vertices + 1, k_max_water_vertices + 2};
    b = pack_water(many, tri, {}, {});
    GIANT_CHECK(b.overflow && b.dropped == 0 && b.indices.size() == 3 && b.vertices.size() == k_max_water_vertices);
    GIANT_CHECK(b.looks.size() == 1);
    // Past the triangle cap.
    std::vector<u32> lots(static_cast<usize>(k_max_water_triangles + 1) * 3, 0);
    b = pack_water(many, lots, {}, {});
    GIANT_CHECK(b.overflow && b.indices.size() == static_cast<usize>(k_max_water_triangles) * 3);
}

GIANT_TEST("water render: waves are the same at a place whatever the camera, move with time, and grow with the wind") {
    const Vec3 breeze{4.0f, 0.0f, 3.0f}; // 5 m/s
    const DVec3 near{10.0, 2.0, 20.0};
    const auto a = water_waves(breeze, 100.0, near);
    // The same point's height from two cameras a few hundred metres apart,
    // near the origin and far out (the phase holds the camera's place).
    for (const DVec3 base : {near, DVec3{1'000'000.0, 2.0, -3'000'000.0}}) {
        const DVec3 other = base + DVec3{310.0, 0.0, -170.0};
        const auto wa = water_waves(breeze, 100.0, base);
        const auto wb = water_waves(breeze, 100.0, other);
        for (const auto& [x, z] : {std::pair{12.0, 25.0}, std::pair{150.0, -90.0}}) {
            const double ha = height(wa, base, base.x + x, base.z + z);
            const double hb = height(wb, other, base.x + x, base.z + z);
            GIANT_CHECK(std::fabs(ha - hb) < 1e-3);
        }
    }
    for (const gpu::WaterWave& w : a) {
        GIANT_CHECK(w.phase >= 0.0f && w.phase < 6.2832f && w.k > 0.0f && w.steepness > 0.0f);
        GIANT_CHECK(std::fabs(w.dir_x * w.dir_x + w.dir_z * w.dir_z - 1.0f) < 1e-4f);
    }
    // The longest wave runs with the wind.
    GIANT_CHECK(a[0].dir_x > 0.79f && a[0].dir_z > 0.59f);
    // Later: each wave has moved on by its speed (deep water: omega = sqrt(g k)).
    const auto later = water_waves(breeze, 101.0, near);
    for (u32 i = 0; i < k_water_waves; ++i) {
        const double moved = std::remainder(static_cast<double>(a[i].phase) - later[i].phase, 6.283185307179586);
        const double omega = std::sqrt(9.81 * a[i].k);
        GIANT_CHECK(std::fabs(moved - std::remainder(omega, 6.283185307179586)) < 1e-3);
    }
    // A storm's waves are longer and steeper than a calm's ripples; a bad wind is calm.
    const auto calm = water_waves({0.0f, 0.0f, 0.0f}, 0.0, near);
    const auto storm = water_waves({20.0f, 0.0f, 0.0f}, 0.0, near);
    GIANT_CHECK(storm[0].k < calm[0].k && storm[0].steepness > calm[0].steepness && calm[0].steepness > 0.0f);
    const auto bad = water_waves({std::nanf(""), 0.0f, 0.0f}, std::numeric_limits<double>::infinity(), near);
    GIANT_CHECK(bad[0].k == calm[0].k && std::isfinite(bad[0].phase));
}

GIANT_TEST("water render: drawn after shading in lit views, two passes, only with water (null device)") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    auto device = rhi::create_device(desc);
    auto renderer = Renderer::create(*device);
    GIANT_CHECK(renderer != nullptr);
    if (renderer == nullptr) return;
    rhi::TextureDesc target_desc;
    target_desc.width = 32;
    target_desc.height = 16;
    target_desc.format = rhi::Format::rgba8_srgb;
    target_desc.usage = rhi::TextureUsage::render_target;
    const rhi::TextureHandle target = device->create_texture(target_desc);
    std::vector<Instance> instances(1);
    instances[0].mesh = renderer->builtin(BuiltinMesh::cube);
    instances[0].position = {0.0, -2.0, -6.0};
    std::vector<WaterVertex> v;
    std::vector<u32> idx;
    square(v, idx, {0.0, -1.0, -6.0}, 20.0);
    FrameInput frame;
    frame.instances = instances;
    const auto draw = [&] {
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        GIANT_CHECK(cmd != nullptr);
        if (cmd != nullptr) {
            renderer->render(*cmd, target, frame);
            device->end_frame();
        }
        return renderer->last_stats();
    };
    const u32 without = draw().post_draws;
    GIANT_CHECK(draw().water_triangles == 0);
    frame.water_vertices = v;
    frame.water_indices = idx;
    RenderStats s = draw();
    GIANT_CHECK(s.water_triangles == 2 && s.post_draws == without + 2 && !s.water_overflow && s.water_dropped == 0);
    // Timed as its own pass.
    bool timed = false;
    for (const rhi::GpuTiming& t : device->gpu_timings()) timed = timed || std::string(t.name) == "water";
    GIANT_CHECK(timed || device->gpu_timings().empty());
    // Off, or a view that is not lit: none.
    frame.settings.water = false;
    s = draw();
    GIANT_CHECK(s.water_triangles == 0 && s.post_draws == without);
    frame.settings.water = true;
    frame.settings.debug_view = DebugView::clusters;
    GIANT_CHECK(draw().water_triangles == 0);
    frame.settings.debug_view = DebugView::lit;
    // Only bad triangles: none drawn, counted.
    std::vector<u32> bad{0, 1, 77};
    frame.water_indices = bad;
    s = draw();
    GIANT_CHECK(s.water_triangles == 0 && s.water_dropped == 1 && s.post_draws == without);
    GIANT_CHECK(!device->device_lost());
    device->destroy(target);
}

GIANT_TEST("water render: d3d12 shallow water shows its bed, deep water its own colour, and seen flat it mirrors the sky") {
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
    // A white floor; the water over it 0.3 m deep (x < 500) or 30 m deep (x >= 500, the floor lower).
    std::vector<Instance> instances(4);
    instances[0].mesh = renderer->builtin(BuiltinMesh::plane);
    instances[0].position = DVec3{400.0, 0.0, 0.0};
    instances[0].scale = {100.0f, 1.0f, 100.0f};
    instances[1].mesh = renderer->builtin(BuiltinMesh::plane);
    instances[1].position = DVec3{600.0, -29.7, 0.0};
    instances[1].scale = {100.0f, 1.0f, 100.0f};
    // A glowing post standing in the deep water ahead and to the left of the
    // flat view (reflected), and a dark bed 0.15 m under the surface (foam).
    instances[2].mesh = renderer->builtin(BuiltinMesh::cube);
    instances[2].position = DVec3{588.0, 3.0, -40.0};
    instances[2].scale = {4.0f, 6.0f, 1.0f};
    instances[2].material = 1;
    instances[3].mesh = renderer->builtin(BuiltinMesh::plane);
    instances[3].position = DVec3{200.0, 0.15, 0.0};
    instances[3].scale = {100.0f, 1.0f, 100.0f};
    instances[3].material = 2;
    std::vector<Material> materials(3);
    materials[0].base_color = {0.8f, 0.8f, 0.8f};
    materials[0].roughness = 0.9f;
    materials[1].base_color = {0.0f, 0.0f, 0.0f};
    materials[1].emissive = {4.0f, 4.0f, 4.0f};
    materials[2].base_color = {0.03f, 0.03f, 0.03f};
    materials[2].roughness = 0.9f;
    std::vector<WaterVertex> v;
    std::vector<u32> idx;
    square(v, idx, {500.0, 0.3, 0.0}, 400.0);
    std::vector<WaterLook> looks(1);
    looks[0].waves = 0.0f; // still: a clean mirror
    FrameInput frame;
    frame.settings.auto_exposure = false;
    frame.settings.bloom = false;
    frame.settings.shadows = false;
    frame.settings.gi = false;
    frame.settings.physical_sky = false;
    frame.settings.reflections = false;
    frame.settings.screen_space_reflections = false;
    frame.settings.antialiasing = false;
    frame.settings.ambient_occlusion = false;
    frame.settings.contact_shadows = false;
    frame.instances = instances;
    frame.materials = materials;
    frame.water_vertices = v;
    frame.water_indices = idx;
    frame.water_looks = looks;
    frame.lighting.background = {0.5f, 0.5f, 0.5f};
    frame.lighting.sun_direction = {0.0f, 1.0f, 0.0f};
    frame.lighting.sun_light = {0.0f, 0.0f, 0.0f};
    frame.lighting.sky_light = {0.6f, 0.6f, 0.6f};
    frame.lighting.ground_light = {0.02f, 0.02f, 0.02f};
    const auto draw = [&] {
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
        GIANT_CHECK(renderer->last_stats().water_triangles == 2);
        device->wait_idle();
        return device->read_texture(target);
    };
    const auto green = [](const std::vector<u8>& image, u32 x, u32 y) {
        const usize i = (static_cast<usize>(y) * k_w + x) * 4;
        return i + 3 < image.size() ? static_cast<int>(image[i + 1]) : -1;
    };
    // Straight down from 10 m over the shallow, then the deep.
    frame.view.orientation = quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -1.5707963f);
    frame.view.position = {400.0, 10.0, 0.0};
    const std::vector<u8> shallow = draw();
    frame.view.position = {600.0, 10.0, 0.0};
    const std::vector<u8> deep = draw();
    // Over the deep, looking out nearly flat (a few degrees down): the sky mirrored.
    frame.view.position = {600.0, 2.0, 0.0};
    frame.view.orientation = quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -0.12f);
    const std::vector<u8> flat = draw();
    // The same with screen reflections: the post shows in the water under it.
    frame.settings.reflections = true; // (screen reflections need both; GI off: no voxel cones)
    frame.settings.screen_space_reflections = true;
    const std::vector<u8> mirrored = draw();
    frame.settings.reflections = false;
    frame.settings.screen_space_reflections = false;
    // A low sun ahead, still water then waves: the waves move the view,
    // which stays lit, and catch the sun.
    frame.wind.velocity = {6.0f, 0.0f, 2.0f};
    frame.water_seconds = 12.5;
    frame.lighting.sun_direction = normalize_or_zero(Vec3{0.0f, 0.25f, -1.0f});
    frame.lighting.sun_light = {3.0f, 3.0f, 3.0f};
    const std::vector<u8> still = draw();
    looks[0].waves = 1.0f;
    const std::vector<u8> wavy = draw();
    int brightest = 0;
    usize differing = 0, dark = 0;
    for (u32 y = 34; y < k_h; ++y) {
        for (u32 x = 0; x < k_w; ++x) {
            differing += std::abs(green(wavy, x, y) - green(still, x, y)) > 2 ? usize{1} : usize{0};
            brightest = std::max(brightest, green(wavy, x, y));
            dark += green(wavy, x, y) <= 0 ? usize{1} : usize{0};
        }
    }
    std::printf("    shallow %d, deep %d, flat (near the horizon) %d, wavy differing %zu dark %zu brightest %d\n",
                green(shallow, 32, 32), green(deep, 32, 32), green(flat, 32, 34), differing, dark, brightest);
    GIANT_CHECK(green(shallow, 32, 32) > 3 * green(deep, 32, 32));
    GIANT_CHECK(green(shallow, 32, 32) > 60);
    // Near the horizon Fresnel mirrors much of the sky.
    GIANT_CHECK(green(flat, 32, 34) > 2 * green(deep, 32, 32));
    GIANT_CHECK(differing > 64 && dark == 0 && brightest > 200);
    // The post's reflection (rows 28 to 35 under it, column 16): only with
    // screen reflections, and not on the other side.
    int post_with = 0, post_without = 0, other_side = 0;
    for (u32 y = 29; y <= 34; ++y) {
        post_with = std::max(post_with, green(mirrored, 16, y));
        post_without = std::max(post_without, green(flat, 16, y));
        other_side = std::max(other_side, green(mirrored, 48, y));
    }
    std::printf("    post reflected %d, without screen reflections %d, other side %d\n", post_with, post_without,
                other_side);
    GIANT_CHECK(post_with > post_without + 40 && post_with > other_side + 40);
    // Straight down over the dark bed 0.15 m under: still water shows it
    // dark; with waves, foam (bright patches) breaks out.
    looks[0].waves = 0.0f;
    frame.view.orientation = quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -1.5707963f);
    frame.view.position = {200.0, 10.0, 0.0};
    frame.lighting.sun_direction = normalize_or_zero(Vec3{0.6f, 0.8f, 0.0f}); // its glint off the screen
    const std::vector<u8> bare = draw();
    looks[0].waves = 1.0f;
    const std::vector<u8> foamy = draw();
    usize bright = 0;
    for (u32 y = 0; y < k_h; ++y) {
        for (u32 x = 0; x < k_w; ++x) {
            bright += green(foamy, x, y) > 2 * green(bare, x, y) + 20 ? usize{1} : usize{0};
        }
    }
    std::printf("    bed %d, foam pixels %zu\n", green(bare, 32, 32), bright);
    GIANT_CHECK(bright > 64);
    GIANT_CHECK(!device->device_lost() && device->validation_errors() == 0);
    device->destroy(target);
}

GIANT_TEST("water render: d3d12 far water fades into the fog as the ground does") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::d3d12;
    desc.software_adapter = true;
    desc.debug_layer = true;
    auto device = rhi::create_device(desc);
    if (device == nullptr) return; // not on Windows
    auto renderer = Renderer::create(*device);
    GIANT_CHECK(renderer != nullptr);
    if (renderer == nullptr) return;
    constexpr u32 k_w = 64, k_h = 48;
    rhi::TextureDesc target_desc;
    target_desc.width = k_w;
    target_desc.height = k_h;
    target_desc.format = rhi::Format::rgba8_srgb;
    target_desc.usage = rhi::TextureUsage::render_target;
    const rhi::TextureHandle target = device->create_texture(target_desc);
    // A grey ground; the water (when given) 10 km across, 0.5 m over it.
    std::vector<Instance> instances(1);
    instances[0].mesh = renderer->builtin(BuiltinMesh::plane);
    instances[0].scale = {40000.0f, 1.0f, 40000.0f};
    instances[0].always_mesh = true;
    std::vector<Material> materials(1);
    materials[0].base_color = {0.5f, 0.5f, 0.5f};
    materials[0].roughness = 0.9f;
    std::vector<WaterVertex> v;
    std::vector<u32> idx;
    square(v, idx, {0.0, 0.5, 0.0}, 5000.0);
    FrameInput frame;
    frame.settings.auto_exposure = false;
    frame.settings.bloom = false;
    frame.settings.physical_sky = false;
    frame.settings.gi = false;
    frame.settings.voxel_world = false;
    frame.settings.shadows = false;
    frame.settings.antialiasing = false;
    frame.view.position = {0.0, 2.0, 0.0};
    frame.view.orientation = Quat{}; // level: the horizon on row 24
    frame.instances = instances;
    frame.materials = materials;
    frame.lighting.sun_direction = normalize_or_zero(Vec3{0.2f, 0.7f, -0.6f});
    frame.lighting.sun_light = {2.6f, 2.5f, 2.3f};
    frame.lighting.sky_light = {0.15f, 0.25f, 0.45f};
    frame.lighting.ground_light = {0.08f, 0.07f, 0.06f};
    frame.sky.on = true;
    frame.sky.sun_top = {3.0f, 3.0f, 3.0f};
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
    const auto pixel = [](const std::vector<u8>& image, u32 x, u32 y, u32 c) {
        const usize i = (static_cast<usize>(y) * k_w + x) * 4 + c;
        return i < image.size() ? static_cast<int>(image[i]) : -1000;
    };
    // Row 40 looks steeply down (the water mirrors little), 4 to 6 m out,
    // in a fog of 1 a metre all but hidden.
    constexpr u32 k_row = 40;
    const auto clear_ground = draw();
    const auto clear_water = [&] {
        frame.water_vertices = v;
        frame.water_indices = idx;
        return draw();
    }();
    GIANT_CHECK(renderer->last_stats().water_triangles == 2);
    frame.sky.fog_density = 1.0f;
    frame.sky.fog_falloff = 1.0f / 100.0f;
    const auto fogged_water = draw();
    frame.water_vertices = {};
    frame.water_indices = {};
    const auto fogged_ground = draw();
    int fog_change = 0, versus_ground = 0, water_shows = 0;
    for (u32 c = 0; c < 3; ++c) {
        water_shows = std::max(water_shows, std::abs(pixel(clear_water, 32, k_row, c) - pixel(clear_ground, 32, k_row, c)));
        fog_change = std::max(fog_change, std::abs(pixel(fogged_water, 32, k_row, c) - pixel(clear_water, 32, k_row, c)));
        versus_ground = std::max(versus_ground, std::abs(pixel(fogged_water, 32, k_row, c) - pixel(fogged_ground, 32, k_row, c)));
    }
    std::printf("    far water: unlike the clear ground by %d, fog changed it by %d, from the fogged ground by %d\n",
                water_shows, fog_change, versus_ground);
    GIANT_CHECK(water_shows > 12 && fog_change > 12 && versus_ground <= 8);
    GIANT_CHECK(!device->device_lost() && device->validation_errors() == 0);
    device->destroy(target);
}

GIANT_TEST("water render: under water where the near plane dips under the camera's water; looks packed in range") {
    // Level: the lower half of the near plane is under a surface at the camera.
    const Quat level{};
    GIANT_CHECK(near_plane_under(level, 0.577f, 1.0f, 0.1f, 0.0));
    GIANT_CHECK(!near_plane_under(level, 0.577f, 1.0f, 0.1f, -0.06)); // its lowest corner is 0.0577 m down
    GIANT_CHECK(near_plane_under(level, 0.577f, 1.0f, 0.1f, -0.05));
    // Looking straight down: all of it 0.1 m down; straight up: 0.1 m up.
    const Quat down = quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -1.5707963f);
    const Quat up = quat_from_axis_angle({1.0f, 0.0f, 0.0f}, 1.5707963f);
    GIANT_CHECK(near_plane_under(down, 0.577f, 1.0f, 0.1f, -0.09) && !near_plane_under(down, 0.577f, 1.0f, 0.1f, -0.11));
    GIANT_CHECK(near_plane_under(up, 0.577f, 1.0f, 0.1f, 0.11) && !near_plane_under(up, 0.577f, 1.0f, 0.1f, 0.09));
    GIANT_CHECK(!near_plane_under(level, 0.577f, 1.0f, 0.1f, std::numeric_limits<double>::quiet_NaN()));
    WaterLook l;
    l.absorption = {-1.0f, 50.0f, std::nanf("")};
    const gpu::WaterLook g = pack_water_look(l);
    GIANT_CHECK(g.absorption[0] == 0.0f && g.absorption[1] == 10.0f && g.absorption[2] == WaterLook{}.absorption.z);
}

GIANT_TEST("water render: the camera's water dims the view with no surfaces drawn, only where the near plane is under (null device)") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    auto device = rhi::create_device(desc);
    auto renderer = Renderer::create(*device);
    GIANT_CHECK(renderer != nullptr);
    if (renderer == nullptr) return;
    rhi::TextureDesc target_desc;
    target_desc.width = 32;
    target_desc.height = 16;
    target_desc.format = rhi::Format::rgba8_srgb;
    target_desc.usage = rhi::TextureUsage::render_target;
    const rhi::TextureHandle target = device->create_texture(target_desc);
    FrameInput frame;
    frame.view.position = {0.0, 5.0, 0.0};
    const auto draw = [&] {
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        GIANT_CHECK(cmd != nullptr);
        if (cmd != nullptr) {
            renderer->render(*cmd, target, frame);
            device->end_frame();
        }
        return renderer->last_stats();
    };
    const u32 without = draw().post_draws;
    frame.underwater.on = true;
    frame.underwater.level = 10.0;
    RenderStats s = draw();
    GIANT_CHECK(s.underwater && s.post_draws == without + 1 && s.water_triangles == 0);
    // The surface well under the near plane: nothing.
    frame.underwater.level = 4.0;
    s = draw();
    GIANT_CHECK(!s.underwater && s.post_draws == without);
    // Off, a bad level, water off, a view not lit: nothing.
    frame.underwater.level = std::numeric_limits<double>::infinity();
    GIANT_CHECK(!draw().underwater);
    frame.underwater.level = 10.0;
    frame.underwater.on = false;
    GIANT_CHECK(!draw().underwater);
    frame.underwater.on = true;
    frame.settings.water = false;
    GIANT_CHECK(!draw().underwater && draw().post_draws == without);
    frame.settings.water = true;
    frame.settings.debug_view = DebugView::clusters;
    GIANT_CHECK(!draw().underwater);
    frame.settings.debug_view = DebugView::lit;
    // With surfaces too: both passes.
    std::vector<WaterVertex> v;
    std::vector<u32> idx;
    square(v, idx, {0.0, 10.0, 0.0}, 50.0);
    frame.water_vertices = v;
    frame.water_indices = idx;
    s = draw();
    GIANT_CHECK(s.underwater && s.post_draws == without + 2 && s.water_triangles == 2);
    GIANT_CHECK(!device->device_lost());
    device->destroy(target);
}

GIANT_TEST("water render: d3d12 under water: far things hidden, near seen, split at the near plane, Snell's window, caustics") {
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
    // A white floor at 0.
    std::vector<Instance> instances(1);
    instances[0].mesh = renderer->builtin(BuiltinMesh::plane);
    instances[0].scale = {2000.0f, 1.0f, 2000.0f};
    instances[0].always_mesh = true;
    std::vector<Material> materials(1);
    materials[0].base_color = {0.8f, 0.8f, 0.8f};
    materials[0].roughness = 0.9f;
    FrameInput frame;
    frame.settings.auto_exposure = false;
    frame.settings.bloom = false;
    frame.settings.shadows = false;
    frame.settings.gi = false;
    frame.settings.voxel_world = false;
    frame.settings.physical_sky = false;
    frame.settings.reflections = false;
    frame.settings.screen_space_reflections = false;
    frame.settings.antialiasing = false;
    frame.settings.ambient_occlusion = false;
    frame.settings.contact_shadows = false;
    frame.instances = instances;
    frame.materials = materials;
    frame.lighting.background = {0.5f, 0.5f, 0.5f};
    frame.lighting.sun_direction = {0.0f, 1.0f, 0.0f};
    frame.lighting.sun_light = {2.0f, 2.0f, 2.0f};
    frame.lighting.sky_light = {0.4f, 0.4f, 0.4f};
    frame.lighting.ground_light = {0.02f, 0.02f, 0.02f};
    frame.wind.velocity = {6.0f, 0.0f, 2.0f};
    frame.water_seconds = 7.0;
    frame.underwater.level = 10.0;
    frame.underwater.look.waves = 0.0f;
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
    const auto pixel = [](const std::vector<u8>& image, u32 x, u32 y, u32 c) {
        const usize i = (static_cast<usize>(y) * k_w + x) * 4 + c;
        return i < image.size() ? static_cast<int>(image[i]) : -1000;
    };
    // Level, 5 m under: row 44 sees the floor about 22 m out, row 63 about 8 m.
    frame.view.position = {0.0, 5.0, 0.0};
    frame.view.orientation = Quat{};
    const auto dry = draw();
    frame.underwater.on = true;
    const auto wet = draw();
    GIANT_CHECK(renderer->last_stats().underwater);
    // Straight down from a metre over the floor: seen through a metre of water.
    frame.view.position = {0.0, 1.0, 0.0};
    frame.view.orientation = quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -1.5707963f);
    const auto near_wet = draw();
    frame.underwater.on = false;
    const auto near_dry = draw();
    std::printf("    far floor red %d -> %d, green %d -> %d; near floor green %d -> %d\n", pixel(dry, 32, 44, 0),
                pixel(wet, 32, 44, 0), pixel(dry, 32, 44, 1), pixel(wet, 32, 44, 1), pixel(near_dry, 32, 32, 1),
                pixel(near_wet, 32, 32, 1));
    // Far: red (absorbed first) all but gone, and dimmer than near.
    GIANT_CHECK(pixel(wet, 32, 44, 0) < pixel(dry, 32, 44, 0) / 3);
    GIANT_CHECK(pixel(wet, 32, 63, 1) > pixel(wet, 32, 44, 1));
    // Near: still seen.
    GIANT_CHECK(pixel(near_wet, 32, 32, 1) > pixel(near_dry, 32, 32, 1) / 2 && pixel(near_wet, 32, 32, 1) > 40);
    // The camera at the surface, level: the sky above as it is, the floor under dimmed.
    frame.underwater.on = true;
    frame.view.position = {0.0, 10.0, 0.0};
    frame.view.orientation = Quat{};
    const auto split = draw();
    frame.underwater.on = false;
    const auto split_dry = draw();
    int top_change = 0;
    for (u32 c = 0; c < 3; ++c) top_change = std::max(top_change, std::abs(pixel(split, 32, 8, c) - pixel(split_dry, 32, 8, c)));
    std::printf("    at the surface: top changed by %d, bottom red %d -> %d\n", top_change, pixel(split_dry, 32, 60, 0),
                pixel(split, 32, 60, 0));
    GIANT_CHECK(top_change <= 2 && pixel(split, 32, 60, 0) < pixel(split_dry, 32, 60, 0) / 2);
    // Looking up at the surface from 5 m under, a wide view: the sky through
    // the window overhead, the water mirrored past it (outside 48.75 degrees).
    std::vector<WaterVertex> v;
    std::vector<u32> idx;
    square(v, idx, {0.0, 10.0, 0.0}, 400.0);
    std::vector<WaterLook> looks(1);
    looks[0].waves = 0.0f;
    frame.water_vertices = v;
    frame.water_indices = idx;
    frame.water_looks = looks;
    frame.underwater.on = true;
    frame.view.position = {0.0, 5.0, 0.0};
    frame.view.orientation = quat_from_axis_angle({1.0f, 0.0f, 0.0f}, 1.5707963f);
    frame.view.vertical_fov_degrees = 120.0f;
    const auto up = draw();
    GIANT_CHECK(renderer->last_stats().water_triangles == 2);
    std::printf("    from below: overhead %d, past the window %d\n", pixel(up, 32, 32, 1), pixel(up, 32, 1, 1));
    GIANT_CHECK(pixel(up, 32, 32, 1) > pixel(up, 32, 1, 1) + 40);
    // Straight down at the floor 3 m under, 6 m of water over it: still
    // water lights it evenly, waves gather the sun into caustics.
    frame.water_vertices = {};
    frame.water_indices = {};
    frame.view.vertical_fov_degrees = 60.0f;
    frame.view.position = {0.0, 3.0, 0.0};
    frame.view.orientation = quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -1.5707963f);
    frame.underwater.level = 6.0;
    const auto even = draw();
    frame.underwater.look.waves = 2.0f;
    const auto gathered = draw();
    int even_low = 255, even_high = 0, low = 255, high = 0;
    for (u32 y = 8; y < k_h - 8; ++y) {
        for (u32 x = 8; x < k_w - 8; ++x) {
            even_low = std::min(even_low, pixel(even, x, y, 1));
            even_high = std::max(even_high, pixel(even, x, y, 1));
            low = std::min(low, pixel(gathered, x, y, 1));
            high = std::max(high, pixel(gathered, x, y, 1));
        }
    }
    std::printf("    caustics: still %d to %d, waves %d to %d\n", even_low, even_high, low, high);
    GIANT_CHECK(high - low > even_high - even_low + 30);
    GIANT_CHECK(!device->device_lost() && device->validation_errors() == 0);
    device->destroy(target);
}
