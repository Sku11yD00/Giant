// The scene in the physics world: a body for each entity that collides
// (world::Entity::body), the ground's tiles as height fields, kept in step
// with edits; and where the dynamic bodies have moved to, as poses beside
// the scene (the scene keeps where things were put: what undo, saving and
// stopping a simulation go by).
//
//   ScenePhysics follow(physics);
//   follow.sync(scene, changed.ids, changed.all); // each frame, what the scene changed
//   physics.step(delta);
//   follow.update_poses();                        // where the dynamic entities are now
//   draw(follow.poses(), follow.take_redraw());
//
// Bodies are made from what the renderer draws: a cube is a box, a sphere
// a ball (an ellipsoid when scaled unevenly), a plane a slab whose top is
// the plane, a kit one compound of its cube, sphere and plane pieces. Meshes
// and decals do not collide yet. A kit entity is fixed whatever its body
// says: a house breaks rather than falls over. A breakable kit's pieces
// (world::Kit::breakable) come loose when struck hard enough (a contact,
// or impact()), and so do the pieces no longer standing on the ground
// through pieces they touch; loose pieces fall as bodies of their own,
// drawn where they are (piece_poses), until reset_poses puts them back.
// Near the camera they fall as bodies; far off (or past a cap) they come
// down at once as rubble, laid on what is under them, with no body (the
// mid ring: a city wrecked a kilometre away costs the steps nothing). A
// collapse throws up dust (set_dust).
//
// A sync does a bounded amount of work (rule 9): at most
// `entities_per_sync` entities are looked at and `tiles_per_sync` ground
// tiles copied out, the rest wait for the next; a tile's height field is
// made on a worker of its own and swapped in when ready (the old one holds
// the ground meanwhile). A dynamic entity's body waits until the fixed
// bodies waiting are made and the ground round it is in, so nothing falls
// through ground still being built. flush() does all of it at once.
#pragma once

#include "giant/core/jobs.h"
#include "giant/sim/dust.h"
#include "giant/sim/physics.h"
#include "giant/world/scene.h"

#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace giant::sim {

struct ScenePhysicsSettings {
    // Entities looked at (bodies made, remade, moved or let go) per sync,
    // a body made counting each of its parts (a kit of 40 pieces counts 40).
    u32 entities_per_sync = 1024;
    u32 tiles_per_sync = 16;      // ground tiles copied out per sync (their shapes are made on a worker)
    // Loose pieces fall as bodies within `simulate_radius` metres of the
    // camera (set_camera), at most `max_loose` at once; past either they
    // come down as rubble: no body, laid flat on what is under them (where
    // the blow threw them), at once. Without a camera all are bodies.
    double simulate_radius = 600.0;
    u32 max_loose = 4096;
    // Rays a blow (an impact, a step's hits) casts at most to lay rubble on
    // what is under it; past them rubble lies at its building's foot.
    u32 rubble_rays = 4096;
};

struct ScenePhysicsStats {
    u32 entity_bodies = 0;
    u32 dynamic_bodies = 0;
    u32 tile_bodies = 0;
    u32 not_colliding = 0;  // entities whose shape does not collide (meshes, decals) or body is none
    u32 refused = 0;        // entities the physics world refused (bad numbers, or full)
    u32 waiting = 0;        // entities waiting to be looked at
    u32 tiles_waiting = 0;  // ground tiles waiting to be copied out or for their shape
    u32 posed = 0;          // dynamic entities drawn where the simulation moved them
    u32 broken_kits = 0;    // breakable kit placements with pieces loose
    u32 loose_pieces = 0;   // their pieces falling (or fallen) as bodies of their own
    u32 rubble = 0;         // their pieces come down as rubble (no body: far off, or past max_loose)
    u32 kept_pieces = 0;    // their pieces down as the scene's kept damage says (no body)
};

