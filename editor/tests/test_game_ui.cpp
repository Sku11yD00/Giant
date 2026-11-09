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
