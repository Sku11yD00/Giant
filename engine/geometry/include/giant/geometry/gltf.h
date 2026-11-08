// glTF 2.0 import (.gltf with its buffers, or .glb), through cgltf.
//
// The default scene (or every root node) is flattened into one triangle
// mesh in the file's own space: node transforms applied, mirrored nodes
// rewound, normals transformed (or made flat when the file has none, as the
// glTF spec asks). Units are metres and +Y is up, as in the engine.
//
// Read for now: positions, normals and indices of triangle, strip and fan
// primitives, and one skin with its skeleton and animation clips.
// Materials, textures and morph targets are skipped with a warning;
// compressed files (Draco, meshopt) are refused.
//
// Skinned files. When a mesh the scene shows is skinned, the file is read
// as one character: `mesh` is it in its rest pose (as if no clip played),
// and `skinned` the same triangles in bind space with each vertex's joints
// and weights (the four heaviest, of JOINTS_0/WEIGHTS_0 and _1), the
// skeleton and every animation's joint channels. Skinned meshes ignore
// their node's transform (glTF's rule); meshes that are not skinned but
// hang under a joint (a sword in a hand) move rigidly with it, and others
// with the first root joint. Nodes between two joints become joints too
// (unweighted), so their transforms and animation count. One skin only.
#pragma once

#include "giant/anim/skeleton.h"
#include "giant/core/types.h"
#include "giant/geometry/mesh.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace giant::geometry {

// Larger files are refused rather than run out of memory.
inline constexpr u64 k_max_import_triangles = 16'000'000;
inline constexpr u64 k_max_import_vertices = 3 * k_max_import_triangles; // read, over all nodes
inline constexpr u64 k_max_import_file_bytes = u64{2} << 30; // 2 GiB, buffers included

struct SkinnedImport {
    SkinnedTriangleMesh mesh;
    anim::Skeleton skeleton;
    std::vector<anim::Clip> clips; // names unique
    // Per joint: how far the vertices it moves lie from it when bound (0:
    // it moves none), for anim::pose_bounds.
    std::vector<float> joint_radius;
    // Hit shapes round the bones (anim::fit_capsules): what bullets meet.
    std::vector<anim::BoneCapsule> capsules;
};

struct ImportResult {
    TriangleMesh mesh;
    std::optional<SkinnedImport> skinned; // the file had a skin
    std::vector<std::string> warnings; // things that were skipped
    std::string error;                 // empty on success
    bool ok() const { return error.empty(); }
};

// Reads a .gltf (buffers resolved next to it, never outside its folder) or
// .glb file.
ImportResult import_gltf_file(const std::filesystem::path& path);

// Reads a .glb, or a .gltf whose buffers are embedded data URIs.
ImportResult import_gltf_memory(std::span<const u8> data);

} // namespace giant::geometry
