#include "giant/voxel/brick.h"
#include "giant/voxel/clipmap.h"

#include "giant_test.h"

#include <bit>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>

using namespace giant;
using namespace giant::voxel;

namespace {
const float k_nan = std::numeric_limits<float>::quiet_NaN();
bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }
bool near(const Vec3& a, const Vec3& b, float eps) {
    return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}
using Key = std::tuple<u32, i32, i32, i32>;
void add_box(std::multiset<Key>& out, const BrickBox& b) {
    for (i32 z = b.min.z; z < b.max.z; ++z)
        for (i32 y = b.min.y; y < b.max.y; ++y)
            for (i32 x = b.min.x; x < b.max.x; ++x) out.insert({b.level, x, y, z});
}
} // namespace

GIANT_TEST("voxel: colours and normals survive packing") {
    const Vec3 colors[] = {{0, 0, 0}, {1, 1, 1}, {0.8f, 0.75f, 0.6f}, {0.02f, 0.3f, 0.12f}};
    for (const Vec3& c : colors) {
        const Vec3 back = unpack_color(pack_voxel(c, {0, 1, 0}));
        // sqrt-encoded 5/6 bits: within a few percent, tighter in the darks.
        GIANT_CHECK(near(back, c, 0.04f));
    }
    const Vec3 normals[] = {{0, 1, 0}, {0, -1, 0}, {1, 0, 0}, {0, 0, -1}, {0.577f, -0.577f, 0.577f},
                            {-0.3f, 0.2f, -0.93f}};
    for (const Vec3& n : normals) {
        const Vec3 unit = normalize_or_zero(n);
        const Vec3 back = unpack_normal(pack_voxel({0.5f, 0.5f, 0.5f}, unit));
        GIANT_CHECK(dot(back, unit) > 0.995f);
    }
    // A zero (or NaN) normal packs as +Y; negative colours as black.
    GIANT_CHECK(near(unpack_normal(pack_voxel({}, {})), {0, 1, 0}, 1e-2f));
    GIANT_CHECK(near(unpack_normal(pack_voxel({}, {k_nan, 0, 0})), {0, 1, 0}, 1e-2f));
    GIANT_CHECK(near(unpack_color(pack_voxel({-1, k_nan, 2}, {0, 1, 0})), {0, 0, 1}, 1e-6f));
    // A NaN in blue (the low bits) leaves the normal alone.
    GIANT_CHECK(pack_voxel({1, 1, k_nan}, {0, 1, 0}) >> 16 == pack_voxel({1, 1, 0}, {0, 1, 0}) >> 16);
    GIANT_CHECK(near(unpack_color(pack_voxel({1, 1, k_nan}, {0, 1, 0})), {1, 1, 0}, 1e-6f));
}

GIANT_TEST("voxel: light packs within a percent, with the sun bit and an unlit state") {
    const Vec3 lights[] = {{0, 0, 0}, {1, 1, 1}, {0.5f, 0.25f, 0.125f}, {3.0f, 2.9f, 2.5f}, {0.002f, 0.01f, 0.001f},
                           {40.0f, 0.5f, 0.0f}, {0.999f, 0.0f, 0.0f}, {0.9995f, 0.0f, 0.0f}};
    for (const Vec3& c : lights) {
        for (const bool sun : {false, true}) {
            const u32 packed = pack_light(c, sun);
            GIANT_CHECK(light_lit(packed) && light_sun(packed) == sun);
            const Vec3 back = unpack_light(packed);
            const float peak = std::max(c.x, std::max(c.y, c.z));
            // Each channel within half a mantissa step of the brightest.
            GIANT_CHECK(near(back, c, peak / 511.0f + 1e-7f));
        }
    }
    GIANT_CHECK(!light_lit(k_unlit) && unpack_light(k_unlit) == Vec3{});
    // Too bright is clamped; negative and NaN are black (still lit).
    GIANT_CHECK(near(unpack_light(pack_light({1000.0f, 0, 0}, false)), {k_max_light, 0, 0}, 0.07f));
    GIANT_CHECK(unpack_light(pack_light({-1.0f, k_nan, 0.0f}, false)) == Vec3{});
    GIANT_CHECK(light_lit(pack_light({-1.0f, k_nan, 0.0f}, false)));
}

