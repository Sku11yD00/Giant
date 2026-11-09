#include "giant/world/ground_damage.h"

#include <algorithm>
#include <cmath>

namespace giant::world {

bool GroundDamage::ours(const Terrain& terrain, CellCoord cell, const Tile& t) const {
    return t.ours != 0 && terrain.version(cell) == t.ours;
}

void GroundDamage::dent(Terrain& terrain, const Dent& d, double x, double z, std::vector<CellCoord>& changed) {
    if (terrain.identity() != m_terrain) {
        forget();
        m_terrain = terrain.identity();
    }
    const usize first = changed.size();
    // Each tile it changes kept as it is first (once while it stays as we
    // leave it; edited otherwise since, that edit is what it is now).
    apply_dent(terrain, d, x, z, changed, [&](CellCoord c) {
        const auto it = m_tiles.find(c);
        if (it != m_tiles.end() && ours(terrain, c, it->second)) return;
        Tile& t = m_tiles[c];
        t = Tile{};
        t.before = *terrain.find(c);
        t.before_version = terrain.version(c);
    });
    for (usize k = first; k < changed.size(); ++k) m_tiles.at(changed[k]).ours = terrain.version(changed[k]);
    if (changed.size() > first) ++m_dents;
}

bool GroundDamage::press(Terrain& terrain, CellCoord cell, Terrain::Heights heights) {
    if (terrain.identity() != m_terrain) {
        forget();
        m_terrain = terrain.identity();
    }
    const Terrain::Heights* now = terrain.find(cell);
    if (now == nullptr || now->size() != heights.size()) return false;
    const auto it = m_tiles.find(cell);
    if (it == m_tiles.end() || it->second.lifted || !ours(terrain, cell, it->second)) {
        Tile& t = m_tiles[cell];
        t = Tile{};
        t.before = *now;
        t.before_version = terrain.version(cell);
    }
    terrain.set(cell, std::move(heights));
    m_tiles.at(cell).ours = terrain.version(cell);
    ++m_dents;
    return true;
}

std::vector<CellCoord> GroundDamage::ours_tiles(const Terrain& terrain) const {
    std::vector<CellCoord> out;
    if (terrain.identity() != m_terrain) return out;
    for (const auto& [c, t] : m_tiles)
        if (!t.lifted && ours(terrain, c, t)) out.push_back(c);
    std::sort(out.begin(), out.end(), [](CellCoord a, CellCoord b) { return a.z != b.z ? a.z < b.z : a.x < b.x; });
    return out;
}

usize GroundDamage::restore(Terrain& terrain) {
    usize back = 0;
    if (terrain.identity() == m_terrain) {
        std::vector<CellCoord> cells;
        cells.reserve(m_tiles.size());
        for (const auto& [c, t] : m_tiles) cells.push_back(c);
        std::sort(cells.begin(), cells.end(), [](CellCoord a, CellCoord b) { return a.z != b.z ? a.z < b.z : a.x < b.x; });
        for (const CellCoord c : cells) {
            Tile& t = m_tiles.at(c);
            if (t.lifted) continue; // lifted: as it was already
            if (!ours(terrain, c, t)) continue;
            terrain.restore(c, std::move(t.before), t.before_version);
            ++back;
        }
    }
    m_tiles.clear();
    m_dents = 0;
    return back;
}

std::vector<GroundDamage::Kept> GroundDamage::keep(const Terrain& terrain) {
    std::vector<Kept> out;
    if (terrain.identity() == m_terrain) {
        for (auto& [c, t] : m_tiles) {
            if (t.lifted || !ours(terrain, c, t)) continue;
            out.push_back({c, std::move(t.before), t.before_version, *terrain.find(c), terrain.version(c)});
        }
    }
    std::sort(out.begin(), out.end(), [](const Kept& a, const Kept& b) {
        return a.cell.z != b.cell.z ? a.cell.z < b.cell.z : a.cell.x < b.cell.x;
    });
    m_tiles.clear();
    m_dents = 0;
    return out;
}

void GroundDamage::lift(Terrain& terrain) {
    if (terrain.identity() != m_terrain) {
        m_tiles.clear();
        return;
    }
    std::erase_if(m_tiles, [&](auto& entry) {
        Tile& t = entry.second;
        if (t.lifted) return false;
        if (!ours(terrain, entry.first, t)) return true;
        t.lifted = *terrain.find(entry.first);
        terrain.restore(entry.first, t.before, t.before_version);
        return false;
    });
}

void GroundDamage::put_back(Terrain& terrain) {
    if (terrain.identity() != m_terrain) {
        m_tiles.clear();
        return;
    }
    std::erase_if(m_tiles, [&](auto& entry) {
        Tile& t = entry.second;
        if (!t.lifted) return false;
        // Still as lifted (the saved heights): dented again, as exactly as it
        // was (its version too: nothing that follows it sees a change).
        if (terrain.version(entry.first) != t.before_version) return true;
        terrain.restore(entry.first, std::move(*t.lifted), t.ours);
        t.lifted.reset();
        return false;
    });
}

} // namespace giant::world
