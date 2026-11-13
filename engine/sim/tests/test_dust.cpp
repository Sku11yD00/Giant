// Dust: puffs grow, rise, drift and thin away; work stays bounded.
#include "giant/sim/dust.h"

#include "giant_test.h"

#include <cmath>
#include <cstdio>

using namespace giant;
using namespace giant::sim;

GIANT_TEST("dust: a puff grows, rises, drifts with the wind, thins and goes at the end of its life") {
    Dust dust;
    dust.emit({100.0, 2.0, -50.0}, 4.0f, 0.5f);
    GIANT_CHECK(dust.puffs().size() == 1);
    const DustPuff start = dust.puffs()[0];
    GIANT_CHECK(start.life >= 8.0f && start.life <= 30.0f);
    for (int i = 0; i < 60; ++i) dust.step(0.1f, {5.0f, 0.0f, 0.0f}); // 6 s
    GIANT_CHECK(dust.puffs().size() == 1);
    const DustPuff& p = dust.puffs()[0];
    GIANT_CHECK(p.radius > start.radius * 1.5f);
    GIANT_CHECK(p.density < start.density);
    GIANT_CHECK(p.position.y > start.position.y + 1.0);           // rose
    GIANT_CHECK(p.position.x > start.position.x + 10.0);          // drifted downwind (5 m/s x 0.6 x 6 s)
    GIANT_CHECK(std::abs(p.position.z - start.position.z) < 1e-6); // and only that way
    for (int i = 0; i < 400; ++i) dust.step(0.1f, {});
    GIANT_CHECK(dust.puffs().empty());
    GIANT_CHECK(dust.stats().emitted == 1);
}

GIANT_TEST("dust: emits inside a young puff swell it; bad numbers are left out") {
    Dust dust;
    dust.emit({0.0, 0.0, 0.0}, 10.0f, 0.3f);
    for (int i = 0; i < 20; ++i) dust.emit({2.0 * i / 20.0, 0.0, 1.0}, 3.0f, 0.3f);
    GIANT_CHECK(dust.puffs().size() == 1);
    GIANT_CHECK(dust.stats().merged == 20);
    GIANT_CHECK(dust.puffs()[0].radius >= 10.0f);
    // Outside it: a puff of its own; once old, it takes no more.
    dust.emit({50.0, 0.0, 0.0}, 3.0f, 0.3f);
    GIANT_CHECK(dust.puffs().size() == 2);
    for (int i = 0; i < 20; ++i) dust.step(0.1f, {});
    dust.emit({0.0, 2.0, 0.0}, 3.0f, 0.3f);
    GIANT_CHECK(dust.puffs().size() == 3);
    const usize before = dust.puffs().size();
    dust.emit({NAN, 0.0, 0.0}, 3.0f, 0.3f);
    dust.emit({0.0, 0.0, 0.0}, -1.0f, 0.3f);
    dust.emit({0.0, 0.0, 0.0}, 1.0f, INFINITY);
    GIANT_CHECK(dust.puffs().size() == before);
    for (const DustPuff& p : dust.puffs()) GIANT_CHECK(std::isfinite(p.radius) && std::isfinite(p.density));
}

