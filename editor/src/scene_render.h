// Hands the scene to the renderer as plain data (the renderer never sees
// world types).
#pragma once

#include "mesh_library.h"
#include "terrain_meshes.h"

#include "giant/render/renderer.h"
#include "giant/sim/forest.h"
#include "giant/sim/scene_physics.h"
#include "giant/world/scene.h"

#include <array>
#include <map>
#include <span>
#include <unordered_map>
#include <vector>

namespace giant::editor {

class FarGround;
class ScatterPlacer;
class TextureLibrary;

// What the renderer draws of a scene, kept from frame to frame by world
// cell (world::k_cell_size): a frame rebuilds only the cells whose
// entities changed (world::Scene::take_changes), and those of a kit that
// changed; importing a mesh, a new scene or a material falling out of use
// rebuild them all (as does the material table filling up with unused
// entries). Equal materials share one table entry, so every
// placement of a kit, and every entity with the same look, sends its
// material once.
struct SceneFrame {
    // For the renderer: instances by cell (render::CellRange says where
    // each cell's are; a cell keeps its place from frame to frame, with
    // room to grow, and places no cell has are holes: instances with no
    // mesh, sent as cells of their own), the material table, the decals.
    std::vector<render::Instance> instances;
    std::vector<render::CellRange> cells;
    std::vector<render::Material> materials;
    std::vector<render::Decal> decals;
    u32 cells_rebuilt = 0; // by the last scene_frame
    // What scene_frame took from the scene (it takes them all), kept for the
    // other followers of its changes (the physics), who take it in turn
    // with take_for_followers. Past a million ids it says "all" instead.
    world::SceneChanges for_followers;
    world::SceneChanges take_for_followers() {
        world::SceneChanges out = std::move(for_followers);
        for_followers = {};
        return out;
    }
    // The same changes kept again for the animation (AnimationState), taken
    // apart from the physics' (which may take its own early).
    world::SceneChanges for_animation;
    world::SceneChanges take_for_animation() {
        world::SceneChanges out = std::move(for_animation);
        for_animation = {};
        return out;
    }
    // Cells rebuilt whole a frame for fallen trees coming or going (the rest
    // wait for the next frames; moving ones are made again in place).
    u32 fallen_rebuilds_per_frame = 8;
    usize scatter_room = 0; // instances reserved once for scatter to grow into (0: not yet)
    // Where entities are drawn instead of where the scene has them (the
    // physics' poses), during a scene_frame call.
    const std::unordered_map<world::EntityId, world::Transform>* poses = nullptr;
    // Breakable kits' loose pieces, drawn where they are (likewise).
    const std::unordered_map<world::EntityId, std::vector<sim::PiecePose>>* piece_poses = nullptr;
    // Fallen scatter trees, drawn where they lie (likewise).
    const sim::FallenTrees* fallen = nullptr;
    // Animated skinned entities' own posed meshes, drawn instead of their
    // files' (likewise).
    const std::unordered_map<world::EntityId, render::MeshId>* skinned = nullptr;
    // Cloth entities' meshes (likewise).
    const std::unordered_map<world::EntityId, render::MeshId>* cloth = nullptr;
    // Where swinging entities are drawn when the physics does not pose them (likewise).
    const std::unordered_map<world::EntityId, world::Transform>* swings = nullptr;
    const std::unordered_map<world::EntityId, world::Transform>* walkers = nullptr;

    // What an entity drew in its cell (instances from `first`, decals from
    // `decal_first`, within the cell's), so moving it can be done in place.
    struct Slot {
        world::EntityId id = 0;
        u32 first = 0;
        u32 count = 0;
        u32 decal_first = 0;
        u32 decals = 0;
    };

