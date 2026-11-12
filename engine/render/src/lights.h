// Local light binning: which lamps reach which clusters of the view, worked
// out on the CPU each frame (like decals) so shading walks only the few
// lights near each pixel. The screen is cut into k_light_columns x
// k_light_rows tiles and the view's depth into k_light_slices slices,
// spaced evenly in log depth from k_light_near to k_light_far. A light's
// bounding sphere (a spot's is around its cone) gives the tiles and slices
// it may touch; lights are listed nearest first, so when a cluster or the
// frame is full the furthest are the ones left out.
//
// Far lamps are merged (a lamp LOD): a lamp whose sphere is at least
// k_light_merge_distance away and whose radius is at most half a grid cell
// k_light_merge_angle of its distance across (rounded down to a power of two
// metres) is summed with the others in its cell into one point light at
// their power-weighted centre, reaching as far as they all do, its source
// as wide as they are spread, as bright as to light the ground under them
// as much as they all did (a spot as the point light sending as much
// light down its cone). Lamps are grouped by distance alone, in view or not, so turning
// the view never changes a group. A far town's thousand lamps so take a
// few dozen lights, not hundreds in each of its clusters; merged lamps
// cast no shadows (a casting lamp that far sharing its cell loses its
// shadow; one alone in its cell keeps it).
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/geometry/clusters.h"
#include "giant/render/renderer.h"
#include "gpu_layout.h"

#include <span>
#include <vector>

namespace giant::render {

inline constexpr u32 k_light_columns = 16;
inline constexpr u32 k_light_rows = 9;
inline constexpr u32 k_light_slices = 24;
inline constexpr u32 k_light_clusters = k_light_columns * k_light_rows * k_light_slices;
inline constexpr float k_light_near = 0.5f;  // metres: slice 0 reaches from the camera past this
inline constexpr float k_light_far = 4096.0f; // metres: lights wholly further are left out
inline constexpr float k_light_merge_distance = 256.0f; // metres: lamps further may merge
inline constexpr float k_light_merge_angle = 1.0f / 32.0f; // a merge cell across, per metre away
inline constexpr u32 k_merged_light = ~0u; // LightBins::sources of a merged light

struct LightBins {
    std::vector<gpu::Light> lights; // those shaded, nearest first
    std::vector<u32> sources;       // each one's index in the frame's lights (k_merged_light: merged)
    std::vector<u32> clusters;      // per cluster: first entry, count
    std::vector<u32> entries;       // light indices, cluster by cluster
    bool overflow = false;          // a limit was reached: some lights were left out somewhere
    u32 lamps = 0;                  // lamps the lights shaded stand for (each merged one counted)
    u32 merged = 0;                 // of the lights, far lamps merged into one
};

// The depth slices: slice = floor(log2(depth / k_light_near) * light_slice_scale()).
float light_slice_scale();
// The slice a view depth (metres in front of the camera) falls in
// (k_light_slices: beyond the last).
u32 light_slice(float depth);

// The GPU record of one light (camera-relative).
gpu::Light to_gpu(const LocalLight& light, const DVec3& camera);

// The sphere around what a light reaches, camera-relative (a spot's around
// its cone).
void light_bounds(const LocalLight& light, const DVec3& camera, Vec3& center, float& radius);

// Cluster index of tile (x, y) in slice z.
constexpr u32 light_cluster(u32 x, u32 y, u32 z) { return (z * k_light_rows + y) * k_light_columns + x; }

void bin_lights(std::span<const LocalLight> lights, const View& view, float aspect,
                const geometry::Frustum& frustum, LightBins& out);

} // namespace giant::render
