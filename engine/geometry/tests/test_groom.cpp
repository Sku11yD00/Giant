#include "giant/geometry/groom.h"

#include "giant_test.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace giant;
using namespace giant::geometry;

namespace {

float strand_length(const Groom& g, u32 s) {
    const std::span<const Vec3> p = g.strand(s);
    float along = 0.0f;
    for (u32 i = 1; i < k_strand_points; ++i) along += length(p[i] - p[i - 1]);
    return along;
}

Vec3 root(const Groom& g, u32 s) { return g.strand(s)[0]; }

GroomParams straight(u32 strands, u32 guides) {
    GroomParams p;
    p.strands = strands;
    p.guides = guides;
    p.length = 0.2f;
    p.length_variation = 0.25f;
    return p;
}

} // namespace

GIANT_TEST("groom: strands grow from the scalp, as long as asked, the same every time") {
    const TriangleMesh scalp = make_sphere();
    const GroomParams params = straight(4000, 100);
    const Groom groom = generate_groom(scalp, params);
    GIANT_CHECK(groom.strand_count() == 4000 && groom.guide_count == 100);
    GIANT_CHECK(groom_problem(groom).empty());
    float longest = 0.0f;
    for (u32 s = 0; s < groom.strand_count(); ++s) {
        // Roots lie on the sphere's faces (just inside its 0.5 m), strands
        // leave outwards and are 75 to 100 % of the length.
        const float from_centre = length(root(groom, s));
        GIANT_CHECK(from_centre > 0.49f && from_centre <= 0.5001f);
        GIANT_CHECK(dot(groom.root_normals[s], normalize_or_zero(root(groom, s))) > 0.99f);
        GIANT_CHECK(dot(groom.strand(s)[1] - root(groom, s), groom.root_normals[s]) > 0.0f);
        const float along = strand_length(groom, s);
        GIANT_CHECK(along > 0.2f * 0.75f - 1e-4f && along < 0.2f + 1e-4f);
        longest = std::max(longest, along);
    }
    GIANT_CHECK_NEAR(groom.longest, longest, 1e-6);
    float shortest = longest;
    for (u32 s = 0; s < groom.strand_count(); ++s) shortest = std::min(shortest, strand_length(groom, s));
    GIANT_CHECK(shortest < 0.2f * 0.8f && longest > 0.2f * 0.97f); // lengths vary across the range
    for (const Vec3& p : groom.points) {
        GIANT_CHECK(p.x >= groom.bounds_min.x && p.y >= groom.bounds_min.y && p.z >= groom.bounds_min.z);
        GIANT_CHECK(p.x <= groom.bounds_max.x && p.y <= groom.bounds_max.y && p.z <= groom.bounds_max.z);
    }
    // Drooping: tips end lower than strands standing on end would.
    GroomParams upright = params;
    upright.droop = 0.0f;
    const Groom stiff = generate_groom(scalp, upright);
    double tips = 0.0;
    double stiff_tips = 0.0;
    for (u32 s = 0; s < groom.strand_count(); ++s) {
        tips += static_cast<double>(groom.strand(s)[k_strand_points - 1].y);
        stiff_tips += static_cast<double>(stiff.strand(s)[k_strand_points - 1].y);
        // On end: straight out along the normal.
        const Vec3 out = stiff.strand(s)[k_strand_points - 1] - root(stiff, s);
        GIANT_CHECK_NEAR(length(out), strand_length(stiff, s), 1e-5);
        GIANT_CHECK(dot(normalize_or_zero(out), stiff.root_normals[s]) > 0.9999f);
        // Hanging: never turning back up once it points down.
        bool downward = false;
        for (u32 i = 1; i < k_strand_points; ++i) {
            const float dy = groom.strand(s)[i].y - groom.strand(s)[i - 1].y;
            GIANT_CHECK(!downward || dy < 0.0f);
            downward = downward || dy < 0.0f;
        }
    }
    GIANT_CHECK(tips / groom.strand_count() < stiff_tips / groom.strand_count() - 0.05);
    // The same again, and another seed's is another groom.
    const Groom again = generate_groom(scalp, params);
    GIANT_CHECK(again.seeds == groom.seeds && again.follow == groom.follow);
    bool same = again.points.size() == groom.points.size();
    for (usize i = 0; same && i < groom.points.size(); ++i) {
        same = again.points[i].x == groom.points[i].x && again.points[i].y == groom.points[i].y &&
               again.points[i].z == groom.points[i].z;
    }
    GIANT_CHECK(same);
    GroomParams other = params;
    other.seed = 2;
    const Groom another = generate_groom(scalp, other);
    GIANT_CHECK(another.seeds != groom.seeds);
    u32 moved_roots = 0;
    for (u32 s = 0; s < groom.strand_count(); ++s) {
        moved_roots += length(root(another, s) - root(groom, s)) > 1e-3f ? 1u : 0u;
    }
    GIANT_CHECK(moved_roots > groom.strand_count() * 9 / 10);
}

