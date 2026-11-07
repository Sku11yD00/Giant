// Trees that fall: trunks stand near the camera, topple when struck hard
// enough, fall onto what is there, and stand again on reset.
#include "giant/sim/forest.h"
#include "giant/sim/scene_physics.h"

#include "giant_test.h"

#include <algorithm>
#include <cmath>

using namespace giant;
using namespace giant::sim;
using world::CellCoord;

namespace {

// A ground slab 400 m across, top at y = 0.
void ground(PhysicsWorld& physics) {
    BodyDesc g;
    g.parts.push_back({BoxShape{{200.0f, 0.5f, 200.0f}}});
    g.position = {0.0, -0.5, 0.0};
    physics.add(g);
}

TreeSpot tree(u32 point, DVec3 foot, float strength = world::k_default_tree_strength) {
    TreeSpot s;
    s.point = point;
    s.at.position = foot;
    s.strength = strength;
    return s;
}

void run(PhysicsWorld& physics, Forest& forest, int steps) {
    for (int i = 0; i < steps; ++i) {
        physics.step(PhysicsWorld::k_step_seconds);
        forest.after_step();
    }
}

const FallenTree* fallen(const Forest& forest, CellCoord cell, u32 point) {
    const auto it = forest.fallen().find(cell);
    if (it == forest.fallen().end()) return nullptr;
    for (const FallenTree& t : it->second.trees) {
        if (t.point == point) return &t;
    }
    return nullptr;
}

// How far the tree's trunk leans from upright (degrees).
float lean(const world::Transform& at) {
    const Vec3 up = rotate(at.rotation, Vec3{0.0f, 1.0f, 0.0f});
    return std::acos(std::clamp(up.y, -1.0f, 1.0f)) * 57.29578f;
}

} // namespace

GIANT_TEST("forest: trunks stand near the camera within the budget, go far off, and a cell is forgotten") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    ForestSettings settings;
    settings.radius = 100.0;
    settings.trunks_per_sync = 3;
    Forest forest(physics, settings);
    std::vector<TreeSpot> trees;
    for (u32 i = 0; i < 5; ++i) trees.push_back(tree(i, {10.0 + 8.0 * i, 0.0, 10.0}));
    GIANT_CHECK(forest.wants({0, 0}, {0.0, 0.0, 0.0}));
    GIANT_CHECK(!forest.wants({10, 0}, {0.0, 0.0, 0.0}));
    forest.set_cell({0, 0}, 7, trees);
    GIANT_CHECK(forest.serial({0, 0}) == 7);
    forest.sync({0.0, 0.0, 0.0});
    GIANT_CHECK(physics.size() == 3); // the budget
    GIANT_CHECK(forest.stats().waiting == 2);
    forest.sync({0.0, 0.0, 0.0});
    GIANT_CHECK(physics.size() == 5);
    GIANT_CHECK(forest.stats().trunks == 5);
    GIANT_CHECK(forest.body_of({0, 0}, 2) != k_no_body);
    // The trunk is a capsule up from the foot: a ray across it at 5 m hits.
    const auto hit = physics.raycast({18.0, 5.0, 0.0}, {0.0f, 0.0f, 1.0f}, 100.0);
    GIANT_CHECK(hit.has_value() && hit->body == forest.body_of({0, 0}, 1));
    GIANT_CHECK(hit.has_value() && std::fabs(hit->point.z - 9.75) < 0.01);
    GIANT_CHECK(forest.tree_of(hit->body).has_value() && forest.tree_of(hit->body)->point == 1);
    // Placed again: the old trunks go within the budget, the new follow.
    forest.set_cell({0, 0}, 8, trees);
    GIANT_CHECK(physics.size() == 5 && forest.stats().trunks == 0);
    forest.sync({0.0, 0.0, 0.0});
    GIANT_CHECK(physics.size() == 2); // three old ones gone, the budget spent
    forest.sync({0.0, 0.0, 0.0});
    GIANT_CHECK(physics.size() == 1 && forest.stats().trunks == 1);
    forest.flush({0.0, 0.0, 0.0});
    GIANT_CHECK(physics.size() == 5 && forest.stats().trunks == 5);
    // Far off: let go (the cell forgotten at once), the bodies leaving within the budget.
    forest.sync({1000.0, 0.0, 0.0});
    GIANT_CHECK(forest.stats().trunks == 0 && forest.cells().empty() && physics.size() == 5);
    GIANT_CHECK(forest.waiting()); // bodies still to leave
    forest.sync({1000.0, 0.0, 0.0});
    GIANT_CHECK(physics.size() == 2);
    forest.sync({1000.0, 0.0, 0.0});
    GIANT_CHECK(physics.size() == 0 && !forest.waiting());
}

