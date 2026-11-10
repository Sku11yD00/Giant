// Imported meshes, by file path. The scene stores only the path of each
// mesh an entity draws; the library imports a file the first time it is
// needed (glTF/GLB to cluster LOD) and hands it to the renderer.
//
// Importing is an asset load, not a scene change: it is not undoable and
// the mesh stays loaded for the session (until reload).
//
// A skinned file (a character) is also kept as one: its skeleton, clips and
// bind-space clusters, which the animation state copies to the renderer for
// each entity it animates (AnimationState). The mesh itself is the file in
// its rest pose, as any other (what entities not animated draw).
#pragma once

#include "giant/anim/player.h"
#include "giant/core/types.h"
#include "giant/geometry/clusters.h"
#include "giant/render/renderer.h"
#include "giant/world/scene.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <unordered_map>
#include <string>
#include <string_view>
#include <vector>

namespace giant::editor {

class MeshLibrary {
public:
    // A skinned file's character, shared by every entity animating it (and
    // kept by them while a reload replaces it).
    struct Skinned {
        anim::Skeleton skeleton;
        std::vector<anim::Clip> clips; // names unique
        std::vector<float> joint_radius; // per joint, for anim::pose_bounds
        std::vector<anim::BoneCapsule> capsules; // hit shapes round the bones (joint space)
        std::vector<Mat4> rest_model;            // each joint's model matrix in the rest pose
        geometry::SkinnedClusterMesh clusters; // bind space, base level
        // The clip of that name, or anim::k_no_clip.
        u32 find_clip(std::string_view name) const;
    };
    struct Mesh {
        std::string path;                      // normalised, as entities store it
        render::MeshId id = render::k_no_mesh; // k_no_mesh without a renderer
        u32 triangles = 0;                     // in the file
        u32 clusters = 0;
        u32 levels = 0;
        u32 coarsest_triangles = 0; // drawn when the whole mesh is tiny
        float import_seconds = 0.0f;
        std::vector<std::string> warnings;
        bool has_uvs = false; // the file has texture coordinates (material maps draw only then)
        // The file's own look: its material with the most triangles (values,
        // and maps when it covers three quarters of the mesh: an image inside
        // the file named "<path>#image<N>", one beside it by its path).
        // Entities given this file take it, under the look fields given.
        // None if the file has no materials, or glTF's default covers most.
        std::optional<world::Material> material;
        u32 file_materials = 0; // materials the file's shown meshes use
        std::shared_ptr<const Skinned> skinned; // null: not skinned
    };
    struct Loaded {
        const Mesh* mesh = nullptr; // null on failure
        std::string error;
    };

    // Without a renderer (tests), meshes are imported and measured only.
    explicit MeshLibrary(render::Renderer* renderer) : m_renderer(renderer) {}
    ~MeshLibrary();
    MeshLibrary(const MeshLibrary&) = delete;
    MeshLibrary& operator=(const MeshLibrary&) = delete;

    // Imports the file unless it is loaded already. Failures are not kept,
    // so a fixed file can be tried again.
    Loaded load(std::string_view path);
    // Imports it again from disk (the file changed); the old one stays if
    // the new import fails.
    Loaded reload(std::string_view path);
    const Mesh* find(std::string_view path) const;
    // For a path already normalised (as entities store it): a hash lookup,
    // cheap enough to run per entity per frame.
    const Mesh* find_normalized(const std::string& path) const;
    const std::vector<std::unique_ptr<Mesh>>& meshes() const { return m_meshes; }
    // Changes whenever a mesh is imported or replaced.
    u64 version() const { return m_version; }

    // "a/./b\\c.glb" -> "a/b/c.glb": one spelling per file, forward slashes.
    static std::string normalize(std::string_view path);

private:
    Loaded import(const std::string& path, Mesh* replace);

    render::Renderer* m_renderer;
    std::vector<std::unique_ptr<Mesh>> m_meshes; // stable addresses
    std::unordered_map<std::string, Mesh*> m_by_path;
    u64 m_version = 0;
};

} // namespace giant::editor
