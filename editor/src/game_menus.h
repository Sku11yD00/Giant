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
// runs; a game keeps them between runs as text in the person's settings
// folder (`keep_in`: read by main before the window is made, written on a
// job when one changes and once more as the program ends, with the
// window's size). A game takes fullscreen at once and vsync when it next
// starts (the swap chain is made with it); the editor only shows the two,
// its own window and frames left as they are. Menus take the window's input
// only over play (outside play they are MCP's), and only while the game UI
// is shown: with it hidden, Esc stops play as it did before the menus.
//
// Run as a game (EditorState::game_mode), the menus lead: outside play the
// main menu stands (Continue: the newest save of this world that can be
// loaded; New game: the game as the world is authored, play starting at
// the view the program opened with, since a world names no player start
// yet; Load; Settings; Quit: the program ends) and takes the input, the
// pause menu's Quit goes back to it, and a game loaded from a menu is
// played once it is in (one that could not be loaded leads back to the
// main menu, which says why). Nothing leads to the editor. A game is not
// begun into a world still arriving: a loading screen stands first, over
// everything, until the cells and textures round the player are in (see
// Loading); over a game loaded in play it holds the game still as a menu
// does.
#pragma once

#include "giant/core/jobs.h"
#include "giant/core/types.h"
#include "giant/platform/input.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace giant::editor {

struct EditorCamera;
struct EditorState;

