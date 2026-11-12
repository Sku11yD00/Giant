// Water in the physics world (PhysicsWorld::set_water): bodies float or
// sink by their weight, a river carries them along, characters wade, swim,
// dive and float back up, and walk out again.
#include "giant/sim/physics.h"
#include "giant/world/scene.h"
#include "giant/world/water.h"

#include "giant_test.h"

#include <cmath>
#include <limits>
#include <vector>

using namespace giant;
using namespace giant::sim;

namespace {

BodyDesc box(DVec3 at, Vec3 half, Motion motion = Motion::fixed, float mass = 0.0f) {
    BodyDesc d;
    d.motion = motion;
    d.position = at;
    d.mass = mass;
    d.parts.push_back({BoxShape{half}});
    return d;
}

void run(PhysicsWorld& physics, double seconds) {
    for (int i = 0; i < static_cast<int>(std::lround(seconds * 60.0)); ++i) physics.step(1.0 / 60.0);
}

world::Water lake(float level, double half = 50.0) {
    world::Water w;
    w.name = "lake";
    w.kind = world::WaterKind::lake;
    w.level = level;
    w.outline = {{-half, -half}, {half, -half}, {half, half}, {-half, half}};
    return w;
}

} // namespace

GIANT_TEST("water: light bodies float at their share, heavy ones sink, the default mass hangs, a roof keeps it out") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    physics.add(box({0.0, -4.5, 0.0}, {100.0f, 0.5f, 100.0f})); // a bed 4 m down
    // 1 m cubes: 400 kg (floats 0.4 under), 3000 kg (sinks), default (1000 kg: hangs).
    const BodyId light = physics.add(box({0.0, 1.0, 0.0}, {0.5f, 0.5f, 0.5f}, Motion::dynamic, 400.0f));
    const BodyId heavy = physics.add(box({5.0, 1.0, 0.0}, {0.5f, 0.5f, 0.5f}, Motion::dynamic, 3000.0f));
    const BodyId even = physics.add(box({10.0, -2.0, 0.0}, {0.5f, 0.5f, 0.5f}, Motion::dynamic));
    // Under a fixed roof (a tunnel under the lake): no water there.
    physics.add(box({20.0, -1.0, 0.0}, {3.0f, 0.25f, 3.0f}));
    const BodyId roofed = physics.add(box({20.0, -3.4, 0.0}, {0.5f, 0.5f, 0.5f}, Motion::dynamic, 400.0f));
    const std::vector<world::Water> water{lake(0.0f)};
    physics.set_water(water);
    GIANT_CHECK(physics.water().size() == 1 && physics.water()[0].name == "lake");
    run(physics, 6.0);
    const BodyState l = *physics.state(light);
    // Floating: 0.4 m of it under, the centre 0.1 m over the surface.
    GIANT_CHECK(std::fabs(l.position.y - 0.1) < 0.02 && std::fabs(l.linear_velocity.y) < 0.05f);
    GIANT_CHECK(std::fabs(physics.state(heavy)->position.y - -3.5) < 0.05); // on the bed
    GIANT_CHECK(std::fabs(physics.state(even)->position.y - -2.0) < 0.02);  // where it was
    GIANT_CHECK(std::fabs(physics.state(roofed)->position.y - -3.5) < 0.05); // on the bed, dry
    // No water: the floating one (asleep) is woken and falls to the bed.
    physics.set_water({});
    run(physics, 2.0);
    GIANT_CHECK(std::fabs(physics.state(light)->position.y - -3.5) < 0.05);
    // Water again: it is woken (it slept on the bed) and comes up.
    physics.set_water(water);
    run(physics, 6.0);
    GIANT_CHECK(std::fabs(physics.state(light)->position.y - 0.1) < 0.02);
    // Lowered under it: it falls with it.
    physics.set_water(std::vector<world::Water>{lake(-2.0f)});
    run(physics, 3.0);
    GIANT_CHECK(std::fabs(physics.state(light)->position.y - -1.9) < 0.05);
    // Hidden: not there.
    world::Water hidden = lake(0.0f);
    hidden.visible = false;
    physics.set_water(std::vector<world::Water>{hidden});
    run(physics, 2.0);
    GIANT_CHECK(std::fabs(physics.state(light)->position.y - -3.5) < 0.05);
    // A cleared world keeps its water.
    physics.set_water(water);
    physics.clear();
    GIANT_CHECK(physics.water().size() == 1);
    const BodyId again = physics.add(box({0.0, 1.0, 0.0}, {0.5f, 0.5f, 0.5f}, Motion::dynamic, 400.0f));
    run(physics, 6.0);
    GIANT_CHECK(std::fabs(physics.state(again)->position.y - 0.1) < 0.02);
}

