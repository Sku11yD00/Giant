// The scene in the physics world: entities and ground tiles become bodies,
// edits follow, dynamic entities fall (as poses beside the scene), within
// per-sync budgets.
#include "giant/sim/scene_physics.h"

#include "giant/world/terrain.h"

#include "giant_test.h"

#include <chrono>
#include <cmath>
#include <random>
#include <thread>

using namespace giant;
using namespace giant::sim;
using world::Entity;
using world::EntityId;
using world::Scene;

namespace {

void run(PhysicsWorld& physics, ScenePhysics& follow, Scene& scene, int steps) {
    for (int i = 0; i < steps; ++i) {
        const world::SceneChanges c = scene.take_changes();
        follow.sync(scene, c.ids, c.all);
        physics.step(1.0 / 60.0);
        follow.update_poses();
    }
}

// Where an entity is drawn: its pose, or where the scene puts it.
DVec3 at(const ScenePhysics& follow, const Scene& scene, EntityId id) {
    const auto it = follow.poses().find(id);
    return it != follow.poses().end() ? it->second.position : scene.find(id)->transform.position;
}

Entity make(world::Shape shape, DVec3 at, Vec3 scale, world::BodyKind body = world::BodyKind::fixed) {
    Entity e;
    e.shape = shape;
    e.transform.position = at;
    e.transform.scale = scale;
    e.body = body;
    return e;
}

} // namespace

GIANT_TEST("scene physics: a dynamic cube falls onto a plane, a sphere onto a kit's roof") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    ScenePhysics follow(physics);
    Scene scene;
    scene.add(make(world::Shape::plane, {0.0, 0.0, 0.0}, {100.0f, 1.0f, 100.0f}));
    const EntityId cube = scene.add(make(world::Shape::cube, {0.0, 4.0, 0.0}, {1.0f, 2.0f, 1.0f},
                                         world::BodyKind::dynamic));
    // A house kit: walls 3 m tall, a roof slab on top (y 3 to 3.6).
    world::Kit house{"house", {}};
    world::KitPiece walls;
    walls.shape = world::Shape::cube;
    walls.transform.position = {0.0, 1.5, 0.0};
    walls.transform.scale = {4.0f, 3.0f, 4.0f};
    world::KitPiece roof;
    roof.shape = world::Shape::cube;
    roof.transform.position = {0.0, 3.3, 0.0};
    roof.transform.scale = {4.4f, 0.6f, 4.4f};
    house.pieces = {walls, roof};
    scene.kits.push_back(house);
    Entity kit = make(world::Shape::kit, {20.0, 0.0, 0.0}, {1.0f, 1.0f, 1.0f}, world::BodyKind::dynamic);
    kit.kit = "house";
    const EntityId home = scene.add(kit);
    const EntityId ball = scene.add(make(world::Shape::sphere, {20.0, 8.0, 0.0}, {0.6f, 0.6f, 0.6f},
                                         world::BodyKind::dynamic));
    Entity decal = make(world::Shape::decal, {5.0, 0.0, 0.0}, {1.0f, 1.0f, 1.0f});
    scene.add(decal);
    run(physics, follow, scene, 180);
    // The cube stands on the plane (half its 2 m height up), the ball on the
    // roof (3.6 m plus its 0.3 m radius); the kit stays where it is (fixed).
    GIANT_CHECK(std::fabs(at(follow, scene, cube).y - 1.0) < 0.03);
    GIANT_CHECK(std::fabs(at(follow, scene, ball).y - 3.9) < 0.03);
    GIANT_CHECK(at(follow, scene, home) == DVec3(20.0, 0.0, 0.0));
    // The scene still holds where they were put; the poses where they are.
    GIANT_CHECK(scene.find(cube)->transform.position == DVec3(0.0, 4.0, 0.0) && follow.poses().size() == 2);
    GIANT_CHECK(scene.take_changes().ids.empty());
    const ScenePhysicsStats s = follow.stats();
    GIANT_CHECK(s.entity_bodies == 4 && s.dynamic_bodies == 2 && s.not_colliding == 1 && s.refused == 0 && s.posed == 2);
    GIANT_CHECK(follow.entity_of(follow.body_of(cube)) == cube);
    // Put back: the bodies where the scene says, no poses, both drawn again.
    (void)follow.take_redraw();
    follow.reset_poses();
    GIANT_CHECK(follow.poses().empty() && follow.take_redraw().size() == 2);
    GIANT_CHECK(physics.state(follow.body_of(cube))->position == DVec3(0.0, 4.0, 0.0));
    run(physics, follow, scene, 180); // and they fall again
    GIANT_CHECK(std::fabs(at(follow, scene, cube).y - 1.0) < 0.03);
}

