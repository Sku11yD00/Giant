#include "giant/voxel/brick.h"
#include "giant/voxel/clipmap.h"

#include "giant_test.h"

#include <cmath>
#include <set>
#include <tuple>

using namespace giant;
using namespace giant::voxel;

namespace {
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
    GIANT_CHECK(near(unpack_normal(pack_voxel({}, {NAN, 0, 0})), {0, 1, 0}, 1e-2f));
    GIANT_CHECK(near(unpack_color(pack_voxel({-1, NAN, 2}, {0, 1, 0})), {0, 0, 1}, 1e-6f));
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

GIANT_TEST("voxel: a triangle outside the brick, or with no area, adds nothing") {
    BrickBuilder b;
    b.reset(1.0f);
    GIANT_CHECK(b.add_triangle({10, 0, 0}, {11, 0, 0}, {10, 1, 0}, {1, 1, 1}) == 0);
    GIANT_CHECK(b.add_triangle({-3, 0, 0}, {-2, 0, 0}, {-3, 1, 0}, {1, 1, 1}) == 0);
    GIANT_CHECK(b.add_triangle({1, 1, 1}, {2, 2, 2}, {3, 3, 3}, {1, 1, 1}) == 0); // a line
    GIANT_CHECK(b.add_triangle({NAN, 1, 1}, {2, 2, 2}, {3, 1, 3}, {1, 1, 1}) == 0);
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
    const BrickCoord far = clip.brick_at(0, {1e300, -1e300, NAN});
    GIANT_CHECK(far.x == 1 << 29 && far.y == -(1 << 29) && far.z == -(1 << 29));
    clip.recenter({1e300, 0, 0});
    GIANT_CHECK(clip.contains(0, clip.brick_at(0, {1e300, 0, 0})));
    GIANT_CHECK(clip.slot(0, clip.brick_at(0, {1e300, 0, 0})) < clip.slots_per_level());
}
