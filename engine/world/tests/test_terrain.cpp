// The terrain: tiles by cell, the surface between samples, changes, text.
#include "giant/world/scene.h"

#include "giant_test.h"

#include <chrono>
#include <cmath>
#include <string>

using namespace giant;
using namespace giant::world;

namespace {
bool contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

// A tile whose height is a + b * i + c * j at sample (i, j) of `cell`, in
// whole-world sample numbers (so neighbouring tiles continue the slope).
Terrain::Heights slope(const Terrain& t, CellCoord cell, float a, float b, float c) {
    const i64 n = t.samples();
    Terrain::Heights h(static_cast<usize>(n * n));
    for (i64 j = 0; j < n; ++j) {
        for (i64 i = 0; i < n; ++i) {
            h[static_cast<usize>(j * n + i)] =
                a + b * static_cast<float>(cell.x * n + i) + c * static_cast<float>(cell.z * n + j);
        }
    }
    return h;
}

std::string load_error(const std::string& text) {
    Scene s;
    const auto e = load_scene(text, s);
    return e ? e->message : std::string();
}

// A scene file with one terrain tile of 8 samples a side, rows given.
std::string with_rows(const std::string& rows, const char* extra = "") {
    return std::string(R"({"format": "giant-scene", "version": 1, "terrain": {"samples": 8,)") + extra +
           R"( "tiles": [{"cell": [0, 0], "heights": )" + rows + "}]}}";
}
std::string rows_of(const char* row, int count = 8) {
    std::string out = "[";
    for (int i = 0; i < count; ++i) out += std::string(i ? ", " : "") + "\"" + row + "\"";
    return out + "]";
}
} // namespace

GIANT_TEST("terrain: tiles by cell, sample spacing set only while empty") {
    Terrain t;
    GIANT_CHECK(t.empty() && t.samples() == k_default_terrain_samples && t.spacing() == 1.0);
    GIANT_CHECK(!t.set_samples(48) && !t.set_samples(4) && !t.set_samples(256)); // not a choice
    GIANT_CHECK(t.set_samples(16) && t.spacing() == 4.0);
    t.set({2, -3}, Terrain::Heights(16 * 16, 5.0f));
    GIANT_CHECK(t.size() == 1 && t.find({2, -3}) != nullptr && t.find({2, 3}) == nullptr);
    GIANT_CHECK(!t.set_samples(32) && t.samples() == 16); // has tiles
    GIANT_CHECK(t.set_samples(16));                       // the same is fine
    GIANT_CHECK(t.remove({2, -3}).has_value() && !t.remove({2, -3}).has_value());
    GIANT_CHECK(t.set_samples(32));
}

GIANT_TEST("terrain: the surface is the mesh's, and continues into neighbours") {
    Terrain t;
    t.set_samples(8); // 8 m apart
    const CellCoord a{0, 0};
    const CellCoord b{1, 0};
    t.set(a, slope(t, a, 10.0f, 0.5f, 0.25f));
    // A plane: the surface is the same plane everywhere on the tile, also
    // between its last sample and the first of the neighbour...
    t.set(b, slope(t, b, 10.0f, 0.5f, 0.25f));
    for (const double x : {0.0, 3.0, 8.0, 59.9, 60.0, 63.99, 64.0, 100.0}) {
        for (const double z : {0.0, 7.0, 33.3, 55.9}) { // past 56 m: row 7 and the missing (0, 1)
            const auto h = t.height_at(x, z);
            GIANT_CHECK(h.has_value());
            GIANT_CHECK(std::fabs(*h - (10.0 + 0.5 * x / 8.0 + 0.25 * z / 8.0)) < 1e-4);
        }
    }
    GIANT_CHECK(!t.height_at(-0.5, 10.0).has_value()); // cell (-1, 0): no tile
    GIANT_CHECK(!t.height_at(10.0, 64.5).has_value()); // cell (0, 1)
    // On the far edge with no neighbour there: the tile's edge, as drawn.
    GIANT_CHECK(std::fabs(*t.height_at(10.0, 64.0) - *t.height_at(10.0, 63.0)) < 1e-6);

    // ...and without the neighbour, the tile's own edge stands in for it.
    t.remove(b);
    const double edge = *t.height_at(56.0, 0.0); // the last sample of row 0
    GIANT_CHECK(std::fabs(*t.height_at(63.0, 0.0) - edge) < 1e-6);

    // Inside a square, the two triangles split on the (0, 0)-(1, 1) diagonal.
    Terrain one;
    one.set_samples(8);
    Terrain::Heights h(64, 0.0f);
    h[1 * 8 + 1] = 8.0f; // sample (1, 1)
    one.set(a, h);
    GIANT_CHECK(std::fabs(*one.height_at(4.0, 4.0) - 4.0) < 1e-6); // on the diagonal: half way
    GIANT_CHECK(std::fabs(*one.height_at(6.0, 2.0) - 2.0) < 1e-6); // triangle (0,0) (1,0) (1,1)
    GIANT_CHECK(std::fabs(*one.height_at(2.0, 6.0) - 2.0) < 1e-6); // triangle (0,0) (0,1) (1,1)
    GIANT_CHECK(std::fabs(*one.height_at(8.0, 8.0) - 8.0) < 1e-6);
    GIANT_CHECK(std::fabs(*one.height_at(-0.0, 0.0) - 0.0) < 1e-6);

    // Sample lookups by world sample number, across tiles and far out.
    GIANT_CHECK(one.sample(1, 1) == 8.0f);
    GIANT_CHECK(one.sample(8, 1) == 0.0f && one.sample(8, 2) == 0.0f); // cell (1, 0) missing: (7, j)
    GIANT_CHECK(one.sample(8, 8) == 0.0f);         // (7, 7), past both edges
    GIANT_CHECK(!one.sample(9, 1).has_value());    // two samples into a missing cell
    GIANT_CHECK(!one.sample(-1, 1).has_value());   // before the tile: nothing to repeat
    GIANT_CHECK(!one.sample(-8LL * 1000000000LL, 1).has_value()); // past what i32 cells hold
    // A tile touching only at a corner does not pull the other's corner to it.
    Terrain corner;
    corner.set_samples(8);
    corner.set({0, 0}, Terrain::Heights(64, 0.0f));
    corner.set({1, 1}, Terrain::Heights(64, 100.0f));
    const CellCoord home{0, 0};
    GIANT_CHECK(corner.sample(8, 8) == 100.0f && corner.sample(8, 8, &home) == 0.0f);
    GIANT_CHECK(std::fabs(*corner.height_at(63.9, 63.9)) < 1e-6);
    GIANT_CHECK(std::fabs(*corner.height_at(64.0, 64.0) - 100.0) < 1e-6); // tile (1, 1)'s own corner
    corner.set({1, 0}, Terrain::Heights(64, 50.0f)); // now they share edges through (1, 0)
    GIANT_CHECK(corner.sample(8, 8, &home) == 100.0f);
    // The same point seen from either side of a corner, whichever tiles are there.
    one.set({1, 0}, Terrain::Heights(64, 3.0f));
    GIANT_CHECK(one.sample(8, 8) == 3.0f); // (1, 1) missing; (8, 7) is in (1, 0)
}

