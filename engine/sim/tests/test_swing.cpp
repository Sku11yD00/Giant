// Swings: hanging things lean and swing in the wind (further in a storm),
// hinged ones only across their hinge, within their limit; knocked by
// shots and blasts; asleep when still; bounded work.
#include "giant/sim/swing.h"

#include "giant_test.h"

#include <cmath>

using namespace giant;
using namespace giant::sim;

namespace {

WindState blowing(float speed, float gustiness = 0.0f) {
    WindState w;
    w.velocity = {speed, 0.0f, 0.0f}; // along +x
    w.gustiness = gustiness;
    return w;
}

world::SwingSettings lantern() {
    world::SwingSettings s;
    s.on = true;
    s.length = 0.5f;
    s.weight = 2.0f;
    return s;
}

world::Transform at(double x, double y, double z) {
    world::Transform t;
    t.position = {x, y, z};
    t.scale = {0.3f, 0.4f, 0.3f};
    return t;
}

const Vec3 k_lantern{0.3f, 0.4f, 0.3f};

void run(Swings& swings, const WindState& wind, float seconds, const DVec3& camera = {}) {
    for (float t = 0.0f; t < seconds; t += 1.0f / 60.0f) swings.step(1.0 / 60.0, wind, camera);
}

// The pose's rotation and position agree: the pivot stays put, the
// origin hangs `length` from it.
bool on_its_rod(const Swings::Swing& s) {
    const DVec3 d = s.pose.position - s.pivot;
    return std::fabs(length(d) - s.settings.length) < 1e-3 &&
           length(s.pose.position - (s.pivot + DVec3{rotate(s.turn, Vec3{0.0f, -s.settings.length, 0.0f}).x,
                                                     rotate(s.turn, Vec3{0.0f, -s.settings.length, 0.0f}).y,
                                                     rotate(s.turn, Vec3{0.0f, -s.settings.length, 0.0f}).z})) <
               1e-3;
}

} // namespace

GIANT_TEST("swing: it hangs still where it was put, drawn there, and sleeps in calm air") {
    Swings swings;
    swings.set(1, lantern(), at(0.0, 3.0, 0.0), k_lantern);
    const Swings::Swing* s = swings.find(1);
    GIANT_CHECK(s != nullptr);
    GIANT_CHECK(std::fabs(s->pivot.y - 3.5) < 1e-9);
    GIANT_CHECK(swings.poses().count(1) == 1 && std::fabs(swings.poses().at(1).position.y - 3.0) < 1e-6);
    GIANT_CHECK(swings.take_moved() == std::vector<world::EntityId>{1});
    run(swings, blowing(0.0f), 2.0f);
    GIANT_CHECK(swings.find(1)->asleep);
    GIANT_CHECK(swings.find(1)->angle() < 0.1f);
    GIANT_CHECK(swings.take_moved().empty()); // nothing moved: nothing to draw again
    GIANT_CHECK(swings.stats().asleep == 1 && swings.stats().simulated == 0);
}

GIANT_TEST("swing: the wind leans it downwind, further the harder it blows") {
    float lean[3] = {};
    const float speeds[3] = {5.0f, 15.0f, 35.0f};
    for (int k = 0; k < 3; ++k) {
        Swings swings;
        swings.set(1, lantern(), at(0.0, 3.0, 0.0), k_lantern);
        run(swings, blowing(speeds[k]), 8.0f);
        const Swings::Swing& s = *swings.find(1);
        GIANT_CHECK(s.bob.x > 0.0f); // downwind
        GIANT_CHECK(on_its_rod(s));
        lean[k] = s.angle();
    }
    GIANT_CHECK(lean[0] > 0.5f && lean[0] < lean[1] && lean[1] < lean[2]);
    // A 0.3 by 0.4 m lantern of 2 kg in 35 m/s: drag about 3.3 times its weight.
    GIANT_CHECK(lean[2] > 60.0f && lean[2] <= 80.0f + 1e-3f);
    GIANT_CHECK(lean[0] < 10.0f);
}

GIANT_TEST("swing: gusts keep it swinging, faster and wider in a storm") {
    // How much it moves about its mean lean, under the same gusty pattern.
    float travel[2] = {};
    const float speeds[2] = {4.0f, 25.0f};
    for (int k = 0; k < 2; ++k) {
        Swings swings;
        swings.set(1, lantern(), at(0.0, 3.0, 0.0), k_lantern);
        WindClock clock;
        Wind wind;
        wind.velocity = {speeds[k], 0.0f, 0.0f};
        wind.gustiness = 0.8f;
        Vec3 last{};
        for (int i = 0; i < 60 * 12; ++i) {
            clock.advance(wind, 1.0 / 60.0);
            swings.step(1.0 / 60.0, clock.state(), {});
            const Vec3 b = swings.find(1)->bob;
            if (i > 60 * 4) travel[k] += length(b - last);
            last = b;
        }
        GIANT_CHECK(!swings.find(1)->asleep);
    }
    GIANT_CHECK(travel[0] > 0.0f && travel[1] > 2.0f * travel[0]);
}

