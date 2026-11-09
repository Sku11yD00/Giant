// Characters: walk, climb steps, stop at walls and steep slopes, jump, ride
// and push, walk on height fields, and carry on through a cleared world.
#include "giant/sim/physics.h"

#include "giant_test.h"

#include <cmath>
#include <numbers>

using namespace giant;
using namespace giant::sim;

namespace {

BodyDesc box(DVec3 at, Vec3 half, Motion motion = Motion::fixed) {
    BodyDesc d;
    d.motion = motion;
    d.position = at;
    d.parts.push_back({BoxShape{half}});
    return d;
}

// A ground whose top is y = 0.
BodyDesc ground() { return box({0.0, -0.5, 0.0}, {100.0f, 0.5f, 100.0f}); }

void run(PhysicsWorld& physics, double seconds) {
    for (int i = 0; i < static_cast<int>(std::lround(seconds * 60.0)); ++i) physics.step(1.0 / 60.0);
}

CharacterState state(const PhysicsWorld& physics, CharacterId id) { return *physics.character_state(id); }

} // namespace

GIANT_TEST("character: stands, walks at its speed, stops; bad descriptions are refused") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    physics.add(ground());
    CharacterDesc desc;
    desc.position = {0.0, 0.5, 0.0}; // dropped from half a metre
    const CharacterId player = physics.add_character(desc);
    GIANT_CHECK(player != k_no_character && physics.characters() == 1 && physics.contains_character(player));
    run(physics, 1.0);
    CharacterState s = state(physics, player);
    GIANT_CHECK(s.on_ground && std::fabs(s.position.y) < 0.03 && s.ground != k_no_body);
    // A world at rest: the character's body does not count as awake.
    GIANT_CHECK(physics.stats().active == 0 && physics.stats().characters == 1 && physics.stats().bodies == 1);
    // Walking 1.5 m/s along x for two seconds: about 3 m (it gets going within a frame or two).
    physics.set_character_input(player, {{1.5f, 0.0f, 0.0f}});
    run(physics, 2.0);
    s = state(physics, player);
    GIANT_CHECK(std::fabs(s.position.x - 3.0) < 0.1 && std::fabs(s.position.z) < 0.01 && s.on_ground);
    GIANT_CHECK(std::fabs(s.velocity.x - 1.5f) < 0.05f && std::fabs(s.position.y) < 0.03);
    // Told to stop: stops.
    physics.set_character_input(player, {});
    run(physics, 0.5);
    s = state(physics, player);
    GIANT_CHECK(std::fabs(s.velocity.x) < 0.01f && std::fabs(s.position.x - 3.0) < 0.15);
    // Rays hit its body.
    const auto hit = physics.raycast({s.position.x, 10.0, 0.0}, {0.0f, -1.0f, 0.0f}, 20.0);
    GIANT_CHECK(hit && hit->body == physics.character_body(player) && hit->point.y > 1.7);
    // Bad numbers: refused.
    CharacterDesc bad = desc;
    bad.radius = 1.0f; // wider than tall
    GIANT_CHECK(physics.add_character(bad) == k_no_character);
    bad = desc;
    bad.position.x = std::nan("");
    GIANT_CHECK(physics.add_character(bad) == k_no_character);
    physics.remove_character(player);
    GIANT_CHECK(!physics.contains_character(player) && !physics.character_state(player) &&
                physics.characters() == 0 && physics.character_body(player) == k_no_body);
    run(physics, 0.1); // no character left to step
}

