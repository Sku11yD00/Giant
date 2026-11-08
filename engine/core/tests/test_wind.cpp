// Wind: its clocks, gusts, and how swaying things move in it.
#include "giant/core/wind.h"

#include "giant_test.h"

#include <cmath>

using namespace giant;

namespace {

WindState blowing(float speed, float gustiness, double seconds = 37.25) {
    WindClock clock;
    Wind g;
    g.velocity = {speed, 0.0f, 0.0f};
    g.gustiness = gustiness;
    for (double t = 0.0; t < seconds; t += 1.0 / 60.0) clock.advance(g, 1.0 / 60.0);
    return clock.state();
}

float most_offset(const WindState& w, float height, float sway, float flutter, const DVec3& base) {
    float most = 0.0f;
    for (int i = 0; i <= 20; ++i) {
        const float y = height * static_cast<float>(i) / 20.0f;
        for (const DVec3 side : {DVec3{0, 0, 0}, DVec3{2, 0, 0}, DVec3{0, 0, -1.5}}) {
            const DVec3 rest{base.x + side.x, base.y + y, base.z + side.z};
            most = std::max(most, length(sway_offset(w, base, height, rest, sway, flutter)));
        }
    }
    return most;
}

} // namespace

GIANT_TEST("wind: the clocks run with the wind and wrap, bad steps keep them") {
    WindClock clock;
    Wind g{{10.0f, 0.0f, 0.0f}, 0.5f};
    clock.advance(g, 1.0);
    const WindState one = clock.state();
    GIANT_CHECK(std::abs(one.gust_x - 8.0f) < 1e-4f && one.gust_z == 0.0f);
    GIANT_CHECK(std::abs(one.sway_clock - 0.6f) < 1e-5f && std::abs(one.flutter_clock - 2.5f) < 1e-5f);
    clock.advance(g, NAN);
    clock.advance(g, -1.0);
    clock.advance(g, 3600.0);
    GIANT_CHECK(clock.state() == one);
    for (int i = 0; i < 1000; ++i) clock.advance(g, 1.0);
    const WindState later = clock.state();
    GIANT_CHECK(later.gust_x >= 0.0f && later.gust_x < 64.0f);
    GIANT_CHECK(later.sway_clock >= 0.0f && later.sway_clock < 256.0f);
    GIANT_CHECK(later.flutter_clock >= 0.0f && later.flutter_clock < 256.0f);
    clock.reset();
    GIANT_CHECK(clock.state() == WindState{});
}

GIANT_TEST("wind: gusts come and go over the ground, the same every 64 m") {
    const WindState w = blowing(8.0f, 0.6f);
    float lo = 1.0f, hi = 0.0f;
    for (int i = 0; i < 64; ++i) {
        for (int j = 0; j < 64; ++j) {
            const float g = gust_at(w, i, j);
            lo = std::min(lo, g);
            hi = std::max(hi, g);
            GIANT_CHECK(std::abs(g - gust_at(w, i + 64.0 * 3, j - 64.0 * 70)) < 1e-3f);
        }
    }
    GIANT_CHECK(lo >= 0.0f && hi <= 1.0f && hi - lo > 0.5f);
    const Vec3 a = wind_at(w, {3, 0, 5});
    GIANT_CHECK(a.x > 8.0f * 0.6f && a.x < 8.0f * 1.4f && a.y == 0.0f);
}

GIANT_TEST("wind: swaying things bend from the base, harder in a storm, within their bounds") {
    const DVec3 base{1234.5, 80.0, -987.25};
    const WindState breeze = blowing(3.0f, 0.3f);
    const WindState storm = blowing(18.0f, 0.8f);
    // Nothing moves at the base, nor for still things, nor without wind.
    GIANT_CHECK(length(sway_offset(storm, base, 10.0f, base, 1.0f, 1.0f)) < 1e-6f);
    GIANT_CHECK(sway_offset(storm, base, 10.0f, {base.x, base.y + 9, base.z}, 0.0f, 0.0f) == Vec3{});
    GIANT_CHECK(sway_offset(WindState{}, base, 10.0f, {base.x, base.y + 9, base.z}, 1.0f, 1.0f) == Vec3{});
    GIANT_CHECK(sway_offset(storm, base, 0.0f, {base.x, base.y + 9, base.z}, 1.0f, 1.0f) == Vec3{});
    // The top moves further than the middle, mostly downwind (on average).
    float top_along = 0.0f, mid = 0.0f, top = 0.0f;
    WindClock clock;
    for (int i = 0; i < 600; ++i) {
        clock.advance({{18.0f, 0.0f, 0.0f}, 0.8f}, 1.0 / 30.0);
        const WindState s = clock.state();
        const Vec3 t = sway_offset(s, base, 10.0f, {base.x, base.y + 10, base.z}, 1.0f, 0.0f);
        top_along += t.x;
        top += length(t);
        mid += length(sway_offset(s, base, 10.0f, {base.x, base.y + 5, base.z}, 1.0f, 0.0f));
        GIANT_CHECK(t.y <= 0.0f); // it bends, it never stretches
    }
    GIANT_CHECK(top > 2.0f * mid && top_along > 0.0f);
    GIANT_CHECK(most_offset(storm, 10.0f, 1.0f, 1.0f, base) > 3.0f * most_offset(breeze, 10.0f, 1.0f, 1.0f, base));
    // Bounded by max_sway_offset, every point and every moment.
    for (const float height : {0.4f, 3.0f, 12.0f, 30.0f}) {
        for (const float sway : {0.5f, 1.0f, 4.0f}) {
            for (const float speed : {25.0f, 100.0f}) { // a gale, and the most the weather allows
            WindClock c;
            for (int i = 0; i < 120; ++i) {
                c.advance({{speed, 0.0f, -10.0f}, 1.0f}, 0.37);
                const WindState s = c.state();
                GIANT_CHECK(most_offset(s, height, sway, 2.0f, base) <= max_sway_offset(s, height, sway, 2.0f) + 1e-4f);
            }
            }
        }
    }
    GIANT_CHECK(max_sway_offset(storm, 10.0f, 1.0f, 0.0f) <= 10.0f * k_max_sway_lean * 1.2f);
}

GIANT_TEST("wind: a place sways the same whatever the origin, and on as the clocks wrap") {
    const WindState w = blowing(12.0f, 0.7f);
    const DVec3 base{17.25, 3.0, -41.5};
    const DVec3 rest{18.0, 9.5, -40.0};
    const DVec3 far{64.0 * 400, 64.0 * 2, -64.0 * 900};
    const Vec3 a = sway_offset(w, base, 8.0f, rest, 1.0f, 1.5f);
    const Vec3 b = sway_offset(w, base + far, 8.0f, rest + far, 1.0f, 1.5f);
    GIANT_CHECK(length(a - b) < 2e-3f && length(a) > 0.01f);
    // Every frequency turns a whole number of times in the clocks' period.
    WindState before = w;
    WindState after = w;
    before.sway_clock = 255.9999f;
    before.flutter_clock = 255.9999f;
    after.sway_clock = 0.0f;
    after.flutter_clock = 0.0f;
    for (const float height : {0.5f, 2.0f, 7.0f, 25.0f}) {
        const DVec3 top{base.x + 1.0, base.y + height, base.z};
        const Vec3 x = sway_offset(before, base, height, top, 2.0f, 2.0f);
        const Vec3 y = sway_offset(after, base, height, top, 2.0f, 2.0f);
        GIANT_CHECK(length(x - y) < 0.01f * (1.0f + height * 0.05f));
    }
}
