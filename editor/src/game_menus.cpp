#include "game_menus.h"

#include "editor_commands.h"
#include "far_ground.h"
#include "game_saves.h"
#include "game_ui.h"
#include "play_state.h"
#include "pose_bakes.h"
#include "scatter_placer.h"
#include "terrain_meshes.h"
#include "texture_library.h"
#include "world_streaming.h"

#include "giant/ai/json.h"
#include "giant/core/log.h"
#include "giant/core/text.h"
#include "giant/core/text_files.h"
#include "giant/platform/window.h"
#include "giant/save/slot.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <system_error>

namespace giant::editor {

namespace {

using Json = ai::Json;

constexpr int k_layer = 900; // over the game's own screens
constexpr const char* k_back = "#0b0e12b8";
constexpr const char* k_button = "#1d232be6";
constexpr const char* k_lit = "#3d4b5cff";

Json button(const std::string& id, const std::string& text, float width = 380.0f) {
    return {{"type", "button"}, {"id", id},         {"text", text},     {"size", {width, 54}}, {"align", "center"},
            {"font_size", 26},  {"background", k_button}, {"hover", k_lit}};
}

Json title(const std::string& text) {
    return {{"type", "text"}, {"text", text}, {"font_size", 44}, {"anchor", {0.5, 0}}, {"shadow", 2}};
}

Json menu_screen(const std::string& shown_by, Json column) {
    return {{"type", "panel"},
            {"visible", shown_by},
            {"background", k_back},
            {"children",
             {{{"type", "panel"}, {"anchor", {0.5, 0.5}}, {"layout", "column"}, {"gap", 14}, {"children", std::move(column)}}}}};
}

Json pause_screen() {
    return menu_screen("menu.pause", {title("Paused"),
                                      {{"type", "panel"}, {"size", {0, 12}}},
                                      button("resume", "Resume"),
                                      button("save", "Save game"),
                                      button("load", "Load game"),
                                      button("settings", "Settings"),
                                      button("quit", "{menu.quit}")});
}

// What a menu says when something asked of it could not be done.
Json message(const std::string& value) {
    return {{"type", "text"},  {"text", "{" + value + "}"}, {"visible", value},      {"anchor", {0.5, 0}}, {"color", "#ffd27a"},
            {"font_size", 22}, {"wrap", true},              {"size", {720, 0}},      {"align", "center"}};
}

Json main_screen() {
    Json again = button("continue", "Continue");
    again["visible"] = "menu.continue";
    return menu_screen("menu.main", {title("{menu.title}"),
                                     message("menu.message"),
                                     {{"type", "panel"}, {"size", {0, 12}}},
                                     std::move(again),
                                     button("new", "New game"),
                                     button("load", "Load game"),
                                     button("settings", "Settings"),
                                     button("quit", "Quit")});
}

// Over everything while the world round the player comes in: nothing of the half-made world shows.
Json loading_screen() {
    return {{"type", "panel"},
            {"visible", "menu.loading"},
            {"background", "#07090cff"},
            {"children",
             {{{"type", "panel"},
               {"anchor", {0.5, 0.5}},
               {"layout", "column"},
               {"gap", 18},
               {"children",
                {title("{menu.title}"),
                 {{"type", "text"}, {"text", "Loading..."}, {"font_size", 26}, {"anchor", {0.5, 0}}},
                 {{"type", "bar"},
                  {"id", "progress"},
                  {"value", "loading.progress"},
                  {"max", 1},
                  {"size", {480, 10}},
                  {"anchor", {0.5, 0}},
                  {"background", k_button},
                  {"color", "#c9d4e0ff"}}}}}}}};
}

const char* screen_of(GameMenus::Menu menu) {
    switch (menu) {
    case GameMenus::Menu::pause: return "menu.pause";
    case GameMenus::Menu::saves: return "menu.saves";
    case GameMenus::Menu::settings: return "menu.settings";
    case GameMenus::Menu::main: return "menu.main";
    case GameMenus::Menu::loading: return "menu.loading";
    case GameMenus::Menu::none: break;
    }
    return "";
}

Json saves_screen() {
    Json column = {title("{saves.title}"),
                   message("saves.message"),
                   {{"type", "text"},
                    {"text", "No saves yet"},
                    {"visible", "saves.empty"},
                    {"anchor", {0.5, 0}},
                    {"color", "#a0a8b0"},
                    {"font_size", 22}}};
    for (usize i = 0; i < GameMenus::k_rows; ++i) {
        const std::string n = "slot." + std::to_string(i);
        Json row = button(n, "{" + n + ".name}", 720.0f);
        row["align"] = "left";
        row["padding"] = 18;
        row["visible"] = n + ".shown";
        row["children"] = {{{"type", "text"},
                            {"text", "{" + n + ".info}"},
                            {"anchor", {1, 0.5}},
                            {"offset", {-18, 0}},
                            {"font_size", 20},
                            {"color", "#b8c0c8"}}};
        column.push_back(std::move(row));
    }
    Json fresh = button("new", "New save", 720.0f);
    fresh["visible"] = "saves.new";
    column.push_back(std::move(fresh));
    column.push_back({{"type", "panel"}, {"size", {0, 8}}});
    column.push_back(button("back", "Back"));
    return menu_screen("menu.saves", std::move(column));
}

Json setting_row(const std::string& id, const std::string& label, const std::string& value) {
    return {{"type", "panel"},
            {"layout", "row"},
            {"gap", 12},
            {"anchor", {0.5, 0}},
            {"children",
             {{{"type", "text"}, {"text", label}, {"size", {260, 54}}, {"font_size", 26}},
              button(id + ".down", "-", 64.0f),
              {{"type", "text"}, {"text", value}, {"size", {140, 54}}, {"font_size", 26}, {"align", "center"}},
              button(id + ".up", "+", 64.0f)}}};
}

// A setting that is on or off: its name, which it is, and a button that changes it.
Json toggle_row(const std::string& id, const std::string& label, const std::string& value) {
    return {{"type", "panel"},
            {"layout", "row"},
            {"gap", 12},
            {"anchor", {0.5, 0}},
            {"children",
             {{{"type", "text"}, {"text", label}, {"size", {260, 54}}, {"font_size", 26}},
              {{"type", "text"}, {"text", value}, {"size", {128, 54}}, {"font_size", 26}, {"align", "center"}},
              button(id + ".toggle", "Change", 152.0f)}}};
}

Json settings_screen() {
    return menu_screen("menu.settings", {title("Settings"),
                                         {{"type", "panel"}, {"size", {0, 12}}},
                                         setting_row("ui_scale", "UI scale", "{settings.ui_scale:2}"),
                                         setting_row("fov", "Field of view", "{settings.fov:0}"),
                                         setting_row("look", "Mouse speed", "{settings.look:2}"),
                                         toggle_row("fullscreen", "Fullscreen", "{settings.fullscreen}"),
                                         toggle_row("vsync", "Vsync (next start)", "{settings.vsync}"),
                                         {{"type", "panel"}, {"size", {0, 8}}},
                                         button("back", "Back")});
}

std::string when(i64 unix_seconds) {
    if (unix_seconds <= 0) return "?";
    const std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm tm{};
#ifdef _WIN32
    if (localtime_s(&tm, &t) != 0) return "?";
#else
    if (localtime_r(&t, &tm) == nullptr) return "?";
#endif
    char buffer[32];
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &tm) == 0) return "?";
    return buffer;
}

