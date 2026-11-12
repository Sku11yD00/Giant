#include "water.h"

#include <algorithm>
#include <cmath>

namespace giant::render {

namespace {

bool finite(const DVec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

float clamp_or(float v, float lo, float hi, float fallback) {
    return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
}

gpu::WaterLook look_for_gpu(const WaterLook& l) {
    const WaterLook d;
    gpu::WaterLook g{};
    g.color[0] = clamp_or(l.color.x, 0.0f, 1.0f, d.color.x);
    g.color[1] = clamp_or(l.color.y, 0.0f, 1.0f, d.color.y);
    g.color[2] = clamp_or(l.color.z, 0.0f, 1.0f, d.color.z);
    g.absorption[0] = clamp_or(l.absorption.x, 0.0f, 10.0f, d.absorption.x);
    g.absorption[1] = clamp_or(l.absorption.y, 0.0f, 10.0f, d.absorption.y);
    g.absorption[2] = clamp_or(l.absorption.z, 0.0f, 10.0f, d.absorption.z);
    g.turbidity = clamp_or(l.turbidity, 0.0f, 1.0f, d.turbidity);
    g.waves = clamp_or(l.waves, 0.0f, 4.0f, d.waves);
    return g;
}

// A slope as a snorm16 of k_water_slope_most (a bad one: level).
u32 pack_slope(float slope) {
    const float s = std::isfinite(slope) ? std::clamp(slope / k_water_slope_most, -1.0f, 1.0f) : 0.0f;
    return static_cast<u32>(static_cast<u16>(static_cast<i16>(std::lround(s * 32767.0f))));
}

constexpr double k_two_pi = 6.283185307179586;
constexpr double k_gravity = 9.81;

} // namespace

WaterBatch pack_water(std::span<const WaterVertex> vertices, std::span<const u32> indices,
                      std::span<const WaterLook> looks, const DVec3& camera) {
    WaterBatch out;
    const usize look_count = std::min<usize>(looks.size(), k_max_water_looks);
    out.looks.reserve(look_count + 1);
    for (usize i = 0; i < look_count; ++i) out.looks.push_back(look_for_gpu(looks[i]));
    const u32 fallback_look = static_cast<u32>(out.looks.size());
    out.looks.push_back(look_for_gpu(WaterLook{})); // the default, last
    if (vertices.size() > k_max_water_vertices) out.overflow = true;
    const usize vertex_count = std::min<usize>(vertices.size(), k_max_water_vertices);
    // Each vertex once, camera-relative; a bad one marked so its triangles go.
    std::vector<u8> bad(vertex_count, 0);
    out.vertices.resize(vertex_count);
    for (usize i = 0; i < vertex_count; ++i) {
        const WaterVertex& v = vertices[i];
        gpu::WaterVertex& g = out.vertices[i];
        const DVec3 p = v.position - camera;
        if (!finite(v.position) || !finite(p) || !std::isfinite(v.flow_x) || !std::isfinite(v.flow_z)) {
            bad[i] = 1;
            continue;
        }
        g.position[0] = static_cast<float>(p.x);
        g.position[1] = static_cast<float>(p.y);
        g.position[2] = static_cast<float>(p.z);
        g.flow[0] = std::clamp(v.flow_x, -100.0f, 100.0f);
        g.flow[1] = std::clamp(v.flow_z, -100.0f, 100.0f);
        g.rapids = std::isfinite(v.rapids) ? std::clamp(v.rapids, 0.0f, 1.0f) : 0.0f;
        g.slope = pack_slope(v.slope_x) | (pack_slope(v.slope_z) << 16);
        g.look = v.look < fallback_look ? v.look : fallback_look;
    }
    const usize triangles = indices.size() / 3;
    if (triangles > k_max_water_triangles) out.overflow = true;
    const usize kept = std::min<usize>(triangles, k_max_water_triangles);
    out.indices.reserve(kept * 3);
    for (usize t = 0; t < kept; ++t) {
        const u32 a = indices[t * 3], b = indices[t * 3 + 1], c = indices[t * 3 + 2];
        const u32 most = std::max({a, b, c});
        if (most >= vertex_count && most < vertices.size()) continue; // its vertex left out: overflow's
        if (most >= vertex_count || bad[a] || bad[b] || bad[c]) {
            ++out.dropped;
            continue;
        }
        out.indices.insert(out.indices.end(), {a, b, c});
    }
    return out;
}

gpu::WaterLook pack_water_look(const WaterLook& look) { return look_for_gpu(look); }

float water_clock(double seconds, float period) {
    if (!std::isfinite(seconds) || !(period > 0.0f)) return 0.0f;
    const double t = std::fmod(seconds, static_cast<double>(period));
    const float folded = static_cast<float>(t < 0.0 ? t + period : t);
    return folded < period ? folded : 0.0f; // (a hair under the period rounds to it: the start)
}

float water_flow_clock(double seconds) { return water_clock(seconds, k_water_flow_period); }

float water_surf(const Vec3& velocity) {
    const float x = std::isfinite(velocity.x) ? velocity.x : 0.0f;
    const float z = std::isfinite(velocity.z) ? velocity.z : 0.0f;
    return std::clamp(std::sqrt(x * x + z * z) / 8.0f, 0.15f, 1.0f);
}

bool near_plane_under(const Quat& orientation, float tan_y, float aspect, float near_plane, double level_relative) {
    if (!std::isfinite(level_relative)) return false;
    const Vec3 right = rotate(orientation, {1.0f, 0.0f, 0.0f}) * (tan_y * aspect);
    const Vec3 up = rotate(orientation, {0.0f, 1.0f, 0.0f}) * tan_y;
    const Vec3 ahead = rotate(orientation, {0.0f, 0.0f, -1.0f});
    // The lowest corner: each axis's part taken downwards.
    const double lowest = (ahead.y - std::fabs(right.y) - std::fabs(up.y)) * static_cast<double>(near_plane);
    return lowest < level_relative;
}

std::array<gpu::WaterWave, k_water_waves> water_waves(const Vec3& velocity, double seconds, const DVec3& camera) {
    // The wind over the water (level); a calm keeps a breath of ripples.
    double wx = std::isfinite(velocity.x) ? velocity.x : 0.0;
    double wz = std::isfinite(velocity.z) ? velocity.z : 0.0;
    double speed = std::sqrt(wx * wx + wz * wz);
    if (!(speed > 1e-3)) {
        wx = 1.0;
        wz = 0.0;
        speed = 0.0;
    }
    const double angle = std::atan2(wz, wx);
    speed = std::min(speed, 40.0);
    // The longest wave: where a sea under this wind peaks (Pierson-Moskowitz,
    // 2 pi U^2 / (0.877^2 g)), at least 2 m and at most 200 m.
    const double longest = std::clamp(k_two_pi * speed * speed / (0.877 * 0.877 * k_gravity), 2.0, 200.0);
    // How rough: a calm's ripples, up to a gale's.
    const double rough = std::clamp(speed / 6.0, 0.15, 2.0);
    static constexpr double k_lengths[k_water_waves] = {1.0, 0.71, 0.5, 0.35, 0.24, 0.17, 0.11, 0.07};
    static constexpr double k_turns[k_water_waves] = {0.0, 0.55, -0.45, 1.05, -0.95, 0.3, -0.25, 1.5};
    static constexpr double k_steep[k_water_waves] = {0.05, 0.045, 0.04, 0.035, 0.03, 0.025, 0.02, 0.015};
    const double t = std::isfinite(seconds) ? seconds : 0.0;
    const double cx = std::isfinite(camera.x) ? camera.x : 0.0;
    const double cz = std::isfinite(camera.z) ? camera.z : 0.0;
    std::array<gpu::WaterWave, k_water_waves> out{};
    for (u32 i = 0; i < k_water_waves; ++i) {
        const double length = std::max(longest * k_lengths[i], 0.05);
        // As the GPU holds them (floats), so the phase below lines up with
        // what it computes from a camera anywhere.
        const double k = static_cast<float>(k_two_pi / length);
        const double dx = static_cast<float>(std::cos(angle + k_turns[i]));
        const double dz = static_cast<float>(std::sin(angle + k_turns[i]));
        const double omega = std::sqrt(k_gravity * k);
        // The phase at the camera, folded into a turn (doubles: exact far out and late on).
        const double phase = std::fmod(k * (dx * cx + dz * cz) - omega * t, k_two_pi);
        gpu::WaterWave& w = out[i];
        w.dir_x = static_cast<float>(dx);
        w.dir_z = static_cast<float>(dz);
        w.k = static_cast<float>(k);
        w.steepness = static_cast<float>(k_steep[i] * rough);
        w.phase = static_cast<float>(phase < 0.0 ? phase + k_two_pi : phase);
    }
    return out;
}

} // namespace giant::render
