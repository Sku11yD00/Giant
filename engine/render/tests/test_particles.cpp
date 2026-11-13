// Particles (src/particles.h): the closed-form motion, births, the ring,
// and particles through a frame on the null device.
#include "particles.h"

#include "giant/render/renderer.h"
#include "giant/rhi/rhi.h"
#include "giant_test.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

using namespace giant;
using namespace giant::render;

namespace {

bool near(float a, float b, float tolerance) { return std::abs(a - b) <= tolerance; }
bool near(const Vec3& a, const Vec3& b, float tolerance) { return length(a - b) <= tolerance; }

// Small explicit steps of the same motion, to check the closed form by.
Vec3 stepped(const ParticleMotion& m, float age) {
    using D = DVec3;
    const auto wide = [](const Vec3& v) { return D{v.x, v.y, v.z}; };
    D x = wide(m.x0);
    D v = wide(m.v0);
    const D air = wide(m.air);
    const int steps = 100000;
    const double dt = static_cast<double>(age) / steps;
    for (int i = 0; i < steps; ++i) {
        // Midpoint (RK2) on the velocity, trapezoid on the position.
        const D a = (air - v) * double{m.drag} + D{0.0, m.lift, 0.0};
        const D v_half = v + a * (0.5 * dt);
        const D a_half = (air - v_half) * double{m.drag} + D{0.0, m.lift, 0.0};
        const D v_next = v + a_half * dt;
        x += (v + v_next) * (0.5 * dt);
        v = v_next;
    }
    return {static_cast<float>(x.x), static_cast<float>(x.y), static_cast<float>(x.z)};
}

ParticleStyle smoke() {
    ParticleStyle s;
    s.life_min = 2.0f;
    s.life_max = 3.0f;
    s.gravity = 0.0f;
    s.buoyancy = 1.5f;
    s.drag = 2.0f;
    return s;
}

ParticleBurst burst_at(const DVec3& at, u32 count, u32 seed = 1) {
    ParticleBurst b;
    b.position = at;
    b.count = count;
    b.seed = seed;
    return b;
}

} // namespace

GIANT_TEST("particles: without drag a particle flies like a thrown stone") {
    ParticleMotion m;
    m.x0 = {1.0f, 2.0f, 3.0f};
    m.v0 = {4.0f, 10.0f, -2.0f};
    m.air = {30.0f, 0.0f, 0.0f}; // no drag: the air does nothing
    for (const float t : {0.0f, 0.25f, 1.0f, 2.5f}) {
        const Vec3 want = m.x0 + m.v0 * t + Vec3{0.0f, -k_particle_gravity, 0.0f} * (0.5f * t * t);
        GIANT_CHECK(near(particle_position(m, t), want, 1e-4f * (1.0f + t * t)));
        GIANT_CHECK(near(particle_velocity(m, t), m.v0 + Vec3{0.0f, -k_particle_gravity * t, 0.0f}, 1e-4f));
    }
}

GIANT_TEST("particles: with drag the closed form matches small steps and ends at the air's speed") {
    for (const float drag : {0.004f, 0.3f, 2.0f, 25.0f}) { // across the series and exp forms
        ParticleMotion m;
        m.v0 = {5.0f, 8.0f, 0.0f};
        m.air = {-3.0f, 0.0f, 2.0f};
        m.lift = 1.5f - k_particle_gravity;
        m.drag = drag;
        for (const float t : {0.1f, 0.7f, 2.0f}) {
            const Vec3 want = stepped(m, t);
            GIANT_CHECK(near(particle_position(m, t), want, 1e-4f * (1.0f + length(want))));
        }
    }
    // Long after: the air's speed plus what lift settles to against the drag.
    ParticleMotion m;
    m.v0 = {20.0f, 0.0f, 0.0f};
    m.air = {2.0f, 0.0f, 1.0f};
    m.lift = 3.0f;
    m.drag = 1.5f;
    GIANT_CHECK(near(particle_velocity(m, 30.0f), Vec3{2.0f, 3.0f / 1.5f, 1.0f}, 1e-3f));
}

