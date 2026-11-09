// The game's own UI in the editor: shown while playing (or always, or
// never), a crosshair while playing, toasts that fade and go, scaled by the
// window's height; drawn on the null device without a complaint.
#include "../src/editor_commands.h"
#include "../src/game_ui.h"
#include "../src/physics_state.h"
#include "../src/play_state.h"

#include "giant_test.h"

#include "giant/rhi/rhi.h"
#include "giant/world/scene.h"

#include <string>
#include <vector>

using namespace giant;
using namespace giant::editor;
using ai::Json;

GIANT_TEST("game ui: shown while playing, a crosshair and toasts, scaled to the window; drawn on the null device") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    const std::unique_ptr<rhi::Device> device = rhi::create_device(desc);
    GIANT_CHECK(device != nullptr);
    if (device == nullptr) return;
    PhysicsState physics;
    PlayState play{physics};
    GameUi game_ui(device.get());
    EditorState state;
    state.physics = &physics;
    state.play = &play;
    state.game_ui = &game_ui;
    ai::CommandRegistry commands;
    register_scene_commands(commands, state);
    register_physics_commands(commands, state);
    register_game_ui_commands(commands, state);
    const auto run = [&](const char* name, const Json& args = Json::object()) {
        return commands.execute(name, args, {});
    };
    world::Entity floor;
    floor.shape = world::Shape::plane;
    floor.transform.scale = {40.0f, 1.0f, 40.0f};
    state.scene.add(floor);

    ai::CommandResult r = run("get_game_ui");
    GIANT_CHECK(r.ok && r.value["show"] == "playing" && r.value["error"] == "");
    GIANT_CHECK(r.value["font"]["glyphs"].get<int>() > 300 && r.value["font"]["on_gpu"] == true);
    // Not playing: not shown, nothing drawn.
    game_ui.update(state, 1.0 / 60.0, 1920.0f, 1080.0f);
    GIANT_CHECK(!game_ui.shown() && game_ui.list().vertices.empty());
    // Playing: the crosshair (8 rectangles: dark edges, then light), at the middle.
    GIANT_CHECK(run("play", {{"on", true}}).ok && play.playing());
    game_ui.update(state, 1.0 / 60.0, 1920.0f, 1080.0f);
    GIANT_CHECK(game_ui.shown() && game_ui.vertices() == 8 * 4 && game_ui.commands() == 1);
    float lo = 1e9f, hi = -1e9f;
    for (const rhi::UiVertex& v : game_ui.list().vertices) {
        lo = std::min(lo, v.x);
        hi = std::max(hi, v.x);
    }
    GIANT_CHECK(std::fabs(0.5f * (lo + hi) - 960.0f) <= 1.0f && hi - lo < 40.0f);
    // At 2160 lines: twice the size.
    game_ui.update(state, 1.0 / 60.0, 3840.0f, 2160.0f);
    GIANT_CHECK(game_ui.scale() == 2.0f);
    float lo2 = 1e9f, hi2 = -1e9f;
    for (const rhi::UiVertex& v : game_ui.list().vertices) {
        lo2 = std::min(lo2, v.x);
        hi2 = std::max(hi2, v.x);
    }
    GIANT_CHECK(std::fabs((hi2 - lo2) - 2.0f * (hi - lo)) <= 2.0f);
    // A toast: a band and its text, in the same draw call; gone after its seconds.
    r = run("show_toast", {{"text", "Checkpoint reached"}, {"seconds", 1.0}});
    GIANT_CHECK(r.ok && r.value["toasts"] == 1);
    game_ui.update(state, 0.5, 1920.0f, 1080.0f);
    GIANT_CHECK(game_ui.vertices() > 8 * 4 + 4 * 10 && game_ui.commands() == 1);
    // Fading over its last half second: a quarter second left, its band half as dark.
    game_ui.update(state, 0.25, 1920.0f, 1080.0f);
    bool faded = false;
    for (const rhi::UiVertex& v : game_ui.list().vertices) {
        if ((v.color & 0xFFFFFFu) == ui::rgba(10, 12, 14, 0)) faded = std::abs(static_cast<int>(v.color >> 24) - 85) <= 1;
    }
    GIANT_CHECK(faded);
    game_ui.update(state, 0.35, 1920.0f, 1080.0f);
    GIANT_CHECK(game_ui.toasts() == 0 && game_ui.vertices() == 8 * 4);
    // At most four at once (the oldest goes); bad input refused.
    for (int i = 0; i < 6; ++i) GIANT_CHECK(run("show_toast", {{"text", "t" + std::to_string(i)}}).ok);
    GIANT_CHECK(game_ui.toasts() == GameUi::k_max_toasts);
    GIANT_CHECK(!run("show_toast", {{"text", ""}}).ok);
    GIANT_CHECK(!run("show_toast", {{"text", "x"}, {"seconds", 100}}).ok);
    // Drawn on the null device: the list is one it takes.
    rhi::TextureDesc target_desc;
    target_desc.width = 1920;
    target_desc.height = 1080;
    target_desc.format = rhi::Format::rgba8_srgb;
    target_desc.usage = rhi::TextureUsage::render_target;
    const rhi::TextureHandle target = device->create_texture(target_desc);
    game_ui.update(state, 1.0 / 60.0, 1920.0f, 1080.0f);
    const rhi::UiDrawList list = game_ui.list();
    rhi::CommandList* cmd = device->begin_frame(nullptr);
    cmd->draw_ui(target, rhi::UiFrame{std::span<const rhi::UiDrawList>(&list, 1)});
    device->end_frame();
    GIANT_CHECK(device->last_frame_stats().ui_draws == 1);
    device->destroy(target);
    // Never, and always (in the editor too).
    GIANT_CHECK(run("set_game_ui", {{"show", "never"}}).ok);
    game_ui.update(state, 1.0 / 60.0, 1920.0f, 1080.0f);
    GIANT_CHECK(!game_ui.shown());
    GIANT_CHECK(run("play", {{"on", false}}).ok);
    r = run("set_game_ui", {{"show", "always"}});
    GIANT_CHECK(r.ok && r.value["show"] == "always");
    game_ui.update(state, 1.0 / 60.0, 1920.0f, 1080.0f);
    GIANT_CHECK(game_ui.shown() && game_ui.vertices() > 0); // the toasts, no crosshair
    GIANT_CHECK(!run("set_game_ui", {{"show", "sometimes"}}).ok);
}

