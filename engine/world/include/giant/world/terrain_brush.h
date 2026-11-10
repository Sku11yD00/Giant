// Sculpting the terrain with a round brush: raise, lower, smooth or
// flatten the heights around a point. Only tiles that exist are changed
// (a brush does not make ground).
#pragma once

#include "giant/core/types.h"
#include "giant/world/terrain.h"

#include <functional>
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

// One dab of a paint brush at (x, z): raises (or, `erase`, lowers) the
// painted weight of TerrainLayer `layer` (1 to k_paintable_layers) at the
// samples within its radius, of the tiles there are, by brush.strength (0
// to 1: the share of the way to full, or to none, at the centre) times
// brush_weight. Adds the cells whose paint changed to `changed` (each once
// a dab; one whose weights all stay is not touched).
void apply_paint(Terrain& terrain, const Brush& brush, u32 layer, bool erase, double x, double z,
                 std::vector<CellCoord>& changed);

// A dent pressed into the ground: a crater's bowl with a rim thrown up
// round it, or (stretched along `yaw`) a footprint.
struct Dent {
    double radius = 4.0;  // metres: the bowl's (stretched: half its width)
    float depth = 1.0f;   // metres at the centre
    float rim = 0.25f;    // the rim's height as a share of depth, out to 1.5 radius
    float stretch = 1.0f; // half its length over half its width, 1 or more (1: round)
    float yaw = 0.0f;     // radians: its length's way, from +x toward +z
};

// The farthest a dent reaches (larger: not a dent).
inline constexpr double k_max_dent_reach = 4096.0;
// The ground's reach of a dent: metres from its centre (rim included; 0: not a dent).
double dent_reach(const Dent& dent);
// How much a dent lowers (positive: raises) the ground `du` metres along
// its length and `dv` across it.
float dent_offset(const Dent& dent, double du, double dv);
// Presses a dent in at (x, z) into the tiles there are, adding the cells
// changed to `changed` (each once; `before`, when given, is told each one
// just before it changes). Heights stay within k_max_terrain_height. The
// work is the samples in the dent's own (turned) bounds.
void apply_dent(Terrain& terrain, const Dent& dent, double x, double z, std::vector<CellCoord>& changed,
                const std::function<void(CellCoord)>& before = {});

// The dabs a stroke through `points` makes: the points, with more between
// them so dabs are at most a quarter radius apart (a fast drag leaves no gaps).
// A stroke that would make more than `max_dabs` gives max_dabs + 1 (cut
// short): the caller refuses it.
std::vector<GroundPoint> brush_dabs(const Brush& brush, std::span<const GroundPoint> points,
                                    usize max_dabs = std::numeric_limits<usize>::max());

} // namespace giant::world
