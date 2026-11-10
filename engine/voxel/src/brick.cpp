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

// The area of the triangle's part inside the box [0, edge]^3 (clipped
// against its six planes, Sutherland-Hodgman).
float area_inside(const Vec3& a, const Vec3& b, const Vec3& c, float edge) {
    std::array<Vec3, 9> poly{a, b, c};
    std::array<Vec3, 9> next{};
    u32 count = 3;
    const auto comp = [](const Vec3& v, u32 axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; };
    for (u32 plane = 0; plane < 6 && count > 0; ++plane) {
        const u32 axis = plane / 2;
        const bool upper = plane % 2 == 1;
        // Signed distance inside the plane: >= 0 keeps.
        const auto inside = [&](const Vec3& v) { return upper ? edge - comp(v, axis) : comp(v, axis); };
        u32 kept = 0;
        for (u32 i = 0; i < count; ++i) {
            const Vec3& p = poly[i];
            const Vec3& q = poly[(i + 1) % count];
            const float dp = inside(p);
            const float dq = inside(q);
            if (dp >= 0.0f && kept < next.size()) {
                next[kept++] = p;
            }
            if ((dp >= 0.0f) != (dq >= 0.0f) && kept < next.size()) {
                next[kept++] = p + (q - p) * (dp / (dp - dq));
            }
        }
        poly = next;
        count = kept;
    }
    Vec3 twice{};
    for (u32 i = 1; i + 1 < count; ++i) {
        twice += cross(poly[i] - poly[0], poly[i + 1] - poly[0]);
    }
    return 0.5f * length(twice);
}

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

u32 pack_light(const Vec3& light, bool sun) {
    // Mantissa m and exponent e (1 to 15) stand for m / 512 * 2^(e - 9).
    const auto clean = [](float c) { return std::min(std::max(0.0f, c), k_max_light); }; // NaN: 0
    const float r = clean(light.x);
    const float g = clean(light.y);
    const float b = clean(light.z);
    const float peak = std::max(r, std::max(g, b));
    int e = 1;
    while (e < 15 && peak >= std::ldexp(1.0f, e - 9)) {
        ++e;
    }
    float scale = std::ldexp(512.0f, 9 - e);
    if (std::lround(peak * scale) > 511 && e < 15) {
        ++e; // rounds up into the next exponent
        scale *= 0.5f;
    }
    const auto mantissa = [scale](float c) { return static_cast<u32>(std::min<long>(std::lround(c * scale), 511)); };
    return mantissa(r) | mantissa(g) << 9 | mantissa(b) << 18 | static_cast<u32>(e) << 27 | (sun ? 1u << 31 : 0u);
}

Vec3 unpack_light(u32 light) {
    const u32 e = light >> 27 & 15u;
    if (e == 0) {
        return {};
    }
    const float scale = std::ldexp(1.0f / 512.0f, static_cast<int>(e) - 9);
    return {static_cast<float>(light & 511u) * scale, static_cast<float>(light >> 9 & 511u) * scale,
            static_cast<float>(light >> 18 & 511u) * scale};
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

void BrickBuilder::reset(float voxel_size, u32 seed) {
    for (u32 i = 0; i < m_touched; ++i) {
        m_sums[m_list[i]] = {};
    }
    m_voxel_size = voxel_size > 0.0f ? voxel_size : 1.0f;
    m_seed = seed;
    m_touched = 0;
}

float BrickBuilder::threshold(u32 index) const {
    // A hash of the seed and the voxel (lowbias32), to (0, 1].
    u32 h = m_seed * 0x9E3779B9u ^ index;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return static_cast<float>((h >> 8) + 1u) / 16777216.0f;
}

void BrickBuilder::add(u32 index, const Vec3& color, const Vec3& normal, float coverage, float cover,
                       bool stand_in) {
    Sum& sum = m_sums[index];
    if (!sum.touched) {
        sum.touched = true;
        m_list[m_touched++] = static_cast<u16>(index);
    }
    Part& part = stand_in ? sum.stand : sum.own;
    part.color += color * coverage;
    part.normal += normal * coverage;
    part.weight += coverage;
    if (coverage >= 1.0f) {
        part.solid = true;
    } else {
        part.cover += cover;
    }
}

u32 BrickBuilder::add_triangle(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& color, bool stand_in,
                               float coverage) {
    if (!(coverage > 0.0f)) {
        return 0; // also NaN: cut away entirely
    }
    coverage = std::min(coverage, 1.0f);
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
    // Cut-outs share their area among the voxels they touch, so they are
    // gathered first (a solid triangle fills each at once).
    std::array<u16, k_brick_voxels> hit;
    u32 hits = 0;
    u32 tested = 0;
    for (u32 z = z0; z <= z1; ++z) {
        for (u32 y = y0; y <= y1; ++y) {
            for (u32 x = x0; x <= x1; ++x) {
                ++tested;
                const Vec3 center{(static_cast<float>(x) + 0.5f) * s, (static_cast<float>(y) + 0.5f) * s,
                                  (static_cast<float>(z) + 0.5f) * s};
                if (triangle_overlaps_box(center, half, a, b, c)) {
                    hit[hits++] = static_cast<u16>(voxel_index(x, y, z));
                }
            }
        }
    }
    if (hits == 0) {
        return tested;
    }
    // Its area inside the brick, shared among the voxels it touches here,
    // and never more than a plane can cut through one (sqrt 2 faces).
    float cover = 0.0f;
    if (coverage < 1.0f) {
        const float edge = s * static_cast<float>(k_brick_edge);
        const float share = std::min(area_inside(a, b, c, edge) / static_cast<float>(hits), 1.41421356f * s * s);
        cover = coverage * share / (s * s);
    }
    for (u32 k = 0; k < hits; ++k) {
        add(hit[k], color, normal, coverage, cover, stand_in);
    }
    return tested;
}

bool BrickBuilder::fills(const Part& part, u32 index) const {
    return part.weight > 0.0f && (part.solid || part.cover >= threshold(index));
}

Brick BrickBuilder::build() const {
    Brick brick;
    for (u32 k = 0; k < m_touched; ++k) {
        const u32 i = m_list[k];
        const Sum& sum = m_sums[i];
        // Opposite faces in one voxel (a thin wall) cancel: pack_voxel
        // then falls back to +Y.
        if (fills(sum.stand, i)) {
            brick.set(i, pack_voxel(sum.stand.color / sum.stand.weight, sum.stand.normal), true);
        } else if (fills(sum.own, i)) {
            brick.set(i, pack_voxel(sum.own.color / sum.own.weight, sum.own.normal));
        }
    }
    return brick;
}

} // namespace giant::voxel
