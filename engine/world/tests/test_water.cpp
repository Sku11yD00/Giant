// Lakes, rivers and the sea (world/water.h): checks, where water is, and
// saving them with the scene and the world.
#include "giant/world/water.h"

#include "giant_test.h"

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
    // Bad: two of a name, a river without a path, an unknown key.
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
}
