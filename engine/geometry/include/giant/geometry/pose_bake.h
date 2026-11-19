// A skinned mesh held in one pose, as a plain mesh: what a crowd's distant
// people are drawn with (crowds plan, CR1). Someone far off needs no
// skeleton of their own: a few poses of each character (standing, sitting,
// some steps of a walk) are baked once, each a small static mesh, and every
// distant person is one instance of one of them.
//
// Three steps, all plain data in and out, each fit for a worker thread:
// the character's bind mesh cut down to a few thousand triangles once
// (simplify_skinned: weights kept, so every pose is made from the same
// small mesh), that mesh moved into a pose (bake_pose: what the renderer's
// skinning does on the GPU, done here on the CPU), and the result given to
// build_clusters like any mesh.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/geometry/mesh.h"

#include <span>
#include <vector>

namespace giant::geometry {

// Morph targets added to bind vertices: each target's deltas times its
// weight onto the positions and normals (normals renormalised; +Y where
// one comes to nothing), as the renderer does before skinning. `weights`
// is one per target (fewer: the rest weigh 0); deltas naming a vertex out
// of range and weights that are not finite are left out.
void apply_morphs(std::span<Vertex> vertices, std::span<const MorphTarget> morphs, std::span<const float> weights);

// Where meshes meet: for each of `parts`, one flag per vertex, non-zero
// where another part has a vertex at the same place (the parts of one
// character, cut by material). Given to simplify_skinned as its locked
// vertices, the parts still meet after each is simplified alone. Places
// that are not numbers meet nothing.
std::vector<std::vector<u8>> seams_between(std::span<const std::span<const Vertex>> parts);

struct SimplifiedSkin {
    SkinnedTriangleMesh mesh; // no morph targets
    // How far (metres) the surface may have moved from the original's, as
    // the simplifier measured it (0: nothing was simplified). Its measure
    // counts turned normals a little too, so it reads somewhat over the
    // distance alone. It grows fast below a mesh's natural size: a person
    // of 11,000 triangles reads a few millimetres at 3,000 and over 0.1 m
    // at 1,000.
    float error = 0.0f;
    u32 source_triangles = 0;
    bool unlocked = false; // the locked vertices and the seams had to be let go to get near the target
};
// The mesh with about `triangles` triangles (at least 1 asked for; a mesh
// with no more is returned whole): simplification only drops vertices, so
// each one kept has its own place, normal, uv and weights, and the result
// poses as the original does. It is done in up to two steps: with vertices
// marked in `locked` (one per vertex, or empty) kept and attribute seams
// (uv, normal) kept; then, only when that left more than 1.25 times the
// target, with both let go and the smallest loose pieces dropped. Loose
// pieces (hair cards, lashes: triangles sharing no edge with the rest)
// come down like anything else, each to fewer triangles or to none. Where
// even that cannot reach the target, what it reached is returned. Vertices
// no triangle uses are dropped; degenerate triangles too. Empty when the input is (no triangle, weights not one per vertex,
// an index out of range). `morphs` of the input are not carried (apply
// them first: apply_morphs).
SimplifiedSkin simplify_skinned(std::span<const Vertex> vertices, std::span<const u32> indices,
                                std::span<const SkinWeights> weights, u32 triangles,
                                std::span<const u8> locked = {});

// The mesh in a pose: each vertex moved by its joints' skin matrices (one
// per joint, bind space to the mesh's space: anim::skin_matrices), as the
// renderer's skinning has a sound mesh (engine/render/shaders/skin.slang;
// vanishing or non-finite tangents and normals differ in detail):
// the place the weighted sum, the normal likewise and made unit (+Y where
// it comes to nothing), the tangent likewise and made perpendicular to it,
// the bitangent turned when the first weighted joint mirrors, a joint out
// of range left out, and a vertex whose place comes out not finite left
// where it was bound. The triangles are the input's. Morph targets are
// not applied (apply_morphs first).
TriangleMesh bake_pose(const SkinnedTriangleMesh& mesh, std::span<const Mat4> skin);

} // namespace giant::geometry
