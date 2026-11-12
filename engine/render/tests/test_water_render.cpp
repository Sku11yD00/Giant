// Water's surfaces (water plan W2a): packing what the caller gives (bad
// triangles dropped, looks kept in range, the caps), the waves (the same
// at a place whatever the camera, moving with time), the pass on the null
// device, and on the software adapter: shallow water shows its bed, deep
// water its own colour, and seen flat it mirrors the sky.
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
    std::vector<Instance> instances(2);
    instances[0].mesh = renderer->builtin(BuiltinMesh::plane);
    instances[0].position = DVec3{400.0, 0.0, 0.0};
    instances[0].scale = {100.0f, 1.0f, 100.0f};
    instances[1].mesh = renderer->builtin(BuiltinMesh::plane);
    instances[1].position = DVec3{600.0, -29.7, 0.0};
    instances[1].scale = {100.0f, 1.0f, 100.0f};
    std::vector<Material> materials(1);
    materials[0].base_color = {0.8f, 0.8f, 0.8f};
    materials[0].roughness = 0.9f;
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
    GIANT_CHECK(!device->device_lost() && device->validation_errors() == 0);
    device->destroy(target);
}