GIANT_TEST("particles: a little drag over a long life stays as exact as floats are") {
    for (const float drag : {0.0003f, 0.001f, 0.004f, 0.02f, 0.05f, 0.2f}) {
        ParticleMotion m;
        m.v0 = {12.0f, 30.0f, -4.0f};
        m.air = {6.0f, 0.0f, 0.0f};
        m.lift = -k_particle_gravity;
        m.drag = drag;
        for (float t = 1.0f; t <= 60.0f; t += 0.37f) {
            // Exact, in doubles.
            const double k = drag;
            const double e1 = -std::expm1(-k * t) / k;
            const double e2 = (t - e1) / k;
            const double y = m.air.y * t + (m.v0.y - m.air.y) * e1 + m.lift * e2;
            const double x = m.air.x * t + (m.v0.x - m.air.x) * e1;
            const Vec3 p = particle_position(m, t);
            const double scale = 1.0 + std::abs(y) + std::abs(x);
            GIANT_CHECK(std::abs(p.y - y) < 2e-6 * scale + 1e-3 && std::abs(p.x - x) < 2e-6 * scale + 1e-3);
        }
    }
}

GIANT_TEST("particles: the ground stops a falling particle where it reaches it") {
    // Thrown up at 10 m/s from 2 m above the ground, no drag: lands at the
    // quadratic's root.
    ParticleMotion m;
    m.v0 = {3.0f, 10.0f, 0.0f};
    const float g = k_particle_gravity;
    const float root = (10.0f + std::sqrt(100.0f + 2.0f * g * 2.0f)) / g;
    const float stop = particle_ground_stop(m, -2.0f, 5.0f);
    GIANT_CHECK(near(stop, root, 1e-4f));
    m.stop = stop;
    const Vec3 rest = particle_position(m, stop);
    GIANT_CHECK(near(rest.y, -2.0f, 1e-3f));
    GIANT_CHECK(particle_position(m, stop + 3.0f) == rest);        // held there
    GIANT_CHECK(particle_velocity(m, stop + 0.01f) == Vec3{});      // still
    GIANT_CHECK(particle_ground_stop(m, -2.0f, root - 0.1f) > root - 0.1f); // dies first: never
    // With drag: on the ground at the stop, above it just before.
    m.drag = 1.7f;
    m.air = {0.0f, 0.0f, 4.0f};
    m.stop = 1e30f;
    const float s2 = particle_ground_stop(m, -2.0f, 10.0f);
    GIANT_CHECK(s2 > 0.0f && s2 <= 10.0f);
    GIANT_CHECK(near(particle_position(m, s2).y, -2.0f, 1e-3f));
    GIANT_CHECK(particle_position(m, s2 - 0.01f).y > -2.0f);
    // Born at or under the ground: stopped at once.
    GIANT_CHECK(particle_ground_stop(m, 0.0f, 3.0f) == 0.0f);
    // Going down but rising (smoke shot down): crosses on the way down, or
    // turns before it and never does.
    ParticleMotion up;
    up.v0 = {0.0f, -6.0f, 0.0f};
    up.lift = 4.0f;
    up.drag = 1.0f;
    const float s3 = particle_ground_stop(up, -1.0f, 6.0f);
    GIANT_CHECK(s3 > 0.0f && s3 < 1.0f);
    GIANT_CHECK(near(particle_position(up, s3).y, -1.0f, 1e-3f));
    GIANT_CHECK(particle_ground_stop(up, -50.0f, 6.0f) > 6.0f);
    // Dropped from high up: lands when it should.
    ParticleMotion high;
    high.x0.y = 3000.0f;
    const float s4 = particle_ground_stop(high, 0.0f, 60.0f);
    GIANT_CHECK(near(s4, std::sqrt(2.0f * 3000.0f / k_particle_gravity), 1e-4f));
}

