// The game's menus over play: Esc pauses (the game stands still), the
// pause menu's buttons by mouse, keys or MCP; saves listed (on a job),
// saved new and over, and loaded; settings change the UI scale, the field
// of view and the look speed; held keys act once, focus does not carry
// over, the menus' screens are their own; quitting stops play; outside
// play or with the game UI hidden, menus take no input.
#include "../src/editor_commands.h"
#include "../src/game_menus.h"
#include "../src/game_saves.h"
#include "../src/game_ui.h"
#include "../src/physics_state.h"
#include "../src/play_state.h"
#include "../src/weather_state.h"
#include "../src/world_streaming.h"

#include "giant_test.h"

#include "giant/rhi/rhi.h"
#include "giant/world/scene.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

using namespace giant;
using namespace giant::editor;
using ai::Json;

namespace {

std::filesystem::path fresh_folder() {
    static std::atomic<int> n{0};
    const std::filesystem::path p =
        std::filesystem::temp_directory_path() /
        ("giant_menus_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
         std::to_string(n++));
    std::filesystem::remove_all(p);
    return p;
}

platform::InputEvent key(platform::Key k, bool down = true) {
    platform::InputEvent e;
    e.type = platform::InputEvent::Type::key;
    e.key = k;
    e.down = down;
    return e;
}

} // namespace