GIANT_TEST("groom: guides come first, cover the scalp, and every strand follows its nearest") {
    const Groom groom = generate_groom(make_sphere(), straight(6000, 128));
    for (u32 g = 0; g < groom.guide_count; ++g) {
        GIANT_CHECK(groom.follow[g].guides[0] == g && groom.follow[g].weights[0] == 1.0f);
        GIANT_CHECK(groom.follow[g].weights[1] == 0.0f && groom.follow[g].weights[2] == 0.0f);
    }
    // A sphere 0.5 m in radius has 3.14 m2: with 128 guides spread evenly no
    // root is far from one (evenly, a guide owns a disc 0.088 m in radius).
    float worst = 0.0f;
    for (u32 s = groom.guide_count; s < groom.strand_count(); ++s) {
        const StrandFollow& f = groom.follow[s];
        std::array<float, k_strand_guides> near{};
        for (u32 k = 0; k < k_strand_guides; ++k) {
            GIANT_CHECK(f.guides[k] < groom.guide_count);
            near[k] = length(root(groom, f.guides[k]) - root(groom, s));
        }
        GIANT_CHECK(near[0] <= near[1] && near[1] <= near[2]);
        GIANT_CHECK(f.weights[0] >= f.weights[1] && f.weights[1] >= f.weights[2] && f.weights[2] > 0.0f);
        GIANT_CHECK_NEAR(f.weights[0] + f.weights[1] + f.weights[2], 1.0, 1e-5);
        // By the inverse square of the distance.
        if (near[0] > 1e-3f) {
            GIANT_CHECK_NEAR(f.weights[1] / f.weights[0], (near[0] * near[0]) / (near[1] * near[1]), 1e-3);
        }
        worst = std::max(worst, near[0]);
        // Truly the nearest three: no other guide is nearer than the third.
        if (s % 7 == 0) {
            for (u32 g = 0; g < groom.guide_count; ++g) {
                if (g == f.guides[0] || g == f.guides[1] || g == f.guides[2]) continue;
                GIANT_CHECK(length(root(groom, g) - root(groom, s)) >= near[2] - 1e-6f);
            }
        }
    }
    GIANT_CHECK(worst < 0.2f);
    // Spread out: no two guides close together (evenly they are about
    // 0.17 m apart).
    for (u32 a = 0; a < groom.guide_count; ++a) {
        for (u32 b = a + 1; b < groom.guide_count; ++b) {
            GIANT_CHECK(length(root(groom, a) - root(groom, b)) > 0.06f);
        }
    }
}

