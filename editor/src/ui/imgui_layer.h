// Dear ImGui glue: feeds platform input to ImGui and turns its draw data into
// the graphics layer's plain UI frames. The editor's panels draw with ImGui
// between begin_frame and end_frame.
#pragma once

#include "giant/platform/input.h"
#include "giant/platform/window.h"
#include "giant/rhi/rhi.h"

#include <memory>
#include <vector>

struct ImGuiContext;

namespace giant::editor {

class ImGuiLayer {
public:
    // Null (and logged) if the font texture cannot be created.
    // `ini_path` stores the panel layout; null keeps it in memory only.
    static std::unique_ptr<ImGuiLayer> create(platform::Window& window, rhi::Device& device,
                                              const char* ini_path);
    ~ImGuiLayer();

    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    // Starts a UI frame: hands ImGui this frame's input and display size.
    void begin_frame(float delta_seconds, const std::vector<platform::InputEvent>& input);
    // Finishes the UI frame and converts it for drawing. Updates the mouse
    // cursor and text input to what ImGui wants.
    void end_frame();
    // The UI built by the last end_frame, in pixels.
    rhi::UiFrame frame() const { return {m_lists}; }

    // True when the UI wants the keyboard (a text field has focus), so
    // shortcuts and game input should leave it alone.
    bool wants_keyboard() const;

    struct ConstructKey {
    private:
        friend class ImGuiLayer;
        ConstructKey() = default;
    };
    ImGuiLayer(ConstructKey, platform::Window& window, rhi::Device& device);

private:
    platform::Window& m_window;
    rhi::Device& m_device;
    ImGuiContext* m_context = nullptr;
    rhi::TextureId m_font_texture = 0;

    // Converted draw data; m_lists views into the other three.
    std::vector<rhi::UiVertex> m_vertices;
    std::vector<u32> m_indices;
    std::vector<rhi::UiDrawCommand> m_commands;
    std::vector<rhi::UiDrawList> m_lists;
};

} // namespace giant::editor