GIANT_TEST("game ui: screens from JSON over the scene, by layer; values filled in; clicks and keys queued as events") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    const std::unique_ptr<rhi::Device> device = rhi::create_device(desc);
    GIANT_CHECK(device != nullptr);
    if (device == nullptr) return;
    PhysicsState physics;
    PlayState play{physics};
    GameUi game_ui(device.get());
    EditorState state;
    state.physics = &physics;
    state.play = &play;
    state.game_ui = &game_ui;
    ai::CommandRegistry commands;
    register_game_ui_commands(commands, state);
    const auto run = [&](const char* name, const Json& args = Json::object()) {
        return commands.execute(name, args, {});
    };
    GIANT_CHECK(run("set_game_ui", {{"show", "always"}}).ok);

    // A HUD: ammo text bottom right, a health bar bottom left.
    const Json hud = {{"type", "panel"},
                      {"stretch", {true, true}},
                      {"children",
                       {{{"type", "text"}, {"id", "ammo"}, {"text", "{ammo} / {spare}"}, {"anchor", {1, 1}}, {"offset", {-40, -40}}},
                        {{"type", "bar"},
                         {"id", "hp"},
                         {"value", "health"},
                         {"max", 100},
                         {"size", {300, 16}},
                         {"anchor", {0, 1}},
                         {"offset", {40, -40}},
                         {"background", "#00000080"},
                         {"color", "#c03030"}}}}};
    ai::CommandResult r = run("ui_load_screen", {{"name", "hud"}, {"screen", hud}});
    GIANT_CHECK(r.ok && r.value["screens"].size() == 1 && r.value["screens"][0]["widgets"] == 3);
    // A pause menu on top, given as text, shown while "paused".
    const std::string menu = R"({"type": "panel", "stretch": [true, true], "visible": "paused", "background": "#00000099",
        "children": [{"type": "panel", "anchor": [0.5, 0.5], "layout": "column", "gap": 12, "children": [
            {"type": "button", "id": "resume", "text": "Resume", "size": [320, 56], "background": "#202020", "hover": "#404040"},
            {"type": "button", "id": "quit", "text": "Quit", "size": [320, 56], "background": "#202020"}]}]})";
    r = run("ui_load_screen", {{"name", "pause"}, {"screen", menu}, {"layer", 10}});
    GIANT_CHECK(r.ok && r.value["screens"].size() == 2 && r.value["screens"][1]["name"] == "pause");
    // Wrong screens refused, saying where; nothing changes.
    r = run("ui_load_screen", {{"name", "bad"}, {"screen", {{"type", "panel"}, {"colour", "#fff"}}}});
    GIANT_CHECK(!r.ok && r.error.find("colour") != std::string::npos && game_ui.screen("bad") == nullptr);
    GIANT_CHECK(!run("ui_load_screen", {{"name", "bad"}, {"screen", "{"}}).ok);
    GIANT_CHECK(!run("ui_load_screen", {{"name", "bad"}, {"screen", 3}}).ok);

    r = run("ui_set_values", {{"values", {{"ammo", 30}, {"spare", 90}, {"health", 50}, {"paused", false}}}});
    GIANT_CHECK(r.ok && r.value["values"]["ammo"] == 30.0 && r.value["values"].size() == 4);
    GIANT_CHECK(!run("ui_set_values", {{"values", {{"x", Json::array()}}}}).ok);
    game_ui.update(state, 1.0 / 60.0, 1920.0f, 1080.0f);
    GIANT_CHECK(game_ui.shown() && game_ui.vertices() > 0);
    const usize hud_only = game_ui.vertices();
    r = run("get_ui_screen", {{"name", "hud"}});
    GIANT_CHECK(r.ok && r.value["widgets"].size() == 3);
    const Json& hp = r.value["widgets"][2];
    GIANT_CHECK(hp["id"] == "hp" && hp["rect"][0] == 40.0 && hp["rect"][1] == 1080.0 - 40.0 - 16.0 && hp["rect"][2] == 300.0);
    const Json& ammo = r.value["widgets"][1];
    GIANT_CHECK(ammo["shown"] == true && std::fabs(ammo["rect"][0].get<double>() + ammo["rect"][2].get<double>() - 1880.0) < 0.01);
    // The bar's fill: half its width.
    bool half = false;
    for (const rhi::UiVertex& v : game_ui.list().vertices) {
        if (v.color == ui::rgba(0xc0, 0x30, 0x30) && std::fabs(v.x - 190.0f) < 0.01f) half = true;
    }
    GIANT_CHECK(half);
    // The menu hidden: its buttons not clicked.
    GIANT_CHECK(run("ui_click", {{"x", 960}, {"y", 540}}).value["event"].is_null());
    GIANT_CHECK(run("ui_click", {{"screen", "pause"}, {"id", "resume"}}).value["event"].is_null());

    // Paused: the menu over the HUD, its buttons clicked and moved through.
    GIANT_CHECK(run("ui_set_values", {{"values", {{"paused", true}}}}).ok);
    game_ui.update(state, 1.0 / 60.0, 1920.0f, 1080.0f);
    GIANT_CHECK(game_ui.vertices() > hud_only);
    r = run("get_ui_screen", {{"name", "pause"}});
    const Json& resume = r.value["widgets"][2];
    GIANT_CHECK(resume["id"] == "resume" && resume["shown"] == true);
    const double rx = resume["rect"][0].get<double>() + 10.0, ry = resume["rect"][1].get<double>() + 10.0;
    r = run("ui_click", {{"x", rx}, {"y", ry}});
    GIANT_CHECK(r.ok && r.value["event"]["screen"] == "pause" && r.value["event"]["id"] == "resume");
    GIANT_CHECK(run("ui_click", {{"x", 5}, {"y", 5}}).value["event"].is_null());
    r = run("ui_key", {{"screen", "pause"}, {"key", "next"}});
    GIANT_CHECK(r.ok && r.value["focused"] == "quit" && r.value["event"].is_null());
    r = run("ui_key", {{"screen", "pause"}, {"key", "activate"}});
    GIANT_CHECK(r.ok && r.value["event"]["id"] == "quit");
    r = run("ui_click", {{"screen", "pause"}, {"id", "resume"}});
    GIANT_CHECK(r.ok && r.value["event"]["id"] == "resume");
    GIANT_CHECK(!run("ui_click", {{"screen", "nope"}, {"id", "resume"}}).ok);
    GIANT_CHECK(!run("ui_click", {{"x", 1}}).ok);
    GIANT_CHECK(!run("ui_key", {{"screen", "nope"}, {"key", "next"}}).ok);
    // The focused button lit: its hover colour.
    GIANT_CHECK(run("ui_key", {{"screen", "pause"}, {"key", "previous"}}).value["focused"] == "quit");
    GIANT_CHECK(run("ui_key", {{"screen", "pause"}, {"key", "previous"}}).value["focused"] == "resume");
    game_ui.update(state, 1.0 / 60.0, 1920.0f, 1080.0f);
    bool lit = false;
    for (const rhi::UiVertex& v : game_ui.list().vertices) lit = lit || v.color == ui::rgba(0x40, 0x40, 0x40);
    GIANT_CHECK(lit);

    // The events, oldest first, taken once.
    r = run("get_ui_events");
    GIANT_CHECK(r.ok && r.value["events"].size() == 3 && r.value["events"][0]["id"] == "resume" &&
                r.value["events"][1]["id"] == "quit" && r.value["events"][2]["id"] == "resume");
    GIANT_CHECK(run("get_ui_events").value["events"].empty());
    // At most 64 kept: the oldest go.
    for (int i = 0; i < 70; ++i) game_ui.press("pause", i % 2 == 0 ? "resume" : "quit");
    const std::vector<ui::UiEvent> events = game_ui.take_events();
    GIANT_CHECK(events.size() == GameUi::k_max_events && events.back().id == "quit");

    // Replaced under the same name: its focus kept; removed; limits.
    GIANT_CHECK(run("ui_load_screen", {{"name", "pause"}, {"screen", menu}, {"layer", 10}}).ok);
    GIANT_CHECK(game_ui.screen("pause")->focused() == "quit" && game_ui.screen_names().size() == 2); // (the last pressed)
    GIANT_CHECK(run("ui_load_screen", {{"name", "under"}, {"screen", {{"type", "panel"}}}, {"layer", -5}}).ok);
    GIANT_CHECK(game_ui.screen_names() == std::vector<std::string>({"under", "hud", "pause"}));
    GIANT_CHECK(!run("ui_load_screen", {{"name", "x"}, {"screen", {{"type", "panel"}}}, {"layer", 5000}}).ok);
    std::string error;
    for (usize i = game_ui.screen_names().size(); i < GameUi::k_max_screens; ++i)
        GIANT_CHECK(game_ui.load_screen("s" + std::to_string(i), R"({"type": "panel"})", 0, error));
    GIANT_CHECK(!game_ui.load_screen("one too many", R"({"type": "panel"})", 0, error) && !error.empty());
    GIANT_CHECK(game_ui.load_screen("hud", R"({"type": "panel"})", 0, error)); // (replacing is not one more)
    GIANT_CHECK(run("ui_remove_screen", {{"name", "hud"}}).ok && game_ui.screen("hud") == nullptr);
    GIANT_CHECK(!run("ui_remove_screen", {{"name", "hud"}}).ok && !run("ui_remove_screen").ok);
    r = run("ui_remove_screen", {{"all", true}});
    GIANT_CHECK(r.ok && r.value["screens"].empty());
    // Values: null forgets one; clear forgets all.
    r = run("ui_set_values", {{"values", {{"ammo", nullptr}}}});
    GIANT_CHECK(r.ok && r.value["values"]["ammo"].is_null() && r.value["count"] == 3 + 2); // (and heading, playing: the editor's)
    r = run("ui_set_values", {{"clear", true}, {"values", {{"name", "Ana"}}}});
    GIANT_CHECK(r.ok && r.value["count"] == 1 && r.value["values"]["name"] == "Ana");
    // At the most: one forgotten makes room for one more.
    for (usize i = 1; i < ui::Values::k_max_values; ++i) game_ui.values.set("v" + std::to_string(i), 1.0);
    GIANT_CHECK(!run("ui_set_values", {{"values", {{"new", 1}}}}).ok);
    r = run("ui_set_values", {{"values", {{"v1", nullptr}, {"new", 1}}}});
    GIANT_CHECK(r.ok && r.value["count"] == ui::Values::k_max_values);
    GIANT_CHECK(run("ui_set_values", {{"clear", true}}).ok && game_ui.values.size() == 0);
    // Not shown: clicks go nowhere.
    GIANT_CHECK(run("ui_load_screen", {{"name", "pause"}, {"screen", menu}}).ok);
    GIANT_CHECK(run("ui_set_values", {{"values", {{"paused", true}}}}).ok);
    game_ui.update(state, 1.0 / 60.0, 1920.0f, 1080.0f);
    GIANT_CHECK(run("ui_click", {{"x", rx}, {"y", ry}}).value["event"]["id"] == "resume");
    GIANT_CHECK(run("set_game_ui", {{"show", "never"}}).ok); // (at once: before a frame)
    GIANT_CHECK(run("ui_click", {{"x", rx}, {"y", ry}}).value["event"].is_null());
    GIANT_CHECK(run("ui_click", {{"screen", "pause"}, {"id", "resume"}}).value["event"].is_null());
    GIANT_CHECK(!run("ui_click", {{"x", 1e30}, {"y", 0}}).ok);
    GIANT_CHECK(run("ui_key", {{"screen", "pause"}, {"key", "activate"}}).value["event"].is_null());

    // Loaded after a frame: laid out at once, so what it hides is not pressed before it is drawn.
    GIANT_CHECK(run("set_game_ui", {{"show", "always"}}).ok);
    game_ui.update(state, 1.0 / 60.0, 1920.0f, 1080.0f);
    const std::string hidden = R"({"type": "panel", "stretch": [true, true], "children": [
        {"type": "button", "id": "secret", "size": [100, 100], "visible": "open"},
        {"type": "button", "id": "plain", "size": [100, 100], "anchor": [1, 1], "pivot": [1, 1]}]})";
    GIANT_CHECK(run("ui_load_screen", {{"name", "late"}, {"screen", hidden}, {"layer", 100}}).ok);
    GIANT_CHECK(run("ui_click", {{"screen", "late"}, {"id", "secret"}}).value["event"].is_null());
    GIANT_CHECK(run("ui_click", {{"x", 1915}, {"y", 1075}}).value["event"]["id"] == "plain");
    GIANT_CHECK(run("ui_key", {{"screen", "late"}, {"key", "next"}}).value["focused"] == "plain");
    // A screen showing a background takes clicks from the screens under it.
    GIANT_CHECK(run("ui_load_screen", {{"name", "modal"}, {"screen", {{"type", "panel"}, {"background", "#000000c0"}}}, {"layer", 200}}).ok);
    GIANT_CHECK(run("ui_click", {{"x", 1915}, {"y", 1075}}).value["event"].is_null());
    GIANT_CHECK(run("ui_remove_screen", {{"name", "modal"}}).ok);
    GIANT_CHECK(run("ui_click", {{"x", 1915}, {"y", 1075}}).value["event"]["id"] == "plain");
}

