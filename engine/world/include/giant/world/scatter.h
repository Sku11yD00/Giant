// Scatter: things strewn over the ground by rules (trees, rocks, bushes,
// grass), like Unreal's PCG. A layer says what to place (items, each a
// shape, mesh or kit, picked by weight) and where: points about `spacing`
// apart (a jittered grid, so no two are closer than spacing * (1 - jitter)),
// kept by density, a clumping noise, an area, and the ground's height and
// slope there. Placements are not saved: they are worked out from the
// rules and the terrain, cell by cell (k_cell_size), the same every time,
// so a cell is made again when its ground or the rules change, and a
// streamed world scatters only the ground it has loaded.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/world/cell.h"
#include "giant/world/scene.h"
#include "giant/world/terrain.h"

#include <optional>
#include <string>
#include <vector>

namespace giant::world {

// The layer types (ScatterItem, ScatterArea, ScatterLayer) are in scene.h,
// which holds a scene's layers.

// One placement: the layer's item (an index) and where it stands.
struct ScatterPoint {
    u32 layer = 0;
    u32 item = 0;
    Transform transform;
};

// The points the grid has in a cell: at most this many a layer.
usize scatter_candidates(const ScatterLayer& layer, CellCoord cell);

// Appends the layer's placements in `cell` (layer index `layer_index`),
// in a fixed order. The same rules, seed and ground there give the same
// placements, whatever else the terrain holds: it reads the ground within
// a few samples of the cell (its tile and the neighbours'). Nothing for a
// layer with no items, not visible, or a cell with no tile.
void scatter_cell(const ScatterLayer& layer, u32 layer_index, const Terrain& terrain, CellCoord cell,
                  std::vector<ScatterPoint>& out);

// The ground's unit normal at (x, z), from heights `step` metres either
// side (none where there is no ground).
std::optional<Vec3> ground_normal(const Terrain& terrain, double x, double z, double step);

// Why a layer cannot be used (names, spacing, items, ranges), or none.
std::optional<std::string> check_scatter_layer(const ScatterLayer& layer);

} // namespace giant::world
