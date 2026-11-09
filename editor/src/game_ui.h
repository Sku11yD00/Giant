// The game's own UI in the editor (engine/ui): drawn over the scene, under
// the editor's panels, while playing (or always, or never: `show`). For
// now a crosshair, short messages (toasts: "Saved", or what show_toast
// asks), and a mark while a save is being written; screens from data come
// next (docs/architecture.md, "Game UI"). Built each frame after the
// frame's commands: a command's change shows from the next frame.
//
// Sized by the window: 1 UI unit is a pixel at 1080 lines high (scaled by
// height / 1080). The font's atlas is made and uploaded once, when this is
// made; a frame's UI is one canvas (a few draw calls). View state: nothing
// of it is saved or undone.
#pragma once

#include "giant/core/types.h"
#include "giant/rhi/rhi.h"
#include "giant/ui/canvas.h"

#include <deque>
#include <memory>
#include <string>

namespace giant::editor {

struct EditorState;

class GameUi {
public:
    // Its font's atlas uploaded to `device` (null: not drawn: tests).
    explicit GameUi(rhi::Device* device);
    ~GameUi();
    GameUi(const GameUi&) = delete;
    GameUi& operator=(const GameUi&) = delete;

    enum class Show : u8 { playing, always, never };
    static const char* to_string(Show show);
    Show show = Show::playing;

    // Shown for `seconds` (at most 30; the last 0.5 s fading), newest
    // lowest; at most k_max_toasts at once (the oldest goes).
    void toast(std::string text, double seconds = 3.0);
    static constexpr usize k_max_toasts = 4;
    static constexpr usize k_max_toast_bytes = 256;

    // Each frame: built for a `width` by `height` pixel target.
    void update(const EditorState& state, double seconds, float width, float height);
    bool shown() const { return m_shown; }
    rhi::UiDrawList list() const { return m_shown ? m_canvas.list() : rhi::UiDrawList{}; }

    const ui::Font* font() const { return m_font.get(); }
    const std::string& error() const { return m_error; } // the font's, if it failed
    float scale() const { return m_scale; }
    usize toasts() const { return m_toasts.size(); }
    usize vertices() const { return m_shown ? m_canvas.vertices() : 0; }
    usize commands() const { return m_shown ? m_canvas.commands() : 0; }
    double build_ms() const { return m_build_ms; }

private:
    struct Toast {
        std::string text;
        double left = 0.0; // seconds
    };
    rhi::Device* m_device = nullptr;
    rhi::TextureHandle m_atlas;
    std::unique_ptr<ui::Font> m_font;
    std::string m_error;
    ui::Canvas m_canvas;
    std::deque<Toast> m_toasts;
    bool m_shown = false;
    float m_scale = 1.0f;
    double m_build_ms = 0.0;
    u64 m_saves_seen = 0, m_autosaves_seen = 0; // as last seen
    bool m_saves_known = false;
};

} // namespace giant::editor
