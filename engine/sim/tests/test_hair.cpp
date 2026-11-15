// Hair: guide strands hang, swing and blow about without stretching, keep
// out of the character's capsules, and work stays bounded.
#include "giant/sim/hair.h"

#include "giant_test.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace giant;
using namespace giant::sim;

namespace {

constexpr float k_piece = 0.02f; // metres between a strand's points: 0.3 m strands

// `count` guides rooted along z at x = 0, y = 0, each pointing along `way`.
std::vector<Vec3> guides(u32 count, const Vec3& way) {
    std::vector<Vec3> points;
    for (u32 g = 0; g < count; ++g) {
        const Vec3 root{0.0f, 0.0f, 0.05f * static_cast<float>(g)};
        for (u32 k = 0; k < k_hair_points; ++k) points.push_back(root + way * (k_piece * static_cast<float>(k)));
    }
    return points;
}

void run(HairSim& hair, const HairFrame& frame, float seconds) {
    for (float t = 0.0f; t < seconds; t += 1.0f / 60.0f) hair.step(1.0 / 60.0, frame);
}

// The most any piece's length is off its rest length, as a share of it.
float stretch(const HairSim& hair) {
    float worst = 0.0f;
    const std::span<const Vec3> now = hair.points();
    const std::span<const Vec3> rest = hair.rest();
    for (usize i = 1; i < now.size(); ++i) {
        if (i % k_hair_points == 0) continue;
        const float was = length(rest[i] - rest[i - 1]);
        worst = std::max(worst, std::abs(length(now[i] - now[i - 1]) - was) / was);
    }
    return worst;
}

float off_rest(const HairSim& hair) {
    float worst = 0.0f;
    for (usize i = 0; i < hair.points().size(); ++i) {
        worst = std::max(worst, length(hair.points()[i] - hair.rest()[i]));
    }
    return worst;
}

Vec3 tip(const HairSim& hair, u32 guide = 0) { return hair.points()[guide * k_hair_points + k_hair_points - 1]; }

// The point of a capsule's axis nearest `p`.
Vec3 nearest_point(const HairCapsule& c, const Vec3& p) {
    const Vec3 ab = c.b - c.a;
    const float t = std::clamp(dot(p - c.a, ab) / length_squared(ab), 0.0f, 1.0f);
    return c.a + ab * t;
}

HairSettings loose() {
    HairSettings s;
    s.root_stiffness = 0.0f;
    s.tip_stiffness = 0.0f;
    return s;
}

} // namespace

GIANT_TEST("hair: with nothing pulling, hair stays in its style; gravity makes it hang") {
    const std::vector<Vec3> rest = guides(4, {1.0f, 0.0f, 0.0f}); // straight out sideways
    HairSettings weightless;
    weightless.gravity = 0.0f;
    HairSim still(rest, weightless);
    GIANT_CHECK(still.guide_count() == 4 && still.stats().guides == 4);
    run(still, HairFrame{}, 2.0f);
    GIANT_CHECK(off_rest(still) < 1e-5f);
    // Loose hair hangs straight down from its root, as long as it was.
    HairSim hanging(rest, loose());
    run(hanging, HairFrame{}, 10.0f);
    for (u32 g = 0; g < 4; ++g) {
        const Vec3 root = hanging.points()[g * k_hair_points];
        GIANT_CHECK_NEAR(length(root - rest[g * k_hair_points]), 0.0, 1e-6);
        GIANT_CHECK(tip(hanging, g).y < -0.28f && std::abs(tip(hanging, g).x) < 0.05f);
    }
    GIANT_CHECK(stretch(hanging) < 1e-3f);
    const u64 hung = hanging.version();
    run(hanging, HairFrame{}, 1.0f);
    GIANT_CHECK(hanging.version() == hung); // and still
    // Styled hair sags, less the stiffer it is, and comes to rest.
    HairSim styled(rest);
    HairSettings stiff_settings;
    stiff_settings.root_stiffness = 60.0f;
    stiff_settings.tip_stiffness = 30.0f;
    HairSim stiff(rest, stiff_settings);
    run(styled, HairFrame{}, 10.0f);
    run(stiff, HairFrame{}, 10.0f);
    GIANT_CHECK(tip(styled).y < tip(stiff).y - 0.01f && tip(styled).y > tip(hanging).y + 0.02f);
    GIANT_CHECK(tip(stiff).y < 0.0f && tip(stiff).y > -0.05f);
    const Vec3 before = tip(styled);
    const u64 version = styled.version();
    run(styled, HairFrame{}, 1.0f);
    GIANT_CHECK(length(tip(styled) - before) < 1e-4f);
    GIANT_CHECK(styled.version() == version); // at rest nothing changes
    GIANT_CHECK(stretch(styled) < 1e-3f && stretch(stiff) < 1e-3f);
    // The style is the same whatever the step: finer steps sag alike.
    HairSettings coarse;
    coarse.step_seconds = 1.0f / 30.0f;
    HairSettings fine;
    fine.step_seconds = 1.0f / 120.0f;
    HairSim slow(rest, coarse);
    HairSim fast(rest, fine);
    for (int i = 0; i < 600; ++i) {
        slow.step(1.0 / 30.0, HairFrame{});
        for (int k = 0; k < 4; ++k) fast.step(1.0 / 120.0, HairFrame{});
    }
    std::printf("hair: tip sag %.4f (1/30 s), %.4f (1/60 s), %.4f (1/120 s)\n", static_cast<double>(-tip(slow).y),
                static_cast<double>(-tip(styled).y), static_cast<double>(-tip(fast).y));
    GIANT_CHECK(std::abs(tip(slow).y - tip(fast).y) < 0.5f * std::abs(tip(styled).y));
}

