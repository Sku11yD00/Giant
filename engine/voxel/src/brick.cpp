#include "giant/voxel/brick.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace giant::voxel {

namespace {

u32 to_bits(float v, float steps) {
    return static_cast<u32>(std::lround(std::clamp(v, 0.0f, 1.0f) * steps));
}

float sign_not_zero(float v) { return v < 0.0f ? -1.0f : 1.0f; }

} // namespace

u32 pack_voxel(const Vec3& color, const Vec3& normal) {
    // std::max returns its first argument when a comparison fails, so a
    // NaN colour comes out as 0.
    const auto root = [](float c) { return std::sqrt(std::max(0.0f, c)); };
    const u32 rgb = to_bits(root(color.x), 31.0f) << 11 | to_bits(root(color.y), 63.0f) << 5 |
                    to_bits(root(color.z), 31.0f);
    Vec3 n = normalize_or_zero(normal);
    if (!(n.x == n.x && n.y == n.y && n.z == n.z) || n == Vec3{}) {
        n = {0.0f, 1.0f, 0.0f};
    }
    const float l1 = std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z);
    float u = n.x / l1;
    float v = n.y / l1;
    if (n.z < 0.0f) {
        const float fu = (1.0f - std::fabs(v)) * sign_not_zero(u);
        const float fv = (1.0f - std::fabs(u)) * sign_not_zero(v);
        u = fu;
        v = fv;
    }
    const u32 nu = to_bits(u * 0.5f + 0.5f, 255.0f);
    const u32 nv = to_bits(v * 0.5f + 0.5f, 255.0f);
    return rgb | nu << 16 | nv << 24;
}

Vec3 unpack_color(u32 voxel) {
    const float r = static_cast<float>(voxel >> 11 & 31u) / 31.0f;
    const float g = static_cast<float>(voxel >> 5 & 63u) / 63.0f;
    const float b = static_cast<float>(voxel & 31u) / 31.0f;
    return {r * r, g * g, b * b};
}

Vec3 unpack_normal(u32 voxel) {
    const float u = static_cast<float>(voxel >> 16 & 255u) / 255.0f * 2.0f - 1.0f;
    const float v = static_cast<float>(voxel >> 24 & 255u) / 255.0f * 2.0f - 1.0f;
    Vec3 n{u, v, 1.0f - std::fabs(u) - std::fabs(v)};
    if (n.z < 0.0f) {
        const float x = (1.0f - std::fabs(v)) * sign_not_zero(u);
        const float y = (1.0f - std::fabs(u)) * sign_not_zero(v);
        n.x = x;
        n.y = y;
    }
    const Vec3 unit = normalize_or_zero(n);
    return unit == Vec3{} ? Vec3{0.0f, 1.0f, 0.0f} : unit;
}

u32 Brick::count() const {
    u32 n = 0;
    for (const u32 word : occupancy) {
        n += static_cast<u32>(std::popcount(word));
    }
    return n;
}

bool triangle_overlaps_box(const Vec3& box_center, const Vec3& half_size, const Vec3& a, const Vec3& b,
                           const Vec3& c) {
    const Vec3 v0 = a - box_center;
    const Vec3 v1 = b - box_center;
    const Vec3 v2 = c - box_center;
    const Vec3 e0 = v1 - v0;
    const Vec3 e1 = v2 - v1;
    const Vec3 e2 = v0 - v2;
    const float h[3] = {half_size.x, half_size.y, half_size.z};
    const auto comp = [](const Vec3& v, int i) { return i == 0 ? v.x : i == 1 ? v.y : v.z; };

    // The box's three axes.
    for (int i = 0; i < 3; ++i) {
        const float p0 = comp(v0, i);
        const float p1 = comp(v1, i);
        const float p2 = comp(v2, i);
        if (std::min({p0, p1, p2}) > h[i] || std::max({p0, p1, p2}) < -h[i]) {
            return false;
        }
    }
    // The triangle's plane.
    const Vec3 n = cross(e0, e1);
    const float r_plane = h[0] * std::fabs(n.x) + h[1] * std::fabs(n.y) + h[2] * std::fabs(n.z);
    if (std::fabs(dot(n, v0)) > r_plane) {
        return false;
    }
    // The nine cross products of box axes and triangle edges.
    const Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const Vec3 edges[3] = {e0, e1, e2};
    for (const Vec3& u : axes) {
        for (const Vec3& e : edges) {
            const Vec3 axis = cross(u, e);
            const float p0 = dot(v0, axis);
            const float p1 = dot(v1, axis);
            const float p2 = dot(v2, axis);
            const float r = h[0] * std::fabs(axis.x) + h[1] * std::fabs(axis.y) + h[2] * std::fabs(axis.z);
            if (std::min({p0, p1, p2}) > r || std::max({p0, p1, p2}) < -r) {
                return false;
            }
        }
    }
    return true;
}

