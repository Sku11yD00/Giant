#include "giant/world/ground_damage.h"

#include <algorithm>
#include <cmath>

namespace giant::world {

bool GroundDamage::ours(const Terrain& terrain, CellCoord cell, const Tile& t) const {
    return t.ours != 0 && terrain.version(cell) == t.ours;
}

void GroundDamage::dent(Terrain& terrain, const Dent& d, double x, double z, std::vector<CellCoord>& changed) {
    if (terrain.identity() != m_terrain) {
        m_tiles.clear();
        m_terrain = terrain.identity();
    }
    const double reach = dent_reach(d);
    if (!(reach > 0.0) || !std::isfinite(x) || !std::isfinite(z)) return;
    // The tiles it may change, kept as they are first (once).
    const CellCoord lo = terrain.tile_of(x - reach, z - reach);
    const CellCoord hi = terrain.tile_of(x + reach, z + reach);
    std::vector<CellCoord> kept;
    for (i32 cz = lo.z; cz <= hi.z; ++cz) {
        for (i32 cx = lo.x; cx <= hi.x; ++cx) {
            const CellCoord c{cx, cz};
            const Terrain::Heights* h = terrain.find(c);
            if (h == nullptr) continue;
            const auto it = m_tiles.find(c);
            if (it != m_tiles.end()) {
                if (ours(terrain, c, it->second)) continue;
                m_tiles.erase(it); // edited otherwise since: that edit is what it is now
            }
            Tile& t = m_tiles[c];
            t.before = *h;
            t.before_version = terrain.version(c);
            kept.push_back(c);
        }
    }
    const usize first = changed.size();
    apply_dent(terrain, d, x, z, changed);
    for (usize k = first; k < changed.size(); ++k) {
        const auto it = m_tiles.find(changed[k]);
        if (it != m_tiles.end()) it->second.ours = terrain.version(changed[k]);
    }
    // Kept but not changed: nothing to put back.
    for (const CellCoord c : kept) {
        const auto it = m_tiles.find(c);
        if (it != m_tiles.end() && it->second.ours == 0) m_tiles.erase(it);
    }
    ++m_dents;
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
        // Still as lifted (the saved heights): dented again.
        if (terrain.version(entry.first) != t.before_version) return true;
        terrain.set(entry.first, std::move(*t.lifted));
        t.lifted.reset();
        t.ours = terrain.version(entry.first);
        return false;
    });
}

} // namespace giant::world
