// The ground: a height field cut into tiles, one per world cell
// (k_cell_size), so it is drawn, edited and (later) streamed by cell like
// everything else. What a height field cannot hold (overhangs, caves, the
// craters a giantess leaves) comes later as voxel edits on top.
//
// Heights are metres above y = 0 at evenly spaced samples. A tile holds
// samples() x samples() of them, the first at its cell's corner (lowest x
// and z); the last row and column of the surface come from the
// neighbouring tiles, so tiles meet without a seam. Where a neighbour is
// missing, the tile's own edge samples stand in for it (Terrain::sample).
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/world/cell.h"
#include "giant/world/material_maps.h"

#include <optional>
#include <unordered_map>
#include <vector>

namespace giant::world {

// Samples per tile side: a power of two from 8 (8 m apart) to 128 (0.5 m).
constexpr u32 k_min_terrain_samples = 8;
constexpr u32 k_max_terrain_samples = 128;
constexpr u32 k_default_terrain_samples = 64; // 1 m apart
bool valid_terrain_samples(u32 samples);

// Heights stay within this, either side of 0 (metres).
constexpr float k_max_terrain_height = 10000.0f;

// One of the ground's materials (render::TerrainLayer): its colour and
// roughness, image files mapped by world position every `tiling` metres
// (from above, and from the sides where steep), and where it lies: the
// first layer everywhere, each later one over those before it where the
// slope (degrees from level) and height (metres above y = 0) are within
// its bands, fading over the blends at their edges.
struct TerrainLayer {
    Vec3 base_color{0.23f, 0.2f, 0.15f}; // linear; multiplies maps.base_color
    float roughness = 0.92f;             // multiplies the roughness map (its green, as glTF)
    MaterialMaps maps;                   // base_color, normal, roughness_metal (occlusion unused)
    float tiling = 4.0f;                 // metres a repeat of the images covers
    float min_slope = 0.0f;
    float max_slope = 90.0f;
    float slope_blend = 5.0f;
    float min_height = -10000.0f;
    float max_height = 10000.0f;
    float height_blend = 10.0f;
    bool operator==(const TerrainLayer&) const = default;
};
constexpr usize k_max_terrain_layers = 8; // render::k_max_terrain_layers
constexpr float k_min_terrain_tiling = 0.05f;
constexpr float k_max_terrain_tiling = 1000.0f;
constexpr float k_max_terrain_layer_height = 20000.0f; // a band's edges, either side of 0 (metres)

// How the ground looks: one material for all of it, or its layers. The
// far voxel world (past the meshes) always takes base_color: set it near
// the layers' look from afar.
struct TerrainMaterial {
    Vec3 base_color{0.23f, 0.2f, 0.15f}; // linear: dry earth
    float roughness = 0.92f;
    std::vector<TerrainLayer> layers; // up to k_max_terrain_layers (none: base_color and roughness)
    bool operator==(const TerrainMaterial&) const = default;
};

// What changed in a terrain since its last take_changes().
struct TerrainChanges {
    std::vector<CellCoord> tiles; // added, removed or changed (maybe twice, maybe gone)
    bool all = false;              // everything may have changed
};

class Terrain {
public:
    // One tile: samples() * samples() heights, row by row along +z, each
    // row along +x: sample (i, j) is at the cell's corner plus
    // (i, j) * spacing(), at heights[j * samples() + i].
    using Heights = std::vector<float>;

    Terrain() = default;
    // Tiles `tile_size` metres a side instead of one world cell (the far
    // ground's coarse levels, giant/world/far_ground.h). Positive. With
    // `holes`, a height may be NaN: no ground there.
    explicit Terrain(double tile_size, bool holes = false);
    // A copy or a moved-to terrain is a new one: all changed, new identity.
    Terrain(const Terrain& other);
    Terrain(Terrain&& other) noexcept;
    Terrain& operator=(const Terrain& other);
    Terrain& operator=(Terrain&& other) noexcept;
    ~Terrain() = default;

    u32 samples() const { return m_samples; }
    double tile_size() const { return m_tile_size; }
    double spacing() const { return m_tile_size / static_cast<double>(m_samples); }
    // The tile a point on the ground (x, z) is in.
    CellCoord tile_of(double x, double z) const;
    // Only while there are no tiles; false (changing nothing) otherwise or
    // when `samples` is not valid_terrain_samples.
    bool set_samples(u32 samples);

