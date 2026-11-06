#include "giant/world/far_ground.h"

#include "giant/core/assert.h"

#include <cmath>
#include <limits>

namespace giant::world {

namespace {
i32 floor_div(i32 a, i32 b) {
    const i32 q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}
} // namespace

double far_tile_size(u32 level) {
    double size = k_cell_size;
    for (u32 l = 0; l < level; ++l) size *= k_far_branch;
    return size;
}

double far_spacing(u32 level) { return far_tile_size(level) / k_far_samples; }

CellCoord far_chunk_of_cell(CellCoord cell, u32 level) {
    for (u32 l = 0; l < level; ++l) cell = far_parent(cell);
    return cell;
}

CellCoord far_parent(CellCoord chunk) { return {floor_div(chunk.x, k_far_branch), floor_div(chunk.z, k_far_branch)}; }

void far_fill(u32 level, CellCoord chunk, CellCoord tile_coord, const Terrain::Heights* tile, u32 samples,
              Terrain::Heights& chunk_heights) {
    GIANT_ASSERT(level >= 1 && level <= k_far_levels, "far ground levels are 1 and 2");
    GIANT_ASSERT(far_parent(tile_coord) == chunk, "the tile is in the chunk");
    GIANT_ASSERT(chunk_heights.size() == usize{k_far_samples} * k_far_samples, "a chunk's heights");
    GIANT_ASSERT(tile == nullptr || tile->size() == usize{samples} * samples, "the tile's heights");
    constexpr u32 per = k_far_samples / k_far_branch; // the chunk's samples a finer tile side holds
    // Finer samples between two of the chunk's: the finer tile's side over `per`.
    const u32 step = samples / per;
    GIANT_ASSERT(step * per == samples, "a finer tile's samples divide into the chunk's");
    const u32 tx = static_cast<u32>(tile_coord.x - chunk.x * k_far_branch);
    const u32 tz = static_cast<u32>(tile_coord.z - chunk.z * k_far_branch);
    for (u32 b = 0; b < per; ++b) {
        for (u32 a = 0; a < per; ++a) {
            const usize at = usize{tz * per + b} * k_far_samples + (tx * per + a);
            chunk_heights[at] = tile != nullptr ? (*tile)[usize{b * step} * samples + a * step]
                                                : std::numeric_limits<float>::quiet_NaN();
        }
    }
}

} // namespace giant::world
