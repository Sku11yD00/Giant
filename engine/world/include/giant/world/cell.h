// World cells: the grid everything that draws, streams and is worked on
// together goes by.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"

namespace giant::world {

// The world is cut into square cells on the ground (x and z), k_cell_size
// metres a side, cell (0, 0) spanning [0, k_cell_size) on both. An entity
// belongs to the cell its position is in, whatever its size. What draws,
// streams and is worked on together goes by cell, so a change to one
// entity costs its cell's work, not the world's.
constexpr double k_cell_size = 64.0;

struct CellCoord {
    i32 x = 0;
    i32 z = 0;
    bool operator==(const CellCoord&) const = default;
};
struct CellCoordHash {
    usize operator()(const CellCoord& c) const;
};
// The cell a point is in (clamped to the range i32 holds).
CellCoord cell_of(const DVec3& position);

} // namespace giant::world