    bool empty() const { return m_tiles.empty(); }
    usize size() const { return m_tiles.size(); }
    const Heights* find(CellCoord cell) const;
    // The tile, to change in place: marks it changed, so call it only to change it.
    Heights* modify(CellCoord cell);
    // Adds or replaces a tile. `heights` must hold samples()^2 finite
    // values within k_max_terrain_height (asserted: callers check).
    void set(CellCoord cell, Heights heights);
    // Puts back a tile exactly as it was: `heights` with the `version` they
    // had then (an undo; anything else would break "equal versions are
    // equal heights"). A version of 0 takes a new one.
    void restore(CellCoord cell, Heights heights, u64 version);
    // The removed tile's heights (none if it had no tile).
    std::optional<Heights> remove(CellCoord cell);
    void clear();
    const std::unordered_map<CellCoord, Heights, CellCoordHash>& tiles() const { return m_tiles; }
    // The tile's version: new (unique across every terrain) each time it is
    // set or modified, kept by copies; 0 for no tile. Equal versions are
    // equal heights, so "unchanged since" is one compare.
    u64 version(CellCoord cell) const;

    // The surface at a point on the ground (x, z), exactly as the finest
    // mesh draws it (each square of samples is two triangles, split from
    // its lowest to its highest corner), or none where there is no tile
    // (a tile's far edge, where no neighbour is, counts as the tile's).
    std::optional<double> height_at(double x, double z) const;
    // The surface's height at whole-number sample coordinates from the world
    // origin (sample (i, j) of cell (x, z) is (x * samples() + i,
    // z * samples() + j)): from the tile it is in, or, when there is none,
    // from the sample before it on x, on z, or on both, if that one's tile
    // exists (so a tile with no neighbour repeats its last row and column).
    // None where neither is. As seen from `home`'s mesh, a tile that touches
    // home only at a corner, with neither tile between them there, does not
    // count (each keeps its own corner: they share no edge to crack along).
    // Otherwise it depends on (i, j) only, so every tile meeting at a point
    // sees the same height there.
    std::optional<float> sample(i64 i, i64 j, const CellCoord* home = nullptr) const;

    // Hands over what changed since the last call. A terrain made, copied
    // or assigned starts as "all changed".
    TerrainChanges take_changes();
    // The same, kept apart for a second follower (the physics, which must
    // not take the meshes' changes).
    TerrainChanges take_physics_changes();
    // About how many tiles changed since then (all of them after "all").
    usize untaken_changes() const { return m_changes.all ? m_tiles.size() : m_changes.tiles.size(); }
    // Tells terrains apart (a copy or an assignment is a new one).
    u64 identity() const { return m_identity.value; }
    // Counts every change (a tile set, modified, restored or removed, or all
    // cleared): a follower that does not take_changes (they go to one
    // follower) looks over the tiles only when it moved, with identity().
    u64 edits() const { return m_edits; }

    TerrainMaterial material;

private:
    void changed(CellCoord cell);

    struct Identity {
        Identity();
        Identity(const Identity&) : Identity() {}
        Identity& operator=(const Identity&);
        u64 value = 0;
    };

    double m_tile_size = k_cell_size;
    bool m_holes = false;
    u32 m_samples = k_default_terrain_samples;
    std::unordered_map<CellCoord, Heights, CellCoordHash> m_tiles;
    std::unordered_map<CellCoord, u64, CellCoordHash> m_versions; // the same cells
    TerrainChanges m_changes{{}, true};
    TerrainChanges m_physics_changes{{}, true};
    u64 m_edits = 0;
    Identity m_identity;
};

// Rolling hills from fractal noise, for making ground quickly (the AI and
// tests; sculpting and imported height maps come with the terrain editor).
// The same settings give the same heights on every machine.
struct HillsSettings {
    float base = 0.0f;       // metres: the average height
    float amplitude = 20.0f; // metres: the largest rise above or dip below base
    float size = 400.0f;     // metres: the width of the biggest hills
    u32 octaves = 4;         // 1 to 8: each adds hills half as big and half as high
    u32 seed = 0;
};
float hills_height(const HillsSettings& hills, double x, double z);

} // namespace giant::world
