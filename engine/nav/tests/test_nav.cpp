// Navigation: tiles of walkable ground from the terrain and what stands on
// it; paths around walls, up gentle slopes, never up cliffs or high steps,
// pulled straight; searches in bounded slices that wait for missing tiles.
#include "giant/nav/nav.h"

#include "giant_test.h"

#include <cmath>
#include <functional>
#include <string>

using namespace giant;
using namespace giant::nav;

namespace {

// Tiles of terrain over cells [x0, x1] x [z0, z1], heights from f(x, z).
world::Terrain ground(int x0, int x1, int z0, int z1, const std::function<float(double, double)>& f) {
    world::Terrain t;
    const u32 n = t.samples();
    const double s = t.spacing();
    for (int cz = z0; cz <= z1; ++cz) {
        for (int cx = x0; cx <= x1; ++cx) {
            world::Terrain::Heights h(usize{n} * n);
            for (u32 j = 0; j < n; ++j) {
                for (u32 i = 0; i < n; ++i) h[j * n + i] = f(cx * 64.0 + i * s, cz * 64.0 + j * s);
            }
            t.set({cx, cz}, std::move(h));
        }
    }
    return t;
}

float flat(double, double) { return 10.0f; }

Obstacle box(double x, double y, double z, float hx, float hy, float hz, float yaw_degrees = 0.0f) {
    Obstacle o;
    o.center = {x, y, z};
    o.half = {hx, hy, hz};
    o.rotation = quat_from_axis_angle({0.0f, 1.0f, 0.0f}, degrees_to_radians(yaw_degrees));
    return o;
}

// The map's tiles over cells [x0, x1] x [z0, z1] and the ring around them
// (no ground there: the search looks over the edge, and finds nothing).
void build(NavMap& map, const world::Terrain& t, std::span<const Obstacle> obstacles, int x0, int x1, int z0, int z1) {
    for (int cz = z0 - 1; cz <= z1 + 1; ++cz) {
        for (int cx = x0 - 1; cx <= x1 + 1; ++cx) map.set_tile(build_tile({cx, cz}, t, obstacles, map.settings()));
    }
}

SearchState run(PathSearch& s) {
    for (int k = 0; k < 10000 && s.state() == SearchState::searching; ++k) s.advance(1u << 16);
    return s.state();
}

// Whether the path never passes within `r` of the box's footprint (yaw 0).
bool clear_of(const std::vector<DVec3>& path, const Obstacle& o, double r) {
    for (usize k = 1; k < path.size(); ++k) {
        for (int t = 0; t <= 100; ++t) {
            const DVec3 p = path[k - 1] + (path[k] - path[k - 1]) * (t / 100.0);
            if (std::fabs(p.x - o.center.x) < o.half.x + r && std::fabs(p.z - o.center.z) < o.half.z + r) return false;
        }
    }
    return true;
}

} // namespace

GIANT_TEST("nav: flat ground is all walkable at its height; no ground, none") {
    const world::Terrain t = ground(0, 0, 0, 0, flat);
    const NavTile tile = build_tile({0, 0}, t, {}, {});
    GIANT_CHECK(tile.walkable_count == k_nav_side * k_nav_side);
    GIANT_CHECK(tile.height[5] == 10.0f);
    const NavTile none = build_tile({3, 0}, t, {}, {});
    GIANT_CHECK(none.walkable_count == 0 && std::isnan(none.height[0]));
    GIANT_CHECK(none.version != tile.version);
    NavMap map;
    map.set_tile(tile);
    GIANT_CHECK(map.at(10.2, 30.7).built && map.at(10.2, 30.7).walkable && map.at(10.2, 30.7).height == 10.0f);
    GIANT_CHECK(!map.at(-0.1, 3.0).built && !map.at(64.0, 3.0).built);
    GIANT_CHECK(NavMap::cell_of_sample(-1, 0) == (world::CellCoord{-1, 0}) &&
                NavMap::cell_of_sample(-128, 127) == (world::CellCoord{-1, 0}) &&
                NavMap::cell_of_sample(-129, 128) == (world::CellCoord{-2, 1}));
}

