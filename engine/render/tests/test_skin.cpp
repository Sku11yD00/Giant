// Skinned meshes (renderer.h: add_skinned_mesh, set_pose): refused when
// unsound, posed on the GPU only when the pose changes, culled by the
// pose, kept out of the voxel world, and drawn where the pose puts them.
#include "giant/geometry/clusters.h"
#include "giant/geometry/mesh.h"
#include "giant/render/renderer.h"

#include "giant_test.h"

#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace giant;
using namespace giant::render;

namespace {

// Two balls 1 m wide, each cut into many clusters: one at the origin moved
// by joint 0, one at (0, 3, 0) moved by joint 1 (the "hand").
geometry::SkinnedClusterMesh two_balls() {
    geometry::SkinnedTriangleMesh m;
    for (u16 joint = 0; joint < 2; ++joint) {
        const geometry::TriangleMesh ball = geometry::make_sphere(64, 32);
        const u32 base = static_cast<u32>(m.mesh.vertices.size());
        for (geometry::Vertex v : ball.vertices) {
            v.position.y += 3.0f * joint;
            m.mesh.vertices.push_back(v);
            geometry::SkinWeights w;
            w.joints = {joint, 0, 0, 0};
            w.weights = {255, 0, 0, 0};
            m.weights.push_back(w);
        }
        for (const u32 i : ball.indices) {
            m.mesh.indices.push_back(base + i);
        }
    }
    return geometry::build_skinned_clusters(m);
}

// Joint 1 turned -90 degrees about z round the origin: the hand goes from
// (0, 3, 0) to (3, 0, 0).
std::array<Mat4, 2> bent() {
    return {Mat4{}, mat4_from_transform({}, quat_from_axis_angle({0.0f, 0.0f, 1.0f}, -k_pi * 0.5f), {1.0f, 1.0f, 1.0f})};
}
constexpr geometry::Sphere k_bent_bounds{{1.5f, 0.0f, 0.0f}, 2.2f};
std::array<Mat4, 2> two_balls_bind() { return {}; }

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
        t.width = 320;
        t.height = 180;
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
};

} // namespace

GIANT_TEST("skin: unsound skinned meshes and poses are refused") {
    NullScene s;
    GIANT_CHECK(s.renderer != nullptr);
    if (s.renderer == nullptr) return;
    Renderer& r = *s.renderer;
    GIANT_CHECK(r.add_skinned_mesh({}, 2) == k_no_mesh);
    GIANT_CHECK(r.add_skinned_mesh(two_balls(), 0) == k_no_mesh);
    GIANT_CHECK(r.add_skinned_mesh(two_balls(), k_max_skin_joints + 1) == k_no_mesh);
    GIANT_CHECK(r.add_skinned_mesh(two_balls(), 1) == k_no_mesh); // joint 1 is used
    geometry::SkinnedClusterMesh short_weights = two_balls();
    short_weights.weights.pop_back();
    GIANT_CHECK(r.add_skinned_mesh(std::move(short_weights), 2) == k_no_mesh);
    geometry::SkinnedClusterMesh light = two_balls();
    light.weights[5].weights = {0, 0, 0, 0}; // would fall to the origin
    GIANT_CHECK(r.add_skinned_mesh(std::move(light), 2) == k_no_mesh);
    geometry::SkinnedClusterMesh heavy = two_balls();
    heavy.weights[5].weights = {200, 100, 0, 0};
    GIANT_CHECK(r.add_skinned_mesh(std::move(heavy), 2) == k_no_mesh);
    // A mesh with levels of detail (not built for skinning).
    geometry::SkinnedClusterMesh levels;
    levels.mesh = geometry::build_clusters(geometry::make_sphere(64, 32));
    levels.weights.assign(levels.mesh.vertices.size(), geometry::SkinWeights{{0, 0, 0, 0}, {255, 0, 0, 0}});
    GIANT_CHECK(levels.mesh.levels > 1);
    GIANT_CHECK(r.add_skinned_mesh(std::move(levels), 1) == k_no_mesh);
    // An unused joint slot may name anything (its weight is 0).
    geometry::SkinnedClusterMesh spare = two_balls();
    spare.weights[0].joints[3] = 999;
    const MeshId id = r.add_skinned_mesh(std::move(spare), 2);
    GIANT_CHECK(id != k_no_mesh && r.skinned(id) && !r.skinned(r.builtin(BuiltinMesh::cube)));

    const std::array<Mat4, 2> pose = bent();
    GIANT_CHECK(r.set_pose(id, pose, k_bent_bounds));
    GIANT_CHECK(!r.set_pose(id, std::span<const Mat4>(pose).first(1), k_bent_bounds)); // a joint short
    GIANT_CHECK(!r.set_pose(r.builtin(BuiltinMesh::cube), {}, k_bent_bounds));          // not skinned
    GIANT_CHECK(!r.set_pose(k_no_mesh, pose, k_bent_bounds));
    std::array<Mat4, 2> broken = pose;
    broken[1].at(0, 3) = std::numeric_limits<float>::quiet_NaN();
    GIANT_CHECK(!r.set_pose(id, broken, k_bent_bounds));
    GIANT_CHECK(!r.set_pose(id, pose, {{0, 0, 0}, -1.0f}));
    GIANT_CHECK(!r.set_pose(id, pose, {{0, std::numeric_limits<float>::infinity(), 0}, 1.0f}));
    r.remove_mesh(id);
    GIANT_CHECK(!r.skinned(id) && !r.set_pose(id, pose, k_bent_bounds));
}