void BrickBuilder::reset(float voxel_size) {
    for (u32 i = 0; i < m_touched; ++i) {
        m_sums[m_list[i]] = {};
    }
    m_voxel_size = voxel_size > 0.0f ? voxel_size : 1.0f;
    m_touched = 0;
}

u32 BrickBuilder::add_triangle(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& color) {
    const Vec3 face = cross(b - a, c - a);
    const Vec3 normal = normalize_or_zero(face);
    if (normal == Vec3{}) {
        return 0; // degenerate: no area, no facing
    }
    const float s = m_voxel_size;
    const auto range = [s](float lo, float hi, u32& first, u32& last) {
        // Voxels whose cell [i, i + 1) * s meets [lo, hi]; empty if outside.
        const float f = std::floor(lo / s);
        const float l = std::floor(hi / s);
        if (!(l >= 0.0f) || !(f < static_cast<float>(k_brick_edge))) {
            return false; // also catches NaN
        }
        first = static_cast<u32>(std::max(f, 0.0f));
        last = static_cast<u32>(std::min(l, static_cast<float>(k_brick_edge - 1)));
        return true;
    };
    u32 x0 = 0, x1 = 0, y0 = 0, y1 = 0, z0 = 0, z1 = 0;
    if (!range(std::min({a.x, b.x, c.x}), std::max({a.x, b.x, c.x}), x0, x1) ||
        !range(std::min({a.y, b.y, c.y}), std::max({a.y, b.y, c.y}), y0, y1) ||
        !range(std::min({a.z, b.z, c.z}), std::max({a.z, b.z, c.z}), z0, z1)) {
        return 0;
    }
    const Vec3 half{s * 0.5f, s * 0.5f, s * 0.5f};
    u32 tested = 0;
    for (u32 z = z0; z <= z1; ++z) {
        for (u32 y = y0; y <= y1; ++y) {
            for (u32 x = x0; x <= x1; ++x) {
                ++tested;
                const Vec3 center{(static_cast<float>(x) + 0.5f) * s, (static_cast<float>(y) + 0.5f) * s,
                                  (static_cast<float>(z) + 0.5f) * s};
                if (!triangle_overlaps_box(center, half, a, b, c)) {
                    continue;
                }
                Sum& sum = m_sums[voxel_index(x, y, z)];
                if (sum.weight == 0.0f) {
                    m_list[m_touched++] = static_cast<u16>(voxel_index(x, y, z));
                }
                sum.color += color;
                sum.normal += normal;
                sum.weight += 1.0f;
            }
        }
    }
    return tested;
}

Brick BrickBuilder::build() const {
    Brick brick;
    for (u32 k = 0; k < m_touched; ++k) {
        const u32 i = m_list[k];
        const Sum& sum = m_sums[i];
        // Opposite faces in one voxel (a thin wall) cancel: pack_voxel
        // then falls back to +Y.
        brick.set(i, pack_voxel(sum.color / sum.weight, sum.normal));
    }
    return brick;
}

} // namespace giant::voxel
