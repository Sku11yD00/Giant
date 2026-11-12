// Lakes, rivers and the sea (world/water.h): checks, where water is, and
// saving them with the scene and the world.
#include "giant/world/water.h"

#include "giant_test.h"

#include <algorithm>
#include <cmath>

using namespace giant;
using namespace giant::world;

namespace {

Water lake(const char* name, float level) {
    Water w;
    w.name = name;
    w.kind = WaterKind::lake;
    w.level = level;
    // An L: the corner at (10, 10) is dry.
    w.outline = {{0.0, 0.0}, {20.0, 0.0}, {20.0, 5.0}, {5.0, 5.0}, {5.0, 20.0}, {0.0, 20.0}};
    return w;
}

Water river() {
    Water w;
    w.name = "river";
    w.kind = WaterKind::river;
    w.flow = 2.0f;
    w.path = {{0.0, 100.0, 10.0f, 5.0f}, {100.0, 100.0, 30.0f, 3.0f}};
    return w;
}

} // namespace

GIANT_TEST("water: a lake is inside its shore, a river along its path, the sea everywhere; the highest wins") {
    std::vector<Water> waters{lake("pond", 2.0f), river()};
    auto h = water_at(waters, 2.0, 15.0);
    GIANT_CHECK(h && h->water->name == "pond" && h->level == 2.0f);
    GIANT_CHECK(!water_at(waters, 10.0, 10.0)); // the L's dry corner
    GIANT_CHECK(!water_at(waters, -1.0, 2.0));
    // The river: level and width taken along it, flowing from the first point.
    h = water_at(waters, 50.0, 108.0); // half way: 20 m wide, so 10 m each side
    GIANT_CHECK(h && h->water->name == "river" && std::fabs(h->level - 4.0f) < 1e-4f && std::fabs(h->width - 20.0f) < 1e-4f);
    GIANT_CHECK(std::fabs(h->flow_x - 2.0f) < 1e-5f && h->flow_z == 0.0f);
    GIANT_CHECK(!water_at(waters, 50.0, 111.0));
    GIANT_CHECK(water_at(waters, 99.0, 114.0)); // wide at its end
    GIANT_CHECK(!water_at(waters, 1.0, 106.0)); // narrow at its start
    // The sea is everywhere; a lake over it still wins where it is higher.
    Water sea;
    sea.name = "sea";
    sea.kind = WaterKind::sea;
    sea.level = 0.0f;
    waters.push_back(sea);
    GIANT_CHECK(water_at(waters, 10.0, 10.0)->water->name == "sea");
    GIANT_CHECK(water_at(waters, 2.0, 15.0)->water->name == "pond");
    waters[2].level = 9.0f;
    GIANT_CHECK(water_at(waters, 2.0, 15.0)->water->name == "sea");
    // Hidden water is not there.
    waters[2].visible = false;
    waters[0].visible = false;
    GIANT_CHECK(!water_at(waters, 2.0, 15.0));
    double x0, z0, x1, z1;
    GIANT_CHECK(water_bounds(waters[1], x0, z0, x1, z1) && x0 == -5.0 && x1 == 115.0 && z0 == 85.0 && z1 == 115.0);
    GIANT_CHECK(!water_bounds(sea, x0, z0, x1, z1));
    // A river without width (unchecked data) holds no water.
    Water thin = river();
    thin.path[0].width = 0.0f;
    thin.path[1].width = 0.0f;
    GIANT_CHECK(!water_at(std::span<const Water>(&thin, 1), 50.0, 100.0));
}

GIANT_TEST("water: bodies out of their limits are refused") {
    GIANT_CHECK(!check_water(lake("a", 1.0f)));
    GIANT_CHECK(!check_water(river()));
    Water w = lake("a", 1.0f);
    w.outline.resize(2);
    GIANT_CHECK(check_water(w).has_value());
    w = lake("a", std::nanf(""));
    GIANT_CHECK(check_water(w).has_value());
    w = lake("", 1.0f);
    GIANT_CHECK(check_water(w).has_value());
    w = lake("a", 1.0f);
    w.absorption = {0.1f, 11.0f, 0.1f};
    GIANT_CHECK(check_water(w).has_value());
    w = lake("a", 1.0f);
    w.path = river().path; // a lake has no path
    GIANT_CHECK(check_water(w).has_value());
    w = river();
    w.path[1].width = 0.1f;
    GIANT_CHECK(check_water(w).has_value());
    w = river();
    w.path[1] = w.path[0]; // the same point twice
    GIANT_CHECK(check_water(w).has_value());
    w = river();
    w.flow = 30.0f;
    GIANT_CHECK(check_water(w).has_value());
    w = lake("a", 1.0f);
    w.outline[0].x = 1e9;
    GIANT_CHECK(check_water(w).has_value());
    Water sea;
    sea.name = "sea";
    sea.kind = WaterKind::sea;
    GIANT_CHECK(!check_water(sea));
    sea.outline = lake("a", 0.0f).outline;
    GIANT_CHECK(check_water(sea).has_value());
}