GIANT_TEST("dust: bounded: at most max_puffs (the faintest give way) and emits_per_step between steps") {
    DustSettings settings;
    settings.max_puffs = 8;
    settings.emits_per_step = 4;
    Dust dust(settings);
    for (int i = 0; i < 10; ++i) dust.emit({100.0 * i, 0.0, 0.0}, 1.0f, 0.1f);
    GIANT_CHECK(dust.puffs().size() == 4);
    GIANT_CHECK(dust.stats().dropped == 6);
    for (int s = 0; s < 3; ++s) {
        dust.step(0.01f, {});
        for (int i = 0; i < 4; ++i) dust.emit({1000.0 + 100.0 * (s * 4 + i), 0.0, 0.0}, 1.0f, 0.1f);
    }
    GIANT_CHECK(dust.puffs().size() == 8);
    // A strong one takes the faintest's place; a faint one is dropped.
    dust.step(0.01f, {});
    const u32 dropped = dust.stats().dropped;
    dust.emit({-500.0, 0.0, 0.0}, 0.5f, 0.01f);
    GIANT_CHECK(dust.stats().dropped == dropped + 1);
    dust.emit({-900.0, 0.0, 0.0}, 20.0f, 0.5f);
    GIANT_CHECK(dust.puffs().size() == 8);
    bool found = false;
    for (const DustPuff& p : dust.puffs()) found = found || p.position.x == -900.0;
    GIANT_CHECK(found);
    // The nearest, nearest surface first.
    std::vector<DustPuff> near;
    dust.nearest({-900.0, 0.0, 30.0}, 3, near);
    GIANT_CHECK(near.size() == 3);
    GIANT_CHECK(near.size() == 3 && near[0].position.x == -900.0);
    dust.clear();
    GIANT_CHECK(dust.puffs().empty());
    dust.nearest({}, 3, near);
    GIANT_CHECK(near.empty());
}

GIANT_TEST("dust: emits into one puff step after step swell it smoothly, never past what they cover") {
    Dust dust;
    dust.emit({0.0, 0.0, 0.0}, 4.0f, 0.4f);
    float last = dust.puffs()[0].radius;
    for (int i = 0; i < 50; ++i) { // a second of trees landing in it, one a step
        dust.step(1.0f / 60.0f, {});
        dust.emit({0.5, 0.0, 0.0}, 2.0f, 0.4f);
        const float r = dust.puffs()[0].radius;
        GIANT_CHECK(i < 3 || r < last * 1.02f); // no pop (past its first, fastest growth)
        if (i >= 3 && r >= last * 1.02f) std::printf("dust step %d: %f -> %f\n", i, last, r);
        last = r;
    }
    GIANT_CHECK(dust.puffs().size() == 1);
    GIANT_CHECK(last < 8.0f); // 4 m grown for about a second, not compounded
    GIANT_CHECK(dust.puffs()[0].density > 0.1f);
}

GIANT_TEST("dust: emits kept as events with what threw them up, bounded, until taken or cleared") {
    DustSettings settings;
    settings.emits_per_step = 4;
    settings.max_puffs = 1024;
    Dust dust(settings);
    dust.emit({1.0, 2.0, 3.0}, 500.0f, 0.1f, k_dust_earth, DustSource::tree, 1.5f); // past max_radius: kept at it
    dust.emit({1.0, 2.0, 3.0}, 10.0f, 0.1f, k_dust_earth, DustSource::tree); // swelling it: no event
    dust.emit({4000.0, 0.0, 0.0}, 1.0f, 0.1f);
    dust.emit({0.0, 0.0, 0.0}, -1.0f, 0.1f); // bad: no event
    GIANT_CHECK(dust.events().size() == 2);
    GIANT_CHECK(dust.events().size() == 2 && dust.events()[0].source == DustSource::tree &&
                dust.events()[0].at == (DVec3{1.0, 0.5, 3.0}) && dust.events()[0].radius == settings.max_radius);
    GIANT_CHECK(dust.events().size() == 2 && dust.events()[1].source == DustSource::rubble);
    for (int i = 0; i < 4; ++i) dust.emit({-5000.0, 0.0, 10.0 * i}, 1.0f, 0.1f); // past emits_per_step: none
    GIANT_CHECK(dust.events().size() == 3);
    dust.clear_events();
    GIANT_CHECK(dust.events().empty() && !dust.puffs().empty());
    // At most k_max_dust_events between takings, the rest counted.
    for (u32 i = 0; i < k_max_dust_events + 10; ++i) {
        dust.step(0.01f, {});
        dust.emit({10000.0 + 100.0 * i, 0.0, 0.0}, 1.0f, 0.1f, k_dust_earth, DustSource::ground);
    }
    GIANT_CHECK(dust.events().size() == k_max_dust_events && dust.stats().events_dropped == 10);
    dust.clear();
    GIANT_CHECK(dust.events().empty());
}