GIANT_TEST("forest: at most max_trunks stand, the nearest cells' first") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    ForestSettings settings;
    settings.radius = 300.0;
    settings.max_trunks = 4;
    Forest forest(physics, settings);
    forest.set_cell({0, 0}, 1, {tree(0, {10.0, 0.0, 10.0}), tree(1, {20.0, 0.0, 10.0}), tree(2, {30.0, 0.0, 10.0})});
    forest.set_cell({3, 0}, 2, {tree(0, {200.0, 0.0, 10.0}), tree(1, {210.0, 0.0, 10.0}), tree(2, {220.0, 0.0, 10.0})});
    forest.flush({32.0, 0.0, 32.0});
    GIANT_CHECK(forest.stats().trunks == 4 && !forest.waiting());
    GIANT_CHECK(forest.body_of({0, 0}, 2) != k_no_body && forest.body_of({3, 0}, 1) == k_no_body);
    // Now nearer the other: it takes them from the farther.
    forest.flush({224.0, 0.0, 32.0});
    GIANT_CHECK(forest.stats().trunks == 3 && forest.body_of({3, 0}, 2) != k_no_body);
    forest.flush({224.0, 0.0, 32.0}); // the room left: the farther cell's nearest-made first
    GIANT_CHECK(forest.stats().trunks == 4 && physics.size() == 4); // the ones let go have left the world
    GIANT_CHECK(forest.body_of({0, 0}, 0) != k_no_body && forest.body_of({0, 0}, 1) == k_no_body);
}

GIANT_TEST("forest: a tree resting on another falls on when that one is felled") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    ground(physics);
    Forest forest(physics);
    // A felled toward B, 4 m off and too strong to fell: it comes to rest leaning on B.
    forest.set_cell({0, 0}, 1, {tree(0, {10.0, 0.0, 10.0}), tree(1, {14.0, 0.0, 10.0}, 1e7f)});
    forest.flush({0.0, 0.0, 0.0});
    GIANT_CHECK(forest.impact({9.0, 1.0, 10.0}, 1.5f, 1e5f) == 1);
    run(physics, forest, 300);
    const FallenTree* a = fallen(forest, {0, 0}, 0);
    GIANT_CHECK(a != nullptr && lean(a->at) > 10.0f && lean(a->at) < 60.0f && forest.stats().falling == 0);
    // B felled the other way: A, woken, falls the rest of the way.
    GIANT_CHECK(forest.impact({15.0, 1.0, 10.0}, 1.5f, 1e9f) == 1);
    run(physics, forest, 360);
    a = fallen(forest, {0, 0}, 0);
    GIANT_CHECK(a != nullptr && lean(a->at) > 75.0f);
}

GIANT_TEST("forest: a tree of its own size falling on another does not fell it: no chain through the forest") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    ground(physics);
    Forest forest(physics);
    std::vector<TreeSpot> trees;
    for (u32 i = 0; i < 6; ++i) trees.push_back(tree(i, {8.0 + 6.0 * i, 0.0, 10.0}));
    forest.set_cell({0, 0}, 1, trees);
    forest.flush({0.0, 0.0, 0.0});
    GIANT_CHECK(forest.impact({7.0, 1.0, 10.0}, 1.5f, 1e5f) == 1);
    run(physics, forest, 300);
    GIANT_CHECK(forest.stats().fallen == 1 && fallen(forest, {0, 0}, 0) != nullptr);
}

