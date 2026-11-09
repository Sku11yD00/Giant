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
#include "giant/sim/ballistics.h"
#include "giant/sim/dust.h"
#include "giant/sim/forest.h"
#include "giant/sim/physics.h"
#include "giant/sim/scene_physics.h"
#include "giant/world/ground_damage.h"
#include "giant/world/scene.h"
#include "hit_targets.h"
#include "scatter_placer.h"

#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace giant::editor {

class PhysicsState {
public:
    PhysicsState();

    bool simulating() const { return m_on; }
    // Counts starts: a walker made in another run (stopped and started
    // since, even within a frame) is let go.
    u64 runs() const { return m_runs; }
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
    // `steps` fixed steps at once (step_physics); the poses follow. Around
    // each, `before_fixed_step` and `after_fixed_step` (when set) steer
    // what walks and pose it (WalkerState).
    u32 run_steps(u32 steps);
    std::function<void(double seconds)> before_fixed_step;
    std::function<void()> after_fixed_step;

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
    // Told of each blow (impact) and of the shots' hits as they are taken
    // (pushes, wear): what else they move (swinging things, cloth:
    // connect_blows). Null: nothing else.
    std::function<void(const DVec3& point, float radius, float impulse)> on_impact;
    // The dust in the air (drawn by the renderer), moved by `wind` (m/s:
    // the weather's, set each frame) while simulating; stopping clears it.
    const sim::Dust& dust() const { return m_dust; }
    Vec3 wind{};
    // A dent pressed into the ground at `at` (x, z) while simulating: a
    // crater (with dust), a footprint. The scene's terrain changes, so all
    // that follows the ground follows; stopping puts it back exactly (the
    // next update), and saving saves the ground as it was (GroundAsSaved).
    // Returns the tiles it changed.
    usize dent(world::Scene& scene, const world::Dent& dent, const DVec3& at);
    const world::GroundDamage& ground() const { return m_ground; }
    // Stopped: the dented ground put back now (else the next update does).
    void restore_ground(world::Scene& scene);
    // Each frame once the placer swapped in new placements: the cells the
    // forest knows that were placed again, given to it now (so their fallen
    // trees are drawn as they lie this frame, never standing).
    void refresh_forest(const world::Scene& scene, const ScatterPlacer& scatter);
    // While one lives, the dented ground is as it was (a save in between).
    class GroundAsSaved {
    public:
        GroundAsSaved(PhysicsState* physics, world::Scene& scene);
        ~GroundAsSaved();
        GroundAsSaved(const GroundAsSaved&) = delete;
        GroundAsSaved& operator=(const GroundAsSaved&) = delete;

    private:
        PhysicsState* m_physics;
        world::Scene& m_scene;
    };
    // What simulating broke, made part of the world (keep_damage): the
    // broken kit placements' pieces down where they are drawn and the felled
    // trees where they lie become the scene's kept damage (world::Scene::
    // set_damage, by cell), and the dents the ground as authored (forgotten
    // here). Bodies falling now stop where they are. Kept damage is applied
    // whether simulating or not, and stopping leaves it so. Returns what
    // changed, for an undo: the cells' damage before and after, and the
    // ground's tiles (heights and versions before their first dent, and now).
    struct KeptDamage {
        world::Damage before; // the cells it changes, as they were (no entry: none)
        world::Damage after;
        std::vector<world::GroundDamage::Kept> ground;
        usize kits = 0;   // kit placements with pieces down, kept now in all
        usize pieces = 0;
        usize trees = 0;
    };
    KeptDamage keep_damage(world::Scene& scene);
    // What a game save keeps of the damage shown now, by cell, touching
    // nothing: the game's damage (world::Scene::game_damage), with what the
    // bodies have broken and the forest has felled now (as keep_damage
    // gathers them; the kept damage of the world they show is in it too,
    // as a game breaks more than the world kept, never less). Cells with
    // none are left out.
    world::Damage game_damage_now(world::Scene& scene);
    // Fallen scatter trees (drawn where they lie), and the cells to draw again (taken by the frame).
    const sim::FallenTrees& fallen_trees() const { return m_forest.fallen(); }
    std::vector<world::CellCoord> take_tree_redraw() { return m_forest.take_redraw(); }
    const sim::Forest& forest() const { return m_forest; }