GIANT_TEST("swing: a hinged sign swings only across its hinge, its x axis staying level") {
    world::SwingSettings sign = lantern();
    sign.hinge = world::SwingHinge::x;
    sign.weight = 5.0f;
    world::Transform t = at(0.0, 3.0, 0.0);
    t.scale = {1.0f, 0.6f, 0.05f};
    // Turned a quarter about y: its x axis along world z, so it swings in x-y.
    t.rotation = quat_from_axis_angle({0.0f, 1.0f, 0.0f}, k_pi / 2.0f);
    Swings swings;
    swings.set(1, sign, t, t.scale);
    // Wind along x (across its face now) swings it; wind along its hinge does not.
    run(swings, blowing(15.0f), 6.0f);
    const Swings::Swing& s = *swings.find(1);
    GIANT_CHECK(s.angle() > 20.0f);
    GIANT_CHECK(std::fabs(s.bob.z) < 1e-4f);
    GIANT_CHECK(on_its_rod(s));
    const Vec3 x = rotate(s.pose.rotation, Vec3{1.0f, 0.0f, 0.0f});
    GIANT_CHECK(std::fabs(x.y) < 1e-3f && std::fabs(std::fabs(x.z) - 1.0f) < 1e-3f);
    Swings along;
    world::Transform u = t;
    u.rotation = Quat{};
    along.set(1, sign, u, u.scale); // its hinge along the wind
    run(along, blowing(15.0f), 4.0f);
    GIANT_CHECK(along.find(1)->angle() < 0.1f);
}

GIANT_TEST("swing: it never goes past its limit, however hard it blows or is struck") {
    world::SwingSettings s = lantern();
    s.limit = 30.0f;
    s.weight = 0.05f; // a paper lantern
    Swings swings;
    swings.set(1, s, at(0.0, 3.0, 0.0), k_lantern);
    run(swings, blowing(60.0f), 3.0f);
    GIANT_CHECK(swings.find(1)->angle() <= 30.0f + 1e-2f);
    for (int i = 0; i < 20; ++i) {
        GIANT_CHECK(swings.knock(1, {0.0f, 0.0f, 1000.0f}));
        run(swings, blowing(60.0f), 0.1f);
        GIANT_CHECK(swings.find(1)->angle() <= 30.0f + 1e-2f);
        GIANT_CHECK(on_its_rod(*swings.find(1)));
    }
    GIANT_CHECK(length(swings.find(1)->velocity) <= 50.0f + 1e-3f);
}

GIANT_TEST("swing: a shot knocks it and wakes it; a blast throws it away from where it went off") {
    Swings swings;
    swings.set(1, lantern(), at(0.0, 3.0, 0.0), k_lantern);
    swings.set(2, lantern(), at(4.0, 3.0, 0.0), k_lantern);
    run(swings, blowing(0.0f), 2.0f);
    GIANT_CHECK(swings.find(1)->asleep && swings.find(2)->asleep);
    swings.take_moved();
    // A bullet's few N s along +z.
    GIANT_CHECK(swings.knock(1, {0.0f, 0.0f, 6.0f}));
    GIANT_CHECK(!swings.knock(99, {0.0f, 0.0f, 6.0f}));
    GIANT_CHECK(!swings.find(1)->asleep && swings.find(2)->asleep);
    run(swings, blowing(0.0f), 0.2f);
    GIANT_CHECK(swings.find(1)->bob.z > 0.1f);
    GIANT_CHECK(swings.take_moved() == std::vector<world::EntityId>{1});
    // Swinging back and forth: it passes through hanging straight.
    bool back = false;
    for (int i = 0; i < 120; ++i) {
        swings.step(1.0 / 60.0, blowing(0.0f), {});
        back = back || swings.find(1)->bob.z < -0.05f;
    }
    GIANT_CHECK(back);
    // A blast at x = 2, low: each swings away from it.
    GIANT_CHECK(swings.blast({2.0, 2.5, 0.0}, 5.0f, 16.0f) == 2); // 8 m/s at the centre for 2 kg
    GIANT_CHECK(swings.blast({50.0, 2.5, 0.0}, 5.0f, 16.0f) == 0);
    run(swings, blowing(0.0f), 0.15f);
    GIANT_CHECK(swings.find(1)->bob.x < -0.05f && swings.find(2)->bob.x > 0.05f);
    // A push along a direction.
    GIANT_CHECK(swings.push({4.0, 3.0, 0.0}, 1.0f, {0.0f, 0.0f, -5.0f}) == 1);
    GIANT_CHECK(swings.push({4.0, 3.0, 0.0}, 0.0f, {0.0f, 0.0f, -5.0f}) == 0);
}

