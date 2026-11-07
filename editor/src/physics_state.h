// The editor's physics: the scene's bodies (giant/sim), and simulating them.
//
// The bodies follow the scene always (a bounded share of the work each
// frame), so raycasts answer and simulating starts at once. Simulating is
// view state, as a play button: while on, time moves the dynamic entities,
// which are drawn where they are (poses beside the scene, never written
// into it: undo, saving and the scene's own transforms keep where things
// were put). Turning it off puts every one back where the scene has it.
// Edits while simulating follow (an entity moved is put there, at rest;
// one added joins in). The scatter trees near the camera stand as trunks
// (sim::Forest, fed from the scatter placer); struck while simulating they
// topple, and stopping stands them up again. What comes down throws up
// dust (sim::Dust), drawn by the renderer; far from the camera pieces come
// down as rubble and trees are laid down, with no bodies.
#pragma once

#include "giant/core/jobs.h"
#include "giant/sim/dust.h"
#include "giant/sim/forest.h"
#include "giant/sim/physics.h"
#include "giant/sim/scene_physics.h"
#include "giant/world/scene.h"
#include "scatter_placer.h"

#include <functional>
#include <unordered_map>
#include <vector>

namespace giant::editor {

class PhysicsState {
public:
    PhysicsState();

    bool simulating() const { return m_on; }
    // Starts, or stops (every moved entity drawn where the scene has it again).
    void set_simulating(bool on);
    // Each frame before update, once the placer is updated: the scatter
    // trees in reach of `camera` given to the forest (a bounded share of
    // the cells placed anew: at most points_per_feed points looked at, or one cell).
    void feed_forest(const world::Scene& scene, const ScatterPlacer& scatter, const DVec3& camera);
    u32 points_per_feed = 1u << 15;
    // Each frame, with what the scene changed since the last call: brings
    // the bodies in line (within the budgets) and, while simulating, moves
    // time on.
    void update(world::Scene& scene, const world::SceneChanges& changed, double seconds);
    // Brings the bodies wholly in line now (a raycast's or step_physics's
    // answer): with what the frame took from the scene and has not handed
    // over yet (take_frame_changes, when set), and what the scene has not
    // handed out.
    void flush(world::Scene& scene);
    std::function<world::SceneChanges()> take_frame_changes;
    // As flush, within the budgets of a frame (starting play): what the
    // scene changed is queued and a share made; the rest follows.
    void catch_up(world::Scene& scene);
    // `steps` fixed steps at once (step_physics); the poses follow.
    u32 run_steps(u32 steps);

    // Where simulating has moved dynamic entities to (the renderer draws
    // them there); and which to draw again (taken by the frame).
    const std::unordered_map<world::EntityId, world::Transform>& poses() const { return m_bodies.poses(); }
    std::vector<world::EntityId> take_redraw() { return m_bodies.take_redraw(); }
    const std::unordered_map<world::EntityId, std::vector<sim::PiecePose>>& piece_poses() const {
        return m_bodies.piece_poses();
    }
    // A blow (impact command): breakable kits' pieces in reach come loose,
    // and trees in reach topple (the scatter cells in the blow given to the
    // forest first, at most points_per_impact points: far ones are laid down).
    struct Impact {
        usize loosened = 0; // kit pieces
        usize felled = 0;   // trees
    };
    Impact impact(const DVec3& point, float radius, float impulse);
    u32 points_per_impact = 1u << 16;
    // The dust in the air (drawn by the renderer), moved by `wind` (m/s:
    // the weather's, set each frame) while simulating; stopping clears it.
    const sim::Dust& dust() const { return m_dust; }
    Vec3 wind{};
    // Fallen scatter trees (drawn where they lie), and the cells to draw again (taken by the frame).
    const sim::FallenTrees& fallen_trees() const { return m_forest.fallen(); }
    std::vector<world::CellCoord> take_tree_redraw() { return m_forest.take_redraw(); }
    const sim::Forest& forest() const { return m_forest; }

    sim::PhysicsWorld& world() { return m_physics; }
    const sim::PhysicsWorld& world() const { return m_physics; }
    const sim::ScenePhysics& bodies() const { return m_bodies; }

private:
    // A new scene: the forest's trunks go before the scene's bodies clear the world.
    void notice_scene(const world::Scene& scene);
    // The trees of a cell's placement, in point order.
    static std::vector<sim::TreeSpot> trees_of(const ScatterPlacer::Cell& placed);

    JobSystem m_jobs; // first: outlives the physics world
    sim::Dust m_dust; // before what emits into it
    sim::PhysicsWorld m_physics;
    sim::ScenePhysics m_bodies;
    sim::Forest m_forest; // after the world: its trunks go first
    u64 m_scene = 0;
    DVec3 m_camera{}; // as last fed
    const ScatterPlacer* m_scatter = nullptr; // as last fed (it outlives this)
    bool m_on = false;
};

} // namespace giant::editor
