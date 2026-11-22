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

#include "giant/core/text.h"
#include "giant/core/text_files.h"
#include "giant/rhi/rhi.h"
#include "giant/world/scene.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

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

// A world with a floor to stand on, and everything the menus work with.
struct Stage {
    std::unique_ptr<rhi::Device> device;
    PhysicsState physics;
    PlayState play{physics};
    WeatherState weather;
    WorldStreaming streaming;
    bool streams = true; // (false: the world streams only when the test says)
    GameSaves saves;
    std::unique_ptr<GameUi> game_ui;
    EditorState state;
    std::unique_ptr<GameMenus> menus;
    ai::CommandRegistry commands;
    std::vector<platform::InputEvent> input;

    Stage(const std::filesystem::path& folder, bool game_mode) {
        rhi::DeviceDesc desc;
        desc.backend = rhi::Backend::null;
        device = rhi::create_device(desc);
        if (device == nullptr) return;
        play.escape_stops = false;
        game_ui = std::make_unique<GameUi>(device.get());
        state.physics = &physics;
        state.play = &play;
        state.weather = &weather;
        state.world = &streaming;
        state.saves = &saves;
        state.game_ui = game_ui.get();
        state.game_mode = game_mode;
        menus = std::make_unique<GameMenus>(state);
        state.menus = menus.get();
        register_scene_commands(commands, state);
        register_physics_commands(commands, state);
        register_save_commands(commands, state);
        register_game_ui_commands(commands, state);
        world::Entity floor;
        floor.shape = world::Shape::plane;
        floor.transform.scale = {40.0f, 1.0f, 40.0f};
        state.scene.add(floor);
        GIANT_CHECK(streaming.create(folder, state.scene, state.scene_generation, {}).error.empty());
        state.camera.position = {0.0, 2.0, 0.0};
    }
    ~Stage() {
        state.menus = nullptr;
        menus.reset(); // (before the UI its screens are on)
    }

    ai::CommandResult run(const char* name, const Json& args = Json::object()) { return commands.execute(name, args, {}); }

    // One frame as the program runs it. True: a menu took its input.
    bool frame(float pixels_per_point = 1.0f) {
        const bool took = menus->update(input, pixels_per_point);
        if (streams) streaming.update(state.scene, state.scene_generation, state.camera.position, SceneFrame{});
        if (!took) play.handle_input(input, false);
        input.clear();
        const double dt = menus->game_seconds(1.0 / 60.0);
        play.before_step(state.scene, state.camera, state.scene_generation, dt);
        physics.update(state.scene, state.scene.take_changes(), dt);
        if (play.playing()) play.after_step(state.camera, dt);
        saves.update(state, dt);
        game_ui->update(state, 1.0 / 60.0, 1920.0f, 1080.0f);
        return took;
    }
};

} // namespace

