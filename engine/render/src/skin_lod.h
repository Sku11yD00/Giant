// Skinned meshes with levels of detail: how far a pose moves the mesh from
// bind (its LOD spheres, at bind, widen by that), and the finest depth a
// view can draw (the pose pass skips finer ones).
#pragma once
#include "giant/core/types.h"
#include "giant/geometry/clusters.h"

#include <span>
#include <vector>

namespace giant::render {

// The most any vertex moves from bind under a pose: `matrices` holds 12
// floats per joint (3 rows of the 3x4, bind space to the mesh's),
// `joint_spheres` 4 per joint (the bind-space sphere around the vertices
// the joint moves; radius < 0: none). A vertex blends its joints with
// weights summing to one, so it moves no further than its farthest joint
// would move it alone: |(A - I) c + t| + |A - I| r over the joint's sphere,
// with the Frobenius norm over the spectral one (larger, so safe). A huge
// finite number when a number is not finite (never NaN in the cut).
// It bounds where things go, not how a pose bends them: errors stay the
// bind pose's (a stretched joint or a coarse triangle across a bent joint
// can look further off than the threshold, though the cut stays whole).
float pose_reach(std::span<const float> matrices, std::span<const float> joint_spheres);

// Per depth: a sphere around every non-terminal group's simplified sphere
// of that depth, with their largest error (error < 0: the depth has none).
// A depth's clusters are drawn only where its group projects over the
// threshold; terminal groups are always drawn where reached (and always
// posed), so they are left out (their error is infinite: they would make
// every depth look needed at any distance).
std::vector<geometry::LodBounds> depth_bounds(const geometry::ClusterMesh& mesh);

// The finest depth `view` may draw of non-terminal groups: the first whose
// depth_bounds project over the threshold; the coarsest depth when none
// does. Finer depths have no non-terminal cluster in the cut
// (select_clusters); the frustum is not looked at.
u32 finest_depth(std::span<const geometry::LodBounds> depths, const geometry::LodView& view);

} // namespace giant::render
