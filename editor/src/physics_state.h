// The editor's physics: the scene's bodies (giant/sim), and simulating them.
//
// The bodies follow the scene always (a bounded share of the work each
// frame), so raycasts answer and simulating starts at once. Simulating is
// view state, as a play button: while on, time moves the dynamic entities,
// which are drawn where they are (poses beside the scene, never written
// into it: undo, saving and the scene's own transforms keep where things
// were put). Turning it off puts every one back where the scene has it.
// Edits while simulating follow (an entity moved is put there, at rest;
// one added joins in).
#pragma once

#include "giant/core/jobs.h"
#include "giant/sim/physics.h"
#include "giant/sim/scene_physics.h"
#include "giant/world/scene.h"

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
    // `steps` fixed steps at once (step_physics); the poses follow.
    u32 run_steps(u32 steps);

    // Where simulating has moved dynamic entities to (the renderer draws
    // them there); and which to draw again (taken by the frame).
    const std::unordered_map<world::EntityId, world::Transform>& poses() const { return m_bodies.poses(); }
    std::vector<world::EntityId> take_redraw() { return m_bodies.take_redraw(); }

    sim::PhysicsWorld& world() { return m_physics; }
    const sim::PhysicsWorld& world() const { return m_physics; }
    const sim::ScenePhysics& bodies() const { return m_bodies; }

private:
    JobSystem m_jobs; // first: outlives the physics world
    sim::PhysicsWorld m_physics;
    sim::ScenePhysics m_bodies;
    bool m_on = false;
};

} // namespace giant::editor