GIANT_TEST("water: saved and loaded with the scene and the world, refused when bad or named twice") {
    Scene scene;
    scene.water = {lake("pond", 2.5f), river()};
    scene.water[0].color = {0.1f, 0.2f, 0.3f};
    scene.water[0].flow = 3.0f;  // (kept by a lake: a river again would take it)
    scene.water[1].level = 5.0f; // (kept by a river)
    scene.water[0].visible = false;
    Water sea;
    sea.name = "sea";
    sea.kind = WaterKind::sea;
    sea.level = -1.0f;
    sea.waves = 3.0f;
    scene.water.push_back(sea);
    Scene loaded;
    GIANT_CHECK(!load_scene(save_scene(scene), loaded).has_value());
    GIANT_CHECK(loaded.water == scene.water);
    WorldIndex index;
    index.water = scene.water;
    WorldIndex back;
    GIANT_CHECK(!load_world_index(save_world_index(index), back).has_value());
    GIANT_CHECK(back.water == scene.water);
    // Bad: two of a name.
    scene.water.push_back(lake("pond", 1.0f));
    GIANT_CHECK(load_scene(save_scene(scene), loaded).has_value());
    // Text with one sea named "w", changed: an unknown kind, a river
    // without a path, a misspelt key.
    Scene one;
    Water w = sea;
    w.name = "w";
    one.water = {w};
    const std::string text = save_scene(one);
    const auto changed = [&](const std::string& from, const std::string& to) {
        std::string t = text;
        const usize at = t.find(from);
        GIANT_CHECK(at != std::string::npos);
        if (at != std::string::npos) t.replace(at, from.size(), to);
        return t;
    };
    GIANT_CHECK(!load_scene(text, loaded).has_value() && loaded.water.size() == 1);
    GIANT_CHECK(load_scene(changed("\"sea\"", "\"pool\""), loaded).has_value());
    GIANT_CHECK(load_scene(changed("\"sea\"", "\"river\""), loaded).has_value());
    GIANT_CHECK(load_scene(changed("\"level\"", "\"levle\""), loaded).has_value());
    // A river point missing a key is refused, not read as 0.
    Scene flowing;
    flowing.water = {river()};
    std::string t = save_scene(flowing);
    const usize width = t.find("\"width\"");
    GIANT_CHECK(width != std::string::npos && !load_scene(t, loaded).has_value());
    if (width != std::string::npos) t.replace(width, 7, "\"wodth\"");
    GIANT_CHECK(load_scene(t, loaded).has_value());
}

GIANT_TEST("water: the sea's swell grows with the wind and keeps its waves in shape") {
    const Swell calm = sea_swell(Vec3{0.0f, 0.0f, 0.0f});
    const Swell breeze = sea_swell(Vec3{6.0f, 0.0f, 0.0f});
    const Swell gale = sea_swell(Vec3{0.0f, 3.0f, -20.0f});
    // Higher and longer the stronger; a calm keeps a low swell.
    GIANT_CHECK(swell_most(calm) > 0.1f && swell_most(calm) < 0.5f);
    GIANT_CHECK(swell_most(breeze) > swell_most(calm));
    GIANT_CHECK(swell_most(gale) > swell_most(breeze) && swell_most(gale) < 4.0f);
    GIANT_CHECK(gale.waves[0].k < breeze.waves[0].k);
    // The main wave runs with the wind (level: its up part is not the sea's).
    GIANT_CHECK(std::fabs(gale.waves[0].dir_x) < 1e-5f && gale.waves[0].dir_z < -0.999f);
    for (const Swell* s : {&calm, &breeze, &gale}) {
        for (u32 i = 0; i < k_swell_waves; ++i) {
            const SwellWave& w = s->waves[i];
            if (i >= k_swell_set) {
                GIANT_CHECK(w.amplitude == 0.0f); // (the fading set's: none)
                continue;
            }
            GIANT_CHECK(w.k > 0.0f && w.amplitude > 0.0f && w.amplitude * w.k <= 0.1201f);
            GIANT_CHECK(std::fabs(w.dir_x * w.dir_x + w.dir_z * w.dir_z - 1.0f) < 1e-5f);
        }
    }
    // Bad wind: as a calm.
    GIANT_CHECK(sea_swell(Vec3{std::nanf(""), 0.0f, 0.0f}) == calm);
}