GIANT_TEST("nav: slopes past the limit are walls; walls block, grown by the agent; low slabs are stood on") {
    // A ramp along x: gentle (20 degrees) for x < 32, a cliff (60) past it.
    const world::Terrain t = ground(0, 0, 0, 0, [](double x, double) {
        return static_cast<float>(x < 32.0 ? x * std::tan(0.35) : 32.0 * std::tan(0.35) + (x - 32.0) * std::tan(1.05));
    });
    const NavTile tile = build_tile({0, 0}, t, {}, {});
    GIANT_CHECK(tile.walkable[20 * k_nav_side + 20] == 1);  // x 10 m: gentle
    GIANT_CHECK(tile.walkable[20 * k_nav_side + 100] == 0); // x 50 m: a cliff
    const world::Terrain f = ground(0, 0, 0, 0, flat);
    const Obstacle wall = box(20.0, 11.5, 20.0, 0.2f, 1.5f, 5.0f);
    const Obstacle slab = box(40.0, 10.1, 40.0, 3.0f, 0.1f, 3.0f); // 0.2 m: a step up
    const Obstacle sign = box(40.0, 13.0, 50.0, 2.0f, 0.5f, 0.1f); // overhead: walked under
    const Obstacle obstacles[] = {wall, slab, sign};
    NavMap map;
    map.set_tile(build_tile({0, 0}, f, obstacles, {}));
    GIANT_CHECK(!map.at(20.0, 20.0).walkable);
    GIANT_CHECK(!map.at(20.45, 20.0).walkable); // within the agent's radius (0.35) of the wall's face
    GIANT_CHECK(map.at(20.8, 20.0).walkable);
    GIANT_CHECK(map.at(40.0, 40.0).walkable && std::fabs(map.at(40.0, 40.0).height - 10.2f) < 1e-4f);
    GIANT_CHECK(map.at(40.0, 50.0).walkable && map.at(40.0, 50.0).height == 10.0f);
    // A wall turned 90 degrees blocks across x.
    const Obstacle turned[] = {box(20.0, 11.5, 20.0, 0.2f, 1.5f, 5.0f, 90.0f)};
    NavMap other;
    other.set_tile(build_tile({0, 0}, f, turned, {}));
    GIANT_CHECK(!other.at(24.0, 20.0).walkable && other.at(20.0, 24.0).walkable);
}

GIANT_TEST("nav: a path goes around a wall, pulled straight, keeping clear of it") {
    const world::Terrain f = ground(0, 0, 0, 0, flat);
    const Obstacle wall = box(32.0, 11.5, 32.0, 0.2f, 1.5f, 10.0f);
    const Obstacle obstacles[] = {wall};
    NavMap map;
    build(map, f, obstacles, 0, 0, 0, 0);
    PathSearch open(map, {10.0, 10.0, 10.0}, {50.0, 10.0, 10.0});
    GIANT_CHECK(run(open) == SearchState::found);
    GIANT_CHECK(open.path().size() == 2); // nothing in the way: one straight line
    GIANT_CHECK(std::fabs(open.length() - 40.0) < 0.6);
    PathSearch around(map, {20.0, 10.0, 32.0}, {44.0, 10.0, 32.0});
    GIANT_CHECK(run(around) == SearchState::found);
    const std::vector<DVec3>& p = around.path();
    GIANT_CHECK(p.size() >= 3 && p.size() <= 5);
    GIANT_CHECK(clear_of(p, wall, 0.3));
    GIANT_CHECK(around.length() > 24.0 && around.length() < 24.0 + 2.0 * 10.6 + 2.0);
    GIANT_CHECK(std::fabs(p.front().x - 20.0) < 0.3 && std::fabs(p.back().x - 44.0) < 0.3 && p.back().y == 10.0);
}

