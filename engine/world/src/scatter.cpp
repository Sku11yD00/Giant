#include "giant/world/scatter.h"

#include <algorithm>
#include <cmath>

namespace giant::world {

namespace {

constexpr double k_pi = 3.14159265358979323846;

u64 mix(u64 x) { // splitmix64's finaliser
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// A hash of a grid point: the same for the same seed, point and use.
u64 point_hash(u32 seed, i64 gx, i64 gz, u64 use) {
    return mix(mix(mix(u64{seed} ^ (use << 32)) ^ static_cast<u64>(gx)) ^ static_cast<u64>(gz));
}

// [0, 1) from a hash's top 53 bits.
double unit(u64 h) { return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0); }

double smoothstep(double a, double b, double v) {
    const double t = std::clamp((v - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// Smooth value noise in [0, 1), `size` metres between its lattice points.
double clump_noise(u32 seed, double x, double z, double size) {
    const double fx = x / size, fz = z / size;
    const double x0 = std::floor(fx), z0 = std::floor(fz);
    const auto gx = static_cast<i64>(x0), gz = static_cast<i64>(z0);
    const double tx = smoothstep(0.0, 1.0, fx - x0), tz = smoothstep(0.0, 1.0, fz - z0);
    const auto at = [&](i64 i, i64 j) { return unit(point_hash(seed, gx + i, gz + j, 7)); };
    const double a = at(0, 0) + (at(1, 0) - at(0, 0)) * tx;
    const double b = at(0, 1) + (at(1, 1) - at(0, 1)) * tx;
    return a + (b - a) * tz;
}

// The grid squares (spacing a side) a cell overlaps, on one axis.
struct Span {
    i64 first = 0;
    i64 last = -1;
};
Span squares(i32 cell, double spacing) {
    const double lo = cell * k_cell_size;
    return {static_cast<i64>(std::floor(lo / spacing)), static_cast<i64>(std::floor((lo + k_cell_size) / spacing))};
}

} // namespace

std::optional<Vec3> ground_normal(const Terrain& terrain, double x, double z, double step) {
    const std::optional<double> centre = terrain.height_at(x, z);
    if (!centre) return std::nullopt;
    // A central difference; one-sided where the ground stops on one side.
    const auto slope = [&](std::optional<double> lo, std::optional<double> hi) {
        if (lo && hi) return (*hi - *lo) / (2.0 * step);
        if (hi) return (*hi - *centre) / step;
        if (lo) return (*centre - *lo) / step;
        return 0.0;
    };
    const double sx = slope(terrain.height_at(x - step, z), terrain.height_at(x + step, z));
    const double sz = slope(terrain.height_at(x, z - step), terrain.height_at(x, z + step));
    const double length = std::sqrt(sx * sx + 1.0 + sz * sz);
    return Vec3{static_cast<float>(-sx / length), static_cast<float>(1.0 / length), static_cast<float>(-sz / length)};
}

usize scatter_candidates(const ScatterLayer& layer, CellCoord cell) {
    if (!(layer.spacing >= k_min_scatter_spacing)) return 0;
    const Span x = squares(cell.x, layer.spacing), z = squares(cell.z, layer.spacing);
    return static_cast<usize>(x.last - x.first + 1) * static_cast<usize>(z.last - z.first + 1);
}

void scatter_cell(const ScatterLayer& layer, u32 layer_index, const Terrain& terrain, CellCoord cell,
                  std::vector<ScatterPoint>& out) {
    if (!layer.visible || layer.items.empty() || terrain.find(cell) == nullptr) return;
    if (!(layer.spacing >= k_min_scatter_spacing) || !(layer.density > 0.0f)) return;
    double total_weight = 0.0;
    for (const ScatterItem& item : layer.items) total_weight += std::max(0.0f, item.weight);
    if (!(total_weight > 0.0)) return;
    const double s = layer.spacing;
    const double jitter = std::clamp(static_cast<double>(layer.jitter), 0.0, 1.0);
    const double step = terrain.spacing();
    const double min_cos = std::cos(static_cast<double>(layer.slope_max) * k_pi / 180.0) - 1e-9;
    const double max_cos = std::cos(static_cast<double>(layer.slope_min) * k_pi / 180.0) + 1e-9;
    const Span xs = squares(cell.x, s), zs = squares(cell.z, s);
    // One square more each side: a point on a square's edge can round into
    // the next cell (cell_of below keeps only this cell's).
    for (i64 gz = zs.first - 1; gz <= zs.last + 1; ++gz) {
        for (i64 gx = xs.first - 1; gx <= xs.last + 1; ++gx) {
            const u64 h0 = point_hash(layer.seed, gx, gz, 0);
            const u64 h1 = mix(h0);
            const u64 h2 = mix(h1);
            const u64 h3 = mix(h2);
            const u64 h4 = mix(h3);
            const u64 h5 = mix(h4);
            const double x = (static_cast<double>(gx) + 0.5 + (unit(h0) - 0.5) * jitter) * s;
            const double z = (static_cast<double>(gz) + 0.5 + (unit(h1) - 0.5) * jitter) * s;
            if (!(cell_of({x, 0.0, z}) == cell)) continue; // another cell's
            if (layer.area && (x < layer.area->from_x || x > layer.area->to_x || z < layer.area->from_z ||
                               z > layer.area->to_z)) {
                continue;
            }
            double keep = layer.density;
            if (layer.clump_size > 0.0f && layer.clumping > 0.0f) {
                const double n = smoothstep(0.35, 0.65, clump_noise(layer.seed, x, z, layer.clump_size));
                keep *= 1.0 - layer.clumping + layer.clumping * n;
            }
            if (unit(h2) >= keep) continue;
            const std::optional<double> ground = terrain.height_at(x, z);
            if (!ground || *ground < layer.height_min || *ground > layer.height_max) continue;
            const std::optional<Vec3> normal = ground_normal(terrain, x, z, step);
            if (!normal || normal->y < min_cos || normal->y > max_cos) continue;
            // The item, by weight.
            double pick = unit(h3) * total_weight;
            u32 index = 0;
            for (; index + 1 < layer.items.size(); ++index) {
                pick -= std::max(0.0f, layer.items[index].weight);
                if (pick < 0.0) break;
            }
            const ScatterItem& item = layer.items[index];
            const float scale =
                item.scale_min + (item.scale_max - item.scale_min) * static_cast<float>(unit(h4));
            const Quat yaw = quat_from_axis_angle({0.0f, 1.0f, 0.0f}, static_cast<float>(unit(h5) * 2.0 * k_pi));
            Quat tilt{};
            const Vec3 axis{normal->z, 0.0f, -normal->x}; // up x normal
            const float sine = std::sqrt(axis.x * axis.x + axis.z * axis.z);
            if (item.align > 0.0f && sine > 1e-6f) {
                const float angle = std::atan2(sine, normal->y) * item.align;
                tilt = quat_from_axis_angle({axis.x / sine, 0.0f, axis.z / sine}, angle);
            }
            ScatterPoint p;
            p.layer = layer_index;
            p.item = index;
            p.transform.position = {x, *ground - static_cast<double>(item.sink * scale), z};
            p.transform.rotation = normalize(tilt * yaw);
            p.transform.scale = {scale, scale, scale};
            out.push_back(p);
        }
    }
}

std::optional<std::string> check_scatter_layer(const ScatterLayer& l) {
    const auto range = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    if (l.name.empty() || l.name.size() > k_max_name_bytes) return "a layer's name is 1 to 128 bytes";
    if (l.items.empty() || l.items.size() > k_max_scatter_items) {
        return "a layer has 1 to " + std::to_string(k_max_scatter_items) + " items";
    }
    if (!range(l.spacing, k_min_scatter_spacing, k_max_scatter_spacing)) return "spacing is 0.25 to 1000 metres";
    if (!range(l.density, 0.0f, 1.0f) || !range(l.jitter, 0.0f, 1.0f) || !range(l.clumping, 0.0f, 1.0f)) {
        return "density, jitter and clumping are 0 to 1";
    }
    if (!(l.clump_size == 0.0f || range(l.clump_size, 1.0f, 100000.0f))) {
        return "clump_size is 0 (no clumps) or 1 to 100000 metres";
    }
    if (!range(l.height_min, -k_max_terrain_height, k_max_terrain_height) ||
        !range(l.height_max, -k_max_terrain_height, k_max_terrain_height) || l.height_min > l.height_max) {
        return "height_min and height_max are -10000 to 10000 metres, min first";
    }
    if (!range(l.slope_min, 0.0f, 90.0f) || !range(l.slope_max, 0.0f, 90.0f) || l.slope_min > l.slope_max) {
        return "slope_min and slope_max are 0 to 90 degrees, min first";
    }
    if (l.area) {
        const ScatterArea& a = *l.area;
        for (const double v : {a.from_x, a.from_z, a.to_x, a.to_z}) {
            if (!std::isfinite(v) || std::fabs(v) > 1e8) return "an area's corners are within 1e8 metres";
        }
        if (a.from_x > a.to_x || a.from_z > a.to_z) return "an area's `from` is its lowest x and z";
    }
    for (const ScatterItem& i : l.items) {
        if (i.shape == Shape::decal) return "items are cube, sphere, plane, mesh or kit";
        if (i.shape == Shape::mesh && (i.mesh.empty() || i.mesh.size() > k_max_mesh_path_bytes)) {
            return "a mesh item names its file (at most 1024 bytes)";
        }
        if (i.shape == Shape::kit && (i.kit.empty() || i.kit.size() > k_max_name_bytes)) {
            return "a kit item names its kit (at most 128 bytes)";
        }
        if ((i.shape != Shape::mesh && !i.mesh.empty()) || (i.shape != Shape::kit && !i.kit.empty())) {
            return "only a mesh item names a mesh, and only a kit item a kit";
        }
        if (!range(i.weight, 1e-6f, 1e6f)) return "an item's weight is above 0, at most 1000000";
        if (!range(i.scale_min, 0.0001f, 10000.0f) || !range(i.scale_max, 0.0001f, 10000.0f) ||
            i.scale_min > i.scale_max) {
            return "an item's scale_min and scale_max are 0.0001 to 10000, min first";
        }
        if (!range(i.align, 0.0f, 1.0f)) return "an item's align is 0 to 1";
        if (!range(i.sink, -1000.0f, 1000.0f)) return "an item's sink is -1000 to 1000 metres";
        if (i.tree) {
            const ScatterTree& t = *i.tree;
            if (!range(t.trunk_radius, 0.01f, 100.0f) || !range(t.height, 0.1f, 1000.0f) ||
                t.height < 2.0f * t.trunk_radius) {
                return "a tree's trunk_radius is 0.01 to 100 m, its height 0.1 to 1000 m and at least twice the radius";
            }
            if (!range(t.strength, 0.0f, 1e9f)) return "a tree's strength is 0 to 1000000000 N s";
        }
        const Material& m = i.material;
        const auto colour = [&](const Vec3& c, float hi) { return range(c.x, 0, hi) && range(c.y, 0, hi) && range(c.z, 0, hi); };
        if (!colour(m.base_color, 1.0f) || !range(m.roughness, 0, 1) || !range(m.metallic, 0, 1) ||
            !colour(m.emissive, 1000.0f)) {
            return "an item's material: base_color, roughness, metallic 0 to 1; emissive 0 to 1000";
        }
    }
    return std::nullopt;
}

} // namespace giant::world
