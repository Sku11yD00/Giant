// Hair as strands: plain data, a generator that grows it from a scalp, and
// a builder from strands made elsewhere. Metres, +Y up, in the space of
// whatever the hair hangs on (a head joint) at rest.
//
// Every strand has k_strand_points points, root first, so a strand's points
// are found from its index alone. The first `guide_count` strands are the
// guides: the few strands that are simulated. Every strand follows up to
// three guides: posed, each of its points moves as the weighted guides'
// points moved from rest (pose_strand). The strands after the guides are in
// an order whose every prefix covers the scalp evenly, so drawing the first
// N is an even thinning (LOD by strand count).
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/geometry/mesh.h"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace giant::geometry {

inline constexpr u32 k_strand_points = 16;
inline constexpr u32 k_strand_guides = 3;
// The most strands and guides a groom holds (a head of hair is about
// 100,000 strands; more is asked for by mistake).
inline constexpr u32 k_max_strands = 1u << 20;
inline constexpr u32 k_max_guides = 4096;
// The furthest from the origin a groom's point lies, on each axis (metres).
inline constexpr float k_max_groom_reach = 1.0e6f;

// The guides a strand follows (indices below Groom::guide_count) and how
// much of each, summing to 1; unused slots weigh 0. A guide follows itself.
struct StrandFollow {
    std::array<u32, k_strand_guides> guides{};
    std::array<float, k_strand_guides> weights{1.0f, 0.0f, 0.0f};
    bool operator==(const StrandFollow&) const = default;
};

struct Groom {
    std::vector<Vec3> points;         // strand_count() * k_strand_points, rest shape
    std::vector<Vec3> root_normals;   // per strand: the scalp's normal at the root, unit
    std::vector<u32> seeds;           // per strand: for variation in the look
    std::vector<StrandFollow> follow; // per strand
    u32 guide_count = 0;
    Vec3 bounds_min{};
    Vec3 bounds_max{};
    float longest = 0.0f; // the longest strand's length along its points

    u32 strand_count() const { return static_cast<u32>(seeds.size()); }
    std::span<const Vec3> strand(u32 index) const {
        return std::span<const Vec3>(points).subspan(static_cast<usize>(index) * k_strand_points,
                                                     k_strand_points);
    }
};

// What is wrong with a groom made elsewhere (sizes that disagree, guides out
// of range, weights that do not sum to 1, points that are no numbers or
// beyond k_max_groom_reach, root normals that are not unit), or empty when
// it can be used. What generate_groom and groom_from_strands return always
// passes.
std::string groom_problem(const Groom& groom);

struct GroomParams {
    u32 strands = 20000; // 1..k_max_strands
    u32 guides = 512;    // 1..k_max_guides, at most `strands`
    float length = 0.25f;           // metres, 0.001..10
    float length_variation = 0.2f;  // 0..1: strands are shorter by up to this share
    // 0..1: how hard a strand bends over toward straight down along its
    // length (0: stands on end along the scalp's normal, 1: hangs).
    float droop = 0.8f;
    float curl_radius = 0.0f; // metres, 0..1: the helix a strand winds round its line
    float curl_turns = 0.0f;  // 0..100: turns along a full-length strand
    float frizz = 0.0f;       // metres, 0..1: a strand's wander by its tip
    // 0..1: how far a strand's tip is drawn in to its nearest guide (locks).
    float clump = 0.0f;
    u32 seed = 1;
};

// Grows a groom from a scalp: roots spread evenly over the triangles' area,
// each strand leaving along the surface normal. Out-of-range values are
// clamped (a value that is no number takes its default or 0). Triangles
// with a bad index or a point beyond k_max_groom_reach grow nothing; a scalp
// without area gives an empty groom (no strands).
Groom generate_groom(const TriangleMesh& scalp, const GroomParams& params);

// A groom from strands made elsewhere (a file's curves): each is a line of
// two or more points, root first, resampled evenly along its length to
// k_strand_points. Strands with fewer than two points, no length or a point
// that is no number or beyond k_max_groom_reach are left out, and strands
// past k_max_strands. `guides` as GroomParams::guides.
// The root normal is the strand's first direction (+Y if it has none to
// speak of).
Groom groom_from_strands(std::span<const std::vector<Vec3>> strands, u32 guides, u32 seed = 1);

// One strand's points given the guides' posed points (guide_count *
// k_strand_points, as Groom::points holds them at rest): each point moves as
// its weighted guides' points moved from rest. With the guides at rest the
// strand is at rest.
void pose_strand(const Groom& groom, u32 strand, std::span<const Vec3> posed_guides,
                 std::span<Vec3, k_strand_points> out);

} // namespace giant::geometry