GIANT_TEST("terrain: changes are tracked, a copy is a new terrain") {
    Terrain t;
    t.set_samples(8);
    GIANT_CHECK(t.take_changes().all); // new
    GIANT_CHECK(t.take_changes().tiles.empty() && !t.take_changes().all);
    t.set({1, 1}, Terrain::Heights(64, 0.0f));
    t.modify({1, 1})->at(3) = 2.0f;
    GIANT_CHECK(t.modify({5, 5}) == nullptr);
    t.remove({1, 1});
    t.remove({7, 7}); // no tile: no change
    const TerrainChanges c = t.take_changes();
    GIANT_CHECK(!c.all && c.tiles.size() == 3 && c.tiles[0] == (CellCoord{1, 1}));

    t.set({0, 0}, Terrain::Heights(64, 1.0f));
    Terrain copy = t;
    GIANT_CHECK(copy.identity() != t.identity() && copy.take_changes().all && copy.size() == 1);
    Terrain moved = std::move(copy);
    GIANT_CHECK(moved.size() == 1 && copy.empty() && moved.take_changes().all);

    // A scene copy carries its terrain (and so a new one).
    Scene s;
    s.terrain.set_samples(8);
    s.terrain.set({0, 0}, Terrain::Heights(64, 3.0f));
    const Scene sc = s;
    GIANT_CHECK(sc.terrain.size() == 1 && sc.terrain.identity() != s.terrain.identity());
}

GIANT_TEST("terrain: each tile has a version, new on every change, kept by copies and restores") {
    Terrain t;
    t.set_samples(8);
    GIANT_CHECK(t.version({0, 0}) == 0);
    t.set({0, 0}, Terrain::Heights(64, 1.0f));
    const u64 v1 = t.version({0, 0});
    GIANT_CHECK(v1 != 0);
    t.modify({0, 0})->at(3) = 2.0f;
    const u64 v2 = t.version({0, 0});
    GIANT_CHECK(v2 != v1);
    Terrain copy = t;
    GIANT_CHECK(copy.version({0, 0}) == v2 && copy.identity() != t.identity());
    t.set({0, 0}, Terrain::Heights(64, 1.0f)); // same heights as v1, but set again: new
    GIANT_CHECK(t.version({0, 0}) != v1 && t.version({0, 0}) != v2);
    t.restore({0, 0}, Terrain::Heights(64, 1.0f), v1);
    GIANT_CHECK(t.version({0, 0}) == v1);
    Terrain other;
    other.set_samples(8);
    other.set({5, 5}, Terrain::Heights(64, 0.0f));
    GIANT_CHECK(other.version({5, 5}) != v1 && other.version({5, 5}) != v2); // unique across terrains
    GIANT_CHECK(t.remove({0, 0}) && t.version({0, 0}) == 0);
    Terrain moved = std::move(copy);
    GIANT_CHECK(moved.version({0, 0}) == v2 && copy.version({0, 0}) == 0);
}

GIANT_TEST("terrain: saved with the scene as rows of text, exactly") {
    Scene s;
    s.terrain.set_samples(8);
    s.terrain.material.base_color = {0.1f, 0.2f, 0.3f};
    s.terrain.material.roughness = 0.5f;
    Terrain::Heights h(64);
    for (usize i = 0; i < h.size(); ++i) h[i] = 0.1f * static_cast<float>(i) - 3.3f; // not short in binary
    h[5] = -10000.0f;
    h[6] = 1e-7f;
    s.terrain.set({-1, 2}, h);
    s.terrain.set({0, 0}, Terrain::Heights(64, 0.0f));
    const std::string text = save_scene(s);
    GIANT_CHECK(contains(text, "\"terrain\"") && contains(text, "\"samples\": 8"));
    // Tiles go by z then x: (0, 0) before (-1, 2).
    GIANT_CHECK(text.find("\"0 0 0 0 0 0 0 0\"") < text.find("-10000"));

    Scene back;
    GIANT_CHECK(!load_scene(text, back).has_value());
    GIANT_CHECK(back.terrain.samples() == 8 && back.terrain.size() == 2);
    GIANT_CHECK(*back.terrain.find({-1, 2}) == h); // bit for bit
    GIANT_CHECK(back.terrain.material == s.terrain.material);
    GIANT_CHECK(save_scene(back) == text);

    // No terrain, default look: no key at all.
    GIANT_CHECK(!contains(save_scene(Scene{}), "terrain"));
}

