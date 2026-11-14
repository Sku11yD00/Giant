// Characters in the light (characters plan C2): which of the frame's
// occluders (FrameInput::occluders) the shading uses, as GPU records.
// Those that are not finite, have no size, are past k_occluder_distance or
// whose reach is out of view are left out; of the rest, the nearest
// k_max_occluders, nearest first. occluders.slang then lists, per 8 x 8
// pixel tile, those whose reach (a sphere round the capsule and
// k_occluder_reach radii round it) touches the box of what the tile shows
// (the first 64 in this order: the nearest), and dims each
// pixel's ambient light by them.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/geometry/clusters.h"
#include "giant/render/renderer.h"
#include "gpu_layout.h"

#include <span>
#include <utility>
#include <vector>

namespace giant::render {

// How far round a capsule (in its radii, from its axis) it shades.
inline constexpr float k_occluder_reach = 4.0f;

// `frustum` holds camera-relative positions; `view` turns them to view
// space (Renderer's view_from_orientation: the records are in view space);
// `scratch` is kept between frames (no allocation once it has grown).
void pick_occluders(std::span<const Occluder> occluders, const DVec3& camera, const Mat4& view,
                    const geometry::Frustum& frustum, std::vector<gpu::GpuOccluder>& out,
                    std::vector<std::pair<float, u32>>& scratch);

} // namespace giant::render