GIANT_TEST("nav: no way up a cliff or a high step; an end off the ground snaps to it, or fails") {
    // A plateau 2 m up for x >= 32 (a sheer step), reachable by no ramp.
    const world::Terrain t = ground(0, 0, 0, 0, [](double x, double) { return x >= 32.0 ? 12.0f : 10.0f; });
    NavMap map;
    build(map, t, {}, 0, 0, 0, 0);
    PathSearch up(map, {10.0, 10.0, 10.0}, {50.0, 12.0, 10.0});
    GIANT_CHECK(run(up) == SearchState::failed && std::string(up.reason()) == "no way there");
    // A low box (0.6 m) cannot be stepped onto: walked around instead.
    const Obstacle crate = box(30.0, 10.3, 30.0, 0.5f, 0.3f, 5.0f);
    const Obstacle obstacles[] = {crate};
    NavMap flat_map;
    build(flat_map, ground(0, 0, 0, 0, flat), obstacles, 0, 0, 0, 0);
    PathSearch over(flat_map, {25.0, 10.0, 30.0}, {35.0, 10.0, 30.0});
    GIANT_CHECK(run(over) == SearchState::found && clear_of(over.path(), crate, 0.3));
    // Ends inside the crate snap out; ends far from ground fail.
    PathSearch snapped(flat_map, {30.0, 10.0, 30.0}, {40.0, 10.0, 30.0});
    GIANT_CHECK(run(snapped) == SearchState::found);
    GIANT_CHECK(std::fabs(snapped.path().front().x - 30.0) > 0.8);
    PathSearch inside(flat_map, {10.0, 10.0, 10.0}, {30.0, 10.0, 30.0}, {0.2f});
    GIANT_CHECK(run(inside) == SearchState::failed);
    PathSearch bad(flat_map, {std::nan(""), 0.0, 0.0}, {30.0, 10.0, 30.0});
    GIANT_CHECK(bad.advance(10) == SearchState::failed);
}

GIANT_TEST("nav: a search across tiles waits for each missing one, then goes on; budgets bound each slice") {
    const world::Terrain t = ground(0, 3, 0, 0, flat);
    NavMap map;
    build(map, t, {}, 0, 0, 0, 0);
    PathSearch s(map, {10.0, 10.0, 30.0}, {230.0, 10.0, 30.0});
    int built = 1;
    int slices = 0;
    while (s.state() != SearchState::found && s.state() != SearchState::failed && slices < 1000) {
        const SearchState st = s.advance(500);
        ++slices;
        GIANT_CHECK(s.nodes() <= 500u * static_cast<u32>(slices));
        if (st == SearchState::missing) {
            const world::CellCoord c = s.missing_tile();
            GIANT_CHECK(map.find(c) == nullptr);
            map.set_tile(build_tile(c, t, {}, map.settings()));
            ++built;
        }
    }
    GIANT_CHECK(s.state() == SearchState::found);
    GIANT_CHECK(built == 3); // the two cells on the way not built yet (the end's first), no more
    GIANT_CHECK(s.path().size() == 2 && std::fabs(s.length() - 220.0) < 0.6);
    // The end in a tile not built: waits for that one first.
    PathSearch far(map, {10.0, 10.0, 30.0}, {600.0, 10.0, 30.0});
    GIANT_CHECK(far.advance(10) == SearchState::missing && far.missing_tile() == (world::CellCoord{9, 0}));
    // Too far: gives up after max_nodes samples.
    SearchLimits small;
    small.max_nodes = 200;
    PathSearch tired(map, {10.0, 10.0, 30.0}, {230.0, 10.0, 60.0}, small);
    for (int k = 0; k < 100 && tired.state() != SearchState::failed && tired.state() != SearchState::found; ++k) {
        tired.advance(64);
    }
    GIANT_CHECK(tired.state() == SearchState::failed);
}

