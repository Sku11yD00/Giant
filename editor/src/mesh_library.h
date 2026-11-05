// Imported meshes, by file path. The scene stores only the path of each
// mesh an entity draws; the library imports a file the first time it is
// needed (glTF/GLB to cluster LOD) and hands it to the renderer.
//
// Importing is an asset load, not a scene change: it is not undoable and
// the mesh stays loaded for the session (until reload).
#pragma once

#include "giant/core/types.h"
#include "giant/render/renderer.h"

#include <memory>
#include <unordered_map>
#include <string>
#include <string_view>
#include <vector>

namespace giant::editor {

class MeshLibrary {
public:
    struct Mesh {
        std::string path;                      // normalised, as entities store it
        render::MeshId id = render::k_no_mesh; // k_no_mesh without a renderer
        u32 triangles = 0;                     // in the file
        u32 clusters = 0;
        u32 levels = 0;
        u32 coarsest_triangles = 0; // drawn when the whole mesh is tiny
        float import_seconds = 0.0f;
        std::vector<std::string> warnings;
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

    // "a/./b\\c.glb" -> "a/b/c.glb": one spelling per file, forward slashes.
    static std::string normalize(std::string_view path);

private:
    Loaded import(const std::string& path, Mesh* replace);

    render::Renderer* m_renderer;
    std::vector<std::unique_ptr<Mesh>> m_meshes; // stable addresses
    std::unordered_map<std::string, Mesh*> m_by_path;
};

} // namespace giant::editor