GIANT_TEST("water: past a waterfall the upper level is not there; rivers listed either way agree; a tunnel is dry") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    physics.add(box({0.0, -0.5, 0.0}, {200.0f, 0.5f, 200.0f})); // ground: top y 0
    // A river falling 6 m between x 50 and 51; a dry shelf (top 0.5) just past the drop.
    world::Water fall;
    fall.name = "fall";
    fall.kind = world::WaterKind::river;
    fall.path = {{0.0, 0.0, 10.0f, 6.0f}, {50.0, 0.0, 10.0f, 6.0f}, {51.0, 0.0, 10.0f, 0.0f}, {150.0, 0.0, 10.0f, 0.0f}};
    // Two rivers over (-3, 3): one whose nearest stretch there is at level 3 (a farther one at
    // 6 reaches too), one at 5: the water there is at 5 (world::water_at's), either way round.
    world::Water bend;
    bend.name = "bend";
    bend.kind = world::WaterKind::river;
    bend.path = {{-30.0, -100.0, 10.0f, 6.0f}, {-6.0, -100.0, 10.0f, 6.0f}, {-6.0, -94.0, 10.0f, 0.0f}};
    world::Water straight;
    straight.name = "straight";
    straight.kind = world::WaterKind::river;
    straight.path = {{-3.0, -150.0, 4.0f, 5.0f}, {-3.0, -50.0, 4.0f, 5.0f}};
    // A lake 5 m deep over a tunnel (floor y 0, ceiling 2.5 m up) at x -100.
    world::Water lake_over = lake(5.0f, 20.0);
    for (world::WaterPoint& p : lake_over.outline) p.x -= 100.0;
    physics.add(box({-100.0, 3.0, 0.0}, {10.0f, 0.5f, 3.0f}));
    for (int order = 0; order < 2; ++order) {
        const std::vector<world::Water> water =
            order == 0 ? std::vector<world::Water>{fall, bend, straight, lake_over}
                       : std::vector<world::Water>{straight, lake_over, bend, fall};
        physics.set_water(water);
        const std::optional<world::WaterHere> expect = world::water_at(water, -3.0, -97.0);
        GIANT_CHECK(expect && std::fabs(expect->level - 5.0f) < 1e-4f);
        CharacterDesc d;
        d.position = {-3.0, 0.0, -97.0};
        const CharacterId at_bend = physics.add_character(d);
        d.position = {-100.0, 0.0, 0.0}; // in the tunnel
        const CharacterId in_tunnel = physics.add_character(d);
        run(physics, 1.0 / 60.0);
        GIANT_CHECK(std::fabs(physics.character_state(at_bend)->water_depth - 5.0f) < 0.05f);
        GIANT_CHECK(!physics.character_state(in_tunnel)->swimming && physics.character_state(in_tunnel)->water_depth == 0.0f);
        physics.remove_character(at_bend);
        physics.remove_character(in_tunnel);
    }
    // The water reaches a point in the open water, over the tunnel's roof,
    // and over the surface; not in the tunnel (the camera under water).
    GIANT_CHECK(physics.water_reaches({-3.0, 1.0, -97.0}, 5.0));
    GIANT_CHECK(physics.water_reaches({-100.0, 4.5, 0.0}, 5.0));
    GIANT_CHECK(physics.water_reaches({-100.0, 6.0, 0.0}, 5.0));
    GIANT_CHECK(!physics.water_reaches({-100.0, 1.6, 0.0}, 5.0));
    GIANT_CHECK(!physics.water_reaches({-100.0, std::numeric_limits<double>::quiet_NaN(), 0.0}, 5.0));
    // A crate on the shelf past the drop stays dry; one in the lower river floats there.
    physics.add(box({53.0, 0.25, 0.0}, {1.0f, 0.25f, 1.0f}));
    const BodyId dry = physics.add(box({53.0, 0.75, 0.0}, {0.25f, 0.25f, 0.25f}, Motion::dynamic, 20.0f));
    run(physics, 3.0);
    GIANT_CHECK(std::fabs(physics.state(dry)->position.y - 0.75) < 0.02);
}