GIANT_TEST("game menus: Esc pauses play; buttons by mouse, keys and MCP; saves made and loaded; settings; quit") {
    const std::filesystem::path folder = fresh_folder();
    {
        Stage stage(folder, false);
        GIANT_CHECK(stage.device != nullptr);
        if (stage.device == nullptr) return;
        PlayState& play = stage.play;
        GameSaves& saves = stage.saves;
        GameUi& game_ui = *stage.game_ui;
        EditorState& state = stage.state;
        GameMenus& menus = *stage.menus;
        std::vector<platform::InputEvent>& input = stage.input;
        const auto run = [&](const char* name, const Json& args = Json::object()) { return stage.run(name, args); };
        const auto frame = [&](float ppp = 1.0f) { return stage.frame(ppp); };
        // The menus' screens are there, hidden.
        GIANT_CHECK(game_ui.screen("menu.pause") != nullptr && game_ui.screen("menu.saves") != nullptr &&
                    game_ui.screen("menu.settings") != nullptr);
        GIANT_CHECK(run("play", {{"on", true}}).ok && play.playing());

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

GIANT_TEST("game menus: run as a game the main menu leads: new game, quit to menu, continue, load, and out only by Quit") {
    const std::filesystem::path folder = fresh_folder();
    const std::filesystem::path other = fresh_folder();
    {
        Stage stage(folder, true);
        GIANT_CHECK(stage.device != nullptr);
        if (stage.device == nullptr) return;
        GameMenus& menus = *stage.menus;
        GameUi& ui = *stage.game_ui;
        PlayState& play = stage.play;
        const auto settle = [&] {
            menus.wait();
            stage.frame();
        };
        // (Through the loading screen: a few frames more than its quiet ones.)
        const auto into_play = [&] {
            for (u32 i = 0; i < GameMenus::k_quiet_frames + 4 && !play.playing(); ++i) stage.frame();
            stage.frame(); // (the frame the menus see play begun in)
        };
        // From the first frame the main menu stands, named for the world, and its input is the menu's.
        GIANT_CHECK(stage.frame() && menus.menu() == GameMenus::Menu::main && !play.playing() && !menus.paused());
        settle();
        GIANT_CHECK(ui.values.truthy("menu.main") && ui.showing(stage.state) && ui.screen("menu.main")->shown("new"));
        GIANT_CHECK(ui.values.text("menu.title") == folder.filename().string());
        GIANT_CHECK(!ui.values.truthy("menu.continue") && !ui.values.truthy("menu.message")); // no game saved yet; nothing wrong
        // Esc does not leave it; the keys move through it.
        stage.input.push_back(key(platform::Key::escape));
        stage.frame();
        GIANT_CHECK(menus.menu() == GameMenus::Menu::main && !stage.state.quit_requested);
        // Settings and back; the saves and back (Esc): to the main menu, not out of the menus.
        GIANT_CHECK(stage.run("ui_click", {{"screen", "menu.main"}, {"id", "settings"}}).ok);
        stage.frame();
        GIANT_CHECK(menus.menu() == GameMenus::Menu::settings);
        GIANT_CHECK(stage.run("ui_click", {{"screen", "menu.settings"}, {"id", "back"}}).ok);
        stage.frame();
        GIANT_CHECK(menus.menu() == GameMenus::Menu::main);
        GIANT_CHECK(stage.run("ui_click", {{"screen", "menu.main"}, {"id", "load"}}).ok);
        stage.frame();
        GIANT_CHECK(menus.menu() == GameMenus::Menu::saves && !menus.saving());
        stage.input.push_back(key(platform::Key::escape));
        stage.frame();
        GIANT_CHECK(menus.menu() == GameMenus::Menu::main);
        settle();
        // New game by the keys: Down to it (Continue is not shown), Enter.
        stage.input.push_back(key(platform::Key::down));
        stage.frame();
        GIANT_CHECK(ui.screen("menu.main")->focused() == "new");
        stage.input.push_back(key(platform::Key::enter));
        stage.frame();
        // First the loading screen, over everything, while the world round the player comes in: here it
        // is all in, so it stands for the quiet frames and no longer. Esc does not leave it, and it is not play.
        GIANT_CHECK(menus.menu() == GameMenus::Menu::loading && menus.loading().on && !play.playing() && !menus.paused());
        GIANT_CHECK(stage.run("get_menu").value["menu"] == "loading" && stage.run("get_menu").value["loading"]["on"] == true);
        stage.input.push_back(key(platform::Key::escape));
        GIANT_CHECK(stage.frame()); // (its input is the screen's, not play's)
        GIANT_CHECK(ui.values.truthy("menu.loading") && !ui.values.truthy("menu.main") && ui.screen("menu.loading") != nullptr);
        for (u32 i = 3; i < GameMenus::k_quiet_frames; ++i) stage.frame();
        GIANT_CHECK(menus.menu() == GameMenus::Menu::loading && !play.playing());
        GIANT_CHECK(ui.values.number("loading.progress") > 0.5 && ui.values.number("loading.progress") < 1.0);
        into_play();
        GIANT_CHECK(play.playing() && menus.menu() == GameMenus::Menu::none && !menus.loading().on);
        GIANT_CHECK(!ui.values.truthy("menu.loading"));
        for (int i = 0; i < 30; ++i) stage.frame();
        // Esc pauses; the pause menu's Quit says where it leads, and leads there: play over, the main menu again.
        stage.input.push_back(key(platform::Key::escape));
        stage.frame();
        stage.frame();
        GIANT_CHECK(menus.menu() == GameMenus::Menu::pause && ui.values.text("menu.quit") == "Quit to menu");
        // A game saved first (from the pause menu).
        GIANT_CHECK(stage.run("ui_click", {{"screen", "menu.pause"}, {"id", "save"}}).ok);
        stage.frame();
        settle();
        GIANT_CHECK(stage.run("ui_click", {{"screen", "menu.saves"}, {"id", "new"}}).ok);
        stage.frame();
        stage.saves.wait();
        stage.frame();
        GIANT_CHECK(play.playing() && menus.menu() == GameMenus::Menu::none);
        stage.input.push_back(key(platform::Key::escape));
        stage.frame();
        stage.frame();
        GIANT_CHECK(stage.run("ui_click", {{"screen", "menu.pause"}, {"id", "quit"}}).ok);
        stage.frame();
        stage.frame();
        GIANT_CHECK(!play.playing() && menus.menu() == GameMenus::Menu::main && !stage.state.quit_requested);
        // Continue is there now, and plays the saved game.
        settle();
        GIANT_CHECK(ui.values.truthy("menu.continue") && ui.screen("menu.main")->shown("continue"));
        GIANT_CHECK(stage.run("ui_click", {{"screen", "menu.main"}, {"id", "continue"}}).ok);
        stage.frame();
        // The loading screen at once, over the game being read (not the world it is about to replace).
        GIANT_CHECK(menus.menu() == GameMenus::Menu::loading && menus.loading().on && menus.loading().shown == 2);
        stage.saves.wait();
        // The loaded game was saved in play, so it plays as soon as it is in, held still behind the
        // loading screen until its world is in too: then it goes on.
        for (int i = 0; i < 8 && !play.playing(); ++i) {
            stage.frame();
            GIANT_CHECK(menus.menu() == GameMenus::Menu::loading);
        }
        stage.frame(); // (the frame the menus see it playing in: the screen stays)
        GIANT_CHECK(menus.menu() == GameMenus::Menu::loading && menus.loading().on && play.playing() && menus.paused());
        GIANT_CHECK(menus.game_seconds(1.0) == 0.0);
        for (u32 i = 0; i < GameMenus::k_quiet_frames + 4 && menus.menu() != GameMenus::Menu::none; ++i) stage.frame();
        stage.frame();
        GIANT_CHECK(play.playing() && menus.menu() == GameMenus::Menu::none && !menus.paused() && !menus.loading().on);
        // Play stopped by anything else (a command) leads to the main menu too, never to nothing.
        GIANT_CHECK(stage.run("play", {{"on", false}}).ok);
        stage.saves.wait();
        for (int i = 0; i < 64 && menus.menu() != GameMenus::Menu::main; ++i) stage.frame(); // (once the loaded game is all in)
        GIANT_CHECK(menus.menu() == GameMenus::Menu::main);
        // A new game is the world as authored: nothing of the game played before is in it.
        settle();
        GIANT_CHECK(!stage.saves.status().base_slot.empty()); // (the loaded game's)
        stage.state.camera.position = {9.0, 2.0, 9.0};        // (wherever the last game left the view)
        GIANT_CHECK(stage.run("ui_click", {{"screen", "menu.main"}, {"id", "new"}}).ok);
        into_play();
        GIANT_CHECK(play.playing() && stage.saves.status().base_slot.empty());
        GIANT_CHECK(play.player() && std::abs(play.player()->position.x) < 0.5); // from where the program opened, not from there
        const auto to_the_menu = [&] {
            GIANT_CHECK(stage.run("open_menu", {{"menu", "pause"}}).ok);
            stage.frame();
            GIANT_CHECK(stage.run("ui_click", {{"screen", "menu.pause"}, {"id", "quit"}}).ok);
            stage.frame();
            stage.frame();
            GIANT_CHECK(!play.playing() && menus.menu() == GameMenus::Menu::main);
            settle();
        };
        to_the_menu();
        // A game saved outside play (as the editor saves one), loaded from the pause menu's saves, is played:
        // the load stops the play it was begun in, and the menus start the loaded game.
        GIANT_CHECK(stage.run("save_game", {{"slot", "outside"}, {"wait", true}}).ok);
        GIANT_CHECK(stage.run("ui_click", {{"screen", "menu.main"}, {"id", "new"}}).ok);
        into_play();
        GIANT_CHECK(play.playing());
        GIANT_CHECK(stage.run("open_menu", {{"menu", "pause"}}).ok);
        stage.frame();
        GIANT_CHECK(stage.run("ui_click", {{"screen", "menu.pause"}, {"id", "load"}}).ok);
        stage.frame();
        settle();
        GIANT_CHECK(!menus.slots().empty() && menus.slots()[0].name == "outside");
        // Back from the saves in play leads to the pause menu, as in the editor.
        GIANT_CHECK(stage.run("ui_click", {{"screen", "menu.saves"}, {"id", "back"}}).ok);
        stage.frame();
        GIANT_CHECK(menus.menu() == GameMenus::Menu::pause && play.playing());
        GIANT_CHECK(stage.run("ui_click", {{"screen", "menu.pause"}, {"id", "load"}}).ok);
        stage.frame();
        settle();
        GIANT_CHECK(stage.run("ui_click", {{"screen", "menu.saves"}, {"id", "slot.0"}}).ok);
        stage.frame();
        GIANT_CHECK(menus.menu() == GameMenus::Menu::loading && menus.paused()); // (the game played is held still under it)
        stage.frame();
        stage.saves.wait();
        for (int i = 0; i < 64 && (stage.saves.busy() || !play.playing()); ++i) {
            GIANT_CHECK(menus.menu() == GameMenus::Menu::loading); // (through the load stopping the play it was begun in)
            stage.frame();
        }
        stage.frame();
        GIANT_CHECK(play.playing() && menus.menu() == GameMenus::Menu::none && menus.notice().empty());
        to_the_menu();
        // A save that cannot be loaded after all (its files damaged): no game is begun in its place; the
        // main menu stands and says why, and goes on saying it once the saves are read.
        for (const auto& entry : std::filesystem::recursive_directory_iterator(GameSaves::root(stage.state) / "outside")) {
            if (!entry.is_regular_file() || entry.path().filename().string().starts_with("save")) continue;
            std::ofstream(entry.path(), std::ios::binary | std::ios::trunc) << "not what was saved";
        }
        GIANT_CHECK(stage.run("ui_click", {{"screen", "menu.main"}, {"id", "continue"}}).ok); // (the newest: that one)
        stage.frame();
        GIANT_CHECK(menus.menu() == GameMenus::Menu::loading);
        stage.saves.wait();
        for (int i = 0; i < 64 && (stage.saves.busy() || menus.menu() != GameMenus::Menu::main); ++i) stage.frame();
        GIANT_CHECK(!play.playing() && menus.menu() == GameMenus::Menu::main);
        GIANT_CHECK(menus.notice().starts_with("That game could not be loaded") && ui.values.truthy("menu.message"));
        settle();
        GIANT_CHECK(!menus.notice().empty() && ui.values.truthy("menu.message"));
        // Quit ends the program.
        settle();
        GIANT_CHECK(stage.run("ui_click", {{"screen", "menu.main"}, {"id", "quit"}}).ok);
        stage.frame();
        GIANT_CHECK(stage.state.quit_requested);
    }
    {
        // The editor has no main menu: it cannot be opened there, and its pause menu says where its Quit leads.
        Stage stage(other, false);
        GIANT_CHECK(stage.device != nullptr);
        if (stage.device != nullptr) {
            stage.frame();
            GIANT_CHECK(stage.menus->menu() == GameMenus::Menu::none);
            stage.menus->open(GameMenus::Menu::main);
            GIANT_CHECK(stage.menus->menu() == GameMenus::Menu::none && stage.game_ui->values.text("menu.quit") == "Quit to editor");
        }
    }
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
    std::filesystem::remove_all(other, ec);
}

GIANT_TEST("game menus: the loading screen stands while the world round the player comes in, and no longer than it may") {
    const std::filesystem::path folder = fresh_folder();
    {
        Stage stage(folder, true);
        GIANT_CHECK(stage.device != nullptr);
        if (stage.device == nullptr) return;
        GameMenus& menus = *stage.menus;
        PlayState& play = stage.play;
        GameUi& ui = *stage.game_ui;
        SceneFrame frame;
        stage.streams = false;
        const auto stream = [&] { stage.streaming.update(stage.state.scene, stage.state.scene_generation, stage.state.camera.position, frame); };
        // The world as authored and opened again: nothing of it in the scene (and nothing streams here
        // but when the test says so), the main menu standing.
        const auto reopened = [&](bool first) {
            if (!first) GIANT_CHECK(stage.run("play", {{"on", false}}).ok);
            GIANT_CHECK(stage.run("open_world", {{"folder", path_to_utf8(folder)}, {"discard_unsaved", true}}).ok);
            GIANT_CHECK(stage.state.scene.size() == 0);
            for (int i = 0; i < 4; ++i) stage.frame();
            menus.wait();
            stage.frame();
            GIANT_CHECK(menus.menu() == GameMenus::Menu::main);
        };
        // Two cells: the floor's, and one beside it with a thing in it.
        world::Entity thing;
        thing.shape = world::Shape::cube;
        thing.transform.position = {world::k_cell_size + 8.0, 1.0, 8.0};
        stage.state.scene.add(thing);
        GIANT_CHECK(stage.run("save_world").ok);
        reopened(true);
        GIANT_CHECK(stage.run("ui_click", {{"screen", "menu.main"}, {"id", "new"}}).ok);
        stage.frame();
        // The screen is over every other (a layer no screen a game loads can have), and the bar starts empty.
        GIANT_CHECK(ui.layer("menu.loading") == GameUi::k_top_layer && menus.loading().shown == 1);
        std::string refused;
        GIANT_CHECK(!ui.load_screen("over", R"({"type": "panel"})", GameUi::k_top_layer, refused) && !refused.empty());
        GIANT_CHECK(ui.load_screen("over", R"({"type": "panel"})", GameUi::k_max_layer, refused) && ui.screen_names().back() == "menu.loading");
        // Frames in which streaming has not looked at the world are not quiet ones: the screen stands.
        for (u32 i = 0; i < GameMenus::k_quiet_frames * 2; ++i) stage.frame();
        GIANT_CHECK(menus.menu() == GameMenus::Menu::loading && menus.loading().cells == 0 && menus.loading().progress == 0.0f);
        // Its cells being read, not yet taken: the screen stands, however many frames pass, and says what is to come.
        stream();
        for (u32 i = 0; i < GameMenus::k_quiet_frames * 4; ++i) stage.frame();
        GIANT_CHECK(menus.menu() == GameMenus::Menu::loading && menus.loading().on && !play.playing());
        GIANT_CHECK(menus.loading().cells == 2 && menus.loading().progress == 0.0f && !menus.loading().game);
        Json told = stage.run("get_menu").value;
        GIANT_CHECK(told["menu"] == "loading" && told["loading"]["on"] == true && told["loading"]["cells"] == 2);
        GIANT_CHECK(told["loading"]["game"] == false && told["loading"]["textures"] == 0 && told["loading"]["ground"] == 0);
        GIANT_CHECK(told["loading"]["progress"] == 0.0 && told["loading"]["shown"] == 1);
        // The cells stream in: the bar goes forward and never back, and a few quiet frames after the
        // last, play begins, on the ground that came.
        float before = 0.0f;
        bool between = false, forward = true;
        for (int i = 0; i < 400 && !play.playing(); ++i) {
            stage.streaming.finish_reads();
            stream();
            stage.frame();
            forward = forward && menus.loading().progress >= before;
            before = menus.loading().progress;
            between = between || (before > 0.0f && before < 1.0f && menus.loading().on);
            if (menus.loading().on && menus.loading().cells != 0) GIANT_CHECK(before <= 0.9f);
        }
        GIANT_CHECK(forward && between && menus.loading().progress == 1.0f);
        GIANT_CHECK(ui.values.number("loading.progress") == 1.0);
        stage.frame();
        GIANT_CHECK(play.playing() && menus.menu() == GameMenus::Menu::none && !menus.loading().on && stage.state.scene.size() == 2);
        GIANT_CHECK(menus.notice().empty() && stage.streaming.coming() == 0);
        told = stage.run("get_menu").value;
        GIANT_CHECK(told["menu"] == "none" && told["loading"]["on"] == false && told["loading"]["shown"] == 1);

        // The GPU still at it once the rest is in (voxel bricks to build, as the last frame said): the
        // screen stands for it, and says so; once done, play begins.
        const auto streamed_in = [&] {
            for (int i = 0; i < 400 && menus.loading().cells + static_cast<u32>(menus.loading().progress == 0.0f) != 0; ++i) {
                stage.streaming.finish_reads();
                stream();
                stage.frame();
            }
        };
        reopened(false);
        stage.state.scene_stats.voxel_pending = 7;
        GIANT_CHECK(stage.run("ui_click", {{"screen", "menu.main"}, {"id", "new"}}).ok);
        stage.frame();
        const DVec3 found = stage.state.camera.position; // the view the screen found
        streamed_in();
        for (u32 i = 0; i < GameMenus::k_quiet_frames * 4; ++i) {
            stream();
            stage.frame();
        }
        GIANT_CHECK(menus.menu() == GameMenus::Menu::loading && !play.playing() && menus.loading().cells == 0);
        GIANT_CHECK(menus.loading().voxels == 7 && menus.loading().progress < 1.0f);
        told = stage.run("get_menu").value;
        GIANT_CHECK(told["loading"]["voxels"] == 7 && told["loading"]["light"] == 0 && told["loading"]["shadows"] == 0 &&
                    told["loading"]["geometry"] == 0);
        GIANT_CHECK(told["loading"]["scatter"] == 0 && told["loading"]["far_ground"] == 0 && told["loading"]["poses"] == 0);
        // What the GPU makes round the view is made round where the player's eyes will be: the view
        // is there already while the screen stands, and play's first frame looks from the same place
        // (else the voxel world's window moves as play begins, and a slab of it is to build again).
        const DVec3 made_for = stage.state.camera.position;
        GIANT_CHECK(length(made_for - found) > 0.1 && length(made_for - play.eye_at_start(stage.state.scene, found)) < 1e-6);
        // (With the voxel world turned off, what it last said is not waited for.)
        stage.state.render_settings.voxel_world = false;
        for (u32 i = 0; i < GameMenus::k_quiet_frames * 2 && !play.playing(); ++i) {
            stream();
            stage.frame();
        }
        stage.frame();
        GIANT_CHECK(play.playing() && menus.menu() == GameMenus::Menu::none && menus.loading().progress == 1.0f);
        GIANT_CHECK(length(stage.state.camera.position - made_for) < 0.05);
        // (Play was started from the view found: stopped, the view is back there.)
        GIANT_CHECK(stage.run("play", {{"on", false}}).ok);
        GIANT_CHECK(length(stage.state.camera.position - found) < 1e-6);
        GIANT_CHECK(stage.run("play", {{"on", true}}).ok);
        stage.frame();
        GIANT_CHECK(menus.loading().voxels == 0);
        stage.state.render_settings.voxel_world = true;
        stage.state.scene_stats.voxel_pending = 0;
        // Shadow pages that are never drawn (a pool too small): waited for no longer than the GPU's most,
        // and the loading still ends in full (they are not the world's, which is in).
        reopened(false);
        stage.state.scene_stats.shadow_behind = 3;
        GIANT_CHECK(stage.run("ui_click", {{"screen", "menu.main"}, {"id", "new"}}).ok);
        stage.frame();
        streamed_in();
        for (u32 i = 0; i < GameMenus::k_quiet_frames * 4; ++i) {
            stream();
            stage.frame();
        }
        GIANT_CHECK(menus.menu() == GameMenus::Menu::loading && !play.playing() && menus.loading().shadows == 3);
        menus.loading_gpu_waited(GameMenus::k_most_gpu_seconds + 1.0);
        for (u32 i = 0; i < GameMenus::k_quiet_frames * 2 && !play.playing(); ++i) {
            stream();
            stage.frame();
        }
        stage.frame();
        GIANT_CHECK(play.playing() && menus.menu() == GameMenus::Menu::none && menus.loading().progress == 1.0f);
        stage.state.scene_stats.shadow_behind = 0;
        // The voxel world still being built (bricks built in the last frame) is waited for past that,
        // and no longer than the GPU's most in all (work that never ends, a device too slow).
        reopened(false);
        stage.state.scene_stats.voxel_pending = 9;
        stage.state.scene_stats.voxel_built = 1;
        GIANT_CHECK(stage.run("ui_click", {{"screen", "menu.main"}, {"id", "new"}}).ok);
        stage.frame();
        streamed_in();
        for (u32 i = 0; i < GameMenus::k_quiet_frames * 4; ++i) {
            stream();
            stage.frame();
        }
        GIANT_CHECK(menus.menu() == GameMenus::Menu::loading && !play.playing() && menus.loading().voxels == 9);
        menus.loading_gpu_waited(GameMenus::k_most_gpu_seconds + 1.0);
        for (u32 i = 0; i < GameMenus::k_quiet_frames * 2; ++i) {
            stream();
            stage.frame();
        }
        GIANT_CHECK(menus.menu() == GameMenus::Menu::loading && !play.playing());
        menus.loading_gpu_waited(GameMenus::k_most_gpu_seconds_in_all);
        for (u32 i = 0; i < GameMenus::k_quiet_frames * 2 && !play.playing(); ++i) {
            stream();
            stage.frame();
        }
        stage.frame();
        GIANT_CHECK(play.playing() && menus.menu() == GameMenus::Menu::none && menus.loading().progress == 1.0f);
        stage.state.scene_stats.voxel_pending = 0;
        stage.state.scene_stats.voxel_built = 0;

        // A world that never comes in (read, never taken): the screen stands up to the longest it may,
        // in seconds of frames, and then play is begun anyway.
        reopened(false);
        GIANT_CHECK(stage.run("ui_click", {{"screen", "menu.main"}, {"id", "new"}}).ok);
        stage.frame();
        stream();
        for (u32 i = 0; i < GameMenus::k_quiet_frames * 2; ++i) stage.frame();
        GIANT_CHECK(menus.menu() == GameMenus::Menu::loading && menus.loading().cells == 2 && menus.loading().shown == 5);
        menus.loading_waited(GameMenus::k_most_loading_seconds - 5.0);
        stage.frame();
        GIANT_CHECK(menus.menu() == GameMenus::Menu::loading && !play.playing());
        menus.loading_waited(6.0);
        stage.frame();
        stage.frame();
        GIANT_CHECK(play.playing() && menus.menu() == GameMenus::Menu::none && !menus.loading().on && menus.loading().progress < 1.0f);

        // Another menu opened by a command while it loads: the loading is off, and play does not begin behind it.
        reopened(false);
        GIANT_CHECK(stage.run("ui_click", {{"screen", "menu.main"}, {"id", "new"}}).ok);
        stage.frame();
        GIANT_CHECK(menus.loading().on && menus.loading().shown == 6);
        GIANT_CHECK(stage.run("open_menu", {{"menu", "settings"}}).ok);
        for (u32 i = 0; i < GameMenus::k_quiet_frames * 2; ++i) {
            stage.streaming.finish_reads();
            stream();
            stage.frame();
        }
        GIANT_CHECK(menus.menu() == GameMenus::Menu::settings && !menus.loading().on && !play.playing());
        menus.open(GameMenus::Menu::main);
        stage.frame();

        // Play begun by a command while it loads: the screen goes, the game is played.
        reopened(false);
        GIANT_CHECK(stage.run("ui_click", {{"screen", "menu.main"}, {"id", "new"}}).ok);
        stage.frame();
        GIANT_CHECK(menus.menu() == GameMenus::Menu::loading);
        GIANT_CHECK(stage.run("play", {{"on", true}}).ok);
        stage.frame();
        stage.frame();
        GIANT_CHECK(play.playing() && menus.menu() == GameMenus::Menu::none && !menus.loading().on);

        // A cell whose file cannot be read is not waited for: play begins with the rest.
        std::filesystem::path damaged;
        for (const auto& entry : std::filesystem::directory_iterator(folder / "cells")) {
            const auto text = read_text_file(entry.path());
            if (text && text->find("cube") != std::string::npos) damaged = entry.path();
        }
        GIANT_CHECK(!damaged.empty());
        std::ofstream(damaged, std::ios::binary | std::ios::trunc) << "{ not a cell";
        reopened(false);
        GIANT_CHECK(stage.run("ui_click", {{"screen", "menu.main"}, {"id", "new"}}).ok);
        for (int i = 0; i < 400 && !play.playing(); ++i) {
            stage.streaming.finish_reads();
            stream();
            stage.frame();
        }
        GIANT_CHECK(play.playing() && stage.streaming.stats().failed == 1 && stage.state.scene.size() == 1);
    }
    {
        // The loading screen is the menus' own to show: it cannot be opened by name (nothing would end it).
        Stage stage(folder / "editor", false);
        GIANT_CHECK(stage.device != nullptr);
        if (stage.device != nullptr) {
            stage.frame();
            stage.menus->open(GameMenus::Menu::loading);
            stage.frame();
            GIANT_CHECK(stage.menus->menu() == GameMenus::Menu::none && !stage.menus->loading().on && stage.menus->loading().shown == 0);
        }
    }
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
}

GIANT_TEST("game menus: a game's settings as text and back; what is wrong in the text is named and left at its default") {
    GameMenus::Settings s;
    s.ui_scale = 1.25f;
    s.fov_degrees = 95.0f;
    s.look_sensitivity = 0.5f;
    s.fullscreen = true;
    s.vsync = false;
    s.window_width = 1920;
    s.window_height = 1080;
    std::vector<std::string> problems;
    const std::string text = settings_text(s);
    GIANT_CHECK(settings_from_text(text, problems) == s && problems.empty());
    // Text a person can read and change: a key a line.
    GIANT_CHECK(text.find("\"fov\": 95") != std::string::npos && text.find("\"fullscreen\": true") != std::string::npos &&
                text.back() == '\n' && std::count(text.begin(), text.end(), '\n') >= 7);
    // Exactly this: the keys in this order, one a line, a step of the menu's (1.1 as a float) as a person would write it.
    GameMenus::Settings stepped;
    stepped.ui_scale = 1.1f;
    stepped.look_sensitivity = 0.35f;
    stepped.window_width = 1280;
    stepped.window_height = 720;
    GIANT_CHECK(settings_text(stepped) == "{\n  \"ui_scale\": 1.1,\n  \"fov\": 70.0,\n  \"look_sensitivity\": 0.35,\n"
                                          "  \"fullscreen\": false,\n  \"vsync\": true,\n  \"window\": [1280, 720]\n}\n");
    // A number beyond what a float holds is brought in as any too large is, not taken for none.
    const GameMenus::Settings huge = settings_from_text(R"({"ui_scale": 1e300, "fov": -1e300})", problems);
    GIANT_CHECK(huge.ui_scale == 2.0f && huge.fov_degrees == 40.0f && problems.empty());
    // Keys left out keep their defaults; others are not minded.
    GIANT_CHECK(settings_from_text(R"({"fov": 60, "later": [1, 2]})", problems).fov_degrees == 60.0f && problems.empty());
    GIANT_CHECK(settings_from_text("{}", problems) == GameMenus::Settings{} && problems.empty());
    // Values out of range are brought in; a window too small or too large too, one of no size is none.
    const GameMenus::Settings wide = settings_from_text(
        R"({"ui_scale": 9, "fov": 1, "look_sensitivity": 100, "window": [100, 99999]})", problems);
    GIANT_CHECK(wide.ui_scale == 2.0f && wide.fov_degrees == 40.0f && wide.look_sensitivity == 4.0f && problems.empty());
    GIANT_CHECK(wide.window_width == 320 && wide.window_height == 4320);
    const GameMenus::Settings none = settings_from_text(R"({"window": [0, 900]})", problems);
    GIANT_CHECK(none.window_width == 0 && none.window_height == 0 && problems.empty());
    // What is of the wrong kind is named and keeps its default; the rest is taken.
    const GameMenus::Settings mixed = settings_from_text(
        R"({"ui_scale": "big", "fov": 80, "fullscreen": 1, "vsync": "no", "window": [800], "look_sensitivity": null})", problems);
    GIANT_CHECK(mixed.ui_scale == 1.0f && mixed.fov_degrees == 80.0f && !mixed.fullscreen && mixed.vsync && mixed.window_width == 0);
    GIANT_CHECK(problems.size() == 5);
    problems.clear();
    GIANT_CHECK(settings_from_text(R"({"window": [800, -600]})", problems).window_width == 0 && problems.size() == 1);
    // No JSON, or no object: the defaults, and it says so.
    for (const char* bad : {"", "fov = 90", "[1, 2]", "{\"fov\": "}) {
        problems.clear();
        GIANT_CHECK(settings_from_text(bad, problems) == GameMenus::Settings{} && problems.size() == 1);
    }
}

GIANT_TEST("game menus: a game's settings are kept in a file as they change, the window's size as the program ends") {
    const std::filesystem::path folder = fresh_folder();
    const std::filesystem::path file = folder / "settings" / "deeper" / "game_settings.json"; // (a folder not there yet)
    const auto kept = [&] {
        std::vector<std::string> problems;
        const std::optional<std::string> text = read_text_file(file);
        GIANT_CHECK(text.has_value());
        const GameMenus::Settings s = settings_from_text(text.value_or(""), problems);
        GIANT_CHECK(problems.empty());
        return s;
    };
    {
        Stage stage(folder, false);
        GIANT_CHECK(stage.device != nullptr);
        if (stage.device == nullptr) return;
        GameMenus& menus = *stage.menus;
        const auto run = [&](const char* name, const Json& args = Json::object()) { return stage.run(name, args); };
        // Kept nowhere: nothing is written, whatever changes (the editor's own menus).
        GIANT_CHECK(run("set_game_settings", {{"fov", 100}}).value["settings"]["kept_in"] == "");
        GIANT_CHECK(menus.store() && !std::filesystem::exists(folder / "settings"));
        // Kept in a file from now on: what is set now is taken as what it holds, so nothing is written yet.
        menus.keep_in(path_to_utf8(file));
        GIANT_CHECK(menus.store() && !std::filesystem::exists(file));
        GIANT_CHECK(run("get_menu").value["settings"]["kept_in"] == path_to_utf8(file));
        // The menu's buttons and the command change them: written (on a job; store waits for it).
        GIANT_CHECK(run("play", {{"on", true}}).ok);
        for (int i = 0; i < 3; ++i) stage.frame();
        GIANT_CHECK(run("open_menu", {{"menu", "settings"}}).ok);
        stage.frame();
        GIANT_CHECK(run("ui_click", {{"screen", "menu.settings"}, {"id", "fullscreen.toggle"}}).ok);
        GIANT_CHECK(run("ui_click", {{"screen", "menu.settings"}, {"id", "vsync.toggle"}}).ok);
        stage.frame();
        GIANT_CHECK(menus.settings().fullscreen && !menus.settings().vsync);
        GIANT_CHECK(stage.game_ui->values.text("settings.fullscreen") == "On" && stage.game_ui->values.text("settings.vsync") == "Off");
        GIANT_CHECK(menus.store());
        GIANT_CHECK(kept() == menus.settings() && kept().fullscreen && !kept().vsync && kept().fov_degrees == 100.0f);
        const Json set = run("set_game_settings", {{"fullscreen", false}, {"vsync", true}, {"look_sensitivity", 2}}).value;
        GIANT_CHECK(set["settings"]["fullscreen"] == false && set["settings"]["vsync"] == true);
        GIANT_CHECK(menus.store() && kept() == menus.settings() && kept().look_sensitivity == 2.0f);
        // The window's size: noted while it is a window, not written until the program ends; not noted fullscreen.
        const auto written = std::filesystem::last_write_time(file);
        menus.note_window(1280, 720);
        stage.frame();
        GIANT_CHECK(menus.settings().window_width == 1280 && kept().window_width == 0);
        GIANT_CHECK(std::filesystem::last_write_time(file) == written);
        // A setting set to what it is, the window's size other than the file's: still no reason to write.
        GIANT_CHECK(run("set_game_settings", {{"look_sensitivity", 2}}).ok);
        menus.wait();
        GIANT_CHECK(kept().window_width == 0);
        // Many changes one after another, the program ending at once: the file holds the last (each
        // write is the newest text, one at a time, whichever thread runs the job).
        for (int i = 0; i < 60; ++i) GIANT_CHECK(run("set_game_settings", {{"fov", 41 + i}, {"vsync", i % 2 == 0}}).ok);
        GIANT_CHECK(menus.store() && kept() == menus.settings() && kept().fov_degrees == 100.0f && !kept().vsync);
        GIANT_CHECK(kept().window_width == 1280); // (written with the change: the size as noted)
        GIANT_CHECK(run("set_game_settings", {{"vsync", true}}).ok);
        GIANT_CHECK(run("set_game_settings", {{"fullscreen", true}}).ok);
        menus.note_window(3840, 2160);
        menus.note_window(0, 0);
        GIANT_CHECK(menus.settings().window_width == 1280 && menus.settings().window_height == 720);
        GIANT_CHECK(menus.store() && kept() == menus.settings() && kept().window_width == 1280 && kept().fullscreen);
    }
    // The next run: read, set and kept in the same file, they are as they were left.
    const std::filesystem::path second = fresh_folder(), third = fresh_folder();
    {
        Stage stage(second, false);
        if (stage.device == nullptr) return;
        stage.menus->set_settings(kept());
        stage.menus->keep_in(path_to_utf8(file));
        const GameMenus::Settings& s = stage.menus->settings();
        GIANT_CHECK(s.fullscreen && s.vsync && s.fov_degrees == 100.0f && s.look_sensitivity == 2.0f && s.window_height == 720);
        GIANT_CHECK(stage.game_ui->ui_scale == s.ui_scale && stage.play.look_sensitivity == 2.0f);
    }
    // A file that cannot be written (its name is a folder's): store says so, and the settings hold.
    {
        Stage stage(third, false);
        if (stage.device == nullptr) return;
        stage.menus->keep_in(path_to_utf8(folder));
        GIANT_CHECK(stage.run("set_game_settings", {{"fov", 50}}).ok);
        // (The job's write failed: store tries again, and says it could not, each time.)
        GIANT_CHECK(!stage.menus->store() && stage.menus->settings().fov_degrees == 50.0f);
        GIANT_CHECK(!stage.menus->store());
        // Kept in a file that can be written from now on: nothing is owed to the old one, and a change is written.
        const std::filesystem::path good = third / "kept" / "game_settings.json";
        stage.menus->keep_in(path_to_utf8(good));
        GIANT_CHECK(stage.menus->store() && !std::filesystem::exists(good));
        GIANT_CHECK(stage.run("set_game_settings", {{"fov", 51}}).ok);
        GIANT_CHECK(stage.menus->store());
        std::vector<std::string> problems;
        GIANT_CHECK(settings_from_text(read_text_file(good).value_or(""), problems).fov_degrees == 51.0f && problems.empty());
    }
    std::error_code ec;
    for (const std::filesystem::path& made : {folder, second, third}) std::filesystem::remove_all(made, ec);
}
