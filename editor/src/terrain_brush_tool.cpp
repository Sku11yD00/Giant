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
    const auto cast = [&](float px, float py) {
        return terrain_ray_hit(state.scene.terrain, state.camera.position,
                               camera_ray(state.camera, px, py, window_width, window_height),
                               20000.0);
    };
    const auto hit = cast(m_x, m_y);
    if (!hit) return;
    // Held still, it dabs k_dabs_per_second (whatever the frame rate); moved,
    // a dab every quarter radius along the way.
    const double gone = m_last ? std::hypot(hit->x - m_last->x, hit->z - m_last->z) : 0.0;
    const bool moved = m_last && gone >= b.radius * 0.25;
    if (m_last && !moved && !due) return;
    if (!m_last) m_start_height = static_cast<float>(hit->y);
    m_since_dab = 0.0f;
    // The line from the last dab follows the ground the pointer went over:
    // a far move is cast again at points between (in the window), and a
    // jump over 4 radii between two of them (the pointer over a ridge) or
    // since the camera moved (flown) starts the line again where it lands.
    std::vector<std::vector<DVec3>> lines(1);
    if (moved) {
        const bool camera_same = state.camera.position.x == m_dab_camera.x &&
                                 state.camera.position.y == m_dab_camera.y &&
                                 state.camera.position.z == m_dab_camera.z &&
                                 state.camera.yaw_degrees == m_dab_yaw &&
                                 state.camera.pitch_degrees == m_dab_pitch;
        const double jump = b.radius * 4.0;
        if (camera_same && gone > jump) {
            // The pointer's path, cast every few pixels: a gap over 4 radii
            // that is also 4 times its neighbours' is a ridge passed over
            // (a smooth drag's gaps change slowly, even seen at a grazing
            // angle).
            std::vector<DVec3> path{*m_last};
            const float pixels = std::hypot(m_x - m_dab_x, m_y - m_dab_y);
            const int parts =
                std::clamp(static_cast<int>(std::ceil(pixels / 4.0f)), 1, k_max_casts_per_frame);
            for (int k = 1; k < parts; ++k) {
                const float f = static_cast<float>(k) / static_cast<float>(parts);
                if (const auto between =
                        cast(m_dab_x + (m_x - m_dab_x) * f, m_dab_y + (m_y - m_dab_y) * f)) {
                    path.push_back(*between);
                }
            }
            path.push_back(*hit);
            const auto gap = [&](usize k) { // from path[k - 1] to path[k]; 0 off the ends
                if (k == 0 || k >= path.size()) return 0.0;
                return std::hypot(path[k].x - path[k - 1].x, path[k].z - path[k - 1].z);
            };
            lines.back().push_back(path[0]);
            for (usize k = 1; k < path.size(); ++k) {
                const double g = gap(k);
                const bool ridge = g > jump && g > 4.0 * std::max(gap(k - 1), gap(k + 1));
                if (ridge) lines.emplace_back();
                if (k + 1 < path.size()) lines.back().push_back(path[k]);
            }
        } else {
            lines.back().push_back(*m_last);
            if (gone > jump) lines.emplace_back();
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