GIANT_TEST("groom: on a flat scalp and along a line the nearest guides are still the nearest") {
    std::vector<std::vector<Vec3>> row;
    for (int i = 0; i < 600; ++i) {
        const float x = 0.01f * static_cast<float>(i);
        row.push_back({{x, 0, 0}, {x, 0.1f, 0}});
    }
    const Groom line = groom_from_strands(row, 40);
    const Groom flat = generate_groom(make_plane(), straight(5000, 200));
    for (const Groom* groom : {&line, &flat}) {
        GIANT_CHECK(groom_problem(*groom).empty());
        for (u32 s = groom->guide_count; s < groom->strand_count(); ++s) {
            const StrandFollow& f = groom->follow[s];
            const float third = length(root(*groom, f.guides[2]) - root(*groom, s));
            GIANT_CHECK(length(root(*groom, f.guides[0]) - root(*groom, s)) <= third);
            for (u32 g = 0; g < groom->guide_count; ++g) {
                if (g == f.guides[0] || g == f.guides[1] || g == f.guides[2]) continue;
                GIANT_CHECK(length(root(*groom, g) - root(*groom, s)) >= third - 1e-6f);
            }
        }
    }
    // The row was shuffled: its first strands come from all along it.
    float low = 6.0f;
    float high = 0.0f;
    for (u32 s = line.guide_count; s < line.guide_count + 60; ++s) {
        low = std::min(low, root(line, s).x);
        high = std::max(high, root(line, s).x);
    }
    GIANT_CHECK(low < 1.0f && high > 5.0f);
}

GIANT_TEST("groom: any first run of strands covers the scalp evenly") {
    const Groom groom = generate_groom(make_sphere(), straight(20000, 64));
    // The sphere's eight octants are alike in area: each gets about an
    // eighth of the first 800 strands, and of the first 80.
    for (u32 run : {80u, 800u}) {
        std::array<u32, 8> octants{};
        for (u32 s = groom.guide_count; s < groom.guide_count + run; ++s) {
            const Vec3 r = root(groom, s);
            ++octants[(r.x > 0.0f ? 1u : 0u) | (r.y > 0.0f ? 2u : 0u) | (r.z > 0.0f ? 4u : 0u)];
        }
        for (u32 n : octants) {
            GIANT_CHECK(n * 8 > run / 2 && n * 8 < run * 2);
        }
    }
}

GIANT_TEST("groom: a strand moves as its guides moved") {
    const Groom groom = generate_groom(make_sphere(), straight(500, 20));
    const usize guide_points = static_cast<usize>(groom.guide_count) * k_strand_points;
    std::vector<Vec3> posed(groom.points.begin(), groom.points.begin() + static_cast<std::ptrdiff_t>(guide_points));
    std::array<Vec3, k_strand_points> out{};
    const u32 strand = 300;
    // Guides at rest: the strand at rest.
    pose_strand(groom, strand, posed, out);
    for (u32 i = 0; i < k_strand_points; ++i) {
        GIANT_CHECK_NEAR(length(out[i] - groom.strand(strand)[i]), 0.0, 1e-6);
    }
    // All guides moved alike: the strand with them.
    const Vec3 moved{0.1f, -0.2f, 0.3f};
    for (Vec3& p : posed) p = p + moved;
    pose_strand(groom, strand, posed, out);
    for (u32 i = 0; i < k_strand_points; ++i) {
        GIANT_CHECK_NEAR(length(out[i] - (groom.strand(strand)[i] + moved)), 0.0, 1e-5);
    }
    // One guide's tip alone: the strand's tip by that guide's weight.
    std::copy_n(groom.points.begin(), guide_points, posed.begin());
    const StrandFollow& f = groom.follow[strand];
    posed[static_cast<usize>(f.guides[1]) * k_strand_points + k_strand_points - 1].y += 1.0f;
    pose_strand(groom, strand, posed, out);
    GIANT_CHECK_NEAR(out[k_strand_points - 1].y - groom.strand(strand)[k_strand_points - 1].y, f.weights[1], 1e-5);
    GIANT_CHECK_NEAR(length(out[0] - groom.strand(strand)[0]), 0.0, 1e-6);
    // A guide poses as itself.
    pose_strand(groom, f.guides[1], posed, out);
    GIANT_CHECK_NEAR(out[k_strand_points - 1].y - groom.strand(f.guides[1])[k_strand_points - 1].y, 1.0, 1e-5);
}

