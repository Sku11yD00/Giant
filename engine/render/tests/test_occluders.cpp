// Characters in the light (characters plan C2a): which occluders a frame
// uses (src/occluders.h), the pass through a frame, and on the software
// adapter what it does to the floor round a capsule.
#include "occluders.h"

#include "giant/render/renderer.h"
#include "giant/rhi/rhi.h"
#include "giant_test.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace giant;
using namespace giant::render;

namespace {

geometry::Frustum looking_down_minus_z() {
    View view; // at the origin, looking down -Z
    return geometry::frustum_from_view_projection(Renderer::view_projection(view, 16.0f / 9.0f));
}

Occluder capsule(DVec3 a, DVec3 b, float radius) {
    Occluder o;
    o.a = a;
    o.b = b;
    o.radius = radius;
    return o;
}

} // namespace

GIANT_TEST("occluders: those in view and near enough are kept, nearest first, camera-relative") {
    const geometry::Frustum frustum = looking_down_minus_z();
    std::vector<Occluder> in;
    in.push_back(capsule({0.0, 0.0, -20.0}, {0.0, 1.0, -20.0}, 0.2f)); // 0: ahead, 20 m
    in.push_back(capsule({0.0, 0.0, -5.0}, {0.0, 1.0, -5.0}, 0.2f));   // 1: ahead, 5 m
    in.push_back(capsule({0.0, 0.0, 20.0}, {0.0, 1.0, 20.0}, 0.2f));   // 2: behind: out
    in.push_back(capsule({0.0, 0.0, -400.0}, {0.0, 1.0, -400.0}, 0.2f)); // 3: too far
    in.push_back(capsule({0.0, 0.0, -6.0}, {0.0, NAN, -6.0}, 0.2f));    // 4: not finite
    in.push_back(capsule({0.0, 0.0, -6.0}, {0.0, 1.0, -6.0}, 0.0f));    // 5: no size
    in.push_back(capsule({0.0, 0.0, -6.0}, {0.0, 1.0, -6.0}, -1.0f));   // 6: no size
    in.push_back(capsule({0.0, 0.0, 0.5}, {0.0, 1.0, 0.5}, 0.2f));      // 7: just behind, its reach round the camera
    in[1].albedo = {1.0f, 0.5f, 0.0f};
    std::vector<gpu::GpuOccluder> out;
    std::vector<std::pair<float, u32>> scratch;
    // Far from the origin: positions are camera-relative.
    const DVec3 camera{1.0e6, 0.0, 0.0};
    std::vector<Occluder> moved = in;
    for (Occluder& o : moved) {
        o.a = o.a + camera;
        o.b = o.b + camera;
    }
    pick_occluders(moved, camera, Mat4{}, frustum, out, scratch);
    GIANT_CHECK(out.size() == 3);
    if (out.size() == 3) {
        // Nearest first: the one round the camera, 5 m, 20 m.
        GIANT_CHECK(out[0].a[2] == 0.5f);
        GIANT_CHECK(out[1].a[2] == -5.0f && out[1].a[0] == 0.0f && out[1].b[1] == 1.0f && out[1].radius == 0.2f);
        GIANT_CHECK(out[1].albedo == (255u | (128u << 8) | (0u << 16)));
        GIANT_CHECK(out[2].a[2] == -20.0f);
    }
    // Big radii are kept to 10 m; past k_max_occluders only the nearest.
    std::vector<Occluder> many;
    for (u32 i = 0; i < k_max_occluders + 100; ++i) {
        many.push_back(capsule({0.0, 0.0, -2.0 - 0.1 * i}, {0.0, 1.0, -2.0 - 0.1 * i}, i == 0 ? 50.0f : 0.1f));
    }
    pick_occluders(many, DVec3{}, Mat4{}, frustum, out, scratch);
    GIANT_CHECK(out.size() == k_max_occluders);
    float farthest = 0.0f;
    for (const gpu::GpuOccluder& g : out) farthest = std::max(farthest, -g.a[2]);
    GIANT_CHECK(farthest < 2.0f + 0.1f * k_max_occluders);
    GIANT_CHECK(out[0].radius == 10.0f);
    // None: nothing.
    pick_occluders({}, DVec3{}, Mat4{}, frustum, out, scratch);
    GIANT_CHECK(out.empty());
    // Turned to look down +X: one 5 m along +X is in view, 5 m ahead in view space.
    View turned;
    turned.orientation = quat_from_axis_angle({0.0f, 1.0f, 0.0f}, -1.5707963f);
    const Mat4 turned_view = view_from_orientation(turned.orientation);
    const geometry::Frustum turned_frustum =
        geometry::frustum_from_view_projection(Renderer::view_projection(turned, 16.0f / 9.0f));
    const std::vector<Occluder> ahead{capsule({5.0, 0.0, 0.0}, {5.0, 1.0, 0.0}, 0.2f),
                                      capsule({0.0, 0.0, -5.0}, {0.0, 1.0, -5.0}, 0.2f)}; // (now to the side: out)
    pick_occluders(ahead, DVec3{}, turned_view, turned_frustum, out, scratch);
    GIANT_CHECK(out.size() == 1);
    if (out.size() == 1) {
        GIANT_CHECK(std::fabs(out[0].a[2] + 5.0f) < 1e-4f && std::fabs(out[0].a[0]) < 1e-4f);
        GIANT_CHECK(std::fabs(out[0].b[1] - 1.0f) < 1e-4f);
    }
}