std::string played(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) seconds = 0.0;
    const i64 minutes = static_cast<i64>(seconds / 60.0);
    char buffer[32];
    if (minutes < 60) {
        std::snprintf(buffer, sizeof(buffer), "%lld min", static_cast<long long>(minutes));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%lld h %02lld min", static_cast<long long>(minutes / 60),
                      static_cast<long long>(minutes % 60));
    }
    return buffer;
}

// N of "save-N" (1 to 999999999), else 0.
i64 save_number(const std::string& name) {
    if (!name.starts_with("save-") || name.size() < 6 || name.size() > 14) return 0;
    i64 n = 0;
    for (usize i = 5; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return 0;
        n = n * 10 + (name[i] - '0');
    }
    return n <= 999999999 ? n : 0;
}

} // namespace

const char* GameMenus::to_string(Menu menu) {
    switch (menu) {
    case Menu::none: return "none";
    case Menu::pause: return "pause";
    case Menu::saves: return "saves";
    case Menu::settings: return "settings";
    case Menu::main: return "main";
    case Menu::loading: return "loading";
    }
    return "none";
}

GameMenus::GameMenus(EditorState& state) : m_state(state) {
    load_screens();
    set_settings(m_settings);
}

GameMenus::~GameMenus() { m_jobs.wait(m_group); }

void GameMenus::wait() {
    m_jobs.wait(m_group);
    take_read();
}

void GameMenus::load_screens() {
    if (m_state.game_ui == nullptr) return;
    for (const auto& [name, screen] : {std::pair{"menu.pause", pause_screen()}, std::pair{"menu.saves", saves_screen()},
                                       std::pair{"menu.settings", settings_screen()}, std::pair{"menu.main", main_screen()},
                                       std::pair{"menu.loading", loading_screen()}}) {
        std::string error;
        // (The loading screen over every screen a game may load: nothing of the half-made world shows.)
        const int layer = std::string_view(name) == "menu.loading" ? GameUi::k_top_layer : k_layer;
        if (!m_state.game_ui->load_screen(name, screen.dump(), layer, error))
            log_error("editor", std::string("the game's menu ") + name + ": " + error);
    }
    show_values();
}