GIANT_TEST("character: walks up a step, not up a wall; down a step without leaving the ground") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    physics.add(ground());
    // A 0.3 m step at x = 2 (a platform to x = 6), then a 2 m wall at x = 10 beyond its end.
    physics.add(box({4.0, 0.15, 0.0}, {2.0f, 0.15f, 5.0f}));
    physics.add(box({10.5, 1.0, 0.0}, {0.5f, 1.0f, 5.0f}));
    CharacterDesc desc;
    desc.position = {0.0, 0.0, 0.0};
    const CharacterId player = physics.add_character(desc);
    physics.set_character_input(player, {{2.0f, 0.0f, 0.0f}});
    run(physics, 2.0); // 4 m: up onto the step
    CharacterState s = state(physics, player);
    GIANT_CHECK(s.position.x > 3.5 && std::fabs(s.position.y - 0.3) < 0.03 && s.on_ground);
    // On, off the far side (down 0.3 m: rounding the edge, not falling off
    // it), into the wall.
    int off_ground = 0;
    for (int i = 0; i < 4 * 60; ++i) {
        physics.step(1.0 / 60.0);
        if (!state(physics, player).on_ground) ++off_ground;
    }
    s = state(physics, player);
    GIANT_CHECK(off_ground <= 3 && std::fabs(s.position.y) < 0.03);
    GIANT_CHECK(std::fabs(s.position.x - (10.0 - desc.radius)) < 0.05 && std::fabs(s.velocity.x) < 0.05f);
}

GIANT_TEST("character: walks up a gentle slope, slides back off a steep one") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    physics.add(ground());
    // Ramps rising along +x from x = 1: 30 degrees at z = 0, 65 at z = 20.
    const auto ramp = [&](double degrees, double z) {
        const double a = degrees * std::numbers::pi / 180.0;
        BodyDesc d = box({}, {5.0f, 0.5f, 3.0f});
        // Turned about z; its top surface's low edge at (1, 0).
        const float h = static_cast<float>(a * 0.5);
        d.rotation = {0.0f, 0.0f, std::sin(h), std::cos(h)};
        d.position = {1.0 + 5.0 * std::cos(a) + 0.5 * std::sin(a), 5.0 * std::sin(a) - 0.5 * std::cos(a), z};
        physics.add(d);
    };
    ramp(30.0, 0.0);
    ramp(65.0, 20.0);
    CharacterDesc desc;
    desc.position = {0.0, 0.0, 0.0};
    const CharacterId gentle = physics.add_character(desc);
    desc.position = {0.0, 0.0, 20.0};
    const CharacterId steep = physics.add_character(desc);
    for (const CharacterId c : {gentle, steep}) physics.set_character_input(c, {{2.0f, 0.0f, 0.0f}});
    run(physics, 3.0);
    const CharacterState g = state(physics, gentle);
    const CharacterState t = state(physics, steep);
    // Up the 30 degree ramp: 6 m of 2 m/s along it climbs past 2 m.
    GIANT_CHECK(g.on_ground && g.position.y > 2.0 && g.position.x > 4.0);
    // Held at the foot of the 65 degree one.
    GIANT_CHECK(t.position.y < 0.3 && t.position.x < 1.5);
}

GIANT_TEST("character: jumps (once, a press just before landing counts) and lands") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    physics.add(ground());
    CharacterDesc desc;
    const CharacterId player = physics.add_character(desc);
    run(physics, 0.2);
    physics.jump(player, 5.0f);
    double top = 0.0;
    for (int i = 0; i < 60; ++i) {
        physics.step(1.0 / 60.0);
        top = std::max(top, state(physics, player).position.y);
    }
    // v²/2g = 1.27 m.
    GIANT_CHECK(std::fabs(top - 25.0 / (2.0 * 9.81)) < 0.08);
    run(physics, 1.0);
    GIANT_CHECK(state(physics, player).on_ground && state(physics, player).position.y < 0.03);
    // Once: no second jump without another press.
    top = 0.0;
    for (int i = 0; i < 60; ++i) {
        physics.step(1.0 / 60.0);
        top = std::max(top, state(physics, player).position.y);
    }
    GIANT_CHECK(top < 0.03);
    // Pressed a tenth of a second before landing: jumps on landing.
    physics.set_character_position(player, {0.0, 1.0, 0.0});
    bool pressed = false;
    bool jumped = false;
    for (int i = 0; i < 90; ++i) {
        const CharacterState s = state(physics, player);
        if (!pressed && s.velocity.y < 0.0f && s.position.y < 0.45) {
            physics.jump(player, 5.0f);
            pressed = true;
        }
        physics.step(1.0 / 60.0);
        jumped = jumped || (pressed && state(physics, player).velocity.y > 3.0f);
    }
    GIANT_CHECK(pressed && jumped);
    // Under a ceiling 2.2 m up: the head stops it at 0.4 m, and it comes
    // straight back down (no hanging there).
    run(physics, 2.0);
    physics.add(box({0.0, 2.7, 0.0}, {3.0f, 0.5f, 3.0f}));
    physics.jump(player, 5.0f);
    top = 0.0;
    double back = -1.0;
    for (int i = 0; i < 60; ++i) {
        physics.step(1.0 / 60.0);
        const CharacterState s = state(physics, player);
        top = std::max(top, s.position.y);
        if (back < 0.0 && i > 3 && s.on_ground) back = i / 60.0;
    }
    GIANT_CHECK(top < 0.45 && top > 0.3 && back > 0.0 && back < 0.6);
}