GIANT_TEST("occluders: a pass of their own before shading, lit views with occluders only") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    auto device = rhi::create_device(desc);
    auto renderer = Renderer::create(*device);
    GIANT_CHECK(renderer != nullptr);
    if (renderer == nullptr) return;
    rhi::TextureDesc t;
    t.width = 320;
    t.height = 180;
    t.format = rhi::Format::rgba8_srgb;
    t.usage = rhi::TextureUsage::render_target;
    const rhi::TextureHandle target = device->create_texture(t);
    std::vector<Instance> instances(1);
    instances[0].mesh = renderer->builtin(BuiltinMesh::plane);
    instances[0].position = {0.0, -1.0, -5.0};
    instances[0].scale = {20.0f, 1.0f, 20.0f};
    std::vector<Occluder> occluders;
    occluders.push_back(capsule({0.0, -1.0, -5.0}, {0.0, 0.5, -5.0}, 0.2f));
    occluders.push_back(capsule({0.0, -1.0, 50.0}, {0.0, 0.5, 50.0}, 0.2f)); // behind
    FrameInput frame;
    frame.instances = instances;
    const auto draw = [&] {
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        renderer->render(*cmd, target, frame);
        device->end_frame();
        return renderer->last_stats();
    };
    const auto timed = [&](const std::string& name) {
        for (const rhi::GpuTiming& g : device->gpu_timings()) {
            if (g.name == name) return true;
        }
        return false;
    };
    const auto before = [&](const std::string& a, const std::string& b) {
        int ia = -1, ib = -1, i = 0;
        for (const rhi::GpuTiming& g : device->gpu_timings()) {
            if (g.name == a) ia = i;
            if (g.name == b) ib = i;
            ++i;
        }
        return ia >= 0 && ib >= 0 && ia < ib;
    };
    // None given: no pass.
    GIANT_CHECK(draw().occluders == 0 && !timed("character occlusion"));
    frame.occluders = occluders;
    GIANT_CHECK(draw().occluders == 1); // the one in view
    GIANT_CHECK(timed("character occlusion") && before("contact shadows", "character occlusion") &&
                before("character occlusion", "shading"));
    // Off, a debug view, or none in view: no pass.
    frame.settings.character_occlusion = false;
    GIANT_CHECK(draw().occluders == 0 && !timed("character occlusion"));
    frame.settings.character_occlusion = true;
    frame.settings.debug_view = DebugView::clusters;
    GIANT_CHECK(draw().occluders == 0 && !timed("character occlusion"));
    frame.settings.debug_view = DebugView::lighting; // a lit view
    GIANT_CHECK(draw().occluders == 1);
    frame.settings.debug_view = DebugView::lit;
    frame.occluders = std::span<const Occluder>(occluders).subspan(1);
    GIANT_CHECK(draw().occluders == 0 && !timed("character occlusion"));
    // A new size: made again.
    frame.occluders = occluders;
    t.width = 160;
    t.height = 90;
    const rhi::TextureHandle small = device->create_texture(t);
    rhi::CommandList* cmd = device->begin_frame(nullptr);
    renderer->render(*cmd, small, frame);
    device->end_frame();
    GIANT_CHECK(renderer->last_stats().occluders == 1 && timed("character occlusion"));
    GIANT_CHECK(draw().occluders == 1);
    GIANT_CHECK(!device->device_lost());
    device->destroy(small);
    device->destroy(target);
}

