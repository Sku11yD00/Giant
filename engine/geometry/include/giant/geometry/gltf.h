// glTF 2.0 import (.gltf with its buffers, or .glb), through cgltf.
//
// The default scene (or every root node) is flattened into one triangle
// mesh in the file's own space: node transforms applied, mirrored nodes
// rewound, normals transformed (or made flat when the file has none, as the
// glTF spec asks). Units are metres and +Y is up, as in the engine.
//
// Read for now: positions, normals and indices of triangle, strip and fan
// primitives. Materials, textures, skins and morph targets are skipped
// with a warning; compressed files (Draco, meshopt) are refused.
#pragma once

#include "giant/core/types.h"
#include "giant/geometry/mesh.h"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace giant::geometry {

// Larger files are refused rather than run out of memory.
inline constexpr u64 k_max_import_triangles = 16'000'000;
inline constexpr u64 k_max_import_file_bytes = u64{2} << 30; // 2 GiB

struct ImportResult {
    TriangleMesh mesh;
    std::vector<std::string> warnings; // things that were skipped
    std::string error;                 // empty on success
    bool ok() const { return error.empty(); }
};

// Reads a .gltf (buffers resolved next to it) or .glb file.
ImportResult import_gltf_file(const std::filesystem::path& path);

// Reads a .glb, or a .gltf whose buffers are embedded data URIs.
ImportResult import_gltf_memory(std::span<const u8> data);

} // namespace giant::geometry