GIANT_TEST("voxel: the triangle-box test is exact on simple cases") {
    const Vec3 c{0.5f, 0.5f, 0.5f};
    const Vec3 h{0.5f, 0.5f, 0.5f};
    // Through the middle, touching a face, just outside, crossing a corner.
    GIANT_CHECK(triangle_overlaps_box(c, h, {-1, 0.5f, -1}, {2, 0.5f, -1}, {0.5f, 0.5f, 2}));
    GIANT_CHECK(triangle_overlaps_box(c, h, {-1, 1.0f, -1}, {2, 1.0f, -1}, {0.5f, 1.0f, 2}));
    GIANT_CHECK(!triangle_overlaps_box(c, h, {-1, 1.01f, -1}, {2, 1.01f, -1}, {0.5f, 1.01f, 2}));
    // Their bounding boxes cover the cube, the triangles miss it: a slab
    // beside the cube's corner (x + y = 2.1), then one just touching it.
    GIANT_CHECK(!triangle_overlaps_box(c, h, {-1, 3.1f, 0}, {3.1f, -1, 0}, {-1, 3.1f, 1}));
    GIANT_CHECK(triangle_overlaps_box(c, h, {-1, 2.9f, 0}, {2.9f, -1, 0}, {-1, 2.9f, 1}));
}

GIANT_TEST("voxel: a floor triangle fills one layer of the brick, facing up") {
    BrickBuilder b;
    b.reset(0.5f);
    GIANT_CHECK(b.empty());
    // y = 1.25 is inside voxel row 2 ([1.0, 1.5)); counter-clockwise from above.
    const u32 tested = b.add_triangle({-1, 1.25f, -1}, {-1, 1.25f, 9}, {9, 1.25f, -1}, {0.2f, 0.6f, 0.2f});
    GIANT_CHECK(tested == 64); // the clipped box: one 8x8 layer
    GIANT_CHECK(!b.empty());
    const Brick brick = b.build();
    u32 layer = 0;
    u32 others = 0;
    for (u32 z = 0; z < 8; ++z)
        for (u32 y = 0; y < 8; ++y)
            for (u32 x = 0; x < 8; ++x) {
                const u32 i = voxel_index(x, y, z);
                if (!brick.occupied(i)) continue;
                if (y == 2) {
                    // The triangle covers the lower-left half: x + z < 16 (in voxels) and some more.
                    ++layer;
                    GIANT_CHECK(dot(unpack_normal(brick.voxels[i]), {0, 1, 0}) > 0.99f);
                    GIANT_CHECK(near(unpack_color(brick.voxels[i]), {0.2f, 0.6f, 0.2f}, 0.04f));
                } else {
                    ++others;
                }
            }
    GIANT_CHECK(others == 0);
    // Its hypotenuse x + z = 8 m cuts the 4 m brick nowhere: the whole layer.
    GIANT_CHECK(layer == 64);
    GIANT_CHECK(brick.count() == 64);
}