GIANT_TEST("terrain: layers save and load with their images, bands and tiling; bad ones are refused") {
    Scene s;
    TerrainLayer grass;
    grass.base_color = {1.0f, 1.0f, 1.0f};
    grass.maps.base_color = "ground/grass.png";
    grass.maps.normal = "ground/grass_n.png";
    grass.maps.normal_green_down = true;
    grass.tiling = 2.5f;
    TerrainLayer rock;
    rock.min_slope = 35.0f;
    rock.slope_blend = 8.0f;
    rock.maps.roughness_metal = "ground/rock_rm.png";
    TerrainLayer snow;
    snow.min_height = 1500.0f;
    snow.max_height = 20000.0f;
    snow.height_blend = 60.0f;
    snow.roughness = 0.4f;
    s.terrain.material.layers = {grass, rock, snow};
    const std::string text = save_scene(s);
    GIANT_CHECK(contains(text, "\"layers\"") && contains(text, "ground/grass.png"));
    Scene back;
    GIANT_CHECK(!load_scene(text, back).has_value());
    GIANT_CHECK(back.terrain.material == s.terrain.material);
    GIANT_CHECK(save_scene(back) == text);
    // A world's terrain settings carry them too.
    WorldIndex world;
    world.cells = {{0, 0}};
    world.terrain_material = s.terrain.material;
    WorldIndex world_back;
    GIANT_CHECK(!load_world_index(save_world_index(world), world_back).has_value());
    GIANT_CHECK(world_back.terrain_material == s.terrain.material);

    const auto refused = [](const std::string& layers, const char* why) {
        return contains(load_error(R"({"format": "giant-scene", "version": 1, "terrain": {"samples": 8,
            "material": {"base_color": [0.2, 0.2, 0.2], "roughness": 0.9, "layers": )" + layers + "}}}"),
                        why);
    };
    GIANT_CHECK(refused(R"([{"max_slope": 95}])", "max_slope"));
    GIANT_CHECK(refused(R"([{"tiling": 0}])", "tiling"));
    GIANT_CHECK(refused(R"([{"colour": [1, 1, 1]}])", "colour"));
    GIANT_CHECK(refused(R"({"a": 1})", "at most 8"));
    GIANT_CHECK(refused("[{}, {}, {}, {}, {}, {}, {}, {}, {}]", "at most 8"));
    GIANT_CHECK(refused(R"([{"maps": {"base_color": 3}}])", "base_color"));
    GIANT_CHECK(refused(R"([{"maps": {"occlusion": "ao.png"}}])", "occlusion"));
}

GIANT_TEST("terrain: loading is strict") {
    const std::string good = rows_of("0 1 2 3 4 5 6 7");
    GIANT_CHECK(load_error(with_rows(good)).empty());
    GIANT_CHECK(load_error(with_rows(good, R"( "material": {"base_color": [0.1, 0.1, 0.1], "roughness": 1},)")).empty());
    GIANT_CHECK(contains(load_error(with_rows(rows_of("0 1 2 3 4 5 6"))), "must hold 8 heights"));
    GIANT_CHECK(contains(load_error(with_rows(rows_of("0 1 2 3 4 5 6 7 8"))), "must hold 8 heights"));
    GIANT_CHECK(contains(load_error(with_rows(rows_of("0 1 2 3 4 5 6 7", 7))), "must be 8 rows"));
    GIANT_CHECK(contains(load_error(with_rows(rows_of("0 1 2 3 4 5 6 x"))), "terrain.tiles[0].heights[0]"));
    GIANT_CHECK(contains(load_error(with_rows(rows_of("0 1 2 3 4 5 6 7,"))), "one space apart"));
    GIANT_CHECK(contains(load_error(with_rows(rows_of("0 1 2 3 4 5 6 10001"))), "-10000 to 10000"));
    GIANT_CHECK(contains(load_error(with_rows(rows_of("0 1 2 3 4 5 6 nan"))), "-10000 to 10000"));
    GIANT_CHECK(contains(load_error(with_rows(rows_of("0 1 2 3 4 5 6 inf"))), "-10000 to 10000"));
    GIANT_CHECK(contains(load_error(with_rows("[1, 2, 3, 4, 5, 6, 7, 8]")), "string of numbers"));
    GIANT_CHECK(contains(load_error(with_rows(good, R"( "colour": 1,)")), "terrain.colour is not a known key"));
    GIANT_CHECK(contains(load_error(R"({"format": "giant-scene", "version": 1, "terrain": {"samples": 48}})"),
                         "must be 8, 16, 32, 64 or 128"));
    GIANT_CHECK(contains(load_error(R"({"format": "giant-scene", "version": 1, "terrain": {"samples": 8, "tiles": [)"
                                    R"({"cell": [0, 0], "heights": )" + good + R"(}, {"cell": [0, 0], "heights": )" +
                                    good + "}]}}"),
                         "is listed twice"));
    GIANT_CHECK(contains(load_error(R"({"format": "giant-scene", "version": 1, "terrain": {"samples": 8, "tiles": [)"
                                    R"({"cell": [0.5, 0], "heights": )" + good + "}]}}"),
                         "terrain.tiles[0].cell must be [x, z]"));
    // Extra spaces are fine (hand edits).
    GIANT_CHECK(load_error(with_rows(rows_of("  0  1 2 3 4 5 6   7 "))).empty());
}

