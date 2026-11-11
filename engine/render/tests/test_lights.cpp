// Local light binning (src/lights.h): which lights each cluster lists.
#include "lights.h"

#include "giant/render/renderer.h"
#include "giant_test.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

using namespace giant;
using namespace giant::render;

namespace {

constexpr u32 k_width = 320;
constexpr u32 k_height = 180;

struct Camera {
    View view;
    float aspect = static_cast<float>(k_width) / static_cast<float>(k_height);
    Mat4 view_proj;
    geometry::Frustum frustum;
    explicit Camera(const View& v = {}) : view(v) {
        view_proj = Renderer::view_projection(view, aspect);
        frustum = geometry::frustum_from_view_projection(view_proj);
    }
    void bin(std::span<const LocalLight> lights, LightBins& out) const {
        bin_lights(lights, view, aspect, frustum, out);
    }
    // The cluster a point shows in, as the shader finds it (false: off
    // screen or behind the camera).
    bool cluster_of(const DVec3& p, u32& cluster) const {
        const Vec3 rel = to_camera_relative(p, view.position);
        const Vec4 clip = view_proj * Vec4{rel.x, rel.y, rel.z, 1.0f};
        if (!(clip.w > view.near_plane)) {
            return false;
        }
        const float sx = (clip.x / clip.w * 0.5f + 0.5f) * static_cast<float>(k_width);
        const float sy = (0.5f - clip.y / clip.w * 0.5f) * static_cast<float>(k_height);
        if (sx < 0.0f || sy < 0.0f || sx >= static_cast<float>(k_width) || sy >= static_cast<float>(k_height)) {
            return false;
        }
        const float depth = -dot(rel, rotate(view.orientation, {0.0f, 0.0f, 1.0f}));
        const u32 slice = light_slice(depth);
        if (slice >= k_light_slices) {
            return false;
        }
        const u32 x = std::min(static_cast<u32>(sx * k_light_columns / k_width), k_light_columns - 1);
        const u32 y = std::min(static_cast<u32>(sy * k_light_rows / k_height), k_light_rows - 1);
        cluster = light_cluster(x, y, slice);
        return true;
    }
};

std::vector<u32> listed(const LightBins& b, u32 cluster) {
    const u32 first = b.clusters[usize{cluster} * 2];
    const u32 count = b.clusters[usize{cluster} * 2 + 1];
    return {b.entries.begin() + first, b.entries.begin() + first + count};
}

bool lists(const LightBins& b, u32 cluster, u32 light) {
    const std::vector<u32> l = listed(b, cluster);
    return std::find(l.begin(), l.end(), light) != l.end();
}

LocalLight point_at(const DVec3& p, float range) {
    LocalLight l;
    l.position = p;
    l.range = range;
    return l;
}

} // namespace

GIANT_TEST("lights: slices run in log depth from near to far") {
    GIANT_CHECK(light_slice(0.0f) == 0 && light_slice(k_light_near) == 0);
    GIANT_CHECK(light_slice(k_light_far * 0.999f) == k_light_slices - 1);
    GIANT_CHECK(light_slice(k_light_far * 1.001f) == k_light_slices);
    u32 last = 0;
    for (float d = 0.1f; d < k_light_far; d *= 1.1f) {
        const u32 s = light_slice(d);
        GIANT_CHECK(s >= last && s < k_light_slices);
        last = s;
    }
    GIANT_CHECK(last == k_light_slices - 1);
}

GIANT_TEST("lights: a light is listed where it shines, not behind the camera or too far") {
    const Camera cam;
    std::vector<LocalLight> lights = {point_at({0.0, 0.0, -10.0}, 2.0f), point_at({0.0, 0.0, 10.0}, 2.0f),
                                      point_at({0.0, 0.0, -6000.0}, 100.0f)};
    lights[2].intensity = 5.0f;
    LightBins bins;
    cam.bin(lights, bins);
    GIANT_CHECK(bins.lights.size() == 1 && !bins.overflow);
    u32 cluster = 0;
    GIANT_CHECK(cam.cluster_of({0.0, 0.0, -10.0}, cluster));
    GIANT_CHECK(lists(bins, cluster, 0));
    // Far from it on screen and in depth: not listed.
    GIANT_CHECK(cam.cluster_of({8.0, 4.0, -12.0}, cluster));
    GIANT_CHECK(!lists(bins, cluster, 0));
    GIANT_CHECK(cam.cluster_of({0.0, 0.0, -100.0}, cluster));
    GIANT_CHECK(!lists(bins, cluster, 0));
    u32 total = 0;
    for (usize i = 1; i < bins.clusters.size(); i += 2) total += bins.clusters[i];
    GIANT_CHECK(total == bins.entries.size());
    // Dark or broken lights are left out.
    lights = {point_at({0.0, 0.0, -10.0}, 2.0f), point_at({0.0, 0.0, -10.0}, 2.0f),
              point_at({0.0, std::nan(""), -10.0}, 2.0f)};
    lights[0].intensity = 0.0f;
    lights[1].color = {0.0f, 0.0f, 0.0f};
    cam.bin(lights, bins);
    GIANT_CHECK(bins.lights.empty());
    lights = {point_at({0.0, 0.0, -10.0}, 2.0f), point_at({0.0, 0.0, -10.0}, 2.0f)};
    lights[0].intensity = std::numeric_limits<float>::infinity();
    lights[1].intensity = 1e30f;
    lights[1].color = {1e30f, 1.0f, 1.0f};
    cam.bin(lights, bins);
    GIANT_CHECK(bins.lights.size() == 1);
    if (!bins.lights.empty()) GIANT_CHECK(std::isfinite(bins.lights[0].color[0]));
}