GIANT_TEST("forest: a tree struck hard enough topples away from the blow and lies on the ground; a nudge does not") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    ground(physics);
    Forest forest(physics);
    forest.set_cell({0, 0}, 1, {tree(0, {20.0, 0.0, 20.0})});
    forest.flush({0.0, 0.0, 0.0});
    // A 100 kg ball at 5 m/s: 500 N s, under the 8000 it takes.
    BodyDesc ball;
    ball.motion = Motion::dynamic;
    ball.parts.push_back({SphereShape{0.4f}});
    ball.mass = 100.0f;
    ball.position = {17.0, 3.0, 20.0};
    ball.linear_velocity = {5.0f, 0.0f, 0.0f};
    const BodyId nudge = physics.add(ball);
    run(physics, forest, 60);
    GIANT_CHECK(forest.stats().fallen == 0);
    GIANT_CHECK(forest.fallen().empty());
    physics.remove(nudge);
    // A 1000 kg ball at 12 m/s from the west: it falls east.
    ball.mass = 1000.0f;
    ball.linear_velocity = {12.0f, 0.0f, 0.0f};
    physics.add(ball);
    run(physics, forest, 30);
    GIANT_CHECK(forest.stats().fallen == 1);
    GIANT_CHECK(forest.stats().trunks == 0);
    GIANT_CHECK(!forest.take_redraw().empty());
    run(physics, forest, 360);
    const FallenTree* lying = fallen(forest, {0, 0}, 0);
    GIANT_CHECK(lying != nullptr);
    if (lying != nullptr) {
        GIANT_CHECK(lean(lying->at) > 75.0f); // on the ground
        const Vec3 up = rotate(lying->at.rotation, Vec3{0.0f, 1.0f, 0.0f});
        GIANT_CHECK(up.x > 0.8f); // its top to the east
        GIANT_CHECK(lying->at.position.y > -0.5 && lying->at.position.y < 1.0);
    }
    GIANT_CHECK(forest.fallen().at({0, 0}).serial == 1);
    // At rest: fixed where it lies (nothing left falling), still in the world.
    GIANT_CHECK(forest.stats().falling == 0 && forest.stats().fallen == 1);
    const auto resting = physics.state(forest.body_of({0, 0}, 0));
    GIANT_CHECK(resting.has_value() && !resting->active && lying != nullptr && length(resting->position - lying->at.position) < 1e-4);
    const auto ref = forest.tree_of(forest.body_of({0, 0}, 0));
    GIANT_CHECK(ref.has_value() && ref->fallen);
    // Reset: standing again, its trunk made again by a sync.
    forest.take_redraw();
    forest.reset();
    GIANT_CHECK(forest.fallen().empty());
    GIANT_CHECK(forest.take_redraw().size() == 1);
    GIANT_CHECK(forest.body_of({0, 0}, 0) == k_no_body);
    forest.sync({0.0, 0.0, 0.0});
    GIANT_CHECK(forest.stats().trunks == 1);
    const auto standing = physics.state(forest.body_of({0, 0}, 0));
    GIANT_CHECK(standing.has_value() && standing->position == DVec3(20.0, 0.0, 20.0));
}

GIANT_TEST("forest: impact fells trees in reach strong enough to, away from it") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    ground(physics);
    Forest forest(physics);
    forest.set_cell({0, 0},
                    3,
                    {tree(0, {10.0, 0.0, 10.0}),           // 2 m from the blow: falls west
                     tree(1, {14.0, 0.0, 10.0}),           // 2 m: falls east
                     tree(2, {12.0, 0.0, 10.0 + 9.0}),     // 9 m: past the reach
                     tree(3, {12.0, 0.0, 6.0}, 1e7f)});    // too strong
    forest.flush({0.0, 0.0, 0.0});
    GIANT_CHECK(forest.impact({12.0, 1.0, 10.0}, 8.0f, 20000.0f) == 2);
    GIANT_CHECK(forest.stats().fallen == 2);
    GIANT_CHECK(forest.impact({12.0, 1.0, 10.0}, 8.0f, 20000.0f) == 0); // the fallen do not fall again
    GIANT_CHECK(forest.impact({12.0, 1.0, 10.0}, 0.0f, 20000.0f) == 0);  // bad numbers: nothing
    GIANT_CHECK(forest.impact({NAN, 1.0, 10.0}, 8.0f, 20000.0f) == 0);
    run(physics, forest, 300);
    const FallenTree* west = fallen(forest, {0, 0}, 0);
    const FallenTree* east = fallen(forest, {0, 0}, 1);
    GIANT_CHECK(west != nullptr && east != nullptr);
    if (west != nullptr && east != nullptr) {
        GIANT_CHECK(rotate(west->at.rotation, Vec3{0.0f, 1.0f, 0.0f}).x < -0.8f);
        GIANT_CHECK(rotate(east->at.rotation, Vec3{0.0f, 1.0f, 0.0f}).x > 0.8f);
    }
    GIANT_CHECK(fallen(forest, {0, 0}, 2) == nullptr);
    GIANT_CHECK(fallen(forest, {0, 0}, 3) == nullptr);
}

