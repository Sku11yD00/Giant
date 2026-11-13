// Shadows for local lights (lights part L3): depth tiles in an atlas, one
// per spot light and six per point light (a cube round it), for the
// nearest lights in view that cast shadows (LocalLight::shadow_id). Tiles
// are kept from frame to frame by the light's id and drawn again only when
// the light changes or a caster near it comes, goes or moves (the sun
// shadow pages' changed boxes and posed skinned meshes), a few faces a
// frame (faces with nothing drawn first, nearest lights first, then those
// drawn longest ago);
// until a face is drawn the light shines unshadowed there. A light moved
// only a little (a lantern swinging) keeps shading with its old faces while
// they are drawn again.
//
// A face looks from the light down its axis F with right R and up
// U = cross(R, F), a square view `tan_half` across each way (a little over
// 45 degrees for a cube face, so filtering at its edge stays inside it);
// depth is near / (distance along F), larger nearer, as the sun's pages
// keep it. Cube faces: +x, -x, +y, -y, +z, -z, with R (0,0,1) for x and
// (1,0,0) for y and z. A spot's R is any direction across its axis (made
// here); shaders/lights.slang finds the same texels.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/render/renderer.h"
#include "gpu_layout.h"

#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace giant::render {

inline constexpr u32 k_local_shadow_size = 256; // texels across a tile
inline constexpr u32 k_max_shadowed_lights = 64; // per frame

// A light that may cast shadows this frame, nearest first.
struct ShadowCandidate {
    u32 light = 0;  // its index in the frame's binned lights (LightBins::lights)
    const LocalLight* source = nullptr;
};

class LocalShadows {
public:
    struct Face {
        u32 tile = 0;
        DVec3 position{};  // the light (world)
        Vec3 forward{};    // F
        Vec3 right{};      // R
        float tan_half = 1.0f;
        float near = 0.1f;
        float range = 1.0f; // the light's, for detail
        u32 first_tile = 0; // its light's first tile (the faces of one light share it)
    };
    // A shadowed light this frame: its GPU record goes beside the lights.
    struct Shadowed {
        u32 light = 0;        // index in the binned lights
        gpu::LightShadow record{};
    };

    explicit LocalShadows(u32 tiles) : m_tiles(tiles), m_owner(tiles, k_free) {}

    // Plans this frame: which lights keep or get tiles, which faces to draw
    // (at most `budget`). `changed`: the casters' changed boxes; `all`: all
    // casters changed.
    void plan(std::span<const ShadowCandidate> candidates, const DVec3& camera,
              std::span<const std::pair<DVec3, DVec3>> changed, bool all, u32 budget);
    void invalidate_all();
    // (draws() keeps each light's faces together, in face order.)
    // This frame's faces from `from` on could not be drawn: drawn again
    // later, their lights unshadowed there until then.
    void undo_draws(usize from = 0);

    const std::vector<Face>& draws() const { return m_draws; }
    const std::vector<Shadowed>& shadowed() const { return m_shadowed; }
    u32 tiles() const { return m_tiles; }
    u32 tiles_used() const;
    u32 waiting() const { return m_waiting; } // faces of this frame's lights not drawn yet (or stale)

    // The face set of a light: its axis and right (spot: from its direction).
    static void face_axes(LightKind kind, const Vec3& direction, u32 face, Vec3& forward, Vec3& right);
    static float tan_half(LightKind kind, float outer_angle);
    // Whether a face of a light at `light` may see the box [lo, hi] (the
    // box against the face's four sides; conservative).
    static bool face_sees_box(LightKind kind, const Vec3& direction, float outer, u32 face, const DVec3& light,
                              const DVec3& lo, const DVec3& hi);
    // Camera-relative world to the face's clip space (reverse depth, no far plane).
    static Mat4 face_matrix(const Face& face, const DVec3& camera);
    // The face's culling planes (inward; camera-relative): 4 sides and far
    // (at `range` along the face's axis).
    static void face_planes(const Face& face, const DVec3& camera, float out[20]);
    // Culling planes for all of a light's faces at once: the box round its
    // reach, open at the top (five planes; what is above it is mostly sky).
    static void light_planes(const Face& face, const DVec3& camera, float out[20]);

private:
    static constexpr u32 k_free = ~0u;
    struct Entry {
        u32 first = 0;      // first tile
        u32 count = 0;      // 1 or 6
        u32 drawn = 0;      // faces drawn (bit per face)
        u32 dirty = 0;      // faces to draw (again)
        u64 used = 0;       // last frame it was a candidate
        // What its tiles are for (this frame's light).
        DVec3 position{};
        Vec3 direction{};
        LightKind kind = LightKind::point;
        float range = 0.0f;
        float outer = 0.0f;
        float near = 0.0f;
        // Where each face was drawn from (a light moved a little keeps its
        // faces until drawn again; moved further, they are dropped).
        DVec3 face_position[6]{};
        Vec3 face_direction[6]{};
        Vec3 face_right{};     // face 0's right axis as drawn (a spot's)
        u64 face_frame[6]{};   // when each was last drawn
    };
    bool allocate(u32 count, u32& first);
    void release(u32 id);

    u32 m_tiles;
    std::vector<u32> m_owner; // per tile: the id using it, or k_free
    std::unordered_map<u32, Entry> m_entries;
    std::vector<Face> m_draws;
    std::vector<Shadowed> m_shadowed;
    u64 m_frame = 0;
    u32 m_waiting = 0;
};

} // namespace giant::render