GIANT_TEST("character: rides a moving platform, pushes a crate, walks on a height field") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    physics.add(ground());
    // A kinematic platform, top at y = 0.5, moving 1 m/s along z.
    const BodyId platform = physics.add(box({0.0, 0.25, 0.0}, {1.5f, 0.25f, 1.5f}, Motion::kinematic));
    CharacterDesc desc;
    desc.position = {0.0, 0.6, 0.0};
    const CharacterId rider = physics.add_character(desc);
    run(physics, 0.3);
    GIANT_CHECK(state(physics, rider).on_ground && state(physics, rider).ground == platform);
    DVec3 at{0.0, 0.25, 0.0};
    for (int i = 0; i < 60; ++i) {
        at.z += 1.0 / 60.0;
        physics.move_kinematic(platform, at, {});
        physics.step(1.0 / 60.0);
    }
    CharacterState s = state(physics, rider);
    GIANT_CHECK(std::fabs(s.position.z - at.z) < 0.1 && std::fabs(s.position.y - 0.5) < 0.05);
    // Walking into a 20 kg crate pushes it.
    BodyDesc light = box({10.0, 0.5, 0.0}, {0.5f, 0.5f, 0.5f}, Motion::dynamic);
    light.mass = 20.0f;
    const BodyId crate = physics.add(light);
    desc.position = {8.0, 0.0, 0.0};
    const CharacterId pusher = physics.add_character(desc);
    run(physics, 0.5);
    physics.set_character_input(pusher, {{1.5f, 0.0f, 0.0f}});
    run(physics, 3.0);
    GIANT_CHECK(physics.state(crate)->position.x > 11.0);
    // A height field: a 64 m tile, a gentle hill; walking across it stays on it.
    BodyDesc hill;
    hill.position = {0.0, 0.0, 200.0};
    HeightShape h;
    h.samples = 33;
    h.spacing = 2.0f;
    for (u32 j = 0; j < h.samples; ++j) {
        for (u32 i = 0; i < h.samples; ++i) {
            h.heights.push_back(3.0f * std::sin(static_cast<float>(i) * 0.2f) * std::cos(static_cast<float>(j) * 0.15f));
        }
    }
    hill.parts.push_back({h});
    physics.add(hill);
    desc.position = {4.0, 5.0, 230.0};
    const CharacterId walker = physics.add_character(desc);
    run(physics, 1.0);
    physics.set_character_input(walker, {{3.0f, 0.0f, 0.0f}});
    int airborne = 0;
    for (int i = 0; i < 8 * 60; ++i) {
        physics.step(1.0 / 60.0);
        if (!state(physics, walker).on_ground) ++airborne;
    }
    s = state(physics, walker);
    GIANT_CHECK(s.position.x > 25.0 && airborne < 10);
    // Its feet on the hill (the sampled heights, between two of them).
    const double i = s.position.x / 2.0;
    const double j = (s.position.z - 200.0) / 2.0;
    const double surface = 3.0 * std::sin(i * 0.2) * std::cos(j * 0.15);
    GIANT_CHECK(std::fabs(surface - s.position.y) < 0.1);
}

