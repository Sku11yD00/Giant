// Game UI commands: get_game_ui, set_game_ui, show_toast, and screens:
// ui_load_screen, ui_remove_screen, ui_set_values, ui_click, ui_key,
// get_ui_events, get_ui_screen; HUD pieces: ui_set_marker,
// ui_remove_marker, get_ui_markers, ui_fade (game_ui.h). View state, not
// undoable.
#include "editor_commands.h"
#include "game_ui.h"

#include "giant/core/assert.h"

#include <string>
#include <vector>

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
    Json screens = Json::array();
    for (const std::string& name : g.screen_names()) {
        const ui::Screen* screen = g.screen(name);
        screens.push_back({{"name", name}, {"layer", g.layer(name)}, {"widgets", screen->widgets()}, {"focused", screen->focused()}});
    }
    return {{"show", GameUi::to_string(g.show)},
            {"shown", g.shown()},
            {"scale", g.scale()},
            {"toasts", g.toasts()},
            {"screens", screens},
            {"values", g.values.size()},
            {"images", g.images()},
            {"events", g.events()},
            {"markers", g.markers.size()},
            {"fade", {{"alpha", g.fade_alpha()}, {"to", g.fade_target()}}},
            {"vertices", g.vertices()},
            {"draw_calls", g.commands()},
            {"build_ms", g.build_ms()},
            {"font", font},
            {"error", g.error()}};
}

Json event_json(const std::optional<ui::UiEvent>& e) {
    if (!e) return nullptr;
    return {{"screen", e->screen}, {"id", e->id}};
}

const char* type_name(ui::WidgetType t) {
    switch (t) {
    case ui::WidgetType::panel: return "panel";
    case ui::WidgetType::text: return "text";
    case ui::WidgetType::bar: return "bar";
    case ui::WidgetType::image: return "image";
    case ui::WidgetType::button: return "button";
    case ui::WidgetType::compass: return "compass";
    }
    return "panel";
}

// Every widget as last laid out, in the order written (depth first).
void widgets_json(const ui::Widget& w, usize depth, Json& out) {
    out.push_back({{"id", w.id},
                   {"type", type_name(w.type)},
                   {"depth", depth},
                   {"shown", w.shown},
                   {"rect", {w.rect.x, w.rect.y, w.rect.w, w.rect.h}}});
    for (const ui::Widget& c : w.children) widgets_json(c, depth + 1, out);
}

constexpr const char* k_screen_help =
    "A screen is a tree of widgets: {\"type\": panel|text|bar|image|button, \"id\", \"anchor\": [x,y] (0 to 1 in its "
    "parent), \"pivot\": [x,y] (its own point there; default the anchor), \"offset\": [x,y] units, \"size\": [w,h] "
    "units (0: fit what it holds), \"stretch\": [bool,bool], \"layout\": none|column|row, \"gap\", \"padding\", "
    "\"clip\", \"visible\": \"name\" or \"!name\" (a value), \"background\"/\"border_color\"/\"color\"/\"hover\": "
    "\"#rrggbb[aa]\", \"border\", \"text\": \"Ammo {ammo}\" ({name:2}: two decimals), \"font_size\", \"align\": "
    "left|center|right, \"wrap\", \"outline\", \"outline_color\", \"shadow\", \"value\"/\"max\": a number or a "
    "value's name (a bar), \"direction\": right|left|up|down, \"image\": a name the game gave, \"span\": a compass's degrees "
    "(type compass: its value the heading, e.g. \"heading\"; markers on it), \"fade\": seconds it eases in and "
    "out as visible changes, \"children\": [...]}. The editor sets the values heading, playing, player.speed, "
    "player.on_ground, player.climbing. "
    "Units are pixels at 1080 lines high. Unknown keys are refused; a button needs an id; ids are one widget each.";

} // namespace

