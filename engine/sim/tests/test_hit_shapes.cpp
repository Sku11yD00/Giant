// Characters' hit shapes: capsules met by rays, named by zone, gone
// through by bullets.
#include "giant/sim/ballistics.h"
#include "giant/sim/hit_shapes.h"

#include "giant_test.h"

#include <cmath>

using namespace giant;
using namespace giant::sim;

namespace {

bool near(double a, double b, double within) { return std::fabs(a - b) <= within; }

// A standing figure 2 m off along x: a torso capsule (0.2 m round, 1 to
// 1.4 m up) and a head ball (0.12 m round, 1.7 m up).
HitCharacter figure(double x) {
    HitCharacter c;
    c.capsules.push_back({{x, 1.0, 0.0}, {x, 1.4, 0.0}, 0.2f, 3, 1});
    c.capsules.push_back({{x, 1.7, 0.0}, {x, 1.7, 0.0}, 0.12f, 5, 0});
    return c;
}

} // namespace

GIANT_TEST("hit shapes: a ray goes into and out of a capsule where its side and its ends say") {
    // Across the middle: in at x = -0.5, out at 0.5.
    auto i = ray_capsule({-5.0, 0.0, 0.0}, {1.0f, 0.0f, 0.0f}, {0.0, -1.0, 0.0}, {0.0, 1.0, 0.0}, 0.5);
    GIANT_CHECK(i && near(i->first, 4.5, 1e-9) && near(i->second, 5.5, 1e-9));
    // Down its axis: in at the top ball, out at the bottom one.
    i = ray_capsule({0.0, 5.0, 0.0}, {0.0f, -1.0f, 0.0f}, {0.0, -1.0, 0.0}, {0.0, 1.0, 0.0}, 0.5);
    GIANT_CHECK(i && near(i->first, 3.5, 1e-9) && near(i->second, 6.5, 1e-9));
    // Past its end, through the ball only.
    i = ray_capsule({-5.0, 1.3, 0.0}, {1.0f, 0.0f, 0.0f}, {0.0, -1.0, 0.0}, {0.0, 1.0, 0.0}, 0.5);
    GIANT_CHECK(i && near(i->second - i->first, 2.0 * std::sqrt(0.25 - 0.09), 1e-9));
    GIANT_CHECK(!ray_capsule({-5.0, 2.0, 0.0}, {1.0f, 0.0f, 0.0f}, {0.0, -1.0, 0.0}, {0.0, 1.0, 0.0}, 0.5));
    // From inside: in behind it.
    i = ray_capsule({0.0, 0.0, 0.0}, {1.0f, 0.0f, 0.0f}, {0.0, -1.0, 0.0}, {0.0, 1.0, 0.0}, 0.5);
    GIANT_CHECK(i && i->first < 0.0 && near(i->second, 0.5, 1e-9));
    // A ball (both ends one point).
    i = ray_capsule({-5.0, 0.0, 0.0}, {1.0f, 0.0f, 0.0f}, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, 1.0);
    GIANT_CHECK(i && near(i->first, 4.0, 1e-9) && near(i->second, 6.0, 1e-9));
}

GIANT_TEST("hit shapes: zones come from the bones' names by the first rule that matches") {
    const HitZoneMap m = default_hit_zones();
    const auto zone = [&](const char* bone) {
        const u16 z = m.zone_of(bone);
        return z == k_no_zone ? std::string("none") : m.rules[z].zone;
    };
    GIANT_CHECK(zone("mixamorig:Head") == "head" && zone("HeadTop_End") == "head");
    GIANT_CHECK(zone("mixamorig:Neck") == "neck");
    GIANT_CHECK(zone("mixamorig:Spine1") == "torso" && zone("Hips") == "torso" && zone("pelvis") == "torso");
    GIANT_CHECK(zone("mixamorig:LeftForeArm") == "arm" && zone("RightHandIndex2") == "arm" && zone("upperarm_l") == "arm");
    GIANT_CHECK(zone("mixamorig:LeftUpLeg") == "leg" && zone("calf_r") == "leg" && zone("LeftToeBase") == "leg");
    GIANT_CHECK(zone("Tail01") == "none");
    HitZoneMap custom;
    custom.rules.push_back({"helmet", {"HEAD"}});
    GIANT_CHECK(custom.zone_of("head") == 0 && custom.zone_of("spine") == k_no_zone);
}

