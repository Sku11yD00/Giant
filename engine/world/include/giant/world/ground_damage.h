// The ground dented while simulating (craters, footprints): pressed into
// the scene's terrain so everything that follows the ground follows it
// (meshes, physics, voxels, scatter), with each tile's heights as they
// were kept aside, so stopping puts the ground back exactly (the same
// heights and versions: nothing reads as edited or unsaved afterwards).
//
//   GroundDamage damage;
//   damage.dent(scene.terrain, crater, x, z, changed);
//   damage.lift(scene.terrain); save(); damage.put_back(scene.terrain); // a save keeps the ground as it was
//   damage.restore(scene.terrain); // stopping
//
// A tile edited otherwise after its last dent (a sculpt stroke, an undo) is
// left as that edit made it: restore and lift skip it and forget it.
#pragma once

#include "giant/world/terrain.h"
#include "giant/world/terrain_brush.h"

#include <optional>
#include <unordered_map>
#include <vector>

namespace giant::world {

class GroundDamage {
public:
    // Presses `dent` in at (x, z); the tiles it changes go into `changed`.
    void dent(Terrain& terrain, const Dent& dent, double x, double z, std::vector<CellCoord>& changed);
    // A whole tile pressed in as `heights` (a game save's ground, shown over
    // the ground as authored): kept aside as it is first, as a dent's are.
    // False (nothing changed) when there is no tile there or its size differs.
    bool press(Terrain& terrain, CellCoord cell, Terrain::Heights heights);
    // The tiles still as we left them (not lifted), in cell order.
    std::vector<CellCoord> ours_tiles(const Terrain& terrain) const;
    // Every dented tile as it was. Returns how many were put back.
    usize restore(Terrain& terrain);
    // The dented tiles as they were for now (a save), their dents kept
    // aside; put_back presses them in again.
    void lift(Terrain& terrain);
    void put_back(Terrain& terrain);
    // The dents kept for good (keep_damage): the dented tiles stay as they
    // are, now the ground as authored, and are forgotten here. Returns each
    // one's heights before its first dent and now, with their versions (an
    // undo puts `before` back with Terrain::restore).
    struct Kept {
        CellCoord cell;
        Terrain::Heights before;
        u64 before_version = 0;
        Terrain::Heights after;
        u64 after_version = 0;
    };
    std::vector<Kept> keep(const Terrain& terrain);
    // A new scene: nothing to put back.
    void forget() {
        m_tiles.clear();
        m_dents = 0;
    }
    usize tiles() const { return m_tiles.size(); }
    u32 dents() const { return m_dents; }

private:
    struct Tile {
        Terrain::Heights before;
        u64 before_version = 0;
        u64 ours = 0; // the tile's version after our last change (0: none yet)
        std::optional<Terrain::Heights> lifted;
    };
    // Whether the tile is still as we left it.
    bool ours(const Terrain& terrain, CellCoord cell, const Tile& t) const;

    std::unordered_map<CellCoord, Tile, CellCoordHash> m_tiles;
    u64 m_terrain = 0; // the terrain's identity they are of
    u32 m_dents = 0;
};

} // namespace giant::world
