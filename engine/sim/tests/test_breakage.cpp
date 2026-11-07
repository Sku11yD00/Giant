// Breakable kits: pieces struck hard enough come loose, what they held up
// falls with them, and stopping puts everything back.
#include "giant/sim/scene_physics.h"

#include "giant_test.h"

#include <cmath>

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

world::KitPiece cube(DVec3 at, Vec3 scale) {
    world::KitPiece p;
    p.shape = world::Shape::cube;
    p.transform.position = at;
    p.transform.scale = scale;
    return p;
}

// A ground plane 200 m across, top at y = 0.
void ground(Scene& scene) {
    Entity e;
    e.shape = world::Shape::plane;
    e.transform.scale = {200.0f, 1.0f, 200.0f};
    scene.add(e);
}

EntityId place(Scene& scene, const std::string& kit, DVec3 at) {
    Entity e;
    e.shape = world::Shape::kit;
    e.kit = kit;
    e.transform.position = at;
    return scene.add(e);
}

// Three 1 m blocks stacked: 0 at the bottom (y 0 to 1), 1, then 2 on top.
world::Kit tower(bool breakable) {
    world::Kit k;
    k.name = "tower";
    k.breakable = breakable;
    for (int i = 0; i < 3; ++i) k.pieces.push_back(cube({0.0, 0.5 + i, 0.0}, {1.0f, 1.0f, 1.0f}));
    return k;
}

// A 4 m house: four walls 3 m tall, 0.2 thick, and a roof slab on them.
world::Kit house() {
    world::Kit k;
    k.name = "house";
    k.breakable = true;
    k.pieces.push_back(cube({0.0, 1.5, -1.9}, {4.0f, 3.0f, 0.2f})); // north
    k.pieces.push_back(cube({0.0, 1.5, 1.9}, {4.0f, 3.0f, 0.2f}));  // south
    k.pieces.push_back(cube({-1.9, 1.5, 0.0}, {0.2f, 3.0f, 3.6f})); // west
    k.pieces.push_back(cube({1.9, 1.5, 0.0}, {0.2f, 3.0f, 3.6f}));  // east
    k.pieces.push_back(cube({0.0, 3.1, 0.0}, {4.0f, 0.2f, 4.0f}));  // roof
    return k;
}

const PiecePose* loose(const ScenePhysics& follow, EntityId id, u32 piece) {
    const auto it = follow.piece_poses().find(id);
    if (it == follow.piece_poses().end()) return nullptr;
    for (const PiecePose& p : it->second) {
        if (p.piece == piece) return &p;
    }
    return nullptr;
}

} // namespace

GIANT_TEST("breakage: a blow knocks out the middle of a tower, the top falls with it, reset puts it back") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    ScenePhysics follow(physics);
    Scene scene;
    ground(scene);
    scene.kits.push_back(tower(true));
    const EntityId t = place(scene, "tower", {0.0, 0.0, 0.0});
    run(physics, follow, scene, 1);
    // Too weak: nothing.
    GIANT_CHECK(follow.impact({0.6, 1.5, 0.0}, 0.3f, 1000.0f) == 0 && follow.stats().broken_kits == 0);
    // Hard, at the middle block's side: it and the block it held up come loose.
    GIANT_CHECK(follow.impact({0.6, 1.5, 0.0}, 0.3f, 6000.0f) == 2);
    ScenePhysicsStats s = follow.stats();
    GIANT_CHECK(s.broken_kits == 1 && s.loose_pieces == 2 && loose(follow, t, 0) == nullptr);
    GIANT_CHECK(loose(follow, t, 1) != nullptr && loose(follow, t, 2) != nullptr);
    run(physics, follow, scene, 240);
    // Knocked away from the blow (-x), down on the ground; the bottom block stands.
    const PiecePose* middle = loose(follow, t, 1);
    const PiecePose* top = loose(follow, t, 2);
    GIANT_CHECK(middle != nullptr && middle->at.position.x < -0.3 && middle->at.position.y < 1.2);
    GIANT_CHECK(top != nullptr && top->at.position.y < 2.0);
    const auto hit = physics.raycast({0.0, 10.0, 0.0}, {0.0f, -1.0f, 0.0f}, 20.0);
    GIANT_CHECK(hit && follow.entity_of(hit->body) == t);
    // Reset: whole again, one body, nothing loose.
    follow.reset_poses();
    s = follow.stats();
    GIANT_CHECK(s.broken_kits == 0 && s.loose_pieces == 0 && follow.piece_poses().empty());
    const auto again = physics.raycast({0.0, 10.0, 0.0}, {0.0f, -1.0f, 0.0f}, 20.0);
    GIANT_CHECK(again && std::fabs(again->point.y - 3.0) < 0.01 && follow.entity_of(again->body) == t);
    // A kit that is not breakable does not break.
    scene.kits[0].breakable = false;
    run(physics, follow, scene, 2);
    GIANT_CHECK(follow.impact({0.6, 1.5, 0.0}, 0.3f, 1e6f) == 0);
}

