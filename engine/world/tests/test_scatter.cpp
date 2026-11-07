// Scatter: placements by rule over the ground, cell by cell, the same every time.
#include "giant/world/scatter.h"

#include "giant_test.h"

#include <cmath>
#include <string>

using namespace giant;
using namespace giant::world;

namespace {

// Flat ground at `height` over cells x0..x1, z0..z1.
Terrain flat(i32 x0, i32 z0, i32 x1, i32 z1, float height = 0.0f, u32 samples = 16) {
    Terrain t;
    t.set_samples(samples);
    for (i32 z = z0; z <= z1; ++z) {
        for (i32 x = x0; x <= x1; ++x) t.set({x, z}, Terrain::Heights(usize{samples} * samples, height));
    }
    return t;
}

ScatterLayer rocks(float spacing = 4.0f) {
    ScatterLayer l;
    l.name = "rocks";
    l.spacing = spacing;
    ScatterItem a;
    a.shape = Shape::sphere;
    ScatterItem b;
    b.shape = Shape::cube;
    b.weight = 3.0f;
    b.scale_min = 0.5f;
    b.scale_max = 2.0f;
    l.items = {a, b};
    return l;
}

std::vector<ScatterPoint> points(const ScatterLayer& l, const Terrain& t, CellCoord c) {
    std::vector<ScatterPoint> out;
    scatter_cell(l, 0, t, c, out);
    return out;
}

bool same(const std::vector<ScatterPoint>& a, const std::vector<ScatterPoint>& b) {
    if (a.size() != b.size()) return false;
    for (usize k = 0; k < a.size(); ++k) {
        if (a[k].item != b[k].item || !(a[k].transform == b[k].transform)) return false;
    }
    return true;
}

} // namespace

GIANT_TEST("scatter: a cell's points are the same every time, apart by spacing, on the ground, by weight") {
    const Terrain t = flat(-1, -1, 1, 1, 5.0f);
    const ScatterLayer l = rocks();
    const std::vector<ScatterPoint> p = points(l, t, {0, 0});
    GIANT_CHECK(same(p, points(l, t, {0, 0})));
    GIANT_CHECK(p.size() == 16 * 16); // density 1: a point a square
    GIANT_CHECK(scatter_candidates(l, {0, 0}) == 17 * 17);
    usize cubes = 0;
    for (const ScatterPoint& a : p) {
        GIANT_CHECK(cell_of(a.transform.position) == (CellCoord{0, 0}));
        GIANT_CHECK(a.transform.position.y == 5.0);
        cubes += a.item == 1 ? 1 : 0;
        if (a.item == 1) GIANT_CHECK(a.transform.scale.x >= 0.5f && a.transform.scale.x <= 2.0f);
        for (const ScatterPoint& b : p) {
            if (&a == &b) continue;
            const double d = std::hypot(a.transform.position.x - b.transform.position.x,
                                        a.transform.position.z - b.transform.position.z);
            GIANT_CHECK(d >= 4.0 * (1.0 - 0.9) - 1e-9);
        }
    }
    GIANT_CHECK(cubes > p.size() / 2 && cubes < p.size()); // weight 3 to 1: about 3/4
    // Another seed: other points.
    ScatterLayer other = l;
    other.seed = 2;
    GIANT_CHECK(!same(p, points(other, t, {0, 0})));
    // Nothing without a tile, items, or when hidden.
    GIANT_CHECK(points(l, t, {5, 5}).empty());
    other = l;
    other.visible = false;
    GIANT_CHECK(points(other, t, {0, 0}).empty());
}

GIANT_TEST("scatter: a cell does not depend on tiles further than its neighbours, and cells share no point") {
    const ScatterLayer l = rocks(3.0f); // squares cross cell edges
    const Terrain small = flat(-1, -1, 1, 1);
    const Terrain big = flat(-4, -4, 4, 4);
    GIANT_CHECK(same(points(l, small, {0, 0}), points(l, big, {0, 0})));
    // Neighbouring cells together hold every square's point once.
    usize total = 0;
    for (i32 z = -1; z <= 1; ++z) {
        for (i32 x = -1; x <= 1; ++x) total += points(l, big, {x, z}).size();
    }
    GIANT_CHECK(total == 64 * 64); // 192 m / 3 m = 64 squares a side
}