GIANT_TEST("terrain: hills are smooth, bounded and the same every time") {
    HillsSettings h;
    h.base = 100.0f;
    h.amplitude = 30.0f;
    h.size = 200.0f;
    h.seed = 7;
    float lo = 1e9f, hi = -1e9f;
    float last = hills_height(h, 0.0, 0.0);
    float steepest = 0.0f;
    for (int i = 1; i < 4000; ++i) {
        const float v = hills_height(h, i * 1.0, i * 0.37);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
        steepest = std::max(steepest, std::fabs(v - last));
        last = v;
    }
    GIANT_CHECK(lo >= 70.0f && hi <= 130.0f && hi - lo > 15.0f); // within base +- amplitude, and hilly
    GIANT_CHECK(steepest < 3.0f);                                 // no cliffs between metres
    GIANT_CHECK(hills_height(h, 1234.5, -987.25) == hills_height(h, 1234.5, -987.25));
    HillsSettings other = h;
    other.seed = 8;
    GIANT_CHECK(hills_height(h, 500.0, 500.0) != hills_height(other, 500.0, 500.0));
}

// --- Far ground ---------------------------------------------------------------

#include "giant/world/far_ground.h"

#include <limits>

GIANT_TEST("far ground: chunk sizes, and the chunk of a cell, below zero too") {
    GIANT_CHECK(far_tile_size(0) == 64.0 && far_tile_size(1) == 512.0 && far_tile_size(2) == 4096.0);
    GIANT_CHECK(far_spacing(1) == 16.0 && far_spacing(2) == 128.0);
    GIANT_CHECK((far_chunk_of_cell({7, 8}, 1) == CellCoord{0, 1}));
    GIANT_CHECK((far_chunk_of_cell({-1, -8}, 1) == CellCoord{-1, -1}));
    GIANT_CHECK((far_chunk_of_cell({-9, 63}, 1) == CellCoord{-2, 7}));
    GIANT_CHECK((far_chunk_of_cell({-1, 64}, 2) == CellCoord{-1, 1}));
    GIANT_CHECK((far_parent({-1, 8}) == CellCoord{-1, 1}));
    GIANT_CHECK(far_chunk_file_name({-3, 12}) == "-3_12.ground.json");
}

GIANT_TEST("far ground: a chunk takes the finer tile's heights at its own sample points") {
    const usize n = k_far_samples;
    Terrain::Heights chunk(n * n, 7.0f);
    Terrain::Heights tile(16 * 16);
    for (usize k = 0; k < tile.size(); ++k) tile[k] = static_cast<float>(k);
    // Cell (9, -6) is (1, 2) in chunk (1, -1): its 4 x 4 block starts at sample (4, 8).
    far_fill(1, {1, -1}, {9, -6}, &tile, 16, chunk);
    for (usize b = 0; b < 4; ++b) {
        for (usize a = 0; a < 4; ++a) {
            GIANT_CHECK(chunk[(8 + b) * n + 4 + a] == tile[b * 4 * 16 + a * 4]); // every 4th: 16 m apart
        }
    }
    GIANT_CHECK(chunk[0] == 7.0f && chunk[8 * n + 3] == 7.0f && chunk[12 * n + 4] == 7.0f); // the rest untouched
    far_fill(1, {1, -1}, {9, -6}, nullptr, 32, chunk);
    GIANT_CHECK(std::isnan(chunk[8 * n + 4]) && std::isnan(chunk[11 * n + 7]) && chunk[8 * n + 8] == 7.0f);
    // Level 2 from a level-1 chunk: every 8th of its samples (128 m).
    Terrain::Heights l1(n * n);
    for (usize k = 0; k < l1.size(); ++k) l1[k] = static_cast<float>(k) * 0.5f;
    Terrain::Heights l2(n * n, 0.0f);
    far_fill(2, {0, 0}, {7, 7}, &l1, k_far_samples, l2);
    GIANT_CHECK(l2[28 * n + 28] == l1[0] && l2[31 * n + 29] == l1[24 * n + 8]);
}

GIANT_TEST("far ground: chunks as text, exact, with holes, read strictly") {
    const usize n = k_far_samples;
    Terrain::Heights h(n * n);
    for (usize k = 0; k < h.size(); ++k) h[k] = 0.1f * static_cast<float>(k) - 37.3f;
    h[5] = std::numeric_limits<float>::quiet_NaN();
    h[n * n - 1] = -k_max_terrain_height;
    const std::string text = save_far_chunk(2, {-4, 9}, h);
    GIANT_CHECK(contains(text, "\"giant-ground\"") && contains(text, " - "));
    Terrain::Heights back;
    GIANT_CHECK(!load_far_chunk(text, 2, {-4, 9}, back).has_value());
    GIANT_CHECK(back.size() == h.size() && std::isnan(back[5]));
    bool same = true;
    for (usize k = 0; k < h.size(); ++k) same = same && (k == 5 || back[k] == h[k]); // bit for bit
    GIANT_CHECK(same);

    Terrain::Heights untouched{1.0f};
    const auto wrong_level = load_far_chunk(text, 1, {-4, 9}, untouched);
    GIANT_CHECK(wrong_level.has_value() && contains(wrong_level->message, "level"));
    const auto wrong_chunk = load_far_chunk(text, 2, {-4, 8}, untouched);
    GIANT_CHECK(wrong_chunk.has_value() && contains(wrong_chunk->message, "chunk"));
    std::string short_row = text;
    short_row.replace(short_row.find(" - "), 3, " ");
    GIANT_CHECK(load_far_chunk(short_row, 2, {-4, 9}, untouched).has_value());
    std::string too_high = text;
    too_high.replace(too_high.find(" - "), 3, " 10001 ");
    GIANT_CHECK(load_far_chunk(too_high, 2, {-4, 9}, untouched).has_value());
    std::string junk = text;
    junk.replace(junk.find(" - "), 3, " -x ");
    GIANT_CHECK(load_far_chunk(junk, 2, {-4, 9}, untouched).has_value());
    GIANT_CHECK(untouched.size() == 1 && untouched[0] == 1.0f); // left alone on failure
}