GIANT_TEST("lights: a light round the camera covers the whole screen near by") {
    const Camera cam;
    const std::vector<LocalLight> lights = {point_at({0.0, 0.0, 0.0}, 3.0f)};
    LightBins bins;
    cam.bin(lights, bins);
    GIANT_CHECK(bins.lights.size() == 1);
    if (bins.lights.size() != 1) return;
    for (u32 y = 0; y < k_light_rows; ++y) {
        for (u32 x = 0; x < k_light_columns; ++x) {
            GIANT_CHECK(lists(bins, light_cluster(x, y, 0), 0));
        }
    }
    GIANT_CHECK(!lists(bins, light_cluster(0, 0, light_slice(10.0f)), 0));
}

GIANT_TEST("lights: every point a light reaches finds it in its cluster") {
    // The CPU reference: random lights (points and spots, from many views),
    // random points within their reach, the cluster each shows in.
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    for (u32 round = 0; round < 40; ++round) {
        View v;
        v.position = {u(rng) * 50.0, u(rng) * 10.0, u(rng) * 50.0};
        v.orientation = normalize(Quat{u(rng) * 0.3f, u(rng), u(rng) * 0.3f, 1.0f});
        v.vertical_fov_degrees = 40.0f + 40.0f * (u(rng) * 0.5f + 0.5f);
        const Camera cam(v);
        std::vector<LocalLight> lights(64);
        for (LocalLight& l : lights) {
            l.position = v.position + DVec3{u(rng) * 60.0, u(rng) * 20.0, u(rng) * 60.0};
            l.range = 0.5f + 20.0f * (u(rng) * 0.5f + 0.5f);
            if (u(rng) > 0.0f) {
                l.kind = LightKind::spot;
                l.direction = {u(rng), u(rng), u(rng)};
                l.outer_angle = 0.05f + 1.5f * (u(rng) * 0.5f + 0.5f);
                l.inner_angle = l.outer_angle * 0.5f;
            }
        }
        LightBins bins;
        cam.bin(lights, bins);
        GIANT_CHECK(!bins.overflow);
        // Which bin index each light got (by its GPU position).
        for (const LocalLight& l : lights) {
            const gpu::Light g = to_gpu(l, v.position);
            u32 index = ~0u;
            for (u32 i = 0; i < bins.lights.size(); ++i) {
                if (std::memcmp(&bins.lights[i], &g, sizeof(g)) == 0) index = i;
            }
            const Vec3 dir = l.kind == LightKind::spot ? normalize_or_zero(l.direction) : Vec3{};
            for (u32 k = 0; k < 64; ++k) {
                Vec3 off{u(rng), u(rng), u(rng)};
                if (length(off) > 1.0f || length(off) < 1e-3f) continue;
                off = off * (l.range * 0.999f);
                if (l.kind == LightKind::spot &&
                    dot(normalize_or_zero(off), dir) < std::cos(std::min(l.outer_angle, 1.553f)) + 1e-4f) {
                    continue; // outside the cone
                }
                u32 cluster = 0;
                if (!cam.cluster_of(l.position + DVec3{off.x, off.y, off.z}, cluster)) continue;
                GIANT_CHECK(index != ~0u);
                if (index != ~0u) GIANT_CHECK(lists(bins, cluster, index));
            }
        }
    }
}