void register_game_ui_commands(ai::CommandRegistry& registry, EditorState& state) {
    add(registry,
        {"get_game_ui",
         "The game's own UI (drawn over the scene, under the editor's panels): `show` (playing, always, never), "
         "`shown` this frame, `scale` (window height / 1080), `toasts` showing, `screens` (drawn first to last: "
         "`name`, `layer`, `widgets`, `focused` button), how many `values`, `images` and queued `events`, last "
         "frame's `vertices`, "
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

    add(registry,
        {"ui_load_screen",
         std::string("Loads a screen of the game's UI under `name` (1 to 64 bytes; the same name replaced, its focus "
                     "kept), drawn by `layer` (-1000 to 1000, default 0; higher on top) when the game UI is shown. "
                     "`screen` is the JSON (an object, or text). At most 32 screens and 16384 widgets in all. ") +
             k_screen_help + " Returns get_game_ui, or where the screen is wrong.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", Json::array({"name", "screen"})},
              {"properties",
               {{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}},
                {"screen", {{"type", Json::array({"object", "string"})}}},
                {"layer", {{"type", "integer"}, {"minimum", -1000}, {"maximum", 1000}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.game_ui == nullptr) return no_ui();
             const Json& screen = args["screen"];
             const std::string json = screen.is_string() ? screen.get<std::string>() : screen.dump();
             std::string error;
             if (!state.game_ui->load_screen(args["name"].get<std::string>(), json, args.value("layer", 0), error))
                 return CommandResult::failure(error);
             return CommandResult::success(status_json(*state.game_ui));
         }});

    add(registry,
        {"ui_remove_screen",
         "Removes the game UI's screen `name`, or every screen (`all`: true). Returns get_game_ui.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"properties",
               {{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}}, {"all", {{"type", "boolean"}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.game_ui == nullptr) return no_ui();
             if (args.value("all", false)) {
                 state.game_ui->clear_screens();
             } else if (!args.contains("name")) {
                 return CommandResult::failure("give a `name`, or `all`: true");
             } else if (!state.game_ui->remove_screen(args["name"].get<std::string>())) {
                 return CommandResult::failure("no screen \"" + args["name"].get<std::string>() + "\"");
             }
             return CommandResult::success(status_json(*state.game_ui));
         }});

    add(registry,
        {"ui_set_values",
         "Sets the values the game UI's screens show: `values` an object of name (1 to 128 bytes) to a number, "
         "text (at most 4096 bytes) or true/false (null forgets it); `clear` first forgets them all. At most "
         "4096 values. Returns those given as they are now (`values`) and how many there are (`count`).",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"properties", {{"values", {{"type", "object"}}}, {"clear", {{"type", "boolean"}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.game_ui == nullptr) return no_ui();
             ui::Values& values = state.game_ui->values;
             if (args.contains("values")) {
                 for (const auto& [name, v] : args["values"].items()) {
                     if (name.empty() || name.size() > 128) return CommandResult::failure("a value's name is 1 to 128 bytes");
                     if (!v.is_null() && !v.is_number() && !v.is_string() && !v.is_boolean())
                         return CommandResult::failure("\"" + name + "\" is a number, text, true/false or null");
                     if (v.is_string() && v.get_ref<const std::string&>().size() > 4096)
                         return CommandResult::failure("\"" + name + "\" is at most 4096 bytes");
                 }
                 // The count after: what is kept, plus new names, less those forgotten.
                 const bool clear = args.value("clear", false);
                 usize after = clear ? 0 : values.size();
                 for (const auto& [name, v] : args["values"].items()) {
                     const bool there = !clear && values.find(name) != nullptr;
                     if (v.is_null() && there) --after;
                     if (!v.is_null() && !there) ++after;
                 }
                 if (after > ui::Values::k_max_values)
                     return CommandResult::failure("at most " + std::to_string(ui::Values::k_max_values) + " values");
             }
             if (args.value("clear", false)) values.clear();
             if (args.contains("values")) {
                 // Those forgotten first, so they make room.
                 for (const auto& [name, v] : args["values"].items()) {
                     if (v.is_null()) values.erase(name);
                 }
                 for (const auto& [name, v] : args["values"].items()) {
                     if (v.is_boolean()) {
                         values.set(name, v.get<bool>());
                     } else if (v.is_number()) {
                         values.set(name, v.get<double>());
                     } else if (v.is_string()) {
                         values.set(name, v.get<std::string>());
                     }
                 }
             }
             // Those given, as they are now (null: not set), and how many in all.
             Json out = Json::object();
             if (args.contains("values")) {
                 for (const auto& [name, given] : args["values"].items()) {
                     (void)given;
                     const ui::Value* v = values.find(name);
                     if (v == nullptr) out[name] = nullptr;
                     else if (const double* d = std::get_if<double>(v)) out[name] = *d;
                     else if (const bool* b = std::get_if<bool>(v)) out[name] = *b;
                     else out[name] = std::get<std::string>(*v);
                 }
             }
             return CommandResult::success({{"values", out}, {"count", values.size()}});
         }});

    add(registry,
        {"ui_click",
         "Clicks the game UI: at `x`, `y` (pixels, as last laid out: the top button there, on the top screen "
         "with one; a screen showing a background there takes it from those under it), or the button `id` on `screen` (if shown). Only while the game UI is shown. The click is "
         "queued as an event (get_ui_events) and returned as `event` {screen, id} (null: no button).",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"properties",
               {{"x", {{"type", "number"}, {"minimum", -1e6}, {"maximum", 1e6}}},
                {"y", {{"type", "number"}, {"minimum", -1e6}, {"maximum", 1e6}}},
                {"screen", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}},
                {"id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.game_ui == nullptr) return no_ui();
             GameUi& g = *state.game_ui;
             if (args.contains("x") && args.contains("y")) {
                 if (args.contains("screen") || args.contains("id")) return CommandResult::failure("x and y, or screen and id");
                 if (!g.showing(state)) return CommandResult::success({{"event", nullptr}});
                 return CommandResult::success({{"event", event_json(g.click({args["x"].get<float>(), args["y"].get<float>()}))}});
             }
             if (args.contains("screen") && args.contains("id")) {
                 if (args.contains("x") || args.contains("y")) return CommandResult::failure("x and y, or screen and id");
                 if (g.screen(args["screen"].get<std::string>()) == nullptr)
                     return CommandResult::failure("no screen \"" + args["screen"].get<std::string>() + "\"");
                 if (!g.showing(state)) return CommandResult::success({{"event", nullptr}});
                 return CommandResult::success(
                     {{"event", event_json(g.press(args["screen"].get<std::string>(), args["id"].get<std::string>()))}});
             }
             return CommandResult::failure("x and y, or screen and id");
         }});

    add(registry,
        {"ui_key",
         "Keys on a screen of the game UI: `key` \"next\" or \"previous\" moves its focus through the buttons shown "
         "(in the order written, round the ends; as last laid out), \"activate\" presses the focused one (queued "
         "as an event, returned as `event`; null while the game UI is not shown). Returns `focused` and `event`.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", Json::array({"screen", "key"})},
              {"properties",
               {{"screen", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}},
                {"key", {{"type", "string"}, {"enum", Json::array({"next", "previous", "activate"})}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.game_ui == nullptr) return no_ui();
             GameUi& g = *state.game_ui;
             const std::string name = args["screen"].get<std::string>(), key = args["key"].get<std::string>();
             if (g.screen(name) == nullptr) return CommandResult::failure("no screen \"" + name + "\"");
             std::optional<ui::UiEvent> e;
             if (key == "activate") {
                 if (g.showing(state)) e = g.activate(name);
             } else {
                 g.move_focus(name, key == "next" ? 1 : -1);
             }
             return CommandResult::success({{"focused", g.screen(name)->focused()}, {"event", event_json(e)}});
         }});

    add(registry,
        {"get_ui_events",
         "The game UI's button clicks and activations since last asked, oldest first (`events`: [{screen, id}], "
         "at most 64 kept); taken, so the next call has only newer ones.",
         Json{{"type", "object"}, {"additionalProperties", false}, {"properties", Json::object()}},
         false,
         [&state](const Json&, CommandContext&) {
             if (state.game_ui == nullptr) return no_ui();
             Json events = Json::array();
             for (const ui::UiEvent& e : state.game_ui->take_events()) events.push_back({{"screen", e.screen}, {"id", e.id}});
             return CommandResult::success({{"events", events}});
         }});

    add(registry,
        {"get_ui_screen",
         "One screen of the game UI as last laid out: `widgets` in the order written (`id`, `type`, `depth`, "
         "`shown`, `rect` [x, y, w, h] pixels), its `focused` button and `layer`. Laid out only while the game "
         "UI is shown (set_game_ui show \"always\" to see it in the editor).",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", Json::array({"name"})},
              {"properties", {{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}}}}},
         true,
         [&state](const Json& args, CommandContext&) {
             if (state.game_ui == nullptr) return no_ui();
             const std::string name = args["name"].get<std::string>();
             const ui::Screen* screen = state.game_ui->screen(name);
             if (screen == nullptr) return CommandResult::failure("no screen \"" + name + "\"");
             Json widgets = Json::array();
             widgets_json(screen->root(), 0, widgets);
             return CommandResult::success(
                 {{"name", name}, {"layer", state.game_ui->layer(name)}, {"focused", screen->focused()}, {"widgets", widgets}});
         }});

    add(registry,
        {"ui_set_marker",
         "Puts a marker in the world on the game UI (or changes the one with that `id`, 1 to 64 bytes): at "
         "`position` [x, y, z] (metres), `label` under it (at most 128 bytes), `color` \"#rrggbb[aa]\" (default "
         "amber), `clamp` (default true: off the screen or behind, drawn at the edge with an arrow the way), "
         "`compass` (default true: also on compass widgets), `distance` (default true: \"123 m\" under it), "
         "`max_distance` metres (default 0: always; further not shown, fading over the last tenth). At most 256. "
         "Drawn under the screens. Returns get_game_ui.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", Json::array({"id", "position"})},
              {"properties",
               {{"id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}},
                {"position", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}, {"minimum", -1e9}, {"maximum", 1e9}}}}},
                {"label", {{"type", "string"}, {"maxLength", 128}}},
                {"color", {{"type", "string"}, {"minLength", 7}, {"maxLength", 9}}},
                {"clamp", {{"type", "boolean"}}},
                {"compass", {{"type", "boolean"}}},
                {"distance", {{"type", "boolean"}}},
                {"max_distance", {{"type", "number"}, {"minimum", 0}, {"maximum", 1e7}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.game_ui == nullptr) return no_ui();
             ui::Marker m;
             m.id = args["id"].get<std::string>();
             const Json& p = args["position"];
             m.position = {p[0].get<double>(), p[1].get<double>(), p[2].get<double>()};
             m.label = args.value("label", std::string());
             if (args.contains("color")) {
                 const std::optional<u32> c = ui::parse_color(args["color"].get<std::string>());
                 if (!c) return CommandResult::failure("color is \"#rrggbb\" or \"#rrggbbaa\"");
                 m.color = *c;
             }
             m.clamp = args.value("clamp", true);
             m.compass = args.value("compass", true);
             m.distance = args.value("distance", true);
             m.max_distance = args.value("max_distance", 0.0);
             if (!state.game_ui->markers.set(std::move(m)))
                 return CommandResult::failure("at most " + std::to_string(ui::Markers::k_max) + " markers");
             return CommandResult::success(status_json(*state.game_ui));
         }});

    add(registry,
        {"ui_remove_marker",
         "Removes the game UI's marker `id`, or every marker (`all`: true). Returns get_game_ui.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"properties", {{"id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}}, {"all", {{"type", "boolean"}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.game_ui == nullptr) return no_ui();
             if (args.value("all", false)) {
                 state.game_ui->markers.clear();
             } else if (!args.contains("id")) {
                 return CommandResult::failure("give an `id`, or `all`: true");
             } else if (!state.game_ui->markers.erase(args["id"].get<std::string>())) {
                 return CommandResult::failure("no marker \"" + args["id"].get<std::string>() + "\"");
             }
             return CommandResult::success(status_json(*state.game_ui));
         }});

    add(registry,
        {"get_ui_markers",
         "The game UI's markers as last drawn: `id`, `position`, `label`, `shown`, `on_screen` (at its place; "
         "else at the edge, or not shown), `at` [x, y] pixels, `distance` metres, `bearing` degrees clockwise "
         "from north. Drawn only while the game UI is shown.",
         Json{{"type", "object"}, {"additionalProperties", false}, {"properties", Json::object()}},
         true,
         [&state](const Json&, CommandContext&) {
             if (state.game_ui == nullptr) return no_ui();
             const ui::Markers& markers = state.game_ui->markers;
             Json out = Json::array();
             for (const auto& [id, m] : markers.all()) {
                 Json j = {{"id", id}, {"position", {m.position.x, m.position.y, m.position.z}}, {"label", m.label},
                           {"shown", false}, {"on_screen", false}, {"at", nullptr}, {"distance", nullptr}, {"bearing", nullptr}};
                 for (const ui::PlacedMarker& p : markers.placed()) {
                     if (p.id != id) continue;
                     j["shown"] = p.shown;
                     j["on_screen"] = p.on_screen;
                     if (p.shown) j["at"] = {p.at.x, p.at.y};
                     j["distance"] = p.distance;
                     j["bearing"] = p.bearing;
                 }
                 out.push_back(std::move(j));
             }
             return CommandResult::success({{"markers", out}});
         }});

    add(registry,
        {"ui_fade",
         "Fades the game UI (and the scene under it) to `color` \"#rrggbb\" (default black) at `alpha` (0 clear, "
         "1 solid) over `seconds` (0 to 30, default 1; 0 at once): to black before a load, back to clear after. "
         "Over the screens and toasts, under the saving mark. Returns get_game_ui.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", Json::array({"alpha"})},
              {"properties",
               {{"alpha", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
                {"color", {{"type", "string"}, {"minLength", 7}, {"maxLength", 9}}},
                {"seconds", {{"type", "number"}, {"minimum", 0}, {"maximum", 30}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.game_ui == nullptr) return no_ui();
             u32 color = 0;
             if (args.contains("color")) {
                 const std::optional<u32> c = ui::parse_color(args["color"].get<std::string>());
                 if (!c) return CommandResult::failure("color is \"#rrggbb\"");
                 color = *c;
             }
             state.game_ui->fade_to(color, args["alpha"].get<float>(), args.value("seconds", 1.0));
             return CommandResult::success(status_json(*state.game_ui));
         }});
}

} // namespace giant::editor