GIANT_TEST("voxel: stand-in triangles mark their voxels and keep their own colour") {
    BrickBuilder b;
    b.reset(0.5f);
    // A grey floor over the whole layer, then a red stand-in over part of it.
    b.add_triangle({-1, 1.25f, -1}, {-1, 1.25f, 9}, {9, 1.25f, -1}, {0.5f, 0.5f, 0.5f});
    b.add_triangle({0.1f, 1.25f, 0.1f}, {0.1f, 1.25f, 0.4f}, {0.4f, 1.25f, 0.1f}, {0.9f, 0.0f, 0.0f}, true);
    const Brick brick = b.build();
    const u32 corner = voxel_index(0, 2, 0);
    const u32 other = voxel_index(5, 2, 5);
    GIANT_CHECK(brick.occupied(corner) && brick.stands_in(corner));
    GIANT_CHECK(near(unpack_color(brick.voxels[corner]), {0.9f, 0.0f, 0.0f}, 0.05f)); // not mixed with the floor
    GIANT_CHECK(brick.occupied(other) && !brick.stands_in(other));
    u32 stand = 0;
    for (const u32 w : brick.stand_in) stand += static_cast<u32>(std::popcount(w));
    GIANT_CHECK(stand == 1);
    // reset clears the stand-in sums too.
    b.reset(0.5f);
    b.add_triangle({0.1f, 1.25f, 0.1f}, {0.1f, 1.25f, 0.4f}, {0.4f, 1.25f, 0.1f}, {0.5f, 0.5f, 0.5f});
    GIANT_CHECK(!b.build().stands_in(corner));
}

namespace {
// A layer of voxel-sized cut-out cards at y = 1.25 (voxels 0.5 m: layer 2),
// each inset so it touches its own voxel only; `layers` copies stacked.
u32 card_layer(BrickBuilder& b, float coverage, u32 layers, bool stand_in = false) {
    const Vec3 color{0.2f, 0.6f, 0.1f};
    for (u32 l = 0; l < layers; ++l) {
        for (u32 z = 0; z < k_brick_edge; ++z) {
            for (u32 x = 0; x < k_brick_edge; ++x) {
                const float x0 = static_cast<float>(x) * 0.5f + 0.01f;
                const float z0 = static_cast<float>(z) * 0.5f + 0.01f;
                const float x1 = x0 + 0.48f;
                const float z1 = z0 + 0.48f;
                b.add_triangle({x0, 1.25f, z0}, {x0, 1.25f, z1}, {x1, 1.25f, z0}, color, stand_in, coverage);
                b.add_triangle({x1, 1.25f, z0}, {x0, 1.25f, z1}, {x1, 1.25f, z1}, color, stand_in, coverage);
            }
        }
    }
    return b.build().count();
}
} // namespace

GIANT_TEST("voxel: cut-out triangles fill voxels as often as they would hide what is behind") {
    BrickBuilder b;
    // One card of coverage c in a voxel hides about c * 0.92 (its inset
    // area) of it: over many seeds that share of the layer fills.
    for (const float coverage : {0.2f, 0.5f, 0.8f}) {
        u32 filled = 0;
        const u32 seeds = 64;
        for (u32 seed = 0; seed < seeds; ++seed) {
            b.reset(0.5f, seed);
            filled += card_layer(b, coverage, 1);
        }
        const float share = static_cast<float>(filled) / static_cast<float>(seeds * 64);
        const float expected = coverage * 0.48f * 0.48f / 0.25f;
        GIANT_CHECK(near(share, expected, 0.03f));
    }
    // Full coverage is solid: every voxel, whatever the seed.
    b.reset(0.5f, 9);
    GIANT_CHECK(card_layer(b, 1.0f, 1) == 64);
    // Denser (cards stacked) fills more; enough fills every voxel.
    b.reset(0.5f, 5);
    const u32 one = card_layer(b, 0.3f, 1);
    b.reset(0.5f, 5);
    const u32 three = card_layer(b, 0.3f, 3);
    b.reset(0.5f, 5);
    const u32 four = card_layer(b, 0.3f, 4);
    GIANT_CHECK(one < three && three <= four && four == 64);
    // The same seed builds the same brick (a rebuild does not flicker).
    b.reset(0.5f, 11);
    card_layer(b, 0.4f, 1);
    const Brick first = b.build();
    b.reset(0.5f, 11);
    card_layer(b, 0.4f, 1);
    GIANT_CHECK(b.build().occupancy == first.occupancy);
    // Nothing (or NaN) covered adds nothing.
    b.reset(0.5f, 1);
    GIANT_CHECK(b.add_triangle({0, 1.25f, 0}, {0, 1.25f, 4}, {4, 1.25f, 0}, {1, 1, 1}, false, 0.0f) == 0);
    GIANT_CHECK(b.add_triangle({0, 1.25f, 0}, {0, 1.25f, 4}, {4, 1.25f, 0}, {1, 1, 1}, false, k_nan) == 0);
    GIANT_CHECK(b.empty());
    // Thresholds stay in (0, 1].
    bool in_range = true;
    for (u32 i = 0; i < k_brick_voxels; ++i) in_range = in_range && b.threshold(i) > 0.0f && b.threshold(i) <= 1.0f;
    GIANT_CHECK(in_range);
}

