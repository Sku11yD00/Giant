// Flying the viewport camera: hold the right mouse button over the
// viewport to look around, and use W A S D (Q / E down and up, Shift
// faster) while it is held; the mouse wheel changes speed. The usual
// editor controls, so artists feel at home.
#pragma once

#include "editor_commands.h"

#include "giant/platform/input.h"
#include "giant/render/renderer.h"

#include <array>
#include <vector>

namespace giant::editor {

class ViewportCamera {
public:
    // `ui_has_mouse`: the pointer is over a panel, so a click there is the
    // panel's, not the viewport's.
    void handle_input(const std::vector<platform::InputEvent>& events, bool ui_has_mouse);
    void update(EditorCamera& camera, float delta_seconds);
    bool flying() const { return m_flying; }

private:
    bool m_flying = false;
    bool m_have_mouse = false;
    float m_last_x = 0.0f;
    float m_last_y = 0.0f;
    float m_look_x = 0.0f; // mouse travel this frame, in points
    float m_look_y = 0.0f;
    float m_wheel = 0.0f;
    bool m_fast = false;
    std::array<bool, 6> m_move{}; // forward, back, left, right, down, up
};

// The renderer's view of the editor camera.
render::View to_view(const EditorCamera& camera);

} // namespace giant::editor
