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

// A voxel's colour (linear RGB in [0, 1], stored as RGB565 of its square
// root, so darks keep their steps) and unit normal (octahedral, 8 bits per
// axis), in one u32: colour in the low 16 bits.
u32 pack_voxel(const Vec3& color, const Vec3& normal);
Vec3 unpack_color(u32 voxel);
Vec3 unpack_normal(u32 voxel); // unit (a zero normal packs as +Y)

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
class BrickBuilder {
public:
    // Empties it (cheap: clears only the voxels touched since the last reset).
    void reset(float voxel_size);
    // Returns how many voxels it tested (the work done, for budgets).
    // `stand_in`: from a small object the far field stands in for.
    u32 add_triangle(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& color, bool stand_in = false);
    bool empty() const { return m_touched == 0; }
    Brick build() const;

private:
    struct Sum {
        Vec3 color{};
        Vec3 normal{};
        float weight = 0.0f;
        Vec3 stand_color{}; // the stand-in triangles' alone
        Vec3 stand_normal{};
        float stand_weight = 0.0f;
    };
    std::array<Sum, k_brick_voxels> m_sums{};
    std::array<u16, k_brick_voxels> m_list{}; // the touched voxels, so reset clears only them
    float m_voxel_size = 1.0f;
    u32 m_touched = 0;
};

} // namespace giant::voxel
