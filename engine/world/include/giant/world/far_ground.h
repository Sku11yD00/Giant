// The far ground: coarse copies of the terrain for what lies past the cells
// loaded in full, so a world's ground reaches the horizon without holding
// every tile. Two levels, each a world::Terrain of bigger, coarser tiles
// ("chunks"):
//   level 1: 512 m chunks (8 x 8 cells), 32 x 32 heights 16 m apart;
//   level 2: 4096 m chunks (8 x 8 level-1 chunks), 32 x 32 heights 128 m apart.
// A chunk's heights are the finer level's at its sample points (no
// averaging, so the levels agree where they share a point). Where a cell
// has no tile, its samples are NaN: no ground.
//
// A streamed world keeps them as text beside its cells
// (ground/<level>/<x>_<z>.ground.json), written by save_world; the editor
// reads the ones near the camera (editor/src/far_ground.h).
#pragma once

#include "giant/core/types.h"
#include "giant/world/cell.h"
#include "giant/world/scene.h"
#include "giant/world/terrain.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace giant::world {

constexpr u32 k_far_levels = 2;
constexpr u32 k_far_samples = 32; // a chunk's heights a side
constexpr i32 k_far_branch = 8;   // tiles of the level below a chunk side

// Metres a side of a level's chunk (level 0: a cell).
double far_tile_size(u32 level);
// Metres between a level's heights (level 1: 16, level 2: 128).
double far_spacing(u32 level);
// The level's chunk a cell (level 0 coordinates) is in.
CellCoord far_chunk_of_cell(CellCoord cell, u32 level);
// The chunk of `level` that a chunk of `level - 1` is in.
CellCoord far_parent(CellCoord chunk);

// A chunk's heights (k_far_samples^2, row by row along +z), as text: rows
// of numbers in the shortest exact form, "-" for none.
std::string save_far_chunk(u32 level, CellCoord chunk, const Terrain::Heights& heights);
// Replaces `out` only on success; strict (the level and chunk must be the
// file's, every height within k_max_terrain_height or "-").
std::optional<LoadError> load_far_chunk(std::string_view text, u32 level, CellCoord chunk, Terrain::Heights& out);
// "<x>_<z>.ground.json".
std::string far_chunk_file_name(CellCoord chunk);

// Writes a finer tile's heights into its block of `chunk_heights` (a chunk of
// the next level up), at the chunk's sample points: `tile` is the finer
// level's tile `tile_coord` (`samples` a side, null: no ground there, NaN).
// The finer level is `level - 1` (0: terrain cells).
void far_fill(u32 level, CellCoord chunk, CellCoord tile_coord, const Terrain::Heights* tile, u32 samples,
              Terrain::Heights& chunk_heights);

} // namespace giant::world