GIANT_TEST("character: carries on through a cleared world; falling things land on it") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    physics.add(ground());
    CharacterDesc desc;
    desc.position = {5.0, 3.0, 0.0};
    const CharacterId player = physics.add_character(desc);
    run(physics, 0.1);
    const DVec3 before = state(physics, player).position;
    physics.clear();
    GIANT_CHECK(physics.contains_character(player) && physics.size() == 0);
    GIANT_CHECK(std::fabs(state(physics, player).position.y - before.y) < 1e-6);
    physics.add(ground());
    run(physics, 2.0);
    GIANT_CHECK(state(physics, player).on_ground && std::fabs(state(physics, player).position.y) < 0.03);
    // A ball dropped on its head rests on (or rolls off) it, never through it.
    BodyDesc ball;
    ball.motion = Motion::dynamic;
    ball.position = {5.0, 4.0, 0.0};
    ball.parts.push_back({SphereShape{0.2f}});
    const BodyId b = physics.add(ball);
    // Over its head (within its radius) the ball is never lower than the top of it.
    bool above = true;
    for (int i = 0; i < 90; ++i) {
        physics.step(1.0 / 60.0);
        const BodyState s = *physics.state(b);
        const double off = std::hypot(s.position.x - 5.0, s.position.z);
        if (off < 0.25) above = above && s.position.y > 1.8 + 0.15;
    }
    GIANT_CHECK(above);
}

GIANT_TEST("character: walks with a crate on its head; its body is its own; huge numbers are held in") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    physics.add(ground());
    CharacterDesc desc;
    const CharacterId player = physics.add_character(desc);
    run(physics, 0.2);
    // A 20 kg crate dropped onto its head rests there, and does not pin it.
    BodyDesc crate = box({0.0, 2.6, 0.0}, {0.2f, 0.2f, 0.2f}, Motion::dynamic);
    crate.mass = 20.0f;
    const BodyId c = physics.add(crate);
    run(physics, 1.0);
    GIANT_CHECK(physics.state(c)->position.y > 1.8 + 0.15);
    physics.set_character_input(player, {{1.0f, 0.0f, 0.0f}});
    run(physics, 1.0);
    GIANT_CHECK(state(physics, player).position.x > 0.8);
    // Its body cannot be taken away or moved as a body.
    const BodyId body = physics.character_body(player);
    physics.remove(body);
    physics.set_transform(body, {50.0, 0.0, 0.0}, {});
    physics.set_velocity(body, {10.0f, 0.0f, 0.0f}, {});
    run(physics, 0.1);
    GIANT_CHECK(physics.contains(body) && physics.size() == 2);
    // Far-fetched speeds: refused or held to 100 m/s, never a broken world.
    physics.set_character_input(player, {{1e30f, 0.0f, 0.0f}});
    GIANT_CHECK(std::fabs(state(physics, player).velocity.x - 1.0f) < 0.05f); // refused: kept walking
    physics.set_character_input(player, {});
    physics.jump(player, 1e30f);
    run(physics, 0.05);
    GIANT_CHECK(state(physics, player).velocity.y <= 100.0f && state(physics, player).velocity.y > 50.0f);
    // Falling with nothing below: no faster than 100 m/s.
    physics.clear();
    run(physics, 20.0);
    GIANT_CHECK(state(physics, player).velocity.y >= -100.01f);
    // Held: it stays put; a jump pressed then is gone by the time it is let go.
    physics.add(ground());
    physics.set_character_position(player, {0.0, 0.0, 0.0});
    run(physics, 0.3);
    physics.hold_character(player, true);
    physics.jump(player, 5.0f);
    run(physics, 1.0);
    GIANT_CHECK(state(physics, player).held && std::fabs(state(physics, player).position.y) < 0.03);
    physics.hold_character(player, false);
    run(physics, 0.5);
    GIANT_CHECK(state(physics, player).position.y < 0.03);
    // A step as high as itself: refused.
    desc.step_height = 1.0f;
    GIANT_CHECK(physics.add_character(desc) == k_no_character);
}