GIANT_TEST("voxel: a cut-out bigger than the brick, or across its edge, counts only its part inside") {
    BrickBuilder b;
    // One huge triangle over the whole layer: each voxel gets a voxel's
    // face of it, so 30% coverage fills about 30%, not all.
    u32 filled = 0;
    const u32 seeds = 64;
    for (u32 seed = 0; seed < seeds; ++seed) {
        b.reset(0.5f, seed);
        b.add_triangle({-50, 1.25f, -50}, {-50, 1.25f, 100}, {100, 1.25f, -50}, {0.2f, 0.6f, 0.1f}, false, 0.3f);
        filled += b.build().count();
    }
    GIANT_CHECK(near(static_cast<float>(filled) / static_cast<float>(seeds * 64), 0.3f, 0.03f));
    // A card half over the brick's +x edge (the last column): half its
    // area is inside, in one voxel, so it fills as a half-sized card does.
    u32 across = 0;
    u32 half = 0;
    for (u32 seed = 0; seed < 256; ++seed) {
        b.reset(0.5f, seed);
        b.add_triangle({3.75f, 1.25f, 0.0f}, {3.75f, 1.25f, 0.5f}, {4.25f, 1.25f, 0.0f}, {1, 1, 1}, false, 0.8f);
        b.add_triangle({4.25f, 1.25f, 0.0f}, {3.75f, 1.25f, 0.5f}, {4.25f, 1.25f, 0.5f}, {1, 1, 1}, false, 0.8f);
        across += b.build().count();
        b.reset(0.5f, seed);
        b.add_triangle({3.75f, 1.25f, 0.0f}, {3.75f, 1.25f, 0.5f}, {4.0f, 1.25f, 0.0f}, {1, 1, 1}, false, 0.8f);
        b.add_triangle({4.0f, 1.25f, 0.0f}, {3.75f, 1.25f, 0.5f}, {4.0f, 1.25f, 0.5f}, {1, 1, 1}, false, 0.8f);
        half += b.build().count();
    }
    GIANT_CHECK(across == half);
    GIANT_CHECK(near(static_cast<float>(across) / 256.0f, 0.4f, 0.08f));
}

GIANT_TEST("voxel: a thin cut-out stand-in over solid ground leaves the ground, not a hole") {
    BrickBuilder b;
    b.reset(0.5f, 3);
    // Grey ground (solid, stays a mesh) and a barely-there green stand-in
    // in every voxel of the layer.
    b.add_triangle({-1, 1.25f, -1}, {-1, 1.25f, 9}, {9, 1.25f, -1}, {0.5f, 0.5f, 0.5f});
    const u32 count = card_layer(b, 0.05f, 1, true);
    GIANT_CHECK(count == 64);
    const Brick brick = b.build();
    u32 stand = 0;
    u32 green = 0;
    for (u32 z = 0; z < k_brick_edge; ++z) {
        for (u32 x = 0; x < k_brick_edge; ++x) {
            const u32 i = voxel_index(x, 2, z);
            if (brick.stands_in(i)) {
                ++stand;
                green += near(unpack_color(brick.voxels[i]), {0.2f, 0.6f, 0.1f}, 0.05f) ? 1u : 0u;
            } else {
                GIANT_CHECK(near(unpack_color(brick.voxels[i]), {0.5f, 0.5f, 0.5f}, 0.05f)); // the ground's own
            }
        }
    }
    GIANT_CHECK(stand < 16 && green == stand); // few, and those keep the grass's colour
}