GIANT_TEST("hair: a swing never gains height or length, and dies away with damping") {
    // Let go from straight out sideways, nothing holding or slowing it: it
    // may swing back up as high as it began and no higher.
    const std::vector<Vec3> rest = guides(1, {1.0f, 0.0f, 0.0f});
    HairSettings free_settings = loose();
    free_settings.damping = 0.0f;
    free_settings.wind_response = 0.0f;
    float highest = 0.0f;
    float worst = 0.0f;
    for (float step : {1.0f / 60.0f, 1.0f / 240.0f}) {
        free_settings.step_seconds = step;
        HairSim swing(rest, free_settings);
        for (int i = 0; i < static_cast<int>(5.0f / step); ++i) {
            swing.step(static_cast<double>(step), HairFrame{});
            for (const Vec3& p : swing.points()) highest = std::max(highest, p.y);
            worst = std::max(worst, stretch(swing));
        }
    }
    std::printf("hair: free swing rose %.4f m above its root, stretched %.4f\n", static_cast<double>(highest),
                static_cast<double>(worst));
    GIANT_CHECK(highest < 0.01f);
    GIANT_CHECK(worst < 0.03f);
    // Damping: the swing dies sooner.
    const auto swung = [&](float damping) {
        HairSettings s = loose();
        s.damping = damping;
        s.wind_response = 0.0f;
        HairSim h(rest, s);
        run(h, HairFrame{}, 2.0f);
        float moving = 0.0f;
        for (int i = 0; i < 60; ++i) {
            const Vec3 was = tip(h);
            h.step(1.0 / 60.0, HairFrame{});
            moving = std::max(moving, length(tip(h) - was));
        }
        return moving;
    };
    GIANT_CHECK(swung(8.0f) < swung(0.0f) * 0.5f);
}

GIANT_TEST("hair: the wind blows it along, harder in a storm, and it never stretches") {
    const std::vector<Vec3> rest = guides(8, {0.0f, -1.0f, 0.0f}); // hanging
    HairSim breeze(rest);
    HairSim storm(rest);
    HairSim calm(rest);
    HairFrame light;
    light.wind = {3.0f, 0.0f, 0.0f};
    HairFrame hard;
    hard.wind = {30.0f, 0.0f, 0.0f};
    float worst = 0.0f;
    for (int i = 0; i < 300; ++i) {
        breeze.step(1.0 / 60.0, light);
        storm.step(1.0 / 60.0, hard);
        calm.step(1.0 / 60.0, HairFrame{});
        worst = std::max({worst, stretch(breeze), stretch(storm)});
    }
    std::printf("hair: worst stretch in a storm %.4f\n", static_cast<double>(worst));
    GIANT_CHECK(worst < 0.03f);
    GIANT_CHECK(std::abs(tip(calm).x) < 1e-4f);
    GIANT_CHECK(tip(breeze).x > 0.005f);
    GIANT_CHECK(tip(storm).x > tip(breeze).x * 2.0f && tip(storm).x > 0.1f);
    GIANT_CHECK(std::abs(tip(storm).z) < 1e-4f); // along the wind only
    // No response, no blowing.
    HairSettings deaf;
    deaf.wind_response = 0.0f;
    HairSim unmoved(rest, deaf);
    run(unmoved, hard, 2.0f);
    GIANT_CHECK(std::abs(tip(unmoved).x) < 1e-5f);
    // The drag is on the air passing the strand: hair carried along at the
    // wind's own speed feels none.
    HairSim carried(rest);
    HairFrame drifting;
    drifting.wind = {3.0f, 0.0f, 0.0f};
    run(carried, HairFrame{}, 0.5f);
    for (int i = 0; i < 600; ++i) {
        drifting.position.x += 3.0 / 60.0;
        carried.step(1.0 / 60.0, drifting);
    }
    GIANT_CHECK(std::abs(tip(carried).x) < 2e-3f);
}

GIANT_TEST("hair: a moving head swings it, a turned or scaled one carries it whole") {
    const std::vector<Vec3> rest = guides(4, {0.0f, -1.0f, 0.0f});
    HairSim hair(rest);
    HairFrame frame;
    frame.position = {5000.0, 100.0, -3000.0}; // far from the origin
    run(hair, frame, 1.0f);
    GIANT_CHECK(off_rest(hair) < 1e-4f);
    // The head sets off along +x at 3 m/s: the tips trail behind.
    const u64 version = hair.version();
    float trailed = 0.0f;
    for (int i = 0; i < 30; ++i) {
        frame.position.x += 3.0 / 60.0;
        hair.step(1.0 / 60.0, frame);
        trailed = std::min(trailed, tip(hair).x);
        GIANT_CHECK_NEAR(length(hair.points()[0] - rest[0]), 0.0, 1e-5); // roots ride along
    }
    GIANT_CHECK(trailed < -0.02f);
    GIANT_CHECK(hair.version() != version);
    GIANT_CHECK(stretch(hair) < 0.03f);
    // Stopped, it settles back.
    run(hair, frame, 10.0f);
    GIANT_CHECK(off_rest(hair) < 1e-3f);
    GIANT_CHECK(hair.stats().resets == 0);

    // Placed turned and scaled: at rest in its own space, hanging on.
    HairSettings weightless;
    weightless.gravity = 0.0f;
    HairSim turned(rest, weightless);
    HairFrame tilted;
    tilted.rotation = quat_from_axis_angle({0.0f, 0.0f, 1.0f}, 1.0f);
    tilted.scale = 2.0f;
    run(turned, tilted, 2.0f);
    GIANT_CHECK(off_rest(turned) < 1e-4f && stretch(turned) < 1e-4f);
    // A new size resizes it as it hangs: no reset, no snap from one size's
    // lengths to the other's, and points() stays right for sizes that
    // change a little at a time.
    HairSim grown(rest, loose());
    HairFrame sized;
    sized.rotation = quat_from_axis_angle({0.0f, 0.0f, 1.0f}, 1.0f);
    run(grown, sized, 5.0f); // hangs down in the world, off its style
    const Vec3 hung = tip(grown);
    u64 sized_version = grown.version();
    sized.scale = 2.0f;
    grown.step(1.0 / 60.0, sized);
    GIANT_CHECK(grown.stats().resets == 0 && grown.version() != sized_version);
    GIANT_CHECK(length(tip(grown) - hung) < 2e-3f && stretch(grown) < 1e-3f);
    for (int i = 0; i < 600; ++i) {
        sized.scale *= 0.999f; // down to about 1.1
        grown.step(1.0 / 60.0, sized);
    }
    run(grown, sized, 3.0f);
    GIANT_CHECK(grown.stats().resets == 0 && stretch(grown) < 1e-3f);
    GIANT_CHECK(length(tip(grown) - hung) < 5e-3f);
    // With gravity it hangs down in the world: in its own turned space
    // that is off toward where down went.
    HairSim swung(rest, loose());
    run(swung, tilted, 10.0f);
    const Vec3 down_local = rotate(conjugate(tilted.rotation), Vec3{0.0f, -1.0f, 0.0f});
    GIANT_CHECK(dot(normalize_or_zero(tip(swung) - swung.points()[0]), down_local) > 0.98f);
    GIANT_CHECK(stretch(swung) < 1e-3f); // lengths in its own space, whatever the scale
}

