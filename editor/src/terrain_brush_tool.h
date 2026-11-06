// The editor's terrain brush: with the tool on, holding the left mouse
// button over the viewport sculpts the ground under the pointer
// (sculpt_terrain, one call a frame, one undo step a stroke).
#pragma once

#include "editor_commands.h"

#include "giant/ai/commands.h"
#include "giant/platform/input.h"
#include "giant/world/terrain.h"

#include <optional>
#include <vector>

namespace giant::editor {

// The direction (unit) from the camera through a point of the window, in
// points from its top-left, for a window `width` x `height` points.
Vec3 camera_ray(const EditorCamera& camera, float x, float y, float width, float height);

// Where a ray first meets the ground (the terrain's tiles), within
// `max_distance` metres: marched at half the sample spacing near the
// origin (steps growing with the distance, a 500th of it), then refined.
std::optional<DVec3> terrain_ray_hit(const world::Terrain& terrain, const DVec3& origin, const Vec3& direction,
                                     double max_distance);

class TerrainBrushTool {
public:
    // Once a frame, after the UI took its input: while the tool is on
    // (state.brush.enabled), the left button held over the viewport (not a
    // panel) sculpts where the pointer meets the ground. The panel keeps the
    // radius to 128 samples (BrushSettings::max_radius), so a dab is a small
    // part of a frame.
    void update(const std::vector<platform::InputEvent>& events, bool ui_has_mouse, float window_width,
                float window_height, float delta_seconds, EditorState& state, ai::CommandRegistry& commands);
    static constexpr float k_dabs_per_second = 30.0f; // held still

    bool stroking() const { return m_down; }

private:
    bool m_down = false;
    float m_x = 0.0f;
    float m_y = 0.0f;
    u64 m_stroke = 0; // merge group of the stroke now
    u64 m_strokes = 0;
    std::optional<DVec3> m_last; // last dab of the stroke
    float m_since_dab = 0.0f;   // seconds
    float m_start_height = 0.0f; // the ground where the stroke began (flatten)
    // What the last ray was cast from: none is cast again until one changes.
    DVec3 m_camera{};
    float m_yaw = 0.0f;
    float m_pitch = 0.0f;
    float m_cast_x = -1.0f;
    float m_cast_y = -1.0f;
};

} // namespace giant::editor