GIANT_TEST("character: climbs a wall walked into, pulls itself up onto its top; not when told not to") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    physics.add(ground());
    // A 4 m block (top y = 4) from x = 2 to x = 6.
    const BodyId block = physics.add(box({4.0, 2.0, 0.0}, {2.0f, 2.0f, 3.0f}));
    CharacterDesc desc;
    desc.position = {0.0, 0.0, 0.0};
    const CharacterId walker = physics.add_character(desc);
    run(physics, 0.3);
    // Not climbing: it stops at the wall.
    physics.set_character_input(walker, {{1.5f, 0.0f, 0.0f}});
    run(physics, 2.0);
    CharacterState s = state(physics, walker);
    GIANT_CHECK(!s.climbing && s.on_ground && std::fabs(s.position.y) < 0.05 && s.position.x < 1.75);
    // Climbing: up the wall at climb speed (2 m/s), held against it.
    CharacterInput in{{1.5f, 0.0f, 0.0f}};
    in.climb = true;
    physics.set_character_input(walker, in);
    run(physics, 0.5);
    s = state(physics, walker);
    GIANT_CHECK(s.climbing && s.wall == block && s.wall_normal.x < -0.99f && !s.mantling);
    GIANT_CHECK(s.position.y > 0.4 && s.position.y < 1.2 && s.position.x > 1.6 && s.position.x < 1.75);
    // On to the top: up over the edge, standing on the block.
    bool mantled = false;
    for (int i = 0; i < 2 * 60; ++i) {
        physics.step(1.0 / 60.0);
        mantled = mantled || state(physics, walker).mantling;
    }
    s = state(physics, walker);
    GIANT_CHECK(mantled && !s.climbing && !s.mantling);
    GIANT_CHECK(std::fabs(s.position.y - 4.0) < 0.05 && s.position.x > 2.2 && s.on_ground && s.ground == block);
    // Walking along the top on (climb still asked): it walks, it does not climb anything.
    run(physics, 1.0);
    s = state(physics, walker);
    GIANT_CHECK(s.position.x > 3.5 && std::fabs(s.position.y - 4.0) < 0.05 && !s.climbing);
}

GIANT_TEST("character: climbs across and down a wall; lets go on the ground, jumping off, or told to") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    physics.add(ground());
    // A 20 m wall (no top in reach) facing -x at x = 2.
    physics.add(box({2.5, 10.0, 0.0}, {0.5f, 10.0f, 20.0f}));
    CharacterDesc desc;
    const CharacterId climber = physics.add_character(desc);
    run(physics, 0.3);
    CharacterInput in{{1.5f, 0.0f, 0.0f}};
    in.climb = true;
    physics.set_character_input(climber, in);
    run(physics, 3.0); // to the wall (x = 1.7) in a second, then up 1.5 m/s
    CharacterState s = state(physics, climber);
    GIANT_CHECK(s.climbing && s.position.y > 2.0 && s.position.y < 3.5);
    const double high = s.position.y;
    // Across (along z) at climb speed, holding its height.
    in.velocity = {0.0f, 0.0f, 3.0f};
    physics.set_character_input(climber, in);
    run(physics, 1.0);
    s = state(physics, climber);
    GIANT_CHECK(s.climbing && std::fabs(s.position.y - high) < 0.05 && std::fabs(s.position.z - 2.0) < 0.15);
    GIANT_CHECK(s.position.x > 1.6 && s.position.x < 1.75);
    // Still (no input): hangs there.
    in.velocity = {};
    physics.set_character_input(climber, in);
    run(physics, 1.0);
    s = state(physics, climber);
    GIANT_CHECK(s.climbing && std::fabs(s.position.y - high) < 0.05);
    // Down (away from it) to the ground: lets go there, standing.
    in.velocity = {-1.5f, 0.0f, 0.0f};
    physics.set_character_input(climber, in);
    run(physics, 0.5);
    GIANT_CHECK(state(physics, climber).climbing && state(physics, climber).position.y < high - 0.5);
    run(physics, 3.0);
    s = state(physics, climber);
    GIANT_CHECK(!s.climbing && s.on_ground && std::fabs(s.position.y) < 0.05 && s.position.x < 1.0);
    // Back up, then a jump: pushed off, away from the wall, falling to the ground.
    in.velocity = {1.5f, 0.0f, 0.0f};
    physics.set_character_input(climber, in);
    run(physics, 3.0);
    GIANT_CHECK(state(physics, climber).climbing);
    physics.jump(climber, 5.0f);
    physics.step(1.0 / 60.0);
    s = state(physics, climber);
    GIANT_CHECK(!s.climbing && s.velocity.x < -1.0f);
    in.velocity = {};
    physics.set_character_input(climber, in);
    run(physics, 3.0);
    s = state(physics, climber);
    GIANT_CHECK(!s.climbing && s.on_ground && std::fabs(s.position.y) < 0.05 && s.position.x < 1.4);
    // Up again, then told not to climb: it lets go and falls.
    in.velocity = {1.5f, 0.0f, 0.0f};
    physics.set_character_input(climber, in);
    run(physics, 2.0);
    GIANT_CHECK(state(physics, climber).climbing && state(physics, climber).position.y > 2.0);
    in.climb = false;
    physics.set_character_input(climber, in);
    run(physics, 0.1);
    GIANT_CHECK(!state(physics, climber).climbing);
    run(physics, 2.0);
    GIANT_CHECK(state(physics, climber).on_ground && std::fabs(state(physics, climber).position.y) < 0.05);
}

