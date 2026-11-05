#include "viewport_camera.h"

#include <algorithm>
#include <cmath>

namespace giant::editor {

namespace {
using platform::InputEvent;
using platform::Key;

constexpr float k_degrees_per_point = 0.2f;

Quat orientation(const EditorCamera& camera) {
    return quat_from_euler({camera.pitch_degrees, camera.yaw_degrees, 0.0f});
}
} // namespace

void ViewportCamera::handle_input(const std::vector<InputEvent>& events, bool ui_has_mouse) {
    for (const InputEvent& e : events) {
        switch (e.type) {
        case InputEvent::Type::mouse_button:
            if (e.button == platform::MouseButton::right) {
                if (e.down && !ui_has_mouse) {
                    m_flying = true;
                    m_have_mouse = true;
                    m_last_x = e.x;
                    m_last_y = e.y;
                } else if (!e.down) {
                    m_flying = false;
                    m_move = {};
                }
            }
            break;
        case InputEvent::Type::mouse_move:
            if (m_flying && m_have_mouse) {
                m_look_x += e.x - m_last_x;
                m_look_y += e.y - m_last_y;
            }
            m_last_x = e.x;
            m_last_y = e.y;
            m_have_mouse = true;
            break;
        case InputEvent::Type::mouse_wheel:
            if (m_flying) {
                m_wheel += e.y;
            }
            break;
        case InputEvent::Type::key: {
            m_fast = (e.modifiers & platform::modifier::shift) != 0;
            // Releases always count, so a key let go over a panel never sticks.
            if (!m_flying && e.down) {
                break;
            }
            const auto set = [&](usize i) { m_move[i] = e.down; };
            switch (e.key) {
            case Key::w: set(0); break;
            case Key::s: set(1); break;
            case Key::a: set(2); break;
            case Key::d: set(3); break;
            case Key::q: set(4); break;
            case Key::e: set(5); break;
            default: break;
            }
            break;
        }
        case InputEvent::Type::focus:
            if (!e.down) { // focus lost: stop, as no release will arrive
                m_flying = false;
                m_move = {};
            }
            break;
        case InputEvent::Type::mouse_left:
            m_have_mouse = false;
            break;
        case InputEvent::Type::text: break;
        }
    }
}

void ViewportCamera::update(EditorCamera& camera, float delta_seconds) {
    camera.yaw_degrees -= m_look_x * k_degrees_per_point;
    camera.pitch_degrees =
        std::clamp(camera.pitch_degrees - m_look_y * k_degrees_per_point, -89.0f, 89.0f);
    camera.yaw_degrees = std::remainder(camera.yaw_degrees, 360.0f);
    m_look_x = 0.0f;
    m_look_y = 0.0f;
    if (m_wheel != 0.0f) {
        camera.speed = std::clamp(camera.speed * std::pow(1.25f, m_wheel), 0.1f, 10000.0f);
        m_wheel = 0.0f;
    }

    const Quat q = orientation(camera);
    const Vec3 forward = rotate(q, {0.0f, 0.0f, -1.0f});
    const Vec3 right = rotate(q, {1.0f, 0.0f, 0.0f});
    Vec3 move{};
    if (m_move[0]) move += forward;
    if (m_move[1]) move -= forward;
    if (m_move[2]) move -= right;
    if (m_move[3]) move += right;
    if (m_move[4]) move -= Vec3{0.0f, 1.0f, 0.0f};
    if (m_move[5]) move += Vec3{0.0f, 1.0f, 0.0f};
    move = normalize_or_zero(move);
    const double step = static_cast<double>(camera.speed * (m_fast ? 4.0f : 1.0f) *
                                            std::min(delta_seconds, 0.25f));
    camera.position += DVec3{move.x * step, move.y * step, move.z * step};
}

render::View to_view(const EditorCamera& camera) {
    render::View view;
    view.position = camera.position;
    view.orientation = orientation(camera);
    view.vertical_fov_degrees = camera.vertical_fov_degrees;
    view.near_plane = 0.05f;
    return view;
}

} // namespace giant::editor
