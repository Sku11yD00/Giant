// Hair (characters plan C6c): the grooms the renderer keeps, and what a
// frame draws of them. The GPU side is shaders/hair.slang.
//
// A groom is packed once (pack_groom): its strands' guides and seeds, then
// every point as 16 bits an axis across the groom's bounds (steps of 5
// thousandths of a millimetre for a head of hair 0.3 m across). Its bytes go to the GPU
// over the frames after it is added, under a budget (rule 9), and it draws
// once they are all there.
//
// A frame draws the nearest grooms in view (plan_hair). Strands are far
// thinner than a pixel, so how many are drawn follows the size on screen:
// as many as make each about 1 / detail of a pixel wide, the first of the
// groom (its strands are in an order whose every prefix covers the scalp
// evenly), each widened by the share left out, so the hair covers the
// same. Pieces shorter than a few pixels are joined (a strand's 15 pieces
// drawn as 5, 3 or 1).
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/geometry/groom.h"
#include "giant/render/renderer.h"
#include "giant/rhi/rhi.h"
#include "gpu_layout.h"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace giant::render {

inline constexpr u32 k_groom_slot_mask = (1u << 20) - 1;
inline constexpr u32 k_groom_generations = 4095; // generations 1 to 4094: ~0u stays k_no_groom
// A groom smaller on screen than this many pixels is not drawn.
inline constexpr float k_hair_least_pixels = 2.0f;
// The fewest strands a drawn groom keeps (or all it has).
inline constexpr u32 k_hair_least_strands = 64;
// The guides a frame takes as simulated, in bytes (12 a point: over 1300
// grooms' worth of 16 guides, or 20 of 512); the grooms past it, furthest
// first, draw at rest.
inline constexpr usize k_hair_posed_bytes = 4u << 20;
// Pieces are joined until one is this many pixels long.
inline constexpr float k_hair_piece_pixels = 3.0f;
// The sun's shadows of hair on itself (C6d): a square of tiles, one a
// drawn groom, each spanning the ball round its groom as the sun sees it.
inline constexpr u32 k_hair_shadow_tile = 128;                      // texels a side
inline constexpr u32 k_hair_shadow_tiles_across = 16;               // 16 x 16 tiles: k_max_hair_draws
inline constexpr u32 k_hair_shadow_size = k_hair_shadow_tile * k_hair_shadow_tiles_across;
static_assert(k_hair_shadow_tiles_across * k_hair_shadow_tiles_across >= k_max_hair_draws);
// The most strands of a groom drawn into its tile (its first, each
// standing for the rest), and the points a piece of them spans at least.
inline constexpr u32 k_hair_shadow_strands = 4096;
inline constexpr u32 k_hair_shadow_stride = 3;
// The grooms whose hair shades the world (the nearest drawn).
inline constexpr u32 k_hair_casters = 16;

struct PackedGroom {
    std::vector<u8> bytes; // gpu::HairStrand a strand, then the points; empty: not a groom to draw
    u32 strands = 0;       // kept
    u32 authored = 0;      // the groom's own count (more than kept: each kept one stands for several)
    u32 guides = 0;
    u32 points_offset = 0;
    Vec3 bounds_min{};
    Vec3 bounds_size{};
    float longest = 0.0f;
};

// Empty for a groom with a groom_problem or no strands. Strands past
// k_max_groom_strands are left out.
PackedGroom pack_groom(const geometry::Groom& groom);

// A groom packed away from the renderer's thread (Renderer::prepare_groom).
struct PreparedGroom {
    PackedGroom packed;
    std::string problem; // why `packed` is empty
};

// A packed point back in the groom's space (as the shader reads it).
Vec3 unpack_point(const PackedGroom& groom, u32 strand, u32 point);

struct HairLod {
    u32 strands = 0; // drawn
    u32 stride = 1;  // points a piece spans: 1, 3, 5 or 15
    float widen = 1.0f; // each strand's width times this
};

// What to draw of a groom of `strands` strands, `width` metres across
// each and `longest` metres long, where a metre is `pixels_per_metre`
// pixels; `most` strands at most (0: all).
HairLod hair_lod(u32 strands, float width, float longest, float pixels_per_metre, float detail, u32 most);

struct GroomSlot {
    u32 generation = 1;
    bool used = false;
    PackedGroom groom;         // bytes emptied once uploaded
    rhi::BufferHandle buffer;
    u64 size = 0;              // of the buffer
    u64 uploaded = 0;          // bytes sent so far
};

class Grooms {
public:
    // The slot's id, or k_no_groom when full (k_max_grooms).
    GroomId add(PackedGroom groom);
    // Null for an unknown or removed id.
    GroomSlot* find(GroomId id);
    const GroomSlot* find(GroomId id) const;
    void remove(GroomId id);
    // Slots with bytes still to send, oldest first.
    std::vector<u32>& waiting() { return m_waiting; }
    GroomSlot& slot(u32 index) { return m_slots[index]; }
    usize count() const { return m_count; }

private:
    std::vector<GroomSlot> m_slots;
    std::vector<u32> m_free;
    std::vector<u32> m_waiting;
    usize m_count = 0;
};

// One groom of a frame's plan.
struct HairPlanned {
    u32 draw = 0;  // index into the frame's HairDraw
    u32 slot = 0;  // its groom's slot
    HairLod lod;
    u32 first = 0; // its first strand among the frame's
    bool posed = false; // its guides are given, and good (and fit the frame's k_hair_posed_bytes)
    double distance = 0.0;
    Vec3 centre{};      // camera-relative: the middle of the ball its shadow tile spans
    float reach = 0.0f; // that ball's radius: round its guides as posed (or its bounds at rest)
};

struct HairPlan {
    std::vector<HairPlanned> grooms; // nearest first
    u32 strands = 0;
    u32 segments = 0;
    u32 left_out = 0;
};

struct HairView {
    DVec3 position{};
    std::array<Vec4, 5> planes{}; // camera-relative, inward (as gpu::Frame's)
    float pixels_per_metre_at_one = 0.0f; // pixels a metre spans one metre away
    float detail = 3.0f;
};

// What this frame draws of `draws`. Reuses `plan`'s memory.
void plan_hair(const Grooms& grooms, std::span<const HairDraw> draws, const HairView& view, HairPlan& plan);

// A planned groom as the shaders take it.
gpu::HairGroom to_gpu(const HairPlanned& planned, const GroomSlot& slot, const HairDraw& draw, const DVec3& camera);

} // namespace giant::render
