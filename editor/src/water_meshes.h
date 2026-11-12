// The scene's lakes, rivers and sea as the renderer's water
// (render::FrameInput::water_* and seas, water plan W2c): a lake's shore
// cut into triangles at its level (ear clipping), a river as a strip down
// its path (each point's width, level and flow; rapids where it falls
// steeply), the sea as a render::SeaSurface (its level and look: the
// renderer draws its rings round the camera, between the triangles lower
// than it and the rest, which show over it). Each body's triangles are kept and made again
// only when that body's shape changes (its look and a lake's level are put
// on as they are) (the scene's water is compared: its edits come from many
// places). The water pass draws over itself in order, so bodies are put
// lowest first: where surfaces overlap the highest shows, as the physics
// has it. Each visible body has its own look.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/render/renderer.h"
#include "giant/world/scene.h"

#include <span>
#include <vector>

namespace giant::editor {

// A simple polygon's triangles (x, z; either winding), as index triples
// into `points`, by ear clipping. A shore that crosses itself (or has no
// area, or fewer than 3 points) gives none and `whole` false; points on
// top of the one before are dropped.
std::vector<u32> clip_ears(std::span<const world::WaterPoint> points, bool& whole);

// A body's look as the renderer takes it.
render::WaterLook water_look(const world::Water& water);

// The water the camera is in (render::FrameInput::underwater, water plan
// W3): the body over the camera's place (x, z) where its surface wins
// (world::water_at), whether the camera is under it or not (the renderer
// splits the view where the near plane meets it); off where there is none,
// or the camera is more than k_camera_water_margin over every surface (a
// sea's over its highest crest: `swell_most` metres, world::swell_most,
// times its waves). The sea rides the swell (render::Underwater::swell).
inline constexpr double k_camera_water_margin = 2.0;
render::Underwater camera_water(std::span<const world::Water> water, const DVec3& camera, float swell_most = 0.0f);

class WaterMeshes {
public:
    // A river's rapids (render::WaterVertex::rapids), each stretch's own
    // (its corners its own): none where its surface falls less than this a
    // metre, the most from k_rapids_full (a waterfall too).
    static constexpr double k_rapids_from = 0.02;
    static constexpr double k_rapids_full = 0.1;
    // A river's surface slope given to the renderer (render::WaterVertex::slope_*,
    // a waterfall's too) is at most this (metres a metre).
    static constexpr double k_slope_most = 10.0;

    // Follows the scene's water (bodies that changed made again).
    void update(std::span<const world::Water> water);

    std::span<const render::WaterVertex> vertices() const { return m_vertices; }
    std::span<const u32> indices() const { return m_indices; }
    std::span<const render::WaterLook> looks() const { return m_looks; }
    std::span<const render::SeaSurface> seas() const { return m_seas; }
    // Of indices(), how many come before the seas (bodies lower than the
    // lowest sea: drawn under it, render::FrameInput::water_indices_under_seas).
    u32 indices_under_seas() const { return m_under_seas; }
    // Changes whenever vertices(), indices() or looks() do (render::FrameInput::water_version).
    u64 version() const { return m_version; }
    u32 rebuilds() const { return m_rebuilds; } // bodies cut again (lakes and rivers), so far
    u32 broken_lakes() const { return m_broken; } // lakes not drawn: their shore crosses itself (or has no area)

private:
    // One body's own triangles, kept while it stays the same.
    struct Body {
        world::Water water;
        std::vector<render::WaterVertex> vertices; // look 0 (set when put together)
        std::vector<u32> indices;                  // into `vertices`
        bool broken = false;
    };
    void assemble();
    std::vector<Body> m_bodies; // as the scene has them
    std::vector<world::Water> m_seen;
    bool m_built = false;
    u64 m_version = 0;
    std::vector<render::WaterVertex> m_vertices;
    std::vector<u32> m_indices;
    std::vector<render::WaterLook> m_looks;
    std::vector<render::SeaSurface> m_seas; // lowest first
    u32 m_under_seas = 0;
    u32 m_rebuilds = 0;
    u32 m_broken = 0;
};

} // namespace giant::editor