GIANT_TEST("breakage: a heavy ball thrown at a house knocks a wall out; the roof stays on the other three") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    ScenePhysics follow(physics);
    Scene scene;
    ground(scene);
    scene.kits.push_back(house());
    const EntityId h = place(scene, "house", {0.0, 0.0, 0.0});
    run(physics, follow, scene, 1);
    // 500 kg at 20 m/s into the east wall (about 10000 N s).
    BodyDesc ball;
    ball.motion = Motion::dynamic;
    ball.mass = 500.0f;
    ball.position = {6.0, 1.5, 0.0};
    ball.linear_velocity = {-20.0f, 0.0f, 0.0f};
    ball.parts.push_back({SphereShape{0.5f}});
    physics.add(ball);
    run(physics, follow, scene, 180);
    const ScenePhysicsStats s = follow.stats();
    GIANT_CHECK(s.broken_kits == 1 && loose(follow, h, 3) != nullptr);
    // The roof rests on the other walls; they stand.
    GIANT_CHECK(loose(follow, h, 4) == nullptr && loose(follow, h, 0) == nullptr && loose(follow, h, 1) == nullptr);
    const PiecePose* wall = loose(follow, h, 3);
    GIANT_CHECK(wall != nullptr && wall->at.position.y < 1.4); // fallen over
}

GIANT_TEST("breakage: a wall knocked out from under a roof on two walls brings the roof down; edits mend") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    ScenePhysics follow(physics);
    Scene scene;
    ground(scene);
    // A shelter: two walls and a roof on them.
    world::Kit k = house();
    k.name = "shelter";
    k.pieces.erase(k.pieces.begin(), k.pieces.begin() + 2); // west, east, roof
    scene.kits.push_back(k);
    const EntityId h = place(scene, "shelter", {0.0, 0.0, 0.0});
    run(physics, follow, scene, 1);
    // Both walls out: the roof (held by nothing on the ground) falls too.
    GIANT_CHECK(follow.impact({-2.3, 1.5, 0.0}, 0.5f, 5000.0f) == 1);
    GIANT_CHECK(loose(follow, h, 2) == nullptr); // the east wall still holds it
    GIANT_CHECK(follow.impact({2.3, 1.5, 0.0}, 0.5f, 5000.0f) == 2);
    GIANT_CHECK(loose(follow, h, 2) != nullptr && follow.stats().loose_pieces == 3);
    run(physics, follow, scene, 180);
    GIANT_CHECK(loose(follow, h, 2)->at.position.y < 2.5); // down (on the fallen walls)
    // Moving the kit mends it, where it now is.
    scene.modify(h)->transform.position = {10.0, 0.0, 0.0};
    run(physics, follow, scene, 2);
    GIANT_CHECK(follow.stats().broken_kits == 0 && follow.piece_poses().empty());
    const auto hit = physics.raycast({10.0, 10.0, 0.0}, {0.0f, -1.0f, 0.0f}, 20.0);
    GIANT_CHECK(hit && follow.entity_of(hit->body) == h && std::fabs(hit->point.y - 3.2) < 0.01);
    // A colour change does not.
    GIANT_CHECK(follow.impact({7.7, 1.5, 0.0}, 0.5f, 5000.0f) == 1);
    scene.modify(h)->material.base_color = {1.0f, 0.0f, 0.0f};
    run(physics, follow, scene, 2);
    GIANT_CHECK(follow.stats().broken_kits == 1);
    // Gone from the scene: its bodies go.
    scene.remove(h);
    run(physics, follow, scene, 2);
    GIANT_CHECK(follow.stats().broken_kits == 0 && follow.stats().loose_pieces == 0 && physics.size() == 1);
}

