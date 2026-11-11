// The clock in the editor: set and got by command (undoable, saved with
// the scene), moved on by play, and the light and the night sky it gives
// the renderer.
#include "../src/clock_state.h"
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"
#include "../src/weather_state.h"

#include "giant_test.h"

#include "giant/world/clock.h"
#include "giant/world/weather.h"

#include <cmath>

using namespace giant;
using namespace giant::editor;
using ai::Json;

namespace {

struct Fixture {
    ai::CommandRegistry commands;
    MeshLibrary meshes{nullptr};
    ClockState clock;
    EditorState state;
    Fixture() {
        state.meshes = &meshes;
        state.clock = &clock;
        register_scene_commands(commands, state);
        register_clock_commands(commands, state);
    }
    ai::CommandResult run(const char* name, const Json& args = Json::object()) {
        return commands.execute(name, args, {});
    }
};

} // namespace

GIANT_TEST("clock commands: set, get, refused out of range, undoable, saved with the scene") {
    Fixture f;
    GIANT_CHECK(!f.state.scene.clock.on);
    ai::CommandResult r = f.run("get_time");
    GIANT_CHECK(r.ok && r.value["light"] == "none" && r.value["clock"]["on"] == false);
    r = f.run("set_time", {{"on", true}, {"hours", 6.5}, {"day", 80}, {"latitude", 50}});
    GIANT_CHECK(r.ok);
    GIANT_CHECK(f.state.scene.clock.on && f.state.scene.clock.hours == 6.5 && f.state.scene.clock.day == 80 &&
                f.state.scene.clock.latitude == 50.0f);
    GIANT_CHECK(!f.run("set_time", {{"hours", 25}}).ok);
    GIANT_CHECK(!f.run("set_time", {{"day", 0}}).ok);
    GIANT_CHECK(!f.run("set_time", {{"latitude", -91}}).ok);
    GIANT_CHECK(!f.run("set_time", {{"time", 3}}).ok);
    GIANT_CHECK(f.state.scene.clock.hours == 6.5);
    // 24 is midnight: the start of the next day.
    GIANT_CHECK(f.run("set_time", {{"hours", 24}}).ok && f.state.scene.clock.hours == 0.0 &&
                f.state.scene.clock.day == 81);
    GIANT_CHECK(f.run("undo").ok && f.state.scene.clock.hours == 6.5);
    GIANT_CHECK(f.run("undo").ok && !f.state.scene.clock.on);
    GIANT_CHECK(f.run("redo").ok && f.state.scene.clock.on && f.state.scene.clock.hours == 6.5);
    // Noon at the equinox: the sun due south, 90 - latitude up.
    GIANT_CHECK(f.run("set_time", {{"hours", 12}}).ok);
    f.clock.update(f.state.scene.clock, 0.0, false);
    r = f.run("get_time");
    GIANT_CHECK(r.ok && r.value["light"] == "sun");
    GIANT_CHECK(std::fabs(r.value["sun"]["elevation_degrees"].get<float>() - 40.0f) < 1.0f);
    GIANT_CHECK(std::fabs(r.value["sun"]["azimuth_degrees"].get<float>() - 180.0f) < 1.0f);
    GIANT_CHECK(r.value["daylight"].get<float>() > 0.99f && r.value["stars"].get<float>() == 0.0f);
    // Midnight: the moon is the light.
    GIANT_CHECK(f.run("set_time", {{"hours", 0}}).ok);
    f.clock.update(f.state.scene.clock, 0.0, false);
    r = f.run("get_time");
    GIANT_CHECK(r.ok && r.value["light"] == "moon" && r.value["stars"].get<float>() >= 1.0f);
    GIANT_CHECK(r.value["daylight"].get<float>() < 0.03f && r.value["daylight"].get<float>() > 0.0f);
    // Saved and loaded whole (hours not through float).
    GIANT_CHECK(f.run("set_time", {{"hours", 6.3}}).ok);
    world::Scene loaded;
    GIANT_CHECK(!world::load_scene(world::save_scene(f.state.scene), loaded).has_value());
    GIANT_CHECK(loaded.clock == f.state.scene.clock);
    // Play moved the time on: set_time to the scene's own time shows it again.
    f.clock.update(f.state.scene.clock, 0.0, true);
    f.clock.update(f.state.scene.clock, 60.0, true); // an hour at speed 60
    GIANT_CHECK(f.clock.shown().hours > 7.0);
    GIANT_CHECK(f.run("set_time", {{"hours", 6.3}}).ok);
    GIANT_CHECK(f.clock.shown().hours == 6.3 && f.run("get_time").value["shown"]["hours"] == 6.3);
    f.clock.update(f.state.scene.clock, 0.0, false);
    GIANT_CHECK(f.clock.shown().hours == 6.3);
}