GIANT_TEST("particles: births are the same for the same seed, inside the burst's cone, speeds and spread") {
    std::vector<ParticleStyle> styles{smoke()};
    ParticleBurst b = burst_at({100.0, 20.0, -50.0}, 500, 7);
    b.direction = {1.0f, 1.0f, 0.0f};
    b.cone_degrees = 20.0f;
    b.speed_min = 3.0f;
    b.speed_max = 5.0f;
    b.spread = 0.5f;
    b.velocity = {0.0f, 0.0f, 10.0f};
    b.size_scale = 2.0f;
    ParticleRing a;
    ParticleRing c;
    const DVec3 camera{90.0, 25.0, -40.0};
    a.update({&b, 1}, styles, 0.0, camera, {});
    c.update({&b, 1}, styles, 0.0, camera, {});
    GIANT_CHECK(a.frame().born == 500 && a.born_records().size() == 500);
    GIANT_CHECK(std::memcmp(a.born_records().data(), c.born_records().data(), 500 * sizeof(gpu::Particle)) == 0);
    const Vec3 dir = normalize_or_zero(b.direction);
    const Vec3 centre = to_camera_relative(b.position, a.frame().anchor);
    for (const gpu::Particle& p : a.born_records()) {
        const Vec3 v = Vec3{p.velocity[0], p.velocity[1], p.velocity[2]} - b.velocity;
        const float speed = length(v);
        GIANT_CHECK(speed >= 3.0f - 1e-4f && speed <= 5.0f + 1e-4f);
        GIANT_CHECK(dot(v / speed, dir) >= std::cos(degrees_to_radians(20.0f)) - 1e-4f);
        GIANT_CHECK(length(Vec3{p.position[0], p.position[1], p.position[2]} - centre) <= 0.5f + 1e-4f);
        GIANT_CHECK(p.life >= 2.0f && p.life <= 3.0f);
        GIANT_CHECK((p.seed_style & 0xffu) == 0 && p.size == 2.0f);
        GIANT_CHECK(near(p.lift, 1.5f, 1e-6f) && p.drag == 2.0f && p.stop >= 1e29f);
    }
    // Another seed: other particles.
    b.seed = 8;
    c.update({&b, 1}, styles, 0.0, camera, {});
    GIANT_CHECK(std::memcmp(a.born_records().data(), c.born_records().data(), 500 * sizeof(gpu::Particle)) != 0);
}

GIANT_TEST("particles: the wind is the air they drift in, by their style's share") {
    std::vector<ParticleStyle> styles{smoke()};
    styles[0].wind = 0.5f;
    ParticleBurst b = burst_at({}, 4);
    WindState wind;
    wind.velocity = {8.0f, 0.0f, -2.0f};
    ParticleRing ring;
    ring.update({&b, 1}, styles, 0.0, {}, wind);
    for (const gpu::Particle& p : ring.born_records()) {
        GIANT_CHECK(near(Vec3{p.air[0], p.air[1], p.air[2]}, Vec3{4.0f, 0.0f, -1.0f}, 1e-6f));
    }
}

GIANT_TEST("particles: sparks falling to the ground given stop on it") {
    ParticleStyle spark;
    spark.life_min = 3.0f;
    spark.life_max = 3.0f;
    spark.shape = ParticleShape::streak;
    std::vector<ParticleStyle> styles{spark};
    ParticleBurst b = burst_at({0.0, 101.5, 0.0}, 200);
    b.cone_degrees = 60.0f;
    b.speed_min = 2.0f;
    b.speed_max = 6.0f;
    b.ground = 100.0;
    ParticleRing ring;
    ring.update({&b, 1}, styles, 0.0, {0.0, 102.0, 5.0}, {});
    for (const gpu::Particle& p : ring.born_records()) {
        GIANT_CHECK(p.stop > 0.0f && p.stop <= p.life); // all land within 3 s
        const Vec3 rest = particle_position(ParticleRing::motion(p), p.stop + 1.0f);
        const double y = ring.frame().anchor.y + static_cast<double>(rest.y);
        GIANT_CHECK(std::abs(y - 100.0) < 2e-3);
    }
}