GIANT_TEST("groom: curls wind round the strand's line, frizz wanders, clumps draw tips in to a guide") {
    const TriangleMesh scalp = make_plane();
    GroomParams params = straight(400, 16);
    params.length_variation = 0.0f;
    params.droop = 0.0f; // straight up from the plane
    const Groom plain = generate_groom(scalp, params);
    for (u32 s = 0; s < plain.strand_count(); ++s) {
        const Vec3 tip = plain.strand(s)[k_strand_points - 1] - root(plain, s);
        GIANT_CHECK_NEAR(tip.x, 0.0, 1e-5);
        GIANT_CHECK_NEAR(tip.y, 0.2, 1e-5);
        GIANT_CHECK_NEAR(tip.z, 0.0, 1e-5);
    }
    GroomParams curly = params;
    curly.curl_radius = 0.01f;
    curly.curl_turns = 3.0f;
    const Groom curls = generate_groom(scalp, curly);
    GIANT_CHECK(groom_problem(curls).empty());
    for (u32 s = 0; s < curls.strand_count(); ++s) {
        GIANT_CHECK_NEAR(length(root(curls, s) - root(plain, s)), 0.0, 1e-6); // roots stay on the scalp
        for (u32 i = 4; i < k_strand_points; ++i) {
            const Vec3 off = curls.strand(s)[i] - root(curls, s);
            GIANT_CHECK_NEAR(std::sqrt(off.x * off.x + off.z * off.z), 0.01, 1e-4);
        }
        GIANT_CHECK(strand_length(curls, s) > 0.2f);
    }
    // Curls on hanging hair wind smoothly: no jump from one point to the
    // next where the strand turns downward.
    GroomParams hanging = curly;
    hanging.droop = 0.8f;
    hanging.strands = 2000;
    const Groom hung = generate_groom(make_sphere(), hanging);
    GroomParams hanging_plain = hanging;
    hanging_plain.curl_radius = 0.0f;
    const Groom hung_plain = generate_groom(make_sphere(), hanging_plain);
    float worst_step = 0.0f;
    for (u32 s = 0; s < hung.strand_count(); ++s) {
        for (u32 i = 1; i < k_strand_points; ++i) {
            const Vec3 off = hung.strand(s)[i] - hung_plain.strand(s)[i];
            const Vec3 before = hung.strand(s)[i - 1] - hung_plain.strand(s)[i - 1];
            GIANT_CHECK(length(off) <= 0.01f + 1e-5f);
            // 3 turns over 15 steps: 72 degrees a step, a chord of 1.18 radii;
            // the frame's own turn adds a little (a jump would be up to 2).
            worst_step = std::max(worst_step, length(off - before));
        }
    }
    std::printf("groom: worst curl step %.5f\n", static_cast<double>(worst_step));
    GIANT_CHECK(worst_step < 0.01f * 1.5f);
    GroomParams frizzy = params;
    frizzy.frizz = 0.02f;
    const Groom frizz = generate_groom(scalp, frizzy);
    float wandered = 0.0f;
    for (u32 s = 0; s < frizz.strand_count(); ++s) {
        const Vec3 off = frizz.strand(s)[k_strand_points - 1] - plain.strand(s)[k_strand_points - 1];
        GIANT_CHECK(length(off) <= 0.02f * 1.4143f);
        wandered = std::max(wandered, length(off));
        // Growing from nothing at the root.
        GIANT_CHECK(length(frizz.strand(s)[1] - plain.strand(s)[1]) <= 0.02f * 1.4143f / 15.0f + 1e-6f);
        GIANT_CHECK_NEAR(length(root(frizz, s) - root(plain, s)), 0.0, 1e-6);
    }
    GIANT_CHECK(wandered > 0.005f);
    GroomParams locks = params;
    locks.clump = 1.0f;
    const Groom clumped = generate_groom(scalp, locks);
    GIANT_CHECK(clumped.follow == plain.follow);
    for (u32 s = clumped.guide_count; s < clumped.strand_count(); ++s) {
        const u32 guide = clumped.follow[s].guides[0];
        GIANT_CHECK_NEAR(length(clumped.strand(s)[k_strand_points - 1] - clumped.strand(guide)[k_strand_points - 1]), 0.0,
                         1e-6);
        GIANT_CHECK_NEAR(length(root(clumped, s) - root(plain, s)), 0.0, 1e-6);
        // Drawn in more and more toward the tip: a third of the way at point 5.
        const float apart = length(plain.strand(s)[5] - plain.strand(guide)[5]);
        GIANT_CHECK_NEAR(length(clumped.strand(s)[5] - clumped.strand(guide)[5]), apart * (2.0f / 3.0f), 1e-5);
    }
    // In the curls the helix widens over the first quarter of the strand.
    for (u32 s = 0; s < curls.strand_count(); ++s) {
        const Vec3 off = curls.strand(s)[1] - root(curls, s);
        GIANT_CHECK_NEAR(std::sqrt(off.x * off.x + off.z * off.z), 0.01 * 4.0 / 15.0, 1e-4);
    }
}