GIANT_TEST("breakage: a kit made breakable after it was placed reports hits on every piece struck") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    ScenePhysics follow(physics);
    Scene scene;
    ground(scene);
    // Two 1 m blocks side by side, not breakable yet.
    world::Kit k;
    k.name = "pair";
    k.pieces.push_back(cube({-0.5, 0.5, 0.0}, {1.0f, 1.0f, 1.0f}));
    k.pieces.push_back(cube({0.5, 0.5, 0.0}, {1.0f, 1.0f, 1.0f}));
    scene.kits.push_back(k);
    const EntityId p = place(scene, "pair", {0.0, 0.0, 0.0});
    run(physics, follow, scene, 2);
    scene.kits[0].breakable = true; // as set_kit_breakable does, with the body made
    run(physics, follow, scene, 2);
    // A tonne dropped from 6 m across both.
    BodyDesc box;
    box.motion = Motion::dynamic;
    box.mass = 1000.0f;
    box.position = {0.0, 7.0, 0.0};
    box.parts.push_back({BoxShape{{1.5f, 0.5f, 0.5f}}});
    physics.add(box);
    run(physics, follow, scene, 120);
    GIANT_CHECK(loose(follow, p, 0) != nullptr && loose(follow, p, 1) != nullptr);
}

GIANT_TEST("breakage: a flattened piece never collides, the rest still break true; a kit all loose still counts") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    ScenePhysics follow(physics);
    Scene scene;
    ground(scene);
    // Piece 0 a disc flattened to nothing (no shape), then two cubes.
    world::Kit k;
    k.name = "odd";
    k.breakable = true;
    world::KitPiece disc = cube({0.0, 5.0, 0.0}, {1.0f, 0.0f, 1.0f});
    disc.shape = world::Shape::sphere;
    k.pieces.push_back(disc);
    k.pieces.push_back(cube({-3.0, 0.5, 0.0}, {1.0f, 1.0f, 1.0f}));
    k.pieces.push_back(cube({3.0, 0.5, 0.0}, {1.0f, 1.0f, 1.0f}));
    scene.kits.push_back(k);
    const EntityId o = place(scene, "odd", {0.0, 0.0, 0.0});
    run(physics, follow, scene, 2);
    // A ball into piece 2: that one comes loose, not piece 1.
    BodyDesc ball;
    ball.motion = Motion::dynamic;
    ball.mass = 500.0f;
    ball.position = {7.0, 0.5, 0.0};
    ball.linear_velocity = {-20.0f, 0.0f, 0.0f};
    ball.parts.push_back({SphereShape{0.5f}});
    physics.add(ball);
    run(physics, follow, scene, 60);
    GIANT_CHECK(loose(follow, o, 2) != nullptr && loose(follow, o, 1) == nullptr && loose(follow, o, 0) == nullptr);
    // The other cube too: every colliding piece loose, the placement still counted, not refused.
    GIANT_CHECK(follow.impact({-3.6, 0.5, 0.0}, 0.3f, 1e5f) == 1);
    const ScenePhysicsStats s = follow.stats();
    GIANT_CHECK(s.refused == 0 && s.entity_bodies == 2 && s.loose_pieces == 2);
    run(physics, follow, scene, 30); // and they fall without trouble
}

GIANT_TEST("breakage: placements waiting to be made again after their kit changed do not break by the old pieces") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    ScenePhysicsSettings settings;
    settings.entities_per_sync = 1; // one tower made a sync
    ScenePhysics follow(physics, settings);
    Scene scene;
    ground(scene);
    scene.kits.push_back(tower(true));
    place(scene, "tower", {0.0, 0.0, 0.0});
    place(scene, "tower", {10.0, 0.0, 0.0});
    run(physics, follow, scene, 4);
    GIANT_CHECK(follow.impact({0.6, 2.5, 0.0}, 0.3f, 6000.0f) == 1);
    GIANT_CHECK(follow.impact({10.6, 2.5, 0.0}, 0.3f, 6000.0f) == 1);
    // The kit is cut to its bottom block; one placement is made again a sync.
    scene.kits[0].pieces.resize(1);
    run(physics, follow, scene, 1);
    GIANT_CHECK(follow.stats().broken_kits == 1); // the other still waits, broken
    // The one made again has no middle block now; the one waiting holds the
    // old pieces, so a blow waits for it (it must not break by them).
    GIANT_CHECK(follow.impact({0.6, 1.5, 0.0}, 0.3f, 6000.0f) == 0);
    GIANT_CHECK(follow.impact({10.6, 1.5, 0.0}, 0.3f, 6000.0f) == 0);
    run(physics, follow, scene, 4);
    // Both made again from the new kit: they break as it is now.
    follow.reset_poses();
    run(physics, follow, scene, 1);
    GIANT_CHECK(follow.impact({0.6, 1.5, 0.0}, 0.3f, 6000.0f) == 0); // nothing there now
    GIANT_CHECK(follow.impact({0.6, 0.5, 0.0}, 0.3f, 6000.0f) == 1); // the one block
}