GIANT_TEST("particles: the alive range ends when its particles die") {
    std::vector<ParticleStyle> styles{smoke()}; // 2 to 3 s
    ParticleBurst b = burst_at({}, 100);
    ParticleRing ring;
    ring.update({&b, 1}, styles, 0.0, {}, {});
    GIANT_CHECK(ring.frame().count == 100 && ring.frame().first == 0 && ring.frame().born_first == 0);
    ring.update({&b, 1}, styles, 1.0, {}, {}); // another 100, a second later
    GIANT_CHECK(ring.frame().count == 200 && ring.frame().born_first == 100 && ring.spans() == 2);
    for (const gpu::Particle& p : ring.born_records()) GIANT_CHECK(ring.age(p) == 0.0f);
    ring.update({}, styles, 1.0, {}, {});
    GIANT_CHECK(ring.frame().count == 200); // the first lives to 2-3 s
    ring.update({}, styles, 1.0, {}, {});    // 3 s: the first span is gone
    GIANT_CHECK(ring.frame().count == 100 && ring.frame().first == 100);
    // Held (no time passing): nothing dies.
    ring.update({}, styles, 0.0, {}, {});
    GIANT_CHECK(ring.frame().count == 100);
    ring.update({}, styles, 0.0, {}, {});
    ring.update({}, styles, 1.0, {}, {});
    GIANT_CHECK(ring.frame().count == 0 && ring.spans() == 0);
    // Bad clocks count as held; long ones as a second.
    ring.update({&b, 1}, styles, std::nan(""), {}, {});
    ring.update({}, styles, -2.0, {}, {});
    GIANT_CHECK(ring.frame().count == 100);
    ring.update({}, styles, 5.0, {}, {});
    ring.update({}, styles, 1.0, {}, {});
    GIANT_CHECK(ring.frame().count == 100); // 2 s of 2 to 3
    ring.update({}, styles, 1.0, {}, {});
    GIANT_CHECK(ring.frame().count == 0);
    // Counts never wrap.
    ParticleBurst huge[2] = {burst_at({}, ~0u), burst_at({}, ~0u)};
    ring.update(huge, styles, 0.0, {}, {});
    GIANT_CHECK(ring.frame().dropped == ~0u);
}

GIANT_TEST("particles: the ring wraps, a full ring makes the oldest make way, births are capped") {
    std::vector<ParticleStyle> styles{smoke()};
    styles[0].life_min = styles[0].life_max = 30.0f;
    ParticleBurst b = burst_at({}, k_max_particles_born);
    ParticleRing ring;
    constexpr u32 frames = k_max_particles / k_max_particles_born;
    for (u32 i = 0; i < frames; ++i) ring.update({&b, 1}, styles, 0.01, {}, {});
    GIANT_CHECK(ring.frame().count == k_max_particles && ring.frame().cut == 0);
    // Another: wraps to the start, cutting the oldest frame's.
    ring.update({&b, 1}, styles, 0.01, {}, {});
    GIANT_CHECK(ring.frame().born_first == 0 && ring.frame().cut == k_max_particles_born);
    GIANT_CHECK(ring.frame().first == k_max_particles_born && ring.frame().count == k_max_particles);
    GIANT_CHECK(ring.spans() == frames);
    // Past the cap a frame: left out (counted).
    ParticleBurst two[2] = {b, b};
    two[1].count = 10;
    ring.update(two, styles, 0.01, {}, {});
    GIANT_CHECK(ring.frame().born == k_max_particles_born && ring.frame().dropped == 10);
    // Those with a ground have a cap of their own.
    ParticleBurst landing[2] = {burst_at({}, k_max_particles_grounded + 100), burst_at({}, 30)};
    landing[0].ground = -3.0;
    ring.update(landing, styles, 0.01, {}, {});
    GIANT_CHECK(ring.frame().born == k_max_particles_grounded + 30 && ring.frame().dropped == 100);
    // A burst crossing the end of the ring goes on at its start.
    ParticleRing small;
    ParticleBurst odd = burst_at({}, 10000);
    for (u32 i = 0; i < 27; ++i) small.update({&odd, 1}, styles, 0.01, {}, {});
    GIANT_CHECK(small.frame().born_first == 260000u && small.frame().count == 262144u);
    GIANT_CHECK(small.frame().cut == 10000u - (k_max_particles - 260000u));
    // Taken back (could not be sent): dropped, and the ring as it was.
    small.forget_births();
    GIANT_CHECK(small.frame().born == 0 && small.frame().dropped == 10000 && small.alive() == 252144u);
    small.update({&odd, 1}, styles, 0.01, {}, {});
    GIANT_CHECK(small.frame().born_first == 260000u);
}

