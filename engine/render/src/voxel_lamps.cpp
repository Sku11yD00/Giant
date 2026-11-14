#include "voxel_lamps.h"

#include "lights.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace giant::render {

namespace {

constexpr i64 k_most_cells = 64; // a lamp or brick spanning more looks at all

bool usable(const LocalLight& l) {
    return std::isfinite(l.position.x + l.position.y + l.position.z) && l.intensity > 0.0f &&
           std::isfinite(l.intensity) && std::isfinite(l.color.x + l.color.y + l.color.z) &&
           std::max({l.color.x, l.color.y, l.color.z}) > 0.0f;
}

float clamped_lamp_range(float range) { return std::isfinite(range) ? std::clamp(range, 0.01f, 1000.0f) : 0.01f; }

// Grid g's cells are k_voxel_lamp_cell * 2^g across.
double cell_size(u32 g) { return k_voxel_lamp_cell * static_cast<double>(u64{1} << g); }
i64 cell_of(double v, double size) { return static_cast<i64>(std::floor(v / size)); }

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
    m_listed = 0;
    m_merged = 0;
}

void VoxelLamps::list(std::span<const LocalLight> lights, std::span<const gpu::LightJob> in,
                      const voxel::Clipmap& clipmap, const DVec3& camera) {
    m_jobs.assign(in.begin(), in.end());
    std::vector<gpu::LightJob>& jobs = m_jobs;
    m_lamps.clear();
    for (std::vector<u32>& big : m_big) big.clear();
    m_lights.clear();
    m_entries.clear();
    m_listed = 0;
    m_merged = 0;
    for (u32 g = 0; g < k_voxel_lamp_grids; ++g) {
        if ((m_filled & (1u << g)) == 0) continue; // (emptied already)
        std::unordered_map<u64, std::vector<u32>>& grid = m_grid[g];
        usize used_cells = 0;
        for (auto& [k, cell] : grid) {
            used_cells += cell.empty() ? 0u : 1u;
            cell.clear();
        }
        if (grid.size() > 4096 && grid.size() > used_cells * 4) grid.clear(); // (lamps moved on)
    }
    m_filled = 0;
    for (gpu::LightJob& j : jobs) {
        j.lamp_first = 0;
        j.lamp_count = 0;
    }
    if (jobs.empty()) return;
    // The grids this frame's bricks look in (the finest whose cells are at
    // least the brick's size, else the coarsest): only those are filled.
    const auto grid_for = [](double size) {
        u32 g = 0;
        while (g + 1 < k_voxel_lamp_grids && cell_size(g) < size) ++g;
        return g;
    };
    u32 needed = 0;
    for (const gpu::LightJob& j : jobs) {
        if (j.level < clipmap.levels()) needed |= 1u << grid_for(clipmap.brick_size(j.level));
    }
    m_filled = needed;
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
        float share = 1.0f;
        if (l.kind == LightKind::spot) {
            const float outer = std::isfinite(l.outer_angle) ? std::clamp(l.outer_angle, 0.001f, 1.5533f) : 1.5533f;
            share = 1.0f - std::cos(outer);
        }
        const float sent = l.intensity * share;
        lamp.sent = {std::min(std::max(l.color.x, 0.0f) * sent, 1e9f), std::min(std::max(l.color.y, 0.0f) * sent, 1e9f),
                     std::min(std::max(l.color.z, 0.0f) * sent, 1e9f)};
        lamp.position = l.position;
        lamp.range = clamped_lamp_range(l.range);
        const u32 at = static_cast<u32>(m_lamps.size());
        m_lamps.push_back(lamp);
        for (u32 g = 0; g < k_voxel_lamp_grids; ++g) {
            if ((needed & (1u << g)) == 0) continue;
            const double s = cell_size(g);
            const i64 x0 = cell_of(lamp.center.x - lamp.radius, s), x1 = cell_of(lamp.center.x + lamp.radius, s);
            const i64 y0 = cell_of(lamp.center.y - lamp.radius, s), y1 = cell_of(lamp.center.y + lamp.radius, s);
            const i64 z0 = cell_of(lamp.center.z - lamp.radius, s), z1 = cell_of(lamp.center.z + lamp.radius, s);
            if ((x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > k_most_cells) {
                m_big[g].push_back(at);
                continue;
            }
            for (i64 z = z0; z <= z1; ++z)
                for (i64 y = y0; y <= y1; ++y)
                    for (i64 x = x0; x <= x1; ++x) m_grid[g][key(x, y, z)].push_back(at);
        }
    }
    if (m_lamps.empty()) return;

    std::vector<Pick>& picks = m_picks;
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
            const double d2 = distance2(lamp.position, lo, hi);
            picks.push_back({at, double(lamp.power) / std::max(d2, 0.25)});
        };
        const u32 g = grid_for(size); // (it spans at most two of its cells a side)
        const double s = cell_size(g);
        // (Its far faces are not in it: a brick as big as the cells lies in
        // one. A lamp only touching a far face lights nothing there.)
        const i64 x0 = cell_of(lo.x, s), x1 = cell_of(std::nextafter(hi.x, lo.x), s);
        const i64 y0 = cell_of(lo.y, s), y1 = cell_of(std::nextafter(hi.y, lo.y), s);
        const i64 z0 = cell_of(lo.z, s), z1 = cell_of(std::nextafter(hi.z, lo.z), s);
        if ((x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > std::max<i64>(k_most_cells, i64(m_lamps.size()))) {
            for (u32 at = 0; at < m_lamps.size(); ++at) consider(at);
        } else {
            for (const u32 at : m_big[g]) consider(at);
            const std::unordered_map<u64, std::vector<u32>>& grid = m_grid[g];
            for (i64 z = z0; z <= z1; ++z)
                for (i64 y = y0; y <= y1; ++y)
                    for (i64 x = x0; x <= x1; ++x) {
                        const auto it = grid.find(key(x, y, z));
                        if (it == grid.end()) continue;
                        for (const u32 at : it->second) consider(at);
                    }
        }
        if (picks.empty()) continue;
        j.lamp_first = static_cast<u32>(m_entries.size());
        if (picks.size() > k_max_voxel_lamps && size / 8.0 >= k_voxel_lamp_merge) {
            merge(j, picks, lights, lo, size, camera);
            continue;
        }
        const usize keep = std::min<usize>(picks.size(), k_max_voxel_lamps);
        std::partial_sort(picks.begin(), picks.begin() + static_cast<std::ptrdiff_t>(keep), picks.end(),
                          [](const Pick& x, const Pick& y) {
                              return x.score != y.score ? x.score > y.score : x.lamp < y.lamp;
                          });
        j.lamp_count = static_cast<u32>(keep);
        for (usize k = 0; k < keep; ++k) m_entries.push_back(record(picks[k].lamp, lights, camera));
    }
}

