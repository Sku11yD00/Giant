// The game's menus over play, built from screens (ui::Screen JSON made
// here) on the game UI: Esc while playing pauses and opens the pause menu
// (resume, save, load, settings, quit to editor); the saves menu lists the
// open world's slots (newest first: name, date, play time) to save over,
// save new or load; settings change the UI scale, the field of view and
// the mouse's look speed. While a menu is open over play, the game is
// paused (game_seconds is 0: the player, bodies, walkers, weather and the
// play clock stand still) and the mouse and keys are the menu's: the
// pointer points and clicks, Up/Down/Tab move the focus, Enter or Space
// presses, Esc goes back (or resumes). The same buttons are pressed over
// MCP with ui_click on the "menu.*" screens.
//
// The slot list is read from disk on a job when the saves menu opens
// ("Reading saves..." until it is in). Settings last while the editor
// runs. Menus take the window's input only over play (outside play they
// are MCP's), and only while the game UI is shown: with it hidden, Esc
// stops play as it did before the menus.
#pragma once

#include "giant/core/jobs.h"
#include "giant/core/types.h"
#include "giant/platform/input.h"

#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace giant::editor {

struct EditorState;

class GameMenus {
public:
    enum class Menu : u8 { none, pause, saves, settings };
    static const char* to_string(Menu menu);

    // Its screens loaded on `state.game_ui` (none: nothing shown).
    explicit GameMenus(EditorState& state);
    ~GameMenus();
    GameMenus(const GameMenus&) = delete;
    GameMenus& operator=(const GameMenus&) = delete;

    struct Settings {
        float ui_scale = 1.0f;          // 0.5 to 2
        float fov_degrees = 70.0f;      // vertical, while playing: 40 to 110
        float look_sensitivity = 1.0f;  // 0.25 to 4
    };
    const Settings& settings() const { return m_settings; }
    // Clamped to the ranges above; applied at once.
    void set_settings(const Settings& settings);

    // Opens `menu` (saves: `saving` true to save, false to load).
    void open(Menu menu, bool saving = false);
    void close() { open(Menu::none); }
    Menu menu() const { return m_menu; }
    bool saving() const { return m_saving; }
    // A menu over play: the game stands still.
    bool paused() const;
    // `seconds` as the game takes them (0 while paused).
    double game_seconds(double seconds) const { return paused() ? 0.0 : seconds; }

    // Each frame, before play steps: Esc taken from play; while a menu is
    // open, `events` (the window's, positions in points times
    // `pixels_per_point`) point, click and move the focus; the buttons'
    // presses (theirs and MCP's) done. True: a menu was open, so this
    // frame's input was its (play gets none of it).
    bool update(const std::vector<platform::InputEvent>& events, float pixels_per_point);

    // The saves menu's rows (as last read) and what it last said.
    struct Slot {
        std::string name, info;
        bool of_this_world = true;
        bool readable = true; // (else not loaded)
    };
    const std::vector<Slot>& slots() const { return m_slots; }
    const std::string& message() const { return m_message; }
    // The slot list still being read; wait() blocks until it is in (tests).
    bool reading() const { return m_reading; }
    void wait();
    static constexpr usize k_rows = 8;

    // The values the menus' screens show (menu.*, saves.*, slot.N.*,
    // settings.*) set again: done as they change, and after MCP sets values.
    void show_values();

private:
    void load_screens();
    void read_slots();
    void take_read();
    void press(const std::string& screen, const std::string& id);

    EditorState& m_state;
    Menu m_menu = Menu::none;
    bool m_saving = false;
    Settings m_settings;
    std::vector<Slot> m_slots;
    std::string m_message;
    bool m_was_playing = false;

    struct Read {
        u64 id = 0;
        std::vector<Slot> slots;
        std::vector<std::string> names; // every slot's, for "New save"
        std::string message;
    };
    std::vector<std::string> m_names;
    bool m_reading = false;
    u64 m_read_id = 0;
    std::mutex m_read_mutex;
    std::optional<Read> m_read; // done on the job, taken by update
    JobSystem m_jobs{1};
    WaitGroup m_group;
};

} // namespace giant::editor