GIANT_TEST("forest: a falling tree fells the tree it lands on, unless that one is strong enough") {
    for (const bool strong : {false, true}) {
        JobSystem jobs(2);
        PhysicsWorld physics(jobs);
        ground(physics);
        Forest forest(physics);
        // A 20 m tree falling east onto a 10 m one 6 m away.
        TreeSpot big = tree(0, {10.0, 0.0, 10.0});
        big.height = 20.0f;
        big.trunk_radius = 0.5f;
        forest.set_cell({0, 0}, 1, {big, tree(1, {16.0, 0.0, 10.0}, strong ? 1e7f : world::k_default_tree_strength)});
        forest.flush({0.0, 0.0, 0.0});
        GIANT_CHECK(forest.impact({9.0, 1.0, 10.0}, 2.0f, 1e6f) == 1);
        run(physics, forest, 300);
        GIANT_CHECK((fallen(forest, {0, 0}, 1) != nullptr) == !strong);
        GIANT_CHECK(forest.stats().fallen == (strong ? 1u : 2u));
    }
}

GIANT_TEST("forest: a falling tree breaks the building it lands on") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    ScenePhysics follow(physics);
    Forest forest(physics);
    world::Scene scene;
    world::Entity plane;
    plane.shape = world::Shape::plane;
    plane.transform.scale = {200.0f, 1.0f, 200.0f};
    scene.add(plane);
    world::Kit shed;
    shed.name = "shed";
    shed.breakable = true;
    world::KitPiece roof;
    roof.transform.position = {0.0, 3.1, 0.0};
    roof.transform.scale = {4.0f, 0.2f, 4.0f};
    for (const double x : {-1.9, 1.9}) {
        world::KitPiece wall;
        wall.transform.position = {x, 1.5, 0.0};
        wall.transform.scale = {0.2f, 3.0f, 4.0f};
        shed.pieces.push_back(wall);
    }
    shed.pieces.push_back(roof);
    scene.kits.push_back(shed);
    world::Entity e;
    e.shape = world::Shape::kit;
    e.kit = "shed";
    e.transform.position = {18.0, 0.0, 10.0};
    const world::EntityId id = scene.add(e);
    const world::SceneChanges c = scene.take_changes();
    follow.flush(scene, c.ids, c.all);
    // A 20 m tree 8 m west of the shed, felled toward it.
    TreeSpot big = tree(0, {10.0, 0.0, 10.0});
    big.height = 20.0f;
    big.trunk_radius = 0.5f;
    forest.set_cell({0, 0}, 1, {big});
    forest.flush({0.0, 0.0, 0.0});
    GIANT_CHECK(forest.impact({9.0, 1.0, 10.0}, 2.0f, 1e6f) == 1);
    for (int i = 0; i < 300; ++i) {
        physics.step(PhysicsWorld::k_step_seconds);
        follow.update_poses();
        forest.after_step();
    }
    GIANT_CHECK(follow.piece_poses().contains(id));
    GIANT_CHECK(follow.stats().loose_pieces >= 1);
}

GIANT_TEST("forest: a cell placed again stands its trees afresh; a cell gone takes its trees") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    ground(physics);
    Forest forest(physics);
    forest.set_cell({0, 0}, 1, {tree(0, {10.0, 0.0, 10.0}), tree(4, {30.0, 0.0, 10.0})});
    forest.set_cell({1, 0}, 2, {}); // no trees: still known (not given again)
    forest.flush({0.0, 0.0, 0.0});
    GIANT_CHECK(forest.serial({1, 0}) == 2);
    GIANT_CHECK(forest.impact({10.0, 1.0, 9.0}, 3.0f, 1e5f) == 1);
    GIANT_CHECK(fallen(forest, {0, 0}, 0) != nullptr);
    forest.take_redraw();
    const usize bodies = physics.size();
    forest.set_cell({0, 0}, 5, {tree(0, {12.0, 0.0, 10.0})});
    GIANT_CHECK(forest.fallen().empty());
    GIANT_CHECK(forest.take_redraw().size() == 1);
    GIANT_CHECK(physics.size() == bodies - 1); // the falling one gone now, the standing one with the next sync
    forest.sync({0.0, 0.0, 0.0});
    GIANT_CHECK(physics.size() == bodies - 1); // and the new trunk made
    forest.remove_cell({0, 0});
    GIANT_CHECK(forest.serial({0, 0}) == 0);
    forest.sync({0.0, 0.0, 0.0});
    GIANT_CHECK(physics.size() == bodies - 2);
    forest.clear();
    GIANT_CHECK(physics.size() == 1); // the ground
    GIANT_CHECK(forest.cells().empty());
}

