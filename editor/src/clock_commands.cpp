// Clock commands: the scene's time of day (world/clock.h), which places
// the sun, the moon and the stars. Scene data (saved, undoable); play
// moves the time shown on (ClockState).
#include "clock_state.h"
#include "editor_commands.h"

#include "giant/core/assert.h"
#include "giant/world/clock.h"

namespace giant::editor {

namespace {

using ai::CommandContext;
using ai::CommandResult;
using ai::Json;
using world::Clock;

Json number(double minimum, double maximum, const char* description) {
    return {{"type", "number"}, {"minimum", minimum}, {"maximum", maximum}, {"description", description}};
}

void add(ai::CommandRegistry& registry, ai::CommandDesc desc) {
    const bool added = registry.add(std::move(desc));
    GIANT_ASSERT(added, "clock command could not be registered");
}

Json clock_json(const Clock& c) {
    return {{"on", c.on},           {"hours", c.hours}, {"day", c.day}, {"latitude", c.latitude},
            {"north_degrees", c.north_degrees}, {"speed", c.speed}};
}

Json vec(const Vec3& v) { return Json::array({v.x, v.y, v.z}); }

// The clock shown: the scene's at the time play reached (the scene's
// itself until the view has followed it).
const Clock& shown(const EditorState& state) {
    return state.clock != nullptr && state.clock->follows(state.scene.clock) ? state.clock->shown() : state.scene.clock;
}

} // namespace

void register_clock_commands(ai::CommandRegistry& registry, EditorState& state) {
    add(registry,
        {"set_time",
         "Sets the scene's clock: the time of day and the date place the sun (by latitude: a simple model "
         "good to about a degree, local solar time), and at night the moon (lit by its phase, new on day 1 "
         "and twelve times a year, every 30.4 days) is the light that casts shadows, the sky's light falling to what the moon "
         "and stars give, and the stars turn overhead (drawn with the weather's sky; clouds hide them). "
         "While on, the clock places the light instead of set_lighting's sun angles (its colour and "
         "intensity stay). Play moves the time on, `speed` game seconds a second. Starts from the time "
         "shown now (as play left it) and sets the fields given. Undoable.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"properties",
               {{"on", {{"type", "boolean"}, {"description", "Whether the clock places the light (default off)."}}},
                {"hours", number(0, 24, "Local solar time, 12 noon.")},
                {"day", {{"type", "integer"}, {"minimum", 1}, {"maximum", 365}, {"description", "Day of the year (80: spring equinox, 172: midsummer in the north)."}}},
                {"latitude", number(-90, 90, "Degrees north (negative: south).")},
                {"north_degrees", number(0, 360, "The compass heading the world's -Z points to (0: -Z is north).")},
                {"speed", number(0, 86400, "Game seconds a real second in play (0: still). Default 60.")}}}},
         false,
         [&state](const Json& args, CommandContext& context) {
             Clock c = shown(state);
             c.on = args.value("on", c.on);
             c.hours = args.value("hours", c.hours);
             c.day = args.value("day", c.day);
             c.latitude = args.value("latitude", c.latitude);
             c.north_degrees = args.value("north_degrees", c.north_degrees);
             c.speed = args.value("speed", c.speed);
             if (const auto bad = world::check_clock(c)) return CommandResult::failure(*bad);
             c = world::midnight_wrapped(c); // 24: the next day's start
             const Clock before = state.scene.clock;
             // The view shows it at once, even where the scene's clock is
             // unchanged (play or a loaded game had moved the time on).
             const auto put = [&state](const Clock& to) {
                 state.scene.clock = to;
                 if (state.clock != nullptr) state.clock->show(to);
             };
             put(c);
             context.record(scene_change(state, [put, before] { put(before); }, [put, c] { put(c); }));
             return CommandResult::success({{"clock", clock_json(c)}});
         }});
    add(registry,
        {"get_time",
         "The scene's clock (as saved), the clock shown now (play moves its time on), where the sun and "
         "the moon are (elevation and compass azimuth in degrees, direction in the world), the moon's "
         "phase (age 0 new, 0.5 full; lit: the share of its disc lit), which is the light (sun, moon, or "
         "none: clock off), the sky's light against the day's (daylight) and how bright the stars are drawn.",
         Json{{"type", "object"}, {"additionalProperties", false}, {"properties", Json::object()}},
         true,
         [&state](const Json&, CommandContext&) {
             const Clock& now = shown(state);
             const world::SkyBodies b = world::sky_bodies(now);
             Json out;
             out["clock"] = clock_json(state.scene.clock);
             out["shown"] = clock_json(now);
             out["sun"] = {{"elevation_degrees", b.sun_elevation_degrees},
                           {"azimuth_degrees", b.sun_azimuth_degrees},
                           {"direction", vec(b.sun)}};
             out["moon"] = {{"elevation_degrees", b.moon_elevation_degrees},
                            {"azimuth_degrees", b.moon_azimuth_degrees},
                            {"direction", vec(b.moon)},
                            {"age", b.moon_age},
                            {"lit", b.moon_lit}};
             out["light"] = !now.on ? "none" : world::moon_leads(b) ? "moon" : "sun";
             out["daylight"] = world::ambient_scale(b);
             out["stars"] = world::star_brightness(b);
             return CommandResult::success(std::move(out));
         }});
}

} // namespace giant::editor
