#include "game_menus.h"

#include "editor_commands.h"
#include "game_saves.h"
#include "game_ui.h"
#include "play_state.h"
#include "world_streaming.h"

#include "giant/ai/json.h"
#include "giant/core/log.h"
#include "giant/save/slot.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>

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
                                      button("quit", "Quit to editor")});
}

Json saves_screen() {
    Json column = {title("{saves.title}"),
                   {{"type", "text"},
                    {"text", "{saves.message}"},
                    {"visible", "saves.message"},
                    {"anchor", {0.5, 0}},
                    {"color", "#ffd27a"},
                    {"font_size", 22},
                    {"wrap", true},
                    {"size", {720, 0}},
                    {"align", "center"}},
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

Json settings_screen() {
    return menu_screen("menu.settings", {title("Settings"),
                                         {{"type", "panel"}, {"size", {0, 12}}},
                                         setting_row("ui_scale", "UI scale", "{settings.ui_scale:2}"),
                                         setting_row("fov", "Field of view", "{settings.fov:0}"),
                                         setting_row("look", "Mouse speed", "{settings.look:2}"),
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
                                       std::pair{"menu.settings", settings_screen()}}) {
        std::string error;
        if (!m_state.game_ui->load_screen(name, screen.dump(), k_layer, error))
            log_error("editor", std::string("the game's menu ") + name + ": " + error);
    }
    show_values();
}

void GameMenus::set_settings(const Settings& s) {
    const auto pick = [](float v, float lo, float hi, float otherwise) { return std::isfinite(v) ? std::clamp(v, lo, hi) : otherwise; };
    m_settings.ui_scale = pick(s.ui_scale, 0.5f, 2.0f, 1.0f);
    m_settings.fov_degrees = pick(s.fov_degrees, 40.0f, 110.0f, 70.0f);
    m_settings.look_sensitivity = pick(s.look_sensitivity, 0.25f, 4.0f, 1.0f);
    if (m_state.game_ui != nullptr) m_state.game_ui->ui_scale = m_settings.ui_scale;
    if (m_state.play != nullptr) m_state.play->look_sensitivity = m_settings.look_sensitivity;
    if (m_state.play != nullptr && m_state.play->playing()) m_state.camera.vertical_fov_degrees = m_settings.fov_degrees;
    show_values();
}

bool GameMenus::paused() const { return m_menu != Menu::none && m_state.play != nullptr && m_state.play->playing(); }

void GameMenus::open(Menu menu, bool saving) {
    if (menu != Menu::none && m_menu == Menu::none && m_state.play != nullptr) m_state.play->release_keys();
    m_menu = menu;
    m_saving = saving;
    m_message.clear();
    m_reading = false;
    ++m_read_id; // (a read still under way for an earlier opening is dropped)
    // No focus kept from the last time (Enter must not press what was left focused).
    if (m_state.game_ui != nullptr && menu != Menu::none)
        m_state.game_ui->focus(menu == Menu::pause ? "menu.pause" : menu == Menu::saves ? "menu.saves" : "menu.settings", {});
    if (menu == Menu::saves) read_slots();
    show_values(); // (shown from the next layout: Down or Tab then focuses its first button)
}

void GameMenus::read_slots() {
    m_slots.clear();
    m_names.clear();
    if (m_state.saves == nullptr || m_state.world == nullptr) {
        m_message = "No world open: games are saved beside an open world.";
        return;
    }
    const std::filesystem::path root = GameSaves::root(m_state);
    if (root.empty()) {
        m_message = "No world open: games are saved beside an open world.";
        return;
    }
    const u64 hash = GameSaves::world_identity(m_state.world->index());
    m_reading = true;
    m_message = "Reading saves...";
    // On a job: every slot's manifest opened (no wait on the disk on the frame).
    m_jobs.submit(m_group, [this, root, hash, saving = m_saving, id = m_read_id] {
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
                r.slot.of_this_world = m.world_hash == hash;
                r.slot.info = when(m.saved_unix) + "   " + played(m.play_seconds) + (r.slot.of_this_world ? "" : "   (older world)");
            }
            rows.push_back(std::move(r));
        }
        std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.saved > b.saved; });
        for (Row& r : rows) {
            if (read.slots.size() >= k_rows) break;
            read.slots.push_back(std::move(r.slot));
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
    m_message = std::move(read->message);
    show_values();
}

void GameMenus::show_values() {
    if (m_state.game_ui == nullptr) return;
    ui::Values& v = m_state.game_ui->values;
    v.set("menu.pause", m_menu == Menu::pause);
    v.set("menu.saves", m_menu == Menu::saves);
    v.set("menu.settings", m_menu == Menu::settings);
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
}

void GameMenus::press(const std::string& screen, const std::string& id) {
    PlayState* play = m_state.play;
    const bool playing = play != nullptr && play->playing();
    if (screen == "menu.pause") {
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
            open(playing ? Menu::pause : Menu::none);
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
        const std::string error = m_saving ? m_state.saves->begin_save(m_state, slot) : m_state.saves->begin_load(m_state, slot, false);
        if (!error.empty()) {
            m_message = error;
            show_values();
            return;
        }
        // Saving: back to the game ("Saved" shows when it is written); loading: the game it was.
        close();
    } else if (screen == "menu.settings") {
        Settings s = m_settings;
        if (id == "back") {
            open(playing ? Menu::pause : Menu::none);
            return;
        }
        if (id == "ui_scale.down") s.ui_scale -= 0.1f;
        if (id == "ui_scale.up") s.ui_scale += 0.1f;
        if (id == "fov.down") s.fov_degrees -= 5.0f;
        if (id == "fov.up") s.fov_degrees += 5.0f;
        if (id == "look.down") s.look_sensitivity -= 0.25f;
        if (id == "look.up") s.look_sensitivity += 0.25f;
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
    if (m_was_playing != playing && m_menu != Menu::none) close();
    m_was_playing = playing;
    if (playing) m_state.camera.vertical_fov_degrees = m_settings.fov_degrees;
    // Only drawn menus pause the game and take the input (the game UI hidden: Esc stops play).
    const bool can_show = ui != nullptr && ui->showing(m_state);
    if (!can_show && m_menu != Menu::none && playing) close();
    // Esc in play: the pause menu.
    if (playing && play->take_escape() && m_menu == Menu::none) {
        if (can_show) open(Menu::pause);
        else play->stop(m_state.camera);
    }
    const bool took = playing && m_menu != Menu::none;

    if (took) {
        if (!std::isfinite(pixels_per_point) || pixels_per_point <= 0.0f) pixels_per_point = 1.0f;
        const char* screen = m_menu == Menu::pause ? "menu.pause" : m_menu == Menu::saves ? "menu.saves" : "menu.settings";
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
                    else open(Menu::pause);
                    break;
                default: break;
                }
                if (m_menu == Menu::none) break; // (closed: the rest is play's, next frame)
                screen = m_menu == Menu::pause ? "menu.pause" : m_menu == Menu::saves ? "menu.saves" : "menu.settings";
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