GIANT_TEST("skin: a pose is put on the GPU once, after the mesh loads, and again when it changes") {
    NullScene s;
    if (s.renderer == nullptr) return;
    Renderer& r = *s.renderer;
    const MeshId id = r.add_skinned_mesh(two_balls(), 2);
    GIANT_CHECK(id != k_no_mesh);
    std::vector<Instance> instances(1);
    instances[0].mesh = id;
    instances[0].position = {0.0, 1.5, -10.0};
    FrameInput frame;
    frame.instances = instances;
    frame.settings.gpu_culling = false;
    RenderStats st = s.draw(frame);
    for (int i = 0; i < 8 && !r.mesh_ready(id); ++i) st = s.draw(frame);
    GIANT_CHECK(r.mesh_ready(id));
    GIANT_CHECK(st.posed == 1 && st.skinned_meshes == 1); // the bind pose, once loaded
    const u32 dispatches = s.device->last_frame_stats().dispatches;
    st = s.draw(frame);
    GIANT_CHECK(st.posed == 0);
    GIANT_CHECK(s.device->last_frame_stats().dispatches + 1 == dispatches);
    GIANT_CHECK(r.set_pose(id, bent(), k_bent_bounds));
    st = s.draw(frame);
    GIANT_CHECK(st.posed == 1 && s.device->last_frame_stats().dispatches == dispatches);
    st = s.draw(frame);
    GIANT_CHECK(st.posed == 0);
    // Posed even when not in view (another instance of it may be, or a shadow).
    instances[0].position = {0.0, 0.0, 1000.0};
    GIANT_CHECK(r.set_pose(id, bent(), k_bent_bounds));
    st = s.draw(frame);
    GIANT_CHECK(st.posed == 1 && st.instances == 0);
    r.remove_mesh(id);
    st = s.draw(frame);
    GIANT_CHECK(st.posed == 0 && st.skinned_meshes == 0);
}