GIANT_TEST("groom: wrong values are clamped, and a scalp without area grows nothing") {
    GroomParams wild;
    wild.strands = 50;
    wild.guides = 9999; // more than the strands
    wild.length = -1.0f;
    wild.length_variation = 7.0f;
    wild.droop = std::numeric_limits<float>::quiet_NaN();
    wild.curl_radius = std::numeric_limits<float>::infinity();
    wild.frizz = -3.0f;
    wild.clump = std::numeric_limits<float>::quiet_NaN();
    const Groom groom = generate_groom(make_plane(), wild);
    GIANT_CHECK(groom.strand_count() == 50 && groom.guide_count == 50);
    GIANT_CHECK(groom_problem(groom).empty());
    TriangleMesh flat = make_plane();
    for (Vertex& v : flat.vertices) v.position = {1.0f, 2.0f, 3.0f};
    const Groom none = generate_groom(flat, GroomParams{});
    GIANT_CHECK(none.strand_count() == 0 && none.guide_count == 0 && groom_problem(none).empty());
    GIANT_CHECK(generate_groom(TriangleMesh{}, GroomParams{}).strand_count() == 0);
    // One strand, no guides asked for: it is its own guide. Too many: capped.
    GroomParams one;
    one.strands = 1;
    one.guides = 0;
    const Groom single = generate_groom(make_plane(), one);
    GIANT_CHECK(single.strand_count() == 1 && single.guide_count == 1 && groom_problem(single).empty());
    GroomParams many = straight(k_max_guides + 500, 0xffffffffu);
    const Groom capped = generate_groom(make_plane(), many);
    GIANT_CHECK(capped.guide_count == k_max_guides && groom_problem(capped).empty());
    // A scalp with indices past its vertices and points that are no numbers:
    // those triangles grow nothing, the rest does.
    TriangleMesh hostile = make_plane();
    const u32 plane_vertices = static_cast<u32>(hostile.vertices.size());
    Vertex nan_vertex;
    nan_vertex.position = {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f};
    hostile.vertices.push_back(nan_vertex);
    hostile.indices.insert(hostile.indices.begin(), {0u, 1u, 999u, 0u, 1u, plane_vertices});
    hostile.indices.push_back(0); // a stray index
    const Groom survived = generate_groom(hostile, straight(300, 10));
    GIANT_CHECK(survived.strand_count() == 300 && groom_problem(survived).empty());
    // A triangle without area beside one with: every root on the real one.
    TriangleMesh mixed;
    mixed.vertices.resize(6);
    mixed.vertices[0].position = {5.0f, 0.0f, 5.0f};
    mixed.vertices[1].position = {5.0f, 0.0f, 5.0f};
    mixed.vertices[2].position = {5.0f, 0.0f, 5.0f};
    mixed.vertices[3].position = {0.0f, 0.0f, 0.0f};
    mixed.vertices[4].position = {0.0f, 0.0f, 1.0f};
    mixed.vertices[5].position = {1.0f, 0.0f, 0.0f};
    for (Vertex& v : mixed.vertices) v.normal = {0.0f, 1.0f, 0.0f};
    mixed.indices = {0, 1, 2, 3, 4, 5, 0, 1, 2};
    const Groom real = generate_groom(mixed, straight(200, 8));
    GIANT_CHECK(real.strand_count() == 200);
    for (u32 s = 0; s < real.strand_count(); ++s) {
        const Vec3 r = root(real, s);
        GIANT_CHECK(r.x >= 0.0f && r.z >= 0.0f && r.x + r.z <= 1.0001f);
    }
    // Roots go by area: a triangle nine times the size grows nine times the hair.
    TriangleMesh sizes;
    sizes.vertices.resize(6);
    sizes.vertices[0].position = {0.0f, 0.0f, 0.0f};
    sizes.vertices[1].position = {0.0f, 0.0f, 1.0f};
    sizes.vertices[2].position = {1.0f, 0.0f, 0.0f};
    sizes.vertices[3].position = {10.0f, 0.0f, 0.0f};
    sizes.vertices[4].position = {10.0f, 0.0f, 3.0f};
    sizes.vertices[5].position = {13.0f, 0.0f, 0.0f};
    for (Vertex& v : sizes.vertices) v.normal = {0.0f, 1.0f, 0.0f};
    sizes.indices = {0, 1, 2, 3, 4, 5};
    const Groom shared = generate_groom(sizes, straight(2000, 8));
    u32 on_small = 0;
    for (u32 s = 0; s < shared.strand_count(); ++s) on_small += root(shared, s).x < 5.0f ? 1u : 0u;
    GIANT_CHECK(on_small > 150 && on_small < 250);
    // Points out of reach grow nothing, so nothing a groom holds overflows.
    TriangleMesh distant = sizes;
    distant.vertices[3].position.x = 1e21f;
    distant.vertices[4].position.x = 1e21f;
    distant.vertices[5].position.x = -1e21f;
    GroomParams drawn = straight(100, 1);
    drawn.clump = 0.5f;
    const Groom near_only = generate_groom(distant, drawn);
    GIANT_CHECK(near_only.strand_count() == 100 && groom_problem(near_only).empty());
    for (u32 s = 0; s < near_only.strand_count(); ++s) GIANT_CHECK(root(near_only, s).x <= 1.0001f);
}

