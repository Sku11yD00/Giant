// Decal binning (src/decals.h) and decals and materials through a frame.
#include "decals.h"

#include "giant/render/renderer.h"
#include "giant/rhi/rhi.h"
#include "giant_test.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace giant;
using namespace giant::render;

namespace {

struct Camera {
    Mat4 view_proj;
    geometry::Frustum frustum;
    explicit Camera(u32 width, u32 height) {
        View view; // at the origin, looking down -Z
        view_proj = Renderer::view_projection(view, static_cast<float>(width) / static_cast<float>(height));
        frustum = geometry::frustum_from_view_projection(view_proj);
    }
};

// The decals listed in tile (x, y).
std::vector<u32> in_tile(const DecalBins& b, u32 x, u32 y) {
    const usize at = (usize{y} * b.columns + x) * 2;
    return {b.entries.begin() + b.tiles[at], b.entries.begin() + b.tiles[at] + b.tiles[at + 1]};
}

} // namespace

GIANT_TEST("decals: a decal is listed in the tiles it covers, in order") {
    const Camera cam(320, 160);
    std::vector<Decal> decals(3);
    decals[0].position = {0.0, 0.0, -10.0}; // the centre
    decals[1].position = {0.0, 0.0, -10.0}; // the same place, drawn over it
    decals[2].position = {0.0, 0.0, 10.0};  // behind the camera: left out
    DecalBins bins;
    bin_decals(decals, {}, cam.view_proj, cam.frustum, 320, 160, bins);
    GIANT_CHECK(bins.columns == 20 && bins.rows == 10);
    GIANT_CHECK(bins.decals.size() == 2 && !bins.overflow);
    const std::vector<u32> centre = in_tile(bins, 10, 5);
    GIANT_CHECK(centre.size() == 2 && centre[0] == 0 && centre[1] == 1);
    GIANT_CHECK(in_tile(bins, 0, 0).empty() && in_tile(bins, 19, 9).empty());
    // Every listed pair is real: count matches the entries.
    u32 total = 0;
    for (usize i = 1; i < bins.tiles.size(); i += 2) total += bins.tiles[i];
    GIANT_CHECK(total == bins.entries.size());
}

GIANT_TEST("decals: a box around the camera covers the whole screen") {
    const Camera cam(64, 64);
    std::vector<Decal> decals(1);
    decals[0].size = {4.0f, 4.0f, 4.0f}; // the camera is inside it
    DecalBins bins;
    bin_decals(decals, {}, cam.view_proj, cam.frustum, 64, 64, bins);
    GIANT_CHECK(bins.decals.size() == 1);
    for (u32 y = 0; y < bins.rows; ++y) {
        for (u32 x = 0; x < bins.columns; ++x) {
            GIANT_CHECK(in_tile(bins, x, y).size() == 1);
        }
    }
}

GIANT_TEST("decals: the GPU record maps the box to the unit cube") {
    Decal d;
    d.position = {100.0, 5.0, -3.0};
    d.rotation = normalize(Quat{0.0f, 0.3826834f, 0.0f, 0.9238795f}); // 45 degrees about Y
    d.size = {2.0f, 0.5f, 4.0f};
    const DVec3 camera{90.0, 0.0, 0.0};
    const gpu::Decal g = to_gpu(d, camera);
    const auto local = [&g](const Vec3& p) {
        float out[3];
        for (int i = 0; i < 3; ++i) {
            out[i] = g.rows[i * 4] * p.x + g.rows[i * 4 + 1] * p.y + g.rows[i * 4 + 2] * p.z + g.rows[i * 4 + 3];
        }
        return Vec3{out[0], out[1], out[2]};
    };
    const Vec3 centre = to_camera_relative(d.position, camera);
    const Vec3 c = local(centre);
    GIANT_CHECK(std::fabs(c.x) < 1e-5f && std::fabs(c.y) < 1e-5f && std::fabs(c.z) < 1e-5f);
    // Half a size along the decal's own x axis is the box face.
    const Vec3 face = centre + rotate(d.rotation, {1.0f, 0.0f, 0.0f});
    GIANT_CHECK(std::fabs(local(face).x - 0.5f) < 1e-5f);
    const Vec3 top = centre + rotate(d.rotation, {0.0f, 0.25f, 0.0f});
    GIANT_CHECK(std::fabs(local(top).y - 0.5f) < 1e-5f);
}

GIANT_TEST("decals: limits drop decals, not the frame") {
    const Camera cam(4096, 2048);
    std::vector<Decal> decals(k_max_decal_entries / (256 * 128) + 4);
    for (Decal& d : decals) d.size = {50.0f, 50.0f, 50.0f}; // each covers the screen
    DecalBins bins;
    bin_decals(decals, {}, cam.view_proj, cam.frustum, 4096, 2048, bins);
    GIANT_CHECK(bins.overflow);
    GIANT_CHECK(bins.entries.size() <= k_max_decal_entries);
    GIANT_CHECK(!bins.decals.empty());
}

GIANT_TEST("decals: materials and decals reach the frame; draw calls stay the same") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    auto device = rhi::create_device(desc);
    auto renderer = Renderer::create(*device);
    GIANT_CHECK(renderer != nullptr);
    rhi::TextureDesc t;
    t.width = 256;
    t.height = 128;
    t.format = rhi::Format::rgba8_srgb;
    t.usage = rhi::TextureUsage::render_target;
    const rhi::TextureHandle target = device->create_texture(t);
    const auto draw = [&](const FrameInput& frame) {
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        renderer->render(*cmd, target, frame);
        device->end_frame();
        return renderer->last_stats();
    };
    // A kit-like crowd: 2,000 instances of 2 meshes sharing 3 materials.
    std::vector<Instance> instances(2000);
    for (usize i = 0; i < instances.size(); ++i) {
        instances[i].mesh = renderer->builtin(i % 2 ? BuiltinMesh::cube : BuiltinMesh::sphere);
        instances[i].position = {static_cast<double>(i % 50) - 25.0, 0.0, -20.0 - static_cast<double>(i / 50)};
        instances[i].material = static_cast<u32>(i % 4); // 3 is out of range: the default
    }
    std::vector<Material> materials(3);
    std::vector<Decal> decals(2);
    decals[0].position = {0.0, 0.0, -25.0};
    decals[0].size = {10.0f, 4.0f, 10.0f};
    decals[1].position = {0.0, 0.0, 40.0}; // behind: not in view
    FrameInput frame;
    frame.instances = instances;
    frame.materials = materials;
    frame.decals = decals;
    frame.settings.gpu_culling = false;
    RenderStats s = draw(frame);
    GIANT_CHECK(s.materials == 3 && s.decals == 1 && !s.decal_overflow);
    GIANT_CHECK(s.draws == 2);
    frame.instances = std::span<const Instance>(instances).first(10);
    GIANT_CHECK(draw(frame).draws == 2); // fewer instances, same passes
    frame.settings.debug_view = DebugView::clusters;
    GIANT_CHECK(draw(frame).decals == 0); // debug views show geometry only
    device->destroy(target);
    renderer.reset();
    device->wait_idle();
    GIANT_CHECK(device->validation_errors() == 0);
}