GIANT_TEST("skin: the CPU cut takes every cluster when the pose is in view, none when it is out") {
    NullScene s;
    if (s.renderer == nullptr) return;
    Renderer& r = *s.renderer;
    const MeshId id = r.add_skinned_mesh(two_balls(), 2);
    const usize clusters = r.mesh(id)->clusters.size();
    GIANT_CHECK(clusters > 16);
    std::vector<Instance> instances(1);
    instances[0].mesh = id;
    instances[0].position = {0.0, 0.0, -1000.0}; // far: a plain mesh would draw a few coarse clusters
    FrameInput frame;
    frame.instances = instances;
    frame.settings.gpu_culling = false;
    frame.view.vertical_fov_degrees = 30.0f;
    RenderStats st = s.draw(frame);
    for (int i = 0; i < 8 && !r.mesh_ready(id); ++i) st = s.draw(frame);
    st = s.draw(frame);
    GIANT_CHECK(st.instances == 1 && st.clusters == clusters);
    // Looking at where the hand is bent to, from close by: the bind pose
    // is out of view, the pose in it.
    instances[0].position = {0.0, 0.0, 0.0};
    frame.view.position = {4.0, 0.0, 3.0};
    st = s.draw(frame);
    GIANT_CHECK(st.instances == 0 && st.clusters == 0);
    GIANT_CHECK(r.set_pose(id, bent(), k_bent_bounds));
    st = s.draw(frame);
    GIANT_CHECK(st.instances == 1 && st.clusters == clusters);
}

GIANT_TEST("skin: a skinned mesh is never left to the far field nor held by the voxel world") {
    NullScene s;
    if (s.renderer == nullptr) return;
    Renderer& r = *s.renderer;
    const MeshId id = r.add_skinned_mesh(two_balls(), 2);
    std::vector<Instance> instances(2);
    instances[0].mesh = id;
    instances[0].scale = {10, 10, 10};
    instances[0].position = {0.0, 0.0, -1000.0};
    instances[1].mesh = r.builtin(BuiltinMesh::sphere);
    instances[1].scale = {10, 10, 10};
    instances[1].position = {300.0, 0.0, -1000.0};
    FrameInput frame;
    frame.instances = instances;
    frame.settings.gpu_culling = false;
    frame.settings.far_field_distance = 500.0f;
    frame.settings.voxel_ms_per_frame = 50.0f;
    RenderStats st = s.draw(frame);
    for (int i = 0; i < 200 && (st.voxel_pending > 0 || !r.mesh_ready(id)); ++i) st = s.draw(frame);
    st = s.draw(frame);
    GIANT_CHECK(st.far_field && st.far_culled == 1 && st.instances == 1); // the plain ball alone
    const auto solid_at = [&](const DVec3& p) {
        bool solid = false;
        for (const VoxelProbeLevel& l : r.probe_voxels(p)) solid = solid || std::string(l.state) == "solid";
        return solid;
    };
    GIANT_CHECK(solid_at({300.0, 0.0, -1000.0}));
    GIANT_CHECK(!solid_at({0.0, 0.0, -1000.0}));
}