GameMenus::Settings GameMenus::clamped(const Settings& s) {
    const auto pick = [](float v, float lo, float hi, float otherwise) { return std::isfinite(v) ? std::clamp(v, lo, hi) : otherwise; };
    Settings out = s;
    out.ui_scale = pick(s.ui_scale, 0.5f, 2.0f, 1.0f);
    out.fov_degrees = pick(s.fov_degrees, 40.0f, 110.0f, 70.0f);
    out.look_sensitivity = pick(s.look_sensitivity, 0.25f, 4.0f, 1.0f);
    if (s.window_width == 0 || s.window_height == 0) { // (none kept: as the program makes it)
        out.window_width = 0;
        out.window_height = 0;
    } else {
        out.window_width = std::clamp<u32>(s.window_width, 320, 7680);
        out.window_height = std::clamp<u32>(s.window_height, 240, 4320);
    }
    return out;
}

void GameMenus::set_settings(const Settings& s) {
    const bool was_fullscreen = m_settings.fullscreen;
    m_settings = clamped(s);
    // (A game's window: the editor's own is left as it is, the setting only shown there.)
    if (m_settings.fullscreen != was_fullscreen && m_state.game_mode && m_state.window != nullptr &&
        !m_state.window->request_fullscreen(m_settings.fullscreen))
        m_settings.fullscreen = was_fullscreen; // (the OS would not: the setting says what the window is)
    if (m_state.game_ui != nullptr) m_state.game_ui->ui_scale = m_settings.ui_scale;
    if (m_state.play != nullptr) m_state.play->look_sensitivity = m_settings.look_sensitivity;
    if (m_state.play != nullptr && m_state.play->playing()) m_state.camera.vertical_fov_degrees = m_settings.fov_degrees;
    show_values();
    if (!(m_settings == m_written)) write_later();
}

void GameMenus::keep_in(std::string file) {
    m_jobs.wait(m_group); // (nothing still on its way to the last file)
    m_file = std::move(file);
    m_written = m_settings;
    const std::lock_guard lock(m_keeper->mutex);
    m_keeper->text.reset();
    m_keeper->failed = false;
}

void GameMenus::note_window(u32 width, u32 height) {
    if (m_settings.fullscreen || width == 0 || height == 0) return;
    m_settings.window_width = std::clamp<u32>(width, 320, 7680);
    m_settings.window_height = std::clamp<u32>(height, 240, 4320);
}

namespace {

bool write_settings(const std::string& file, const std::string& text) {
    std::error_code ec;
    const std::filesystem::path path = path_from_utf8(file);
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec); // (the first run: no folder yet)
    if (write_text_file(path, text)) return true;
    log_warning("game", "the settings could not be written to " + file);
    return false;
}

} // namespace

void GameMenus::write_later() {
    if (m_file.empty()) return;
    // (A window dragged to another size is no change of the person's to write for: `store` writes it.)
    Settings but_window = m_settings;
    but_window.window_width = m_written.window_width;
    but_window.window_height = m_written.window_height;
    if (but_window == m_written) return;
    m_written = m_settings;
    // On a job (no wait on the disk on the frame). Each job writes the newest text asked for, under
    // the keeper's lock: a thread waiting on the group runs jobs too, beside the jobs' own thread.
    {
        const std::lock_guard lock(m_keeper->mutex);
        m_keeper->text = settings_text(m_settings);
    }
    m_jobs.submit(m_group, [file = m_file, keeper = m_keeper] {
        const std::lock_guard lock(keeper->mutex);
        if (!keeper->text) return; // (a job before this one wrote it)
        const std::string text = std::move(*keeper->text);
        keeper->text.reset();
        keeper->failed = !write_settings(file, text);
    });
}

bool GameMenus::store() {
    m_jobs.wait(m_group);
    take_read();
    if (m_file.empty()) return true;
    const std::lock_guard lock(m_keeper->mutex); // (no job is left: the lock is for the rule, not a wait)
    if (m_settings == m_written && !m_keeper->failed) return true;
    m_written = m_settings;
    m_keeper->failed = !write_settings(m_file, settings_text(m_settings));
    return !m_keeper->failed;
}

std::string settings_text(const GameMenus::Settings& s) {
    // By hand: a key a line in this order, numbers as a person would write them (1.1, not the float's 1.100000023841858).
    const auto number = [](float v) { return Json(std::round(static_cast<double>(v) * 1000.0) / 1000.0).dump(); };
    const auto flag = [](bool v) { return std::string(v ? "true" : "false"); };
    return "{\n  \"ui_scale\": " + number(s.ui_scale) + ",\n  \"fov\": " + number(s.fov_degrees) + ",\n  \"look_sensitivity\": " +
           number(s.look_sensitivity) + ",\n  \"fullscreen\": " + flag(s.fullscreen) + ",\n  \"vsync\": " + flag(s.vsync) +
           ",\n  \"window\": [" + std::to_string(s.window_width) + ", " + std::to_string(s.window_height) + "]\n}\n";
}

