// Game UI commands: get_game_ui, set_game_ui, show_toast (game_ui.h). View
// state, not undoable.
#include "editor_commands.h"
#include "game_ui.h"

#include "giant/core/assert.h"

#include <string>

namespace giant::editor {

namespace {

using ai::CommandContext;
using ai::CommandResult;
using ai::Json;

void add(ai::CommandRegistry& registry, ai::CommandDesc desc) {
    const bool added = registry.add(std::move(desc));
    GIANT_ASSERT(added, "game UI command could not be registered");
}

CommandResult no_ui() { return CommandResult::failure("the game UI is not available"); }

Json status_json(const GameUi& g) {
    Json font = nullptr;
    if (const ui::Font* f = g.font()) {
        font = {{"glyphs", f->glyph_count()},
                {"atlas", {f->atlas_width(), f->atlas_height()}},
                {"pixel_size", f->pixel_size()},
                {"on_gpu", f->texture != 0}};
    }
    return {{"show", GameUi::to_string(g.show)},
            {"shown", g.shown()},
            {"scale", g.scale()},
            {"toasts", g.toasts()},
            {"vertices", g.vertices()},
            {"draw_calls", g.commands()},
            {"build_ms", g.build_ms()},
            {"font", font},
            {"error", g.error()}};
}

} // namespace

void register_game_ui_commands(ai::CommandRegistry& registry, EditorState& state) {
    add(registry,
        {"get_game_ui",
         "The game's own UI (drawn over the scene, under the editor's panels): `show` (playing, always, never), "
         "`shown` this frame, `scale` (window height / 1080), `toasts` showing, last frame's `vertices`, "
         "`draw_calls` and `build_ms` (CPU), and its `font` (`glyphs`, `atlas` [width, height], `pixel_size` "
         "the glyphs were made at, `on_gpu`) or `error`.",
         Json{{"type", "object"}, {"additionalProperties", false}, {"properties", Json::object()}},
         true,
         [&state](const Json&, CommandContext&) {
             if (state.game_ui == nullptr) return no_ui();
             return CommandResult::success(status_json(*state.game_ui));
         }});

    add(registry,
        {"set_game_ui",
         "When the game's UI shows: `show` \"playing\" (the default: while playing), \"always\" (in the editor "
         "too: to look at it in a screenshot) or \"never\". View state. Returns get_game_ui.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", Json::array({"show"})},
              {"properties", {{"show", {{"type", "string"}, {"enum", Json::array({"playing", "always", "never"})}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.game_ui == nullptr) return no_ui();
             const std::string show = args["show"].get<std::string>();
             state.game_ui->show = show == "always"  ? GameUi::Show::always
                                   : show == "never" ? GameUi::Show::never
                                                     : GameUi::Show::playing;
             return CommandResult::success(status_json(*state.game_ui));
         }});

    add(registry,
        {"show_toast",
         "Shows `text` (UTF-8, at most 256 bytes; Latin letters and common symbols, others as '?') at the "
         "bottom of the game's UI for `seconds` (default 3, 0.1 to 30; the last half second fading), newest "
         "lowest, at most 4 at once. Shown when the game UI is (set_game_ui). View state. Returns get_game_ui.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", Json::array({"text"})},
              {"properties",
               {{"text", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
                {"seconds", {{"type", "number"}, {"minimum", 0.1}, {"maximum", 30}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.game_ui == nullptr) return no_ui();
             const std::string text = args["text"].get<std::string>();
             if (text.size() > GameUi::k_max_toast_bytes) return CommandResult::failure("at most 256 bytes");
             state.game_ui->toast(text, args.value("seconds", 3.0));
             return CommandResult::success(status_json(*state.game_ui));
         }});
}

} // namespace giant::editor