// A breakable kit's piece, come loose: where it is (its world transform).
// Moving (its body moved in one of the last k_still updates: drifting down
// a river, still falling) it is drawn kept out of the voxel world, so it
// costs the voxels nothing each frame; once still, it is drawn into it
// again (once).
struct PiecePose {
    static constexpr u32 k_still = 30; // update_poses calls without moving
    u32 piece = 0; // its index in the kit
    world::Transform at;
    u32 still = k_still; // update_poses calls since its body last moved (rubble and kept pieces: still)
    bool moving() const { return still < k_still; }
};

class ScenePhysics {
public:
    explicit ScenePhysics(PhysicsWorld& physics, const ScenePhysicsSettings& settings = {});
    ~ScenePhysics();
    ScenePhysics(const ScenePhysics&) = delete;
    ScenePhysics& operator=(const ScenePhysics&) = delete;

    // Brings the bodies in line with the scene, within the budgets.
    // `changed`: the ids the scene handed out since the last sync
    // (SceneChanges::ids); `all`: look at every entity. A new scene (another
    // identity), changed kits and the ground's tiles (it takes
    // Terrain::take_physics_changes) are noticed here. An entity moved by an
    // edit is put there, at rest (and drawn there again).
    void sync(world::Scene& scene, std::span<const world::EntityId> changed, bool all);
    // A sync with no budget: every entity waiting (and in `changed`) looked
    // at, every tile's shape made and in (a command's answer: a raycast,
    // step_physics).
    void flush(world::Scene& scene, std::span<const world::EntityId> changed = {}, bool all = false);

    // After a step: the dynamic entities whose bodies moved get their pose
    // (and are to be drawn again). Returns how many.
    usize update_poses();
    // Where the simulation has moved dynamic entities to (position and
    // rotation; the scale is the entity's). Entities not in it are where
    // the scene puts them.
    const std::unordered_map<world::EntityId, world::Transform>& poses() const { return m_poses; }
    // Of those, the ones whose bodies moved in one of the last
    // PiecePose::k_still update_poses calls (drawn kept out of the voxel
    // world; once still, they are to be drawn again, into it).
    const std::unordered_set<world::EntityId>& moving() const { return m_moving; }
    // A dynamic entity's body put at `position` and `rotation` (unit), at
    // rest, and posed there (a game save's bodies loaded). False, changing
    // nothing: not a dynamic entity with a body now, an edit of it waiting
    // to be seen, or bad numbers. reset_poses puts it back as any pose.
    bool place(world::EntityId entity, const DVec3& position, const Quat& rotation);
    // Every moved body back where its entity is, at rest; no poses (a
    // simulation stopping).
    void reset_poses();
    // The entities whose pose changed or went since the last call (to draw
    // again), each once (a kit whose pieces moved among them).
    std::vector<world::EntityId> take_redraw();

