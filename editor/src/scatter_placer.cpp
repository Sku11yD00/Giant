#include "scatter_placer.h"

#include <algorithm>
#include <limits>

namespace giant::editor {

namespace {

u64 mix(u64 h, u64 v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xBF58476D1CE4E5B9ull;
}

bool in_i32(i64 v) { return v >= std::numeric_limits<i32>::min() && v <= std::numeric_limits<i32>::max(); }

} // namespace

ScatterPlacer::ScatterPlacer() = default;

ScatterPlacer::~ScatterPlacer() { m_jobs.wait(m_jobs_done); }

const ScatterPlacer::Cell* ScatterPlacer::find(world::CellCoord cell) const {
    const auto it = m_cells.find(cell);
    return it == m_cells.end() ? nullptr : &it->second.cell;
}

std::vector<world::CellCoord> ScatterPlacer::take_changed() {
    std::vector<world::CellCoord> out = std::move(m_changed);
    m_changed.clear();
    return out;
}

std::vector<world::CellCoord> ScatterPlacer::cells() const {
    std::vector<world::CellCoord> out;
    out.reserve(m_cells.size());
    for (const auto& [cell, placed] : m_cells) out.push_back(cell);
    return out;
}

void ScatterPlacer::want(world::CellCoord cell) {
    if (m_queued.insert(cell).second) m_queue.push_back(cell);
}

void ScatterPlacer::want_around(world::CellCoord c) {
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            const i64 x = i64{c.x} + dx, z = i64{c.z} + dz;
            if (in_i32(x) && in_i32(z)) want({static_cast<i32>(x), static_cast<i32>(z)});
        }
    }
}

// The layers and the ground a cell's placements read (its tile and its
// neighbours'); 0 when the cell has no tile (nothing to place).
u64 ScatterPlacer::key_of(const world::Terrain& terrain, world::CellCoord cell) const {
    if (terrain.find(cell) == nullptr) return 0;
    u64 h = mix(0x51ED270B27u, m_layers_serial);
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            const i64 x = i64{cell.x} + dx, z = i64{cell.z} + dz;
            const u64 v = in_i32(x) && in_i32(z) ? terrain.version({static_cast<i32>(x), static_cast<i32>(z)}) : 0;
            h = mix(h, v);
        }
    }
    return h == 0 ? 1 : h;
}

void ScatterPlacer::update(const world::Scene& scene, const world::TerrainChanges& changes) {
    const world::Terrain& terrain = scene.terrain;
    bool all = changes.all;
    if (terrain.identity() != m_terrain) {
        m_terrain = terrain.identity();
        all = true;
    }
    if (!(scene.scatter == *m_layers)) {
        m_layers = std::make_shared<const Layers>(scene.scatter);
        ++m_layers_serial;
        all = true;
    }
    if (all) {
        for (const auto& [cell, heights] : terrain.tiles()) want(cell);
        for (const auto& [cell, placed] : m_cells) want(cell);
    } else {
        for (const world::CellCoord c : changes.tiles) want_around(c);
    }

    // Finished placements swap in whole, within the points budget.
    {
        std::vector<Result> done;
        {
            const std::lock_guard lock(m_mutex);
            done.swap(m_done);
        }
        for (Result& r : done) m_arrived.push_back(std::move(r));
    }
    usize points = 0;
    bool first = true;
    while (!m_arrived.empty()) {
        Result& r = m_arrived.front();
        if (!first && points + r.placed.points.size() > points_per_frame) break;
        first = false;
        --m_in_flight;
        const auto w = m_wanted.find(r.cell);
        if (w != m_wanted.end() && w->second.serial == r.serial) {
            m_wanted.erase(w);
            points += r.placed.points.size();
            ++m_placed_count;
            if (r.cut_short) {
                m_cut_short.insert(r.cell);
            } else {
                m_cut_short.erase(r.cell);
            }
            if (r.placed.points.empty()) {
                if (m_cells.erase(r.cell) > 0) m_changed.push_back(r.cell);
            } else {
                m_cells[r.cell] = {std::move(r.placed), r.key};
                m_changed.push_back(r.cell);
            }
        }
        m_arrived.pop_front();
    }

    // Look at the cells wanted: start a placement where the key changed.
    for (u32 looked = 0; looked < looks_per_frame && !m_queue.empty() && m_in_flight < cells_in_flight; ++looked) {
        const world::CellCoord cell = m_queue.front();
        m_queue.pop_front();
        m_queued.erase(cell);
        const u64 key = key_of(terrain, cell);
        const auto placed = m_cells.find(cell);
        const auto flying = m_wanted.find(cell);
        if (flying != m_wanted.end() && flying->second.key == key) continue; // on its way
        if (flying == m_wanted.end() && (placed == m_cells.end() ? key == 0 : placed->second.key == key)) continue;
        bool any = false;
        for (const world::ScatterLayer& l : *m_layers) any = any || (l.visible && !l.items.empty());
        if (key == 0 || !any) {
            // Nothing to place: gone now (and whatever is in flight is not wanted).
            if (flying != m_wanted.end()) m_wanted.erase(flying);
            m_cut_short.erase(cell);
            if (placed != m_cells.end()) {
                m_cells.erase(placed);
                m_changed.push_back(cell);
            }
            continue;
        }
        // A copy of the ground it reads (the tile and its neighbours').
        world::Terrain ground;
        ground.set_samples(terrain.samples());
        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                const i64 x = i64{cell.x} + dx, z = i64{cell.z} + dz;
                if (!in_i32(x) || !in_i32(z)) continue;
                const world::CellCoord n{static_cast<i32>(x), static_cast<i32>(z)};
                if (const world::Terrain::Heights* h = terrain.find(n)) ground.set(n, *h);
            }
        }
        const u64 serial = ++m_serial;
        m_wanted[cell] = {serial, key};
        ++m_in_flight;
        m_jobs.submit(m_jobs_done, [this, cell, serial, key, layers = m_layers, ground = std::move(ground)] {
            Result r;
            r.cell = cell;
            r.serial = serial;
            r.key = key;
            r.placed.layers = layers;
            for (usize k = 0; k < layers->size(); ++k) {
                world::scatter_cell((*layers)[k], static_cast<u32>(k), ground, cell, r.placed.points);
                if (r.placed.points.size() > k_max_points_per_cell) {
                    r.placed.points.resize(k_max_points_per_cell);
                    r.cut_short = true;
                    break;
                }
            }
            const std::lock_guard lock(m_mutex);
            m_done.push_back(std::move(r));
        });
    }
}

void ScatterPlacer::finish(const world::Scene& scene, const world::TerrainChanges& changes) {
    const u32 looks = looks_per_frame, flights = cells_in_flight;
    const usize budget = points_per_frame;
    looks_per_frame = cells_in_flight = std::numeric_limits<u32>::max();
    points_per_frame = std::numeric_limits<usize>::max();
    update(scene, changes);
    for (;;) {
        m_jobs.wait(m_jobs_done);
        update(scene, {});
        if (m_queue.empty() && m_in_flight == 0 && m_arrived.empty()) break;
    }
    looks_per_frame = looks;
    cells_in_flight = flights;
    points_per_frame = budget;
}

ScatterStats ScatterPlacer::stats() const {
    ScatterStats s;
    s.cells = static_cast<u32>(m_cells.size());
    for (const auto& [cell, placed] : m_cells) s.points += placed.cell.points.size();
    s.building = static_cast<u32>(m_queue.size() + m_wanted.size());
    s.cut_short = static_cast<u32>(m_cut_short.size());
    s.placed = m_placed_count;
    return s;
}

} // namespace giant::editor
