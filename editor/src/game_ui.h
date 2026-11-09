// The game's own UI in the editor (engine/ui): drawn over the scene, under
// the editor's panels, while playing (or always, or never: `show`). From
// the bottom: a crosshair, world markers (at their place or the screen's
// edge), screens from data (ui::Screen: named, drawn by layer, filled in
// from `values`, their compasses showing the markers, their buttons'
// clicks queued as events), short messages (toasts: "Saved", or what
// show_toast asks), a fade over everything (fade_to), and a mark while a
// save is being written (docs/architecture.md, "Game UI"). Each frame it
// sets the values `heading`, `playing` and, while playing, `player.speed`,
// `player.on_ground` and `player.climbing`. Built each frame
// after the frame's commands: a command's change shows from the next frame.
//
// Sized by the window: 1 UI unit is a pixel at 1080 lines high (scaled by
// height / 1080). The font's atlas is made and uploaded once, when this is
// made; a frame's UI is one canvas (a few draw calls). View state: nothing
// of it is saved or undone.
#pragma once

#include "giant/core/types.h"
#include "giant/rhi/rhi.h"
#include "giant/ui/canvas.h"
#include "giant/ui/hud.h"
#include "giant/ui/screen.h"

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
    // Times the window's scale (height / 1080): the player's UI scale setting (0.5 to 2).
    float ui_scale = 1.0f;

    // Shown for `seconds` (at most 30; the last 0.5 s fading), newest
    // lowest; at most k_max_toasts at once (the oldest goes).
    void toast(std::string text, double seconds = 3.0);
    static constexpr usize k_max_toasts = 4;
    static constexpr usize k_max_toast_bytes = 256;

    // Screens: loaded from JSON under `name` (1 to 64 bytes; the same name
    // replaced, its focus kept), drawn by `layer` (higher on top; the same
    // layer by name). False (and `error` set) if it is not a screen, or there
    // would be more than k_max_screens or k_max_widgets in all.
    bool load_screen(const std::string& name, std::string_view json, int layer, std::string& error);
    bool remove_screen(const std::string& name);
    void clear_screens(); // (all but the reserved ones)
    // Names starting "menu." are the editor's own menus (game_menus.h):
    // MCP may not load or remove them, clearing keeps them, and
    // take_events leaves their events for the menus.
    static bool reserved(std::string_view name) { return name.starts_with("menu."); }
    const ui::Screen* screen(std::string_view name) const;
    int layer(std::string_view name) const;
    std::vector<std::string> screen_names() const; // drawn first to last
    usize screen_widgets() const;
    static constexpr usize k_max_screens = 32;
    static constexpr usize k_max_widgets = 16384;
    static constexpr int k_max_layer = 1000;

    // What the screens show ("{ammo}", a bar's "health"), set by the game.
    ui::Values values;

    // Markers in the world (ui::Markers): drawn under the screens, on their compasses.
    ui::Markers markers;

    // The whole UI (and the scene under it) fading to `color` at `alpha`
    // (0 clear, 1 solid) over `seconds` (0: at once; at most 30).
    void fade_to(u32 color, float alpha, double seconds);
    float fade_alpha() const { return m_fade_alpha; }
    float fade_target() const { return m_fade_target; }
    u32 fade_color() const { return m_fade_color; }

    // Images screens may show by name (`image`): a texture the game owns
    // (rgba8_unorm, sampled). 0 forgets it; forget it (or update) before
    // destroying it, as the last frame's list may still draw it.
    void set_image(const std::string& name, rhi::TextureId texture);
    usize images() const { return m_images.size(); }

    // A click at `point` (pixels, as last laid out): the top button shown
    // there, on the top screen with one (a screen showing a background
    // there takes it from those under it), queued as an event (and
    // returned). Whoever calls these asks `showing` first.
    std::optional<ui::UiEvent> click(ui::Vec2 point);
    // Pointed at: that button is hovered on its screen.
    void point(ui::Vec2 point);
    // Keys on one screen: the focus moved (+1 next, -1 previous), or the
    // focused button activated (queued, returned). False / none if no such screen.
    bool move_focus(std::string_view screen, int step);
    std::optional<ui::UiEvent> activate(std::string_view screen);
    // The focus put on button `id` of that screen ("": none).
    void focus(std::string_view screen, std::string id);
    // A button pressed by id (as if clicked), if shown on that screen.
    std::optional<ui::UiEvent> press(std::string_view screen, std::string_view id);
    // The events since last taken, oldest first (at most k_max_events: the oldest go),
    // but the reserved screens'.
    std::vector<ui::UiEvent> take_events();
    // Only those of screens named starting with `prefix` (the rest left queued).
    std::vector<ui::UiEvent> take_events_of(std::string_view prefix);
    usize events() const { return m_events.size(); }
    static constexpr usize k_max_events = 64;

    // Each frame: built for a `width` by `height` pixel target.
    void update(const EditorState& state, double seconds, float width, float height);
    bool shown() const { return m_shown; }
    // Whether it would be shown now (`show`, and playing), before the next frame says so.
    bool showing(const EditorState& state) const;
    rhi::UiDrawList list() const { return m_shown ? m_canvas.list() : rhi::UiDrawList{}; }

    const ui::Font* font() const { return m_font.get(); }
    const std::string& error() const { return m_error; } // the font's, if it failed
    float scale() const { return m_scale; }
    usize toasts() const { return m_toasts.size(); }
    usize vertices() const { return m_shown ? m_canvas.vertices() : 0; }
    usize commands() const { return m_shown ? m_canvas.commands() : 0; }
    double build_ms() const { return m_build_ms; }

private:
    struct Loaded {
        std::unique_ptr<ui::Screen> screen;
        int layer = 0;
    };
    void reorder(); // m_order from m_screens
    ui::Screen* find_screen(std::string_view name);
    void queue(const ui::UiEvent& e);

    std::map<std::string, Loaded, std::less<>> m_screens;
    std::vector<std::pair<std::string, Loaded*>> m_order; // drawn first to last
    std::map<std::string, rhi::TextureId, std::less<>> m_images;
    std::deque<ui::UiEvent> m_events;
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
    float m_width = 0.0f, m_height = 0.0f; // the last frame's (0: none yet)
    u32 m_fade_color = 0;
    float m_fade_alpha = 0.0f, m_fade_target = 0.0f, m_fade_rate = 0.0f; // alpha a second
    double m_build_ms = 0.0;
    u64 m_saves_seen = 0, m_autosaves_seen = 0; // as last seen
    bool m_saves_known = false;
    u64 m_failures_seen = 0;
};

} // namespace giant::editor