class GameMenus {
public:
    enum class Menu : u8 { none, pause, saves, settings, main, loading };
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
        bool fullscreen = false;        // the window borderless over the whole of its screen
        bool vsync = true;              // frames wait for the display (a game: taken up when it next starts)
        // The window's size in points while it is not fullscreen (0: as the program makes it; else 320 to
        // 7680 by 240 to 4320).
        u32 window_width = 0, window_height = 0;
        bool operator==(const Settings&) const = default;
    };
    const Settings& settings() const { return m_settings; }
    // `settings` within the ranges above (a value that is no number: its default).
    static Settings clamped(const Settings& settings);
    // Clamped; applied at once (but vsync: see above).
    void set_settings(const Settings& settings);
    // Where the settings are kept from now on (a text file; empty: nowhere), those set now taken as
    // what it holds: a change after this is written there on a job.
    void keep_in(std::string file);
    const std::string& kept_in() const { return m_file; }
    // The window's size while it is a plain window (the caller's to tell: not fullscreen, not
    // maximised): kept with the settings, written by `store`.
    void note_window(u32 width, u32 height);
    // What is not written yet is, before this returns (the program ends), and what a job could not
    // write is tried again. False: it could not be.
    bool store();

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
    // What the main menu says went wrong (a game that could not be loaded or begun); empty: nothing.
    const std::string& notice() const { return m_notice; }
    // A game being loaded or about to be played, while it and the world round the player come in
    // (the loading screen): what is still to come, and how far along it is. Play begins (a game
    // loaded in play: goes on) once nothing has been coming for k_quiet_frames frames, or after
    // k_most_loading_seconds of frames whatever is still to come (logged). What the GPU still has
    // to do of it (`voxels`, `light`, `shadows`, `geometry`) and the bodies are waited for only once the
    // rest is in, and each for no longer than its own most (some may never be done: a pool full).
    struct Loading {
        bool on = false;
        bool game = false;   // the saved game itself is still being read and put in
        u32 cells = 0;       // the world's cells streaming is still bringing (WorldStreaming::coming)
        u32 textures = 0;    // waiting to cook, cooking, waiting to upload, finer levels being read
        u32 ground = 0;      // terrain tiles whose mesh is still to build
        u32 scatter = 0;     // cells whose scatter (grass, trees, rocks) is waiting to be placed, or being placed
        u32 far_ground = 0;  // far ground chunks being read or built
        u32 poses = 0;       // poses being baked (those far-off people are drawn in)
        bool bodies = false; // what the player would stand on or in is still being made (entities' bodies, ground, trunks)
        // The last frame's, of the renderer: voxel bricks to build (the far world) and not lit yet
        // (`light`; neither while the voxel world is off), sun shadow pages never drawn or long behind,
        // cluster groups of geometry asked for and not loaded with the ground's tiles waiting for room.
        u32 voxels = 0;
        u32 light = 0;
        u32 shadows = 0;
        u32 geometry = 0;
        float progress = 0.0f; // 0 to 1, never back: the share of all that was to come that is in
        u32 shown = 0;       // how many times the screen has stood since the program began
    };
    const Loading& loading() const { return m_arriving; }
    static constexpr u32 k_quiet_frames = 10;
    static constexpr double k_most_loading_seconds = 120.0;
    // The voxel world's CPU budget a frame while the screen stands (RenderSettings::voxel_ms_per_frame, 1 in play).
    static constexpr float k_loading_voxel_ms = 12.0f;
    static constexpr double k_most_gpu_seconds = 5.0; // the GPU's own (voxels, light, shadows, geometry), once the rest is in: of the clock, not of frames
    // (The bodies alone are waited for no longer than play itself waits for them, PlayState::k_most_placing_seconds: some never come.)
    // Tests: the loading screen as if `seconds` more of frames had passed.
    void loading_waited(double seconds) { m_arriving_seconds += seconds; }
    // Tests: as if the GPU's own had been waited for `seconds` more.
    void loading_gpu_waited(double seconds) { m_arriving_gpu += seconds; }
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
    // Where Back and Esc lead from the saves and the settings.
    Menu before() const;
    // Starts loading `slot` (not `force`d); false with `m_message` saying why not.
    bool load(const std::string& slot);
    // A game: the main menu, saying `notice`.
    void lead(std::string notice);
    // Play begun from the menus (a new game, or the loaded one). A game: the loading screen stands
    // first (true). Else at once; false: the main menu, saying why not.
    bool begin_play();

    EditorState& m_state;
    Menu m_menu = Menu::none;
    bool m_saving = false;
    Settings m_settings;
    // The loading screen (see Loading): what was to come last frame, what has come in since it
    // stood, the frames nothing was coming, the seconds of frames it has stood (not while the game
    // itself is read: that has its own end) and the bodies alone were waited for.
    Loading m_arriving;
    u32 m_arriving_before = 0;
    u64 m_arriving_done = 0;
    u32 m_arriving_quiet = 0;
    u64 m_arriving_streamed = 0; // WorldStreaming::updates when last looked at
    double m_arriving_seconds = 0.0, m_arriving_bodies = 0.0, m_arriving_gpu = 0.0;
    std::chrono::steady_clock::time_point m_arriving_frame;
    void show_loading();  // the screen stands from now
    void arrive();        // each frame while it is on
    std::string gpu_left() const; // what the GPU still had to do, for the log
    bool start_play();    // play itself, from where the camera is
    Settings m_written; // as the file has them (or will, once the job under way is done)
    std::string m_file;
    // The writes of the settings: one at a time, each the newest text asked for (a job run by a
    // waiting thread beside the jobs' own would else write over a newer one, or into its half-made file).
    struct Keeper {
        std::mutex mutex;
        std::optional<std::string> text; // waiting to be written
        bool failed = false;             // the last write could not be done
    };
    std::shared_ptr<Keeper> m_keeper = std::make_shared<Keeper>();
    void write_later();
    std::vector<Slot> m_slots;
    std::string m_message;
    bool m_was_playing = false;
    // A load begun from the menus, until it is in or has failed (a game then plays it, or says why not).
    bool m_loading = false;
    u64 m_failures_before = 0; // GameSaves::Status::failures when it began
    std::string m_notice;
    std::string m_newest;   // the slot Continue loads (the newest of this world that can be read); empty: none
    std::unique_ptr<EditorCamera> m_start; // a game: the view it opened with (a new game starts there)

    struct Read {
        u64 id = 0;
        std::vector<Slot> slots;
        std::vector<std::string> names; // every slot's, for "New save"
        std::string newest;             // the newest of this world that can be read (of all, not only the rows)
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

// A game's settings as the text kept between runs (JSON, a key a line, numbers to three places), and read back: a key left
// out or of the wrong kind keeps its default and, wrong, is named in `problems`; every value is
// clamped. Text that is no JSON object gives the defaults and says so.
std::string settings_text(const GameMenus::Settings& settings);
GameMenus::Settings settings_from_text(std::string_view text, std::vector<std::string>& problems);

} // namespace giant::editor