GameMenus::Settings settings_from_text(std::string_view text, std::vector<std::string>& problems) {
    GameMenus::Settings s;
    const Json json = Json::parse(text, nullptr, false);
    if (!json.is_object()) {
        problems.emplace_back("not a JSON object");
        return s;
    }
    const auto number = [&](const char* key, float& out) {
        const auto found = json.find(key);
        if (found == json.end()) return;
        if (found->is_number()) {
            // (Beyond what a float holds: its nearest end, not a number that is none.)
            out = static_cast<float>(std::clamp(found->get<double>(), -1.0e9, 1.0e9));
        } else {
            problems.emplace_back(std::string(key) + " is not a number");
        }
    };
    const auto boolean = [&](const char* key, bool& out) {
        const auto found = json.find(key);
        if (found == json.end()) return;
        if (found->is_boolean()) {
            out = found->get<bool>();
        } else {
            problems.emplace_back(std::string(key) + " is not true or false");
        }
    };
    number("ui_scale", s.ui_scale);
    number("fov", s.fov_degrees);
    number("look_sensitivity", s.look_sensitivity);
    boolean("fullscreen", s.fullscreen);
    boolean("vsync", s.vsync);
    if (const auto window = json.find("window"); window != json.end()) {
        const auto whole = [](const Json& v) { return v.is_number_unsigned() && v.get<u64>() <= 100000; };
        if (window->is_array() && window->size() == 2 && whole((*window)[0]) && whole((*window)[1])) {
            s.window_width = (*window)[0].get<u32>();
            s.window_height = (*window)[1].get<u32>();
        } else {
            problems.emplace_back("window is not [width, height]");
        }
    }
    return GameMenus::clamped(s);
}

bool GameMenus::paused() const { return m_menu != Menu::none && m_state.play != nullptr && m_state.play->playing(); }

void GameMenus::open(Menu menu, bool saving) {
    if (menu == Menu::main && !m_state.game_mode) menu = Menu::none; // (the editor has none: its Quit would end the program)
    if (menu == Menu::loading && !m_arriving.on) menu = Menu::none;   // (the menus' own to show: nothing else would end it)
    if (menu != Menu::none && m_menu == Menu::none && m_state.play != nullptr) m_state.play->release_keys();
    m_menu = menu;
    m_saving = saving;
    m_message.clear();
    m_notice.clear();
    m_reading = false;
    ++m_read_id; // (a read still under way for an earlier opening is dropped)
    // No focus kept from the last time (Enter must not press what was left focused).
    if (m_state.game_ui != nullptr && menu != Menu::none) m_state.game_ui->focus(screen_of(menu), {});
    if (menu == Menu::saves || menu == Menu::main) read_slots(); // (the main menu's Continue is the newest of them)
    show_values(); // (shown from the next layout: Down or Tab then focuses its first button)
}

void GameMenus::read_slots() {
    m_slots.clear();
    m_names.clear();
    m_newest.clear();
    if (m_state.saves == nullptr || m_state.world == nullptr) {
        m_message = "No world open: games are saved beside an open world.";
        return;
    }
    const std::filesystem::path root = GameSaves::root(m_state);
    if (root.empty()) {
        m_message = "No world open: games are saved beside an open world.";
        return;
    }
    const u64 hash = GameSaves::world_identity(*m_state.world);
    const std::string world_name = m_state.world->folder().filename().string(); // (as a save names its world)
    m_reading = true;
    m_message = "Reading saves...";
    // On a job: every slot's manifest opened (no wait on the disk on the frame).
    m_jobs.submit(m_group, [this, root, hash, world_name, saving = m_saving, id = m_read_id] {
        struct Row {
            Slot slot;
            i64 saved = 0;
        };
        Read read;
        read.id = id;
        std::vector<Row> rows;
        for (const save::Listed& l : save::list_slots(root)) {
            read.names.push_back(l.name);
            if (saving && l.name == GameSaves::k_autosave_slot) continue; // (the autosave's own)
            Row r;
            r.slot.name = l.name;
            if (!l.opened.error.empty()) {
                r.slot.info = "cannot be read";
                r.slot.readable = false;
            } else {
                const save::Manifest& m = l.opened.manifest;
                r.saved = m.saved_unix;
                r.slot.of_this_world = m.world_hash == hash && m.world_name == world_name; // (what a load asks of it)
                r.slot.info = when(m.saved_unix) + "   " + played(m.play_seconds) + (r.slot.of_this_world ? "" : "   (older world)");
            }
            rows.push_back(std::move(r));
        }
        std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.saved > b.saved; });
        for (Row& r : rows) {
            if (read.newest.empty() && r.slot.readable && r.slot.of_this_world) read.newest = r.slot.name;
            if (read.slots.size() < k_rows) read.slots.push_back(std::move(r.slot));
        }
        const std::lock_guard lock(m_read_mutex);
        if (!m_read || m_read->id <= read.id) m_read = std::move(read); // (the newest opening's kept)
    });
}

void GameMenus::take_read() {
    std::optional<Read> read;
    {
        const std::lock_guard lock(m_read_mutex);
        read.swap(m_read);
    }
    if (!read || !m_reading || read->id != m_read_id) return;
    m_reading = false;
    m_slots = std::move(read->slots);
    m_names = std::move(read->names);
    m_newest = std::move(read->newest);
    m_message = std::move(read->message);
    show_values();
}