GIANT_TEST("water: under a pier over the bed is open water; a room with its own floor is dry") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    // The terrain's bed at y 0 (a heightfield), a lake 5 m deep over it.
    BodyDesc bed;
    HeightShape h;
    h.samples = 9;
    h.spacing = 10.0f;
    h.heights.assign(81, 0.0f);
    bed.parts.push_back({h});
    bed.position = {-40.0, 0.0, -40.0};
    physics.add(bed);
    physics.add(box({0.0, 3.0, 0.0}, {4.0f, 0.25f, 4.0f}));   // a pier's deck over the bed
    physics.add(box({20.0, 3.0, 0.0}, {4.0f, 0.25f, 4.0f}));  // a room's roof...
    physics.add(box({20.0, 0.5, 0.0}, {4.0f, 0.25f, 4.0f}));  // ...and its floor
    run(physics, 1.0 / 60.0);
    GIANT_CHECK(physics.water_reaches({0.0, 1.5, 0.0}, 5.0));
    GIANT_CHECK(!physics.water_reaches({20.0, 1.5, 0.0}, 5.0));
}

GIANT_TEST("water: a river carries a floating body down its stretches; a sunk rock stays; out of it, nothing") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    physics.add(box({0.0, -2.5, 0.0}, {300.0f, 0.5f, 300.0f}));
    world::Water river;
    river.name = "river";
    river.kind = world::WaterKind::river;
    river.flow = 2.0f;
    // Three stretches (the middle one long: many grid squares), the middle point at x 0.
    river.path = {{-100.0, 0.0, 20.0f, 0.0f}, {-5.0, 0.0, 20.0f, 0.0f}, {200.0, 0.0, 20.0f, 0.0f}, {250.0, 50.0, 20.0f, 0.0f}};
    const std::vector<world::Water> water{river};
    physics.set_water(water);
    const BodyId log = physics.add(box({-10.0, 0.5, 0.0}, {1.0f, 0.25f, 0.25f}, Motion::dynamic, 100.0f));
    const BodyId rock = physics.add(box({-20.0, -1.5, 3.0}, {0.5f, 0.5f, 0.5f}, Motion::dynamic, 3000.0f));
    BodyDesc boulder; // round, granite (2600 kg a cubic metre)
    boulder.motion = Motion::dynamic;
    boulder.position = {-20.0, -1.5, -3.0};
    boulder.mass = 2600.0f * 4.18879f * 0.125f;
    boulder.parts.push_back({SphereShape{0.5f}});
    const BodyId round = physics.add(boulder);
    // In the river's box (by its last stretch) but not its strip.
    const BodyId dry = physics.add(box({150.0, -1.5, 40.0}, {0.5f, 0.5f, 0.5f}, Motion::dynamic, 100.0f));
    run(physics, 6.0);
    const BodyState s = *physics.state(log);
    // Drifting downstream (+x, over from the first stretch to the second) near the flow's speed, afloat.
    GIANT_CHECK(s.position.x > -10.0 + 8.0 && std::fabs(s.linear_velocity.x - 2.0f) < 0.3f);
    GIANT_CHECK(std::fabs(s.position.y) < 0.3 && std::fabs(s.position.z) < 1.0);
    // The rock (three times water's density) on the bed: not swept along.
    GIANT_CHECK(std::fabs(physics.state(rock)->position.x - -20.0) < 0.05);
    GIANT_CHECK(std::fabs(physics.state(round)->position.x - -20.0) < 1.0); // (the drag alone: it may roll a little)
    const BodyState d = *physics.state(dry);
    GIANT_CHECK(std::fabs(d.position.x - 150.0) < 0.01 && std::fabs(d.position.y - -1.5) < 0.05);
}

