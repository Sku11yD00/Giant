#include "occluders.h"

#include <algorithm>
#include <cmath>

namespace giant::render {

namespace {

bool finite(const DVec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

u32 pack_albedo(const Vec3& c) {
    const auto byte = [](float v) {
        const float x = std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.5f;
        return static_cast<u32>(std::lround(x * 255.0f));
    };
    return byte(c.x) | (byte(c.y) << 8) | (byte(c.z) << 16);
}

} // namespace

void pick_occluders(std::span<const Occluder> occluders, const DVec3& camera, const Mat4& view,
                    const geometry::Frustum& frustum, std::vector<gpu::GpuOccluder>& out,
                    std::vector<std::pair<float, u32>>& scratch) {
    const auto to_view = [&view](const Vec3& p) {
        return Vec3{view.at(0, 0) * p.x + view.at(0, 1) * p.y + view.at(0, 2) * p.z,
                    view.at(1, 0) * p.x + view.at(1, 1) * p.y + view.at(1, 2) * p.z,
                    view.at(2, 0) * p.x + view.at(2, 1) * p.y + view.at(2, 2) * p.z};
    };
    out.clear();
    scratch.clear();
    for (usize i = 0; i < occluders.size(); ++i) {
        const Occluder& o = occluders[i];
        if (!finite(o.a) || !finite(o.b) || !std::isfinite(o.radius) || !(o.radius > 0.0f)) continue;
        const float radius = std::min(o.radius, 10.0f);
        const Vec3 a = to_camera_relative(o.a, camera);
        const Vec3 b = to_camera_relative(o.b, camera);
        const Vec3 mid = (a + b) * 0.5f;
        const float half = length(b - a) * 0.5f;
        if (!std::isfinite(half) || half > 100.0f) continue; // (a capsule this long is no body part)
        const float reach = half + radius * k_occluder_reach;
        // The nearest point of its reach to the camera.
        const float t = std::clamp(-dot(a, b - a) / std::max(dot(b - a, b - a), 1e-12f), 0.0f, 1.0f);
        const float distance = std::max(length(a + (b - a) * t) - radius * k_occluder_reach, 0.0f);
        if (!(distance <= k_occluder_distance)) continue;
        if (!geometry::sphere_in_frustum(frustum, mid, reach)) continue;
        scratch.emplace_back(distance, static_cast<u32>(i));
    }
    if (scratch.size() > k_max_occluders) {
        std::nth_element(scratch.begin(), scratch.begin() + k_max_occluders, scratch.end());
        scratch.resize(k_max_occluders);
    }
    std::sort(scratch.begin(), scratch.end());
    for (const auto& [distance, i] : scratch) {
        const Occluder& o = occluders[i];
        const Vec3 a = to_view(to_camera_relative(o.a, camera));
        const Vec3 b = to_view(to_camera_relative(o.b, camera));
        gpu::GpuOccluder g{};
        g.a[0] = a.x;
        g.a[1] = a.y;
        g.a[2] = a.z;
        g.radius = std::min(o.radius, 10.0f);
        g.b[0] = b.x;
        g.b[1] = b.y;
        g.b[2] = b.z;
        g.albedo = pack_albedo(o.albedo);
        out.push_back(g);
    }
}

} // namespace giant::render
