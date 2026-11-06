// Hands the scene to the renderer as plain data (the renderer never sees
// world types).
#pragma once

#include "mesh_library.h"

#include "giant/render/renderer.h"
#include "giant/world/scene.h"

#include <unordered_map>
#include <vector>

namespace giant::editor {

// What the renderer draws of a scene, kept from frame to frame by world
// cell (world::k_cell_size): a frame rebuilds only the cells whose
// entities changed (world::Scene::take_changes), and those of a kit that
// changed; importing a mesh, a new scene or a material falling out of use
// rebuild them all. Equal materials share one table entry, so every
// placement of a kit, and every entity with the same look, sends its
// material once.
struct SceneFrame {
    // For the renderer: instances cell after cell (render::CellRange says
    // where each starts), the material table, the decals.
    std::vector<render::Instance> instances;
    std::vector<render::CellRange> cells;
    std::vector<render::Material> materials;
    std::vector<render::Decal> decals;
    u32 cells_rebuilt = 0; // by the last scene_frame

    struct MaterialKey {
        u32 bits[8];
        bool operator==(const MaterialKey&) const = default;
    };
    struct MaterialHash {
        usize operator()(const MaterialKey& k) const;
    };
    std::unordered_map<MaterialKey, u32, MaterialHash> lookup; // material -> index
    std::vector<u32> users; // instances using each material
    std::vector<u32> scratch;

    struct Cell {
        std::vector<world::EntityId> ids; // the entities in it
        std::vector<render::Instance> instances;
        std::vector<render::Decal> decals;
        u64 version = 0; // new each rebuild (unique across cells)
        bool dirty = true;
    };
    std::unordered_map<world::CellCoord, Cell, world::CellCoordHash> grid;
    std::unordered_map<world::EntityId, world::CellCoord> where; // entity -> its cell
    std::vector<world::CellCoord> order;                          // cells, as sent
    std::vector<world::Kit> kits;                                 // as last built
    u64 scene_identity = 0;
    u64 meshes_version = ~u64{0};
    u64 next_version = 1;
};

// The scene's visible entities, with kits expanded into their pieces. Mesh
// entities and pieces whose file is not imported are left out, as are kit
// entities whose kit is missing. Takes the scene's changes. Within a cell,
// instances and decals go in id (creation) order; cells go by z, then x.
void scene_frame(world::Scene& scene, const render::Renderer& renderer, const MeshLibrary& meshes,
                 SceneFrame& out);

// A kit piece placed by a kit entity: the piece's transform inside the
// entity's (exact for uniform scales; a non-uniform entity scale stretches
// rotated pieces along the piece's own axes).
world::Transform place(const world::Transform& parent, const world::Transform& child);

render::Lighting scene_lighting(const world::Lighting& lighting);

} // namespace giant::editor
