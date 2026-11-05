// The clipmap: which bricks of the shared voxel world are kept, around the
// camera. Level 0 has the finest voxels; each level up doubles the voxel
// size and so covers twice as far, out to the horizon. Each level is a
// window of `bricks_per_axis` bricks on every axis, centred on the camera
// and moved in whole bricks, so a brick's place in the level's table
// (slot: its coordinate modulo the window, a ring buffer on each axis) never
// changes while it stays in the window: moving the camera only fills the
// slabs that come into view, never the whole level.
//
// Brick coordinates are a level's grid: brick (x, y, z) of level L covers
// [x, x + 1) * brick_size(L) metres on x, and so on, in world space.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"

#include <vector>

namespace giant::voxel {

struct BrickCoord {
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;
    constexpr bool operator==(const BrickCoord&) const = default;
};

// Bricks [min, max) of one level.
struct BrickBox {
    u32 level = 0;
    BrickCoord min;
    BrickCoord max;
    u64 count() const;
};

struct ClipmapDesc {
    u32 levels = 11;                // 0.25 m to 256 m voxels: level 10 reaches about 30 km out
    float voxel_size = 0.25f;       // level 0, metres
    i32 bricks_per_axis = 32;       // per level and axis (a power of two)
};

class Clipmap {
public:
    explicit Clipmap(const ClipmapDesc& desc = {});

    const ClipmapDesc& desc() const { return m_desc; }
    u32 levels() const { return m_desc.levels; }
    double voxel_size(u32 level) const;
    double brick_size(u32 level) const { return voxel_size(level) * 8.0; }

    // Moves every level's window to stay around `camera` and returns the
    // bricks that came into view (every window, the first time and after
    // reset()). On each axis a window moves once the camera's brick is two
    // or more from where a centred window would put it, so standing on a
    // brick border does not refill slabs.
    std::vector<BrickBox> recenter(const DVec3& camera);
    void reset() { m_placed = false; }
    bool placed() const { return m_placed; }

    BrickCoord origin(u32 level) const { return m_origin[level]; } // the window's minimum brick
    bool contains(u32 level, const BrickCoord& brick) const;
    // The brick's slot in its level's table, in [0, bricks_per_axis^3).
    u32 slot(u32 level, const BrickCoord& brick) const;
    u32 slots_per_level() const;

    // The brick holding a world position.
    BrickCoord brick_at(u32 level, const DVec3& position) const;
    // The bricks of `level` that a world-space box touches, clipped to the
    // window; empty (count 0) when it misses.
    BrickBox bricks_touching(u32 level, const DVec3& min, const DVec3& max) const;
    DVec3 brick_min(u32 level, const BrickCoord& brick) const;

private:
    ClipmapDesc m_desc;
    std::vector<BrickCoord> m_origin;
    bool m_placed = false;
};

BrickBox intersect(const BrickBox& a, const BrickBox& b); // same level

} // namespace giant::voxel