GIANT_TEST("scene physics: edits follow: moved, resized, made still, deleted, a new scene") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    ScenePhysics follow(physics);
    Scene scene;
    scene.add(make(world::Shape::plane, {0.0, 0.0, 0.0}, {100.0f, 1.0f, 100.0f}));
    const EntityId cube =
        scene.add(make(world::Shape::cube, {0.0, 0.5, 0.0}, {1.0f, 1.0f, 1.0f}, world::BodyKind::dynamic));
    run(physics, follow, scene, 30);
    const BodyId body = follow.body_of(cube);
    // Moved by an edit: the same body, put there, and it falls from there.
    scene.modify(cube)->transform.position = {10.0, 5.0, 0.0};
    run(physics, follow, scene, 2);
    GIANT_CHECK(follow.body_of(cube) == body);
    run(physics, follow, scene, 120);
    const DVec3 p = at(follow, scene, cube);
    GIANT_CHECK(std::fabs(p.x - 10.0) < 0.01 && std::fabs(p.y - 0.5) < 0.03);
    // Resized: a new body, 4 m tall now: it rests 2 m up.
    scene.modify(cube)->transform.scale = {1.0f, 4.0f, 1.0f};
    run(physics, follow, scene, 120);
    GIANT_CHECK(follow.body_of(cube) != body && std::fabs(at(follow, scene, cube).y - 2.0) < 0.03);
    // Body none: no collision; deleted: no body.
    scene.modify(cube)->body = world::BodyKind::none;
    run(physics, follow, scene, 1);
    GIANT_CHECK(follow.body_of(cube) == k_no_body && follow.stats().not_colliding == 1);
    scene.remove(cube);
    run(physics, follow, scene, 1);
    GIANT_CHECK(physics.size() == 1 && follow.stats().entity_bodies == 1);
    // A new scene (an assignment): everything again.
    scene = Scene();
    scene.add(make(world::Shape::cube, {0.0, 0.5, 0.0}, {1.0f, 1.0f, 1.0f}));
    run(physics, follow, scene, 1);
    GIANT_CHECK(physics.size() == 1 && follow.stats().entity_bodies == 1);
}

GIANT_TEST("scene physics: the ground's tiles collide where the mesh is, edits and all") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    ScenePhysics follow(physics);
    Scene scene;
    scene.terrain.set_samples(16);
    world::HillsSettings hills;
    hills.amplitude = 12.0f;
    hills.size = 60.0f;
    // Four tiles, 2 x 2 cells round the origin.
    for (i32 cz = -1; cz <= 0; ++cz) {
        for (i32 cx = -1; cx <= 0; ++cx) {
            world::Terrain::Heights h(16 * 16);
            for (u32 j = 0; j < 16; ++j)
                for (u32 i = 0; i < 16; ++i)
                    h[j * 16 + i] = world::hills_height(hills, cx * 64.0 + i * 4.0, cz * 64.0 + j * 4.0);
            scene.terrain.set({cx, cz}, std::move(h));
        }
    }
    follow.sync(scene, {}, true);
    GIANT_CHECK(follow.stats().tile_bodies == 0 && follow.stats().tiles_waiting == 4); // shapes on their way
    follow.flush(scene);
    GIANT_CHECK(follow.stats().tile_bodies == 4 && follow.stats().tiles_waiting == 0);
    // Rays down meet the ground where height_at says (the mesh's own
    // triangles), across tile edges too.
    std::mt19937 rng(5);
    std::uniform_real_distribution<double> at(-63.0, 63.0);
    int checked = 0;
    for (int k = 0; k < 200; ++k) {
        const double x = at(rng);
        const double z = at(rng);
        const std::optional<double> h = scene.terrain.height_at(x, z);
        if (!h) continue;
        const auto hit = physics.raycast({x, 100.0, z}, {0.0f, -1.0f, 0.0f}, 200.0);
        GIANT_CHECK(hit && std::fabs(hit->point.y - *h) < 0.01);
        ++checked;
    }
    GIANT_CHECK(checked > 150);
    // An edit to one tile remakes it, and the tiles whose edge it is.
    world::Terrain::Heights* h = scene.terrain.modify({0, 0});
    (*h)[0] += 5.0f; // its corner: also the edge of the three tiles before it
    const u64 before = physics.stats().steps;
    run(physics, follow, scene, 1);
    GIANT_CHECK(physics.stats().steps == before + 1);
    follow.flush(scene);
    const auto hit = physics.raycast({0.0, 100.0, 0.0}, {0.0f, -1.0f, 0.0f}, 200.0);
    GIANT_CHECK(hit && std::fabs(hit->point.y - *scene.terrain.height_at(0.0, 0.0)) < 0.01);
    // A tile removed: no ground there.
    scene.terrain.remove({-1, -1});
    follow.flush(scene);
    GIANT_CHECK(follow.stats().tile_bodies == 3 && !physics.raycast({-30.0, 100.0, -30.0}, {0, -1, 0}, 200.0));
}

