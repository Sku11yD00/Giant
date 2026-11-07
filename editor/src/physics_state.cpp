#include "physics_state.h"

#include <algorithm>
#include <unordered_set>
#include <cmath>
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
    : m_jobs(physics_workers()), m_physics(m_jobs), m_bodies(m_physics), m_forest(m_physics) {
    m_bodies.set_dust(&m_dust);
    m_forest.set_dust(&m_dust);
}

std::vector<sim::TreeSpot> PhysicsState::trees_of(const ScatterPlacer::Cell& placed) {
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
}

void PhysicsState::give(world::CellCoord cell, const ScatterPlacer::Cell& placed) {
    const world::CellDamage* kept = m_scene_at != nullptr ? m_scene_at->damage_in(cell) : nullptr;
    m_forest.set_cell(cell, placed.serial, trees_of(placed), kept != nullptr ? kept->trees : std::vector<world::KeptTree>{});
}

void PhysicsState::follow_kept(const world::Scene& scene, u64* looked) {
    if (m_scatter == nullptr) return;
    const auto differs = [&](world::CellCoord c) {
        const world::CellDamage* kept = scene.damage_in(c);
        const std::vector<world::KeptTree>* had = m_forest.records(c);
        const usize now = kept != nullptr ? kept->trees.size() : 0;
        if (had == nullptr) return now > 0;
        return now == 0 ? !had->empty() : !(*had == kept->trees);
    };
    if (scene.damage_edits() != m_damage_edits) {
        m_damage_edits = scene.damage_edits();
        // Cells the forest knows: given again now (keep_damage, an undo: what
        // falls stops where it is, what was kept goes, the same frame).
        for (const world::CellCoord c : m_forest.cells()) {
            if (!differs(c)) continue;
            if (const ScatterPlacer::Cell* placed = m_scatter->find(c)) {
                give(c, *placed);
            } else {
                m_kept_waiting.insert(c);
            }
        }
        // Others (streamed in, or not placed yet): within the feed's budget, when placed.
        for (const auto& [c, d] : scene.damage()) {
            if (!d.trees.empty() && m_forest.records(c) == nullptr) m_kept_waiting.insert(c);
        }
    }
    std::erase_if(m_kept_waiting, [&](world::CellCoord c) {
        if (!differs(c)) return true;
        const ScatterPlacer::Cell* placed = m_scatter->find(c);
        if (placed == nullptr) return false; // given once it is placed
        if (looked != nullptr) {
            if (*looked > 0 && *looked + placed->points.size() > points_per_feed) return false;
            *looked += placed->points.size();
        }
        give(c, *placed);
        return true;
    });
}

void PhysicsState::notice_scene(const world::Scene& scene) {
    m_scene_at = &scene;
    if (scene.identity() == m_scene) return;
    m_scene = scene.identity();
    m_damage_edits = ~u64{0};
    m_kept_waiting.clear();
    m_dust.clear();
    m_ground.forget(); // another scene's ground
    m_restore_ground = false;
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
        m_dust.clear();
        m_restore_ground = true; // with the scene, at the next update
    }
}

