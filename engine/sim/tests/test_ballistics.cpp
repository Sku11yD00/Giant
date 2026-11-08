// Bullets: they drop and slow as they fly, go through what their energy
// takes them through, glance off hard things at shallow angles, and stop
// in the rest.
#include "giant/sim/ballistics.h"

#include "giant_test.h"

#include <cmath>
#include <unordered_map>

using namespace giant;
using namespace giant::sim;

namespace {

// A wall of `thickness` metres across x at `x`, 4 m square.
BodyDesc wall(double x, float thickness) {
    BodyDesc d;
    d.position = {x, 0.0, 0.0};
    d.parts.push_back({BoxShape{{thickness * 0.5f, 2.0f, 2.0f}}});
    return d;
}

struct Range {
    JobSystem jobs{2};
    PhysicsWorld physics{jobs};
    Ballistics bullets{physics};
    std::unordered_map<BodyId, world::Surface> surfaces;

    Range() {
        bullets.surface_of = [this](const RayHit& hit) {
            const auto it = surfaces.find(hit.body);
            return it == surfaces.end() ? world::Surface::standard : it->second;
        };
    }
    BodyId add(const BodyDesc& d, world::Surface s) {
        const BodyId id = physics.add(d);
        surfaces[id] = s;
        return id;
    }
};

// A 9 mm pistol round: 8 g at 360 m/s, about 518 J.
Shot pistol(DVec3 from, Vec3 dir) {
    Shot s;
    s.origin = from;
    s.velocity = dir * 360.0f;
    s.mass = 0.008f;
    return s;
}

bool near(double a, double b, double within) { return std::fabs(a - b) <= within; }

} // namespace

GIANT_TEST("ballistics: in open air a bullet falls and slows as drag says, whatever the frame times") {
    Range a;
    Range b;
    Shot shot;
    shot.origin = {0.0, 1000.0, 0.0};
    shot.velocity = {880.0f, 0.0f, 0.0f};
    shot.mass = 0.0034f;
    GIANT_CHECK(a.bullets.fire(shot) != k_no_bullet && b.bullets.fire(shot) != k_no_bullet);
    for (int i = 0; i < 60; ++i) a.bullets.step(1.0 / 60.0);
    for (int i = 0; i < 100; ++i) b.bullets.step(i % 2 == 0 ? 0.007 : 0.013);
    GIANT_CHECK(a.bullets.stats().steps == 240 && b.bullets.stats().steps == 240);
    const BulletState s = a.bullets.bullets().at(0);
    GIANT_CHECK(s.position == b.bullets.bullets().at(0).position);
    // Drag alone takes it ln(1 + k v t) / k along: about 640 m in a second.
    const double k = 0.0007;
    const double along = std::log(1.0 + k * 880.0 * 1.0) / k;
    GIANT_CHECK(near(s.position.x, along, along * 0.01));
    // Fell less than g t^2 / 2 (4.9 m): drag slows its fall too.
    GIANT_CHECK(1000.0 - s.position.y > 3.9 && 1000.0 - s.position.y < 4.8);
    GIANT_CHECK(near(s.velocity.x, 880.0 / (1.0 + k * 880.0), 10.0));
    GIANT_CHECK(near(s.distance, s.position.x, 1.0) && a.bullets.hits().empty());
}

GIANT_TEST("ballistics: a pistol round goes through a wooden wall slowed, and stops in concrete") {
    Range r;
    const BodyId wood = r.add(wall(5.0, 0.1f), world::Surface::wood);
    const BodyId concrete = r.add(wall(20.0, 0.2f), world::Surface::concrete);
    const std::vector<BulletHit> hits = r.bullets.resolve(pistol({0.0, 0.0, 0.0}, {1.0f, 0.0f, 0.0f}));
    GIANT_CHECK(hits.size() == 2);
    if (hits.size() != 2) return;
    const BulletHit& through = hits[0];
    GIANT_CHECK(through.body == wood && through.outcome == HitOutcome::passed && through.surface == world::Surface::wood);
    GIANT_CHECK(near(through.point.x, 4.95, 0.01) && near(through.thickness, 0.1, 0.01));
    GIANT_CHECK(near(through.exit.x, 5.05, 0.01));
    // 3000 J a metre of wood: 300 J of its ~510 taken.
    const double in = 0.5 * 0.008 * through.speed_in * through.speed_in;
    GIANT_CHECK(near(through.energy, 300.0, 30.0));
    GIANT_CHECK(near(0.5 * 0.008 * through.speed_out * through.speed_out, in - through.energy, 1.0));
    GIANT_CHECK(through.impulse.x > 0.0f && near(through.impulse.x, 0.008 * (through.speed_in - through.speed_out), 0.01));
    const BulletHit& stop = hits[1];
    GIANT_CHECK(stop.body == concrete && stop.outcome == HitOutcome::stopped && stop.speed_out == 0.0f);
    // Gone in a few millimetres (its energy over 50000 J a metre), never through.
    GIANT_CHECK(stop.thickness > 0.001f && stop.thickness < 0.01f && near(stop.exit.x, 19.9 + stop.thickness, 0.01));
    GIANT_CHECK(near(stop.distance, 19.9, 0.05) && stop.distance > through.distance);
    GIANT_CHECK(r.bullets.hits().empty() && r.bullets.stats().flying == 0); // resolved: none flying, none in hits()
}