GIANT_TEST("groom: strands made elsewhere are resampled evenly, bad ones left out") {
    std::vector<std::vector<Vec3>> lines;
    // An L: 0.3 m up, then 0.15 m along x, in uneven pieces.
    lines.push_back({{0, 0, 0}, {0, 0.05f, 0}, {0, 0.3f, 0}, {0.15f, 0.3f, 0}});
    lines.push_back({{1, 0, 0}});                       // one point
    lines.push_back({{2, 0, 0}, {2, 0, 0}, {2, 0, 0}}); // no length
    lines.push_back({{3, 0, 0}, {3, std::numeric_limits<float>::quiet_NaN(), 0}});
    for (int i = 0; i < 40; ++i) {
        const float x = 0.1f * static_cast<float>(i % 8);
        const float z = 0.1f * static_cast<float>(i / 8) + 1.0f;
        lines.push_back({{x, 0, z}, {x, 0.1f, z}});
    }
    const Groom groom = groom_from_strands(lines, 8);
    GIANT_CHECK(groom.strand_count() == 41 && groom.guide_count == 8);
    GIANT_CHECK(groom_problem(groom).empty());
    GIANT_CHECK_NEAR(groom.longest, 0.45, 1e-5);
    u32 bent = 0;
    for (u32 s = 0; s < groom.strand_count(); ++s) {
        const std::span<const Vec3> p = groom.strand(s);
        if (p[0].z != 0.0f) {
            GIANT_CHECK_NEAR(strand_length(groom, s), 0.1, 1e-5);
            GIANT_CHECK_NEAR(groom.root_normals[s].y, 1.0, 1e-5);
            continue;
        }
        ++bent;
        GIANT_CHECK_NEAR(length(p[0]), 0.0, 1e-6);
        GIANT_CHECK_NEAR(length(p[k_strand_points - 1] - Vec3{0.15f, 0.3f, 0.0f}), 0.0, 1e-5);
        for (u32 i = 0; i < k_strand_points; ++i) {
            // 0.03 m of the line apart: up the first 0.3 m, then along x.
            const float want = 0.03f * static_cast<float>(i);
            const Vec3 at = want <= 0.3f ? Vec3{0.0f, want, 0.0f} : Vec3{want - 0.3f, 0.3f, 0.0f};
            GIANT_CHECK_NEAR(length(p[i] - at), 0.0, 1e-5);
        }
    }
    GIANT_CHECK(bent == 1);
    // The same again; nothing from nothing.
    GIANT_CHECK(groom_from_strands(lines, 8).seeds == groom.seeds);
    GIANT_CHECK(groom_from_strands({}, 8).strand_count() == 0);
    // Strands too far apart to weigh guides by distance still follow one.
    std::vector<std::vector<Vec3>> apart;
    for (float x : {0.0f, 1e6f, -1e6f}) apart.push_back({{x, 0, 0}, {x, 1, 0}});
    apart.push_back({{1e20f, 0, 0}, {1e20f, 1, 0}}); // out of reach: left out
    std::vector<Vec3> endless;                         // a line whose length overflows a float's square
    for (int i = 0; i < 17; ++i) endless.push_back({0, 1.8e19f * static_cast<float>(i), 0});
    apart.push_back(endless);
    const Groom far = groom_from_strands(apart, 1);
    GIANT_CHECK(far.strand_count() == 3 && far.guide_count == 1 && groom_problem(far).empty());
    // Roots all in one place, no guides asked for, more guides than strands.
    std::vector<std::vector<Vec3>> together(20, std::vector<Vec3>{{0, 0, 0}, {0, 0.1f, 0}});
    const Groom heap = groom_from_strands(together, 0);
    GIANT_CHECK(heap.strand_count() == 20 && heap.guide_count == 1 && groom_problem(heap).empty());
    const Groom all = groom_from_strands(together, 500);
    GIANT_CHECK(all.guide_count == 20 && groom_problem(all).empty());
}