GIANT_TEST("game ui: HUD pieces: markers in the world, a compass with them on it, a fade, and the player's values") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    const std::unique_ptr<rhi::Device> device = rhi::create_device(desc);
    GIANT_CHECK(device != nullptr);
    if (device == nullptr) return;
    PhysicsState physics;
    PlayState play{physics};
    GameUi game_ui(device.get());
    EditorState state;
    state.physics = &physics;
    state.play = &play;
    state.game_ui = &game_ui;
    ai::CommandRegistry commands;
    register_scene_commands(commands, state);
    register_physics_commands(commands, state);
    register_game_ui_commands(commands, state);
    const auto run = [&](const char* name, const Json& args = Json::object()) {
        return commands.execute(name, args, {});
    };
    GIANT_CHECK(run("set_game_ui", {{"show", "always"}}).ok);
    state.camera.position = {0.0, 2.0, 0.0};
    state.camera.yaw_degrees = 0.0f; // north
    state.camera.pitch_degrees = 0.0f;

    // Markers: one ahead (north), one behind, one east and only near.
    ai::CommandResult r = run("ui_set_marker", {{"id", "camp"}, {"position", {0, 2, -200}}, {"label", "Camp"}});
    GIANT_CHECK(r.ok && r.value["markers"] == 1);
    GIANT_CHECK(run("ui_set_marker", {{"id", "home"}, {"position", {0, 2, 300}}, {"color", "#40c0ffff"}}).ok);
    GIANT_CHECK(run("ui_set_marker", {{"id", "east"}, {"position", {50, 2, 0}}, {"max_distance", 20}}).ok);
    GIANT_CHECK(!run("ui_set_marker", {{"id", "bad"}, {"position", {0, 0, 0}}, {"color", "red"}}).ok);
    GIANT_CHECK(!run("ui_set_marker", {{"id", "bad"}, {"position", {0, 0}}}).ok);
    // A compass across the top, the heading under it.
    const Json hud = {{"type", "panel"},
                      {"children",
                       {{{"type", "compass"}, {"id", "compass"}, {"value", "heading"}, {"size", {640, 44}}, {"anchor", {0.5, 0}},
                         {"offset", {0, 16}}, {"background", "#00000060"}},
                        {{"type", "text"}, {"id", "deg"}, {"text", "{heading}"}, {"anchor", {0.5, 0}}, {"offset", {0, 64}}}}}};
    GIANT_CHECK(run("ui_load_screen", {{"name", "hud"}, {"screen", hud}}).ok);
    game_ui.update(state, 1.0 / 60.0, 1920.0f, 1080.0f);
    r = run("get_ui_markers");
    GIANT_CHECK(r.ok && r.value["markers"].size() == 3);
    const auto marker = [&](const char* id) {
        for (const Json& m : r.value["markers"]) {
            if (m["id"] == id) return m;
        }
        return Json();
    };
    GIANT_CHECK(marker("camp")["on_screen"] == true && std::fabs(marker("camp")["at"][0].get<double>() - 960.0) < 1.0);
    GIANT_CHECK(std::fabs(marker("camp")["distance"].get<double>() - 200.0) < 0.1);
    GIANT_CHECK(marker("home")["shown"] == true && marker("home")["on_screen"] == false); // at the bottom edge
    GIANT_CHECK(std::fabs(marker("home")["bearing"].get<double>() - 180.0) < 0.1);
    GIANT_CHECK(marker("east")["shown"] == false); // further than its distance
    GIANT_CHECK(game_ui.values.number("heading", -1.0) == 0.0 && game_ui.values.truthy("playing") == false);
    // Turned west: the heading reads 270, and what is shown follows.
    state.camera.yaw_degrees = 90.0f;
    game_ui.update(state, 1.0 / 60.0, 1920.0f, 1080.0f);
    GIANT_CHECK(game_ui.values.number("heading") == 270.0);
    r = run("get_ui_markers");
    GIANT_CHECK(marker("camp")["on_screen"] == false && marker("camp")["shown"] == true);
    GIANT_CHECK(game_ui.screen("hud")->find("deg")->shown_text == "270");

    // The fade: to black over a second, half way after half of it, then clear at once.
    const usize before = game_ui.vertices();
    r = run("ui_fade", {{"alpha", 1}, {"seconds", 1}});
    GIANT_CHECK(r.ok && r.value["fade"]["to"] == 1.0 && r.value["fade"]["alpha"] == 0.0);
    game_ui.update(state, 0.5, 1920.0f, 1080.0f);
    GIANT_CHECK(std::fabs(game_ui.fade_alpha() - 0.5f) < 0.01f && game_ui.vertices() == before + 4);
    bool half = false;
    for (const rhi::UiVertex& v : game_ui.list().vertices) half = half || v.color == ui::rgba(0, 0, 0, 128);
    GIANT_CHECK(half);
    game_ui.update(state, 1.0, 1920.0f, 1080.0f);
    GIANT_CHECK(game_ui.fade_alpha() == 1.0f);
    GIANT_CHECK(run("ui_fade", {{"alpha", 0}, {"seconds", 0}}).ok && game_ui.fade_alpha() == 0.0f);
    GIANT_CHECK(!run("ui_fade", {{"alpha", 2}}).ok && !run("ui_fade", {{"alpha", 0.5}, {"color", "#zzzzzz"}}).ok);

    // Playing: the player's values.
    world::Entity floor;
    floor.shape = world::Shape::plane;
    floor.transform.scale = {40.0f, 1.0f, 40.0f};
    state.scene.add(floor);
    GIANT_CHECK(run("play", {{"on", true}}).ok && play.playing());
    game_ui.update(state, 1.0 / 60.0, 1920.0f, 1080.0f);
    GIANT_CHECK(game_ui.values.truthy("playing") && game_ui.values.find("player.speed") != nullptr &&
                game_ui.values.find("player.on_ground") != nullptr && game_ui.values.find("player.climbing") != nullptr);
    GIANT_CHECK(run("play", {{"on", false}}).ok);
    game_ui.update(state, 1.0 / 60.0, 1920.0f, 1080.0f);
    GIANT_CHECK(!game_ui.values.truthy("playing") && game_ui.values.find("player.speed") == nullptr);
    // Removed.
    GIANT_CHECK(run("ui_remove_marker", {{"id", "camp"}}).ok && !run("ui_remove_marker", {{"id", "camp"}}).ok);
    r = run("ui_remove_marker", {{"all", true}});
    GIANT_CHECK(r.ok && r.value["markers"] == 0 && !run("ui_remove_marker").ok);
}