GIANT_TEST("clock state: play moves the time on by its speed, the scene's clock shows again when it changes") {
    world::Clock c;
    c.on = true;
    c.hours = 23.5;
    c.day = 365;
    c.speed = 60.0f;
    ClockState s;
    s.update(c, 0.0, true);
    GIANT_CHECK(s.shown() == c);
    s.update(c, 10.0, false); // not playing: still
    GIANT_CHECK(s.shown().hours == 23.5);
    s.update(c, 60.0, true); // an hour of game time: past midnight into day 1
    GIANT_CHECK(std::fabs(s.shown().hours - 0.5) < 1e-9 && s.shown().day == 1);
    // A loaded game's time stays until the scene's clock changes.
    s.set_time(8.0, 100);
    s.update(c, 0.0, true);
    GIANT_CHECK(s.shown().hours == 8.0 && s.shown().day == 100);
    s.set_time(30.0, 100); // refused
    GIANT_CHECK(s.shown().hours == 8.0);
    c.speed = 0.0f;
    s.update(c, 60.0, true);
    GIANT_CHECK(s.shown() == c); // changed: shown again, and still at speed 0
    // Off: the time does not move.
    c.on = false;
    c.speed = 60.0f;
    s.update(c, 60.0, true);
    GIANT_CHECK(s.shown() == c);
    // A new scene: its clock again.
    c.on = true;
    s.update(c, 60.0, true, 1);
    s.update(c, 0.0, true, 2);
    GIANT_CHECK(s.shown() == c);
}

GIANT_TEST("clock: the light it places and the night sky it gives the renderer") {
    world::Lighting lighting;
    world::Clock c;
    // Off: as set_lighting has it.
    ClockLight off = clock_light(lighting, c);
    GIANT_CHECK(!off.daylight && off.lighting.sun_elevation_degrees == lighting.sun_elevation_degrees);
    render::Sky sky;
    clock_sky(sky, c);
    GIANT_CHECK(sky.stars == 0.0f && !sky.moon);
    // A full moon at midnight (day 15: about half a month on).
    c.on = true;
    c.day = 15;
    c.hours = 0.0;
    c.latitude = 40.0f;
    const ClockLight night = clock_light(lighting, c);
    GIANT_CHECK(night.daylight && *night.daylight < 0.05f);
    GIANT_CHECK(night.lighting.sun_elevation_degrees > 20.0f); // the moon, high
    GIANT_CHECK(night.lighting.sun_intensity < lighting.sun_intensity * world::k_moon_light * 1.01f &&
                night.lighting.sun_intensity > lighting.sun_intensity * world::k_moon_light * 0.9f);
    clock_sky(sky, c);
    GIANT_CHECK(sky.moon && sky.moon_lit > 0.95f);
    GIANT_CHECK(sky.stars > 4.0f && sky.stars <= 5.0f); // (brighter under a full moon up)
    // The sky's light falls at night, but not to black (no weather: the flat colours).
    const render::Lighting day_light = weather_lighting(lighting, std::nullopt);
    const render::Lighting night_light = weather_lighting(night.lighting, std::nullopt, night.daylight);
    GIANT_CHECK(night_light.sky_light.y < day_light.sky_light.y * 0.05f && night_light.sky_light.y > 0.0f);
    GIANT_CHECK(night_light.background.y < day_light.background.y * 0.05f);
    // Under weather likewise.
    const world::Weather w = *world::weather_preset("clear");
    const render::Lighting night_weather = weather_lighting(night.lighting, w, night.daylight);
    const render::Lighting day_weather = weather_lighting(lighting, w);
    GIANT_CHECK(night_weather.sky_light.y < day_weather.sky_light.y * 0.05f && night_weather.sky_light.y > 0.0f);
    // Dusk: the sun's light fades out by 2 degrees under, and the moon's
    // starts from nothing at 6 under: no jump in the light either side.
    c.day = 80;
    float last = -1.0f, last_flat = -1.0f;
    for (double h = 17.5; h < 20.5; h += 1.0 / 240.0) {
        c.hours = h;
        const world::SkyBodies b = world::sky_bodies(c);
        const ClockLight l = clock_light(lighting, c);
        const render::Lighting r = weather_lighting(l.lighting, w, l.daylight);
        if (b.sun_elevation_degrees <= -2.0f && b.sun_elevation_degrees > -6.0f) GIANT_CHECK(r.sun_light.y == 0.0f);
        if (last >= 0.0f) GIANT_CHECK(std::fabs(r.sun_light.y - last) < 0.01f);
        last = r.sun_light.y;
        // Without weather (the sun's full colour) likewise.
        const render::Lighting flat = weather_lighting(l.lighting, std::nullopt, l.daylight);
        if (last_flat >= 0.0f) GIANT_CHECK(std::fabs(flat.sun_light.y - last_flat) < 0.1f); // (a 15 s step of the ramp)
        last_flat = flat.sun_light.y;
    }
}