void GameMenus::show_values() {
    if (m_state.game_ui == nullptr) return;
    ui::Values& v = m_state.game_ui->values;
    v.set("menu.pause", m_menu == Menu::pause);
    v.set("menu.saves", m_menu == Menu::saves);
    v.set("menu.settings", m_menu == Menu::settings);
    v.set("menu.main", m_menu == Menu::main);
    v.set("menu.loading", m_menu == Menu::loading);
    v.set("loading.progress", static_cast<double>(m_arriving.progress));
    // The main menu: the world's name over it, Continue when a game of this world can be loaded, and
    // what went wrong (not the saves' own "Reading saves...").
    const std::string world = m_state.world != nullptr ? path_to_utf8(m_state.world->folder().filename()) : std::string();
    v.set("menu.title", world.empty() ? std::string("Giant") : world);
    v.set("menu.continue", !m_reading && !m_newest.empty());
    v.set("menu.message", m_notice);
    v.set("menu.quit", std::string(m_state.game_mode ? "Quit to menu" : "Quit to editor"));
    v.set("menu.open", m_menu != Menu::none);
    v.set("saves.title", std::string(m_saving ? "Save game" : "Load game"));
    v.set("saves.message", m_message);
    v.set("saves.empty", m_menu == Menu::saves && m_slots.empty() && m_message.empty());
    v.set("saves.new", m_saving && m_message.empty() && m_state.world != nullptr);
    for (usize i = 0; i < k_rows; ++i) {
        const std::string n = "slot." + std::to_string(i);
        const bool shown = i < m_slots.size();
        v.set(n + ".shown", shown);
        v.set(n + ".name", shown ? m_slots[i].name : std::string());
        v.set(n + ".info", shown ? m_slots[i].info : std::string());
    }
    v.set("settings.ui_scale", static_cast<double>(m_settings.ui_scale));
    v.set("settings.fov", static_cast<double>(m_settings.fov_degrees));
    v.set("settings.look", static_cast<double>(m_settings.look_sensitivity));
    v.set("settings.fullscreen", std::string(m_settings.fullscreen ? "On" : "Off"));
    v.set("settings.vsync", std::string(m_settings.vsync ? "On" : "Off"));
}

GameMenus::Menu GameMenus::before() const {
    if (m_state.play != nullptr && m_state.play->playing()) return Menu::pause;
    return m_state.game_mode ? Menu::main : Menu::none;
}

bool GameMenus::load(const std::string& slot) {
    const std::string error = m_state.saves->begin_load(m_state, slot, false);
    if (!error.empty()) {
        m_message = error;
        show_values();
        return false;
    }
    m_loading = true;
    m_failures_before = m_state.saves->status().failures;
    // The game it was is gone from here. A game: the loading screen stands from now, over the game
    // being read and then over its world coming in (in play too: that game is held still).
    if (m_state.game_mode && m_state.game_ui != nullptr) show_loading();
    else close();
    return true;
}

void GameMenus::lead(std::string notice) {
    open(Menu::main);
    m_notice = std::move(notice);
    show_values();
}

void GameMenus::show_loading() {
    const u32 shown = m_arriving.shown + 1;
    m_arriving = Loading{};
    m_arriving.on = true;
    m_arriving.shown = shown;
    m_arriving_before = 0;
    m_arriving_done = 0;
    m_arriving_quiet = 0;
    m_arriving_seconds = m_arriving_bodies = m_arriving_gpu = 0.0;
    m_arriving_frame = std::chrono::steady_clock::now();
    m_arriving_streamed = 0;
    open(Menu::loading);
}

bool GameMenus::begin_play() {
    m_state.follow.person = 0; // the camera is the player's eyes from here
    // A game: first the world round the player comes in, behind the loading screen (play then
    // begins without the hitches of its arriving, and with ground to stand on).
    if (m_state.game_mode && m_state.game_ui != nullptr) {
        if (!m_arriving.on) show_loading();
        return true;
    }
    return start_play();
}

bool GameMenus::start_play() {
    if (m_state.play != nullptr && m_state.play->start(m_state.scene, m_state.camera, m_state.scene_generation)) {
        close();
        return true;
    }
    lead("The game could not start here: there is no ground to stand on.");
    return false;
}

