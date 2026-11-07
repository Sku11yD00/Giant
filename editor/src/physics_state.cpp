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

PhysicsState::PhysicsState()
    : m_jobs(physics_workers()), m_physics(m_jobs), m_bodies(m_physics), m_forest(m_physics) {}

void PhysicsState::notice_scene(const world::Scene& scene) {
    if (scene.identity() == m_scene) return;
    m_scene = scene.identity();
    // The scene's bodies and the trunks all go: the world cleared whole
    // (off the frame) when they are all it holds, the trunks' ids forgotten.
    if (m_bodies.clear(m_forest.bodies())) {
        m_forest.forget();
    } else {
        m_forest.clear();
    }
}

void PhysicsState::set_simulating(bool on) {
    if (on == m_on) return;
    m_on = on;
    if (!on) {
        m_bodies.reset_poses();
        m_forest.reset();
    }
}

void PhysicsState::feed_forest(const world::Scene& scene, const ScatterPlacer& scatter, const DVec3& camera) {
    notice_scene(scene);
    m_camera = camera;
    // The trees of a cell's placement, in point order.
    const auto trees_of = [](const ScatterPlacer::Cell& placed) {
        std::vector<sim::TreeSpot> out;
        const ScatterPlacer::Layers& layers = *placed.layers;
        for (u32 k = 0; k < placed.points.size(); ++k) {
            const world::ScatterPoint& p = placed.points[k];
            if (p.layer >= layers.size() || p.item >= layers[p.layer].items.size()) continue;
            const world::ScatterItem& item = layers[p.layer].items[p.item];
            if (!item.tree) continue;
            const float scale = p.transform.scale.x; // uniform
            sim::TreeSpot& s = out.emplace_back();
            s.point = k;
            s.at = p.transform;
            s.trunk_radius = item.tree->trunk_radius * scale;
            s.height = item.tree->height * scale;
            s.buried = item.sink * scale;
            const float strength = item.tree->strength > 0.0f ? item.tree->strength : world::k_default_tree_strength;
            s.strength = strength * scale * scale;
        }
        return out;
    };
    u64 looked = 0;
    // Within the budget (one cell at least).
    const auto fits = [&](const ScatterPlacer::Cell& placed) {
        return looked == 0 || looked + placed.points.size() <= points_per_feed;
    };
    const auto give = [&](world::CellCoord c, const ScatterPlacer::Cell& placed) {
        m_forest.set_cell(c, placed.serial, trees_of(placed));
        looked += placed.points.size();
    };
    // Cells known whose placement went, or changed.
    for (const world::CellCoord c : m_forest.cells()) {
        const ScatterPlacer::Cell* placed = scatter.find(c);
        if (placed == nullptr) {
            m_forest.remove_cell(c);
        } else if (placed->serial != m_forest.serial(c)) {
            if (m_forest.wants(c, camera) && fits(*placed)) {
                give(c, *placed);
            } else {
                m_forest.remove_cell(c); // given again when in reach
            }
        }
    }
    // Cells in reach not given yet, nearest first.
    const double reach = m_forest.settings().radius + world::k_cell_size;
    const world::CellCoord lo = world::cell_of({camera.x - reach, 0.0, camera.z - reach});
    const world::CellCoord hi = world::cell_of({camera.x + reach, 0.0, camera.z + reach});
    std::vector<std::pair<double, world::CellCoord>> wanted;
    for (i32 z = lo.z; z <= hi.z; ++z) {
        for (i32 x = lo.x; x <= hi.x; ++x) {
            const world::CellCoord c{x, z};
            if (!m_forest.wants(c, camera)) continue;
            const ScatterPlacer::Cell* placed = scatter.find(c);
            if (placed == nullptr || placed->serial == m_forest.serial(c)) continue;
            const double dx = (x + 0.5) * world::k_cell_size - camera.x;
            const double dz = (z + 0.5) * world::k_cell_size - camera.z;
            wanted.push_back({dx * dx + dz * dz, c});
        }
    }
    std::sort(wanted.begin(), wanted.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : (a.second.z != b.second.z ? a.second.z < b.second.z : a.second.x < b.second.x);
    });
    for (const auto& [d, c] : wanted) {
        const ScatterPlacer::Cell& placed = *scatter.find(c);
        if (!fits(placed)) break;
        give(c, placed);
    }
}

void PhysicsState::update(world::Scene& scene, const world::SceneChanges& changed, double seconds) {
    notice_scene(scene);
    m_bodies.sync(scene, changed.ids, changed.all);
    m_forest.sync(m_camera);
    if (!m_on) return;
    m_physics.step(seconds);
    m_bodies.update_poses();
    m_forest.after_step();
}

PhysicsState::Impact PhysicsState::impact(const DVec3& point, float radius, float impulse) {
    Impact out;
    out.loosened = m_bodies.impact(point, radius, impulse);
    out.felled = m_forest.impact(point, radius, impulse);
    return out;
}

void PhysicsState::flush(world::Scene& scene) {
    notice_scene(scene);
    if (take_frame_changes) {
        const world::SceneChanges taken = take_frame_changes();
        m_bodies.sync(scene, taken.ids, taken.all);
    }
    const world::SceneChanges& untaken = scene.untaken_changes();
    m_bodies.flush(scene, untaken.ids, untaken.all);
    m_forest.flush(m_camera);
}

void PhysicsState::catch_up(world::Scene& scene) {
    notice_scene(scene);
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
        m_forest.after_step();
    }
    return steps;
}

} // namespace giant::editor
