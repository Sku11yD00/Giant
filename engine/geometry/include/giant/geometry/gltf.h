// glTF 2.0 import (.gltf with its buffers, or .glb), through cgltf.
//
// The default scene (or every root node) is flattened into one triangle
// mesh in the file's own space: node transforms applied, mirrored nodes
// rewound, normals transformed (or made flat when the file has none, as the
// glTF spec asks). Units are metres and +Y is up, as in the engine.
//
// Read for now: positions, normals, texture coordinates (TEXCOORD_0),
// tangents and indices of triangle, strip and fan primitives, and one skin
// with its skeleton and animation clips. Tangents the file lacks are made
// from the uvs (generate_tangents, mesh.h).
// Materials are read as their metallic-roughness values and images
// (ImportedMaterial; the mesh itself is one, so a drawer picks one of
// them); morph targets are read for skinned files only (else skipped with a
// warning); compressed files (Draco, meshopt) are refused.
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
//
// Morph targets (blend shapes) of a skinned file's meshes, its rigid parts'
// too, go to `skinned->mesh.morphs`: each target's position and normal
// moves at full weight, in bind space, on the vertices it moves (glTF's
// NORMAL deltas; flat-shaded meshes take their faces' turn), named by the
// mesh's targetNames (else "<mesh name>#<index>", "mesh#<index>" for an
// unnamed mesh, so two unnamed meshes' targets merge too) and merged by
// name over the file's meshes, so one weight moves a face and its teeth (a
// name repeated within one mesh: the first stands); the rest weight is the
// first non-zero default weight a node (else its mesh) gives (a skinned
// mesh shown by several nodes is read once: its first node's). Names are
// cut to k_max_morph_name_bytes. At most k_max_morph_targets (a mesh with
// over four times that is refused); tangent deltas are not read (the posed
// tangent is made perpendicular to the posed normal). `mesh`, the rest
// pose, is of the base shape: no morph weight applied.
#pragma once

#include "giant/anim/skeleton.h"
#include "giant/core/math.h"
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

// An image a material uses: a file beside the glTF (`path`, relative to its
// folder, forward slashes, never leaving it) or one inside it (`embedded`:
// its index in the file's images; read_gltf_image gives its bytes).
struct ImportedImage {
    std::string path; // UTF-8
    i32 embedded = -1;
    bool empty() const { return path.empty() && embedded < 0; }
};

// A glTF material as the engine draws it: metallic-roughness factors (base
// colour linear, emissive times KHR_materials_emissive_strength) and the
// images of its four maps, each read with TEXCOORD_0.
struct ImportedMaterial {
    std::string name;
    Vec3 base_color{1.0f, 1.0f, 1.0f};
    float alpha = 1.0f; // not drawn yet
    float roughness = 1.0f;
    float metallic = 1.0f;
    Vec3 emissive{};
    ImportedImage base_color_image;
    ImportedImage normal_image;
    ImportedImage roughness_metal_image; // glTF metallicRoughnessTexture: roughness G, metallic B
    bool names_roughness_metal = false;  // the file gives one (even if not read): its factors scale it
    ImportedImage occlusion_image;       // R
    u64 triangles = 0;                   // of the mesh drawn with it
};

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
    bool has_uvs = false;              // some primitive had texture coordinates (else all are 0)
    // The file's materials that the shown meshes use, most triangles first
    // (a primitive without one uses glTF's default, which is not listed:
    // its triangles are counted apart).
    std::vector<ImportedMaterial> materials;
    u64 default_material_triangles = 0;
    std::string error;                 // empty on success
    bool ok() const { return error.empty(); }
};

// Reads a .gltf (buffers resolved next to it, never outside its folder) or
// .glb file.
ImportResult import_gltf_file(const std::filesystem::path& path);

// Reads a .glb, or a .gltf whose buffers are embedded data URIs.
ImportResult import_gltf_memory(std::span<const u8> data);

// The bytes of image `index` inside a glTF file (a buffer view or a data
// URI; ImportedImage::embedded), reading the file again (for a worker that
// decodes it). False with why if it is not there or cannot be read.
bool read_gltf_image(const std::filesystem::path& file, u32 index, std::vector<u8>& out, std::string& error);
// The buffer files a glTF names (beside it, URIs decoded; data URIs left
// out), to tell when images inside them change. Empty if it names none or
// cannot be read.
std::vector<std::filesystem::path> gltf_buffer_files(const std::filesystem::path& file);

} // namespace giant::geometry
