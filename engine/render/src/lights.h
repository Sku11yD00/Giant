// Local light binning: which lamps reach which clusters of the view, worked
// out on the CPU each frame (like decals) so shading walks only the few
// lights near each pixel. The screen is cut into k_light_columns x
// k_light_rows tiles and the view's depth into k_light_slices slices,
// spaced evenly in log depth from k_light_near to k_light_far. A light's
// bounding sphere (a spot's is around its cone) gives the tiles and slices
// it may touch; lights are listed nearest first, so when a cluster or the
// frame is full the furthest are the ones left out.
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

struct LightBins {
    std::vector<gpu::Light> lights; // those shaded, nearest first
    std::vector<u32> sources;       // each one's index in the frame's lights
    std::vector<u32> clusters;      // per cluster: first entry, count
    std::vector<u32> entries;       // light indices, cluster by cluster
    bool overflow = false;          // a limit was reached: some lights were left out somewhere
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