GIANT_TEST("forest: blows past what may fall at once are shrugged off") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    ground(physics);
    ForestSettings settings;
    settings.topples_per_step = 3;
    settings.max_fallen = 5;
    Forest forest(physics, settings);
    std::vector<TreeSpot> trees;
    for (u32 i = 0; i < 10; ++i) trees.push_back(tree(i, {2.0 + 6.0 * i, 0.0, 30.0}));
    forest.set_cell({0, 0}, 1, trees);
    forest.flush({0.0, 0.0, 0.0});
    GIANT_CHECK(forest.impact({30.0, 1.0, 30.0}, 60.0f, 1e7f) == 3);
    GIANT_CHECK(forest.stats().shrugged == 7);
    // The hardest struck first: the three nearest the blow (x 32, 26 and 38).
    GIANT_CHECK(fallen(forest, {0, 0}, 5) && fallen(forest, {0, 0}, 4) && fallen(forest, {0, 0}, 6));
    GIANT_CHECK(forest.impact({30.0, 1.0, 30.0}, 60.0f, 1e7f) == 2); // up to max_fallen
    GIANT_CHECK(forest.stats().fallen == 5);
    GIANT_CHECK(forest.impact({30.0, 1.0, 30.0}, 60.0f, 1e7f) == 0);
    // A fallen tree far off keeps its cell (and body) while its standing trunks go.
    forest.sync({5000.0, 0.0, 0.0});
    forest.sync({5000.0, 0.0, 0.0});
    GIANT_CHECK(forest.serial({0, 0}) == 1);
    GIANT_CHECK(forest.stats().trunks == 0);
    GIANT_CHECK(physics.size() == 1 + 5);
}

GIANT_TEST("forest: far from the camera a blow lays its trees down at once, tops on what is there, with dust") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    ground(physics);
    // A block 4 m tall east of the first tree, where its top comes down.
    BodyDesc block;
    block.parts.push_back({BoxShape{{5.0f, 2.0f, 5.0f}}});
    block.position = {90.0, 2.0, 20.0};
    physics.add(block);
    Dust dust;
    Forest forest(physics);
    forest.set_dust(&dust);
    forest.set_cell({1, 0}, 1, {tree(0, {80.0, 0.0, 20.0}), tree(1, {80.0, 0.0, 60.0}), tree(2, {150.0, 0.0, 20.0})});
    const DVec3 camera{5000.0, 0.0, 0.0};
    // (A sync this far off would forget the cell: given it for the blow, as the editor does.)
    const usize bodies = physics.size();
    GIANT_CHECK(forest.stats().trunks == 0);
    // Too weak: nothing.
    GIANT_CHECK(forest.impact({70.0, 1.0, 20.0}, 15.0f, 100.0f) == 0);
    GIANT_CHECK(forest.impact({70.0, 1.0, 20.0}, 15.0f, 1e6f) == 1);
    GIANT_CHECK(forest.impact({70.0, 1.0, 60.0}, 15.0f, 1e6f) == 1);
    ForestStats s = forest.stats();
    GIANT_CHECK(s.laid == 2 && s.fallen == 2 && s.falling == 0 && s.trunks == 0);
    GIANT_CHECK(physics.size() == bodies); // no bodies for them
    const FallenTree* onto = fallen(forest, {1, 0}, 0);
    const FallenTree* flat = fallen(forest, {1, 0}, 1);
    GIANT_CHECK(onto != nullptr && flat != nullptr && fallen(forest, {1, 0}, 2) == nullptr);
    if (onto != nullptr && flat != nullptr) {
        // Its top on the block (about 66 degrees over), to the east; the other flat on the ground.
        GIANT_CHECK(lean(onto->at) > 58.0f && lean(onto->at) < 72.0f);
        GIANT_CHECK(rotate(onto->at.rotation, Vec3{0.0f, 1.0f, 0.0f}).x > 0.8f);
        GIANT_CHECK(lean(flat->at) > 85.0f && lean(flat->at) < 95.0f);
        GIANT_CHECK(onto->at.position == DVec3(80.0, 0.0, 20.0)); // about its foot
    }
    GIANT_CHECK(!dust.puffs().empty());
    GIANT_CHECK(!forest.take_redraw().empty());
    // Far off, the cell stays for its fallen trees; standing up again, it goes.
    forest.sync(camera);
    GIANT_CHECK(forest.cells().size() == 1);
    forest.reset();
    s = forest.stats();
    GIANT_CHECK(s.laid == 0 && s.fallen == 0 && forest.fallen().empty());
    forest.sync(camera);
    GIANT_CHECK(forest.cells().empty());
}