GIANT_TEST("ballistics: armour piercing goes through what stops a plain round, and energy runs out through layers") {
    Range r;
    for (int i = 0; i < 6; ++i) r.add(wall(2.0 + i, 0.06f), world::Surface::wood);
    // 518 J through 0.06 m boards at 180 J each: two, and stopped in the third.
    std::vector<BulletHit> hits = r.bullets.resolve(pistol({0.0, 0.0, 0.0}, {1.0f, 0.0f, 0.0f}));
    GIANT_CHECK(hits.size() == 3 && hits[0].outcome == HitOutcome::passed && hits[1].outcome == HitOutcome::passed &&
                hits[2].outcome == HitOutcome::stopped);
    if (hits.size() == 3) GIANT_CHECK(hits[1].speed_in < hits[0].speed_in && hits[2].speed_in < hits[1].speed_in);
    Shot ap = pistol({0.0, 0.0, 0.0}, {1.0f, 0.0f, 0.0f});
    ap.penetration = 2.0f; // half the resistance: five of them
    hits = r.bullets.resolve(ap);
    GIANT_CHECK(hits.size() == 6 && hits[4].outcome == HitOutcome::passed && hits[5].outcome == HitOutcome::stopped);
}

GIANT_TEST("ballistics: struck at a shallow angle a metal plate sends it off mirrored, slower; head on it stops") {
    Range r;
    // A plate lying flat, its top at y = 0.
    BodyDesc plate;
    plate.position = {0.0, -0.01, 0.0};
    plate.parts.push_back({BoxShape{{20.0f, 0.01f, 20.0f}}});
    const BodyId id = r.add(plate, world::Surface::metal);
    const float a = degrees_to_radians(10.0f); // under metal's 25 degrees
    Shot shot = pistol({-10.0, 10.0 * std::tan(a), 0.0}, {std::cos(a), -std::sin(a), 0.0f});
    std::vector<BulletHit> hits = r.bullets.resolve(shot);
    GIANT_CHECK(!hits.empty() && hits[0].body == id && hits[0].outcome == HitOutcome::ricochet);
    if (!hits.empty()) {
        GIANT_CHECK(near(hits[0].point.y, 0.0, 0.01) && near(hits[0].point.x, 0.0, 0.1));
        GIANT_CHECK(near(hits[0].speed_out, hits[0].speed_in * 0.7f, 0.5));
        GIANT_CHECK(hits[0].impulse.y < 0.0f); // pushed the plate down
    }
    if (hits.size() > 1) GIANT_CHECK(hits[1].body != id || hits[1].point.x > 1.0); // went on away from it
    // Head on: stopped (metal takes a pistol round in a millimetre or two).
    hits = r.bullets.resolve(pistol({0.0, 5.0, 0.0}, {0.0f, -1.0f, 0.0f}));
    GIANT_CHECK(hits.size() == 1 && hits[0].outcome == HitOutcome::stopped && hits[0].thickness < 0.005f);
}

GIANT_TEST("ballistics: the ground and thick blocks stop it inside; the shooter's own body is passed") {
    Range r;
    BodyDesc ground;
    HeightShape h;
    h.samples = 3;
    h.spacing = 10.0f;
    h.heights.assign(9, 0.0f);
    ground.position = {-10.0, 0.0, -10.0};
    ground.parts.push_back({h});
    r.add(ground, world::Surface::foliage); // so little resistance it would go through, if the ground had a far side
    std::vector<BulletHit> hits = r.bullets.resolve(pistol({0.0, 5.0, 0.0}, {0.0f, -1.0f, 0.0f}));
    GIANT_CHECK(hits.size() == 1 && hits[0].outcome == HitOutcome::stopped);
    const BodyId block = r.add(wall(5.0, 3.0f), world::Surface::glass); // 3 m of glass: past what 518 J goes through
    hits = r.bullets.resolve(pistol({0.0, 1.0, 0.0}, {1.0f, 0.0f, 0.0f}));
    GIANT_CHECK(hits.size() == 1 && hits[0].body == block && hits[0].outcome == HitOutcome::stopped);
    // Fired from inside its own block, it leaves it untouched.
    Shot inside = pistol({5.0, 1.0, 0.0}, {1.0f, 0.0f, 0.0f});
    inside.ignore = block;
    hits = r.bullets.resolve(inside);
    GIANT_CHECK(hits.empty() || hits[0].body != block);
}