    // Bullets (sim::Ballistics): fired while simulating, they fly with the
    // steps and meet what is made of what (the entity's material surface,
    // a kit piece's own; trees are wood, characters flesh, the ground dirt).
    // What they meet is pushed (dynamic bodies, by the momentum they lose)
    // and a breakable kit's pieces wear (blow_per_joule of the energy they
    // leave there, as N s, adding up to the kit's strength: then the piece
    // is shot out). Stopping clears them.
    struct ShotHit {
        sim::BulletHit hit;
        world::EntityId entity = 0;   // the entity met (0: none of the scene's)
        std::optional<u32> piece;     // a kit placement's piece
        bool tree = false;            // a scatter tree's trunk
        bool character = false;       // a character (the player, a skinned one's capsule)
        usize loosened = 0;           // kit pieces it shot out
        std::string zone;             // a skinned character's capsule: its zone (empty: none) and bone, as hit
        std::string bone;
    };
    // What a ray met is made of (the scene as last noticed).
    world::Surface surface_of(const sim::RayHit& hit) const;
    // k_no_bullet when refused (not simulating, bad numbers, too many flying).
    sim::BulletId fire(const sim::Shot& shot);
    // A shot flown to its end now, its hits returned (and taken: pushes,
    // wear) while simulating; not simulating, only reported.
    std::vector<ShotHit> resolve(const sim::Shot& shot);
    // Characters' capsules the bullets meet (null: none). Not owned.
    void set_hit_targets(const HitTargets* targets);
    const sim::Ballistics& ballistics() const { return m_ballistics; }
    sim::Ballistics& ballistics() { return m_ballistics; }
    // The latest hits (at most k_kept_hits), oldest first.
    static constexpr usize k_kept_hits = 256;
    const std::deque<ShotHit>& recent_hits() const { return m_recent; }
    std::function<void(std::span<const ShotHit>)> on_hits;
    float blow_per_joule = 0.1f; // N s of wear each joule a bullet leaves in a kit piece

    sim::PhysicsWorld& world() { return m_physics; }
    const sim::PhysicsWorld& world() const { return m_physics; }
    const sim::ScenePhysics& bodies() const { return m_bodies; }

private:
    // A new scene: the forest's trunks go before the scene's bodies clear the world.
    void notice_scene(const world::Scene& scene);
    // The trees of a cell's placement, in point order.
    sim::Ballistics::Target target_of(const sim::RayHit& hit) const;
    // Hits described (what they met), and taken when `apply` (pushes, wear).
    std::vector<ShotHit> take(std::span<const sim::BulletHit> hits, bool apply);
    void take_hits();
    static std::vector<sim::TreeSpot> trees_of(const ScatterPlacer::Cell& placed);
    // The cell's placement given to the forest, with the scene's kept trees there.
    void give(world::CellCoord cell, const ScatterPlacer::Cell& placed);
    // When the scene's kept damage changed: the cells the forest knows whose
    // kept trees it was given otherwise are given again now; others with
    // kept trees wait until placed, then are given within the feed's budget
    // (`looked`: points looked at this feed; null: no budget).
    void follow_kept(const world::Scene& scene, u64* looked);

    JobSystem m_jobs; // first: outlives the physics world
    sim::Dust m_dust; // before what emits into it
    world::GroundDamage m_ground;
    bool m_restore_ground = false;
    sim::PhysicsWorld m_physics;
    u64 m_runs = 0;
    sim::ScenePhysics m_bodies;
    sim::Forest m_forest; // after the world: its trunks go first
    sim::Ballistics m_ballistics;
    std::deque<ShotHit> m_recent;
    const HitTargets* m_targets = nullptr;
    u64 m_scene = 0;
    const world::Scene* m_scene_at = nullptr; // as last noticed (the editor's scene outlives this)
    u64 m_damage_edits = ~u64{0};             // the scene's kept damage the forest follows
    std::unordered_set<world::CellCoord, world::CellCoordHash> m_kept_waiting; // cells with kept trees not given yet
    DVec3 m_camera{}; // as last fed
    const ScatterPlacer* m_scatter = nullptr; // as last fed (it outlives this)
    bool m_on = false;
};

} // namespace giant::editor