GIANT_TEST("particles: bad numbers, styles and far bursts are left out, not drawn") {
    std::vector<ParticleStyle> styles{smoke(), smoke(), smoke()};
    styles[1].life_min = 0.0f;           // no life
    styles[2].size_end = std::nanf("");  // not a number
    GIANT_CHECK(particle_style_ok(styles[0]) && !particle_style_ok(styles[1]) && !particle_style_ok(styles[2]));
    // Finite but past the bounds: as bad (a float would overflow in the motion or the look).
    for (int i = 0; i < 4; ++i) {
        ParticleStyle big = smoke();
        if (i == 0) big.gravity = 1e38f;
        if (i == 1) big.buoyancy = -1e30f;
        if (i == 2) big.size_end = 1e6f;
        if (i == 3) big.emissive = {1e9f, 0.0f, 0.0f};
        GIANT_CHECK(!particle_style_ok(big));
    }
    GIANT_CHECK(to_gpu(styles[1], false).flags == 0);
    GIANT_CHECK((to_gpu(styles[0], true).flags & gpu::k_particle_style_usable) != 0);
    std::vector<ParticleBurst> bursts(8, burst_at({}, 10));
    bursts[1].style = 1;
    bursts[2].style = 2;
    bursts[3].style = 3; // no such style
    bursts[4].direction = {};
    bursts[5].speed_min = 5.0f; // over speed_max
    bursts[6].position = {k_particle_reach + 10.0, 0.0, 0.0};
    bursts[7].position.y = std::nan("");
    ParticleRing ring;
    ring.update(bursts, styles, 0.0, {}, {});
    GIANT_CHECK(ring.frame().born == 10 && ring.frame().dropped == 70);
}

GIANT_TEST("particles: positions are kept near the camera; going far drops them") {
    std::vector<ParticleStyle> styles{smoke()};
    styles[0].life_min = styles[0].life_max = 30.0f;
    const DVec3 far{3.0e6, 10.0, -2.0e6};
    ParticleBurst b = burst_at(far + DVec3{1.0, 2.0, 3.0}, 3);
    ParticleRing ring;
    ring.update({&b, 1}, styles, 0.0, far, {});
    GIANT_CHECK(ring.frame().anchor == far);
    for (const gpu::Particle& p : ring.born_records()) GIANT_CHECK(near(p.position[1], 2.0f, 1e-6f));
    // A few kilometres on: kept, the anchor stays.
    ring.update({}, styles, 0.0, far + DVec3{3000.0, 0.0, 0.0}, {});
    GIANT_CHECK(ring.frame().count == 3 && ring.frame().anchor == far);
    // Past k_particle_reanchor: all go, and the anchor follows the camera.
    const DVec3 there = far + DVec3{5000.0, 0.0, 0.0};
    ring.update({}, styles, 0.0, there, {});
    GIANT_CHECK(ring.frame().count == 0 && ring.frame().cut == 3 && ring.frame().anchor == there);
}

