// Physics on Jolt: bodies fall, rest, are pushed, hit by rays, and step in
// fixed steps whatever the frame time.
#include "giant/sim/physics.h"

#include "giant_test.h"

#include <cmath>

using namespace giant;
using namespace giant::sim;

namespace {

BodyDesc ground_box() {
    BodyDesc d;
    d.parts.push_back({BoxShape{{50.0f, 0.5f, 50.0f}}, {0.0f, -0.5f, 0.0f}});
    return d;
}

BodyDesc crate(DVec3 at) {
    BodyDesc d;
    d.motion = Motion::dynamic;
    d.position = at;
    d.parts.push_back({BoxShape{{0.5f, 0.5f, 0.5f}}});
    return d;
}

} // namespace

GIANT_TEST("physics: a crate falls onto the ground and comes to rest on it") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    GIANT_CHECK(physics.add(ground_box()) != k_no_body);
    const BodyId id = physics.add(crate({0.0, 5.0, 0.0}));
    GIANT_CHECK(id != k_no_body && physics.size() == 2);
    // Half a second in: falling.
    u32 steps = 0;
    for (int i = 0; i < 30; ++i) steps += physics.step(1.0 / 60.0);
    GIANT_CHECK(steps == 30);
    const BodyState mid = *physics.state(id);
    GIANT_CHECK(mid.position.y < 4.0 && mid.linear_velocity.y < -3.0f && mid.active);
    GIANT_CHECK(!physics.moved().empty() && physics.moved().front() == id);
    // Three more seconds, in frames of every length: resting, half a metre up.
    for (int i = 0; i < 180; ++i) physics.step(i % 2 == 0 ? 0.01 : 0.023);
    const BodyState rest = *physics.state(id);
    GIANT_CHECK(std::fabs(rest.position.y - 0.5) < 0.02 && std::fabs(rest.linear_velocity.y) < 0.05f);
    GIANT_CHECK(physics.stats().steps > 200 && physics.stats().bodies == 2);
}

GIANT_TEST("physics: fixed steps: one long call or many short ones run the same world") {
    JobSystem jobs(2);
    PhysicsWorld a(jobs);
    PhysicsWorld b(jobs);
    for (PhysicsWorld* w : {&a, &b}) {
        w->add(ground_box());
        BodyDesc c = crate({0.0, 3.0, 0.0});
        c.angular_velocity = {1.0f, 2.0f, 0.5f};
        w->add(c);
    }
    // 60 steps each: four at a time in one, one at a time in the other.
    for (int i = 0; i < 15; ++i) GIANT_CHECK(a.step(4.0 / 60.0) == 4);
    for (int i = 0; i < 60; ++i) GIANT_CHECK(b.step(1.0 / 60.0) == 1);
    const BodyState sa = *a.state(a.moved().empty() ? 2 : a.moved().front());
    const BodyState sb = *b.state(b.moved().empty() ? 2 : b.moved().front());
    GIANT_CHECK(std::fabs(sa.position.y - sb.position.y) < 1e-3);
    // A very long frame runs at most four steps and drops the rest.
    GIANT_CHECK(a.step(10.0) == 4 && a.stats().dropped > 500);
}

GIANT_TEST("physics: double precision: a crate 30 km out rests as at the origin") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    BodyDesc far_ground = ground_box();
    far_ground.position = {30000.0, 0.0, -30000.0};
    physics.add(far_ground);
    const BodyId id = physics.add(crate({30000.25, 2.0, -30000.0}));
    physics.step(0.06); // 3 steps, below the frame cap
    for (int i = 0; i < 120; ++i) physics.step(1.0 / 60.0);
    const BodyState s = *physics.state(id);
    GIANT_CHECK(std::fabs(s.position.x - 30000.25) < 1e-3 && std::fabs(s.position.y - 0.5) < 0.02);
}