GIANT_TEST("scene physics: what rested on something taken away falls; the ground first, then what falls") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    ScenePhysics follow(physics);
    Scene scene;
    scene.add(make(world::Shape::plane, {0.0, 0.0, 0.0}, {100.0f, 1.0f, 100.0f}));
    const EntityId table = scene.add(make(world::Shape::cube, {0.0, 1.0, 0.0}, {2.0f, 2.0f, 2.0f}));
    const EntityId crate =
        scene.add(make(world::Shape::cube, {0.0, 2.5, 0.0}, {1.0f, 1.0f, 1.0f}, world::BodyKind::dynamic));
    run(physics, follow, scene, 240);
    GIANT_CHECK(std::fabs(at(follow, scene, crate).y - 2.5) < 0.03 && physics.stats().active == 0); // asleep on it
    scene.remove(table);
    run(physics, follow, scene, 120);
    GIANT_CHECK(std::fabs(at(follow, scene, crate).y - 0.5) < 0.03); // woken, on the plane
}

GIANT_TEST("scene physics: a sync does a bounded share; a dynamic body waits for the ground round it") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    ScenePhysicsSettings budget;
    budget.entities_per_sync = 100;
    budget.tiles_per_sync = 1;
    ScenePhysics follow(physics, budget);
    Scene scene;
    scene.terrain.set_samples(8);
    scene.terrain.set({0, 0}, world::Terrain::Heights(64, 0.0f));
    const EntityId crate =
        scene.add(make(world::Shape::cube, {10.0, 3.0, 10.0}, {1.0f, 1.0f, 1.0f}, world::BodyKind::dynamic));
    for (int i = 0; i < 250; ++i) scene.add(make(world::Shape::cube, {200.0 + 2.0 * i, 0.5, 300.0}, {1.0f, 1.0f, 1.0f}));
    const world::SceneChanges c = scene.take_changes();
    follow.sync(scene, c.ids, c.all);
    ScenePhysicsStats s = follow.stats();
    // At most the budget (an eighth kept for dynamic ones), the rest waiting.
    GIANT_CHECK(s.entity_bodies >= 80 && s.entity_bodies <= 100 && s.waiting == 251 - s.entity_bodies);
    GIANT_CHECK(follow.body_of(crate) == k_no_body);
    for (int i = 0; i < 2; ++i) follow.sync(scene, {}, false);
    s = follow.stats();
    const bool crate_in = follow.body_of(crate) != k_no_body;
    GIANT_CHECK(s.entity_bodies == 250u + (crate_in ? 1u : 0u)); // the fixed ones first
    GIANT_CHECK(!crate_in || s.tile_bodies == 1);
    // The crate's body comes once the tile's shape is in: it never falls through.
    // (By the clock: the tile's worker may be slow to get a core.)
    const auto start = std::chrono::steady_clock::now();
    while (follow.body_of(crate) == k_no_body && std::chrono::steady_clock::now() - start < std::chrono::seconds(60)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        follow.sync(scene, {}, false);
        physics.step(1.0 / 60.0);
        follow.update_poses();
        if (follow.body_of(crate) == k_no_body) GIANT_CHECK(follow.stats().tiles_waiting == 1 || follow.stats().tile_bodies == 1);
    }
    GIANT_CHECK(follow.body_of(crate) != k_no_body && follow.stats().tile_bodies == 1);
    run(physics, follow, scene, 120);
    GIANT_CHECK(std::fabs(at(follow, scene, crate).y - 0.5) < 0.03);
}

