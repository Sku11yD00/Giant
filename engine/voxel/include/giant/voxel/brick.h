// Voxel bricks: the unit the shared voxel world is stored, streamed and
// mirrored on the GPU in. A brick is 8x8x8 voxels; each voxel is either
// empty or holds a surface colour and normal (a surface voxelization: the
// shell of what is there, which is what far geometry and lighting see).
//
// The layout is the GPU's (render/shaders/voxel_march.slang): 16 u32 of
// occupancy bits, one packed u32 per voxel, then 16 u32 of stand-in bits
// (voxels the far field may show: touched by small objects, which it
// stands in for, not by the big ones that stay meshes). Voxel (x, y, z)
// is index x + 8 * y + 64 * z.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"

#include <array>

namespace giant::voxel {

inline constexpr u32 k_brick_edge = 8;
inline constexpr u32 k_brick_voxels = k_brick_edge * k_brick_edge * k_brick_edge;

constexpr u32 voxel_index(u32 x, u32 y, u32 z) { return x + k_brick_edge * (y + k_brick_edge * z); }
// The share of a voxel water must cover to show over the ground there (BrickBuilder).
inline constexpr float k_water_share = 0.5f;

// A voxel's colour (linear RGB in [0, 1], stored as RGB565 of its square
// root, so darks keep their steps) and unit normal (octahedral, 8 bits per
// axis), in one u32: colour in the low 16 bits.
u32 pack_voxel(const Vec3& color, const Vec3& normal);
Vec3 unpack_color(u32 voxel);
Vec3 unpack_normal(u32 voxel); // unit (a zero normal packs as +Y; water's is +Y)
// Water's surface (lakes and rivers: a mirror the shaders treat as one,
// water plan W5b): its colour (what it scatters back) and a normal code
// pack_voxel never makes (both octahedral bytes 0, a -Z pack_voxel writes
// as 255, 255), facing up.
u32 pack_water_voxel(const Vec3& color);
constexpr bool water_voxel(u32 voxel) { return (voxel >> 16) == 0u; }

// A voxel's light (the GPU's light pool, render/shaders/voxel_gi.slang):
// the bounce and sky light reaching it (linear RGB, 9-bit mantissas over a
// shared 4-bit exponent, up to about 64) and whether the sun reaches it, in
// one u32. 0 (exponent 0) means not lit yet.
inline constexpr u32 k_unlit = 0;
inline constexpr float k_max_light = 63.875f;
u32 pack_light(const Vec3& light, bool sun);
Vec3 unpack_light(u32 light);
constexpr bool light_lit(u32 light) { return (light >> 27 & 15u) != 0; }
constexpr bool light_sun(u32 light) { return (light >> 31) != 0; }

struct Brick {
    std::array<u32, k_brick_voxels / 32> occupancy{}; // bit i % 32 of word i / 32
    std::array<u32, k_brick_voxels> voxels{};         // pack_voxel; 0 where empty
    std::array<u32, k_brick_voxels / 32> stand_in{};  // a subset of occupancy

    bool occupied(u32 index) const { return (occupancy[index / 32] >> (index % 32) & 1u) != 0; }
    bool stands_in(u32 index) const { return (stand_in[index / 32] >> (index % 32) & 1u) != 0; }
    void set(u32 index, u32 voxel, bool stands = false) {
        occupancy[index / 32] |= 1u << (index % 32);
        if (stands) {
            stand_in[index / 32] |= 1u << (index % 32);
        }
        voxels[index] = voxel;
    }
    u32 count() const;
    bool empty() const { return count() == 0; }
};
static_assert(sizeof(Brick) == 2176, "the GPU reads 2176-byte bricks");

// True if the triangle touches the axis-aligned box (separating axis test,
// Akenine-Moller): conservative, so a surface voxelized with it has no holes.
bool triangle_overlaps_box(const Vec3& box_center, const Vec3& half_size, const Vec3& a, const Vec3& b,
                           const Vec3& c);

// Builds one brick from triangles. Positions are in the brick's frame:
// its minimum corner at the origin, voxels `voxel_size` wide. Each voxel a
// triangle touches gets the triangle's colour and facing (the average of
// every triangle touching it; where stand-in triangles touch it, theirs
// only, so a far tree keeps its colour over the ground it stands on).
//
// Cut-out triangles (coverage under 1: leaves and grass whose texture cuts
// most of them away) fill a voxel only as often as they would hide what is
// behind it: each adds its coverage times its area in the voxel (its area
// shared evenly among the voxels it touches) over the voxel's face, and a
// voxel only they touch is filled when that sum reaches a threshold
// dithered per voxel (stable across rebuilds by `seed`). A thin grass card
// leaves a sparse voxel field; a dense clump, a solid one. Their colour and
// facing count by coverage.
class BrickBuilder {
public:
    // Empties it (cheap: clears only the voxels touched since the last
    // reset). `seed`: picks the dither (the brick's place, so a brick built
    // again comes out the same).
    void reset(float voxel_size, u32 seed = 0);
    // Returns how many voxels it tested (the work done, for budgets).
    // `stand_in`: from a small object the far field stands in for.
    // `coverage`: the share of the surface its texture leaves (1: solid);
    // 0 or less (or NaN) adds nothing. `facing`: the way it faces for
    // lighting, if not its own (leafy crowns bend theirs outward); zero or
    // not finite: its own.
    u32 add_triangle(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& color, bool stand_in = false,
                     float coverage = 1.0f, const Vec3& facing = {});
    // Water's surface (either winding): a voxel it covers at least
    // k_water_share of (its area in the voxel over a face) is water
    // (pack_water_voxel, its colour) over what else is there (the bed at
    // the shore), unless a stand-in's (a far tree over the water); less,
    // and what else is there shows (a narrow river in a coarse voxel of
    // its valley), water only where nothing else is (a voxel it only
    // touches, at its edge, is not water).
    u32 add_water_triangle(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& color);
    bool empty() const { return m_touched == 0; }
    Brick build() const;
    // The dither threshold of voxel `index` under the current seed, in (0, 1].
    float threshold(u32 index) const;

private:
    struct Part {
        Vec3 color{};
        Vec3 normal{};
        float weight = 0.0f;
        float cover = 0.0f; // cut-out triangles' coverage times area, over the voxel's face
        bool solid = false; // a triangle with full coverage touches it
    };
    struct Sum {
        Part own;   // triangles of objects that stay meshes
        Part stand; // the stand-in triangles'
        Part water; // water's surface
        bool touched = false;
    };
    void add(u32 index, const Vec3& color, const Vec3& normal, float coverage, float cover, bool stand_in);
    bool fills(const Part& part, u32 index) const;
    std::array<Sum, k_brick_voxels> m_sums{};
    std::array<u16, k_brick_voxels> m_list{}; // the touched voxels, so reset clears only them
    float m_voxel_size = 1.0f;
    bool m_water = false; // add_triangle adds water (add_water_triangle)
    u32 m_seed = 0;
    u32 m_touched = 0;
};

} // namespace giant::voxel