void GameMenus::arrive() {
    // The frame's seconds, as the frames have them: a window put away (no frames) is no time passed.
    const auto now = std::chrono::steady_clock::now();
    const double real = std::max(std::chrono::duration<double>(now - m_arriving_frame).count(), 0.0);
    const double frame = std::min(real, 0.1);
    m_arriving_frame = now;
    const WorldStreaming* streaming = m_state.world;
    const bool streamed = streaming != nullptr && streaming->active(m_state.scene_generation);
    const bool playing = m_state.play != nullptr && m_state.play->playing();
    m_arriving.game = m_state.saves != nullptr && m_state.saves->busy(); // (the game being read; a save being written too)
    // (A frame streaming has not looked at the world in, as when nothing could be drawn, says nothing of what is to come.)
    const bool looked = !streamed || streaming->updates() != m_arriving_streamed;
    if (streamed) m_arriving_streamed = streaming->updates();
    m_arriving.cells = streamed ? streaming->coming() : 0;
    m_arriving.textures = m_state.textures != nullptr ? static_cast<u32>(std::min<usize>(m_state.textures->pending(), 1u << 30)) : 0;
    // (A loaded game's dented tiles are put in as their cells come, and built then: not counted while they
    // wait, since those of cells far from the player never come.)
    // (Tiles built and waiting for room in the renderer's geometry pool are the GPU's own, below: a pool
    // too small may never take them.)
    u32 ground_waiting = 0;
    m_arriving.ground = 0;
    if (m_state.terrain_meshes != nullptr) {
        const TerrainMeshStats ground = m_state.terrain_meshes->stats();
        m_arriving.ground = ground.building - ground.waiting;
        ground_waiting = ground.waiting;
    }
    m_arriving.scatter = m_state.scatter != nullptr ? m_state.scatter->stats().building : 0u;
    m_arriving.far_ground = 0;
    if (m_state.far_ground != nullptr) {
        const FarGroundStats far = m_state.far_ground->stats();
        m_arriving.far_ground = far.reading + far.wanted;
        for (u32 l = 0; l < far.building.size(); ++l) {
            m_arriving.far_ground += far.building[l] - far.waiting[l];
            ground_waiting += far.waiting[l];
        }
    }
    m_arriving.poses = m_state.poses != nullptr ? static_cast<u32>(std::min<usize>(m_state.poses->baking(), 1u << 30)) : 0u;
    const u32 counted = m_arriving.cells + m_arriving.textures + m_arriving.ground + m_arriving.scatter + m_arriving.far_ground +
                        m_arriving.poses + (m_arriving.game ? 1u : 0u);
    // The bodies, once the rest is in, and for no longer than play itself would wait for them.
    // (A player already put down by a loaded game waits for its own, held still.)
    m_arriving.bodies = !playing && m_state.play != nullptr && m_state.play->world_waits(m_state.scene, m_state.camera.position);
    bool bodies = false;
    if (counted == 0 && m_arriving.bodies) {
        m_arriving_bodies += frame;
        bodies = m_arriving_bodies <= PlayState::k_most_placing_seconds;
    }
    // The GPU's own, the same: once the rest is in, for no longer than k_most_gpu_seconds, of the clock:
    // on a slow device (a software one: seconds a frame) what is behind is not worth waiting for
    // frame by frame, and a window put away does nothing for it either.
    const render::RenderStats& drawn = m_state.scene_stats;
    // (The voxel world's count is kept as it was when it is turned off: not counted then. Shadow pages:
    // those never drawn or long behind, not the backlog of what moves on them, which may never end.)
    const bool voxels = m_state.render_settings.voxel_world;
    m_arriving.voxels = voxels ? drawn.voxel_pending : 0u;
    m_arriving.light = voxels ? drawn.gi_waiting : 0u;
    m_arriving.shadows = drawn.shadow_behind;
    m_arriving.geometry = drawn.waiting + ground_waiting;
    const u32 gpu = m_arriving.voxels + m_arriving.light + m_arriving.shadows + m_arriving.geometry;
    bool gpu_waited = false;
    if (counted == 0 && gpu != 0) {
        m_arriving_gpu += real;
        gpu_waited = m_arriving_gpu <= k_most_gpu_seconds;
    }
    const u32 coming = counted + (bodies ? 1u : 0u) + (gpu_waited ? 1u : 0u);
    // What has come in since the screen stood, of all that was ever to come: more found to come
    // later (a cell in brings its textures) holds the bar where it is, never back.
    if (coming < m_arriving_before) m_arriving_done += m_arriving_before - coming;
    m_arriving_before = coming;
    if (coming != 0) m_arriving_quiet = 0;
    else if (looked) ++m_arriving_quiet;
    const double all = static_cast<double>(m_arriving_done) + static_cast<double>(coming);
    const float in = all > 0.0 ? static_cast<float>(static_cast<double>(m_arriving_done) / all) : 1.0f;
    // (The bar's last tenth is the quiet frames, but the first two: what a cell just in asks for is asked a
    // frame later, and nothing is known to come in the first frames the screen stands.)
    const u32 held = 2;
    const float quiet = static_cast<float>(std::clamp(m_arriving_quiet, held, k_quiet_frames) - held) / static_cast<float>(k_quiet_frames - held);
    // (Rounded: a value the screen and get_menu show.)
    const float progress = std::max(m_arriving.progress, std::round((coming != 0 ? 0.9f * in : quiet > 0.0f || m_arriving_done != 0 ? 0.9f + 0.1f * quiet : 0.0f) * 1000.0f) / 1000.0f);
    if (progress != m_arriving.progress) {
        m_arriving.progress = progress;
        if (m_state.game_ui != nullptr) m_state.game_ui->values.set("loading.progress", static_cast<double>(progress));
    }
    if (m_arriving.game) return; // (the game itself still being read: its own end, counted as no time of the world's)
    m_arriving_seconds += frame;
    const bool in_time = m_arriving_quiet >= k_quiet_frames;
    if (!in_time && m_arriving_seconds <= k_most_loading_seconds) return;
    if (!in_time)
        log_warning("game", "the world was still coming in after " + std::to_string(static_cast<int>(k_most_loading_seconds)) + " s (" +
                                std::to_string(m_arriving.cells) + " cells, " + std::to_string(m_arriving.textures) + " textures, " +
                                std::to_string(m_arriving.ground) + " ground tiles, " + std::to_string(m_arriving.scatter) +
                                " scatter cells, " + std::to_string(m_arriving.far_ground) + " far ground chunks, " +
                                std::to_string(m_arriving.poses) + " poses" + (m_arriving.bodies ? ", bodies" : "") + "; " + gpu_left() +
                                "): play begins without them");
    else if (m_arriving.voxels + m_arriving.light + m_arriving.shadows + m_arriving.geometry != 0)
        log_info("game", "play begins with the GPU still at it after " + std::to_string(static_cast<int>(k_most_gpu_seconds)) + " s (" +
                             gpu_left() + ")");
    m_arriving.on = false;
    // (A loaded game that was saved in play is playing already, held still behind the screen: it goes on.)
    if (playing) close();
    else start_play();
}