    // Breakable kits' loose pieces, by placement, in piece order; the rest
    // of a kit's pieces are where the scene puts them.
    const std::unordered_map<world::EntityId, std::vector<PiecePose>>& piece_poses() const { return m_piece_poses; }
    // A blow at `point`: the breakable kits' pieces within `radius` metres
    // take `impulse` (N s; less toward the edge) pushing them away, and
    // those it is enough for come loose. Returns how many came loose
    // (with what they held up). A giantess's footstep, an explosion.
    usize impact(const DVec3& point, float radius, float impulse);
    // What a body is in the scene (what a ray or a bullet met; `part` the
    // RayHit's): an entity's, and for a kit placement which of the kit's
    // pieces (its standing body's part, or a loose piece's own body). 0: not
    // an entity's (the ground, a tree, a character).
    struct Owner {
        world::EntityId entity = 0;
        std::optional<u32> piece; // a kit placement's piece, as its index in the kit
    };
    Owner owner_of(BodyId body, u32 part) const;
    // Bullets' blows (N s) on breakable kit placements' standing pieces:
    // a piece's blows add up (wear), and once they come to the kit's
    // strength it comes loose, pushed along `direction`, with what it held
    // up. A batch (a frame's hits) loosens each placement once, within one
    // blow's rubble rays. Returns how many pieces came loose by each strike
    // (counted on the strike that brought its placement's pieces down).
    // Wear is forgotten when the placement is moved or made again, and by
    // reset_poses.
    struct Strike {
        world::EntityId entity = 0;
        u32 piece = 0;
        float blow = 0.0f;
        Vec3 direction{};
    };
    std::vector<usize> strike(std::span<const Strike> strikes);
    // Where the camera is (the near ring: loose pieces within
    // simulate_radius of it fall as bodies).
    void set_camera(const DVec3& camera) { m_camera = camera; }
    // Where collapses throw up dust (none: no dust). Not owned.
    void set_dust(Dust* dust) { m_dust = dust; }
    // The broken kit placements as they are now, by id (keep_damage): each
    // piece down where it is drawn (falling ones where they are this step).
    // The scene's kept damage (world::Scene::damage) is applied by sync: a
    // placement its record still matches stands with those pieces down
    // where the record says, with no bodies, simulating or not, and
    // reset_poses puts it back so (not whole).
    std::vector<world::KeptKit> damage() const;

    // Where the scene had the entity when its body was last brought in line
    // (null: not tracked). A pose is stale while the scene's transform differs.
    const world::Transform* placed(world::EntityId entity) const;
    world::EntityId entity_of(BodyId body) const;
    BodyId body_of(world::EntityId entity) const;
    ScenePhysicsStats stats() const;
    // True while ground near `at` (its tile and those round it) is still on
    // its way: what walks or falls there would go through.
    bool ground_waiting(const world::Terrain& terrain, const DVec3& at) const;
    // True while entities are still waiting to be looked at (bodies to make).
    bool entities_waiting() const;
    // Lets every body go (the physics world is left empty of the scene's).
    // `others`: bodies in the world not ours that may go too (a forest's);
    // when ours and those are all it holds, the world is cleared whole (off
    // the frame) and true is returned: the others' ids are stale.
    bool clear(usize others = 0);

private:
    struct Broken; // a breakable kit placement with pieces loose
    struct Tracked {
        BodyId body = k_no_body; // a broken kit's: the pieces still standing (k_no_body: none)
        world::Entity look; // what the body was made from, where the scene put it
        bool dynamic = false;
        bool remake = false; // its kit changed: made again whatever else
        std::shared_ptr<Broken> broken;
        std::vector<world::KeptPiece> kept; // the scene's kept damage it was made with (none: none)
        std::vector<std::pair<u32, float>> wear; // blows taken by standing pieces (strike), by piece
        mutable std::vector<u32> part_pieces;    // unbroken kit body: which piece each part is (owner_of's, made on first use)
    };
    struct Build; // a tile's shape being made
    struct Tile {
        BodyId body = k_no_body;
        u64 versions[4] = {}; // of the body: the tile's and its +x, +z and +x+z neighbours'
        std::shared_ptr<Build> building; // the shape on its way, with the versions it is of
    };
    struct Made {
        world::EntityId id = 0;
        BodyDesc desc;
        bool dynamic = false;
    };