GIANT_TEST("particles: born and alive reach the frame's stats on the null device") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    auto device = rhi::create_device(desc);
    auto renderer = Renderer::create(*device);
    GIANT_CHECK(renderer != nullptr);
    rhi::TextureDesc t;
    t.width = 64;
    t.height = 64;
    t.format = rhi::Format::rgba8_srgb;
    t.usage = rhi::TextureUsage::render_target;
    const rhi::TextureHandle target = device->create_texture(t);
    const auto draw = [&](const FrameInput& frame) {
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        renderer->render(*cmd, target, frame);
        device->end_frame();
        return renderer->last_stats();
    };
    std::vector<ParticleStyle> styles{smoke()};
    std::vector<ParticleBurst> bursts{burst_at({0.0, 0.0, -5.0}, 300), burst_at({0.0, 0.0, -5.0}, 5)};
    bursts[1].style = 9; // no such style
    FrameInput frame;
    frame.settings.gpu_culling = false;
    frame.particle_styles = styles;
    frame.particle_bursts = bursts;
    frame.delta_seconds = 0.0f;
    RenderStats s = draw(frame);
    GIANT_CHECK(s.particles == 300 && s.particles_born == 300 && s.particles_dropped == 5 && s.particles_cut == 0);
    frame.particle_bursts = {};
    // Their own clock (the game's): stood still however long the frame.
    frame.delta_seconds = 1.0f;
    frame.particle_seconds = 0.0f;
    for (int i = 0; i < 8; ++i) s = draw(frame);
    GIANT_CHECK(s.particles == 300);
    frame.particle_seconds = -1.0f; // (negative: delta_seconds)
    frame.delta_seconds = 0.5f;
    s = draw(frame);
    GIANT_CHECK(s.particles == 300 && s.particles_born == 0);
    for (int i = 0; i < 6; ++i) s = draw(frame); // 3.5 s: all dead
    GIANT_CHECK(s.particles == 0);
}

GIANT_TEST("particles: births reach the GPU ring where the ring says, wrapping past its end") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    auto device = rhi::create_device(desc);
    auto renderer = Renderer::create(*device);
    GIANT_CHECK(renderer != nullptr);
    rhi::TextureDesc t;
    t.width = 32;
    t.height = 32;
    t.format = rhi::Format::rgba8_srgb;
    t.usage = rhi::TextureUsage::render_target;
    const rhi::TextureHandle target = device->create_texture(t);
    std::vector<ParticleStyle> styles{smoke()};
    styles[0].life_min = styles[0].life_max = 50.0f;
    FrameInput frame;
    frame.settings.gpu_culling = false;
    frame.settings.voxel_world = false;
    frame.particle_styles = styles;
    frame.delta_seconds = 0.01f;
    ParticleRing same; // the renderer's ring, born the same way
    std::vector<gpu::Particle> want(k_max_particles);
    for (u32 i = 0; i < 27; ++i) {
        ParticleBurst b = burst_at({1.0, 2.0, -3.0}, 10000, i);
        frame.particle_bursts = {&b, 1};
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        renderer->render(*cmd, target, frame);
        device->end_frame();
        same.update({&b, 1}, styles, 0.01, frame.view.position, frame.wind);
        const auto born = same.born_records();
        for (usize j = 0; j < born.size(); ++j) want[(same.frame().born_first + j) % k_max_particles] = born[j];
    }
    GIANT_CHECK(same.frame().born_first == 260000u); // the last crossed the end
    GIANT_CHECK(renderer->last_stats().particles == same.frame().count);
    const std::vector<u8> got = device->read_buffer(renderer->particle_ring());
    GIANT_CHECK(got.size() == want.size() * sizeof(gpu::Particle));
    GIANT_CHECK(std::memcmp(got.data(), want.data(), got.size()) == 0);
}

