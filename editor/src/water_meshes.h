// The scene's lakes, rivers and sea as the renderer's water triangles
// (render::FrameInput::water_*, water plan W2c): a lake's shore cut into
// triangles at its level (ear clipping), a river as a strip down its path
// (each point's width, level and flow), the sea as rings round the camera
// out past the horizon. Each body's triangles are kept and made again only
// when that body's shape changes (its look and a lake's level are put on
// as they are) (the scene's water is compared: its edits come
// from many places); the sea's rings follow the camera every update. The
// water pass draws over itself in order, so bodies are put lowest first:
// where surfaces overlap the highest shows, as the physics has it. Each
// visible body has its own look.
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
// or the camera is more than k_camera_water_margin over every surface.
inline constexpr double k_camera_water_margin = 2.0;
render::Underwater camera_water(std::span<const world::Water> water, const DVec3& camera);

class WaterMeshes {
public:
    // The sea's rings: from the camera out to this far (metres), then one
    // more ring this far (past where a flat sea's edge shows from any height
    // the camera goes).
    static constexpr double k_sea_radius = 40000.0;
    static constexpr double k_sea_edge = 2.0e6;
    static constexpr u32 k_sea_segments = 64;

    // Follows the scene's water (bodies that changed made again) and the camera.
    void update(std::span<const world::Water> water, const DVec3& camera);

    std::span<const render::WaterVertex> vertices() const { return m_vertices; }
    std::span<const u32> indices() const { return m_indices; }
    std::span<const render::WaterLook> looks() const { return m_looks; }
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
    void place_seas(const DVec3& camera);
    std::vector<Body> m_bodies; // as the scene has them
    std::vector<world::Water> m_seen;
    bool m_built = false;
    std::vector<render::WaterVertex> m_vertices;
    std::vector<u32> m_indices;
    std::vector<render::WaterLook> m_looks;
    struct Sea {
        float level;
        u32 look;
        usize first_vertex; // its rings' vertices, rewritten as the camera moves
    };
    std::vector<Sea> m_seas;
    std::vector<double> m_radii; // the sea's rings, from 0
    u32 m_rebuilds = 0;
    u32 m_broken = 0;
};

} // namespace giant::editor
