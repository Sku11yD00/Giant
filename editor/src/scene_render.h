// Hands the scene to the renderer as plain data (the renderer never sees
// world types).
#pragma once

#include "mesh_library.h"

#include "giant/render/renderer.h"
#include "giant/world/scene.h"

#include <unordered_map>
#include <vector>

namespace giant::editor {

// What the renderer draws of a scene, rebuilt each frame (the vectors are
// reused). Equal materials share one table entry, so every placement of a
// kit, and every entity with the same look, sends its material once.
struct SceneFrame {
    std::vector<render::Instance> instances;
    std::vector<render::Material> materials;
    std::vector<render::Decal> decals;

    struct MaterialKey {
        u32 bits[8];
        bool operator==(const MaterialKey&) const = default;
    };
    struct MaterialHash {
        usize operator()(const MaterialKey& k) const;
    };
    std::unordered_map<MaterialKey, u32, MaterialHash> lookup; // material -> index
};

// The scene's visible entities, with kits expanded into their pieces. Mesh
// entities and pieces whose file is not imported are left out, as are kit
// entities whose kit is missing.
void scene_frame(const world::Scene& scene, const render::Renderer& renderer, const MeshLibrary& meshes,
                 SceneFrame& out);

// A kit piece placed by a kit entity: the piece's transform inside the
// entity's (exact for uniform scales; a non-uniform entity scale stretches
// rotated pieces along the piece's own axes).
world::Transform place(const world::Transform& parent, const world::Transform& child);

render::Lighting scene_lighting(const world::Lighting& lighting);

} // namespace giant::editor
