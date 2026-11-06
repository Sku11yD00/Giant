// The terrain: tiles by cell, the surface between samples, changes, text.
#include "giant/world/scene.h"

#include "giant_test.h"

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
    GIANT_CHECK(!t.height_at(10.0, 64.0).has_value()); // cell (0, 1)

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