GIANT_TEST("voxel: a triangle outside the brick, or with no area, adds nothing") {
    BrickBuilder b;
    b.reset(1.0f);
    GIANT_CHECK(b.add_triangle({10, 0, 0}, {11, 0, 0}, {10, 1, 0}, {1, 1, 1}) == 0);
    GIANT_CHECK(b.add_triangle({-3, 0, 0}, {-2, 0, 0}, {-3, 1, 0}, {1, 1, 1}) == 0);
    GIANT_CHECK(b.add_triangle({1, 1, 1}, {2, 2, 2}, {3, 3, 3}, {1, 1, 1}) == 0); // a line
    GIANT_CHECK(b.add_triangle({k_nan, 1, 1}, {2, 2, 2}, {3, 1, 3}, {1, 1, 1}) == 0);
    GIANT_CHECK(b.empty() && b.build().empty());
}

GIANT_TEST("voxel: a closed box voxelizes as a watertight shell with outward normals") {
    // A 2.6 m cube centred in a 4 m brick of 0.5 m voxels.
    BrickBuilder b;
    b.reset(0.5f);
    const float lo = 0.7f;
    const float hi = 3.3f;
    const Vec3 p[8] = {{lo, lo, lo}, {hi, lo, lo}, {hi, hi, lo}, {lo, hi, lo},
                       {lo, lo, hi}, {hi, lo, hi}, {hi, hi, hi}, {lo, hi, hi}};
    const int faces[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5}};
    for (const auto& f : faces) {
        b.add_triangle(p[f[0]], p[f[1]], p[f[2]], {1, 0, 0});
        b.add_triangle(p[f[0]], p[f[2]], p[f[3]], {1, 0, 0});
    }
    const Brick brick = b.build();
    // Voxels 1..6 on each axis are touched by the box's surface; 2..5 inside
    // (the faces at 0.7 and 3.3 sit in voxels 1 and 6).
    bool shell = true;
    bool normals_out = true;
    for (u32 z = 0; z < 8; ++z)
        for (u32 y = 0; y < 8; ++y)
            for (u32 x = 0; x < 8; ++x) {
                const bool in_box = x >= 1 && x <= 6 && y >= 1 && y <= 6 && z >= 1 && z <= 6;
                const bool inside = x >= 2 && x <= 5 && y >= 2 && y <= 5 && z >= 2 && z <= 5;
                const bool expect = in_box && !inside;
                const u32 i = voxel_index(x, y, z);
                shell = shell && brick.occupied(i) == expect;
                if (expect) {
                    const Vec3 out = normalize_or_zero(Vec3{static_cast<float>(x) - 3.5f, static_cast<float>(y) - 3.5f,
                                                            static_cast<float>(z) - 3.5f});
                    normals_out = normals_out && dot(unpack_normal(brick.voxels[i]), out) > 0.3f;
                }
            }
    GIANT_CHECK(shell);
    GIANT_CHECK(normals_out);
    GIANT_CHECK(brick.count() == 6 * 6 * 6 - 4 * 4 * 4);
}

GIANT_TEST("voxel: clipmap levels double, slots are stable and unique") {
    ClipmapDesc desc;
    desc.levels = 4;
    desc.voxel_size = 0.25f;
    desc.bricks_per_axis = 8;
    Clipmap clip(desc);
    GIANT_CHECK(clip.voxel_size(0) == 0.25 && clip.voxel_size(3) == 2.0 && clip.brick_size(1) == 4.0);
    const auto first = clip.recenter({0, 0, 0});
    GIANT_CHECK(first.size() == 4);
    for (const BrickBox& b : first) GIANT_CHECK(b.count() == 512);
    GIANT_CHECK(clip.origin(0) == (BrickCoord{-4, -4, -4}));
    GIANT_CHECK(clip.contains(0, {3, 3, 3}) && !clip.contains(0, {4, 0, 0}) && clip.contains(0, {-4, -4, -4}));
    // Every brick of the window has its own slot.
    std::set<u32> slots;
    for (i32 z = -4; z < 4; ++z)
        for (i32 y = -4; y < 4; ++y)
            for (i32 x = -4; x < 4; ++x) slots.insert(clip.slot(0, {x, y, z}));
    GIANT_CHECK(slots.size() == 512 && *slots.rbegin() == 511);
    // Bricks a box touches, clipped to the window.
    const BrickBox touched = clip.bricks_touching(0, {-0.1, 0.0, 0.0}, {2.1, 0.5, 0.5});
    GIANT_CHECK(touched.min == (BrickCoord{-1, 0, 0}) && touched.max == (BrickCoord{2, 1, 1}));
    GIANT_CHECK(clip.bricks_touching(0, {100, 0, 0}, {101, 1, 1}).count() == 0);
    const BrickBox ground = clip.bricks_touching(0, {-1000, -0.01, -1000}, {1000, 0.01, 1000});
    GIANT_CHECK(ground.count() == 8 * 8 * 2);
}