GIANT_TEST("particles: drawn after water in the lit view, two passes, only with some alive (null device)") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    auto device = rhi::create_device(desc);
    auto renderer = Renderer::create(*device);
    GIANT_CHECK(renderer != nullptr);
    rhi::TextureDesc t;
    t.width = 96;
    t.height = 64;
    t.format = rhi::Format::rgba8_srgb;
    t.usage = rhi::TextureUsage::render_target;
    const rhi::TextureHandle target = device->create_texture(t);
    const auto draw = [&](const FrameInput& frame) {
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        renderer->render(*cmd, target, frame);
        device->end_frame();
        return renderer->last_stats();
    };
    std::vector<ParticleStyle> styles{smoke()};
    FrameInput frame;
    frame.settings.gpu_culling = false;
    frame.particle_styles = styles;
    frame.delta_seconds = 0.0f;
    const u32 bare = draw(frame).post_draws; // none alive
    GIANT_CHECK(!renderer->last_stats().particles_drawn);
    ParticleBurst b = burst_at({0.0, 0.0, -5.0}, 50);
    frame.particle_bursts = {&b, 1};
    RenderStats s = draw(frame);
    GIANT_CHECK(s.particles_drawn && s.post_draws == bare + 2);
    frame.particle_bursts = {};
    frame.settings.particles = false; // off: still alive, not drawn
    s = draw(frame);
    GIANT_CHECK(!s.particles_drawn && s.post_draws == bare && s.particles == 50);
    frame.settings.particles = true;
    frame.settings.debug_view = DebugView::clusters; // not the lit view
    GIANT_CHECK(!draw(frame).particles_drawn);
    frame.settings.debug_view = DebugView::lit;
    frame.particle_styles = {}; // no styles: nothing to draw them with
    GIANT_CHECK(!draw(frame).particles_drawn);
}

