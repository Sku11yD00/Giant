// The scene's water as the renderer's triangles (water plan W2c): lakes cut
// by ear clipping, rivers as strips, the sea as rings round the camera;
// made again only when the water changes.
#include "../src/water_meshes.h"

#include "giant_test.h"

#include <cmath>

using namespace giant;
using namespace giant::editor;

namespace {

double area(std::span<const world::WaterPoint> p, std::span<const u32> tris) {
    double sum = 0.0;
    for (usize i = 0; i + 2 < tris.size(); i += 3) {
        const world::WaterPoint& a = p[tris[i]];
        const world::WaterPoint& b = p[tris[i + 1]];
        const world::WaterPoint& c = p[tris[i + 2]];
        sum += std::fabs((b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x)) * 0.5;
    }
    return sum;
}

world::Water lake(std::vector<world::WaterPoint> outline, float level = 2.0f) {
    world::Water w;
    w.name = "lake";
    w.kind = world::WaterKind::lake;
    w.level = level;
    w.outline = std::move(outline);
    return w;
}

} // namespace

GIANT_TEST("water meshes: shores of either winding cut whole, concave ones too; crossed ones counted") {
    bool whole = false;
    // A square, clockwise and counter-clockwise: two triangles, its area.
    std::vector<world::WaterPoint> sq{{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    std::vector<u32> t = clip_ears(sq, whole);
    GIANT_CHECK(whole && t.size() == 6 && std::fabs(area(sq, t) - 100.0) < 1e-9);
    std::vector<world::WaterPoint> back(sq.rbegin(), sq.rend());
    t = clip_ears(back, whole);
    GIANT_CHECK(whole && t.size() == 6 && std::fabs(area(back, t) - 100.0) < 1e-9);
    // An L (a corner turning back): four triangles covering 75.
    std::vector<world::WaterPoint> ell{{0, 0}, {10, 0}, {10, 5}, {5, 5}, {5, 10}, {0, 10}};
    t = clip_ears(ell, whole);
    GIANT_CHECK(whole && t.size() == 12 && std::fabs(area(ell, t) - 75.0) < 1e-9);
    // A point in a straight line and one doubled: the area kept, the doubled one dropped.
    std::vector<world::WaterPoint> extra{{0, 0}, {5, 0}, {10, 0}, {10, 10}, {10, 10}, {0, 10}};
    t = clip_ears(extra, whole);
    GIANT_CHECK(whole && t.size() <= 9 && std::fabs(area(extra, t) - 100.0) < 1e-9); // (no sliver: a cut through the line point is fine)
    // Shores that cross themselves (a bow tie, one with area left over, a
    // figure eight, a star, one touching itself): none, not whole.
    const std::vector<std::vector<world::WaterPoint>> crossed{
        {{0, 0}, {10, 10}, {10, 0}, {0, 10}},
        {{0, 0}, {10, 10}, {10, 0}, {0, 12}},
        {{0, 0}, {4, 4}, {6, 2}, {4, 0}, {0, 4}},
        {{0, 10}, {6, -8}, {-9.5, 3}, {9.5, 3}, {-6, -8}},
        {{0, 0}, {10, 0}, {10, 10}, {5, 0}, {0, 10}}};
    for (const auto& c : crossed) {
        GIANT_CHECK(clip_ears(c, whole).empty() && !whole);
    }
    // Far from the origin (10,000 km out): a small square still whole.
    std::vector<world::WaterPoint> far{{9.9e6, 9.9e6}, {9.9e6 + 1, 9.9e6}, {9.9e6 + 1, 9.9e6 + 1}, {9.9e6, 9.9e6 + 1}};
    t = clip_ears(far, whole);
    GIANT_CHECK(whole && t.size() == 6 && std::fabs(area(far, t) - 1.0) < 1e-6);
    // Too few points, or none with area.
    std::vector<world::WaterPoint> line{{0, 0}, {5, 0}, {10, 0}};
    GIANT_CHECK(clip_ears(line, whole).empty() && !whole);
    GIANT_CHECK(clip_ears(std::span<const world::WaterPoint>{}, whole).empty() && !whole);
    // A big round shore (4096 points): whole, n - 2 triangles.
    std::vector<world::WaterPoint> round;
    for (u32 i = 0; i < 4096; ++i) {
        const double a = 6.283185307179586 * i / 4096.0;
        round.push_back({1000.0 * std::cos(a) + 3.0 * std::sin(a * 37.0), 1000.0 * std::sin(a)});
    }
    t = clip_ears(round, whole);
    GIANT_CHECK(whole && t.size() == (4096 - 2) * 3);
    // A spiral-like comb (many corners turning back): whole, its area kept.
    std::vector<world::WaterPoint> comb;
    for (u32 i = 0; i < 1000; ++i) {
        comb.push_back({i * 2.0, 0.0});
        comb.push_back({i * 2.0, 0.0 + 50.0});
        comb.push_back({i * 2.0 + 1.0, 50.0});
        comb.push_back({i * 2.0 + 1.0, 1.0});
    }
    comb.push_back({2000.0, 1.0});
    comb.push_back({2000.0, -1.0});
    comb.push_back({0.0, -1.0});
    comb.erase(comb.begin()); // starts at (0, 50)
    comb.insert(comb.begin(), {0.0, -1.0});
    comb.pop_back();
    t = clip_ears(comb, whole);
    double comb_area = 0.0;
    for (usize i = 0; i < comb.size(); ++i) {
        const auto& a = comb[i];
        const auto& b = comb[(i + 1) % comb.size()];
        comb_area += a.x * b.z - b.x * a.z;
    }
    GIANT_CHECK(whole && std::fabs(area(comb, t) - std::fabs(comb_area) * 0.5) < 1e-6 * std::fabs(comb_area));
}

GIANT_TEST("water meshes: lakes at their level, rivers as strips with their flow, hidden bodies left out") {
    WaterMeshes m;
    std::vector<world::Water> water;
    water.push_back(lake({{0, 0}, {10, 0}, {10, 10}, {0, 10}}, 3.0f));
    water.back().color = {0.1f, 0.2f, 0.3f};
    water.back().waves = 2.0f;
    world::Water river;
    river.name = "river";
    river.kind = world::WaterKind::river;
    river.flow = 2.0f;
    river.path = {{0.0, 100.0, 8.0f, 5.0f}, {50.0, 100.0, 12.0f, 4.0f}, {100.0, 100.0, 8.0f, 3.0f}};
    water.push_back(river);
    world::Water hidden = water[0];
    hidden.name = "hidden";
    hidden.visible = false;
    water.push_back(hidden);
    m.update(water, {0.0, 10.0, 0.0});
    GIANT_CHECK(m.rebuilds() == 2 && m.broken_lakes() == 0); // the lake and the river (the hidden one is not cut)
    GIANT_CHECK(m.looks().size() == 2 && m.looks()[0].color.y == 0.2f && m.looks()[0].waves == 2.0f);
    // Lowest first: the lake (3 m) then the river (to 5 m). The lake: 4
    // vertices, 2 triangles; the river: 2 a point, 2 triangles a stretch.
    GIANT_CHECK(m.vertices().size() == 4 + 6 && m.indices().size() == 6 + 12);
    for (usize i = 0; i < 4; ++i) GIANT_CHECK(m.vertices()[i].position.y == 3.0 && m.vertices()[i].look == 0);
    // Across the river at its middle point: 12 m wide, at its level, flowing +x at 2 m/s.
    const render::WaterVertex& l = m.vertices()[4 + 2];
    const render::WaterVertex& r = m.vertices()[4 + 3];
    GIANT_CHECK(std::fabs(l.position.x - 50.0) < 1e-9 && std::fabs(std::fabs(l.position.z - r.position.z) - 12.0) < 1e-6);
    GIANT_CHECK(l.position.y == 4.0 && l.look == 1 && std::fabs(l.flow_x - 2.0f) < 1e-6f && l.flow_z == 0.0f);
    for (const u32 i : m.indices()) GIANT_CHECK(i < m.vertices().size());
    // The same water again: not made again; a colour changed: made again.
    m.update(water, {5.0, 10.0, 5.0});
    GIANT_CHECK(m.rebuilds() == 2);
    // The lake's colour and level changed: put together again, not cut again.
    water[0].color.x = 0.5f;
    water[0].level = 3.5f;
    m.update(water, {5.0, 10.0, 5.0});
    GIANT_CHECK(m.rebuilds() == 2 && m.looks()[0].color.x == 0.5f && m.vertices()[0].position.y == 3.5);
    // Its shore moved: only the lake cut again.
    water[0].outline[2].x = 12.0;
    m.update(water, {5.0, 10.0, 5.0});
    GIANT_CHECK(m.rebuilds() == 3);
    // A crossed shore: counted, not drawn.
    water[0].outline = {{0, 0}, {10, 10}, {10, 0}, {0, 12}};
    m.update(water, {});
    GIANT_CHECK(m.broken_lakes() == 1 && m.vertices().size() == 6 && m.indices().size() == 12);
    // No water: nothing.
    m.update({}, {});
    GIANT_CHECK(m.vertices().empty() && m.indices().empty() && m.looks().empty());
}

GIANT_TEST("water meshes: a river keeps its width round a bend") {
    WaterMeshes m;
    world::Water river;
    river.name = "river";
    river.kind = world::WaterKind::river;
    river.path = {{0.0, 0.0, 10.0f, 1.0f}, {100.0, 0.0, 10.0f, 1.0f}, {100.0, 100.0, 10.0f, 1.0f}};
    m.update(std::vector<world::Water>{river}, {});
    // At the right-angle bend the two edges are the stretches' own edges' corners.
    const render::WaterVertex& a = m.vertices()[2];
    const render::WaterVertex& b = m.vertices()[3];
    GIANT_CHECK(std::fabs(std::hypot(a.position.x - b.position.x, a.position.z - b.position.z) - 10.0 * std::sqrt(2.0)) < 1e-6);
    GIANT_CHECK(std::fabs(std::fabs(a.position.x - 100.0) - 5.0) < 1e-6 && std::fabs(std::fabs(a.position.z) - 5.0) < 1e-6);
}

GIANT_TEST("water meshes: the sea is drawn under higher water, whatever the scene's order") {
    WaterMeshes m;
    world::Water sea;
    sea.name = "sea";
    sea.kind = world::WaterKind::sea;
    sea.level = 0.0f;
    std::vector<world::Water> water{lake({{0, 0}, {10, 0}, {10, 10}, {0, 10}}, -2.0f),
                                    lake({{0, 0}, {10, 0}, {10, 10}, {0, 10}}, 3.0f), sea};
    water[1].name = "high";
    m.update(water, {});
    // Indices in order: the low lake's, the sea's, the high lake's last.
    const auto idx = m.indices();
    const auto v = m.vertices();
    GIANT_CHECK(v[idx[0]].position.y == -2.0 && v[idx[6]].position.y == 0.0 && v[idx.back()].position.y == 3.0);
    // Looks stay in the scene's order.
    GIANT_CHECK(v[idx[0]].look == 0 && v[idx[6]].look == 2 && v[idx.back()].look == 1);
}

GIANT_TEST("water meshes: the sea is rings round the camera out to the horizon, following it") {
    WaterMeshes m;
    world::Water sea;
    sea.name = "sea";
    sea.kind = world::WaterKind::sea;
    sea.level = -1.0f;
    std::vector<world::Water> water{sea};
    m.update(water, {1000.0, 20.0, -500.0});
    const auto vertices = m.vertices();
    GIANT_CHECK(!vertices.empty() && m.indices().size() % 3 == 0);
    double farthest = 0.0;
    for (const render::WaterVertex& v : vertices) {
        GIANT_CHECK(v.position.y == -1.0 && v.look == 0);
        farthest = std::max(farthest, std::hypot(v.position.x - 1000.0, v.position.z + 500.0));
    }
    GIANT_CHECK(std::fabs(farthest - WaterMeshes::k_sea_edge) < 1.0);
    GIANT_CHECK(std::fabs(vertices[0].position.x - 1000.0) < 1e-9 && std::fabs(vertices[0].position.z + 500.0) < 1e-9);
    for (const u32 i : m.indices()) GIANT_CHECK(i < vertices.size());
    // Moved: the rings with it, nothing made again.
    m.update(water, {-3000.0, 20.0, 7000.0});
    GIANT_CHECK(m.rebuilds() == 0 && std::fabs(m.vertices()[0].position.x + 3000.0) < 1e-9); // (the sea is never cut)
    // Within the renderer's caps.
    GIANT_CHECK(m.vertices().size() < render::k_max_water_vertices && m.indices().size() / 3 < render::k_max_water_triangles);
    // A camera that is not a number: the rings where they were.
    m.update(water, {std::nan(""), 0.0, 0.0});
    GIANT_CHECK(std::fabs(m.vertices()[0].position.x + 3000.0) < 1e-9);
}