    // Material image files are cooked by `textures` (set by the owner; null:
    // materials draw their plain values), each material's textures kept
    // beside it (TextureLibrary refs: base colour, normal, roughness-metal,
    // occlusion) and their renderer ids filled in as they become ready.
    TextureLibrary* textures = nullptr;
    std::vector<std::array<u32, 4>> material_textures; // per material
    // Each material's own two_sided and alpha_cutoff (materials[i] has them
    // only while the material is used: each kind is a raster pass).
    struct MaterialKind {
        bool two_sided = false;
        float alpha_cutoff = 0.0f;
    };
    std::vector<MaterialKind> material_kind;
    u64 textures_version = ~u64{0};
    // Each material's maps' refs, by address, within one scene_frame call.
    std::unordered_map<const world::MaterialMaps*, std::array<u32, 4>> maps_seen;

    struct MaterialKey {
        // The values, the texture refs, 1 for the ground's terrain material,
        // then two-sided, translucency and alpha cutoff.
        u32 bits[19];
        bool operator==(const MaterialKey&) const = default;
    };
    struct MaterialHash {
        usize operator()(const MaterialKey& k) const;
    };
    std::unordered_map<MaterialKey, u32, MaterialHash> lookup; // material -> index
    std::vector<u32> users; // instances using each material
    // Materials nothing uses stay in the table (they come back when used
    // again) until more than this many, and more than the used ones.
    usize compact_unused_over = 256;
    std::vector<u32> scratch;