GIANT_TEST("particles: d3d12 a puff lit by the sun, a glowing spark, hidden behind a wall, still from frame to frame") {
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
    std::vector<ParticleStyle> styles(2);
    styles[0].life_min = styles[0].life_max = 2.0f; // a white puff, opaque all its life, still
    styles[0].size_start = styles[0].size_end = 1.5f;
    styles[0].color_start = styles[0].color_end = {0.8f, 0.8f, 0.8f};
    styles[0].opacity_start = styles[0].opacity_end = 1.0f;
    styles[0].gravity = 0.0f;
    styles[0].soft = 0.0f;
    styles[1] = styles[0]; // a spark: unlit, glowing, streaking sideways
    styles[1].lit = false;
    styles[1].shape = ParticleShape::streak;
    styles[1].size_start = styles[1].size_end = 1.0f;
    styles[1].color_start = styles[1].color_end = {0.0f, 0.0f, 0.0f};
    styles[1].emissive = {8.0f, 4.0f, 1.0f};
    std::vector<Instance> wall(1);
    wall[0].mesh = renderer->builtin(BuiltinMesh::cube);
    wall[0].position = {0.0, 0.0, -3.0};
    wall[0].scale = {1.0f, 1.0f, 1.0f};
    std::vector<Material> materials(1);
    materials[0].base_color = {0.2f, 0.2f, 0.2f};
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
    frame.settings.ambient_occlusion = false;
    frame.settings.contact_shadows = false;
    frame.materials = materials;
    frame.particle_styles = styles;
    frame.lighting.background = {0.5f, 0.5f, 0.5f};
    frame.lighting.sun_direction = {0.0f, 0.0f, 1.0f}; // behind the camera (it looks down -Z)
    frame.lighting.sun_light = {3.0f, 3.0f, 3.0f};
    frame.lighting.sky_light = {0.3f, 0.3f, 0.3f};
    frame.lighting.ground_light = {0.1f, 0.1f, 0.1f};
    // A burst born, aged half a second (faded in), then held while
    // streaming settles; the image read; then all of them aged out.
    std::vector<u8> again;
    const auto draw = [&](std::span<const ParticleBurst> bursts) {
        int quiet = 0;
        for (int i = 0; i < 64 && (i < 4 || quiet < 4); ++i) {
            frame.particle_bursts = i == 0 ? bursts : std::span<const ParticleBurst>{};
            frame.delta_seconds = i == 1 ? 0.5f : 0.0f;
            rhi::CommandList* cmd = device->begin_frame(nullptr);
            GIANT_CHECK(cmd != nullptr);
            if (cmd != nullptr) {
                renderer->render(*cmd, target, frame);
                device->end_frame();
            }
            const RenderStats& st = renderer->last_stats();
            quiet = st.streamed_in == 0 && st.waiting == 0 ? quiet + 1 : 0;
        }
        GIANT_CHECK(renderer->last_stats().particles_drawn == !bursts.empty());
        device->wait_idle();
        std::vector<u8> image = device->read_texture(target);
        // One more frame, the clock held: the same image.
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        if (cmd != nullptr) {
            renderer->render(*cmd, target, frame);
            device->end_frame();
        }
        device->wait_idle();
        again = device->read_texture(target);
        // Aged out before the next.
        frame.delta_seconds = 1.0f;
        for (int i = 0; i < 3; ++i) {
            cmd = device->begin_frame(nullptr);
            if (cmd != nullptr) {
                renderer->render(*cmd, target, frame);
                device->end_frame();
            }
        }
        GIANT_CHECK(renderer->last_stats().particles == 0);
        return image;
    };
    const auto channel = [](const std::vector<u8>& image, u32 x, u32 y, u32 c) {
        const usize i = (static_cast<usize>(y) * k_w + x) * 4;
        return i + 3 < image.size() ? static_cast<int>(image[i + c]) : -1;
    };
    ParticleBurst puff;
    puff.position = {0.0, 0.0, -5.0};
    puff.speed_min = puff.speed_max = 0.0f;
    const std::vector<u8> bare = draw({});
    const std::vector<u8> lit = draw({&puff, 1});
    const bool still = again == lit;
    // The same puff in the dark: no light of its own, it hides the grey.
    frame.lighting.sun_light = {0.0f, 0.0f, 0.0f};
    frame.lighting.sky_light = {0.0f, 0.0f, 0.0f};
    frame.lighting.ground_light = {0.0f, 0.0f, 0.0f};
    const std::vector<u8> dark = draw({&puff, 1});
    // The spark glows in the dark.
    ParticleBurst spark = puff;
    spark.style = 1;
    spark.position.x = -3.0; // at the centre half a second on
    spark.velocity = {6.0f, 0.0f, 0.0f};
    const std::vector<u8> glowing = draw({&spark, 1});
    // Behind a wall: not seen.
    frame.lighting.sun_light = {3.0f, 3.0f, 3.0f};
    frame.lighting.sky_light = {0.3f, 0.3f, 0.3f};
    frame.instances = wall;
    const std::vector<u8> wall_only = draw({});
    const std::vector<u8> behind = draw({&puff, 1});
    int corner_change = 0;
    int wall_change = 0;
    for (u32 y = 0; y < k_h; ++y) {
        for (u32 x = 0; x < k_w; ++x) {
            if ((x < 6 || x >= k_w - 6) && (y < 6 || y >= k_h - 6)) {
                corner_change = std::max(corner_change, std::abs(channel(lit, x, y, 1) - channel(bare, x, y, 1)));
            }
            if (x >= 28 && x < 36 && y >= 28 && y < 36) {
                wall_change = std::max(wall_change, std::abs(channel(behind, x, y, 1) - channel(wall_only, x, y, 1)));
            }
        }
    }
    std::printf("    grey %d, lit puff %d, dark puff %d, spark red %d blue %d, corners changed %d, behind the wall changed %d, still %d\n",
                channel(bare, 32, 32, 1), channel(lit, 32, 32, 1), channel(dark, 32, 32, 1), channel(glowing, 32, 32, 0),
                channel(glowing, 32, 32, 2), corner_change, wall_change, still ? 1 : 0);
    GIANT_CHECK(channel(lit, 32, 32, 1) > channel(bare, 32, 32, 1) + 15);
    GIANT_CHECK(channel(dark, 32, 32, 1) + 30 < channel(bare, 32, 32, 1));
    GIANT_CHECK(channel(glowing, 32, 32, 0) > 200 && channel(glowing, 32, 32, 0) > channel(glowing, 32, 32, 2) + 40);
    GIANT_CHECK(corner_change <= 1);
    GIANT_CHECK(wall_change <= 2);
    GIANT_CHECK(still);
    GIANT_CHECK(!device->device_lost() && device->validation_errors() == 0);
    device->destroy(target);
}