GIANT_TEST("terrain: bigger tiles, with holes, for the far ground") {
    Terrain t(512.0, true);
    GIANT_CHECK(t.set_samples(32) && t.tile_size() == 512.0 && t.spacing() == 16.0);
    GIANT_CHECK((t.tile_of(-1.0, 1023.0) == CellCoord{-1, 1}));
    Terrain::Heights h(32 * 32, 5.0f);
    h[0] = std::numeric_limits<float>::quiet_NaN();
    t.set({1, 0}, h);
    GIANT_CHECK(t.height_at(512.0 + 40.0, 40.0).value() == 5.0);
    GIANT_CHECK(!t.height_at(-10.0, 40.0).has_value());
    const Terrain copy = t;
    GIANT_CHECK(copy.tile_size() == 512.0 && copy.find({1, 0}) != nullptr);
}

// --- Brushes and height maps ------------------------------------------------------

#include "giant/world/height_map.h"
#include "giant/world/terrain_brush.h"

GIANT_TEST("terrain brush: full inside, easing to nothing at the rim") {
    Brush b;
    b.radius = 10.0;
    b.falloff = 0.5f;
    GIANT_CHECK(brush_weight(b, 0.0) == 1.0f && brush_weight(b, 5.0) == 1.0f);
    GIANT_CHECK(brush_weight(b, 7.5) == 0.5f && brush_weight(b, 10.0) == 0.0f && brush_weight(b, 11.0) == 0.0f);
    GIANT_CHECK(brush_weight(b, 6.0) > brush_weight(b, 9.0));
    b.falloff = 0.0f;
    GIANT_CHECK(brush_weight(b, 9.99) == 1.0f);
}

GIANT_TEST("terrain brush: raise, lower and flatten change only what the brush reaches, across tiles") {
    Terrain t;
    t.set_samples(16); // 4 m apart
    t.set({0, 0}, Terrain::Heights(256, 1.0f));
    t.set({1, 0}, Terrain::Heights(256, 1.0f));
    Brush b;
    b.radius = 10.0;
    b.strength = 2.0f;
    b.falloff = 0.0f;
    std::vector<CellCoord> changed;
    apply_brush(t, b, 64.0, 8.0, changed); // on the border of the two tiles
    GIANT_CHECK(changed.size() == 2);
    GIANT_CHECK(t.height_at(64.0, 8.0).value() == 3.0 && t.height_at(56.0, 8.0).value() == 3.0);
    GIANT_CHECK(t.height_at(76.0, 8.0).value() == 1.0); // 12 m away: out of reach
    GIANT_CHECK(t.find({0, 0})->at(0) == 1.0f);
    b.op = BrushOp::lower;
    b.strength = 0.5f;
    changed.clear();
    apply_brush(t, b, 64.0, 8.0, changed);
    GIANT_CHECK(t.height_at(64.0, 8.0).value() == 2.5);
    b.op = BrushOp::flatten;
    b.strength = 1.0f;
    b.height = -4.0f;
    apply_brush(t, b, 64.0, 8.0, changed);
    GIANT_CHECK(t.height_at(64.0, 8.0).value() == -4.0 && t.height_at(76.0, 8.0).value() == 1.0);
    // No tile, no change; never past the height limit.
    changed.clear();
    apply_brush(t, b, 500.0, 500.0, changed);
    GIANT_CHECK(changed.empty());
    b.op = BrushOp::raise;
    b.strength = 1e6f;
    apply_brush(t, b, 20.0, 20.0, changed);
    GIANT_CHECK(t.height_at(20.0, 20.0).value() == k_max_terrain_height);
}

GIANT_TEST("terrain brush: smooth evens out a spike, using its neighbours across a tile edge") {
    Terrain t;
    t.set_samples(16);
    t.set({0, 0}, Terrain::Heights(256, 0.0f));
    t.set({1, 0}, Terrain::Heights(256, 0.0f));
    (*t.modify({1, 0}))[0] = 9.0f; // sample (16, 0): on the tile edge
    Brush b;
    b.op = BrushOp::smooth;
    b.radius = 2.0;
    b.strength = 1.0f;
    b.falloff = 0.0f;
    std::vector<CellCoord> changed;
    apply_brush(t, b, 64.0, 0.0, changed); // reaches only that sample (4 m spacing)
    // Its 3 x 3 has 6 samples (row -1 has none): 9 / 6.
    GIANT_CHECK(std::fabs(t.find({1, 0})->at(0) - 1.5f) < 1e-5f);
    GIANT_CHECK(t.find({0, 0})->at(15) == 0.0f); // the neighbours themselves untouched (outside the radius)
}