void PhysicsState::feed_forest(const world::Scene& scene, const ScatterPlacer& scatter, const DVec3& camera) {
    notice_scene(scene);
    m_camera = camera;
    m_scatter = &scatter;
    m_bodies.set_camera(camera);
    u64 looked = 0;
    // Within the budget (one cell at least).
    const auto fits = [&](const ScatterPlacer::Cell& placed) {
        return looked == 0 || looked + placed.points.size() <= points_per_feed;
    };
    const auto give_cell = [&](world::CellCoord c, const ScatterPlacer::Cell& placed) {
        give(c, placed);
        looked += placed.points.size();
    };
    follow_kept(scene, &looked);
    // Cells known whose placement went, or changed (a cell with fallen
    // trees is given again wherever it is: they stay down where they stand).
    for (const world::CellCoord c : m_forest.cells()) {
        const ScatterPlacer::Cell* placed = scatter.find(c);
        if (placed == nullptr) {
            m_forest.remove_cell(c);
        } else if (placed->serial != m_forest.serial(c)) {
            if (m_forest.fallen_in(c) > 0 || (m_forest.wants(c, camera) && fits(*placed))) {
                give_cell(c, *placed);
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
        give_cell(c, placed);
    }
}

void PhysicsState::refresh_forest(const world::Scene& scene, const ScatterPlacer& scatter) {
    notice_scene(scene);
    for (const world::CellCoord c : m_forest.cells()) {
        const ScatterPlacer::Cell* placed = scatter.find(c);
        if (placed == nullptr || placed->serial == m_forest.serial(c) || m_forest.fallen_in(c) == 0) continue;
        give(c, *placed);
    }
}

void PhysicsState::restore_ground(world::Scene& scene) {
    if (!m_restore_ground) return;
    m_restore_ground = false;
    m_ground.restore(scene.terrain);
}

usize PhysicsState::dent(world::Scene& scene, const world::Dent& dent, const DVec3& at) {
    notice_scene(scene);
    restore_ground(scene); // (stopped and started again since: that first)
    std::vector<world::CellCoord> changed;
    m_ground.dent(scene.terrain, dent, at.x, at.z, changed);
    // A crater throws up the ground it was.
    if (!changed.empty() && dent.stretch <= 1.0f && dent.radius >= 2.0) {
        const float r = std::clamp(static_cast<float>(0.8 * dent.radius), 2.0f, 60.0f);
        const std::optional<double> ground = scene.terrain.height_at(at.x, at.z);
        m_dust.emit({at.x, ground.value_or(at.y) + 0.4 * r, at.z}, r, 1.2f / r, sim::k_dust_earth);
    }
    return changed.size();
}

PhysicsState::KeptDamage PhysicsState::keep_damage(world::Scene& scene) {
    notice_scene(scene);
    restore_ground(scene); // (stopped since: that ground is not this simulation's)
    follow_kept(scene, nullptr); // the forest holds the scene's kept trees as they are
    KeptDamage out;
    world::Damage next = scene.damage();
    std::unordered_set<world::CellCoord, world::CellCoordHash> touched;
    // Records whose placement is there but moved or changed since: they no
    // longer apply, and go.
    for (auto& [c, d] : next) {
        const usize was = d.kits.size();
        std::erase_if(d.kits, [&](const world::KeptKit& k) {
            const world::Entity* e = scene.find(k.entity);
            return e != nullptr && (e->shape != world::Shape::kit || e->kit != k.kit ||
                                    !(e->transform.position == k.position) || !(e->transform.rotation == k.rotation) ||
                                    !(e->transform.scale == k.scale));
        });
        if (d.kits.size() != was) touched.insert(c);
    }
    // The broken placements as they are now.
    for (world::KeptKit& k : m_bodies.damage()) {
        // Only placements in the scene as the bodies have them (one streamed
        // out, moved or deleted since the last sync is not this damage's).
        const world::Entity* e = scene.find(k.entity);
        if (e == nullptr || e->shape != world::Shape::kit || e->kit != k.kit || !(e->transform.position == k.position) ||
            !(e->transform.rotation == k.rotation) || !(e->transform.scale == k.scale)) {
            continue;
        }
        const world::CellCoord c = world::cell_of(k.position);
        std::vector<world::KeptKit>& kits = next[c].kits;
        const auto same = std::find_if(kits.begin(), kits.end(), [&](const world::KeptKit& x) { return x.entity == k.entity; });
        if (same != kits.end()) {
            *same = std::move(k);
        } else {
            kits.push_back(std::move(k));
        }
        touched.insert(c);
    }
    // The fallen trees of every cell the forest knows (its own trees only:
    // those standing in it).
    for (auto& [c, trees] : m_forest.damage()) {
        // (A cell streamed out since its placement was made: not in the scene now.)
        if (scene.terrain.find(c) == nullptr) continue;
        std::erase_if(trees, [c](const world::KeptTree& t) { return !(world::cell_of({t.x, 0.0, t.z}) == c); });
        const world::CellDamage* had = scene.damage_in(c);
        if (trees.empty() && (had == nullptr || had->trees.empty())) continue;
        next[c].trees = std::move(trees);
        touched.insert(c);
    }
    for (const world::CellCoord c : touched) {
        world::CellDamage d = next[c];
        world::sort_damage(d);
        const world::CellDamage* had = scene.damage_in(c);
        if (had == nullptr ? d.empty() : *had == d) continue;
        if (had != nullptr) out.before[c] = *had;
        if (!d.empty()) out.after[c] = d;
        scene.set_damage(c, std::move(d));
    }
    out.ground = m_ground.keep(scene.terrain);
    for (const auto& [c, d] : scene.damage()) {
        out.kits += d.kits.size();
        for (const world::KeptKit& k : d.kits) out.pieces += k.pieces.size();
        out.trees += d.trees.size();
    }
    follow_kept(scene, nullptr); // the trees laid as kept now (the kit placements follow with the scene's changes)
    return out;
}

PhysicsState::GroundAsSaved::GroundAsSaved(PhysicsState* physics, world::Scene& scene)
    : m_physics(physics), m_scene(scene) {
    if (m_physics == nullptr) return;
    m_physics->restore_ground(scene);
    m_physics->m_ground.lift(scene.terrain);
}

PhysicsState::GroundAsSaved::~GroundAsSaved() {
    if (m_physics != nullptr) m_physics->m_ground.put_back(m_scene.terrain);
}

void PhysicsState::update(world::Scene& scene, const world::SceneChanges& changed, double seconds) {
    notice_scene(scene);
    restore_ground(scene);
    m_bodies.sync(scene, changed.ids, changed.all);
    m_forest.sync(m_camera);
    if (!m_on) return;
    m_physics.step(seconds);
    m_bodies.update_poses();
    m_forest.after_step();
    m_dust.step(static_cast<float>(seconds), wind);
}

PhysicsState::Impact PhysicsState::impact(const DVec3& point, float radius, float impulse) {
    Impact out;
    // The scatter cells the blow reaches, nearest first, given to the
    // forest if it has not their trees (far ones: laid down, not toppled).
    if (m_scatter != nullptr && std::isfinite(radius) && radius > 0.0f && std::isfinite(point.x) &&
        std::isfinite(point.z)) {
        const double reach = static_cast<double>(radius) + world::k_cell_size;
        const world::CellCoord lo = world::cell_of({point.x - reach, 0.0, point.z - reach});
        const world::CellCoord hi = world::cell_of({point.x + reach, 0.0, point.z + reach});
        std::vector<std::pair<double, world::CellCoord>> cells;
        for (i32 z = lo.z; z <= hi.z; ++z) {
            for (i32 x = lo.x; x <= hi.x; ++x) {
                const world::CellCoord c{x, z};
                const ScatterPlacer::Cell* placed = m_scatter->find(c);
                if (placed == nullptr || placed->serial == m_forest.serial(c)) continue;
                const double dx = std::max({x * world::k_cell_size - point.x, 0.0, point.x - (x + 1) * world::k_cell_size});
                const double dz = std::max({z * world::k_cell_size - point.z, 0.0, point.z - (z + 1) * world::k_cell_size});
                const double d = std::sqrt(dx * dx + dz * dz);
                if (d <= reach) cells.push_back({d, c});
            }
        }
        std::sort(cells.begin(), cells.end(), [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first < b.first
                                      : (a.second.z != b.second.z ? a.second.z < b.second.z : a.second.x < b.second.x);
        });
        u64 looked = 0;
        for (const auto& [d, c] : cells) {
            const ScatterPlacer::Cell& placed = *m_scatter->find(c);
            if (looked > 0 && looked + placed.points.size() > points_per_impact) break;
            give(c, placed);
            looked += placed.points.size();
        }
    }
    out.loosened = m_bodies.impact(point, radius, impulse);
    out.felled = m_forest.impact(point, radius, impulse);
    return out;
}

void PhysicsState::flush(world::Scene& scene) {
    notice_scene(scene);
    restore_ground(scene);
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
    restore_ground(scene);
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
        m_dust.step(static_cast<float>(sim::PhysicsWorld::k_step_seconds), wind);
    }
    return steps;
}

} // namespace giant::editor