GIANT_TEST("lights: the nearest are kept when a cluster or the frame is full") {
    const Camera cam;
    // More than a cluster holds, all at the same place on screen: the
    // nearest k_max_lights_per_cluster stay.
    std::vector<LocalLight> lights;
    for (u32 i = 0; i < k_max_lights_per_cluster + 40; ++i) {
        // Further along the view ray each time, overlapping in depth.
        lights.push_back(point_at({0.0, 0.0, -20.0 - 0.01 * (k_max_lights_per_cluster + 40 - i)}, 5.0f));
    }
    LightBins bins;
    cam.bin(lights, bins);
    GIANT_CHECK(bins.overflow);
    u32 cluster = 0;
    if (!cam.cluster_of({0.0, 0.0, -20.0}, cluster)) {
        GIANT_CHECK(false);
        return;
    }
    const std::vector<u32> l = listed(bins, cluster);
    GIANT_CHECK(l.size() == k_max_lights_per_cluster);
    // The list is nearest first: bins.lights[0] is the last one added.
    GIANT_CHECK(bins.lights[0].position[2] > -20.02f);
    for (const u32 i : l) GIANT_CHECK(i < k_max_lights_per_cluster);
    // More than the frame holds: the nearest k_max_lights stay.
    lights.clear();
    for (u32 i = 0; i < k_max_lights + 10; ++i) {
        lights.push_back(point_at({0.0, 0.0, -10.0 - 0.5 * i}, 0.1f));
    }
    cam.bin(lights, bins);
    GIANT_CHECK(bins.overflow && bins.lights.size() == k_max_lights);
    float furthest = 0.0f;
    for (const gpu::Light& g : bins.lights) furthest = std::min(furthest, g.position[2]);
    GIANT_CHECK(furthest > -10.0f - 0.5f * k_max_lights);
}

GIANT_TEST("lights: GPU records clamp and shape the cone") {
    LocalLight l;
    l.position = {10.0, 2.0, 0.0};
    l.color = {1.0f, 0.5f, 0.25f};
    l.intensity = 4.0f;
    l.range = 5000.0f;
    l.source_radius = 9000.0f;
    gpu::Light g = to_gpu(l, {10.0, 0.0, 0.0});
    GIANT_CHECK(g.position[0] == 0.0f && g.position[1] == 2.0f);
    GIANT_CHECK(g.range == 1000.0f && g.source_radius == 500.0f);
    GIANT_CHECK(g.color[0] == 4.0f && g.color[1] == 2.0f && g.color[2] == 1.0f);
    GIANT_CHECK(g.spot_scale == 0.0f && g.spot_offset == 1.0f); // a point: all round
    l.kind = LightKind::spot;
    l.direction = {0.0f, -2.0f, 0.0f};
    l.inner_angle = 0.3f;
    l.outer_angle = 0.5f;
    g = to_gpu(l, {});
    GIANT_CHECK(std::fabs(g.direction[1] + 1.0f) < 1e-6f);
    const auto cone = [&](float angle) {
        return std::clamp(std::cos(angle) * g.spot_scale + g.spot_offset, 0.0f, 1.0f);
    };
    GIANT_CHECK(std::fabs(cone(0.3f) - 1.0f) < 1e-4f && cone(0.1f) == 1.0f);
    GIANT_CHECK(std::fabs(cone(0.5f)) < 1e-4f && cone(0.7f) == 0.0f);
    GIANT_CHECK(cone(0.4f) > 0.2f && cone(0.4f) < 0.8f);
    // A very narrow cone still reaches full light at its middle.
    l.inner_angle = 0.005f;
    l.outer_angle = 0.01f;
    g = to_gpu(l, {});
    GIANT_CHECK(g.spot_scale + g.spot_offset >= 0.999f);
    // Inner past outer: a hard edge, never a division by zero.
    l.inner_angle = 0.9f;
    g = to_gpu(l, {});
    GIANT_CHECK(std::isfinite(g.spot_scale) && std::isfinite(g.spot_offset));
}

GIANT_TEST("lights: a spot's bounding sphere holds its cone") {
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    for (float angle : {0.05f, 0.3f, 0.78f, 0.8f, 1.2f, 1.55f}) {
        LocalLight l;
        l.kind = LightKind::spot;
        l.range = 7.0f;
        l.outer_angle = angle;
        l.direction = {0.3f, -1.0f, 0.2f};
        Vec3 c;
        float r;
        light_bounds(l, {}, c, r);
        GIANT_CHECK(r <= l.range * 1.0001f); // never more than the point light's
        const Vec3 d = normalize_or_zero(l.direction);
        for (u32 k = 0; k < 4000; ++k) {
            const Vec3 p = Vec3{u(rng), u(rng), u(rng)} * l.range;
            if (length(p) > l.range || length(p) < 1e-4f || dot(normalize_or_zero(p), d) < std::cos(angle)) continue;
            GIANT_CHECK(length(p - c) <= r * 1.0001f + 1e-4f);
        }
    }
}