GIANT_TEST("skin: d3d12 draws the pose, culled by refit clusters, and moves its shadow") {
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
    const auto render = [&](FrameInput& frame, int frames) {
        for (int i = 0; i < frames; ++i) {
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
    const auto green = [&](const std::vector<u8>& p, u32 x, u32 y) {
        const usize i = (static_cast<usize>(y) * k_w + x) * 4;
        return p.size() == k_w * k_h * 4 && p[i + 1] > 100 && p[i + 2] < 60;
    };

    // The hand, emissive green on blue, seen where the pose bends it to:
    // its bind place is out of view, so only clusters refit to the pose
    // pass culling.
    const MeshId id = renderer->add_skinned_mesh(two_balls(), 2);
    GIANT_CHECK(id != k_no_mesh);
    const DVec3 origin{-30'000.0, 50.0, 90'000.0};
    std::vector<Instance> instances(1);
    instances[0].mesh = id;
    instances[0].position = origin;
    std::vector<Material> materials(1);
    materials[0].base_color = {0.0f, 0.0f, 0.0f};
    materials[0].emissive = {0.0f, 0.8f, 0.0f};
    FrameInput frame;
    frame.settings.auto_exposure = false; // (Lighting::exposure alone, no bloom: the colours checked hold)
    frame.settings.bloom = false;
    frame.view.position = origin + DVec3{3.0, 0.0, 4.0};
    frame.view.vertical_fov_degrees = 30.0f;
    frame.instances = instances;
    frame.materials = materials;
    frame.lighting.background = {0.0f, 0.0f, 1.0f};
    frame.settings.shadows = false;
    frame.settings.gi = false;
    frame.settings.antialiasing = false;
    const std::array<bool, 3> gpu = {false, true, true};
    const std::array<bool, 3> mesh_shaders = {false, false, true};
    for (usize mode = 0; mode < gpu.size(); ++mode) {
        frame.settings.gpu_culling = gpu[mode];
        frame.settings.mesh_shaders = mesh_shaders[mode];
        GIANT_CHECK(renderer->set_pose(id, std::array<Mat4, 2>{}, two_balls().mesh.bounds)); // bind
        const auto bind = render(frame, 8);
        GIANT_CHECK(!green(bind, k_w / 2, k_h / 2));
        GIANT_CHECK(renderer->set_pose(id, bent(), k_bent_bounds));
        const auto posed = render(frame, 8);
        GIANT_CHECK(green(posed, k_w / 2, k_h / 2)); // the hand, ahead
        GIANT_CHECK(!green(posed, 2, 2));            // the corner: background
    }

    // Shadows: the hand over ground, seen from above, the sun to +X. Moved
    // 3 m by the pose, its shadow goes with it.
    instances.resize(2);
    instances[1].mesh = renderer->builtin(BuiltinMesh::plane);
    instances[1].position = origin + DVec3{0.0, -1.5, 0.0};
    instances[1].scale = {40.0f, 1.0f, 40.0f};
    instances[1].material = 1;
    instances[0].position = origin + DVec3{0.0, -3.0, 0.0}; // the hand 1.5 m over the ground, the other ball under it
    materials.resize(2);
    materials[1].base_color = {0.7f, 0.7f, 0.7f};
    materials[1].roughness = 0.9f;
    materials[0] = materials[1];
    frame.instances = instances;
    frame.materials = materials;
    frame.view.position = origin + DVec3{0.0, 4.5, 0.0};
    frame.view.orientation = quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -k_pi * 0.5f);
    frame.view.vertical_fov_degrees = 60.0f;
    frame.lighting.sun_direction = normalize_or_zero(Vec3{0.6f, 0.8f, 0.0f});
    frame.lighting.sun_light = {3.0f, 3.0f, 3.0f};
    frame.lighting.sky_light = {0.1f, 0.1f, 0.1f};
    frame.lighting.ground_light = {0.1f, 0.1f, 0.1f};
    frame.settings.gpu_culling = true;
    frame.settings.mesh_shaders = true;
    GIANT_CHECK(renderer->set_pose(id, std::array<Mat4, 2>{}, two_balls().mesh.bounds));
    frame.settings.shadows = false;
    const auto plain = render(frame, 16);
    frame.settings.shadows = true;
    const auto shadowed = render(frame, 16);
    // The hand pushed 3 m to -Z (up the screen): the shadow leaves.
    const Mat4 away = mat4_from_transform({0.0f, 0.0f, -3.0f}, Quat{}, {1.0f, 1.0f, 1.0f});
    GIANT_CHECK(renderer->set_pose(id, std::array<Mat4, 2>{Mat4{}, away}, {{0.0f, 1.5f, -1.5f}, 3.0f}));
    const auto moved = render(frame, 16);
    if (plain.size() == k_w * k_h * 4 && shadowed.size() == plain.size() && moved.size() == plain.size()) {
        const auto at = [](const std::vector<u8>& p, u32 x, u32 y) { return int{p[(static_cast<usize>(y) * k_w + x) * 4]}; };
        // About 0.108 m a pixel on the ground; the hand's shadow falls about
        // 1.1 m to -X of it.
        const u32 shade_x = k_w / 2 - 10;
        GIANT_CHECK(at(shadowed, shade_x, k_h / 2) < at(plain, shade_x, k_h / 2) / 2);
        GIANT_CHECK(std::abs(at(moved, shade_x, k_h / 2) - at(plain, shade_x, k_h / 2)) <= 3); // sunlit again
    }
    GIANT_CHECK(!device->device_lost());
    GIANT_CHECK(device->validation_errors() == 0);
    device->destroy(target);
}

GIANT_TEST("skin: poses set before loading are put on once; slots go between skinned and plain meshes") {
    NullScene s;
    if (s.renderer == nullptr) return;
    Renderer& r = *s.renderer;
    const MeshId id = r.add_skinned_mesh(two_balls(), 2);
    GIANT_CHECK(r.set_pose(id, bent(), k_bent_bounds));
    GIANT_CHECK(r.set_pose(id, bent(), k_bent_bounds));
    std::vector<Instance> instances(1);
    instances[0].mesh = id;
    instances[0].position = {0.0, 0.0, -10.0};
    FrameInput frame;
    frame.instances = instances;
    // GPU culling, with shadows (their tracking of skinned meshes runs).
    frame.settings.gpu_culling = true;
    u32 posed = 0;
    for (int i = 0; i < 12; ++i) {
        posed += s.draw(frame).posed;
    }
    GIANT_CHECK(r.mesh_ready(id) && posed == 1);
    GIANT_CHECK(r.set_pose(id, two_balls_bind(), two_balls().mesh.bounds));
    GIANT_CHECK(s.draw(frame).posed == 1);
    // Its slot goes to a plain mesh, then to a skinned one again.
    r.remove_mesh(id);
    GIANT_CHECK(s.draw(frame).skinned_meshes == 0);
    const MeshId plain = r.add_mesh(geometry::build_clusters(geometry::make_cube()));
    GIANT_CHECK(plain == id && !r.skinned(plain) && !r.set_pose(plain, bent(), k_bent_bounds));
    r.remove_mesh(plain);
    const MeshId again = r.add_skinned_mesh(two_balls(), 2);
    GIANT_CHECK(again == id && r.skinned(again));
    posed = 0;
    for (int i = 0; i < 12; ++i) {
        posed += s.draw(frame).posed;
    }
    GIANT_CHECK(posed == 1 && s.renderer->last_stats().skinned_meshes == 1);
}

GIANT_TEST("skin: a pose changing under a lamp draws the lamp's shadow again") {
    NullScene s;
    if (s.renderer == nullptr) return;
    Renderer& r = *s.renderer;
    const MeshId id = r.add_skinned_mesh(two_balls(), 2);
    std::vector<Instance> instances(1);
    instances[0].mesh = id;
    instances[0].position = {0.0, 0.0, -4.0};
    std::vector<LocalLight> lights(1);
    lights[0].position = {0.0, 2.0, -4.0};
    lights[0].range = 8.0f;
    lights[0].shadow_id = 3;
    FrameInput frame;
    frame.instances = instances;
    frame.lights = lights;
    RenderStats st = s.draw(frame);
    for (int i = 0; i < 8 && !r.mesh_ready(id); ++i) st = s.draw(frame);
    s.draw(frame);
    st = s.draw(frame);
    GIANT_CHECK(st.local_shadows == 1 && st.local_shadow_drawn == 0); // settled
    // Posed in place (its instance unmoved): drawn again.
    GIANT_CHECK(r.set_pose(id, bent(), k_bent_bounds));
    st = s.draw(frame);
    GIANT_CHECK(st.local_shadow_drawn == 6);
    st = s.draw(frame);
    GIANT_CHECK(st.local_shadow_drawn == 0);
}