GIANT_TEST("scatter: density, clumps, area, height and slope choose where") {
    const Terrain t = flat(0, 0, 0, 0, 10.0f);
    ScatterLayer l = rocks(1.0f);
    const usize all = points(l, t, {0, 0}).size();
    GIANT_CHECK(all == 64 * 64);
    l.density = 0.25f;
    const usize quarter = points(l, t, {0, 0}).size();
    GIANT_CHECK(quarter > all / 5 && quarter < all / 3);
    l.density = 1.0f;
    l.clump_size = 16.0f;
    l.clumping = 1.0f;
    const usize clumped = points(l, t, {0, 0}).size();
    GIANT_CHECK(clumped > all / 10 && clumped < all * 9 / 10);
    l.clumping = 0.0f;
    l.area = ScatterArea{10.0, 10.0, 20.0, 30.0};
    for (const ScatterPoint& p : points(l, t, {0, 0})) {
        GIANT_CHECK(p.transform.position.x >= 10.0 && p.transform.position.x <= 20.0);
        GIANT_CHECK(p.transform.position.z >= 10.0 && p.transform.position.z <= 30.0);
    }
    GIANT_CHECK(points(l, t, {0, 0}).size() > 150 && points(l, t, {0, 0}).size() < 250);
    l.area.reset();
    l.height_min = 11.0f;
    GIANT_CHECK(points(l, t, {0, 0}).empty());
    l.height_min = 9.0f;
    l.height_max = 11.0f;
    GIANT_CHECK(points(l, t, {0, 0}).size() == all);
    // A 45 degree slope: kept by slope 40 to 50 only.
    Terrain hill;
    hill.set_samples(16);
    Terrain::Heights h(256);
    for (usize j = 0; j < 16; ++j) {
        for (usize i = 0; i < 16; ++i) h[j * 16 + i] = static_cast<float>(i) * 4.0f; // rises 4 m a 4 m sample
    }
    hill.set({0, 0}, h);
    l.height_min = -10000.0f;
    l.height_max = 10000.0f;
    l.area = ScatterArea{4.0, 4.0, 56.0, 60.0}; // off the tile's edges, where the slope stops
    l.slope_max = 30.0f;
    GIANT_CHECK(points(l, hill, {0, 0}).empty());
    l.slope_min = 40.0f;
    l.slope_max = 50.0f;
    const std::vector<ScatterPoint> on_slope = points(l, hill, {0, 0});
    GIANT_CHECK(!on_slope.empty());
    for (const ScatterPoint& p : on_slope) {
        GIANT_CHECK(std::fabs(p.transform.position.y - p.transform.position.x) < 1e-3); // on the surface
    }
}

GIANT_TEST("scatter: the ground's slope holds at its edge") {
    Terrain hill;
    hill.set_samples(16);
    Terrain::Heights h(256);
    for (usize j = 0; j < 16; ++j) {
        for (usize i = 0; i < 16; ++i) h[j * 16 + i] = static_cast<float>(i) * 4.0f;
    }
    hill.set({0, 0}, h);
    for (const double x : {0.5, 2.0, 30.0}) { // x - 4 off the ground for the first two
        const std::optional<Vec3> n = ground_normal(hill, x, 30.0, 4.0);
        GIANT_CHECK(n.has_value() && std::fabs(n->y - std::sqrt(0.5f)) < 1e-4f);
    }
}

GIANT_TEST("scatter: items stand upright or along the ground, sunk by their scale") {
    Terrain hill;
    hill.set_samples(16);
    Terrain::Heights h(256);
    for (usize j = 0; j < 16; ++j) {
        for (usize i = 0; i < 16; ++i) h[j * 16 + i] = static_cast<float>(i) * 4.0f;
    }
    hill.set({0, 0}, h);
    ScatterLayer l;
    l.name = "boulders";
    l.spacing = 8.0f;
    l.area = ScatterArea{8.0, 8.0, 56.0, 56.0};
    ScatterItem item;
    item.align = 1.0f;
    item.sink = 0.5f;
    item.scale_min = item.scale_max = 2.0f;
    l.items = {item};
    const std::vector<ScatterPoint> p = points(l, hill, {0, 0});
    GIANT_CHECK(!p.empty());
    for (const ScatterPoint& a : p) {
        const Vec3 up = rotate(a.transform.rotation, {0.0f, 1.0f, 0.0f});
        const float s = std::sqrt(0.5f);
        GIANT_CHECK(std::fabs(up.x + s) < 1e-3f && std::fabs(up.y - s) < 1e-3f); // the normal of a 45 degree rise
        GIANT_CHECK(std::fabs(a.transform.position.y - (a.transform.position.x - 1.0)) < 1e-3); // 0.5 m x 2
    }
    l.items[0].align = 0.0f;
    for (const ScatterPoint& a : points(l, hill, {0, 0})) {
        const Vec3 up = rotate(a.transform.rotation, {0.0f, 1.0f, 0.0f});
        GIANT_CHECK(up.y > 0.9999f);
    }
}