GIANT_TEST("nav: enclosed, there is no way out; a gap lets it through") {
    const world::Terrain f = ground(0, 0, 0, 0, flat);
    // A pen of four walls around (32, 32), 8 m a side.
    std::vector<Obstacle> pen = {box(28.0, 11.0, 32.0, 0.2f, 1.0f, 4.2f), box(36.0, 11.0, 32.0, 0.2f, 1.0f, 4.2f),
                                 box(32.0, 11.0, 28.0, 4.2f, 1.0f, 0.2f), box(32.0, 11.0, 36.0, 4.2f, 1.0f, 0.2f)};
    NavMap closed;
    build(closed, f, pen, 0, 0, 0, 0);
    PathSearch out(closed, {32.0, 10.0, 32.0}, {10.0, 10.0, 10.0});
    GIANT_CHECK(run(out) == SearchState::failed);
    // A 2 m gate in the south wall.
    pen[2] = box(29.4, 11.0, 28.0, 1.6f, 1.0f, 0.2f);
    pen.push_back(box(34.6, 11.0, 28.0, 1.6f, 1.0f, 0.2f));
    NavMap gated;
    build(gated, f, pen, 0, 0, 0, 0);
    PathSearch through(gated, {32.0, 10.0, 32.0}, {10.0, 10.0, 10.0});
    GIANT_CHECK(run(through) == SearchState::found);
    bool via_gate = false;
    for (const DVec3& p : through.path()) via_gate = via_gate || (std::fabs(p.z - 28.0) < 1.5 && std::fabs(p.x - 32.0) < 1.2);
    for (usize k = 1; k < through.path().size(); ++k) {
        const DVec3 a = through.path()[k - 1], b = through.path()[k];
        if ((a.z - 28.0) * (b.z - 28.0) < 0.0) {
            const double x = a.x + (b.x - a.x) * (28.0 - a.z) / (b.z - a.z);
            via_gate = via_gate || std::fabs(x - 32.0) < 1.0;
        }
    }
    GIANT_CHECK(via_gate);
}

GIANT_TEST("nav: with no terrain, a floor is the ground; walls on it block, a crate on it is stood on") {
    const world::Terrain t; // none
    const Obstacle floor = box(32.0, -0.5, 32.0, 20.0f, 0.5f, 20.0f);   // top at 0
    const Obstacle wall = box(32.0, 1.5, 32.0, 0.25f, 1.5f, 6.0f);      // on it
    const Obstacle slab = box(20.0, 0.1, 20.0, 1.0f, 0.1f, 1.0f);       // 0.2 m high
    const Obstacle under = box(50.0, -3.0, 50.0, 2.0f, 0.5f, 2.0f);     // a lower box, under the floor
    const Obstacle obstacles[] = {floor, wall, slab, under};
    const NavTile tile = build_tile({0, 0}, t, obstacles, {});
    NavMap map;
    map.set_tile(tile);
    GIANT_CHECK(map.at(15.0, 40.0).walkable && std::fabs(map.at(15.0, 40.0).height) < 1e-4f);
    GIANT_CHECK(!map.at(32.0, 32.0).walkable);           // the wall
    GIANT_CHECK(map.at(20.0, 20.0).walkable && std::fabs(map.at(20.0, 20.0).height - 0.2f) < 1e-4f); // the slab
    GIANT_CHECK(map.at(50.0, 50.0).walkable && std::fabs(map.at(50.0, 50.0).height) < 1e-4f); // the floor, not what is under it
    GIANT_CHECK(!map.at(5.0, 5.0).walkable && std::isnan(map.at(5.0, 5.0).height)); // off the floor
    PathSearch s(map, {24.0, 0.0, 32.0}, {40.0, 0.0, 32.0});
    for (int k = 0; k < 100 && s.state() == SearchState::searching; ++k) s.advance(1u << 16);
    GIANT_CHECK(s.state() == SearchState::found);
    GIANT_CHECK(clear_of(s.path(), wall, 0.3));
}

