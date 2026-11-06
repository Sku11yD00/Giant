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
    // Smooth reads the heights as they were: a snapshot with a ring around.
    std::vector<float> before;
    if (brush.op == BrushOp::smooth) {
        const i64 bw = w + 2;
        before.assign(static_cast<usize>(bw * (j1 - j0 + 3)), std::numeric_limits<float>::quiet_NaN());
        for (i64 j = j0 - 1; j <= j1 + 1; ++j) {
            for (i64 i = i0 - 1; i <= i1 + 1; ++i) {
                if (const auto h = terrain.sample(i, j)) before[static_cast<usize>((j - j0 + 1) * bw + (i - i0 + 1))] = *h;
            }
        }
    }
    const auto old = [&](i64 i, i64 j) { return before[static_cast<usize>((j - j0 + 1) * (w + 2) + (i - i0 + 1))]; };
    for (i64 cz = floor_div(j0, n); cz <= floor_div(j1, n); ++cz) {
        for (i64 cx = floor_div(i0, n); cx <= floor_div(i1, n); ++cx) {
            if (cx < std::numeric_limits<i32>::min() || cx > std::numeric_limits<i32>::max() ||
                cz < std::numeric_limits<i32>::min() || cz > std::numeric_limits<i32>::max()) {
                continue;
            }
            const CellCoord cell{static_cast<i32>(cx), static_cast<i32>(cz)};
            if (terrain.find(cell) == nullptr) continue;
            Terrain::Heights* heights = nullptr; // modified only if a sample changes
            for (i64 j = std::max(j0, cz * n); j <= std::min(j1, cz * n + n - 1); ++j) {
                for (i64 i = std::max(i0, cx * n); i <= std::min(i1, cx * n + n - 1); ++i) {
                    const double d = std::hypot(static_cast<double>(i) * s - x, static_cast<double>(j) * s - z);
                    const float k = brush_weight(brush, d);
                    if (k <= 0.0f) continue;
                    if (heights == nullptr) {
                        heights = terrain.modify(cell);
                        changed.push_back(cell);
                    }
                    float& h = (*heights)[static_cast<usize>((j - cz * n) * n + (i - cx * n))];
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
                    h = std::clamp(next, -k_max_terrain_height, k_max_terrain_height);
                }
            }
        }
    }
}

std::vector<GroundPoint> brush_dabs(const Brush& brush, std::span<const GroundPoint> points) {
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
        for (i64 p = 1; p <= count; ++p) {
            const double t = static_cast<double>(p) / static_cast<double>(count);
            out.push_back({a.x + (b.x - a.x) * t, a.z + (b.z - a.z) * t});
        }
        if (count == 0) out.push_back(b);
    }
    return out;
}

} // namespace giant::world
