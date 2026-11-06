// Weather commands: the scene's weather (world/weather.h): clouds, wind,
// fog and haze over an atmosphere the sun shines through. Scene data
// (saved, undoable); a change can blend in over seconds (view only: the
// scene holds where it is going).
#include "editor_commands.h"
#include "weather_state.h"

#include "giant/core/assert.h"
#include "giant/world/weather.h"

#include <string>

namespace giant::editor {

namespace {

using ai::CommandContext;
using ai::CommandResult;
using ai::Json;
using world::Weather;

Json number(double minimum, double maximum, const char* description) {
    return {{"type", "number"}, {"minimum", minimum}, {"maximum", maximum}, {"description", description}};
}

void add(ai::CommandRegistry& registry, ai::CommandDesc desc) {
    const bool added = registry.add(std::move(desc));
    GIANT_ASSERT(added, "weather command could not be registered");
}

Json weather_json(const Weather& w) {
    return {{"cloud_cover", w.cloud_cover},
            {"cloud_density", w.cloud_density},
            {"cloud_base", w.cloud_base},
            {"cloud_thickness", w.cloud_thickness},
            {"cloud_scale", w.cloud_scale},
            {"wind_direction_degrees", w.wind_direction_degrees},
            {"wind_speed", w.wind_speed},
            {"fog_density", w.fog_density},
            {"fog_height", w.fog_height},
            {"haze", w.haze},
            {"precipitation", w.precipitation},
            {"temperature", w.temperature},
            {"wetness", w.wetness},
            {"snow_cover", w.snow_cover}};
}

void record(EditorState& state, CommandContext& context, std::optional<Weather> before, std::optional<Weather> after) {
    context.record(scene_change(
        state, [&state, before] { state.scene.weather = before; }, [&state, after] { state.scene.weather = after; }));
}

} // namespace

void register_weather_commands(ai::CommandRegistry& registry, EditorState& state) {
    Json presets = Json::array();
    for (const std::string_view name : world::weather_preset_names()) presets.push_back(std::string(name));
    add(registry,
        {"set_weather",
         "Sets the scene's weather: an atmosphere the sun shines through (redder low in the sky and in "
         "haze), a layer of clouds that drifts on the wind and shades the ground, height fog, and rain or "
         "snow (by temperature) that wets the ground (darker, glossier, puddles) or covers what faces up, "
         "except where something above shelters it; the ground then wets, dries, snows over and thaws as "
         "time passes (wetness and snow_cover put it there; a preset's only start a new scene). Starts "
         "from `preset` if given, else the weather now (or the defaults), then sets the fields given. "
         "over_seconds blends the view from what shows now to the new weather (default 0: at once, and at "
         "once from no weather; the scene saves the new weather either way). A scene without weather keeps the flat sky colours of "
         "set_lighting. Undoable.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"properties",
               {{"preset", {{"enum", presets}, {"description", "A weather to start from."}}},
                {"cloud_cover", number(0, 1, "0 clear to 1 overcast.")},
                {"cloud_density", number(0, 1, "How thick and dark the clouds are.")},
                {"cloud_base", number(100, 10000, "Metres above 0.")},
                {"cloud_thickness", number(100, 10000, "Metres from base to top.")},
                {"cloud_scale", number(0.25, 4, "The clouds' size (1: about 3 km across).")},
                {"wind_direction_degrees", number(-360, 360, "Where the wind blows to (0 north, 90 east).")},
                {"wind_speed", number(0, 200, "Metres a second.")},
                {"fog_density", number(0, 1, "Ground fog: at 1 things about 150 m away are hidden.")},
                {"fog_height", number(1, 10000, "Metres: the fog thins by half each this much higher.")},
                {"haze", number(0, 10, "Dust and moisture in the air (paler sky, redder sun). Default 1.")},
                {"precipitation", number(0, 1, "How hard it rains or snows.")},
                {"temperature", number(-50, 50, "Degrees Celsius: snow at -1 and below, rain at 2 and above, "
                                                "sleet between; warm melts lying snow. Default 10.")},
                {"wetness", number(0, 1, "How wet the ground is, now and as the scene starts (then it wets and dries "
                                         "with the weather).")},
                {"snow_cover", number(0, 1, "How much snow lies, now and as the scene starts (then it falls and "
                                            "melts with the weather).")},
                {"over_seconds", number(0, 3600, "Blend the view into it over this long. Default 0.")}}}},
         false,
         [&state](const Json& args, CommandContext& context) {
             Weather w = state.scene.weather.value_or(Weather{});
             if (args.contains("preset")) {
                 const std::optional<Weather> p = world::weather_preset(args["preset"].get<std::string>());
                 if (!p) return CommandResult::failure("no such preset");
                 w = *p;
             }
             const auto field = [&args](const char* key, float& out) { out = args.value(key, out); };
             field("cloud_cover", w.cloud_cover);
             field("cloud_density", w.cloud_density);
             field("cloud_base", w.cloud_base);
             field("cloud_thickness", w.cloud_thickness);
             field("cloud_scale", w.cloud_scale);
             field("wind_direction_degrees", w.wind_direction_degrees);
             field("wind_speed", w.wind_speed);
             field("fog_density", w.fog_density);
             field("fog_height", w.fog_height);
             field("haze", w.haze);
             field("precipitation", w.precipitation);
             field("temperature", w.temperature);
             field("wetness", w.wetness);
             field("snow_cover", w.snow_cover);
             if (const auto bad = world::check_weather(w)) return CommandResult::failure(*bad);
             const std::optional<Weather> before = state.scene.weather;
             state.scene.weather = w;
             if (state.weather != nullptr) {
                 state.weather->transition(args.value("over_seconds", 0.0));
                 // The ground's start values given: the ground is there now
                 // (a preset's alone only say how a new scene starts).
                 // Only the values given: the other stays where the
                 // weather has taken it.
                 if (args.contains("wetness") || args.contains("snow_cover")) {
                     world::Ground g = state.weather->ground();
                     if (args.contains("wetness")) g.wetness = w.wetness;
                     if (args.contains("snow_cover")) g.snow_cover = w.snow_cover;
                     state.weather->set_ground(g);
                 }
             }
             record(state, context, before, w);
             return CommandResult::success({{"weather", weather_json(w)}});
         }});
    add(registry,
        {"clear_weather",
         "Removes the scene's weather: the flat sky colours of set_lighting again, no clouds or fog. Undoable.",
         Json{{"type", "object"}, {"additionalProperties", false}, {"properties", Json::object()}},
         false,
         [&state](const Json&, CommandContext& context) {
             if (!state.scene.weather) return CommandResult::failure("the scene has no weather");
             const std::optional<Weather> before = state.scene.weather;
             state.scene.weather.reset();
             record(state, context, before, std::nullopt);
             return CommandResult::success({{"weather", nullptr}});
         }});
    add(registry,
        {"get_weather",
         "The scene's weather (null: none, the flat sky), what the view shows now (mid-blend it differs), "
         "how far a blend has come (1: done), what is falling (rain, snow), the ground now (wetness, "
         "snow_cover), the presets, and what the last frame drew (sky: the sky, clouds, their shadows, "
         "falling rain or snow, wet or snowy surfaces, the cover map).",
         Json{{"type", "object"}, {"additionalProperties", false}, {"properties", Json::object()}},
         true,
         [&state](const Json&, CommandContext&) {
             Json out;
             out["weather"] = state.scene.weather ? weather_json(*state.scene.weather) : Json(nullptr);
             Json presets_out = Json::array();
             for (const std::string_view name : world::weather_preset_names()) presets_out.push_back(std::string(name));
             out["presets"] = std::move(presets_out);
             const std::optional<Weather>& now =
                 state.weather != nullptr && state.weather->shown() ? state.weather->shown() : state.scene.weather;
             if (now) {
                 const world::Falling f = world::falling(*now);
                 out["falling"] = {{"rain", f.rain}, {"snow", f.snow}};
             }
             if (state.weather != nullptr) {
                 out["shown"] = state.weather->shown() ? weather_json(*state.weather->shown()) : Json(nullptr);
                 out["progress"] = state.weather->progress();
                 const DVec3 o = state.weather->cloud_offset();
                 out["cloud_offset"] = {o.x, o.z};
                 const world::Ground g = state.weather->ground();
                 out["ground"] = {{"wetness", g.wetness}, {"snow_cover", g.snow_cover}};
             }
             const render::RenderStats& s = state.scene_stats;
             out["sky"] = {{"on", s.sky},
                           {"clouds", s.clouds},
                           {"cloud_shadows", s.cloud_shadows},
                           {"falling", s.falling},
                           {"wet_surfaces", s.wet_surfaces},
                           {"weather_cover", s.weather_cover}};
             return CommandResult::success(std::move(out));
         }});
}

} // namespace giant::editor
