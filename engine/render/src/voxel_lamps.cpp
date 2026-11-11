#include "voxel_lamps.h"

#include "lights.h"

#include <algorithm>
#include <cmath>

namespace giant::render {

namespace {

constexpr i64 k_most_cells = 64; // a lamp or brick spanning more looks at all

bool usable(const LocalLight& l) {
    return std::isfinite(l.position.x + l.position.y + l.position.z) && l.intensity > 0.0f &&
           std::isfinite(l.intensity) && std::isfinite(l.color.x + l.color.y + l.color.z) &&
           std::max({l.color.x, l.color.y, l.color.z}) > 0.0f;
}

i64 cell_of(double v) { return static_cast<i64>(std::floor(v / k_voxel_lamp_cell)); }

u64 key(i64 x, i64 y, i64 z) {
    const auto part = [](i64 v) { return static_cast<u64>(v) & 0x1fffffull; };
    return part(x) | (part(y) << 21) | (part(z) << 42);
}

// Squared distance from a point to a box.
double distance2(const DVec3& p, const DVec3& lo, const DVec3& hi) {
    const double dx = std::max({lo.x - p.x, 0.0, p.x - hi.x});
    const double dy = std::max({lo.y - p.y, 0.0, p.y - hi.y});
    const double dz = std::max({lo.z - p.z, 0.0, p.z - hi.z});
    return dx * dx + dy * dy + dz * dz;
}

} // namespace

void VoxelLamps::drop() {
    for (gpu::LightJob& j : m_jobs) j.lamp_count = 0;
    m_lights.clear();
    m_entries.clear();
}

void VoxelLamps::list(std::span<const LocalLight> lights, std::span<const gpu::LightJob> in,
                      const voxel::Clipmap& clipmap, const DVec3& camera) {
    m_jobs.assign(in.begin(), in.end());
    std::vector<gpu::LightJob>& jobs = m_jobs;
    m_lamps.clear();
    m_big.clear();
    m_lights.clear();
    m_entries.clear();
    usize used_cells = 0;
    for (auto& [k, cell] : m_grid) {
        used_cells += cell.empty() ? 0u : 1u;
        cell.clear();
    }
    if (m_grid.size() > 4096 && m_grid.size() > used_cells * 4) m_grid.clear(); // (lamps moved on)
    for (gpu::LightJob& j : jobs) {
        j.lamp_first = 0;
        j.lamp_count = 0;
    }
    if (jobs.empty()) return;
    for (u32 i = 0; i < lights.size(); ++i) {
        const LocalLight& l = lights[i];
        if (!usable(l)) continue;
        Vec3 c;
        float r;
        light_bounds(l, camera, c, r);
        if (!std::isfinite(c.x + c.y + c.z + r)) continue;
        Lamp lamp;
        lamp.source = i;
        lamp.center = camera + DVec3{c.x, c.y, c.z};
        lamp.radius = static_cast<double>(r) * (1.0 + 1e-3) + 1e-3;
        lamp.power = std::max({l.color.x, l.color.y, l.color.z}) * l.intensity;
        const u32 at = static_cast<u32>(m_lamps.size());
        m_lamps.push_back(lamp);
        const i64 x0 = cell_of(lamp.center.x - lamp.radius), x1 = cell_of(lamp.center.x + lamp.radius);
        const i64 y0 = cell_of(lamp.center.y - lamp.radius), y1 = cell_of(lamp.center.y + lamp.radius);
        const i64 z0 = cell_of(lamp.center.z - lamp.radius), z1 = cell_of(lamp.center.z + lamp.radius);
        if ((x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > k_most_cells) {
            m_big.push_back(at);
            continue;
        }
        for (i64 z = z0; z <= z1; ++z)
            for (i64 y = y0; y <= y1; ++y)
                for (i64 x = x0; x <= x1; ++x) m_grid[key(x, y, z)].push_back(at);
    }
    if (m_lamps.empty()) return;

    struct Pick {
        u32 lamp;
        double score;
    };
    std::vector<Pick> picks;
    for (gpu::LightJob& j : jobs) {
        if (j.level >= clipmap.levels()) continue;
        const voxel::BrickCoord b{j.brick[0], j.brick[1], j.brick[2]};
        const DVec3 lo = clipmap.brick_min(j.level, b);
        const double size = clipmap.brick_size(j.level);
        const DVec3 hi = lo + DVec3{size, size, size};
        ++m_stamp;
        picks.clear();
        const auto consider = [&](u32 at) {
            Lamp& lamp = m_lamps[at];
            if (lamp.stamp == m_stamp) return;
            lamp.stamp = m_stamp;
            if (distance2(lamp.center, lo, hi) > lamp.radius * lamp.radius) return;
            // Brightness at the brick (from the light itself, not its sphere).
            const DVec3 p = lights[lamp.source].position;
            const double d2 = distance2(p, lo, hi);
            picks.push_back({at, double(lamp.power) / std::max(d2, 0.25)});
        };
        const i64 x0 = cell_of(lo.x), x1 = cell_of(hi.x);
        const i64 y0 = cell_of(lo.y), y1 = cell_of(hi.y);
        const i64 z0 = cell_of(lo.z), z1 = cell_of(hi.z);
        if ((x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > std::max<i64>(k_most_cells, i64(m_lamps.size()))) {
            for (u32 at = 0; at < m_lamps.size(); ++at) consider(at);
        } else {
            for (const u32 at : m_big) consider(at);
            for (i64 z = z0; z <= z1; ++z)
                for (i64 y = y0; y <= y1; ++y)
                    for (i64 x = x0; x <= x1; ++x) {
                        const auto it = m_grid.find(key(x, y, z));
                        if (it == m_grid.end()) continue;
                        for (const u32 at : it->second) consider(at);
                    }
        }
        if (picks.empty()) continue;
        const usize keep = std::min<usize>(picks.size(), k_max_voxel_lamps);
        std::partial_sort(picks.begin(), picks.begin() + static_cast<std::ptrdiff_t>(keep), picks.end(),
                          [](const Pick& x, const Pick& y) {
                              return x.score != y.score ? x.score > y.score : x.lamp < y.lamp;
                          });
        j.lamp_first = static_cast<u32>(m_entries.size());
        j.lamp_count = static_cast<u32>(keep);
        for (usize k = 0; k < keep; ++k) {
            Lamp& lamp = m_lamps[picks[k].lamp];
            if (lamp.gpu == ~0u) {
                lamp.gpu = static_cast<u32>(m_lights.size());
                m_lights.push_back(to_gpu(lights[lamp.source], camera));
            }
            m_entries.push_back(lamp.gpu);
        }
    }
}

} // namespace giant::render