GIANT_TEST("voxel: moving the camera brings in exactly the new slabs") {
    ClipmapDesc desc;
    desc.levels = 3;
    desc.voxel_size = 0.5f; // bricks: 4, 8, 16 m
    desc.bricks_per_axis = 8;
    Clipmap clip(desc);
    clip.recenter({1, 1, 1});
    // Within a brick of the centre: nothing moves.
    GIANT_CHECK(clip.recenter({5, 1, 1}).empty()); // level 0: one brick over
    GIANT_CHECK(clip.recenter({1, 1, 1}).empty());
    // Walk diagonally; each step's new bricks are the new window minus the old.
    DVec3 cam{1, 1, 1};
    bool exact = true;
    for (int step = 0; step < 40; ++step) {
        std::vector<BrickCoord> before;
        for (u32 l = 0; l < clip.levels(); ++l) before.push_back(clip.origin(l));
        cam = cam + DVec3{3.7, -1.3, 2.9};
        const auto boxes = clip.recenter(cam);
        std::multiset<Key> got;
        for (const BrickBox& b : boxes) add_box(got, b);
        std::multiset<Key> want;
        for (u32 l = 0; l < clip.levels(); ++l) {
            const BrickCoord o = clip.origin(l);
            const BrickCoord p = before[l];
            for (i32 z = o.z; z < o.z + 8; ++z)
                for (i32 y = o.y; y < o.y + 8; ++y)
                    for (i32 x = o.x; x < o.x + 8; ++x) {
                        const bool was = x >= p.x && x < p.x + 8 && y >= p.y && y < p.y + 8 && z >= p.z && z < p.z + 8;
                        if (!was) want.insert({l, x, y, z});
                    }
            // The camera stays well inside every window.
            const BrickCoord c = clip.brick_at(l, cam);
            exact = exact && c.x - o.x >= 2 && o.x + 8 - c.x >= 3 && c.y - o.y >= 2 && c.z - o.z >= 2;
        }
        exact = exact && got == want; // also: no brick twice
    }
    GIANT_CHECK(exact);
    // A jump past the window refills it whole; reset() refills every level.
    const auto jump = clip.recenter({10000, 0, 0});
    u64 total = 0;
    for (const BrickBox& b : jump) total += b.count();
    GIANT_CHECK(total == 3 * 512);
    clip.reset();
    GIANT_CHECK(clip.recenter({10000, 0, 0}).size() == 3);
}

GIANT_TEST("voxel: far and NaN positions do not overflow brick coordinates") {
    Clipmap clip;
    const BrickCoord far = clip.brick_at(0, {1e300, -1e300, k_nan});
    GIANT_CHECK(far.x == 1 << 29 && far.y == -(1 << 29) && far.z == -(1 << 29));
    clip.recenter({1e300, 0, 0});
    GIANT_CHECK(clip.contains(0, clip.brick_at(0, {1e300, 0, 0})));
    GIANT_CHECK(clip.slot(0, clip.brick_at(0, {1e300, 0, 0})) < clip.slots_per_level());
}
