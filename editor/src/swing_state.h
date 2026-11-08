// The editor's swinging things: entities with swing settings on
// (world::SwingSettings: lanterns, signs, lamps on chains) swung about
// their pivots by the weather's wind (giant/sim/swing.h), and knocked by
// shots and blasts. Each is drawn turned about its pivot (scene_frame's
// `swings`), moved in place in its cell, never rebuilt, and kept out of the
// voxel world (render::Instance::moves).
//
// The box the wind pushes on is the entity's scale for primitives, its
// pieces' for a kit and its mesh's size for a mesh (once loaded; a kit's
// when its entity changes or meshes load, not when the kit is edited).
//
// View state, as the cloth: nothing of it is saved or undoable. It runs
// whether or not the physics plays, near the camera only
// (sim::SwingSettingsSim). While the physics poses an entity (a dynamic
// body, simulating), that pose wins.
#pragma once

#include "mesh_library.h"

#include "giant/core/wind.h"
#include "giant/render/renderer.h"
#include "giant/sim/swing.h"
#include "giant/world/scene.h"

#include <set>
#include <unordered_map>
#include <vector>

namespace giant::editor {

// The box (metres) the wind pushes on for the entity: see above.
Vec3 swing_size(const world::Scene& scene, const world::Entity& e, const render::Renderer* renderer,
                const MeshLibrary* meshes);

class SwingState {
public:
    // Without a renderer or meshes (tests), meshes swing as boxes of their scale.
    SwingState(const render::Renderer* renderer, const MeshLibrary* meshes);

    // The scene's changes followed (swings added, changed, moved, gone):
    // before the scene frame is built, with what it will take
    // (Scene::untaken_changes), so it draws them where they now hang.
    void follow(const world::Scene& scene, const world::SceneChanges& changed);
    // Each frame: time moves on under `wind`.
    void update(const WindState& wind, const DVec3& camera, double seconds);

    // Where each is drawn, and those to move (taken by the frame).
    const std::unordered_map<world::EntityId, world::Transform>& poses() const { return m_swings.poses(); }
    std::vector<world::EntityId> take_moved() { return m_swings.take_moved(); }

    sim::Swings& swings() { return m_swings; }
    const sim::Swings& swings() const { return m_swings; }

private:
    void follow_one(const world::Scene& scene, world::EntityId id);

    const render::Renderer* m_renderer;
    const MeshLibrary* m_meshes;
    sim::Swings m_swings;
    std::set<world::EntityId> m_meshed; // mesh and kit entities: their size follows the meshes loading
    u64 m_scene = 0;                    // identity as last followed
    u64 m_meshes_version = 0;
};

} // namespace giant::editor
