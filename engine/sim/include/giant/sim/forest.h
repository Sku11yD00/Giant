// Trees that fall: the scatter trees (world::ScatterTree) near the camera
// stand in the physics world as trunk bodies (fixed capsules: what falls
// into them stops). Struck hard enough (a contact at least the tree's
// strength, or impact()) a trunk snaps at its foot and topples away from
// the blow, a body of its own falling onto what is there: trees it strikes
// hard enough fall in turn, and a building's pieces break as under any
// other blow (ScenePhysics reads the same hits). Once it comes to rest a
// fallen tree sleeps (costing the steps nothing) and wakes, falling again,
// when what it rests on goes. Fallen trees are drawn where they lie
// (fallen()), until reset() stands them up again. Far off, where trunks
// have no bodies, a blow lays its trees down at once where they would fall
// (impact()); a felled tree throws up dust where it lands (set_dust).
//
//   Forest forest(physics);
//   forest.set_cell(cell, serial, trees); // a scatter cell's trees, each time it is placed
//   forest.sync(camera);                  // each frame: trunks made near the camera, let go far off
//   physics.step(delta);
//   forest.after_step();                  // trunks struck topple; where falling ones are now
//   draw(forest.fallen(), forest.take_redraw());
//
// Work is bounded (rule 9). A sync makes or removes at most
// trunks_per_sync bodies (bodies of trees let go wait in a queue for it),
// nearest cells first, and at most max_trunks stand at once (a nearer cell
// takes them from the farthest). Only cells within `radius` (plus a cell)
// of the camera get trunks; past a cell more their standing trunks go
// (fallen ones stay), and a cell with none fallen is forgotten (set_cell
// brings it back). At most topples_per_step trees start falling a step
// (the hardest struck first), max_falling are falling at once and
// max_fallen lie fallen; blows past that are shrugged off. A fallen tree
// belongs to its cell's placement: a cell placed again (another serial)
// stands all its trees up afresh.
#pragma once

#include "giant/sim/dust.h"
#include "giant/sim/physics.h"
#include "giant/world/cell.h"
#include "giant/world/scene.h"

#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace giant::sim {

// A tree of a scatter cell, as placed.
struct TreeSpot {
    u32 point = 0;       // its index in the cell's placements (what fallen() names)
    world::Transform at; // its foot: the placement's transform (the scale draws it)
    float trunk_radius = 0.25f; // metres (the scale applied)
    float height = 10.0f;       // metres, foot to top
    float buried = 0.0f;        // metres of it below the ground (the item's sink): left out of the trunk
    float strength = world::k_default_tree_strength; // N s
};

// A fallen tree: where it is (its foot's transform, moved as its trunk fell).
struct FallenTree {
    u32 point = 0;
    world::Transform at;
};
// A cell's fallen trees, in point order, for the placement `serial` names.
struct FallenCell {
    u64 serial = 0;
    std::vector<FallenTree> trees;
};
using FallenTrees = std::unordered_map<world::CellCoord, FallenCell, world::CellCoordHash>;

struct ForestSettings {
    double radius = 384.0;       // metres from the camera to a cell's centre (plus a cell) for its trunks to stand
    u32 trunks_per_sync = 1024;  // bodies made or let go per sync (about a millisecond)
    u32 max_trunks = 1u << 16;   // standing as bodies at once
    u32 topples_per_step = 64;   // trees that may start falling in one after_step or impact
    u32 max_falling = 512;       // moving (awake) at once
    u32 max_fallen = 1u << 14;   // fallen at once (resting ones asleep)
    u32 lays_per_impact = 2048;  // trees with no trunk body an impact lays down at most
    float density = 700.0f;      // kilograms a cubic metre (wood)
};

struct ForestStats {
    u32 cells = 0;    // known
    u64 trees = 0;    // in them
    u32 trunks = 0;   // standing as bodies
    u32 waiting = 0;  // trees in reach waiting for their trunk
    u32 falling = 0;  // falling (moving)
    u32 fallen = 0;   // falling or fallen
    u32 refused = 0;  // trunks the physics world refused (bad numbers, or full), in all
    u32 shrugged = 0; // blows enough to fell a tree left out (the caps), in all
    u32 laid = 0;     // fallen with no body (laid down where they fell: far off), now
};

class Forest {
public:
    explicit Forest(PhysicsWorld& physics, const ForestSettings& settings = {});
    ~Forest(); // its bodies go
    Forest(const Forest&) = delete;
    Forest& operator=(const Forest&) = delete;

    // A cell's trees, placed afresh (`serial`: the placement's, never 0;
    // `trees` in point order, maybe none): its old trunks go (in the next
    // syncs) and its fallen trees stand again (drawn again).
    void set_cell(world::CellCoord cell, u64 serial, std::vector<TreeSpot> trees);
    // A cell gone (its placements left): its trunks, fallen or not, go.
    void remove_cell(world::CellCoord cell);
    // The placement a cell's trees are of (0: not known).
    u64 serial(world::CellCoord cell) const;
    // Whether set_cell should be given the cell's trees (it is in reach of `camera`).
    bool wants(world::CellCoord cell, const DVec3& camera) const;
    std::vector<world::CellCoord> cells() const;
    // True while trees in reach of the last sync's camera wait for their
    // trunk, or bodies of trees gone still wait to leave the world.
    bool waiting() const;