GIANT_TEST("hair: frames faster or slower than its steps neither stretch it nor change how it moves") {
    const std::vector<Vec3> rest = guides(2, {0.0f, -1.0f, 0.0f});
    // The head moves off at 3 m/s and turns, at 240, 144, 60 and 30 frames a second.
    std::vector<float> trails;
    for (double fps : {240.0, 144.0, 60.0, 30.0}) {
        HairSim hair(rest);
        HairFrame frame;
        frame.position = {1.0e6, 0.0, -2.0e6};
        hair.step(1.0 / fps, frame);
        float worst = 0.0f;
        float trail = 0.0f;
        const int frames = static_cast<int>(fps); // one second
        for (int i = 0; i < frames; ++i) {
            frame.position.x += 3.0 / fps;
            frame.rotation = quat_from_axis_angle({0.0f, 1.0f, 0.0f}, 2.0f * static_cast<float>(i + 1) / static_cast<float>(fps));
            const u64 version = hair.version();
            hair.step(1.0 / fps, frame);
            GIANT_CHECK(hair.stats().steps > 0 || hair.version() == version); // no step: nothing changes
            worst = std::max(worst, stretch(hair));
            GIANT_CHECK_NEAR(length(hair.points()[0] - rest[0]), 0.0, 1e-5); // roots stay on
            trail = std::max(trail, length(tip(hair) - rest[k_hair_points - 1]));
        }
        GIANT_CHECK(worst < 0.03f);
        GIANT_CHECK(hair.stats().resets == 0);
        trails.push_back(trail);
    }
    // Much the same swing whatever the frame rate.
    for (float t : trails) GIANT_CHECK(t > 0.02f && std::abs(t - trails[2]) < 0.5f * trails[2]);
    // The same far from the origin as at it.
    HairSim near_origin(rest);
    HairSim far_away(rest);
    HairFrame here;
    HairFrame there;
    there.position = {4.0e6, 2000.0, -4.0e6};
    for (int i = 0; i < 120; ++i) {
        here.position.x += 0.03125; // exact in a double at both places
        there.position.x += 0.03125;
        near_origin.step(1.0 / 60.0, here);
        far_away.step(1.0 / 60.0, there);
    }
    for (usize i = 0; i < near_origin.points().size(); ++i) {
        GIANT_CHECK(length(near_origin.points()[i] - far_away.points()[i]) < 1e-4f);
    }
    GIANT_CHECK(off_rest(near_origin) > 0.01f);
}

