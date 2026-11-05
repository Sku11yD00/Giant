#include "giant/render/meshes.h"
#include "giant/render/renderer.h"

#include "giant_test.h"

#include <array>
#include <cmath>
#include <vector>

using namespace giant;
using namespace giant::render;

namespace {

// Every triangle must wind counter-clockwise seen from outside, which is
// what back-face culling keeps; `outward` gives the outside direction.
template <typename Outward>
void check_mesh(const MeshData& mesh, Outward outward) {
    GIANT_CHECK(!mesh.indices.empty() && mesh.indices.size() % 3 == 0);
    bool in_range = true;
    bool winding = true;
    for (usize i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const u32 a = mesh.indices[i], b = mesh.indices[i + 1], c = mesh.indices[i + 2];
        if (a >= mesh.vertices.size() || b >= mesh.vertices.size() || c >= mesh.vertices.size()) {
            in_range = false;
            break;
        }
        const Vec3 pa = mesh.vertices[a].position;
        const Vec3 face = cross(mesh.vertices[b].position - pa, mesh.vertices[c].position - pa);
        const Vec3 centre = (pa + mesh.vertices[b].position + mesh.vertices[c].position) / 3.0f;
        if (dot(face, outward(centre)) <= 0.0f) {
            winding = false;
        }
    }
    GIANT_CHECK(in_range);
    GIANT_CHECK(winding);
    bool unit = true;
    for (const MeshVertex& v : mesh.vertices) {
        unit = unit && std::fabs(length(v.normal) - 1.0f) < 1e-4f;
    }
    GIANT_CHECK(unit);
}

Vec3 project(const Mat4& m, const Vec3& p) {
    const Vec4 c = m * Vec4{p.x, p.y, p.z, 1.0f};
    return {c.x / c.w, c.y / c.w, c.z / c.w};
}

} // namespace

GIANT_TEST("render: built-in meshes face outwards with unit normals") {
    check_mesh(make_cube(), [](const Vec3& c) { return c; });
    check_mesh(make_sphere(), [](const Vec3& c) { return c; });
    check_mesh(make_sphere(3, 2), [](const Vec3& c) { return c; });
    check_mesh(make_plane(), [](const Vec3&) { return Vec3{0, 1, 0}; });
    for (const MeshVertex& v : make_sphere().vertices) {
        GIANT_CHECK_NEAR(length(v.position), 0.5, 1e-5);
    }
    GIANT_CHECK(make_cube().indices.size() == 36);
}

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

GIANT_TEST("render: one instanced draw per mesh on the null device") {
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
    FrameInput frame;
    frame.instances = instances;
    frame.lighting.background = {0.5f, 0.0f, 0.0f};

    for (int i = 0; i < 2; ++i) { // the second frame reuses the depth target
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        renderer->render(*cmd, target, frame);
        device->end_frame();
    }
    GIANT_CHECK(renderer->last_stats().draws == 2);
    GIANT_CHECK(renderer->last_stats().instances == 3);
    GIANT_CHECK(device->last_frame_stats().draws == 2);
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
    instances[0].base_color = {0.9f, 0.9f, 0.9f};
    instances[0].roughness = 0.6f;
    // A green emissive cube behind it, partly hidden: depth testing.
    instances[1].mesh = renderer->builtin(BuiltinMesh::cube);
    instances[1].position = camera + DVec3{0.0, 0.0, -8.0};
    instances[1].scale = {20.0f, 0.5f, 1.0f};
    instances[1].base_color = {0.0f, 0.0f, 0.0f};
    instances[1].emissive = {0.0f, 0.6f, 0.0f};

    FrameInput frame;
    frame.view.position = camera;
    frame.instances = instances;
    frame.lighting.sun_direction = {1.0f, 0.0f, 0.0f};
    frame.lighting.sun_light = {3.0f, 3.0f, 3.0f};
    frame.lighting.sky_light = {0.02f, 0.02f, 0.02f};
    frame.lighting.ground_light = {0.02f, 0.02f, 0.02f};
    frame.lighting.background = {0.0f, 0.0f, 1.0f};

    rhi::CommandList* cmd = device->begin_frame(nullptr);
    GIANT_CHECK(cmd != nullptr);
    if (cmd != nullptr) {
        renderer->render(*cmd, target, frame);
        device->end_frame();
    }
    const auto pixels = device->read_texture(target);
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
    GIANT_CHECK(renderer->last_stats().draws == 2);
    device->destroy(target);
    renderer.reset();
    device->wait_idle();
    GIANT_CHECK(!device->device_lost());
    GIANT_CHECK(device->validation_errors() == 0);
}
#endif