GIANT_TEST("ballistics: flying bullets hit in steps; their end comes by range, time or speed; too many are refused") {
    Range r;
    r.add(wall(100.0, 0.5f), world::Surface::concrete);
    r.bullets.settings().max_bullets = 2;
    const BulletId one = r.bullets.fire(pistol({0.0, 0.0, 0.0}, {1.0f, 0.0f, 0.0f}));
    Shot up = pistol({0.0, 0.0, 0.0}, {0.0f, 1.0f, 0.0f});
    up.tag = 7;
    const BulletId two = r.bullets.fire(up);
    GIANT_CHECK(one != k_no_bullet && two != k_no_bullet && one != two);
    GIANT_CHECK(r.bullets.fire(up) == k_no_bullet && r.bullets.stats().refused == 1);
    Shot bad = up;
    bad.mass = 0.0f;
    GIANT_CHECK(r.bullets.fire(bad) == k_no_bullet);
    // The wall is 100 m off: about 0.3 s at 360 m/s.
    bool hit = false;
    for (int i = 0; i < 30 && !hit; ++i) {
        r.bullets.step(1.0 / 60.0);
        for (const BulletHit& h : r.bullets.hits()) hit = hit || h.bullet == one;
    }
    GIANT_CHECK(hit && r.bullets.stats().flying == 1); // stopped in the wall; the other still climbing
    // Straight up it slows to min_speed (drag and gravity) and is spent.
    for (int i = 0; i < 600 && r.bullets.stats().flying > 0; ++i) r.bullets.step(1.0 / 60.0);
    GIANT_CHECK(r.bullets.stats().flying == 0);
    r.bullets.settings().max_range = 50.0;
    r.bullets.fire(pistol({0.0, 10.0, 50.0}, {1.0f, 0.0f, 0.0f}));
    for (int i = 0; i < 20; ++i) r.bullets.step(1.0 / 60.0);
    GIANT_CHECK(r.bullets.stats().flying == 0);
    r.bullets.fire(up);
    r.bullets.clear();
    GIANT_CHECK(r.bullets.stats().flying == 0);
    GIANT_CHECK(r.bullets.step(std::nan("")) == 0);
}

GIANT_TEST("ballistics: a ray names the compound part it met; an impulse pushes a dynamic body only") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    BodyDesc two;
    two.parts.push_back({BoxShape{{0.5f, 0.5f, 0.5f}}, {-2.0f, 0.0f, 0.0f}});
    two.parts.push_back({BoxShape{{0.5f, 0.5f, 0.5f}}, {2.0f, 0.0f, 0.0f}});
    const BodyId fixed = physics.add(two);
    const auto left = physics.raycast({-2.0, 5.0, 0.0}, {0.0f, -1.0f, 0.0f}, 10.0);
    const auto right = physics.raycast({2.0, 5.0, 0.0}, {0.0f, -1.0f, 0.0f}, 10.0);
    GIANT_CHECK(left && right && left->body == fixed && right->body == fixed && left->part == 0 && right->part == 1);
    BodyDesc box;
    box.motion = Motion::dynamic;
    box.position = {0.0, 50.0, 0.0};
    box.mass = 1.0f;
    box.parts.push_back({BoxShape{}});
    const BodyId crate = physics.add(box);
    physics.add_impulse(crate, {3.0f, 0.0f, 0.0f}, {0.0, 50.0, 0.0});
    GIANT_CHECK(near(physics.state(crate)->linear_velocity.x, 3.0, 0.01));
    physics.add_impulse(fixed, {100.0f, 0.0f, 0.0f}, {0.0, 0.0, 0.0}); // fixed: left alone
    GIANT_CHECK(physics.state(fixed)->position == DVec3{});
    GIANT_CHECK(!physics.is_character_body(crate));
    const CharacterId ch = physics.add_character({});
    GIANT_CHECK(physics.is_character_body(physics.character_body(ch)));
}