GIANT_TEST("character: carried by a moving body (a hand), released falls; the body gone lets it go") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    physics.add(ground());
    const BodyId hand = physics.add(box({0.0, -0.1, 0.0}, {0.6f, 0.1f, 0.6f}, Motion::kinematic));
    CharacterDesc desc;
    desc.position = {0.0, 0.0, 0.0};
    const CharacterId player = physics.add_character(desc);
    run(physics, 0.2);
    // Refused: no such character, no such body, a character's own body.
    GIANT_CHECK(!physics.carry_character(player + 100, hand));
    GIANT_CHECK(!physics.carry_character(player, BodyId{0x00ff0001u}));
    const CharacterId other = physics.add_character([&] {
        CharacterDesc d = desc;
        d.position = {5.0, 0.0, 0.0};
        return d;
    }());
    GIANT_CHECK(!physics.carry_character(player, physics.character_body(other)));
    GIANT_CHECK(physics.carry_character(player, hand));
    GIANT_CHECK(state(physics, player).carrier == hand);
    // Lifted 30 m and turned a quarter about y, the player 2 m off the hand's
    // middle along x: held where it was against the hand.
    physics.set_character_position(player, {2.0, 0.0, 0.0});
    GIANT_CHECK(state(physics, player).carrier == k_no_body); // put somewhere: no longer carried
    GIANT_CHECK(physics.carry_character(player, hand));
    // Its input is left aside while carried.
    physics.set_character_input(player, {{3.0f, 0.0f, 0.0f}});
    physics.jump(player, 5.0f);
    const double turn = 0.5 * std::numbers::pi;
    for (int i = 1; i <= 120; ++i) {
        const double t = i / 120.0;
        const double a = 0.5 * turn * t;
        physics.move_kinematic(hand, {0.0, -0.1 + 30.0 * t, 0.0},
                               {0.0f, static_cast<float>(std::sin(a)), 0.0f, static_cast<float>(std::cos(a))});
        physics.step(1.0 / 60.0);
    }
    // The hand held still (a kinematic body keeps the speed it was last moved at).
    const Quat held{0.0f, static_cast<float>(std::sin(0.5 * turn)), 0.0f, static_cast<float>(std::cos(0.5 * turn))};
    for (int i = 0; i < 6; ++i) {
        physics.move_kinematic(hand, {0.0, 29.9, 0.0}, held);
        physics.step(1.0 / 60.0);
    }
    CharacterState s = state(physics, player);
    // A quarter turn about y takes +x to -z; held up in the air, not on the ground.
    GIANT_CHECK(s.carrier == hand && std::fabs(s.position.y - 30.0) < 0.05 && !s.on_ground);
    GIANT_CHECK(std::fabs(s.position.x) < 0.05 && std::fabs(s.position.z + 2.0) < 0.05);
    GIANT_CHECK(std::fabs(s.velocity.y) < 0.05f);
    // Released: falls to the ground.
    physics.release_character(player);
    GIANT_CHECK(state(physics, player).carrier == k_no_body);
    physics.set_character_input(player, {});
    run(physics, 4.0);
    s = state(physics, player);
    GIANT_CHECK(s.on_ground && std::fabs(s.position.y) < 0.05);
    // Carried again, the hand rising; the hand removed: let go, falling.
    for (int i = 0; i < 6; ++i) { // back down under it, held there
        physics.move_kinematic(hand, {0.0, -0.1, 0.0}, {});
        physics.step(1.0 / 60.0);
    }
    GIANT_CHECK(physics.carry_character(player, hand));
    for (int i = 1; i <= 60; ++i) {
        physics.move_kinematic(hand, {0.0, -0.1 + 10.0 * i / 60.0, 0.0}, {});
        physics.step(1.0 / 60.0);
    }
    GIANT_CHECK(state(physics, player).position.y > 9.0);
    physics.remove(hand);
    physics.step(1.0 / 60.0);
    GIANT_CHECK(state(physics, player).carrier == k_no_body && state(physics, player).velocity.y > 9.0f);
    run(physics, 4.0); // up on its way (it was carried up), then down
    s = state(physics, player);
    GIANT_CHECK(s.on_ground && std::fabs(s.position.y) < 0.05);
}