GIANT_TEST("hair: capsules keep it out, but never push it off the scalp it grows from") {
    // Hair falling onto a bar lying under its roots.
    const std::vector<Vec3> rest = guides(6, {1.0f, 0.0f, 0.0f});
    HairSim hair(rest, loose());
    const HairCapsule ball{{0.0f, -0.15f, -0.1f}, {0.0f, -0.15f, 0.4f}, 0.08f};
    HairFrame frame;
    frame.capsules = std::span<const HairCapsule>(&ball, 1);
    float deepest = 1.0f;
    bool touched = false;
    for (int i = 0; i < 600; ++i) {
        hair.step(1.0 / 60.0, frame);
        for (const Vec3& p : hair.points()) {
            const Vec3 axis{0.0f, -0.15f, std::clamp(p.z, -0.1f, 0.4f)};
            deepest = std::min(deepest, length(p - axis));
            touched = touched || length(p - axis) < 0.0851f;
        }
    }
    GIANT_CHECK(touched);                            // it does lie on the bar
    GIANT_CHECK(deepest > 0.08f + 0.005f - 1e-4f);   // the radius and the hair's thickness
    GIANT_CHECK(stretch(hair) < 0.05f);
    // Lying on it, it comes to rest: nothing more changes.
    const u64 lying = hair.version();
    const std::vector<Vec3> lay(hair.points().begin(), hair.points().end());
    run(hair, frame, 1.0f);
    float crept = 0.0f;
    for (usize i = 0; i < lay.size(); ++i) crept = std::max(crept, length(hair.points()[i] - lay[i]));
    std::printf("hair: lying on a bar it moved %.7f m in a second, %llu versions\n", static_cast<double>(crept),
                static_cast<unsigned long long>(hair.version() - lying));
    GIANT_CHECK(hair.version() == lying);
    // Without the bar it hangs through where the bar was.
    HairSim free_hair(rest, loose());
    run(free_hair, HairFrame{}, 10.0f);
    GIANT_CHECK(std::abs(tip(free_hair).x) < 0.05f);
    GIANT_CHECK(tip(hair).x > tip(free_hair).x + 0.02f || tip(hair).y > tip(free_hair).y + 0.02f);

    // Roots inside a capsule (a head's): nothing is pushed off its rest.
    HairSettings weightless;
    weightless.gravity = 0.0f;
    HairSim rooted(rest, weightless);
    const HairCapsule head{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.3f}, 0.2f};
    HairFrame inside;
    inside.capsules = std::span<const HairCapsule>(&head, 1);
    run(rooted, inside, 2.0f);
    GIANT_CHECK(off_rest(rooted) < 1e-5f);
    // A bar brought up against hair hanging at rest pushes it aside (the
    // second of two capsules, the first far off): the hair was not inside
    // it when placed, so it is kept out however the bar comes.
    const std::vector<Vec3> down = guides(3, {0.0f, -1.0f, 0.0f});
    HairSim pushed(down);
    HairCapsule two[2] = {{{9.0f, 9.0f, 9.0f}, {9.0f, 9.5f, 9.0f}, 0.1f}, {{0.5f, -0.15f, -1.0f}, {0.5f, -0.15f, 1.0f}, 0.06f}};
    HairFrame sweep;
    sweep.capsules = std::span<const HairCapsule>(two, 2);
    run(pushed, sweep, 0.5f);
    float nearest = 1.0f;
    for (int i = 0; i < 240; ++i) {
        const float x = std::max(0.5f - 0.005f * static_cast<float>(i), -0.02f);
        two[1].a.x = x;
        two[1].b.x = x;
        pushed.step(1.0 / 60.0, sweep);
        for (const Vec3& p : pushed.points().subspan(1)) {
            if (&p == &pushed.points()[k_hair_points] || &p == &pushed.points()[2 * k_hair_points]) continue; // roots
            nearest = std::min(nearest, length(Vec3{p.x - x, p.y + 0.15f, 0.0f}));
        }
    }
    GIANT_CHECK(nearest > 0.06f); // never into the bar itself (its stand-off is the room for keeping lengths)
    GIANT_CHECK(tip(pushed).x < -0.02f || tip(pushed).y > -0.29f); // pushed aside
    GIANT_CHECK(pushed.stats().resets == 0);
    // A point right on a capsule's axis is put out the way it rests; a
    // capsule that is a ball (both ends one place) works as one.
    HairSim through(guides(1, {1.0f, 0.0f, 0.0f}), loose());
    const HairCapsule centred{{0.15f, -0.25f, 0.0f}, {0.15f, -0.25f, 0.0f}, 0.05f};
    HairFrame balled;
    balled.capsules = std::span<const HairCapsule>(&centred, 1);
    run(through, balled, 5.0f);
    for (const Vec3& p : through.points()) {
        GIANT_CHECK(std::isfinite(p.x + p.y + p.z));
        GIANT_CHECK(length(p - centred.a) > 0.055f - 1e-4f);
    }
    // A capsule with bad numbers is not felt.
    const HairCapsule bad{{std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.5f};
    HairFrame broken;
    broken.capsules = std::span<const HairCapsule>(&bad, 1);
    HairSim sound(rest);
    run(sound, broken, 1.0f);
    GIANT_CHECK(sound.stats().resets == 0 && stretch(sound) < 1e-4f);
}

GIANT_TEST("hair: jumps and bad numbers carry it whole, work is bounded, and it runs the same twice") {
    const std::vector<Vec3> rest = guides(3, {0.0f, -1.0f, 0.0f});
    HairSim hair(rest);
    HairFrame frame;
    frame.wind = {10.0f, 0.0f, 4.0f};
    run(hair, frame, 1.0f);
    GIANT_CHECK(off_rest(hair) > 0.01f);
    // A fast move is a move: 4 m in a frame swings the hair hard, and so
    // does 8 m in a long frame (80 m/s).
    frame.position = {4.0, 0.0, 0.0};
    hair.step(1.0 / 60.0, frame);
    GIANT_CHECK(hair.stats().resets == 0 && off_rest(hair) > 0.01f);
    frame.position = {12.0, 0.0, 0.0};
    hair.step(0.1, frame);
    GIANT_CHECK(hair.stats().resets == 0 && off_rest(hair) > 0.01f);
    // 8 m in a sixtieth of a second is a teleport: at rest in the new place.
    frame.position = {20.0, 0.0, 0.0};
    hair.step(1.0 / 60.0, frame);
    GIANT_CHECK(hair.stats().resets == 1 && off_rest(hair) == 0.0f);
    // A hair's breadth of a size change is not a new size.
    run(hair, frame, 0.5f);
    frame.scale = 1.005f;
    hair.step(1.0 / 60.0, frame);
    frame.scale = 1.0f;
    hair.step(1.0 / 60.0, frame);
    GIANT_CHECK(hair.stats().resets == 1 && off_rest(hair) > 0.01f);
    // Time and places that are no numbers change nothing.
    run(hair, frame, 0.5f);
    const std::vector<Vec3> before(hair.points().begin(), hair.points().end());
    const u64 version = hair.version();
    hair.step(std::numeric_limits<double>::quiet_NaN(), frame);
    hair.step(-1.0, frame);
    HairFrame nowhere = frame;
    nowhere.position.x = std::numeric_limits<double>::infinity();
    hair.step(1.0 / 60.0, nowhere);
    HairFrame unturned = frame;
    unturned.rotation = {0.0f, 0.0f, 0.0f, 0.0f};
    hair.step(1.0 / 60.0, unturned);
    GIANT_CHECK(hair.version() == version);
    for (usize i = 0; i < before.size(); ++i) GIANT_CHECK(length(hair.points()[i] - before[i]) == 0.0f);
    // A long frame takes max_steps steps and drops the rest; a short one none.
    hair.step(10.0, frame);
    GIANT_CHECK(hair.stats().steps == 4);
    hair.step(1.0 / 60.0, frame);
    GIANT_CHECK(hair.stats().steps == 1);
    hair.step(0.001, frame);
    GIANT_CHECK(hair.stats().steps == 0);
    // Short frames add up to a step.
    u32 stepped = 0;
    for (int i = 0; i < 40; ++i) {
        hair.step(1.0 / 240.0, frame);
        stepped += hair.stats().steps;
    }
    GIANT_CHECK(stepped == 10);
    // A wind that is no number is no wind; a storm beyond any storm stays finite.
    HairFrame wild = frame;
    wild.wind = {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f};
    run(hair, wild, 0.5f);
    wild.wind = {1e6f, 0.0f, 0.0f};
    run(hair, wild, 0.5f);
    for (const Vec3& p : hair.points()) GIANT_CHECK(std::isfinite(p.x + p.y + p.z));
    GIANT_CHECK(stretch(hair) < 0.05f);
    // reset(): the style again, and told.
    const u64 unreset = hair.version();
    hair.reset();
    GIANT_CHECK(off_rest(hair) == 0.0f && hair.version() != unreset);
    hair.reset();
    GIANT_CHECK(hair.version() == unreset + 1); // already at rest: nothing new
    // The same twice.
    HairSim a(rest);
    HairSim b(rest);
    HairFrame moving;
    moving.wind = {12.0f, 0.0f, -5.0f};
    for (int i = 0; i < 200; ++i) {
        moving.position.x += 0.01 * std::sin(0.1 * i);
        moving.rotation = quat_from_axis_angle({0.0f, 1.0f, 0.0f}, 0.01f * static_cast<float>(i));
        a.step(1.0 / 60.0, moving);
        b.step(1.0 / 60.0, moving);
    }
    for (usize i = 0; i < a.points().size(); ++i) GIANT_CHECK(length(a.points()[i] - b.points()[i]) == 0.0f);
    GIANT_CHECK(a.version() == b.version());
}

