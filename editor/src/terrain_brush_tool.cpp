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
    // Half a sample apart near the camera, growing with the distance (a
    // 500th of it, under a pixel's worth of slope error), so a ray to the
    // horizon is a few thousand steps, not tens of thousands.
    const double near_step = std::max(terrain.spacing() * 0.5, 0.05);
    const auto step_at = [&](double s) { return std::max(near_step, s * 0.002); };
    std::optional<double> prev = above(0.0);
    if (prev && *prev <= 0.0) return at(0.0); // starts under the ground
    double prev_s = 0.0;
    for (double s = step_at(0.0); s <= max_distance; s += step_at(s)) {
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
    const BrushSettings& b = state.brush;
    // The ray is cast only when a dab may follow: the pointer or camera
    // moved, or it is time for the next dab held still.
    const bool due = !m_last || m_since_dab >= 1.0f / k_dabs_per_second;
    const bool camera_moved = state.camera.position.x != m_camera.x || state.camera.position.y != m_camera.y ||
                              state.camera.position.z != m_camera.z || state.camera.yaw_degrees != m_yaw ||
                              state.camera.pitch_degrees != m_pitch;
    if (!due && !camera_moved && m_x == m_cast_x && m_y == m_cast_y) return;
    m_camera = state.camera.position;
    m_yaw = state.camera.yaw_degrees;
    m_pitch = state.camera.pitch_degrees;
    m_cast_x = m_x;
    m_cast_y = m_y;
    const auto ray = [&](float px, float py) { return camera_ray(state.camera, px, py, window_width, window_height); };
    const auto hit = terrain_ray_hit(state.scene.terrain, state.camera.position, ray(m_x, m_y), 20000.0);
    if (!hit) return;
    // Held still, it dabs k_dabs_per_second (whatever the frame rate); moved,
    // a dab every quarter radius along the way.
    const double gone = m_last ? std::hypot(hit->x - m_last->x, hit->z - m_last->z) : 0.0;
    const bool moved = m_last && gone >= b.radius * 0.25;
    if (m_last && !moved && !due) return;
    if (!m_last) m_start_height = static_cast<float>(hit->y);
    m_since_dab = 0.0f;
    // A hit that jumped (the pointer over a ridge, onto ground behind it;
    // or the camera flown) starts the line again where it lands, so no line
    // is drawn over ground the pointer never went over. A move of over 4
    // radii is bisected along the pointer's path (in the window), keeping
    // the larger half: over smooth ground the gap shrinks with each half
    // (however many metres a pixel covers there), over a ridge it stays.
    std::vector<std::vector<DVec3>> lines(1);
    if (moved) {
        const bool camera_same = state.camera.position.x == m_dab_camera.x &&
                                 state.camera.position.y == m_dab_camera.y &&
                                 state.camera.position.z == m_dab_camera.z &&
                                 state.camera.yaw_degrees == m_dab_yaw && state.camera.pitch_degrees == m_dab_pitch;
        const double jump = b.radius * 4.0;
        lines.back().push_back(*m_last);
        if (!camera_same) {
            if (gone > jump) lines.emplace_back();
        } else if (gone > jump) {
            DVec3 a = *m_last, z = *hit;
            float ax = m_dab_x, ay = m_dab_y, zx = m_x, zy = m_y;
            bool ridge = false;
            for (int level = 0; level < k_max_bisections; ++level) {
                if (std::hypot(zx - ax, zy - ay) < 0.5f) break; // under a pixel apart
                const float mx = 0.5f * (ax + zx), my = 0.5f * (ay + zy);
                const auto mid = terrain_ray_hit(state.scene.terrain, state.camera.position, ray(mx, my), 20000.0);
                if (!mid) { // no ground between: not one line
                    ridge = true;
                    break;
                }
                if (std::hypot(mid->x - a.x, mid->z - a.z) >= std::hypot(z.x - mid->x, z.z - mid->z)) {
                    z = *mid, zx = mx, zy = my;
                } else {
                    a = *mid, ax = mx, ay = my;
                }
            }
            const double left = std::hypot(z.x - a.x, z.z - a.z);
            ridge = ridge || (left > jump && left > 0.5 * gone);
            if (ridge) {
                if (a.x != m_last->x || a.z != m_last->z) lines.back().push_back(a);
                lines.emplace_back();
                if (z.x != hit->x || z.z != hit->z) lines.back().push_back(z);
            }
        }
    }
    lines.back().push_back(*hit);
    for (usize k = 0; k < lines.size(); ++k) {
        const bool continues = k == 0 && moved;
        ai::Json points = ai::Json::array();
        for (const DVec3& p : lines[k]) points.push_back({p.x, p.z});
        if (continues && points.size() == 1) continue; // only the last dab: nothing new
        ai::Json args = {{"op", b.op},           {"points", std::move(points)},
                         {"radius", b.radius},   {"strength", b.strength},
                         {"falloff", b.falloff}, {"continues", continues}};
        if (b.op == "flatten") args["height"] = b.flatten_to_start ? m_start_height : b.height;
        const ai::CommandResult result = commands.execute("sculpt_terrain", args, {m_stroke, true});
        if (!result.ok) {
            log_warning("editor", "sculpt_terrain: " + result.error);
            m_down = false;
            return;
        }
    }
    m_last = hit;
    m_dab_x = m_x;
    m_dab_y = m_y;
    m_dab_camera = state.camera.position;
    m_dab_yaw = state.camera.yaw_degrees;
    m_dab_pitch = state.camera.pitch_degrees;
}

} // namespace giant::editor
