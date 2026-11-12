#include "giant/world/water.h"

#include "giant/world/terrain.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace giant::world {

namespace {

constexpr double k_max_coordinate = 1e7; // metres from the origin

bool finite_point(double x, double z) {
    return std::isfinite(x) && std::isfinite(z) && std::fabs(x) <= k_max_coordinate &&
           std::fabs(z) <= k_max_coordinate;
}

bool in_unit(const Vec3& v, float maximum) {
    for (const float c : {v.x, v.y, v.z}) {
        if (!std::isfinite(c) || c < 0.0f || c > maximum) return false;
    }
    return true;
}

bool level_ok(float level) { return std::isfinite(level) && std::fabs(level) <= k_max_terrain_height; }

// Even-odd: inside the shore.
bool inside(std::span<const WaterPoint> shore, double x, double z) {
    bool in = false;
    for (usize i = 0, j = shore.size() - 1; i < shore.size(); j = i++) {
        const WaterPoint& a = shore[i];
        const WaterPoint& b = shore[j];
        if ((a.z > z) != (b.z > z)) {
            const double cross = a.x + (z - a.z) / (b.z - a.z) * (b.x - a.x);
            if (x < cross) in = !in;
        }
    }
    return in;
}

} // namespace

std::string_view water_kind_name(WaterKind kind) {
    switch (kind) {
    case WaterKind::lake: return "lake";
    case WaterKind::river: return "river";
    case WaterKind::sea: return "sea";
    }
    return "lake";
}

std::optional<WaterKind> water_kind_from_name(std::string_view name) {
    if (name == "lake") return WaterKind::lake;
    if (name == "river") return WaterKind::river;
    if (name == "sea") return WaterKind::sea;
    return std::nullopt;
}

std::optional<std::string> check_water(const Water& w) {
    if (w.name.empty() || w.name.size() > 256) return "name must be 1 to 256 bytes";
    if (!level_ok(w.level)) return "level must be a finite height within the terrain's range";
    if (!std::isfinite(w.flow) || w.flow < 0.0f || w.flow > 20.0f) return "flow must be from 0 to 20 metres a second";
    if (!in_unit(w.color, 1.0f)) return "color values must be from 0 to 1";
    if (!in_unit(w.absorption, 10.0f)) return "absorption values must be from 0 to 10 a metre";
    if (!std::isfinite(w.turbidity) || w.turbidity < 0.0f || w.turbidity > 1.0f) return "turbidity must be from 0 to 1";
    if (!std::isfinite(w.waves) || w.waves < 0.0f || w.waves > 4.0f) return "waves must be from 0 to 4";
    if (w.outline.size() > k_max_water_points || w.path.size() > k_max_water_points) {
        return "at most " + std::to_string(k_max_water_points) + " points";
    }
    for (const WaterPoint& p : w.outline) {
        if (!finite_point(p.x, p.z)) return "outline points must be finite and within 10,000 km";
    }
    for (const RiverPoint& p : w.path) {
        if (!finite_point(p.x, p.z)) return "path points must be finite and within 10,000 km";
        if (!std::isfinite(p.width) || p.width < 0.5f || p.width > 2000.0f) return "path widths must be from 0.5 to 2000 metres";
        if (!level_ok(p.level)) return "path levels must be finite heights within the terrain's range";
    }
    switch (w.kind) {
    case WaterKind::lake:
        if (w.outline.size() < 3) return "a lake needs an outline of 3 or more points";
        if (!w.path.empty()) return "a lake has no path";
        break;
    case WaterKind::river:
        if (w.path.size() < 2) return "a river needs a path of 2 or more points";
        if (!w.outline.empty()) return "a river has no outline";
        for (usize i = 1; i < w.path.size(); ++i) {
            if (w.path[i].x == w.path[i - 1].x && w.path[i].z == w.path[i - 1].z) return "path points must differ from the one before";
        }
        break;
    case WaterKind::sea:
        if (!w.outline.empty() || !w.path.empty()) return "the sea has no outline or path";
        break;
    }
    return std::nullopt;
}

bool water_bounds(const Water& w, double& min_x, double& min_z, double& max_x, double& max_z) {
    if (w.kind == WaterKind::sea) return false;
    min_x = min_z = std::numeric_limits<double>::infinity();
    max_x = max_z = -std::numeric_limits<double>::infinity();
    for (const WaterPoint& p : w.outline) {
        min_x = std::min(min_x, p.x);
        max_x = std::max(max_x, p.x);
        min_z = std::min(min_z, p.z);
        max_z = std::max(max_z, p.z);
    }
    for (const RiverPoint& p : w.path) {
        const double r = p.width * 0.5;
        min_x = std::min(min_x, p.x - r);
        max_x = std::max(max_x, p.x + r);
        min_z = std::min(min_z, p.z - r);
        max_z = std::max(max_z, p.z + r);
    }
    return min_x <= max_x;
}

std::optional<WaterHere> water_at(std::span<const Water> waters, double x, double z) {
    std::optional<WaterHere> best;
    const auto offer = [&](const WaterHere& h) {
        if (!best || h.level > best->level) best = h;
    };
    for (const Water& w : waters) {
        if (!w.visible) continue;
        if (w.kind == WaterKind::sea) {
            offer({&w, w.level});
            continue;
        }
        double x0, z0, x1, z1;
        if (!water_bounds(w, x0, z0, x1, z1) || x < x0 || x > x1 || z < z0 || z > z1) continue;
        if (w.kind == WaterKind::lake) {
            if (w.outline.size() >= 3 && inside(w.outline, x, z)) offer({&w, w.level});
            continue;
        }
        // A river: the nearest stretch whose half width reaches the point
        // (measured against that half width, so a wide stretch beside a
        // narrow one is found where it reaches).
        double nearest = std::numeric_limits<double>::infinity();
        WaterHere here;
        for (usize i = 1; i < w.path.size(); ++i) {
            const RiverPoint& a = w.path[i - 1];
            const RiverPoint& b = w.path[i];
            const double dx = b.x - a.x, dz = b.z - a.z;
            const double len2 = dx * dx + dz * dz;
            if (!(len2 > 0.0)) continue;
            const double t = std::clamp(((x - a.x) * dx + (z - a.z) * dz) / len2, 0.0, 1.0);
            const double px = a.x + dx * t - x, pz = a.z + dz * t - z;
            const double half = 0.5 * (a.width + (b.width - a.width) * t);
            if (!(half > 0.0)) continue; // (unchecked data: no width, no water)
            const double d = std::sqrt(px * px + pz * pz) / half;
            if (!(d <= 1.0) || d >= nearest) continue;
            nearest = d;
            const double len = std::sqrt(len2);
            here.water = &w;
            here.level = static_cast<float>(a.level + (b.level - a.level) * t);
            here.flow_x = static_cast<float>(dx / len * w.flow);
            here.flow_z = static_cast<float>(dz / len * w.flow);
            here.width = static_cast<float>(2.0 * half);
        }
        if (here.water != nullptr) offer(here);
    }
    return best;
}

} // namespace giant::world
