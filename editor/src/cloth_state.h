// The editor's cloth: cloth entities (world::Shape::cloth) blown about by
// the weather's wind (giant/sim/cloth.h), each drawn as its own skinned
// mesh in the renderer whose joints are the sheet's points
// (render::Renderer::add_skinned_mesh), posed when it moves.
//
// Each visible cloth entity is a sheet hanging from its entity's origin
// (its rotation turns it; its scale is unused). Its mesh is a grid of the
// sheet's points, both faces (cloth is seen from either side), each vertex
// moved by its own point alone. Meshes are made a few a frame (making one
// uploads it), and an entity draws nothing until its mesh is loaded. At
// most k_max_cloths are drawn; the rest wait (lowest ids first).
//
// View state, as the animation: nothing of it is saved or undoable. The
// simulation runs whether or not the physics plays (cloth blows in the
// editor's view too), and only near the camera (sim::ClothSettingsSim).
#pragma once

#include "giant/core/jobs.h"
#include "giant/core/wind.h"
#include "giant/geometry/mesh.h"
#include "giant/render/renderer.h"
#include "giant/sim/cloth.h"
#include "giant/world/scene.h"

#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace giant::editor {

// The sheet's mesh: its points at rest, front (+z) and back faces, each
// vertex weighted wholly to its point (joint = point index).
geometry::SkinnedTriangleMesh cloth_mesh(const sim::Cloths::Sheet& sheet);

class ClothState {
public:
    static constexpr usize k_max_cloths = 256;

    // Without a renderer (tests) sheets move but nothing draws them.
    explicit ClothState(render::Renderer* renderer);
    ~ClothState(); // the meshes leave the renderer
    ClothState(const ClothState&) = delete;
    ClothState& operator=(const ClothState&) = delete;

    // The scene's changes followed (sheets added, changed, moved, gone):
    // before the scene frame is built, with what it will take
    // (Scene::untaken_changes), so no cell is built with a mesh its entity
    // should no longer draw.
    void follow(const world::Scene& scene, const world::SceneChanges& changed);
    // Each frame: time moves on under `wind`, meshes are made (at most
    // meshes_per_frame) and those that moved posed.
    void update(const WindState& wind, const DVec3& camera, double seconds);
    u32 meshes_per_frame = 4;

    // What draws each cloth entity (the scene frame's cloth meshes), and
    // which to draw again (taken by the frame).
    const std::unordered_map<world::EntityId, render::MeshId>& meshes() const { return m_drawn; }
    std::vector<world::EntityId> take_redraw();

    sim::Cloths& cloths() { return m_cloths; }
    const sim::Cloths& cloths() const { return m_cloths; }
    usize waiting() const; // cloth entities with no mesh yet
    u64 poses() const { return m_poses; } // handed to the renderer, ever

private:
    struct Drawn {
        render::MeshId mesh = render::k_no_mesh;
        float width = 0.0f;  // the sheet it was made for
        float height = 0.0f;
        u64 sent = 0;        // the sheet's version last posed
        bool ready = false;  // loaded and posed: drawn
    };
    void follow_one(const world::Scene& scene, world::EntityId id);
    void drop(world::EntityId id);

    render::Renderer* m_renderer;
    JobSystem m_jobs{3};
    sim::Cloths m_cloths;
    std::set<world::EntityId> m_wanted;           // cloth entities shown
    std::map<world::EntityId, Drawn> m_made;      // with a mesh (made, maybe not loaded yet)
    std::unordered_map<world::EntityId, render::MeshId> m_drawn;
    std::vector<world::EntityId> m_redraw;
    std::vector<Mat4> m_skin; // scratch
    u64 m_scene = 0;          // identity as last followed
    u64 m_poses = 0;
};

} // namespace giant::editor