GIANT_TEST("hair: settings are clamped, and odd guides are taken as they come") {
    HairSettings wild;
    wild.root_stiffness = 7.0f;
    wild.tip_stiffness = -1.0f;
    wild.damping = std::numeric_limits<float>::quiet_NaN();
    wild.gravity = -5.0f;
    wild.wind_response = std::numeric_limits<float>::infinity();
    wild.thickness = 3.0f;
    wild.friction = 10.0f;
    wild.step_seconds = 0.0f;
    wild.max_steps = 1000;
    wild.root_stiffness = 700.0f;
    const HairSettings c = clean(wild);
    GIANT_CHECK(c.root_stiffness == 60.0f && c.tip_stiffness == 0.0f && c.damping == HairSettings{}.damping);
    GIANT_CHECK(c.gravity == 0.0f && c.wind_response == HairSettings{}.wind_response && c.thickness == 0.1f);
    GIANT_CHECK(c.friction == 1.0f && c.step_seconds == 1.0f / 240.0f && c.max_steps == 16);
    // No guides: nothing to do. A count that is not whole strands: the rest dropped.
    HairSim none(std::span<const Vec3>{});
    none.step(1.0 / 60.0, HairFrame{});
    GIANT_CHECK(none.guide_count() == 0 && none.points().empty());
    std::vector<Vec3> ragged = guides(2, {0.0f, -1.0f, 0.0f});
    ragged.resize(ragged.size() - 3);
    HairSim one(ragged);
    GIANT_CHECK(one.guide_count() == 1 && one.points().size() == k_hair_points);
    // A guide of no length (all its points in one place) and points that are
    // no numbers: carried, never spreading bad numbers.
    std::vector<Vec3> odd = guides(2, {0.0f, -1.0f, 0.0f});
    for (u32 k = 0; k < k_hair_points; ++k) odd[k] = {0.0f, 0.0f, 0.0f};
    odd[k_hair_points + 5].y = std::numeric_limits<float>::quiet_NaN();
    HairSim strange(odd);
    HairFrame frame;
    frame.wind = {8.0f, 0.0f, 0.0f};
    run(strange, frame, 1.0f);
    for (const Vec3& p : strange.points()) GIANT_CHECK(std::isfinite(p.x + p.y + p.z));
    GIANT_CHECK(strange.stats().resets == 0);
    // Rest points beyond all reach are brought in; a scale that is no
    // number is 1, one of nothing or less the smallest.
    std::vector<Vec3> vast = guides(1, {0.0f, -1.0f, 0.0f});
    vast[7].x = 3.0e38f;
    HairSim bounded(vast);
    HairFrame big;
    big.scale = 1000.0f;
    run(bounded, big, 0.5f);
    for (const Vec3& p : bounded.points()) GIANT_CHECK(std::isfinite(p.x + p.y + p.z));
    HairSim scaled(guides(1, {0.0f, -1.0f, 0.0f}));
    HairFrame unsized;
    unsized.scale = std::numeric_limits<float>::quiet_NaN();
    run(scaled, unsized, 0.5f);
    unsized.scale = 1.0f; // the same as no number: no jump
    scaled.step(1.0 / 60.0, unsized);
    GIANT_CHECK(scaled.stats().resets == 0);
    unsized.scale = -4.0f;
    scaled.step(1.0 / 60.0, unsized);
    GIANT_CHECK(scaled.stats().resets == 0);
    for (const Vec3& p : scaled.points()) GIANT_CHECK(std::isfinite(p.x + p.y + p.z));
}