GIANT_TEST("forest: at most lays_per_impact trees are laid down by one blow, the hardest struck first") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    ground(physics);
    ForestSettings settings;
    settings.lays_per_impact = 3;
    Forest forest(physics, settings);
    std::vector<TreeSpot> trees;
    for (u32 i = 0; i < 8; ++i) trees.push_back(tree(i, {2.0 + 4.0 * i, 0.0, 10.0}));
    forest.set_cell({0, 0}, 1, trees);
    GIANT_CHECK(forest.impact({0.0, 1.0, 10.0}, 40.0f, 1e6f) == 3);
    GIANT_CHECK(forest.stats().laid == 3 && forest.stats().shrugged == 5);
    // The nearest three (struck hardest).
    for (u32 i = 0; i < 8; ++i) GIANT_CHECK((fallen(forest, {0, 0}, i) != nullptr) == (i < 3));
}

GIANT_TEST("forest: a cell placed again keeps the fallen trees that stand where they stood (the ground dented)") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    ground(physics);
    Forest forest(physics);
    forest.set_cell({0, 0}, 1, {tree(0, {10.0, 0.0, 10.0}), tree(1, {30.0, 0.0, 10.0}), tree(2, {50.0, 0.0, 10.0})});
    forest.flush({0.0, 0.0, 0.0});
    GIANT_CHECK(forest.impact({10.0, 1.0, 9.0}, 3.0f, 1e5f) == 1);
    GIANT_CHECK(forest.impact({30.0, 1.0, 9.0}, 3.0f, 1e5f) == 1);
    run(physics, forest, 300);
    const BodyId body = forest.body_of({0, 0}, 0);
    GIANT_CHECK(body != k_no_body);
    const world::Transform lying = fallen(forest, {0, 0}, 0)->at;
    forest.take_redraw();
    // Placed again: point 0 where it stood (lower: a dent), point 1 moved, point 2 as it was.
    forest.set_cell({0, 0}, 2, {tree(0, {10.0, -1.0, 10.0}), tree(1, {31.0, 0.0, 10.0}), tree(2, {50.0, 0.0, 10.0})});
    GIANT_CHECK(forest.fallen().at({0, 0}).serial == 2);
    GIANT_CHECK(forest.fallen().at({0, 0}).trees.size() == 1);
    GIANT_CHECK(fallen(forest, {0, 0}, 0) != nullptr && fallen(forest, {0, 0}, 0)->at == lying);
    GIANT_CHECK(fallen(forest, {0, 0}, 1) == nullptr);
    GIANT_CHECK(forest.body_of({0, 0}, 0) == body && forest.tree_of(body)->fallen);
    GIANT_CHECK(forest.stats().fallen == 1);
    GIANT_CHECK(forest.take_redraw().size() == 1);
    // Its point renumbered (a dent took other points before it): kept by where it stands.
    forest.set_cell({0, 0}, 3, {tree(1, {10.0, -1.0, 10.0}), tree(2, {50.0, 0.0, 10.0})});
    GIANT_CHECK(fallen(forest, {0, 0}, 1) != nullptr && fallen(forest, {0, 0}, 0) == nullptr);
    GIANT_CHECK(forest.body_of({0, 0}, 1) == body && forest.tree_of(body)->point == 1);
    GIANT_CHECK(forest.stats().fallen == 1 && forest.fallen_in({0, 0}) == 1);
    // Another model there (bigger): stands up anew.
    TreeSpot big = tree(1, {10.0, -1.0, 10.0});
    big.height = 20.0f;
    forest.set_cell({0, 0}, 4, {big, tree(2, {50.0, 0.0, 10.0})});
    GIANT_CHECK(forest.stats().fallen == 0 && forest.fallen().empty());
    // Still a body of its own: what falls on goes on (reset stands it up and lets it go).
    forest.reset();
    forest.flush({0.0, 0.0, 0.0});
    GIANT_CHECK(forest.stats().fallen == 0 && forest.stats().trunks == 2);
    forest.clear();
    GIANT_CHECK(physics.size() == 1);
}

