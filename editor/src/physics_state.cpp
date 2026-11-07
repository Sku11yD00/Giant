#include "physics_state.h"

#include <algorithm>
#include <thread>

namespace giant::editor {

namespace {

// Physics gets its own workers (the other systems keep theirs): half the
// cores, two to four.
u32 physics_workers() {
    const u32 cores = std::max(1u, std::thread::hardware_concurrency());
    return std::clamp(cores / 2u, 2u, 4u);
}

} // namespace

PhysicsState::PhysicsState() : m_jobs(physics_workers()), m_physics(m_jobs), m_bodies(m_physics) {}

void PhysicsState::set_simulating(bool on) {
    if (on == m_on) return;
    m_on = on;
    if (!on) m_bodies.reset_poses();
}

void PhysicsState::update(world::Scene& scene, const world::SceneChanges& changed, double seconds) {
    m_bodies.sync(scene, changed.ids, changed.all);
    if (!m_on) return;
    m_physics.step(seconds);
    m_bodies.update_poses();
}

void PhysicsState::flush(world::Scene& scene) {
    if (take_frame_changes) {
        const world::SceneChanges taken = take_frame_changes();
        m_bodies.sync(scene, taken.ids, taken.all);
    }
    const world::SceneChanges& untaken = scene.untaken_changes();
    m_bodies.flush(scene, untaken.ids, untaken.all);
}

void PhysicsState::catch_up(world::Scene& scene) {
    if (take_frame_changes) {
        const world::SceneChanges taken = take_frame_changes();
        m_bodies.sync(scene, taken.ids, taken.all);
    }
    const world::SceneChanges& untaken = scene.untaken_changes();
    m_bodies.sync(scene, untaken.ids, untaken.all);
}

u32 PhysicsState::run_steps(u32 steps) {
    for (u32 i = 0; i < steps; ++i) {
        m_physics.step(sim::PhysicsWorld::k_step_seconds);
        m_bodies.update_poses();
    }
    return steps;
}

} // namespace giant::editor
