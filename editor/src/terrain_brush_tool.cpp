#include "terrain_brush_tool.h"

#include "viewport_camera.h"

#include "giant/core/log.h"

#include <algorithm>
#include <cmath>

namespace giant::editor {

Vec3 camera_ray(const EditorCamera& camera, float x, float y, float width, float height) {
    const render::View view = to_view(camera);
    const float aspect = height > 0.0f ? width / height : 1.0f;
    const float t = std::tan(view.vertical_fov_degrees * 0.5f * 3.14159265358979f / 180.0f);
    const float nx = width > 0.0f ? (2.0f * x / width - 1.0f) : 0.0f;
    const float ny = height > 0.0f ? (1.0f - 2.0f * y / height) : 0.0f;
    return normalize_or_zero(rotate(view.orientation, Vec3{nx * t * aspect, ny * t, -1.0f}));
}

std::optional<DVec3> terrain_ray_hit(const world::Terrain& terrain, const DVec3& origin, const Vec3& direction,
                                     double max_distance) {
    if (terrain.empty()) return std::nullopt;
    const DVec3 d{direction.x, direction.y, direction.z};
    const auto at = [&](double s) { return DVec3{origin.x + d.x * s, origin.y + d.y * s, origin.z + d.z * s}; };
    // Above the ground (positive), below (negative), or no ground there.
    const auto above = [&](double s) -> std::optional<double> {
        const DVec3 p = at(s);
        const auto h = terrain.height_at(p.x, p.z);
        if (!h || std::isnan(*h)) return std::nullopt;
        return p.y - *h;
    };
    const double step = std::max(terrain.spacing() * 0.5, 0.05);
    const double limit = std::min(max_distance, step * 200000.0);
    std::optional<double> prev = above(0.0);
    if (prev && *prev <= 0.0) return at(0.0); // starts under the ground
    double prev_s = 0.0;
    for (double s = step; s <= limit; s += step) {
        const std::optional<double> now = above(s);
        if (now && prev && *now <= 0.0) {
            double a = prev_s;
            double b = s;
            for (int k = 0; k < 24; ++k) {
                const double m = 0.5 * (a + b);
                const std::optional<double> v = above(m);
                if (v && *v > 0.0) {
                    a = m;
                } else {
                    b = m;
                }
            }
            return at(b);
        }
        prev = now;
        prev_s = s;
    }
    return std::nullopt;
}

void TerrainBrushTool::update(const std::vector<platform::InputEvent>& events, bool ui_has_mouse, float window_width,
                              float window_height, float delta_seconds, EditorState& state,
                              ai::CommandRegistry& commands) {
    m_since_dab += delta_seconds;
    for (const platform::InputEvent& e : events) {
        if (e.type == platform::InputEvent::Type::mouse_move || e.type == platform::InputEvent::Type::mouse_button) {
            m_x = e.x;
            m_y = e.y;
        }
        if (e.type == platform::InputEvent::Type::mouse_button && e.button == platform::MouseButton::left) {
            if (e.down && !ui_has_mouse && state.brush.enabled) {
                m_down = true;
                m_stroke = (u64{1} << 63) | ++m_strokes; // apart from the panels' groups
                m_last.reset();
            } else if (!e.down) {
                m_down = false;
            }
        }
        if (e.type == platform::InputEvent::Type::mouse_left || e.type == platform::InputEvent::Type::focus) {
            if (!e.down) m_down = false;
        }
    }
    if (!state.brush.enabled) m_down = false;
    if (!m_down) return;
    const Vec3 ray = camera_ray(state.camera, m_x, m_y, window_width, window_height);
    const auto hit = terrain_ray_hit(state.scene.terrain, state.camera.position, ray, 20000.0);
    if (!hit) return;
    const BrushSettings& b = state.brush;
    // Held still, it dabs k_dabs_per_second (whatever the frame rate); moved,
    // a dab every quarter radius along the way.
    const bool moved = m_last && std::hypot(hit->x - m_last->x, hit->z - m_last->z) >= b.radius * 0.25;
    if (m_last && !moved && m_since_dab < 1.0f / k_dabs_per_second) return;
    m_since_dab = 0.0f;
    ai::Json points = ai::Json::array();
    if (moved) points.push_back({m_last->x, m_last->z}); // the stroke goes on from the last dab
    points.push_back({hit->x, hit->z});
    ai::Json args = {{"op", b.op}, {"points", std::move(points)}, {"radius", b.radius}, {"strength", b.strength},
                     {"falloff", b.falloff}, {"continues", moved}};
    if (b.op == "flatten") args["height"] = b.height;
    const ai::CommandResult result = commands.execute("sculpt_terrain", args, {m_stroke, true});
    if (!result.ok) {
        log_warning("editor", "sculpt_terrain: " + result.error);
        m_down = false;
        return;
    }
    m_last = hit;
}

} // namespace giant::editor