GIANT_TEST("character: a jump into a wall is a jump; no room over a ledge, no pulling up; a moving ledge is followed") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    physics.add(ground());
    // A 4 m wall at x = 2 (top y = 4).
    physics.add(box({4.0, 2.0, 0.0}, {2.0f, 2.0f, 3.0f}));
    CharacterDesc desc;
    desc.position = {1.0, 0.0, 0.0};
    const CharacterId jumper = physics.add_character(desc);
    CharacterInput in{{1.5f, 0.0f, 0.0f}};
    in.climb = true;
    physics.set_character_input(jumper, in);
    physics.step(1.0 / 60.0);
    // Walking into it and jumping in the same step: up about 1.3 m, never pushed back.
    physics.jump(jumper, 5.0f);
    double top = 0.0;
    double least_x = 10.0;
    for (int i = 0; i < 30; ++i) {
        physics.step(1.0 / 60.0);
        top = std::max(top, state(physics, jumper).position.y);
        least_x = std::min(least_x, state(physics, jumper).position.x);
    }
    GIANT_CHECK(top > 0.9 && least_x > 0.95);
    physics.remove_character(jumper);

    // A lid over the ledge, set back 0.5 m from its edge (0.5 m of room on
    // top; a ray up from where it would stand misses it, its body would
    // not): it climbs up but never pulls itself up into it.
    physics.add(box({4.25, 4.75, -20.0}, {1.75f, 0.25f, 3.0f}));
    physics.add(box({4.0, 2.0, -20.0}, {2.0f, 2.0f, 3.0f}));
    desc.position = {0.5, 0.0, -20.0};
    const CharacterId blocked = physics.add_character(desc);
    physics.set_character_input(blocked, in);
    bool mantled = false;
    for (int i = 0; i < 5 * 60; ++i) {
        physics.step(1.0 / 60.0);
        mantled = mantled || state(physics, blocked).mantling;
    }
    GIANT_CHECK(!mantled && state(physics, blocked).position.y < 3.5);
    physics.remove_character(blocked);

    // A kinematic block (top y = 3) sliding along z at 1 m/s: climbed and
    // pulled up onto, the character ends standing on it.
    const BodyId moving = physics.add(box({4.0, 1.5, 40.0}, {2.0f, 1.5f, 4.0f}, Motion::kinematic));
    desc.position = {1.0, 0.0, 40.0};
    const CharacterId rider = physics.add_character(desc);
    physics.set_character_input(rider, in);
    DVec3 at{4.0, 1.5, 40.0};
    mantled = false;
    for (int i = 0; i < 3 * 60; ++i) {
        at.z += 1.0 / 60.0;
        physics.move_kinematic(moving, at, {});
        physics.step(1.0 / 60.0);
        mantled = mantled || state(physics, rider).mantling;
    }
    physics.set_character_input(rider, {});
    for (int i = 0; i < 30; ++i) {
        at.z += 1.0 / 60.0;
        physics.move_kinematic(moving, at, {});
        physics.step(1.0 / 60.0);
    }
    const CharacterState s = state(physics, rider);
    GIANT_CHECK(mantled && s.on_ground && s.ground == moving && std::fabs(s.position.y - 3.0) < 0.05);
    GIANT_CHECK(std::fabs(s.position.z - at.z) < 1.0 && s.position.x > 2.1);
}

