// Sculpting the terrain with a round brush: raise, lower, smooth or
// flatten the heights around a point. Only tiles that exist are changed
// (a brush does not make ground).
#pragma once

#include "giant/core/types.h"
#include "giant/world/terrain.h"

#include <limits>
#include <span>
#include <vector>

namespace giant::world {

enum class BrushOp : u8 { raise, lower, smooth, flatten };

// A point on the ground, metres along x and z.
struct GroundPoint {
    double x = 0.0;
    double z = 0.0;
};

struct Brush {
    BrushOp op = BrushOp::raise;
    double radius = 8.0; // metres, on the ground
    // raise and lower: metres at the centre; smooth and flatten: how far
    // towards the target (0 to 1) at the centre.
    float strength = 1.0f;
    // The outer part of the radius (0 to 1) over which the brush fades out.
    float falloff = 0.5f;
    float height = 0.0f; // flatten: the height to flatten to
};

// How much the brush acts at `distance` metres from its centre: 1 inside
// radius * (1 - falloff), easing to 0 at the radius.
float brush_weight(const Brush& brush, double distance);

// One dab of the brush at (x, z): changes the heights of the tiles within
// its radius, adds the cells changed to `changed` (each once a dab; a tile
// whose heights all stay as they were is not touched).
// Heights stay within k_max_terrain_height. Smooth averages each sample
// with its 8 neighbours (across tiles) as they were before the dab.
void apply_brush(Terrain& terrain, const Brush& brush, double x, double z, std::vector<CellCoord>& changed);

// The dabs a stroke through `points` makes: the points, with more between
// them so dabs are at most a quarter radius apart (a fast drag leaves no gaps).
// A stroke that would make more than `max_dabs` gives max_dabs + 1 (cut
// short): the caller refuses it.
std::vector<GroundPoint> brush_dabs(const Brush& brush, std::span<const GroundPoint> points,
                                    usize max_dabs = std::numeric_limits<usize>::max());

} // namespace giant::world