GIANT_TEST("nav: a tile rebuilt mid-search starts it again; a long path is made over calls, each bounded") {
    const world::Terrain t = ground(0, 2, 0, 0, flat);
    NavMap map;
    build(map, t, {}, 0, 2, 0, 0);
    PathSearch s(map, {60.0, 0.0, 32.0}, {190.0, 0.0, 32.0});
    s.advance(80);
    GIANT_CHECK(s.state() == SearchState::searching);
    // A wall across the start's tile now: the search must not walk through it.
    const Obstacle wall = box(62.0, 11.0, 32.0, 0.5f, 2.0f, 40.0f);
    const Obstacle walls[] = {wall};
    map.set_tile(build_tile({0, 0}, t, walls, map.settings()));
    GIANT_CHECK(run(s) == SearchState::failed); // the wall spans the tile (no way round inside the map)
    GIANT_CHECK(s.restarts() == 1);

    // A zig-zag of walls: a long path, made in slices no bigger than the budget (and one line's look).
    std::vector<Obstacle> zig;
    for (int k = 0; k < 9; ++k) {
        const double x = 8.0 + k * 20.0;
        zig.push_back(box(x, 11.0, k % 2 == 0 ? 26.0 : 38.0, 0.5f, 2.0f, 26.0f));
    }
    NavMap m2;
    build(m2, t, zig, 0, 2, 0, 0);
    PathSearch z(m2, {2.0, 0.0, 32.0}, {190.0, 0.0, 32.0});
    u64 worst = 0;
    for (int k = 0; k < 100000 && (z.state() == SearchState::searching); ++k) {
        const u64 before = z.work();
        z.advance(256);
        worst = std::max(worst, z.work() - before);
    }
    GIANT_CHECK(z.state() == SearchState::found);
    GIANT_CHECK(z.path().size() >= 10);
    GIANT_CHECK(worst <= 256 + 2 * 64 + 2);
}

GIANT_TEST("nav: far-off places fail; stacked low things are stood on whatever their order; an end waits for tiles within reach") {
    const world::Terrain t = ground(0, 1, 0, 0, flat);
    NavMap map;
    build(map, t, {}, 0, 0, 0, 0);
    PathSearch far(map, {1e20, 0.0, 0.0}, {10.0, 0.0, 10.0});
    GIANT_CHECK(far.advance(100) == SearchState::failed);

    const Obstacle a = box(20.0, 10.15, 20.0, 1.0f, 0.15f, 1.0f); // 10 to 10.3
    const Obstacle b = box(20.0, 10.5, 20.0, 1.0f, 0.15f, 1.0f);  // 10.35 to 10.65
    const Obstacle ab[] = {a, b};
    const Obstacle ba[] = {b, a};
    const NavTile t1 = build_tile({0, 0}, t, ab, {});
    const NavTile t2 = build_tile({0, 0}, t, ba, {});
    const usize k = 40 * k_nav_side + 40; // (20.25, 20.25)
    GIANT_CHECK(std::fabs(t1.height[k] - 10.65f) < 1e-4f && t1.height[k] == t2.height[k] && t1.walkable == t2.walkable);

    // An end at the edge of the built tile: the tile beyond is waited for.
    NavMap edge;
    edge.set_tile(build_tile({0, 0}, t, {}, {}));
    PathSearch s(edge, {63.5, 0.0, 30.0}, {10.0, 0.0, 30.0});
    GIANT_CHECK(s.advance(100) == SearchState::missing);
    GIANT_CHECK(s.missing_tile() == (world::CellCoord{1, 0}) || s.missing_tile().z != 0);
}

GIANT_TEST("nav: a search spent on max_nodes fails as too far, though one call's budget ran out first") {
    const world::Terrain t = ground(0, 0, 0, 0, flat);
    // The end walled in.
    const Obstacle ring[] = {box(40.0, 11.0, 36.0, 4.0f, 2.0f, 0.3f), box(40.0, 11.0, 44.0, 4.0f, 2.0f, 0.3f),
                             box(36.0, 11.0, 40.0, 0.3f, 2.0f, 4.0f), box(44.0, 11.0, 40.0, 0.3f, 2.0f, 4.0f)};
    NavMap map;
    build(map, t, ring, 0, 0, 0, 0);
    SearchLimits limits;
    limits.max_nodes = 5000;
    PathSearch s(map, {10.0, 0.0, 10.0}, {40.0, 0.0, 40.0}, limits);
    GIANT_CHECK(s.advance(5000) == SearchState::searching); // the snaps took some of it
    while (s.state() == SearchState::searching) s.advance(5000);
    GIANT_CHECK(s.state() == SearchState::failed && std::string(s.reason()).find("too far") != std::string::npos);
}