    // Trunks made near `camera` and let go far from it, within the budget
    // (flush: all of them now; a command's answer, tests).
    void sync(const DVec3& camera);
    void flush(const DVec3& camera);
    // After a step: the trunks struck hard enough topple; falling trees get
    // their pose (their cells to draw again; a resting one woken falls
    // again). Returns how many started falling.
    usize after_step();
    // A blow at `point`: standing trees within `radius` metres take
    // `impulse` (N s; less toward the edge) and those it is enough for
    // topple away from it. Returns how many. A tree whose trunk has no body
    // (far from the camera, or still waiting) is laid down at once where it
    // would fall, its top on what is there, with no body (at most
    // lays_per_impact, the hardest struck first).
    usize impact(const DVec3& point, float radius, float impulse);
    // Where felled trees throw up dust (none: no dust). Not owned.
    void set_dust(Dust* dust) { m_dust = dust; }
    // Every fallen tree stands again (their bodies go; trunks made again by
    // the next syncs). A simulation stopping.
    void reset();
    // Every body let go and every cell forgotten.
    void clear();
    // As clear, the bodies left in the world (a new scene: the world is
    // cleared whole next, so their ids are only forgotten). Returns how many.
    usize forget();
    // Bodies of its own in the world (trunks standing or fallen, and those still to go).
    usize bodies() const { return m_by_body.size() + m_doomed.size(); }

    const FallenTrees& fallen() const { return m_fallen; }
    // Cells whose fallen trees moved, came or went since the last call.
    std::vector<world::CellCoord> take_redraw();
    // The tree a body stands for (a raycast's hit): its cell and point, or none.
    struct TreeRef {
        world::CellCoord cell;
        u32 point = 0;
        bool fallen = false;
    };
    std::optional<TreeRef> tree_of(BodyId body) const;
    // A tree's body (k_no_body: none yet, or not known).
    BodyId body_of(world::CellCoord cell, u32 point) const;
    ForestStats stats() const;
    const ForestSettings& settings() const { return m_settings; }

private:
    struct Tree {
        TreeSpot spot;
        BodyId body = k_no_body;
        bool fallen = false;
        bool laid = false; // fallen with no body: laid down (lay())
    };
    struct Cell {
        u64 serial = 0;
        std::vector<Tree> trees;
        usize next = 0;   // trees before it have their body (or fell)
        u32 trunks = 0;   // standing trunks with bodies
        u32 fallen = 0;
        float reach = 0.0f; // its tallest tree's height and radius (metres)
    };
    struct Owner {
        world::CellCoord cell;
        u32 index = 0; // in the cell's trees
    };
    struct Topple {
        world::CellCoord cell;
        u32 index = 0;
        Vec3 away{};   // which way it falls (across its trunk, after normalizing)
        float taken = 0.0f; // the blow, N s
    };
    struct Falling {};

    void sync(const DVec3& camera, u32 budget);
    // Every body of the cell's trees to go (m_doomed), fallen ones standing again.
    void drop_bodies(world::CellCoord coord, Cell& cell);
    // A cell's standing trunks to go (m_doomed: they leave within the syncs' budget).
    void drop_trunks(Cell& cell);
    BodyDesc trunk(const TreeSpot& spot, Motion motion) const;
    usize topple(std::vector<Topple>& topples);
    // Trees with no trunk body laid down where they fall (no body).
    usize lay(std::vector<Topple>& lays);
    // Which way a tree struck `away` falls: across its trunk.
    static Vec3 across_of(const world::Transform& at, const Vec3& away);
    void settle();
    void redraw(world::CellCoord cell);
    double distance(world::CellCoord cell, const DVec3& camera) const;

    PhysicsWorld& m_physics;
    ForestSettings m_settings;
    std::unordered_map<world::CellCoord, Cell, world::CellCoordHash> m_cells;
    std::unordered_map<BodyId, Owner> m_by_body;
    std::unordered_map<BodyId, Falling> m_falling; // fallen trees moving (awake)
    std::vector<BodyId> m_doomed; // bodies no tree has now, to go within the syncs' budget
    FallenTrees m_fallen;
    std::vector<world::CellCoord> m_redraw;
    std::unordered_set<world::CellCoord, world::CellCoordHash> m_redraw_set;
    DVec3 m_camera{}; // as last synced
    u32 m_trunks = 0;
    u32 m_fallen_count = 0;
    u32 m_refused = 0;
    u32 m_shrugged = 0;
    u32 m_laid = 0;
    Dust* m_dust = nullptr;
};

} // namespace giant::sim