GIANT_TEST("breakage: far from the camera loose pieces come down at once as rubble, flat on the ground, with dust") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    ScenePhysics follow(physics);
    Dust dust;
    follow.set_dust(&dust);
    follow.set_camera({2000.0, 10.0, 0.0}); // past simulate_radius (600 m)
    Scene scene;
    ground(scene);
    scene.kits.push_back(house());
    const EntityId h = place(scene, "house", {0.0, 0.0, 0.0});
    run(physics, follow, scene, 1);
    const usize bodies = physics.size();
    // A blow on the whole house: every piece comes loose, none a body.
    GIANT_CHECK(follow.impact({0.0, 1.5, 0.0}, 10.0f, 1e6f) == 5);
    ScenePhysicsStats s = follow.stats();
    GIANT_CHECK(s.broken_kits == 1 && s.loose_pieces == 0 && s.rubble == 5);
    GIANT_CHECK(physics.size() == bodies - 1); // the house's body gone, no new ones
    for (u32 i = 0; i < 5; ++i) {
        const PiecePose* p = loose(follow, h, i);
        GIANT_CHECK(p != nullptr);
        if (p == nullptr) continue;
        // Its thinnest side (0.2 m) down, on the ground.
        GIANT_CHECK(std::fabs(p->at.position.y - 0.1) < 0.02);
        const Vec3 s3 = p->at.scale;
        const float thin = std::min({s3.x, s3.y, s3.z});
        const Vec3 up = rotate(p->at.rotation, Vec3{s3.x == thin ? 1.0f : 0.0f, s3.y == thin ? 1.0f : 0.0f,
                                                    s3.z == thin ? 1.0f : 0.0f});
        GIANT_CHECK(std::fabs(std::fabs(up.y) - 1.0f) < 1e-3f);
    }
    GIANT_CHECK(dust.puffs().size() == 1);
    // Rubble lies still: steps move nothing of it.
    const PiecePose before = *loose(follow, h, 4);
    run(physics, follow, scene, 30);
    GIANT_CHECK(loose(follow, h, 4)->at == before.at);
    follow.reset_poses();
    s = follow.stats();
    GIANT_CHECK(s.broken_kits == 0 && s.rubble == 0 && follow.piece_poses().empty());
}

GIANT_TEST("breakage: near the camera at most max_loose pieces fall as bodies, the rest as rubble") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    ScenePhysicsSettings settings;
    settings.max_loose = 2;
    ScenePhysics follow(physics, settings);
    follow.set_camera({10.0, 2.0, 10.0});
    Scene scene;
    ground(scene);
    scene.kits.push_back(house());
    const EntityId h = place(scene, "house", {0.0, 0.0, 0.0});
    run(physics, follow, scene, 1);
    GIANT_CHECK(follow.impact({0.0, 1.5, 0.0}, 10.0f, 1e6f) == 5);
    ScenePhysicsStats s = follow.stats();
    GIANT_CHECK(s.loose_pieces == 2 && s.rubble == 3);
    run(physics, follow, scene, 120);
    for (u32 i = 0; i < 5; ++i) {
        const PiecePose* p = loose(follow, h, i);
        GIANT_CHECK(p != nullptr && p->at.position.y > -0.5 && p->at.position.y < 3.0);
    }
    // A second house: no room, all rubble.
    const EntityId h2 = place(scene, "house", {30.0, 0.0, 0.0});
    run(physics, follow, scene, 2);
    GIANT_CHECK(follow.impact({30.0, 1.5, 0.0}, 10.0f, 1e6f) == 5);
    s = follow.stats();
    GIANT_CHECK(s.loose_pieces == 2 && s.rubble == 8 && s.broken_kits == 2);
    GIANT_CHECK(loose(follow, h2, 4) != nullptr);
}