GIANT_TEST("scene physics: a crate on a crate falls when the one under it goes; kits count their pieces") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    ScenePhysicsSettings budget;
    budget.entities_per_sync = 64;
    ScenePhysics follow(physics, budget);
    Scene scene;
    scene.add(make(world::Shape::plane, {0.0, 0.0, 0.0}, {100.0f, 1.0f, 100.0f}));
    const EntityId bottom =
        scene.add(make(world::Shape::cube, {0.0, 0.5, 0.0}, {1.0f, 1.0f, 1.0f}, world::BodyKind::dynamic));
    const EntityId top =
        scene.add(make(world::Shape::cube, {0.0, 1.5, 0.0}, {1.0f, 1.0f, 1.0f}, world::BodyKind::dynamic));
    run(physics, follow, scene, 240);
    GIANT_CHECK(std::fabs(at(follow, scene, top).y - 1.5) < 0.03 && physics.stats().active == 0);
    scene.remove(bottom);
    run(physics, follow, scene, 120);
    GIANT_CHECK(std::fabs(at(follow, scene, top).y - 0.5) < 0.03);
    // A kit of 40 pieces costs 40 of the budget: 64 a sync makes one or two placements.
    world::Kit row{"row", {}};
    for (int i = 0; i < 40; ++i) {
        world::KitPiece p;
        p.shape = world::Shape::cube;
        p.transform.position = {static_cast<double>(i), 0.5, 0.0};
        row.pieces.push_back(p);
    }
    scene.kits.push_back(row);
    for (int i = 0; i < 10; ++i) {
        Entity k = make(world::Shape::kit, {0.0, 0.0, 10.0 + 3.0 * i}, {1.0f, 1.0f, 1.0f});
        k.kit = "row";
        scene.add(k);
    }
    const u32 before = follow.stats().entity_bodies;
    const world::SceneChanges c = scene.take_changes();
    follow.sync(scene, c.ids, c.all);
    GIANT_CHECK(follow.stats().entity_bodies - before <= 2 && follow.stats().waiting >= 8);
    follow.flush(scene);
    GIANT_CHECK(follow.stats().entity_bodies - before == 10 && follow.stats().waiting == 0);
    // A new scene: the world starts afresh (the old let go off the frame).
    scene = Scene();
    scene.add(make(world::Shape::cube, {0.0, 0.5, 0.0}, {1.0f, 1.0f, 1.0f}));
    follow.flush(scene);
    GIANT_CHECK(physics.size() == 1 && follow.stats().entity_bodies == 1);
}

GIANT_TEST("scene physics: an edit to a falling crate is seen at once, behind a stream of fixed entities") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    ScenePhysicsSettings budget;
    budget.entities_per_sync = 64;
    ScenePhysics follow(physics, budget);
    Scene scene;
    scene.add(make(world::Shape::plane, {0.0, 0.0, 0.0}, {100.0f, 1.0f, 100.0f}));
    const EntityId crate =
        scene.add(make(world::Shape::cube, {0.0, 100.0, 0.0}, {1.0f, 1.0f, 1.0f}, world::BodyKind::dynamic));
    run(physics, follow, scene, 30);
    GIANT_CHECK(at(follow, scene, crate).y < 99.0); // falling
    for (int i = 0; i < 2000; ++i) scene.add(make(world::Shape::cube, {300.0 + i, 0.5, 0.0}, {1.0f, 1.0f, 1.0f}));
    scene.modify(crate)->transform.position = {0.0, 500.0, 0.0};
    run(physics, follow, scene, 1);
    GIANT_CHECK(follow.stats().waiting > 1000);                    // the stream still going in
    GIANT_CHECK(std::fabs(at(follow, scene, crate).y - 500.0) < 0.1); // but the crate is where it was put
}

GIANT_TEST("scene physics: a dynamic entity placed where a save had it rests there, posed; others are refused") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    ScenePhysics follow(physics);
    Scene scene;
    const EntityId floor = scene.add(make(world::Shape::plane, {0.0, 0.0, 0.0}, {100.0f, 1.0f, 100.0f}));
    const EntityId cube = scene.add(make(world::Shape::cube, {0.0, 4.0, 0.0}, {1.0f, 1.0f, 1.0f},
                                         world::BodyKind::dynamic));
    run(physics, follow, scene, 2);
    (void)follow.take_redraw();
    const Quat turned = normalize(Quat{0.0f, 0.38268343f, 0.0f, 0.92387953f});
    GIANT_CHECK(follow.place(cube, {7.0, 0.5, -3.0}, turned));
    GIANT_CHECK(follow.poses().at(cube).position == DVec3(7.0, 0.5, -3.0) && follow.take_redraw().size() == 1);
    run(physics, follow, scene, 60); // it rests there (on the plane)
    GIANT_CHECK(std::fabs(at(follow, scene, cube).x - 7.0) < 0.01 && std::fabs(at(follow, scene, cube).y - 0.5) < 0.02);
    // Not dynamic, not there, bad numbers: refused.
    GIANT_CHECK(!follow.place(floor, {0.0, 1.0, 0.0}, Quat{}));
    GIANT_CHECK(!follow.place(999, {0.0, 1.0, 0.0}, Quat{}));
    GIANT_CHECK(!follow.place(cube, {std::nan(""), 1.0, 0.0}, Quat{}));
    GIANT_CHECK(!follow.place(cube, {0.0, 1.0, 0.0}, Quat{0.0f, 0.0f, 0.0f, 2.0f}));
    follow.reset_poses();
    GIANT_CHECK(follow.poses().empty());
}