GIANT_TEST("hair: uneven frames show it the head's true speed, and the head's own capsules ride along") {
    const std::vector<Vec3> rest = guides(2, {0.0f, -1.0f, 0.0f});
    // A head at a steady 3 m/s: once the hair has settled into its trail,
    // it holds still however the frames fall against the steps.
    for (int timing = 0; timing < 4; ++timing) {
        HairSim hair(rest);
        HairFrame frame;
        hair.step(0.0, frame);
        float low = 1.0f;
        float high = -1.0f;
        u32 seed = 12345;
        double clock = 0.0;
        while (clock < 14.0) {
            double seconds = 1.0 / 60.0;
            if (timing == 1) seconds = 1.0 / 72.0;
            if (timing == 2) seconds = 1.0 / 59.94;
            if (timing == 3) {
                seed = seed * 1664525u + 1013904223u; // 60 a second, half as long to half as long again
                seconds = (0.5 + static_cast<double>(seed >> 8) / 16777216.0) / 60.0;
            }
            clock += seconds;
            frame.position.x += 3.0 * seconds;
            hair.step(seconds, frame);
            if (clock > 10.0) {
                low = std::min(low, tip(hair).x);
                high = std::max(high, tip(hair).x);
            }
        }
        GIANT_CHECK(high - low < 2e-3f);
        GIANT_CHECK(stretch(hair) < 1e-3f && hair.stats().resets == 0);
    }
    // A head with its capsule over the roots, running at 15 frames a second
    // (four steps a frame): the capsule is where the head was at each step,
    // so it does not cut through the hair behind it.
    const HairCapsule skull{{0.0f, 0.05f, -0.1f}, {0.0f, 0.05f, 0.2f}, 0.1f};
    // The same for a head that turns (1.5 rad/s about a point 0.2 m behind
    // the roots) as for one that runs.
    for (int turning = 0; turning < 2; ++turning) {
        float off[2] = {0.0f, 0.0f};
        for (int slow = 0; slow < 2; ++slow) {
            HairSim hair(rest);
            HairFrame frame;
            HairCapsule carried = skull;
            frame.capsules = std::span<const HairCapsule>(&carried, 1);
            const int every = slow == 1 ? 4 : 1; // sixtieths of a second a frame
            float worst = 0.0f;
            for (int i = 0; i <= 120; i += every) {
                const float clock = static_cast<float>(i) / 60.0f;
                if (turning == 1) {
                    frame.rotation = quat_from_axis_angle({0.0f, 1.0f, 0.0f}, 1.5f * clock);
                    const Vec3 arm = rotate(frame.rotation, Vec3{0.2f, 0.0f, 0.0f});
                    frame.position = {static_cast<double>(arm.x), 0.0, static_cast<double>(arm.z)};
                    carried = {rotate(frame.rotation, skull.a), rotate(frame.rotation, skull.b), skull.radius};
                } else {
                    frame.position.x = 3.0 * static_cast<double>(clock);
                }
                hair.step(i == 0 ? 0.0 : static_cast<double>(every) / 60.0, frame); // the first places it
                worst = std::max(worst, stretch(hair));
                off[slow] = std::max(off[slow], off_rest(hair));
            }
            GIANT_CHECK(hair.stats().steps == (slow == 1 ? 4u : 1u) && hair.stats().resets == 0);
            GIANT_CHECK(worst < 0.03f);
        }
        std::printf("hair: a head %s moved it %.4f m off rest at 60 frames a second, %.4f m at 15\n",
                    turning == 1 ? "turning" : "running", static_cast<double>(off[0]), static_cast<double>(off[1]));
        GIANT_CHECK(off[0] > 0.01f && std::abs(off[1] - off[0]) < 0.08f * off[0]);
    }
}

GIANT_TEST("hair: capsules are felt far from the groom's origin, turned and sized, up to sixteen of them") {
    // Hair rooted 1.5 m from its groom's origin on a spinning, doubled
    // head, with a bar fixed to the head just under the roots: in the
    // groom's own space the bar never moves, and the hair never enters it.
    std::vector<Vec3> rest;
    for (u32 g = 0; g < 3; ++g) {
        for (u32 k = 0; k < k_hair_points; ++k) {
            rest.push_back({1.5f + k_piece * static_cast<float>(k), 0.0f, 0.05f * static_cast<float>(g)});
        }
    }
    const Vec3 bar_a{1.6f, -0.1f, -0.2f}; // groom space
    const Vec3 bar_b{1.6f, -0.1f, 0.3f};
    const float bar_radius = 0.03f; // groom space (0.06 m in the world)
    HairSim hair(rest, loose());
    HairFrame frame;
    frame.scale = 2.0f;
    frame.position = {300.0, 20.0, -700.0};
    float nearest = 1.0f;
    bool lay = false;
    for (int i = 0; i < 600; ++i) {
        frame.rotation = quat_from_axis_angle({0.0f, 1.0f, 0.0f}, 0.01f * static_cast<float>(i)); // 0.6 rad/s
        const HairCapsule bar{rotate(frame.rotation, bar_a) * 2.0f, rotate(frame.rotation, bar_b) * 2.0f, bar_radius * 2.0f};
        frame.capsules = std::span<const HairCapsule>(&bar, 1);
        hair.step(1.0 / 60.0, frame);
        for (const Vec3& p : hair.points()) {
            const Vec3 axis{bar_a.x, bar_a.y, std::clamp(p.z, bar_a.z, bar_b.z)};
            nearest = std::min(nearest, length(p - axis));
            lay = lay || length(p - axis) < bar_radius + 0.0025f + 5e-4f;
        }
    }
    GIANT_CHECK(lay);
    GIANT_CHECK(nearest > bar_radius + 0.0025f - 1e-4f); // the thickness is 5 mm in the world: half that here
    GIANT_CHECK(hair.stats().resets == 0);

    // The sixteenth capsule is felt, a seventeenth is not.
    const std::vector<Vec3> sideways = guides(1, {1.0f, 0.0f, 0.0f});
    const HairCapsule under{{0.0f, -0.15f, -0.1f}, {0.0f, -0.15f, 0.4f}, 0.08f};
    for (u32 slot : {15u, 16u}) {
        std::vector<HairCapsule> many(17, HairCapsule{{50.0f, 0.0f, 0.0f}, {51.0f, 0.0f, 0.0f}, 0.1f});
        many[slot] = under;
        HairSim draped(sideways, loose());
        HairFrame crowded;
        crowded.capsules = many;
        run(draped, crowded, 5.0f);
        GIANT_CHECK((std::abs(tip(draped).x) > 0.05f) == (slot == 15));
    }
    // A capsule that comes later (the list grows) is felt from then on.
    HairSim later(guides(1, {0.0f, -1.0f, 0.0f}));
    HairFrame alone;
    run(later, alone, 0.5f);
    HairCapsule coming{{0.5f, -0.15f, -1.0f}, {0.5f, -0.15f, 1.0f}, 0.06f};
    alone.capsules = std::span<const HairCapsule>(&coming, 1);
    float closest = 1.0f;
    float pressed = 0.0f;
    for (int i = 0; i < 180; ++i) {
        const float x = std::max(0.5f - 0.005f * static_cast<float>(i), -0.02f); // 0.3 m/s
        coming.a.x = x;
        coming.b.x = x;
        later.step(1.0 / 60.0, alone);
        for (const Vec3& p : later.points().subspan(1)) closest = std::min(closest, length(Vec3{p.x - x, p.y + 0.15f, 0.0f}));
        pressed = std::max(pressed, stretch(later));
    }
    std::printf("hair: pressed by a bar at 0.3 m/s it stretched %.4f and came within %.4f of its axis\n",
                static_cast<double>(pressed), static_cast<double>(closest));
    GIANT_CHECK(closest > 0.06f);
    GIANT_CHECK(pressed < 0.1f);
}