GIANT_TEST("terrain brush: a stroke has a dab at least every quarter radius") {
    Brush b;
    b.radius = 4.0;
    const GroundPoint points[] = {{0.0, 0.0}, {10.0, 0.0}, {10.0, 0.0}};
    const std::vector<GroundPoint> dabs = brush_dabs(b, points);
    GIANT_CHECK(dabs.size() == 1 + 10 + 1);
    for (usize k = 1; k < dabs.size(); ++k) {
        GIANT_CHECK(std::hypot(dabs[k].x - dabs[k - 1].x, dabs[k].z - dabs[k - 1].z) <= 1.0 + 1e-9);
    }
    GIANT_CHECK(dabs.back().x == 10.0);
    // Over a limit, the stroke is cut short past it (refused by the caller).
    GIANT_CHECK(brush_dabs(b, points, 5).size() == 6);
    GIANT_CHECK(brush_dabs(b, points, 12).size() == 12);
}

GIANT_TEST("terrain brush: a dab that changes no height leaves its tiles as they were") {
    Terrain t;
    t.set_samples(16);
    t.set({0, 0}, Terrain::Heights(256, 2.0f));
    const u64 version = t.version({0, 0});
    std::vector<CellCoord> changed;
    Brush b;
    b.strength = 0.0f; // raise by nothing
    apply_brush(t, b, 30.0, 30.0, changed);
    b.op = BrushOp::smooth; // flat already
    b.strength = 1.0f;
    apply_brush(t, b, 30.0, 30.0, changed);
    b.op = BrushOp::flatten;
    b.height = 2.0f;
    apply_brush(t, b, 30.0, 30.0, changed);
    GIANT_CHECK(changed.empty() && t.version({0, 0}) == version);
    b.height = 3.0f;
    apply_brush(t, b, 30.0, 30.0, changed);
    GIANT_CHECK(changed.size() == 1 && t.version({0, 0}) != version);
}

GIANT_TEST("terrain brush: a large smooth reads only the tiles there are") {
    Terrain t;
    t.set_samples(16);
    Terrain::Heights spike(256, 0.0f);
    spike[8 * 16 + 8] = 9.0f;
    t.set({0, 0}, spike);
    Brush b;
    b.op = BrushOp::smooth;
    b.radius = 1500.0; // over 2 million samples of square, one tile of ground
    b.falloff = 0.0f;
    std::vector<CellCoord> changed;
    const auto start = std::chrono::steady_clock::now();
    apply_brush(t, b, 32.0, 32.0, changed);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    GIANT_CHECK(changed.size() == 1 && t.find({0, 0})->at(8 * 16 + 8) == 1.0f);
    GIANT_CHECK(seconds < 1.0); // was ~3.6 s (debug) reading every sample of the square
}

GIANT_TEST("height maps: 16-bit RAW and PGM read as 0 to 1, bilinear between samples") {
    std::vector<u8> raw = {0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x7F, 0x00, 0x80}; // 2 x 2: 0, 1, ~0.5, ~0.5
    HeightMap m;
    GIANT_CHECK(!read_height_map(raw, ".R16", m).has_value());
    GIANT_CHECK(m.width == 2 && m.height == 2 && m.values[0] == 0.0f && m.values[1] == 1.0f);
    GIANT_CHECK(std::fabs(m.at(0.5, 0.0) - 0.5f) < 1e-6f && m.at(-1.0, 2.0) == m.values[2]);
    const std::string p2 = "P2\n# made by hand\n3 2\n10\n0 5 10\n10 5 0\n";
    GIANT_CHECK(!read_height_map({reinterpret_cast<const u8*>(p2.data()), p2.size()}, ".pgm", m).has_value());
    GIANT_CHECK(m.width == 3 && m.height == 2 && m.values[1] == 0.5f && m.values[3] == 1.0f);
    std::string p5 = "P5 2 2 65535\n";
    for (const int b : {0x00, 0x00, 0xFF, 0xFF, 0x80, 0x00, 0x00, 0x01}) p5.push_back(static_cast<char>(b));
    GIANT_CHECK(!read_height_map({reinterpret_cast<const u8*>(p5.data()), p5.size()}, ".pgm", m).has_value());
    GIANT_CHECK(m.values[1] == 1.0f && m.values[3] == 1.0f / 65535.0f);

    HeightMap untouched;
    std::vector<u8> odd(6, 0);
    GIANT_CHECK(read_height_map(odd, ".raw", untouched).has_value()); // 3 samples: not square
    const std::string short_p5 = "P5 4 4 255\n\x01\x02";
    GIANT_CHECK(read_height_map({reinterpret_cast<const u8*>(short_p5.data()), short_p5.size()}, ".pgm", untouched)
                    .has_value());
    const std::string big = "P2 2 1 10\n0 11\n";
    GIANT_CHECK(read_height_map({reinterpret_cast<const u8*>(big.data()), big.size()}, ".pgm", untouched).has_value());
    GIANT_CHECK(read_height_map(raw, ".png", untouched).has_value());
    // Numbers are separated, and at most 10 digits.
    for (const std::string bad : {"P22 1 10\n0 1\n", "P2 2 1 10\n0000000000005 1\n", "P2 2 1 10\n0 1x\n",
                                  "P2 2 1 10\n01\n"}) {
        GIANT_CHECK(read_height_map({reinterpret_cast<const u8*>(bad.data()), bad.size()}, ".pgm", untouched)
                        .has_value());
    }
    GIANT_CHECK(untouched.width == 0);
}