    struct Cell {
        std::vector<world::EntityId> ids; // the entities in it
        std::vector<render::Instance> instances;
        std::vector<render::Decal> decals;
        u64 version = 0; // new each rebuild, unique in the process (0: never built)
        bool dirty = true;
        // Only its ground changed (a terrain tile's or far ground's mesh, the
        // ground's look): its first ground_count instances are made again
        // in place, the rest kept, and the renderer told only they changed.
        bool ground_dirty = false;
        u32 ground_count = 0;
        // Only its fallen trees moved (the same ones): the instances drawing
        // them, from fallen_first (after every other), are made again in place.
        bool fallen_dirty = false;
        u32 fallen_first = 0;
        u32 fallen_decals = 0;          // its decals before the fallen trees'
        std::vector<u32> fallen_points; // the fallen points drawn, in order
        // Its entities by id, as last built, and those only moved since
        // (posed or swung: moved again in place, if they can be).
        std::vector<Slot> slots;
        std::vector<world::EntityId> moved;
        u64 prev_version = 0; // what an in-place change was made from (0: a full rebuild)
        u32 changed_first = 0; // and the instances it changed
        u32 changed_count = 0;
        bool ground = false; // it draws a terrain tile (so it stays without entities)
        bool scattered = false; // it draws scatter placements (so it stays too)
        // Its place in `instances`: capacity instances from first, the
        // ones past its own holes. placed: it has one (maybe of none).
        u32 first = 0;
        u32 capacity = 0;
        bool placed = false;
    };
    std::unordered_map<world::CellCoord, Cell, world::CellCoordHash> grid;
    std::unordered_map<world::EntityId, world::CellCoord> where; // entity -> its cell
    std::vector<world::CellCoord> order;                          // cells by z, then x (decals go so)
    // Holes in `instances` no cell has (first -> size), joined, to find
    // room in; and the same holes as sent to the renderer, as cells of
    // their own: one piece per place a cell left (first -> size, version),
    // kept until a cell takes it. Past holes_allowed (and half the list)
    // holes in all, or with the list past render::k_max_instances, it is
    // laid out again end to end.
    struct Free {
        u32 count = 0;
        u64 version = 0;
    };
    std::map<u32, Free> free;
    std::map<u32, Free> pieces;
    usize holes_allowed = 4096;
    std::vector<world::CellCoord> rebuilt; // scratch
    std::vector<world::Kit> kits;                                 // as last built
    u64 scene_identity = 0;
    u64 meshes_version = ~u64{0};
    world::TerrainMaterial terrain_material; // as last built
    // The ground's layers (render::FrameInput::terrain_layers), made again
    // each scene_frame call, with their TextureLibrary refs (base colour,
    // normal, roughness-metal, none).
    std::vector<render::TerrainLayer> terrain_layers;
    std::vector<std::array<u32, 4>> terrain_layer_textures;
};

// The scene's visible entities, with kits expanded into their pieces, and
// the terrain's tiles whose meshes `terrain` has built (each first in its
// cell, render::Instance::always_mesh). Mesh entities and pieces whose file
// is not imported are left out, as are kit entities whose kit is missing.
// Takes the scene's changes (and `terrain`'s changed cells: call
// TerrainMeshes::update first). Within a cell, instances and decals go in
// id (creation) order; decals of cells go by z, then x. With `far` (updated
// first, and taking `terrain`'s changed cells itself), a tile draws only
// where the far ground says, and its chunks are drawn from their corner cells.
// With `scatter` (updated first), each cell's scatter placements draw after
// its entities, as their items (kits expanded); a cell is rebuilt when its
// placements change (ScatterPlacer::take_changed) or a kit changes.
// With `poses`, an entity in it is drawn there (position and rotation; it
// stays in the cell the scene puts it in), and the cells of the entities in
// `redraw` (whose pose changed or went) are rebuilt. With `piece_poses`, a
// kit's loose pieces are drawn where they are (world transforms), the rest
// of it where it stands. With `fallen`, a cell's fallen scatter trees (of
// the placement it draws: the same serial) are drawn where they lie, and
// the cells in `redraw_cells` are rebuilt. With `skinned`, a mesh entity in
// it draws that mesh (its animated copy) instead of its file's; entities
// that came or went from it are in `redraw`. With `cloth`, a cloth entity
// in it draws that mesh (ClothState); one not in it draws nothing. The
// entities in `moved` are drawn elsewhere now (a pose moved or went) and
// nothing else about them changed: they are moved in place where they can
// be (the renderer told only of those instances), else their cells are
// rebuilt. With `swings`, a swinging entity the physics does not pose is
// drawn there (SwingState); swinging entities' instances are marked
// render::Instance::moves (kept out of the voxel world). With `walkers`,
// a walking entity (WalkerState) is drawn there, marked moving too.
void scene_frame(world::Scene& scene, const render::Renderer& renderer, const MeshLibrary& meshes,
                 SceneFrame& out, TerrainMeshes* terrain = nullptr, FarGround* far = nullptr,
                 ScatterPlacer* scatter = nullptr,
                 const std::unordered_map<world::EntityId, world::Transform>* poses = nullptr,
                 std::span<const world::EntityId> redraw = {},
                 const std::unordered_map<world::EntityId, std::vector<sim::PiecePose>>* piece_poses = nullptr,
                 const sim::FallenTrees* fallen = nullptr, std::span<const world::CellCoord> redraw_cells = {},
                 const std::unordered_map<world::EntityId, render::MeshId>* skinned = nullptr,
                 const std::unordered_map<world::EntityId, render::MeshId>* cloth = nullptr,
                 std::span<const world::EntityId> moved = {},
                 const std::unordered_map<world::EntityId, world::Transform>* swings = nullptr,
                 const std::unordered_map<world::EntityId, world::Transform>* walkers = nullptr);

// A kit piece placed by a kit entity: the piece's transform inside the
// entity's (exact for uniform scales; a non-uniform entity scale stretches
// rotated pieces along the piece's own axes).
world::Transform place(const world::Transform& parent, const world::Transform& child);

render::Lighting scene_lighting(const world::Lighting& lighting);

// Keeps the renderer's painted terrain tiles (Renderer::set_terrain_paint)
// the terrain's: once a frame, only the tiles whose paint changed (all of
// them for a new terrain or another spacing).
class TerrainPaintSync {
public:
    void update(world::Terrain& terrain, render::Renderer& renderer);
    usize sent() const { return m_sent; } // tiles handed over by the last update

private:
    u64 m_identity = 0;
    double m_tile_size = 0.0;
    u32 m_samples = 0;
    usize m_sent = 0;
};

} // namespace giant::editor