GIANT_TEST("groom: a broken groom is told apart") {
    const Groom good = generate_groom(make_plane(), straight(64, 8));
    GIANT_CHECK(groom_problem(good).empty());
    const auto broken = [&](auto&& change) {
        Groom g = good;
        change(g);
        return !groom_problem(g).empty();
    };
    GIANT_CHECK(broken([](Groom& g) { g.points.pop_back(); }));
    GIANT_CHECK(broken([](Groom& g) { g.root_normals.pop_back(); }));
    GIANT_CHECK(broken([](Groom& g) { g.follow.pop_back(); }));
    GIANT_CHECK(broken([](Groom& g) { g.guide_count = 0; }));
    GIANT_CHECK(broken([](Groom& g) { g.guide_count = 65; }));
    GIANT_CHECK(broken([](Groom& g) { g.points[5].x = std::numeric_limits<float>::infinity(); }));
    GIANT_CHECK(broken([](Groom& g) { g.points[5].x = 2.0e6f; }));
    GIANT_CHECK(broken([](Groom& g) { g.root_normals[5] = {0.0f, 2.0f, 0.0f}; }));
    GIANT_CHECK(broken([](Groom& g) { g.root_normals[5].y = std::numeric_limits<float>::quiet_NaN(); }));
    GIANT_CHECK(broken([](Groom& g) { g.follow[20].guides[2] = 8; }));
    GIANT_CHECK(broken([](Groom& g) { g.follow[20].weights = {0.5f, 0.2f, 0.2f}; }));
    GIANT_CHECK(broken([](Groom& g) { g.follow[20].weights = {1.5f, -0.5f, 0.0f}; }));
    GIANT_CHECK(broken([](Groom& g) { g.follow[3].guides[0] = 2; }));
    GIANT_CHECK(broken([](Groom& g) { g.longest = std::numeric_limits<float>::quiet_NaN(); }));
}