GIANT_TEST("terrain dent: a crater's bowl with a rim, a footprint stretched along its yaw, across tiles") {
    Dent d;
    d.radius = 8.0;
    d.depth = 2.0f;
    d.rim = 0.25f;
    GIANT_CHECK(dent_offset(d, 0.0, 0.0) == -2.0f);
    GIANT_CHECK(dent_offset(d, 8.0, 0.0) == 0.0f);                        // the bowl's edge
    GIANT_CHECK(std::fabs(dent_offset(d, 10.0, 0.0) - 0.5f) < 1e-5f);      // the rim's top (1.25 radius)
    GIANT_CHECK(dent_offset(d, 12.0, 0.0) == 0.0f && dent_offset(d, 30.0, 0.0) == 0.0f);
    GIANT_CHECK(dent_reach(d) == 12.0);
    Terrain t;
    t.set_samples(32); // 2 m apart
    t.set({0, 0}, Terrain::Heights(1024, 5.0f));
    t.set({1, 0}, Terrain::Heights(1024, 5.0f));
    std::vector<CellCoord> changed;
    apply_dent(t, d, 64.0, 32.0, changed); // on the border of the two tiles
    GIANT_CHECK(changed.size() == 2);
    GIANT_CHECK(std::fabs(t.height_at(64.0, 32.0).value() - 3.0) < 1e-5);
    GIANT_CHECK(std::fabs(t.height_at(54.0, 32.0).value() - 5.5) < 1e-5); // the rim, 10 m west
    GIANT_CHECK(t.height_at(64.0, 50.0).value() == 5.0);                  // 18 m away: untouched
    // A footprint 3 m long (half 1.5) and 1 m wide (half 0.5), its length along +z.
    Dent foot;
    foot.radius = 2.0;
    foot.depth = 1.0f;
    foot.rim = 0.0f;
    foot.stretch = 3.0f;
    foot.yaw = 0.5f * 3.14159265f;
    changed.clear();
    apply_dent(t, foot, 20.0, 20.0, changed);
    GIANT_CHECK(changed.size() == 1 && changed[0] == (CellCoord{0, 0}));
    GIANT_CHECK(t.height_at(20.0, 24.0).value() < 5.0);  // along its length: pressed in
    GIANT_CHECK(t.height_at(24.0, 20.0).value() == 5.0); // as far across: not
    // Bad numbers change nothing.
    changed.clear();
    Dent bad = d;
    bad.depth = NAN;
    apply_dent(t, bad, 64.0, 32.0, changed);
    apply_dent(t, d, INFINITY, 32.0, changed);
    GIANT_CHECK(changed.empty());
}

#include "giant/world/ground_damage.h"

GIANT_TEST("ground damage: dents follow in the terrain; restore puts every tile back with its version") {
    Terrain t;
    t.set_samples(32);
    t.set({0, 0}, Terrain::Heights(1024, 5.0f));
    t.set({1, 0}, Terrain::Heights(1024, 5.0f));
    t.set({3, 0}, Terrain::Heights(1024, 5.0f));
    const u64 v0 = t.version({0, 0}), v1 = t.version({1, 0}), v3 = t.version({3, 0});
    GroundDamage damage;
    Dent d;
    d.radius = 8.0;
    d.depth = 2.0f;
    std::vector<CellCoord> changed;
    damage.dent(t, d, 64.0, 32.0, changed);
    damage.dent(t, d, 66.0, 32.0, changed); // again, deeper
    damage.dent(t, d, 200.0, 32.0, changed); // tile 3
    GIANT_CHECK(damage.tiles() == 3 && damage.dents() == 3);
    GIANT_CHECK(t.height_at(65.0, 32.0).value() < 2.0);
    // A save: the ground as it was meanwhile, then dented again.
    damage.lift(t);
    GIANT_CHECK(t.version({0, 0}) == v0 && t.version({1, 0}) == v1 && t.height_at(65.0, 32.0).value() == 5.0);
    const u64 dented = t.version({1, 0});
    damage.put_back(t);
    // Put back as exactly as it was: the same version (nothing that follows it sees a change).
    GIANT_CHECK(t.height_at(65.0, 32.0).value() < 2.0 && t.version({1, 0}) != v1);
    GIANT_CHECK(t.version({1, 0}) != dented); // (lifted, it was v1)
    damage.lift(t);
    damage.put_back(t);
    const u64 again = t.version({1, 0});
    damage.lift(t);
    damage.put_back(t);
    GIANT_CHECK(t.version({1, 0}) == again);
    // Tile 3 edited otherwise (a sculpt): kept as that made it.
    Terrain::Heights* h = t.modify({3, 0});
    (*h)[0] = 9.0f;
    GIANT_CHECK(damage.restore(t) == 2);
    GIANT_CHECK(t.version({0, 0}) == v0 && t.version({1, 0}) == v1 && t.version({3, 0}) != v3);
    GIANT_CHECK(t.height_at(65.0, 32.0).value() == 5.0 && t.height_at(200.0, 32.0).value() < 5.0);
    GIANT_CHECK(damage.tiles() == 0 && damage.dents() == 0);
    // Out of every tile: nothing kept, no dent counted. Over a tile's
    // corner of the bounds but not its ground: not kept either.
    damage.dent(t, d, 2000.0, 32.0, changed);
    GIANT_CHECK(damage.tiles() == 0 && damage.dents() == 0);
    Dent small;
    small.radius = 2.0;
    small.depth = 1.0f;
    small.rim = 0.0f;
    damage.dent(t, small, 62.0, 32.0, changed); // within tile 0 (reaches x 59 to 65: tile 1 too by its box)
    GIANT_CHECK(damage.tiles() == 1 && damage.dents() == 1);
    damage.restore(t);
}