GIANT_TEST("water: a character wades slower, swims at the surface, dives, floats up and walks out") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    // A shore at y 0 for x < 0, a shelf at -1 to x = 10, then deep water (its bed at -12).
    physics.add(box({-50.0, -0.5, 0.0}, {50.0f, 0.5f, 50.0f}));      // dry and shallow: top y 0
    physics.add(box({60.0, -12.5, 0.0}, {50.0f, 0.5f, 50.0f}));      // deep: top y -12
    physics.add(box({5.0, -1.25, 0.0}, {5.0f, 0.25f, 50.0f}));       // a shelf: top y -1
    // A jetty out over the deep water, its deck 0.3 m over the surface.
    physics.add(box({40.0, 1.0, 20.0}, {5.0f, 0.1f, 5.0f}));
    // The lake: its surface 0.8 m over the shore (x from -20 on).
    world::Water w = lake(0.8f, 0.0);
    w.outline = {{-20.0, -50.0}, {110.0, -50.0}, {110.0, 50.0}, {-20.0, 50.0}};
    const std::vector<world::Water> water{w};
    physics.set_water(water);

    CharacterDesc desc;
    desc.position = {-40.0, 0.0, 0.0};
    const CharacterId c = physics.add_character(desc);
    GIANT_CHECK(c != k_no_character);
    // On dry land: 4 m/s.
    physics.set_character_input(c, {{4.0f, 0.0f, 0.0f}});
    run(physics, 1.0);
    CharacterState s = *physics.character_state(c);
    GIANT_CHECK(!s.swimming && s.water_depth == 0.0f && std::fabs(s.velocity.x - 4.0f) < 0.1f);
    // In 0.8 m of water: slowed (to 3/4: half way from 0.3 m to swim_depth, slowest half speed).
    physics.set_character_position(c, {-10.0, 0.0, 0.0});
    run(physics, 1.0);
    s = *physics.character_state(c);
    GIANT_CHECK(!s.swimming && s.on_ground && std::fabs(s.water_depth - 0.8f) < 0.05f);
    GIANT_CHECK(std::fabs(s.velocity.x - 3.0f) < 0.1f);
    // Out in the deep, let be: floats up to rest, feet swim_depth under the surface.
    physics.set_character_input(c, {});
    physics.set_character_position(c, {16.0, -3.0, 0.0});
    run(physics, 6.0); // (floating up 2.5 m at 0.6 m/s)
    s = *physics.character_state(c);
    GIANT_CHECK(s.swimming && !s.on_ground && std::fabs(s.position.y - (0.8 - 1.3)) < 0.02);
    // Swimming toward the shore at swim_speed; up asked for at the surface: no higher.
    physics.set_character_input(c, {{-4.0f, 1.6f, 0.0f}});
    run(physics, 1.0);
    s = *physics.character_state(c);
    GIANT_CHECK(s.swimming && std::fabs(s.velocity.x - -1.6f) < 0.05f && std::fabs(s.position.y - (0.8 - 1.3)) < 0.02);
    // Diving, then let be: floats back up.
    physics.set_character_input(c, {{0.0f, -1.6f, 0.0f}});
    run(physics, 2.0);
    s = *physics.character_state(c);
    GIANT_CHECK(s.swimming && s.position.y < -3.0 && s.position.y > -4.0);
    physics.set_character_input(c, {});
    run(physics, 6.0);
    s = *physics.character_state(c);
    GIANT_CHECK(s.swimming && std::fabs(s.position.y - (0.8 - 1.3)) < 0.02);
    // On to the shore: pulled up onto its edge (0.5 m over its feet, shallow enough to
    // stand), then wading out (0.8 m deep: 3/4 speed).
    physics.set_character_input(c, {{-4.0f, 0.0f, 0.0f}});
    run(physics, 12.0);
    s = *physics.character_state(c);
    GIANT_CHECK(!s.swimming && s.on_ground && s.position.x < -3.0 && std::fabs(s.velocity.x - -3.0f) < 0.1f);
    // Under the jetty, diving: it does not come up through the deck.
    physics.set_character_position(c, {40.0, -3.0, 20.0});
    physics.set_character_input(c, {{0.0f, -0.5f, 1.6f}});
    run(physics, 2.0);
    s = *physics.character_state(c);
    GIANT_CHECK(s.swimming && s.position.y < -2.0);
    // At the surface beside it: pulls itself up onto the deck.
    physics.set_character_position(c, {40.0, -0.5, 13.0});
    physics.set_character_input(c, {{0.0f, 0.0f, 1.6f}});
    run(physics, 2.0);
    s = *physics.character_state(c);
    GIANT_CHECK(!s.swimming && s.on_ground && std::fabs(s.position.y - 1.1) < 0.05 && s.position.z > 15.0);
    // A fall into deep water (at about 13 m/s) stops within metres, and comes up.
    physics.set_character_input(c, {});
    physics.set_character_position(c, {80.0, 10.0, 0.0});
    double lowest = 100.0;
    for (int i = 0; i < 300; ++i) {
        run(physics, 1.0 / 60.0);
        lowest = std::min(lowest, physics.character_state(c)->position.y);
    }
    GIANT_CHECK(lowest < -1.5 && lowest > -6.0 && physics.character_state(c)->swimming);
    // A small character (1 m) swims at three quarters of its height.
    CharacterDesc small = desc;
    small.height = 1.0f;
    small.position = {60.0, -2.0, -20.0};
    const CharacterId child = physics.add_character(small);
    GIANT_CHECK(child != k_no_character);
    run(physics, 5.0);
    s = *physics.character_state(child);
    GIANT_CHECK(s.swimming && std::fabs(s.position.y - (0.8 - 0.75)) < 0.02);
    // Bad numbers refused.
    CharacterDesc bad = desc;
    bad.swim_depth = 0.0f;
    GIANT_CHECK(physics.add_character(bad) == k_no_character);
    bad.swim_depth = 1.3f;
    bad.swim_speed = std::nanf("");
    GIANT_CHECK(physics.add_character(bad) == k_no_character);
}