std::string GameMenus::gpu_left() const {
    return std::to_string(m_arriving.voxels) + " voxel bricks, " + std::to_string(m_arriving.light) + " unlit, " +
           std::to_string(m_arriving.shadows) + " shadow pages, " + std::to_string(m_arriving.geometry) + " geometry";
}

void GameMenus::press(const std::string& screen, const std::string& id) {
    PlayState* play = m_state.play;
    const bool playing = play != nullptr && play->playing();
    if (screen == "menu.main") {
        if (id == "continue") {
            if (m_state.saves == nullptr || m_reading || m_newest.empty()) return;
            if (!load(std::string(m_newest))) lead(m_message);
        } else if (id == "new") {
            if (m_state.saves != nullptr) {
                if (m_state.saves->busy()) return; // (a game still being saved or loaded: once it is done)
                m_state.saves->close(m_state); // the world as it is authored: nothing of a game played before
            }
            if (m_start != nullptr) m_state.camera = *m_start;
            begin_play();
        } else if (id == "load") open(Menu::saves, false);
        else if (id == "settings") open(Menu::settings);
        else if (id == "quit") m_state.quit_requested = true;
    } else if (screen == "menu.pause") {
        if (id == "resume") close();
        else if (id == "save") open(Menu::saves, true);
        else if (id == "load") open(Menu::saves, false);
        else if (id == "settings") open(Menu::settings);
        else if (id == "quit") {
            close();
            if (playing) play->stop(m_state.camera);
        }
    } else if (screen == "menu.saves") {
        if (id == "back") {
            open(before());
            return;
        }
        if (m_state.saves == nullptr || m_reading) return;
        std::string slot;
        if (id == "new") {
            if (!m_saving) return;
            // The first "save-N" past those there.
            i64 most = 0;
            for (const std::string& name : m_names) most = std::max(most, save_number(name));
            slot = "save-" + std::to_string(most + 1);
        } else if (id.starts_with("slot.") && id.size() == 6 && id[5] >= '0' && id[5] <= '9') {
            const usize i = static_cast<usize>(id[5] - '0');
            if (i >= m_slots.size()) return;
            const Slot& row = m_slots[i];
            if (!m_saving && (!row.readable || !row.of_this_world)) {
                m_message = !row.readable ? "That save cannot be read." : "That save is of an older version of this world.";
                show_values();
                return;
            }
            slot = row.name;
        } else {
            return;
        }
        if (!m_saving) {
            load(slot);
            return;
        }
        const std::string error = m_state.saves->begin_save(m_state, slot);
        if (!error.empty()) {
            m_message = error;
            show_values();
            return;
        }
        close(); // back to the game ("Saved" shows when it is written)
    } else if (screen == "menu.settings") {
        Settings s = m_settings;
        if (id == "back") {
            open(before());
            return;
        }
        if (id == "ui_scale.down") s.ui_scale -= 0.1f;
        if (id == "ui_scale.up") s.ui_scale += 0.1f;
        if (id == "fov.down") s.fov_degrees -= 5.0f;
        if (id == "fov.up") s.fov_degrees += 5.0f;
        if (id == "look.down") s.look_sensitivity -= 0.25f;
        if (id == "look.up") s.look_sensitivity += 0.25f;
        if (id == "fullscreen.toggle") s.fullscreen = !s.fullscreen;
        if (id == "vsync.toggle") s.vsync = !s.vsync;
        s.ui_scale = std::round(s.ui_scale * 20.0f) / 20.0f;
        set_settings(s);
    }
}