GIANT_TEST("physics: a kinematic body pushes a resting crate; rays hit what is there") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    physics.add(ground_box());
    BodyDesc c = crate({0.0, 0.5, 0.0});
    c.user = 42;
    const BodyId box = physics.add(c);
    for (int i = 0; i < 60; ++i) physics.step(1.0 / 60.0); // settles, then sleeps
    BodyDesc foot;
    foot.motion = Motion::kinematic;
    foot.position = {-3.0, 1.0, 0.0};
    foot.parts.push_back({BoxShape{{1.0f, 1.0f, 1.0f}}});
    const BodyId kick = physics.add(foot);
    for (int i = 0; i < 60; ++i) {
        physics.move_kinematic(kick, {-3.0 + 4.0 * (i + 1) / 60.0, 1.0, 0.0}, {});
        physics.step(1.0 / 60.0);
    }
    GIANT_CHECK(physics.state(box)->position.x > 0.5); // pushed along +x
    // A ray down from above meets the crate's top, and hands back its tag.
    const DVec3 at = physics.state(box)->position;
    const auto hit = physics.raycast({at.x, 10.0, at.z}, {0.0f, -1.0f, 0.0f}, 20.0);
    GIANT_CHECK(hit && hit->body == box && hit->user == 42 && hit->normal.y > 0.9f);
    GIANT_CHECK(std::fabs(hit->distance - (10.0 - (at.y + 0.5))) < 0.05);
    GIANT_CHECK(!physics.raycast({100.0, 10.0, 0.0}, {0.0f, 1.0f, 0.0f}, 20.0)); // nothing above
    // Removed: gone, and its id finds nothing.
    physics.remove(box);
    GIANT_CHECK(!physics.contains(box) && !physics.state(box) && physics.size() == 2);
}

GIANT_TEST("physics: height fields hold the ground; bad descriptions are refused") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    // A 9 x 9 slope (odd: padded inside) rising 1 m per metre along +x, 2 m apart.
    HeightShape slope{9, 2.0f, {}};
    for (u32 j = 0; j < 9; ++j)
        for (u32 i = 0; i < 9; ++i) slope.heights.push_back(static_cast<float>(i) * 2.0f);
    slope.heights[0] = std::nanf(""); // a hole at the first corner
    BodyDesc ground;
    ground.position = {100.0, 0.0, 100.0};
    ground.parts.push_back({slope});
    GIANT_CHECK(physics.add(ground) != k_no_body);
    const auto hit = physics.raycast({105.0, 50.0, 107.0}, {0.0f, -1.0f, 0.0f}, 100.0);
    GIANT_CHECK(hit && std::fabs(hit->point.y - 5.0) < 0.01);
    GIANT_CHECK(!physics.raycast({100.4, 50.0, 100.4}, {0.0f, -1.0f, 0.0f}, 100.0)); // the hole
    // Refused: no parts, a negative size, a moving height field, NaN. A
    // size of nothing is a millimetre (a sheet still collides).
    GIANT_CHECK(physics.add(BodyDesc{}) == k_no_body);
    BodyDesc sheet;
    sheet.position = {-50.0, 0.0, 0.0};
    sheet.parts.push_back({BoxShape{{1.0f, 0.0f, 1.0f}}});
    GIANT_CHECK(physics.add(sheet) != k_no_body);
    BodyDesc flat;
    flat.parts.push_back({BoxShape{{1.0f, -1.0f, 1.0f}}});
    GIANT_CHECK(physics.add(flat) == k_no_body);
    ground.motion = Motion::dynamic;
    GIANT_CHECK(physics.add(ground) == k_no_body);
    BodyDesc nan = crate({std::nan(""), 0.0, 0.0});
    GIANT_CHECK(physics.add(nan) == k_no_body);
    // Many at once: the bad one is left out, the rest go in, in order.
    std::vector<BodyDesc> many{crate({0, 5, 0}), flat, crate({3, 5, 0})};
    const std::vector<BodyId> ids = physics.add_many(many);
    GIANT_CHECK(ids.size() == 3 && ids[0] != k_no_body && ids[1] == k_no_body && ids[2] != k_no_body);
    GIANT_CHECK(physics.size() == 4);
}
