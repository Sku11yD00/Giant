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
// Running water carries its waves along for this long, from two starts half
// of it apart, each faded in and out (water.slang: a flow map's two phases).
inline constexpr float k_water_flow_period = 2.0f;
// A surface's slope is kept within this (WaterVertex::slope_*), packed to 16 bits each.
inline constexpr float k_water_slope_most = 10.0f;
// The surf: a wave runs in to the shore every this many seconds.
inline constexpr float k_water_surf_period = 6.0f;

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

// Running water's clock at `seconds`: seconds into its cycle (0 to
// k_water_flow_period; worked out in double, so it keeps its precision late on).
float water_flow_clock(double seconds);
// A clock folded at `period` (> 0) the same way: seconds into its cycle.
float water_clock(double seconds, float period);
// How high the surf runs under wind `velocity` (m/s): a calm's ripples
// lap (0.15) to a strong wind's breakers (1, from 8 m/s over the water).
float water_surf(const Vec3& velocity);

// A look as the GPU holds it, its numbers kept in range (a bad one: the default's).
gpu::WaterLook pack_water_look(const WaterLook& look);

// Whether any of the near plane lies under the surface `level_relative`
// metres above (or, negative, below) the camera: the view looking along
// `orientation` (-Z, +Y up), `tan_y` the tangent of half its vertical field
// of view, `aspect` its width over its height, `near_plane` metres out.
// (Its lowest corner: the plane is flat.)
bool near_plane_under(const Quat& orientation, float tan_y, float aspect, float near_plane, double level_relative);

// The waves at `seconds` under wind `velocity` (m/s; the sea's waves grow
// with it, longest about where a sea under that wind peaks, with a little
// ripple when calm), camera-relative to `camera`: each wave's phase holds
// the camera's place, worked out in double, so far from the origin the
// pattern keeps its millimetres and does not jump as the camera moves.
// Deep water's waves: a wave k radians a metre long moves at sqrt(g k) / k.
std::array<gpu::WaterWave, k_water_waves> water_waves(const Vec3& velocity, double seconds, const DVec3& camera);

} // namespace giant::render