GIANT_TEST("hair: friction holds hair lying on a slope; without it the hair slides off") {
    // A wide roller lying along z; the strand is laid along it from its
    // root, 20 degrees round from the top, where the surface slopes at 20
    // degrees down toward +x.
    const auto roller = [](float degrees) {
        const float round = degrees_to_radians(degrees);
        const Vec3 out{std::sin(round), std::cos(round), 0.0f};
        return HairCapsule{out * -1.005f + Vec3{0.0f, 0.0f, -5.0f}, out * -1.005f + Vec3{0.0f, 0.0f, 5.0f}, 1.0f};
    };
    const std::vector<Vec3> rest = guides(1, {0.0f, 0.0f, 1.0f});
    const HairCapsule gentle = roller(20.0f);
    HairFrame frame;
    frame.capsules = std::span<const HairCapsule>(&gentle, 1);
    HairSettings gripping = loose();
    gripping.friction = 1.0f;
    HairSim held(rest, gripping);
    run(held, frame, 4.0f);
    GIANT_CHECK(off_rest(held) < 0.01f);
    HairSettings slippery = loose();
    slippery.friction = 0.0f;
    HairSim slid(rest, slippery);
    run(slid, frame, 4.0f);
    GIANT_CHECK(tip(slid).x > 0.2f && tip(slid).y < -0.05f); // swung round to hang down the slope
    for (const Vec3& p : slid.points()) {
        GIANT_CHECK(length(p - nearest_point(gentle, p)) > 1.0f); // on the roller, not in it
    }
    // The steeper the slope the less it holds: at 60 degrees even full friction lets go.
    const HairCapsule steep = roller(60.0f);
    HairFrame leaning;
    leaning.capsules = std::span<const HairCapsule>(&steep, 1);
    HairSim fell(rest, gripping);
    run(fell, leaning, 4.0f);
    GIANT_CHECK(tip(fell).y < -0.15f);
}

GIANT_TEST("hair: a turn given either way round is one turn, a reset far off is no jump, and it falls alike at any step") {
    // q and -q are the same turn: frames that give one, then the other.
    const std::vector<Vec3> rest = guides(2, {1.0f, 0.0f, 0.0f});
    HairSim plain(rest);
    HairSim flipped(rest);
    HairFrame one;
    HairFrame other;
    for (int i = 0; i < 240; ++i) {
        const Quat q = quat_from_axis_angle({0.0f, 0.0f, 1.0f}, 0.7f + 0.01f * static_cast<float>(i));
        one.rotation = q;
        other.rotation = i % 2 == 1 ? Quat{-q.x, -q.y, -q.z, -q.w} : q;
        plain.step(1.0 / 45.0, one); // frames that fall between steps: the turn between them is used
        flipped.step(1.0 / 45.0, other);
    }
    float apart = 0.0f;
    for (usize i = 0; i < plain.points().size(); ++i) {
        apart = std::max(apart, length(plain.points()[i] - flipped.points()[i]));
    }
    GIANT_CHECK(apart < 1e-5f && off_rest(plain) > 1e-3f);

    // After reset() the next step places it wherever the head is: no jump counted.
    one.position = {4.0e6, 0.0, -2.0e6};
    plain.reset();
    GIANT_CHECK(off_rest(plain) == 0.0f);
    plain.step(1.0 / 60.0, one);
    GIANT_CHECK(plain.stats().resets == 0 && off_rest(plain) == 0.0f);
    plain.step(1.0 / 60.0, one);
    GIANT_CHECK(plain.stats().resets == 0 && stretch(plain) < 1e-3f);

    // A strand let go from level swings down through the bottom at the
    // same time whatever the step.
    float crossing[2] = {0.0f, 0.0f};
    for (int fine = 0; fine < 2; ++fine) {
        HairSettings free_fall = loose();
        free_fall.damping = 0.0f;
        free_fall.wind_response = 0.0f;
        free_fall.step_seconds = fine == 1 ? 1.0f / 240.0f : 1.0f / 30.0f;
        free_fall.max_steps = 16;
        HairSim hair(guides(1, {1.0f, 0.0f, 0.0f}), free_fall);
        hair.step(0.0, HairFrame{});
        float before = tip(hair).x;
        for (int i = 1; i <= 240 && crossing[fine] == 0.0f; ++i) {
            hair.step(1.0 / 240.0, HairFrame{});
            const float now = tip(hair).x;
            if (now <= 0.0f && hair.stats().steps > 0) {
                const float step = free_fall.step_seconds;
                const float whole = std::floor(static_cast<float>(i) / 240.0f / step + 0.5f) * step; // the step's own time
                crossing[fine] = whole - step * (0.0f - now) / std::max(before - now, 1e-9f);
            }
            if (hair.stats().steps > 0) before = now;
        }
    }
    std::printf("hair: a level strand swings through the bottom after %.4f s (1/30 s steps), %.4f s (1/240 s)\n",
                static_cast<double>(crossing[0]), static_cast<double>(crossing[1]));
    GIANT_CHECK(crossing[1] > 0.1f && std::abs(crossing[0] - crossing[1]) < 0.1f * crossing[1]);
}

