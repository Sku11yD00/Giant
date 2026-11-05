#include "giant/voxel/clipmap.h"

#include "giant/core/assert.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>

namespace giant::voxel {

namespace {

i32 floor_to_i32(double v) {
    // Far outside any world (or NaN): pinned, so the maths below never overflows.
    constexpr double limit = 1 << 29;
    if (!(v > -limit)) return -(1 << 29);
    if (!(v < limit)) return 1 << 29;
    return static_cast<i32>(std::floor(v));
}

i32 positive_mod(i32 v, i32 n) {
    const i32 m = v % n;
    return m < 0 ? m + n : m;
}

i32& axis(BrickCoord& c, int i) { return i == 0 ? c.x : i == 1 ? c.y : c.z; }
i32 axis(const BrickCoord& c, int i) { return i == 0 ? c.x : i == 1 ? c.y : c.z; }

} // namespace

u64 BrickBox::count() const {
    if (max.x <= min.x || max.y <= min.y || max.z <= min.z) {
        return 0;
    }
    return u64(u32(max.x - min.x)) * u64(u32(max.y - min.y)) * u64(u32(max.z - min.z));
}

BrickBox intersect(const BrickBox& a, const BrickBox& b) {
    BrickBox out;
    out.level = a.level;
    out.min = {std::max(a.min.x, b.min.x), std::max(a.min.y, b.min.y), std::max(a.min.z, b.min.z)};
    out.max = {std::min(a.max.x, b.max.x), std::min(a.max.y, b.max.y), std::min(a.max.z, b.max.z)};
    if (out.count() == 0) {
        out.max = out.min;
    }
    return out;
}

Clipmap::Clipmap(const ClipmapDesc& desc) : m_desc(desc) {
    GIANT_ASSERT(desc.levels >= 1 && desc.levels <= 16, "a clipmap has 1 to 16 levels");
    GIANT_ASSERT(desc.bricks_per_axis >= 4 && desc.bricks_per_axis <= 256 &&
                     std::has_single_bit(static_cast<u32>(desc.bricks_per_axis)),
                 "bricks per axis is a power of two from 4 to 256");
    GIANT_ASSERT(desc.voxel_size > 0.0f, "voxels need a size");
    m_desc.levels = std::clamp<u32>(desc.levels, 1, 16);
    m_desc.bricks_per_axis = std::clamp(static_cast<i32>(std::bit_floor(static_cast<u32>(
                                            std::max(desc.bricks_per_axis, 4)))),
                                        4, 256);
    m_desc.voxel_size = desc.voxel_size > 0.0f ? desc.voxel_size : 0.25f;
    m_origin.resize(m_desc.levels);
}

double Clipmap::voxel_size(u32 level) const {
    return static_cast<double>(m_desc.voxel_size) * std::ldexp(1.0, static_cast<int>(level));
}

BrickCoord Clipmap::brick_at(u32 level, const DVec3& p) const {
    const double s = brick_size(level);
    return {floor_to_i32(p.x / s), floor_to_i32(p.y / s), floor_to_i32(p.z / s)};
}

DVec3 Clipmap::brick_min(u32 level, const BrickCoord& b) const {
    const double s = brick_size(level);
    return {b.x * s, b.y * s, b.z * s};
}

bool Clipmap::contains(u32 level, const BrickCoord& b) const {
    const BrickCoord& o = m_origin[level];
    const i32 n = m_desc.bricks_per_axis;
    return m_placed && b.x >= o.x && b.x < o.x + n && b.y >= o.y && b.y < o.y + n && b.z >= o.z &&
           b.z < o.z + n;
}

u32 Clipmap::slot(u32 level, const BrickCoord& b) const {
    (void)level;
    const i32 n = m_desc.bricks_per_axis;
    return static_cast<u32>(positive_mod(b.x, n) + n * (positive_mod(b.y, n) + n * positive_mod(b.z, n)));
}

u32 Clipmap::slots_per_level() const {
    const u32 n = static_cast<u32>(m_desc.bricks_per_axis);
    return n * n * n;
}

BrickBox Clipmap::bricks_touching(u32 level, const DVec3& min, const DVec3& max) const {
    BrickBox box;
    box.level = level;
    if (!m_placed) {
        return box;
    }
    const BrickCoord lo = brick_at(level, min);
    const BrickCoord hi = brick_at(level, max);
    box.min = lo;
    box.max = {hi.x + 1, hi.y + 1, hi.z + 1};
    const i32 n = m_desc.bricks_per_axis;
    const BrickCoord& o = m_origin[level];
    BrickBox window{level, o, {o.x + n, o.y + n, o.z + n}};
    return intersect(box, window);
}

std::vector<BrickBox> Clipmap::recenter(const DVec3& camera) {
    std::vector<BrickBox> entered;
    const i32 n = m_desc.bricks_per_axis;
    const i32 half = n / 2;
    for (u32 level = 0; level < m_desc.levels; ++level) {
        const BrickCoord c = brick_at(level, camera);
        const BrickCoord target{c.x - half, c.y - half, c.z - half};
        BrickCoord& origin = m_origin[level];
        if (!m_placed) {
            origin = target;
            entered.push_back({level, origin, {origin.x + n, origin.y + n, origin.z + n}});
            continue;
        }
        const BrickCoord old = origin;
        BrickCoord next = old;
        for (int i = 0; i < 3; ++i) {
            if (std::abs(axis(target, i) - axis(old, i)) > 1) {
                axis(next, i) = axis(target, i);
            }
        }
        if (next == old) {
            continue;
        }
        origin = next;
        bool overlap = true;
        for (int i = 0; i < 3; ++i) {
            overlap = overlap && std::abs(axis(next, i) - axis(old, i)) < n;
        }
        if (!overlap) {
            entered.push_back({level, next, {next.x + n, next.y + n, next.z + n}});
            continue;
        }
        // The new window minus the old one, as up to six slabs: per axis,
        // the part outside the old window, then narrow that axis to the
        // overlap before the next.
        BrickBox rest{level, next, {next.x + n, next.y + n, next.z + n}};
        for (int i = 0; i < 3; ++i) {
            const i32 lo_old = axis(old, i);
            const i32 hi_old = lo_old + n;
            const i32 lo_new = axis(rest.min, i);
            const i32 hi_new = axis(rest.max, i);
            if (lo_new < lo_old) {
                BrickBox slab = rest;
                axis(slab.max, i) = lo_old;
                entered.push_back(slab);
            }
            if (hi_new > hi_old) {
                BrickBox slab = rest;
                axis(slab.min, i) = hi_old;
                entered.push_back(slab);
            }
            axis(rest.min, i) = std::max(lo_new, lo_old);
            axis(rest.max, i) = std::min(hi_new, hi_old);
        }
    }
    m_placed = true;
    return entered;
}

} // namespace giant::voxel