GIANT_TEST("terrain dent: turned, its length runs from +x toward +z; past the farthest reach, nothing") {
    Terrain t;
    t.set_samples(32);
    t.set({0, 0}, Terrain::Heights(1024, 5.0f));
    Dent foot;
    foot.radius = 2.0;
    foot.depth = 1.0f;
    foot.rim = 0.0f;
    foot.stretch = 4.0f;
    foot.yaw = 0.25f * 3.14159265f; // 45 degrees
    std::vector<CellCoord> changed;
    apply_dent(t, foot, 32.0, 32.0, changed);
    GIANT_CHECK(t.height_at(36.0, 36.0).value() < 5.0);  // along (+x, +z)
    GIANT_CHECK(t.height_at(36.0, 28.0).value() == 5.0); // across it
    Dent huge;
    huge.radius = 1e300;
    GIANT_CHECK(dent_reach(huge) == 0.0);
    huge.radius = INFINITY;
    GIANT_CHECK(dent_offset(huge, 0.0, 0.0) == 0.0f);
    changed.clear();
    huge.radius = 1e300;
    apply_dent(t, huge, 32.0, 32.0, changed);
    GIANT_CHECK(changed.empty());
}

GIANT_TEST("ground damage: keep leaves the dents as the ground, hands back before and after, and forgets them") {
    Terrain t;
    t.set_samples(32);
    t.set({0, 0}, Terrain::Heights(1024, 5.0f));
    t.set({1, 0}, Terrain::Heights(1024, 5.0f));
    t.set({3, 0}, Terrain::Heights(1024, 5.0f));
    const u64 v1 = t.version({1, 0});
    GroundDamage damage;
    Dent d;
    d.radius = 8.0;
    d.depth = 2.0f;
    std::vector<CellCoord> changed;
    damage.dent(t, d, 64.0, 32.0, changed);  // tiles 0 and 1
    damage.dent(t, d, 200.0, 32.0, changed); // tile 3
    // Tile 3 edited otherwise since: not ours to keep.
    t.modify({3, 0})->at(0) = 6.0f;
    const u64 dented = t.version({1, 0});
    const std::vector<GroundDamage::Kept> kept = damage.keep(t);
    GIANT_CHECK(kept.size() == 2 && kept[0].cell == (CellCoord{0, 0}) && kept[1].cell == (CellCoord{1, 0}));
    GIANT_CHECK(kept[1].before_version == v1 && kept[1].after_version == dented && kept[1].before[0] == 5.0f);
    GIANT_CHECK(kept[1].after == *t.find({1, 0}));
    GIANT_CHECK(damage.tiles() == 0 && damage.dents() == 0);
    // Nothing to put back: the dents stay.
    GIANT_CHECK(damage.restore(t) == 0 && t.height_at(65.0, 32.0).value() < 4.0 && t.version({1, 0}) == dented);
    // An undo of it, and a redo: exactly as each was.
    t.restore({1, 0}, kept[1].before, kept[1].before_version);
    GIANT_CHECK(t.version({1, 0}) == v1 && t.height_at(65.0, 32.0).value() == 5.0);
    t.restore({1, 0}, kept[1].after, kept[1].after_version);
    GIANT_CHECK(t.version({1, 0}) == dented);
}

GIANT_TEST("ground damage: a pressed tile is kept aside as a dent's; dents over it go back to it; both lift exactly") {
    Terrain t;
    t.set_samples(32);
    t.set({0, 0}, Terrain::Heights(1024, 5.0f));
    t.set({1, 0}, Terrain::Heights(1024, 5.0f));
    const u64 v0 = t.version({0, 0});
    GroundDamage game;
    GIANT_CHECK(!game.press(t, {2, 0}, Terrain::Heights(1024, 1.0f))); // no tile there
    GIANT_CHECK(!game.press(t, {0, 0}, Terrain::Heights(16, 1.0f)));   // another size
    GIANT_CHECK(game.tiles() == 0 && t.version({0, 0}) == v0);
    GIANT_CHECK(game.press(t, {0, 0}, Terrain::Heights(1024, 3.0f)));
    const u64 pressed = t.version({0, 0});
    GIANT_CHECK(pressed != v0 && t.height_at(10.0, 10.0).value() == 3.0);
    GIANT_CHECK((game.ours_tiles(t) == std::vector<CellCoord>{{0, 0}}));
    // Pressed again: still kept aside as authored.
    GIANT_CHECK(game.press(t, {0, 0}, Terrain::Heights(1024, 2.0f)));
    const u64 game_v = t.version({0, 0});
    // A simulation's dents over it go back to the game's ground.
    GroundDamage sim;
    Dent d;
    d.radius = 8.0;
    d.depth = 1.0f;
    std::vector<CellCoord> changed;
    sim.dent(t, d, 10.0, 10.0, changed);
    GIANT_CHECK(t.height_at(10.0, 10.0).value() < 2.0 && game.ours_tiles(t).empty());
    // A save: both lifted (the simulation's first), the ground as authored, then both put back.
    sim.lift(t);
    game.lift(t);
    GIANT_CHECK(t.version({0, 0}) == v0 && t.height_at(10.0, 10.0).value() == 5.0);
    game.put_back(t);
    GIANT_CHECK(t.version({0, 0}) == game_v);
    sim.put_back(t);
    GIANT_CHECK(t.height_at(10.0, 10.0).value() < 2.0);
    GIANT_CHECK(sim.restore(t) == 1 && t.version({0, 0}) == game_v && t.height_at(10.0, 10.0).value() == 2.0);
    GIANT_CHECK((game.ours_tiles(t) == std::vector<CellCoord>{{0, 0}}));
    GIANT_CHECK(game.restore(t) == 1 && t.version({0, 0}) == v0 && t.height_at(10.0, 10.0).value() == 5.0);
    (void)pressed;
}