GIANT_TEST("hit shapes: the nearest capsule of any character but the ignored one") {
    HitShapes shapes;
    shapes.set(7, figure(2.0));
    shapes.set(9, figure(5.0));
    GIANT_CHECK(shapes.size() == 2 && shapes.contains(7));
    auto hit = shapes.raycast({0.0, 1.2, 0.0}, {1.0f, 0.0f, 0.0f}, 100.0);
    GIANT_CHECK(hit && hit->owner == 7 && hit->capsule == 0 && near(hit->distance, 1.8, 1e-6) &&
                near(hit->exit, 2.2, 1e-6) && near(hit->normal.x, -1.0, 1e-5));
    hit = shapes.raycast({0.0, 1.2, 0.0}, {1.0f, 0.0f, 0.0f}, 100.0, 7);
    GIANT_CHECK(hit && hit->owner == 9);
    hit = shapes.raycast({0.0, 1.7, 0.0}, {1.0f, 0.0f, 0.0f}, 100.0);
    GIANT_CHECK(hit && hit->owner == 7 && hit->capsule == 1);
    GIANT_CHECK(!shapes.raycast({0.0, 1.2, 0.0}, {1.0f, 0.0f, 0.0f}, 1.5)); // short of it
    GIANT_CHECK(!shapes.raycast({0.0, 1.2, 0.0}, {-1.0f, 0.0f, 0.0f}, 100.0)); // behind
    shapes.set(7, {});
    GIANT_CHECK(!shapes.contains(7) && shapes.size() == 1);
    shapes.clear();
    GIANT_CHECK(shapes.size() == 0);
}

GIANT_TEST("hit shapes: a bullet goes through a torso and a wall behind is met next; its zone and bone reported") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    BodyDesc wall;
    wall.position = {4.0, 1.0, 0.0};
    wall.parts.push_back({BoxShape{{0.1f, 2.0f, 2.0f}}});
    const BodyId w = physics.add(wall);
    HitShapes shapes;
    shapes.set(7, figure(2.0));
    Ballistics bullets(physics);
    bullets.hit_shapes = &shapes;
    Shot rifle;
    rifle.origin = {0.0, 1.2, 0.0};
    rifle.velocity = {880.0f, 0.0f, 0.0f};
    rifle.mass = 0.008f;
    std::vector<BulletHit> hits = bullets.resolve(rifle);
    GIANT_CHECK(hits.size() == 2);
    if (hits.size() == 2) {
        const BulletHit& torso = hits[0];
        GIANT_CHECK(torso.character && torso.owner == 7 && torso.zone == 1 && torso.bone == 3 &&
                    torso.body == k_no_body && torso.surface == world::Surface::flesh);
        GIANT_CHECK(torso.outcome == HitOutcome::passed && near(torso.thickness, 0.4, 0.01));
        GIANT_CHECK(near(torso.energy, 0.4 * 2000.0, 20.0)); // 0.4 m of flesh at 2000 J a metre
        GIANT_CHECK(hits[1].body == w && !hits[1].character);
    }
    // The shooter's own shapes are passed.
    rifle.origin = {2.0, 1.2, 0.0};
    rifle.ignore_owner = 7;
    hits = bullets.resolve(rifle);
    GIANT_CHECK(!hits.empty() && hits[0].body == w);
    // Tougher flesh (4000 J a metre): a pistol round (518 J, 0.13 m) stops in the head.
    Shot pistol;
    pistol.origin = {0.0, 1.7, 0.0};
    pistol.velocity = {360.0f, 0.0f, 0.0f};
    pistol.mass = 0.008f;
    bullets.settings().surfaces[static_cast<usize>(world::Surface::flesh)].resistance = 4000.0f;
    hits = bullets.resolve(pistol);
    GIANT_CHECK(!hits.empty() && hits[0].zone == 0 && hits[0].outcome == HitOutcome::stopped);
}
