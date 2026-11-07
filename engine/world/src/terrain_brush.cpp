#include "giant/world/terrain_brush.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace giant::world {

float brush_weight(const Brush& brush, double distance) {
    if (!(brush.radius > 0.0) || distance >= brush.radius) return 0.0f;
    const double falloff = std::clamp(static_cast<double>(brush.falloff), 0.0, 1.0);
    const double inner = brush.radius * (1.0 - falloff);
    if (distance <= inner) return 1.0f;
    const double t = (brush.radius - distance) / (brush.radius - inner); // 1 at inner, 0 at the rim
    return static_cast<float>(t * t * (3.0 - 2.0 * t));
}

namespace {

bool in_i32(i64 v) { return v >= std::numeric_limits<i32>::min() && v <= std::numeric_limits<i32>::max(); }

i64 floor_div(i64 a, i64 b) {
    const i64 q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

} // namespace

void apply_brush(Terrain& terrain, const Brush& brush, double x, double z, std::vector<CellCoord>& changed) {
    if (terrain.empty() || !(brush.radius > 0.0) || !std::isfinite(x) || !std::isfinite(z)) return;
    const double s = terrain.spacing();
    const i64 n = terrain.samples();
    // Whole-world sample numbers the brush reaches.
    const auto lo = [&](double v) { return static_cast<i64>(std::ceil((v - brush.radius) / s)); };
    const auto hi = [&](double v) { return static_cast<i64>(std::floor((v + brush.radius) / s)); };
    const i64 i0 = lo(x), i1 = hi(x), j0 = lo(z), j1 = hi(z);
    if (i1 < i0 || j1 < j0) return;
    const i64 w = i1 - i0 + 1;
    const i64 bw = w + 2;
    // Smooth reads the heights as they were: a snapshot with a ring around,
    // copied from the tiles there are (no ground: NaN).
    std::vector<float> before;
    if (brush.op == BrushOp::smooth) {
        before.assign(static_cast<usize>(bw * (j1 - j0 + 3)), std::numeric_limits<float>::quiet_NaN());
        for (i64 cz = floor_div(j0 - 1, n); cz <= floor_div(j1 + 1, n); ++cz) {
            for (i64 cx = floor_div(i0 - 1, n); cx <= floor_div(i1 + 1, n); ++cx) {
                if (!in_i32(cx) || !in_i32(cz)) continue;
                const Terrain::Heights* tile = terrain.find({static_cast<i32>(cx), static_cast<i32>(cz)});
                if (tile == nullptr) continue;
                for (i64 j = std::max(j0 - 1, cz * n); j <= std::min(j1 + 1, cz * n + n - 1); ++j) {
                    for (i64 i = std::max(i0 - 1, cx * n); i <= std::min(i1 + 1, cx * n + n - 1); ++i) {
                        before[static_cast<usize>((j - j0 + 1) * bw + (i - i0 + 1))] =
                            (*tile)[static_cast<usize>((j - cz * n) * n + (i - cx * n))];
                    }
                }
            }
        }
    }
    const auto old = [&](i64 i, i64 j) { return before[static_cast<usize>((j - j0 + 1) * bw + (i - i0 + 1))]; };
    for (i64 cz = floor_div(j0, n); cz <= floor_div(j1, n); ++cz) {
        for (i64 cx = floor_div(i0, n); cx <= floor_div(i1, n); ++cx) {
            if (!in_i32(cx) || !in_i32(cz)) continue;
            const CellCoord cell{static_cast<i32>(cx), static_cast<i32>(cz)};
            const Terrain::Heights* source = terrain.find(cell);
            if (source == nullptr) continue;
            Terrain::Heights* heights = nullptr; // modified only once a sample changes
            for (i64 j = std::max(j0, cz * n); j <= std::min(j1, cz * n + n - 1); ++j) {
                for (i64 i = std::max(i0, cx * n); i <= std::min(i1, cx * n + n - 1); ++i) {
                    const double d = std::hypot(static_cast<double>(i) * s - x, static_cast<double>(j) * s - z);
                    const float k = brush_weight(brush, d);
                    if (k <= 0.0f) continue;
                    const usize at = static_cast<usize>((j - cz * n) * n + (i - cx * n));
                    const float h = heights != nullptr ? (*heights)[at] : (*source)[at];
                    float next = h;
                    switch (brush.op) {
                    case BrushOp::raise: next = h + brush.strength * k; break;
                    case BrushOp::lower: next = h - brush.strength * k; break;
                    case BrushOp::flatten:
                        next = h + (brush.height - h) * std::clamp(brush.strength * k, 0.0f, 1.0f);
                        break;
                    case BrushOp::smooth: {
                        float sum = 0.0f;
                        float count = 0.0f;
                        for (i64 dj = -1; dj <= 1; ++dj) {
                            for (i64 di = -1; di <= 1; ++di) {
                                const float v = old(i + di, j + dj);
                                if (!std::isnan(v)) {
                                    sum += v;
                                    count += 1.0f;
                                }
                            }
                        }
                        const float average = count > 0.0f ? sum / count : h;
                        next = h + (average - h) * std::clamp(brush.strength * k, 0.0f, 1.0f);
                        break;
                    }
                    }
                    next = std::clamp(next, -k_max_terrain_height, k_max_terrain_height);
                    if (next == h) continue; // a tile nothing changes in is left as it is
                    if (heights == nullptr) {
                        heights = terrain.modify(cell);
                        changed.push_back(cell);
                    }
                    (*heights)[at] = next;
                }
            }
        }
    }
}

double dent_reach(const Dent& dent) {
    if (!(dent.radius > 0.0) || !std::isfinite(dent.radius)) return 0.0;
    const double stretch = std::isfinite(dent.stretch) ? std::max(1.0, static_cast<double>(dent.stretch)) : 1.0;
    const double reach = 1.5 * dent.radius * stretch;
    return reach <= k_max_dent_reach ? reach : 0.0; // (past it: not a dent, and the sample numbers would overflow)
}

float dent_offset(const Dent& dent, double du, double dv) {
    if (!(dent.radius > 0.0) || !std::isfinite(dent.radius) || !std::isfinite(dent.depth)) return 0.0f;
    const double stretch = std::isfinite(dent.stretch) ? std::max(1.0, static_cast<double>(dent.stretch)) : 1.0;
    const double a = du / (dent.radius * stretch);
    const double b = dv / dent.radius;
    const double e = std::sqrt(a * a + b * b);
    if (e < 1.0) {
        const double q = 1.0 - e * e;
        return static_cast<float>(-dent.depth * q * q); // a smooth bowl, flat at its edge
    }
    if (e < 1.5) {
        const double t = (e - 1.25) / 0.25; // -1 to 1 across the rim
        const double rim = std::isfinite(dent.rim) ? std::clamp(static_cast<double>(dent.rim), 0.0, 1.0) : 0.0;
        const double bump = 1.0 - t * t;
        return static_cast<float>(dent.depth * rim * bump * bump);
    }
    return 0.0f;
}

void apply_dent(Terrain& terrain, const Dent& dent, double x, double z, std::vector<CellCoord>& changed,
                const std::function<void(CellCoord)>& before) {
    const double reach = dent_reach(dent);
    if (terrain.empty() || !(reach > 0.0) || !std::isfinite(x) || !std::isfinite(z) || std::fabs(x) > 1e12 ||
        std::fabs(z) > 1e12 || !std::isfinite(dent.depth) || !std::isfinite(dent.yaw)) {
        return;
    }
    const double s = terrain.spacing();
    const i64 n = terrain.samples();
    const double cu = std::cos(static_cast<double>(dent.yaw)), su = std::sin(static_cast<double>(dent.yaw));
    // The turned ellipse's bounds (rim included).
    const double stretch = reach / (1.5 * dent.radius);
    const double ex = 1.5 * dent.radius * std::sqrt(stretch * stretch * cu * cu + su * su);
    const double ez = 1.5 * dent.radius * std::sqrt(stretch * stretch * su * su + cu * cu);
    const i64 i0 = static_cast<i64>(std::ceil((x - ex) / s)), i1 = static_cast<i64>(std::floor((x + ex) / s));
    const i64 j0 = static_cast<i64>(std::ceil((z - ez) / s)), j1 = static_cast<i64>(std::floor((z + ez) / s));
    if (i1 < i0 || j1 < j0) return;
    for (i64 cz = floor_div(j0, n); cz <= floor_div(j1, n); ++cz) {
        for (i64 cx = floor_div(i0, n); cx <= floor_div(i1, n); ++cx) {
            if (!in_i32(cx) || !in_i32(cz)) continue;
            const CellCoord cell{static_cast<i32>(cx), static_cast<i32>(cz)};
            const Terrain::Heights* source = terrain.find(cell);
            if (source == nullptr) continue;
            Terrain::Heights* heights = nullptr; // modified only once a sample changes
            for (i64 j = std::max(j0, cz * n); j <= std::min(j1, cz * n + n - 1); ++j) {
                for (i64 i = std::max(i0, cx * n); i <= std::min(i1, cx * n + n - 1); ++i) {
                    const double dx = static_cast<double>(i) * s - x;
                    const double dz = static_cast<double>(j) * s - z;
                    const float off = dent_offset(dent, dx * cu + dz * su, dz * cu - dx * su);
                    if (off == 0.0f) continue;
                    const usize at = static_cast<usize>((j - cz * n) * n + (i - cx * n));
                    const float h = heights != nullptr ? (*heights)[at] : (*source)[at];
                    const float next = std::clamp(h + off, -k_max_terrain_height, k_max_terrain_height);
                    if (next == h) continue;
                    if (heights == nullptr) {
                        if (before) before(cell);
                        heights = terrain.modify(cell);
                        changed.push_back(cell);
                    }
                    (*heights)[at] = next;
                }
            }
        }
    }
}

std::vector<GroundPoint> brush_dabs(const Brush& brush, std::span<const GroundPoint> points, usize max_dabs) {
    std::vector<GroundPoint> out;
    if (points.empty()) return out;
    const double step = std::max(brush.radius * 0.25, 1e-3);
    out.push_back(points[0]);
    for (usize k = 1; k < points.size(); ++k) {
        const GroundPoint a = points[k - 1];
        const GroundPoint b = points[k];
        const double d = std::hypot(b.x - a.x, b.z - a.z);
        const double parts = std::min(std::ceil(d / step), 65536.0);
        const i64 count = static_cast<i64>(parts);
        if (out.size() + static_cast<usize>(std::max<i64>(count, 1)) > max_dabs) {
            out.resize(max_dabs + 1, b); // over the limit: the caller refuses it
            return out;
        }
        for (i64 p = 1; p <= count; ++p) {
            const double t = static_cast<double>(p) / static_cast<double>(count);
            out.push_back({a.x + (b.x - a.x) * t, a.z + (b.z - a.z) * t});
        }
        if (count == 0) out.push_back(b);
    }
    return out;
}

} // namespace giant::world