GIANT_TEST("scatter: layers are checked, and saved with scenes and worlds") {
    ScatterLayer l = rocks();
    GIANT_CHECK(!check_scatter_layer(l).has_value());
    ScatterLayer bad = l;
    bad.spacing = 0.1f;
    GIANT_CHECK(check_scatter_layer(bad).has_value());
    bad = l;
    bad.items[0].shape = Shape::mesh; // no file
    GIANT_CHECK(check_scatter_layer(bad).has_value());
    bad = l;
    bad.slope_min = 50.0f;
    bad.slope_max = 10.0f;
    GIANT_CHECK(check_scatter_layer(bad).has_value());
    bad = l;
    bad.items.clear();
    GIANT_CHECK(check_scatter_layer(bad).has_value());
    bad = l;
    bad.clump_size = 1e-30f; // a lattice too fine to count
    GIANT_CHECK(check_scatter_layer(bad).has_value());
    bad.clump_size = 1.0f;
    GIANT_CHECK(!check_scatter_layer(bad).has_value());
    bad = l;
    bad.items[0].mesh = "assets/rock.glb"; // a sphere with a mesh
    GIANT_CHECK(check_scatter_layer(bad).has_value());
    bad = l;
    bad.items[0].tree = ScatterTree{.trunk_radius = 1.0f, .height = 1.5f}; // shorter than it is wide
    GIANT_CHECK(check_scatter_layer(bad).has_value());
    bad.items[0].tree = ScatterTree{.strength = -1.0f};
    GIANT_CHECK(check_scatter_layer(bad).has_value());
    bad.items[0].tree = ScatterTree{};
    GIANT_CHECK(!check_scatter_layer(bad).has_value());

    Scene scene;
    l.area = ScatterArea{-5.5, 2.0, 100.0, 200.25};
    l.clump_size = 30.0f;
    l.clumping = 0.5f;
    l.slope_max = 35.0f;
    l.seed = 77;
    l.items[1].shape = Shape::mesh;
    l.items[1].mesh = "assets/rock.glb";
    l.items[0].material.base_color = {0.25f, 0.5f, 0.75f};
    l.items[0].align = 0.5f;
    l.items[0].sink = 0.25f;
    l.items[0].tree = ScatterTree{.trunk_radius = 0.4f, .height = 18.5f, .strength = 5000.0f};
    scene.scatter.push_back(l);
    ScatterLayer grass;
    grass.name = "grass";
    grass.spacing = 0.5f;
    grass.items = {ScatterItem{}};
    grass.items[0].shape = Shape::kit;
    grass.items[0].kit = "tuft";
    grass.visible = false;
    Kit tuft;
    tuft.name = "tuft";
    tuft.pieces.push_back({});
    scene.kits.push_back(tuft);
    scene.scatter.push_back(grass);
    Scene loaded;
    GIANT_CHECK(!load_scene(save_scene(scene), loaded).has_value());
    GIANT_CHECK(loaded.scatter == scene.scatter);
    WorldIndex world;
    world.kits = scene.kits;
    world.scatter = scene.scatter;
    WorldIndex back;
    GIANT_CHECK(!load_world_index(save_world_index(world), back).has_value());
    GIANT_CHECK(back.scatter == world.scatter);
    // Strict: a bad value, a name used twice.
    std::string text = save_scene(scene);
    std::string twice = text;
    twice.replace(twice.find("\"grass\""), 7, "\"rocks\"");
    GIANT_CHECK(load_scene(twice, loaded).has_value());
    std::string slow = text;
    slow.replace(slow.find("\"spacing\": 0.5"), 14, "\"spacing\": 0.1");
    GIANT_CHECK(load_scene(slow, loaded).has_value());
    GIANT_CHECK(text.find("\"tree\"") != std::string::npos);
    std::string weak_tree = text;
    weak_tree.replace(weak_tree.find("\"height\": 18.5"), 14, "\"height\": 0.01");
    GIANT_CHECK(load_scene(weak_tree, loaded).has_value());
    std::string no_kit = text;
    no_kit.replace(no_kit.find("\"tuft\""), 6, "\"nokit\"");
    GIANT_CHECK(load_scene(no_kit, loaded).has_value()); // a kit item names one of the scene's kits
    GIANT_CHECK(loaded.scatter == scene.scatter); // unchanged by a failed load
}