GIANT_TEST("forest: kept trees lie where their records say, stay down through reset, and stand when taken away") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    ground(physics);
    Forest forest(physics);
    // 25 m apart across the way they fall (+x): none falls on another.
    const std::vector<TreeSpot> spots{tree(0, {10.0, 0.0, 10.0}), tree(1, {10.0, 0.0, 35.0}), tree(2, {10.0, 0.0, 60.0})};
    forest.set_cell({0, 0}, 1, spots);
    forest.flush({0.0, 0.0, 0.0});
    GIANT_CHECK(forest.stats().trunks == 3);
    // Tree 0 felled; the forest's damage names it as it lies.
    GIANT_CHECK(forest.impact({8.0, 1.0, 10.0}, 3.0f, 1e6f) == 1);
    run(physics, forest, 240);
    auto damage = forest.damage();
    GIANT_CHECK(damage.size() == 1 && damage[0].second.size() == 1);
    if (damage.size() != 1 || damage[0].second.size() != 1) return;
    const world::KeptTree record = damage[0].second[0];
    GIANT_CHECK(record.x == 10.0 && record.z == 10.0 && record.height == 10.0f);
    GIANT_CHECK(lean({record.position, record.rotation, record.scale}) > 80.0f);
    // Kept: given again with its record, it lies there with no body.
    const usize bodies = physics.size();
    forest.set_cell({0, 0}, 2, spots, {record});
    forest.flush({0.0, 0.0, 0.0});
    ForestStats s = forest.stats();
    GIANT_CHECK(s.kept == 1 && s.fallen == 1 && s.laid == 1 && s.trunks == 2);
    GIANT_CHECK(physics.size() < bodies); // its fallen body gone
    const FallenTree* lying = fallen(forest, {0, 0}, 0);
    GIANT_CHECK(lying != nullptr && lying->at.position == record.position);
    GIANT_CHECK(forest.records({0, 0}) != nullptr && forest.records({0, 0})->size() == 1);
    // Another felled, then reset: the kept one stays down, the other stands.
    GIANT_CHECK(forest.impact({8.0, 1.0, 60.0}, 3.0f, 1e6f) == 1);
    run(physics, forest, 60);
    GIANT_CHECK(forest.stats().fallen == 2);
    forest.reset();
    forest.flush({0.0, 0.0, 0.0});
    s = forest.stats();
    GIANT_CHECK(s.kept == 1 && s.fallen == 1 && s.trunks == 2 && fallen(forest, {0, 0}, 0) != nullptr);
    GIANT_CHECK(fallen(forest, {0, 0}, 2) == nullptr);
    // A record for a tree another size, or none there, lays nothing.
    world::KeptTree other = record;
    other.height = 12.0f;
    world::KeptTree nowhere = record;
    nowhere.x = 40.0;
    forest.set_cell({0, 0}, 3, spots, {other, nowhere});
    forest.flush({0.0, 0.0, 0.0});
    s = forest.stats();
    GIANT_CHECK(s.kept == 0 && s.fallen == 0 && s.trunks == 3);
    // Points renumbered (another layer's point first): the record finds its tree by place.
    forest.set_cell({0, 0}, 4, {tree(0, {40.0, 0.0, 40.0}), tree(1, {10.0, 0.0, 10.0}), tree(2, {10.0, 0.0, 35.0})}, {record});
    GIANT_CHECK(fallen(forest, {0, 0}, 1) != nullptr && forest.stats().kept == 1);
    // Taken away (given again with none), it stands.
    forest.set_cell({0, 0}, 5, spots);
    forest.flush({0.0, 0.0, 0.0});
    s = forest.stats();
    GIANT_CHECK(s.kept == 0 && s.fallen == 0 && s.trunks == 3);
}
