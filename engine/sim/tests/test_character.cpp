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
    bool above = true;
    for (int i = 0; i < 30; ++i) {
        physics.step(1.0 / 60.0);
        const BodyState s = *physics.state(b);
        if (std::fabs(s.position.x - 5.0) < 0.1 && std::fabs(s.position.z) < 0.1) above = above && s.position.y > 1.7;
    }
    GIANT_CHECK(above);
}
