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

double ScatterPlacer::distance2(world::CellCoord cell) const {
    const double dx = (cell.x + 0.5) * world::k_cell_size - camera.x;
    const double dz = (cell.z + 0.5) * world::k_cell_size - camera.z;
    return dx * dx + dz * dz;
}

bool ScatterPlacer::in_range(world::CellCoord cell) const { return distance2(cell) <= radius * radius; }

// Every placement goes now (another scene, other ground): nothing stale
// drawn while the new ones are made; results on their way are not wanted.
void ScatterPlacer::drop_all() {
    for (const auto& [cell, placed] : m_cells) m_changed.push_back(cell);
    m_cells.clear();
    m_cut_short.clear();
    m_wanted.clear();
    m_instances = 0;
}

// Takes a cell's placements away (room freed for cells cut short).
void ScatterPlacer::remove(world::CellCoord cell) {
    const auto it = m_cells.find(cell);
    if (it == m_cells.end()) return;
    m_instances -= it->second.cell.instances;
    m_cells.erase(it);
    m_changed.push_back(cell);
    m_freed = true;
}

void ScatterPlacer::update(const world::Scene& scene, const world::TerrainChanges& changes) {
    const world::Terrain& terrain = scene.terrain;
    bool all = changes.all;
    if (terrain.identity() != m_terrain) {
        m_terrain = terrain.identity();
        drop_all();
        all = true;
    }
    // The kits items name, by piece count (what a point draws): a change
    // places every cell again, as a layer change does.
    u64 kits = 0x6B17u;
    for (const world::ScatterLayer& l : scene.scatter) {
        for (const world::ScatterItem& i : l.items) {
            if (i.shape != world::Shape::kit) continue;
            const world::Kit* kit = scene.find_kit(i.kit);
            kits = mix(kits, kit == nullptr ? 0 : kit->pieces.size() + 1);
        }
    }
    if (!(scene.scatter == *m_layers) || kits != m_kits) {
        m_layers = std::make_shared<const Layers>(scene.scatter);
        m_kits = kits;
        ++m_layers_serial;
        all = true;
    }
    const world::CellCoord camera_cell = world::cell_of({camera.x, 0.0, camera.z});
    const bool moved = !(camera_cell == m_camera_cell) || radius != m_radius;
    m_camera_cell = camera_cell;
    m_radius = radius;
    if (moved) {
        // Cells out of range go now, before nearer ones arrive wanting room.
        std::vector<world::CellCoord> gone;
        for (const auto& [cell, placed] : m_cells) {
            if (!in_range(cell)) gone.push_back(cell);
        }
        for (const world::CellCoord c : gone) remove(c);
        std::erase_if(m_cut_short, [&](world::CellCoord c) { return !in_range(c); });
        std::erase_if(m_wanted, [&](const auto& w) { return !in_range(w.first); }); // their results are not wanted
    }
    if (all || moved) {
        // Cells come in or go out of range (or all change): nearest first.
        for (const auto& [cell, heights] : terrain.tiles()) {
            if (all || in_range(cell)) want(cell);
        }
        for (const auto& [cell, placed] : m_cells) want(cell);
        std::vector<world::CellCoord> queue(m_queue.begin(), m_queue.end());
        std::sort(queue.begin(), queue.end(),
                  [&](world::CellCoord a, world::CellCoord b) { return distance2(a) < distance2(b); });
        m_queue.assign(queue.begin(), queue.end());
    } else {
        for (const world::CellCoord c : changes.tiles) want_around(c);
    }

    // Finished placements swap in whole, within the instance budget.
    {
        std::vector<Result> done;
        {
            const std::lock_guard lock(m_mutex);
            done.swap(m_done);
        }
        for (Result& r : done) m_arrived.push_back(std::move(r));
    }
    usize instances = 0;
    bool first = true;
    while (!m_arrived.empty()) {
        Result& r = m_arrived.front();
        if (!first && instances + r.placed.instances > instances_per_frame) break;
        first = false;
        --m_in_flight;
        const auto w = m_wanted.find(r.cell);
        if (w != m_wanted.end() && w->second.serial == r.serial) {
            m_wanted.erase(w);
            const u64 was = [&] {
                const auto it = m_cells.find(r.cell);
                return it == m_cells.end() ? u64{0} : it->second.cell.instances;
            }();
            u64 others = m_instances - was;
            if (others + r.placed.instances > max_instances) {
                // Over the whole budget: farther cells make room (they are
                // left out, to come back when there is room), then what
                // still does not fit is cut.
                const double d = distance2(r.cell);
                std::vector<std::pair<double, world::CellCoord>> farther;
                for (const auto& [cell, placed] : m_cells) {
                    const double e = distance2(cell);
                    if (e > d) farther.emplace_back(e, cell);
                }
                std::sort(farther.begin(), farther.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                for (const auto& [e, cell] : farther) {
                    if (others + r.placed.instances <= max_instances) break;
                    const auto it = m_cells.find(cell);
                    others -= it->second.cell.instances;
                    m_instances -= it->second.cell.instances;
                    m_cells.erase(it);
                    m_changed.push_back(cell);
                    m_cut_short.insert(cell);
                }
            }
            if (others + r.placed.instances > max_instances) {
                const u64 room = max_instances > others ? max_instances - others : 0;
                u64 kept = 0;
                usize n = 0;
                for (; n < r.placed.points.size(); ++n) {
                    const world::ScatterPoint& p = r.placed.points[n];
                    const u64 w8 = r.weights[r.first[p.layer] + p.item];
                    if (kept + w8 > room) break;
                    kept += w8;
                }
                r.placed.points.resize(n);
                r.placed.instances = kept;
                r.cut_short = true;
            }
            instances += r.placed.instances;
            ++m_placed_count;
            if (r.cut_short) {
                m_cut_short.insert(r.cell);
            } else {
                m_cut_short.erase(r.cell);
            }
            if (r.placed.points.empty()) {
                remove(r.cell);
            } else {
                m_freed = m_freed || r.placed.instances < was;
                m_instances = m_instances - was + r.placed.instances;
                m_cells[r.cell] = {std::move(r.placed), r.key};
                m_changed.push_back(r.cell);
            }
        }
        m_arrived.pop_front();
    }
    // Room freed: the cells cut short (or left out) try again, nearest first.
    if (m_freed && !m_cut_short.empty()) {
        std::vector<world::CellCoord> retry(m_cut_short.begin(), m_cut_short.end());
        std::sort(retry.begin(), retry.end(),
                  [&](world::CellCoord a, world::CellCoord b) { return distance2(a) < distance2(b); });
        for (const world::CellCoord c : retry) want(c);
    }
    m_freed = false;

    // Look at the cells wanted: start a placement where the key changed.
    bool any = false;
    for (const world::ScatterLayer& l : *m_layers) any = any || (l.visible && !l.items.empty());
    for (u32 looked = 0; looked < looks_per_frame && !m_queue.empty() && m_in_flight < cells_in_flight; ++looked) {
        const world::CellCoord cell = m_queue.front();
        m_queue.pop_front();
        m_queued.erase(cell);
        const u64 key = in_range(cell) ? key_of(terrain, cell) : 0;
        const auto placed = m_cells.find(cell);
        const auto flying = m_wanted.find(cell);
        if (flying != m_wanted.end() && flying->second.key == key) continue; // on its way
        const bool cut = m_cut_short.contains(cell); // tries again whatever its key
        if (flying == m_wanted.end() && !cut && (placed == m_cells.end() ? key == 0 : placed->second.key == key)) {
            continue;
        }
        if (key == 0 || !any) {
            // Nothing to place: gone now (and whatever is in flight is not wanted).
            if (flying != m_wanted.end()) m_wanted.erase(flying);
            m_cut_short.erase(cell);
            remove(cell);
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
        // What a point of each item draws: a kit's pieces (decals too), or one.
        std::vector<u32> first_item(m_layers->size() + 1, 0);
        std::vector<u32> weights;
        for (usize k = 0; k < m_layers->size(); ++k) {
            for (const world::ScatterItem& item : (*m_layers)[k].items) {
                u32 w8 = 1;
                if (item.shape == world::Shape::kit) {
                    const world::Kit* kit = scene.find_kit(item.kit);
                    w8 = kit == nullptr ? 0 : static_cast<u32>(kit->pieces.size());
                }
                weights.push_back(std::max<u32>(w8, 1)); // a point costs one at least
            }
            first_item[k + 1] = static_cast<u32>(weights.size());
        }
        const u64 serial = ++m_serial;
        m_wanted[cell] = {serial, key};
        ++m_in_flight;
        m_jobs.submit(m_jobs_done, [this, cell, serial, key, layers = m_layers, ground = std::move(ground),
                                    weights = std::move(weights), first_item = std::move(first_item)]() mutable {
            Result r;
            r.cell = cell;
            r.serial = serial;
            r.key = key;
            r.placed.layers = layers;
            for (usize k = 0; k < layers->size() && !r.cut_short; ++k) {
                const usize from = r.placed.points.size();
                world::scatter_cell((*layers)[k], static_cast<u32>(k), ground, cell, r.placed.points);
                for (usize n = from; n < r.placed.points.size(); ++n) {
                    const world::ScatterPoint& p = r.placed.points[n];
                    const u32 w8 = weights[first_item[p.layer] + p.item];
                    if (r.placed.instances + w8 > k_max_instances_per_cell) {
                        r.placed.points.resize(n);
                        r.cut_short = true;
                        break;
                    }
                    r.placed.instances += w8;
                }
            }
            r.weights = std::move(weights);
            r.first = std::move(first_item);
            const std::lock_guard lock(m_mutex);
            m_done.push_back(std::move(r));
        });
    }
}

void ScatterPlacer::finish(const world::Scene& scene, const world::TerrainChanges& changes) {
    const u32 looks = looks_per_frame, flights = cells_in_flight;
    const usize budget = instances_per_frame;
    looks_per_frame = cells_in_flight = std::numeric_limits<u32>::max();
    instances_per_frame = std::numeric_limits<usize>::max();
    update(scene, changes);
    for (;;) {
        m_jobs.wait(m_jobs_done);
        update(scene, {});
        if (m_queue.empty() && m_in_flight == 0 && m_arrived.empty()) break;
    }
    looks_per_frame = looks;
    cells_in_flight = flights;
    instances_per_frame = budget;
}

ScatterStats ScatterPlacer::stats() const {
    ScatterStats s;
    s.cells = static_cast<u32>(m_cells.size());
    for (const auto& [cell, placed] : m_cells) s.points += placed.cell.points.size();
    s.instances = m_instances;
    usize flying = 0; // in flight and not queued again too
    for (const auto& [cell, flight] : m_wanted) flying += m_queued.contains(cell) ? 0u : 1u;
    s.building = static_cast<u32>(m_queue.size() + flying);
    s.cut_short = static_cast<u32>(m_cut_short.size());
    s.placed = m_placed_count;
    return s;
}

} // namespace giant::editor