GIANT_TEST("character: put back moving keeps moving; takes hold of a wall it faces, not of the air") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    physics.add(ground());
    const BodyId block = physics.add(box({4.0, 2.0, 0.0}, {2.0f, 2.0f, 3.0f})); // its -x face at x = 2
    CharacterDesc desc;
    desc.position = {-10.0, 3.0, 0.0};
    const CharacterId c = physics.add_character(desc);
    // Falling at 4 m/s and drifting +z: it goes on so (the next step's speed follows on).
    physics.set_character_velocity(c, {0.0f, -4.0f, 1.0f});
    physics.step(1.0 / 60.0);
    CharacterState s = state(physics, c);
    GIANT_CHECK(s.position.y < 3.0 - 4.0 / 60.0 + 0.01 && s.position.z > 0.01);
    physics.set_character_velocity(c, {std::nanf(""), 0.0f, 0.0f}); // bad numbers: nothing
    physics.set_character_velocity(c, {0.0f, -1e6f, 0.0f});         // past the top speed: at it
    GIANT_CHECK(state(physics, c).velocity.y > -1e5f);
    // On the wall it faces: held there, climbing (as walked into).
    physics.set_character_position(c, {1.65, 1.0, 0.0});
    GIANT_CHECK(!physics.take_hold(c, {0.0f, 0.0f, 1.0f})); // nothing that way
    GIANT_CHECK(physics.take_hold(c, {-1.0f, 0.0f, 0.0f}));
    s = state(physics, c);
    GIANT_CHECK(s.climbing && s.wall == block && s.wall_normal.x < -0.99f);
    CharacterInput in;
    in.climb = true;
    physics.set_character_input(c, in);
    run(physics, 0.5);
    s = state(physics, c);
    GIANT_CHECK(s.climbing && std::fabs(s.position.y - 1.0) < 0.1); // still on it, not fallen
    // Out in the open: nothing to hold.
    physics.set_character_position(c, {-20.0, 1.0, 0.0});
    GIANT_CHECK(!physics.take_hold(c, {-1.0f, 0.0f, 0.0f}));
}

GIANT_TEST("character: put back walking up a step with its measured speed, it walks on rather than jumping") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    physics.add(ground());
    CharacterDesc desc;
    desc.position = {0.0, 0.0, 0.0};
    const CharacterId c = physics.add_character(desc);
    run(physics, 0.3);
    GIANT_CHECK(state(physics, c).on_ground);
    // A rise as a step up measures (4.5 m/s up) while standing: not thrown up.
    physics.set_character_velocity(c, {3.0f, 4.5f, 0.0f});
    physics.set_character_input(c, {{3.0f, 0.0f, 0.0f}});
    double highest = 0.0;
    for (int i = 0; i < 30; ++i) {
        physics.step(1.0 / 60.0);
        highest = std::max(highest, state(physics, c).position.y);
    }
    GIANT_CHECK(highest < 0.05 && state(physics, c).on_ground);
    // Huge but finite: the top speed, not none.
    physics.set_character_position(c, {0.0, 20.0, 0.0});
    physics.set_character_velocity(c, {1e20f, 0.0f, 0.0f});
    GIANT_CHECK(state(physics, c).velocity.x > 10.0f);
}