GIANT_TEST("swing: moved keeps its swing about the new pivot; new settings start it hanging again") {
    Swings swings;
    swings.set(1, lantern(), at(0.0, 3.0, 0.0), k_lantern);
    run(swings, blowing(15.0f), 4.0f);
    const float lean = swings.find(1)->angle();
    GIANT_CHECK(lean > 5.0f);
    swings.take_moved();
    swings.set(1, lantern(), at(10.0, 3.0, 0.0), k_lantern);
    GIANT_CHECK(std::fabs(swings.find(1)->angle() - lean) < 1e-3f);
    GIANT_CHECK(std::fabs(swings.find(1)->pivot.x - 10.0) < 1e-9);
    GIANT_CHECK(swings.poses().at(1).position.x > 10.0);
    GIANT_CHECK(swings.take_moved() == std::vector<world::EntityId>{1});
    world::SwingSettings longer = lantern();
    longer.length = 1.5f;
    swings.set(1, longer, at(10.0, 3.0, 0.0), k_lantern);
    GIANT_CHECK(swings.find(1)->angle() < 1e-3f && std::fabs(swings.find(1)->pivot.y - 4.5) < 1e-6);
    swings.remove(1);
    GIANT_CHECK(swings.find(1) == nullptr && swings.poses().empty());
    GIANT_CHECK(swings.take_moved() == std::vector<world::EntityId>{1});
    // Removed and added again before the frame takes them: listed once.
    swings.set(2, lantern(), at(0.0, 3.0, 0.0), k_lantern);
    swings.set(1, lantern(), at(0.0, 3.0, 0.0), k_lantern);
    swings.take_moved();
    swings.remove(1);
    swings.set(1, lantern(), at(0.0, 3.0, 0.0), k_lantern);
    swings.clear();
    swings.set(2, lantern(), at(0.0, 3.0, 0.0), k_lantern);
    GIANT_CHECK((swings.take_moved() == std::vector<world::EntityId>{1, 2}));
}

GIANT_TEST("swing: bounded work: the nearest move, far ones hold their pose, a bad number resets") {
    SwingSettingsSim settings;
    settings.max_simulated = 2;
    settings.distance = 50.0;
    Swings swings(settings);
    for (world::EntityId id = 1; id <= 4; ++id) {
        swings.set(id, lantern(), at(static_cast<double>(id) * 10.0, 3.0, 0.0), k_lantern);
    }
    swings.set(5, lantern(), at(500.0, 3.0, 0.0), k_lantern); // out of reach
    run(swings, blowing(10.0f), 1.0f, {0.0, 3.0, 0.0});
    GIANT_CHECK(swings.stats().swings == 5 && swings.stats().simulated == 2);
    GIANT_CHECK(swings.stats().steps == 1);
    GIANT_CHECK(swings.find(1)->angle() > 1.0f && swings.find(2)->angle() > 1.0f);
    GIANT_CHECK(swings.find(3)->angle() < 1e-3f && swings.find(5)->angle() < 1e-3f);
    // Sleepers count towards those looked at (four per one that may move):
    // past that, the farthest are not looked at, even to wake.
    {
        SwingSettingsSim few;
        few.max_simulated = 1;
        Swings many(few);
        for (world::EntityId id = 1; id <= 10; ++id) {
            many.set(id, lantern(), at(static_cast<double>(id), 3.0, 0.0), k_lantern);
        }
        run(many, blowing(0.0f), 0.5f); // 1 moves a frame; the rest wait
        GIANT_CHECK(many.stats().simulated == 1);
        GIANT_CHECK(many.stats().simulated + many.stats().asleep <= 10);
        GIANT_CHECK(many.find(10)->angle() < 1e-3f);
    }
    settings.distance = -50.0; // no reach: nothing moves
    {
        Swings none(settings);
        none.set(1, lantern(), at(1.0, 3.0, 0.0), k_lantern);
        none.step(1.0 / 60.0, blowing(10.0f), {0.0, 3.0, 0.0});
        GIANT_CHECK(none.stats().simulated == 0);
    }
    // A long frame takes at most max_steps.
    swings.step(1.0, blowing(10.0f), {0.0, 3.0, 0.0});
    GIANT_CHECK(swings.stats().steps == 4);
    swings.step(std::nan(""), blowing(10.0f), {0.0, 3.0, 0.0});
    GIANT_CHECK(swings.stats().steps == 0);
    // Bad settings are clamped; a bad wind is none.
    world::SwingSettings bad = lantern();
    bad.length = -3.0f;
    bad.weight = std::nanf("");
    bad.limit = 900.0f;
    swings.set(6, bad, at(0.0, 3.0, 0.0), k_lantern);
    const Swings::Swing& s = *swings.find(6);
    GIANT_CHECK(s.settings.length == 0.02f && s.settings.weight == 2.0f && s.settings.limit == 175.0f);
    Swings lone;
    lone.set(1, lantern(), at(0.0, 3.0, 0.0), k_lantern);
    lone.step(1.0 / 60.0, blowing(std::nanf("")), {});
    GIANT_CHECK(lone.stats().resets == 0 && lone.find(1)->angle() < 1e-3f);
    GIANT_CHECK(std::isfinite(lone.poses().at(1).position.y));
    GIANT_CHECK(can_swing(world::Shape::kit) && can_swing(world::Shape::mesh) && !can_swing(world::Shape::cloth) &&
                !can_swing(world::Shape::decal));
}