GIANT_TEST("hair: uneven pieces come to rest, and a fast bar or a far one is still kept out") {
    // A strand whose last piece is a tenth of the others (a resampled
    // curl's end): shaken by the head, it comes to rest all the same.
    std::vector<Vec3> uneven = guides(1, {0.0f, -1.0f, 0.0f});
    uneven.back() = uneven[k_hair_points - 2] + Vec3{0.0f, -0.002f, 0.0f};
    for (float step : {1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 240.0f}) {
        HairSettings s;
        s.step_seconds = step;
        s.max_steps = 16;
        HairSim hair(uneven, s);
        HairFrame frame;
        hair.step(0.0, frame);
        for (int i = 0; i < 30; ++i) {
            frame.position.x += 3.0 / 60.0;
            hair.step(1.0 / 60.0, frame);
        }
        run(hair, frame, 12.0f);
        const u64 settled = hair.version();
        const std::vector<Vec3> lay(hair.points().begin(), hair.points().end());
        run(hair, frame, 2.0f);
        float crept = 0.0f;
        for (usize i = 0; i < lay.size(); ++i) crept = std::max(crept, length(hair.points()[i] - lay[i]));
        std::printf("hair: uneven pieces at 1/%.0f s steps moved %.7f m in two seconds at rest\n", 1.0 / static_cast<double>(step),
                    static_cast<double>(crept));
        GIANT_CHECK(hair.version() == settled);
        GIANT_CHECK(stretch(hair) < 1e-3f && off_rest(hair) < 1e-3f);
    }

    // Strands that leave a head's capsule on a slant (not straight out
    // from its axis) are not pushed off their rest by it, still or turning.
    std::vector<Vec3> slanted;
    for (u32 g = 0; g < 3; ++g) {
        const Vec3 root{0.06f, 0.0f, 0.05f * static_cast<float>(g)};
        const Vec3 way = normalize_or_zero(Vec3{0.3f * static_cast<float>(g) - 0.3f, -1.0f, 0.4f});
        for (u32 k = 0; k < k_hair_points; ++k) slanted.push_back(root + way * (k_piece * static_cast<float>(k)));
    }
    HairSettings weightless;
    weightless.gravity = 0.0f;
    weightless.damping = 60.0f;
    weightless.root_stiffness = 60.0f;
    weightless.tip_stiffness = 60.0f;
    const HairCapsule skull{{0.0f, 0.05f, -0.1f}, {0.0f, 0.05f, 0.2f}, 0.1f};
    HairSim grows(slanted, weightless);
    HairSim bare(slanted, weightless);
    HairFrame head;
    float off = 0.0f;
    for (int i = 0; i < 240; ++i) {
        head.rotation = quat_from_axis_angle({0.0f, 1.0f, 0.0f}, 0.005f * static_cast<float>(i)); // 0.3 rad/s
        head.capsules = {};
        bare.step(1.0 / 60.0, head);
        const HairCapsule turned{rotate(head.rotation, skull.a), rotate(head.rotation, skull.b), skull.radius};
        head.capsules = std::span<const HairCapsule>(&turned, 1);
        grows.step(1.0 / 60.0, head);
        for (usize p = 0; p < slanted.size(); ++p) off = std::max(off, length(grows.points()[p] - bare.points()[p]));
    }
    std::printf("hair: a head's capsule moved slanted hair %.7f m off where it lies without\n", static_cast<double>(off));
    GIANT_CHECK(off < 1e-5f);

    // A bar by the tip of long hair, far from the root, is felt; and one
    // swept through at 3 m/s (a step's move larger than itself) never
    // ends a step with hair inside it.
    for (int which = 0; which < 12; ++which) {
        const float speed = which % 2 == 1 ? 3.0f : 0.3f;
        const float height = which % 4 < 2 ? -0.27f : -0.15f;
        const float from = 0.42f + 0.017f * static_cast<float>(which / 4); // how the steps fall against the hair
        HairSim hair(guides(2, {0.0f, -1.0f, 0.0f}));
        HairCapsule bar{{from, height, -1.0f}, {from, height, 1.0f}, 0.03f};
        HairFrame frame;
        frame.capsules = std::span<const HairCapsule>(&bar, 1);
        run(hair, frame, 0.5f);
        float closest = 1.0f;
        float swung = 0.0f;
        float stretched = 0.0f;
        for (int i = 0; i < 120; ++i) {
            const float x = std::max(from - speed / 60.0f * static_cast<float>(i), -0.4f);
            bar.a.x = x;
            bar.b.x = x;
            hair.step(1.0 / 60.0, frame);
            for (usize p = 0; p < hair.points().size(); ++p) {
                if (p % k_hair_points == 0) continue;
                const Vec3& at = hair.points()[p];
                closest = std::min(closest, length(Vec3{at.x - x, at.y - height, 0.0f}));
            }
            swung = std::max(swung, off_rest(hair));
            stretched = std::max(stretched, stretch(hair));
        }
        GIANT_CHECK(closest > 0.03f - 1e-4f);
        GIANT_CHECK(swung > 0.01f && stretched < 0.1f);
        GIANT_CHECK(hair.stats().resets == 0);
    }
}