    void enqueue(const world::Scene& scene, world::EntityId id);
    void enqueue_tile(world::CellCoord cell);
    // Returns its cost against the budget.
    u32 sync_entity(const world::Scene& scene, world::EntityId id, std::vector<Made>& made, std::vector<BodyId>& gone);
    void sync_entities(const world::Scene& scene, u32 budget);
    void sync_tiles(world::Terrain& terrain, u32 budget);
    void finish_tiles(bool wait);
    void drop_pose(world::EntityId id);
    // update_poses' start: an update further for what moves (moving()).
    void settle_poses();
    const world::Kit* find_kit(const std::string& name) const;
    BodyDesc entity_body(const world::Kit* kit, const world::Entity& e) const;
    // The scene's kept record of `e` while it still matches it (null: none).
    const world::KeptKit* kept_of(const world::Scene& scene, const world::Entity& e) const;
    // A placement broken as `kept` says (Tracked::broken, its poses): returns
    // the body of what still stands (no parts: none).
    BodyDesc make_kept(world::EntityId id, Tracked& t, const world::Kit& kit, std::span<const world::KeptPiece> kept);
    // The standing pieces of a broken placement as one fixed body (its standing_parts set).
    BodyDesc standing_body(world::EntityId id, Tracked& t, const world::Kit& kit) const;
    // A broken kit's bodies gone (into `gone`), its pieces drawn whole again.
    void mend(world::EntityId id, Tracked& t, std::vector<BodyId>& gone);
    // Pieces of a breakable kit come loose (with what they held up), each
    // pushed by its `pushes` entry (m/s). Returns how many came loose.
    usize loosen(world::EntityId id, Tracked& t, const std::vector<std::pair<u32, Vec3>>& pushes);
    void break_from_hits();
    // Where a loose piece placed `at` comes down as rubble: thrown by
    // `push`, flat on what is under it (`base`: the building's foot, if nothing).
    world::Transform rubble_at(world::EntityId id, const world::KitPiece& piece, const world::Transform& at,
                               const Vec3& push, u32 index, double base) const;

    PhysicsWorld& m_physics;
    ScenePhysicsSettings m_settings;
    u64 m_scene = 0;
    std::vector<world::Kit> m_kits;
    std::unordered_map<world::EntityId, Tracked> m_entities;
    std::unordered_map<BodyId, world::EntityId> m_by_body;
    std::unordered_map<world::EntityId, world::Transform> m_poses;
    std::unordered_set<world::EntityId> m_moving;           // of m_poses (moving())
    std::unordered_map<world::EntityId, u32> m_still;       // m_moving's update_poses calls since they moved
    std::unordered_set<world::EntityId> m_settling;         // kits with moving pieces (PiecePose::moving)
    std::vector<world::EntityId> m_redraw;
    std::unordered_map<world::EntityId, std::vector<PiecePose>> m_piece_poses;
    struct PieceOf {
        world::EntityId entity = 0;
        u32 piece = 0;
    };
    std::unordered_map<BodyId, PieceOf> m_piece_bodies; // loose pieces' bodies
    std::unordered_set<world::EntityId> m_broken;       // placements with pieces loose
    std::unordered_set<world::EntityId> m_worn;         // placements with wear (strike)
    // Entities to look at: those changed (queued once), then the scene's in
    // turn after an "all"; new dynamic ones go to their own queue to wait
    // for what they could land on.
    std::deque<world::EntityId> m_queue;
    std::unordered_set<world::EntityId> m_queued;
    std::vector<world::EntityId> m_scan;
    usize m_scan_at = 0;
    std::deque<world::EntityId> m_dynamic_queue;
    std::unordered_set<world::EntityId> m_dynamic_queued;
    std::unordered_set<world::EntityId> m_edited; // moved by an edit not yet seen: no pose from the old body
    bool m_closing = false;                        // going: no fresh physics world to clear into
    std::optional<DVec3> m_camera;
    Dust* m_dust = nullptr;
    u32 m_rubble = 0;
    u32 m_kept = 0; // pieces down as kept damage says
    mutable u32 m_rays_left = 0; // rubble_rays left in this blow
    // Ground tiles to look at, and those whose shape is being made.
    std::unordered_map<world::CellCoord, Tile, world::CellCoordHash> m_tiles;
    std::deque<world::CellCoord> m_tile_queue;
    std::unordered_set<world::CellCoord, world::CellCoordHash> m_tile_queued;
    std::vector<world::CellCoord> m_building;
    WaitGroup m_builds;
    JobSystem m_builder{1}; // last: its jobs end before the rest goes
};

} // namespace giant::sim