u32 VoxelLamps::record(u32 at, std::span<const LocalLight> lights, const DVec3& camera) {
    Lamp& lamp = m_lamps[at];
    if (lamp.gpu == ~0u) {
        lamp.gpu = static_cast<u32>(m_lights.size());
        m_lights.push_back(to_gpu(lights[lamp.source], camera));
    }
    count(lamp);
    return lamp.gpu;
}

void VoxelLamps::count(Lamp& lamp) {
    if (!lamp.counted) {
        lamp.counted = true;
        ++m_listed;
    }
}

void VoxelLamps::merge(gpu::LightJob& j, std::span<const Pick> picks, std::span<const LocalLight> lights,
                       const DVec3& lo, double size, const DVec3& camera) {
    // Lamps inside the brick reaching no further than an eighth of it are
    // merged by eighth; the rest (bigger, or from outside) keep their own,
    // as do merged eighths, the brightest at the brick first.
    std::array<std::vector<u32>, 8>& parts = m_parts;
    for (std::vector<u32>& part : parts) part.clear();
    std::array<double, 8> part_score{};
    m_items.clear();
    for (const Pick& p : picks) {
        const Lamp& lamp = m_lamps[p.lamp];
        const DVec3 at = lamp.position;
        const bool inside = at.x >= lo.x && at.y >= lo.y && at.z >= lo.z && at.x < lo.x + size &&
                            at.y < lo.y + size && at.z < lo.z + size;
        if (inside && double(lamp.range) <= size * 0.5) {
            const u32 o = (at.x >= lo.x + size * 0.5 ? 1u : 0u) | (at.y >= lo.y + size * 0.5 ? 2u : 0u) |
                          (at.z >= lo.z + size * 0.5 ? 4u : 0u);
            parts[o].push_back(p.lamp);
            part_score[o] += p.score;
        } else {
            m_items.push_back({p.lamp, p.score, false});
        }
    }
    for (u32 o = 0; o < 8; ++o) {
        if (!parts[o].empty()) m_items.push_back({o, part_score[o], true});
    }
    const usize keep = std::min<usize>(m_items.size(), k_max_voxel_lamps);
    std::partial_sort(m_items.begin(), m_items.begin() + static_cast<std::ptrdiff_t>(keep), m_items.end(),
                      [](const Item& x, const Item& y) {
                          if (x.score != y.score) return x.score > y.score;
                          return x.part != y.part ? x.part : x.at < y.at;
                      });
    for (usize k = 0; k < keep; ++k) {
        const Item& item = m_items[k];
        if (!item.part) {
            m_entries.push_back(record(item.at, lights, camera));
            continue;
        }
        const std::vector<u32>& part = parts[item.at];
        if (part.size() == 1) {
            m_entries.push_back(record(part[0], lights, camera));
            continue;
        }
        // Summed at their centre (weighted by power), reaching as far as
        // they all do; a spot counts as the point light sending as much
        // light down its cone (all of it lands on the coarse ground).
        Vec3 total{};
        DVec3 centre{};
        double weight = 0.0;
        for (const u32 at : part) {
            Lamp& lamp = m_lamps[at];
            const Vec3 c = lamp.sent;
            total = total + c;
            const double w = std::max({c.x, c.y, c.z});
            centre = centre + lamp.position * w;
            weight += w;
            count(lamp);
        }
        centre = weight > 0.0 ? centre * (1.0 / weight) : m_lamps[part[0]].position;
        double reach = 0.0;
        for (const u32 at : part) {
            const Lamp& lamp = m_lamps[at];
            reach = std::max(reach, length(lamp.position - centre) + double(lamp.range));
        }
        const float peak = std::max({total.x, total.y, total.z});
        if (!(peak > 0.0f) || !std::isfinite(peak)) continue;
        LocalLight merged;
        merged.position = centre;
        merged.kind = LightKind::point;
        merged.range = static_cast<float>(reach);
        merged.intensity = peak;
        merged.color = total * (1.0f / peak);
        merged.source_radius = 0.0f;
        m_entries.push_back(static_cast<u32>(m_lights.size()));
        m_lights.push_back(to_gpu(merged, camera));
        ++m_merged;
    }
    j.lamp_count = static_cast<u32>(m_entries.size()) - j.lamp_first;
}

} // namespace giant::render
