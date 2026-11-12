// Water's surfaces for a frame (FrameInput::water_*): checked and packed
// for the GPU, camera-relative (shaders/water.slang draws them), and the
// waves blown by the wind that ripple them. Plain CPU work, kept apart
// from the renderer so it is tested on its own.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/render/renderer.h"
#include "gpu_layout.h"

#include <array>
#include <span>
#include <vector>

namespace giant::render {

inline constexpr u32 k_water_waves = 8;

struct WaterBatch {
    std::vector<gpu::WaterVertex> vertices;
    std::vector<u32> indices; // three a triangle, each a vertex of `vertices`
    std::vector<gpu::WaterLook> looks; // at least one (the default)
    u32 dropped = 0;       // triangles left out: an index past the vertices, a bad number
    bool overflow = false; // past k_max_water_vertices or k_max_water_triangles
};

// The frame's water, camera-relative to `camera`. Vertices past
// k_max_water_vertices and triangles past k_max_water_triangles are left
// out (overflow); a triangle naming a vertex not there, or a vertex with a
// bad number, is dropped; a look's numbers are kept in range (a bad one:
// the default's); a vertex's look past the looks given takes the default.
WaterBatch pack_water(std::span<const WaterVertex> vertices, std::span<const u32> indices,
                      std::span<const WaterLook> looks, const DVec3& camera);

// The waves at `seconds` under wind `velocity` (m/s; the sea's waves grow
// with it, longest about where a sea under that wind peaks, with a little
// ripple when calm), camera-relative to `camera`: each wave's phase holds
// the camera's place, worked out in double, so far from the origin the
// pattern keeps its millimetres and does not jump as the camera moves.
// Deep water's waves: a wave k radians a metre long moves at sqrt(g k) / k.
std::array<gpu::WaterWave, k_water_waves> water_waves(const Vec3& velocity, double seconds, const DVec3& camera);

} // namespace giant::render