GIANT_TEST("render: d3d12 a character's capsules darken the floor round it and tint it, not the open floor") {
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
    // The camera 1.5 m up over a floor; a body (a capsule 20 cm round,
    // 0.2 to 1.6 m up) stands 4 m ahead. It is not drawn: only the light
    // round it shows it. Row 108 is the floor some 26 cm in front of its
    // axis, column 10 of it the open floor 1.8 m to the side; row 90 the
    // floor some 6.3 m out, 2.3 m beyond it.
    const DVec3 camera{500.0, 1.5, 0.0};
    std::vector<Instance> instances(1);
    instances[0].mesh = renderer->builtin(BuiltinMesh::plane);
    instances[0].position = DVec3{500.0, 0.0, 0.0};
    instances[0].scale = {20.0f, 1.0f, 20.0f};
    std::vector<Material> materials(1);
    materials[0].base_color = {0.8f, 0.8f, 0.8f};
    materials[0].roughness = 0.9f;
    std::vector<Occluder> occluders(1);
    occluders[0].a = DVec3{500.0, 0.2, -4.0};
    occluders[0].b = DVec3{500.0, 1.6, -4.0};
    occluders[0].radius = 0.2f;
    occluders[0].albedo = {1.0f, 0.0f, 0.0f};
    FrameInput frame;
    frame.settings.auto_exposure = false;
    frame.settings.bloom = false;
    frame.settings.shadows = false;
    frame.settings.gi = false;
    frame.settings.physical_sky = false;
    frame.settings.reflections = false;
    frame.settings.screen_space_reflections = false;
    frame.settings.ambient_occlusion = false;
    frame.settings.contact_shadows = false;
    frame.settings.antialiasing = false;
    frame.view.position = camera;
    frame.instances = instances;
    frame.materials = materials;
    frame.occluders = occluders;
    frame.lighting.background = {0.0f, 0.0f, 0.0f};
    frame.lighting.sun_direction = {0.0f, 1.0f, 0.0f};
    frame.lighting.sun_light = {0.0f, 0.0f, 0.0f};
    frame.lighting.sky_light = {0.5f, 0.5f, 0.5f};
    frame.lighting.ground_light = {0.5f, 0.5f, 0.5f};
    const auto draw = [&](bool on) {
        frame.settings.character_occlusion = on;
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
        GIANT_CHECK(renderer->last_stats().occluders == (on ? 1u : 0u));
        device->wait_idle();
        return device->read_texture(target);
    };
    const auto channel = [](const std::vector<u8>& image, u32 x, u32 y, u32 c) {
        const usize i = (static_cast<usize>(y) * k_w + x) * 4 + c;
        return i < image.size() ? static_cast<int>(image[i]) : -1;
    };
    const std::vector<u8> off = draw(false);
    const std::vector<u8> on = draw(true);
    std::printf("    floor by it r %d -> %d, g %d -> %d; open floor g %d -> %d; far floor g %d -> %d\n",
                channel(off, 64, 108, 0), channel(on, 64, 108, 0), channel(off, 64, 108, 1),
                channel(on, 64, 108, 1), channel(off, 10, 108, 1), channel(on, 10, 108, 1), channel(off, 64, 90, 1),
                channel(on, 64, 90, 1));
    GIANT_CHECK(channel(off, 64, 108, 1) > 60 && channel(off, 10, 108, 1) > 60 && channel(off, 64, 90, 1) > 60);
    // The open floor, and the floor well beyond it, stay as they were.
    GIANT_CHECK(std::abs(channel(on, 10, 108, 1) - channel(off, 10, 108, 1)) <= 2);
    GIANT_CHECK(std::abs(channel(on, 64, 90, 1) - channel(off, 64, 90, 1)) <= 2);
    // By it the floor darkens (bytes are sRGB: a 5% drop is some 12% of
    // the light) ...
    GIANT_CHECK(channel(on, 64, 108, 1) * 100 < channel(off, 64, 108, 1) * 95);
    // ... and takes its red: red loses less than green.
    GIANT_CHECK(channel(on, 64, 108, 0) * channel(off, 64, 108, 1) >
                channel(on, 64, 108, 1) * channel(off, 64, 108, 0) + 2 * channel(off, 64, 108, 0));
    GIANT_CHECK(!device->device_lost() && device->validation_errors() == 0);
    device->destroy(target);
}