GIANT_TEST("water: a body on the sea rides the swell, up and down with the surface, and does not fall asleep") {
    JobSystem jobs(2);
    PhysicsWorld physics(jobs);
    physics.add(box({0.0, -20.5, 0.0}, {200.0f, 0.5f, 200.0f})); // the bed, deep
    world::Water sea;
    sea.name = "sea";
    sea.kind = world::WaterKind::sea;
    sea.level = 0.0f;
    sea.waves = 1.0f;
    physics.set_water(std::vector<world::Water>{sea});
    // One wave 40 m long, 0.5 m up and down, running along x.
    world::Swell swell;
    swell.waves[0] = {1.0f, 0.0f, static_cast<float>(6.283185307179586 / 40.0), 0.5f, 0.0f};
    physics.set_swell(swell, 100.0);
    GIANT_CHECK(physics.swell() == swell && physics.swell_seconds() == 100.0);
    // A light raft (200 kg a cubic metre: floats 0.08 m under, its centre 0.12 m over the surface).
    const BodyId raft = physics.add(box({7.0, 0.5, 3.0}, {1.0f, 0.2f, 1.0f}, Motion::dynamic, 320.0f));
    run(physics, 5.0);
    double low = 1e9, high = -1e9, off = 0.0;
    int n = 0;
    for (int i = 0; i < 12 * 60; ++i) {
        physics.step(1.0 / 60.0);
        const BodyState s = *physics.state(raft);
        const double surface = world::swell_height(swell, s.position.x, s.position.z, physics.swell_seconds());
        low = std::min(low, s.position.y);
        high = std::max(high, s.position.y);
        off += std::fabs(s.position.y - 0.12 - surface);
        ++n;
    }
    // Up and down nearly as much as the surface (1 m), never far from it.
    GIANT_CHECK(high - low > 0.7 && high - low < 1.3);
    GIANT_CHECK(off / n < 0.15);
    GIANT_CHECK(physics.state(raft)->active);
    // The clock moved on with the steps (5 s + 12 s).
    GIANT_CHECK(std::fabs(physics.swell_seconds() - 117.0) < 0.05);
    // A still sea (its waves 0): flat, the raft settles on the level.
    sea.waves = 0.0f;
    physics.set_water(std::vector<world::Water>{sea});
    run(physics, 6.0);
    // (However it came to lie: on the level, still.)
    GIANT_CHECK(std::fabs(physics.state(raft)->position.y) < 0.25 && std::fabs(physics.state(raft)->linear_velocity.y) < 0.02f);
    // A calm swell (none) then one again: its sleeper is woken and bobs.
    sea.waves = 1.0f;
    physics.set_swell(world::Swell{}, 0.0);
    physics.set_water(std::vector<world::Water>{sea});
    run(physics, 6.0);
    const double rest = physics.state(raft)->position.y;
    physics.set_swell(swell, 10.0);
    low = 1e9;
    high = -1e9;
    for (int i = 0; i < 8 * 60; ++i) {
        physics.step(1.0 / 60.0);
        low = std::min(low, physics.state(raft)->position.y);
        high = std::max(high, physics.state(raft)->position.y);
    }
    GIANT_CHECK(std::fabs(rest) < 0.25 && high - low > 0.7);
    // A rock sunk to the bed under the swell falls asleep (only what floats is kept awake).
    const BodyId rock = physics.add(box({30.0, -15.0, 0.0}, {0.5f, 0.5f, 0.5f}, Motion::dynamic, 3000.0f));
    run(physics, 8.0);
    GIANT_CHECK(std::fabs(physics.state(rock)->position.y - -19.5) < 0.05 && !physics.state(rock)->active);
    GIANT_CHECK(physics.state(raft)->active);
    // The clock set with time still to step: that much behind.
    physics.step(0.5 / 60.0);
    physics.set_swell(swell, 500.0);
    GIANT_CHECK(std::fabs(physics.swell_seconds() - (500.0 - physics.step_fraction() / 60.0)) < 1e-9);
}