GIANT_TEST("game menus: Esc pauses play; buttons by mouse, keys and MCP; saves made and loaded; settings; quit") {
    const std::filesystem::path folder = fresh_folder();
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    const std::unique_ptr<rhi::Device> device = rhi::create_device(desc);
    GIANT_CHECK(device != nullptr);
    if (device == nullptr) return;
    {
        PhysicsState physics;
        PlayState play{physics};
        play.escape_stops = false;
        WeatherState weather;
        WorldStreaming streaming;
        GameSaves saves;
        GameUi game_ui(device.get());
        EditorState state;
        state.physics = &physics;
        state.play = &play;
        state.weather = &weather;
        state.world = &streaming;
        state.saves = &saves;
        state.game_ui = &game_ui;
        GameMenus menus(state);
        state.menus = &menus;
        ai::CommandRegistry commands;
        register_scene_commands(commands, state);
        register_physics_commands(commands, state);
        register_save_commands(commands, state);
        register_game_ui_commands(commands, state);
        const auto run = [&](const char* name, const Json& args = Json::object()) {
            return commands.execute(name, args, {});
        };
        // The menus' screens are there, hidden.
        GIANT_CHECK(game_ui.screen("menu.pause") != nullptr && game_ui.screen("menu.saves") != nullptr &&
                    game_ui.screen("menu.settings") != nullptr);
        world::Entity floor;
        floor.shape = world::Shape::plane;
        floor.transform.scale = {40.0f, 1.0f, 40.0f};
        state.scene.add(floor);
        GIANT_CHECK(streaming.create(folder, state.scene, state.scene_generation, {}).error.empty());
        state.camera.position = {0.0, 2.0, 0.0};
        GIANT_CHECK(run("play", {{"on", true}}).ok && play.playing());

        std::vector<platform::InputEvent> input;
        const auto frame = [&](float ppp = 1.0f) {
            const bool took = menus.update(input, ppp);
            if (!took) play.handle_input(input, false);
            input.clear();
            const double dt = menus.game_seconds(1.0 / 60.0);
            play.before_step(state.scene, state.camera, state.scene_generation, dt);
            physics.update(state.scene, state.scene.take_changes(), dt);
            if (play.playing()) play.after_step(state.camera, dt);
            saves.update(state, dt);
            game_ui.update(state, 1.0 / 60.0, 1920.0f, 1080.0f);
            return took;
        };
        for (int i = 0; i < 30; ++i) frame(); // standing
        GIANT_CHECK(!menus.paused() && state.camera.vertical_fov_degrees == menus.settings().fov_degrees);
        // Esc: paused, the pause menu shown; Esc again goes back to play.
        input.push_back(key(platform::Key::escape));
        frame();
        frame();
        GIANT_CHECK(menus.menu() == GameMenus::Menu::pause && menus.paused() && play.playing());
        GIANT_CHECK(menus.game_seconds(0.5) == 0.0 && game_ui.values.truthy("menu.pause"));
        GIANT_CHECK(game_ui.screen("menu.pause")->shown("resume"));
        // Held W while paused does not walk it.
        input.push_back(key(platform::Key::w));
        frame();
        GIANT_CHECK(play.input().forward == 0.0f);
        input.push_back(key(platform::Key::escape));
        frame();
        GIANT_CHECK(menus.menu() == GameMenus::Menu::none && !menus.paused());
        frame();
        GIANT_CHECK(menus.menu() == GameMenus::Menu::none); // (the Esc that closed it did not open it again)

        // Keys: Down focuses the first button (Resume), Enter presses it.
        input.push_back(key(platform::Key::escape));
        frame();
        frame();
        GIANT_CHECK(menus.menu() == GameMenus::Menu::pause);
        input.push_back(key(platform::Key::down));
        frame();
        GIANT_CHECK(game_ui.screen("menu.pause")->focused() == "resume");
        input.push_back(key(platform::Key::down));
        frame();
        GIANT_CHECK(game_ui.screen("menu.pause")->focused() == "save");
        input.push_back(key(platform::Key::enter));
        frame();
        frame();
        GIANT_CHECK(menus.menu() == GameMenus::Menu::saves && menus.saving());
        // The list is read on a job: until it is in, nothing is pressed.
        GIANT_CHECK(menus.reading() || menus.message().empty());
        menus.wait();
        frame();
        // No saves yet: "New save" makes save-1, and play goes on.
        GIANT_CHECK(menus.slots().empty() && game_ui.values.truthy("saves.new") && game_ui.values.truthy("saves.empty"));
        GIANT_CHECK(run("ui_click", {{"screen", "menu.saves"}, {"id", "new"}}).value["event"]["id"] == "new");
        frame();
        saves.wait();
        frame();
        GIANT_CHECK(menus.menu() == GameMenus::Menu::none && saves.status().error.empty() && saves.status().saves == 1);
        GIANT_CHECK(saves.status().slot == "save-1" && play.playing());

        // The mouse: Esc, then the pointer on "Load game" (points: half the pixels) and a click.
        input.push_back(key(platform::Key::escape));
        frame(2.0f);
        frame(2.0f);
        Json laid = run("get_ui_screen", {{"name", "menu.pause"}}).value;
        Json load;
        for (const Json& w : laid["widgets"]) {
            if (w["id"] == "load") load = w;
        }
        GIANT_CHECK(load["shown"] == true);
        platform::InputEvent move;
        move.type = platform::InputEvent::Type::mouse_move;
        move.x = static_cast<float>(load["rect"][0].get<double>() + 10.0) / 2.0f;
        move.y = static_cast<float>(load["rect"][1].get<double>() + 10.0) / 2.0f;
        platform::InputEvent click = move;
        click.type = platform::InputEvent::Type::mouse_button;
        click.button = platform::MouseButton::left;
        click.down = true;
        input = {move, click};
        frame(2.0f);
        GIANT_CHECK(menus.menu() == GameMenus::Menu::saves && !menus.saving());
        menus.wait();
        // The save is listed, with its date and play time; loading it stops play (as loads do) and the menu goes.
        Json m = run("get_menu").value;
        GIANT_CHECK(m["slots"].size() == 1 && m["slots"][0]["name"] == "save-1" && m["slots"][0]["of_this_world"] == true);
        GIANT_CHECK(m["slots"][0]["info"].get<std::string>().find("min") != std::string::npos);
        frame();
        GIANT_CHECK(game_ui.values.text("slot.0.name") == "save-1" && game_ui.values.truthy("slot.0.shown") &&
                    !game_ui.values.truthy("slot.1.shown"));
        GIANT_CHECK(run("ui_click", {{"screen", "menu.saves"}, {"id", "slot.0"}}).value["event"]["id"] == "slot.0");
        frame();
        GIANT_CHECK(saves.status().doing == "loading" || saves.status().doing == "applying" || saves.status().loads == 1);
        saves.wait();
        for (int i = 0; i < 5; ++i) frame();
        GIANT_CHECK(menus.menu() == GameMenus::Menu::none && saves.status().loads == 1 && saves.status().error.empty());

        // Settings: over MCP and by its buttons.
        if (!play.playing()) GIANT_CHECK(run("play", {{"on", true}}).ok);
        for (int i = 0; i < 3; ++i) frame();
        GIANT_CHECK(run("open_menu", {{"menu", "settings"}}).value["paused"] == true);
        frame();
        GIANT_CHECK(run("ui_click", {{"screen", "menu.settings"}, {"id", "ui_scale.up"}}).ok);
        GIANT_CHECK(run("ui_click", {{"screen", "menu.settings"}, {"id", "fov.up"}}).ok);
        GIANT_CHECK(run("ui_click", {{"screen", "menu.settings"}, {"id", "look.down"}}).ok);
        frame();
        GIANT_CHECK(std::fabs(menus.settings().ui_scale - 1.1f) < 1e-4f && game_ui.ui_scale == menus.settings().ui_scale);
        GIANT_CHECK(menus.settings().fov_degrees == 75.0f && state.camera.vertical_fov_degrees == 75.0f);
        GIANT_CHECK(menus.settings().look_sensitivity == 0.75f && play.look_sensitivity == 0.75f);
        GIANT_CHECK(std::fabs(game_ui.scale() - 1.1f) < 1e-3f);
        Json set = run("set_game_settings", {{"ui_scale", 2}, {"fov", 110}, {"look_sensitivity", 4}}).value;
        GIANT_CHECK(set["settings"]["ui_scale"] == 2.0 && set["settings"]["fov"] == 110.0);
        GIANT_CHECK(run("ui_click", {{"screen", "menu.settings"}, {"id", "fov.up"}}).ok);
        frame();
        GIANT_CHECK(menus.settings().fov_degrees == 110.0f); // (at most 110)
        GIANT_CHECK(!run("set_game_settings", {{"fov", 200}}).ok);
        GIANT_CHECK(run("ui_click", {{"screen", "menu.settings"}, {"id", "back"}}).ok);
        frame();
        GIANT_CHECK(menus.menu() == GameMenus::Menu::pause);

        // Held keys: an Esc repeating does not shut and reopen the menu, nor a held Enter press again.
        platform::InputEvent held = key(platform::Key::escape);
        held.repeat = true;
        input = {held};
        frame();
        frame();
        GIANT_CHECK(menus.menu() == GameMenus::Menu::pause);
        // The focus left on a button goes when a menu opens again (Enter does not press it).
        game_ui.focus("menu.pause", "quit");
        input.push_back(key(platform::Key::escape));
        frame();
        GIANT_CHECK(menus.menu() == GameMenus::Menu::none);
        input.push_back(key(platform::Key::escape));
        frame();
        frame();
        GIANT_CHECK(menus.menu() == GameMenus::Menu::pause && game_ui.screen("menu.pause")->focused().empty());
        input.push_back(key(platform::Key::enter));
        frame();
        GIANT_CHECK(play.playing() && menus.menu() == GameMenus::Menu::pause);
        // The menus' screens are not MCP's to load or remove, and their presses are not get_ui_events'.
        GIANT_CHECK(!run("ui_load_screen", {{"name", "menu.pause"}, {"screen", {{"type", "panel"}}}}).ok);
        GIANT_CHECK(!run("ui_remove_screen", {{"name", "menu.saves"}}).ok);
        GIANT_CHECK(run("ui_remove_screen", {{"all", true}}).ok && game_ui.screen("menu.pause") != nullptr);
        GIANT_CHECK(run("ui_click", {{"screen", "menu.pause"}, {"id", "settings"}}).ok);
        GIANT_CHECK(run("get_ui_events").value["events"].empty());
        frame();
        GIANT_CHECK(menus.menu() == GameMenus::Menu::settings);
        GIANT_CHECK(run("ui_click", {{"screen", "menu.settings"}, {"id", "back"}}).ok);
        frame();
        GIANT_CHECK(menus.menu() == GameMenus::Menu::pause);

        // MCP clearing the values does not take the open menu's away.
        GIANT_CHECK(run("ui_set_values", {{"clear", true}, {"values", {{"x", 1}}}}).ok);
        GIANT_CHECK(game_ui.values.truthy("menu.pause"));
        // A load that fails on its job (after the menu went) is a toast.
        const u64 failed = saves.status().failures;
        GIANT_CHECK(saves.begin_load(state, "not-there", false).empty());
        frame();
        saves.wait();
        frame();
        GIANT_CHECK(saves.status().failures == failed + 1 && !saves.status().error.empty());
        GIANT_CHECK(game_ui.toasts() >= 1);
        // An autosave after it does not say the old error again.
        const usize toasts = game_ui.toasts();
        for (int i = 0; i < 3; ++i) frame();
        GIANT_CHECK(game_ui.toasts() <= toasts);

        // Quit to editor: play stops, the camera goes back, the menu goes.
        GIANT_CHECK(run("ui_click", {{"screen", "menu.pause"}, {"id", "quit"}}).ok);
        frame();
        GIANT_CHECK(!play.playing() && menus.menu() == GameMenus::Menu::none && !menus.paused());
        // The game's own screens' events are left for get_ui_events.
        GIANT_CHECK(run("ui_load_screen", {{"name", "hud"}, {"screen", {{"type", "button"}, {"id", "x"}}}}).ok);
        game_ui.press("hud", "x");
        frame();
        GIANT_CHECK(run("get_ui_events").value["events"].size() == 1);

        // Outside play a menu (opened over MCP) takes no keys, and starting play closes it.
        GIANT_CHECK(run("open_menu", {{"menu", "pause"}}).ok && !menus.paused());
        input.push_back(key(platform::Key::down));
        GIANT_CHECK(!frame());
        GIANT_CHECK(run("play", {{"on", true}}).ok);
        frame();
        GIANT_CHECK(menus.menu() == GameMenus::Menu::none && !menus.paused());
        // The game UI hidden: Esc stops play (no menu to show).
        game_ui.show = GameUi::Show::never;
        for (int i = 0; i < 3; ++i) frame();
        input.push_back(key(platform::Key::escape));
        frame();
        frame();
        GIANT_CHECK(!play.playing() && menus.menu() == GameMenus::Menu::none);
        game_ui.show = GameUi::Show::playing;
    }
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
}