bool GameMenus::update(const std::vector<platform::InputEvent>& events, float pixels_per_point) {
    PlayState* play = m_state.play;
    GameUi* ui = m_state.game_ui;
    const bool playing = play != nullptr && play->playing();
    take_read();
    // Play stopped (Quit, a load, a new scene, MCP) or started: a menu open goes.
    // (Not the loading screen over a game being loaded: the load itself stops the play there was and
    // may begin the loaded game's.)
    if (m_was_playing != playing && m_menu != Menu::none && !(m_menu == Menu::loading && m_loading)) close();
    m_was_playing = playing;
    if (m_state.game_mode && m_start == nullptr) m_start = std::make_unique<EditorCamera>(m_state.camera);
    if (playing) m_state.camera.vertical_fov_degrees = m_settings.fov_degrees;
    // Only drawn menus pause the game and take the input (the game UI hidden: Esc stops play).
    const bool can_show = ui != nullptr && ui->showing(m_state);
    if (!can_show && m_menu != Menu::none && playing) close();
    // Esc in play: the pause menu.
    if (playing && play->take_escape() && m_menu == Menu::none) {
        if (can_show) open(Menu::pause);
        else play->stop(m_state.camera);
    }
    // A load begun from the menus is in, or has failed. A game plays it (one saved in play goes on
    // by itself), or leads back to the main menu saying why not (in play the game goes on, and the
    // saves' own toast says it).
    const bool busy = m_state.saves != nullptr && m_state.saves->busy();
    if (m_loading && !busy) {
        m_loading = false;
        const bool failed = m_state.saves == nullptr || m_state.saves->status().failures != m_failures_before;
        if (m_state.game_mode) {
            // (In play the game there was goes on, and the saves' own toast says what failed.)
            if (failed && playing) close();
            else if (failed) lead("That game could not be loaded: " + (m_state.saves != nullptr ? m_state.saves->status().error : std::string()));
            else {
                // (The loading screen stands on, now for the game's world: then it is played, or goes on. Over
                // another menu a command opened meanwhile too: the game that is in is to be played.)
                if (m_menu != Menu::loading) m_arriving.on = false;
                begin_play();
            }
        }
    }
    // A game about to be played: the world round the player comes in behind the loading screen.
    if (m_arriving.on) {
        if (m_menu != Menu::loading) m_arriving.on = false; // (another menu opened, or play begun or ended, by a command)
        else arrive();
    }
    // A game, outside play (and not while a game is being loaded): the main menu stands.
    if (m_state.game_mode && !(play != nullptr && play->playing()) && m_menu == Menu::none && can_show && !busy) open(Menu::main);
    const bool took = (playing || m_state.game_mode) && m_menu != Menu::none;

    if (took) {
        if (!std::isfinite(pixels_per_point) || pixels_per_point <= 0.0f) pixels_per_point = 1.0f;
        const char* screen = screen_of(m_menu);
        for (const platform::InputEvent& e : events) {
            const ui::Vec2 at{e.x * pixels_per_point, e.y * pixels_per_point};
            switch (e.type) {
            case platform::InputEvent::Type::mouse_move: ui->point(at); break;
            case platform::InputEvent::Type::mouse_button:
                if (e.down && e.button == platform::MouseButton::left) ui->click(at);
                break;
            case platform::InputEvent::Type::key: {
                if (!e.down) break;
                // Held Enter, Space or Esc: once (not pressing again, nor toggling the pause menu).
                if (e.repeat && (e.key == platform::Key::enter || e.key == platform::Key::space || e.key == platform::Key::escape))
                    break;
                const bool shift = (e.modifiers & platform::modifier::shift) != 0;
                switch (e.key) {
                case platform::Key::up:
                case platform::Key::w: ui->move_focus(screen, -1); break;
                case platform::Key::down:
                case platform::Key::s: ui->move_focus(screen, +1); break;
                case platform::Key::tab: ui->move_focus(screen, shift ? -1 : +1); break;
                case platform::Key::enter:
                case platform::Key::space: ui->activate(screen); break;
                case platform::Key::escape:
                    if (m_menu == Menu::pause) close();
                    // (The main menu is left by its buttons only; the loading screen when the world is in.)
                    else if (m_menu != Menu::main && m_menu != Menu::loading) open(before());
                    break;
                default: break;
                }
                if (m_menu == Menu::none) break; // (closed: the rest is play's, next frame)
                screen = screen_of(m_menu);
                break;
            }
            default: break;
            }
            if (m_menu == Menu::none) break;
        }
    }
    // The buttons pressed (by the window's input or MCP's ui_click and ui_key).
    if (ui != nullptr) {
        for (const ui::UiEvent& e : ui->take_events_of("menu.")) press(e.screen, e.id);
    }
    return took; // (values set as they change: open, press, settings, a read in)
}

} // namespace giant::editor
