// Decal binning: which decals touch which screen tiles, worked out on the
// CPU each frame so the shading pass tests only the few decals over each
// pixel. A decal's box is projected to a screen rectangle (all of the
// screen when the box reaches behind the near plane) and listed in every
// tile it covers, in the frame's order (later decals draw on top).
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/geometry/clusters.h"
#include "giant/render/renderer.h"
#include "gpu_layout.h"

#include <span>
#include <vector>

namespace giant::render {

inline constexpr u32 k_decal_tile = 16;              // pixels
inline constexpr u32 k_max_decals = 1u << 16;        // per frame, in view
inline constexpr u32 k_max_decal_entries = 1u << 21; // (tile, decal) pairs

struct DecalBins {
    std::vector<gpu::Decal> decals; // those in view, in frame order
    std::vector<u32> tiles;         // per tile: first entry, count
    std::vector<u32> entries;       // decal indices, tile by tile
    u32 columns = 0;
    u32 rows = 0;
    bool overflow = false; // a limit was reached: some decals were left out
};

// The GPU record of one decal (camera-relative).
gpu::Decal to_gpu(const Decal& decal, const DVec3& camera);

// `view_proj` takes camera-relative positions to clip space.
void bin_decals(std::span<const Decal> decals, const DVec3& camera, const Mat4& view_proj,
                const geometry::Frustum& frustum, u32 width, u32 height, DecalBins& out);

} // namespace giant::render