GIANT_TEST("water: the swell rises and falls as its sines say, exact far out and late on") {
    const Swell s = sea_swell(Vec3{10.0f, 0.0f, 4.0f});
    const double most = swell_most(s);
    double low = 1e9;
    double high = -1e9;
    for (int i = 0; i < 400; ++i) {
        const double h = swell_height(s, 13.0 * i, -7.0 * i, 0.37 * i);
        GIANT_CHECK(std::fabs(h) <= most + 1e-9);
        low = std::min(low, h);
        high = std::max(high, h);
    }
    GIANT_CHECK(high > 0.3 * most && low < -0.3 * most);
    // One wave alone: its sine, moving at deep water's speed.
    Swell one;
    one.waves[0] = {1.0f, 0.0f, 0.1f, 2.0f, 0.5f};
    const double k = 0.1f; // (as held)
    const double c = std::sqrt(k_swell_gravity * k) / k; // m/s
    GIANT_CHECK(std::fabs(swell_height(one, 3.0, 9.0, 0.0) - 2.0 * std::sin(0.1f * 3.0 + 0.5f)) < 1e-6);
    GIANT_CHECK(std::fabs(swell_height(one, 3.0 + c * 4.0, 0.0, 4.0) - swell_height(one, 3.0, 0.0, 0.0)) < 1e-6);
    // Far out and late on: still exact (doubles).
    GIANT_CHECK(std::fabs(swell_height(one, 5.0e6 + c * 1.0e5, 0.0, 1.0e5) - swell_height(one, 5.0e6, 0.0, 0.0)) < 1e-4);
    // Its slope: the height's rise, by differences.
    const SwellHere here = swell_at(s, 40.0, -25.0, 3.0);
    const double dx = (swell_height(s, 40.001, -25.0, 3.0) - swell_height(s, 39.999, -25.0, 3.0)) / 0.002;
    const double dz = (swell_height(s, 40.0, -24.999, 3.0) - swell_height(s, 40.0, -25.001, 3.0)) / 0.002;
    GIANT_CHECK(std::fabs(here.slope_x - dx) < 1e-5 && std::fabs(here.slope_z - dz) < 1e-5);
    GIANT_CHECK(here.height == swell_height(s, 40.0, -25.0, 3.0));
    // A wave with a bad number or no direction is not there.
    Swell bad = one;
    bad.waves[1] = {std::nanf(""), 0.0f, 0.1f, 1.0f, 0.0f};
    bad.waves[2] = {0.0f, 0.0f, 0.1f, 1.0f, 0.0f};
    bad.waves[3] = {1.0f, 0.0f, 0.1f, 1.0f, std::nanf("")};
    GIANT_CHECK(swell_most(bad) == 2.0f && swell_height(bad, 3.0, 9.0, 0.0) == swell_height(one, 3.0, 9.0, 0.0));
    // Not a number: 0. A sea's waves scale it, a bad one none.
    GIANT_CHECK(swell_height(s, std::nan(""), 0.0, 0.0) == 0.0);
    Water sea;
    sea.kind = WaterKind::sea;
    sea.waves = 2.5f;
    GIANT_CHECK(swell_scale(sea) == 2.5f);
    sea.waves = std::nanf("");
    GIANT_CHECK(swell_scale(sea) == 0.0f);
}

GIANT_TEST("water: the swell follows a changing wind without its waves jumping") {
    SwellTrack track;
    const double dt = 1.0 / 60.0;
    // An hour in, far from the origin, the wind easing from 5 to 12 m/s
    // over a minute and turning 90 degrees over the next.
    const double x = 3.0e5, z = -2.0e5;
    double t = 3600.0;
    track.update({5.0f, 0.0f, 0.0f}, t);
    double last = swell_height(track.swell(), x, z, t);
    double most_step = 0.0;
    bool faded = false;
    for (int i = 0; i < 60 * 150; ++i) {
        t += dt;
        const double s = std::min(i * dt / 60.0, 1.0);
        const double turn = std::clamp((i * dt - 60.0) / 60.0, 0.0, 1.0) * 1.5707963;
        const float speed = static_cast<float>(5.0 + 7.0 * s);
        track.update({speed * static_cast<float>(std::cos(turn)), 0.0f, speed * static_cast<float>(std::sin(turn))}, t);
        const double h = swell_height(track.swell(), x, z, t);
        most_step = std::max(most_step, std::fabs(h - last));
        last = h;
        for (u32 w = k_swell_set; w < k_swell_waves; ++w) faded |= track.swell().waves[w].amplitude > 0.0f;
    }
    // At most what the waves' own motion gives a frame (a 2 m sea's: a few cm).
    GIANT_CHECK(most_step < 0.08);
    GIANT_CHECK(faded);
    // In the end: the new wind's swell, the old one gone.
    const Swell end = sea_swell({0.0f, 0.0f, 12.0f});
    track.update({0.0f, 0.0f, 12.0f}, t + 100.0);
    GIANT_CHECK(std::fabs(swell_most(track.swell()) - swell_most(end)) < 0.05f);
    GIANT_CHECK(track.swell().waves[0].dir_z > 0.9f);
}

GIANT_TEST("water: a sudden change of wind eases the swell in, no step") {
    SwellTrack track;
    double t = 500.0;
    track.update({4.0f, 0.0f, 0.0f}, t);
    double last = swell_height(track.swell(), 120.0, 40.0, t);
    double most_step = 0.0;
    for (int i = 0; i < 60 * 60; ++i) {
        t += 1.0 / 60.0;
        track.update({6.8f, 0.0f, 0.0f}, t); // (a step in speed, the waves' length held at 40 m)
        const double h = swell_height(track.swell(), 120.0, 40.0, t);
        most_step = std::max(most_step, std::fabs(h - last));
        last = h;
    }
    GIANT_CHECK(most_step < 0.05);
    // A minute on: nearly the new wind's height.
    GIANT_CHECK(std::fabs(swell_most(track.swell()) - swell_most(sea_swell({6.8f, 0.0f, 0.0f}))) < 0.02f);
}
