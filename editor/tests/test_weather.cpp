// Weather in the editor: set, blended over seconds, cleared, undone; the
// clouds drift on the wind; the renderer's sky and light follow.
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"
#include "../src/weather_state.h"

#include "giant_test.h"

#include "giant/world/weather.h"

#include <cmath>

using namespace giant;
using namespace giant::editor;
using ai::Json;

namespace {

struct Fixture {
    ai::CommandRegistry commands;
    MeshLibrary meshes{nullptr};
    WeatherState weather;
    EditorState state;
    Fixture() {
        state.meshes = &meshes;
        state.weather = &weather;
        register_scene_commands(commands, state);
        register_weather_commands(commands, state);
    }
    ai::CommandResult run(const char* name, const Json& args = Json::object()) {
        return commands.execute(name, args, {});
    }
};

} // namespace

GIANT_TEST("weather commands: set from a preset and fields, get, clear, all undoable") {
    Fixture f;
    GIANT_CHECK(!f.state.scene.weather.has_value());
    GIANT_CHECK(f.run("get_weather").ok && f.run("get_weather").value["weather"].is_null());
    ai::CommandResult r = f.run("set_weather", {{"preset", "storm"}, {"wind_speed", 12}});
    GIANT_CHECK(r.ok);
    GIANT_CHECK(f.state.scene.weather && f.state.scene.weather->cloud_cover == 1.0f &&
                f.state.scene.weather->wind_speed == 12.0f);
    // Fields alone change the weather now.
    GIANT_CHECK(f.run("set_weather", {{"fog_density", 0.5}}).ok);
    GIANT_CHECK(f.state.scene.weather->fog_density == 0.5f && f.state.scene.weather->cloud_cover == 1.0f);
    GIANT_CHECK(!f.run("set_weather", {{"preset", "sunny with frogs"}}).ok);
    GIANT_CHECK(!f.run("set_weather", {{"cloud_cover", 3}}).ok);
    GIANT_CHECK(!f.run("set_weather", {{"weather", 1}}).ok);
    GIANT_CHECK(f.state.scene.weather->fog_density == 0.5f);
    r = f.run("get_weather");
    GIANT_CHECK(r.ok && r.value["weather"]["fog_density"].get<float>() == 0.5f && r.value["presets"].size() == 6);
    GIANT_CHECK(f.run("undo").ok && f.state.scene.weather->fog_density == world::weather_preset("storm")->fog_density);
    GIANT_CHECK(f.run("redo").ok && f.state.scene.weather->fog_density == 0.5f);
    GIANT_CHECK(f.run("clear_weather").ok && !f.state.scene.weather);
    GIANT_CHECK(!f.run("clear_weather").ok);
    GIANT_CHECK(f.run("undo").ok && f.state.scene.weather && f.state.scene.weather->fog_density == 0.5f);
    // Saved with the scene.
    world::Scene loaded;
    GIANT_CHECK(!world::load_scene(world::save_scene(f.state.scene), loaded).has_value());
    GIANT_CHECK(loaded.weather == f.state.scene.weather);
}

GIANT_TEST("weather state: a change blends in over its seconds, the clouds drift on the wind") {
    Fixture f;
    GIANT_CHECK(f.run("set_weather", {{"preset", "clear"}}).ok);
    f.weather.update(f.state.scene.weather, 0.1);
    GIANT_CHECK(f.weather.shown() == f.state.scene.weather && f.weather.progress() == 1.0);
    GIANT_CHECK(f.run("set_weather", {{"preset", "overcast"}, {"over_seconds", 10}}).ok);
    f.weather.update(f.state.scene.weather, 0.0);
    GIANT_CHECK(f.weather.shown()->cloud_cover == 0.0f); // just begun
    for (int i = 0; i < 50; ++i) f.weather.update(f.state.scene.weather, 0.1); // 5 s
    GIANT_CHECK(std::fabs(f.weather.shown()->cloud_cover - 0.475f) < 0.01f);
    GIANT_CHECK(std::fabs(f.weather.progress() - 0.5) < 0.01);
    for (int i = 0; i < 60; ++i) f.weather.update(f.state.scene.weather, 0.1);
    GIANT_CHECK(f.weather.shown() == f.state.scene.weather && f.weather.progress() == 1.0);
    // Without over_seconds: at once.
    GIANT_CHECK(f.run("set_weather", {{"preset", "fog"}}).ok);
    f.weather.update(f.state.scene.weather, 0.016);
    GIANT_CHECK(f.weather.shown() == f.state.scene.weather);
    // The same weather again at once ends a blend under way.
    GIANT_CHECK(f.run("set_weather", {{"preset", "overcast"}, {"over_seconds", 100}}).ok);
    f.weather.update(f.state.scene.weather, 0.1);
    GIANT_CHECK(f.weather.progress() < 0.01);
    GIANT_CHECK(f.run("set_weather", {{"preset", "overcast"}}).ok);
    f.weather.update(f.state.scene.weather, 0.016);
    GIANT_CHECK(f.weather.shown() == f.state.scene.weather && f.weather.progress() == 1.0);
    // The wind: east at 10 m/s carries the clouds 10 m a second east.
    GIANT_CHECK(f.run("set_weather", {{"wind_direction_degrees", 90}, {"wind_speed", 10}}).ok);
    f.weather.update(f.state.scene.weather, 0.0);
    const DVec3 before = f.weather.cloud_offset();
    for (int i = 0; i < 10; ++i) f.weather.update(f.state.scene.weather, 0.5);
    const DVec3 moved = f.weather.cloud_offset() - before;
    GIANT_CHECK(std::fabs(moved.x - 50.0) < 1e-3 && std::fabs(moved.z) < 1e-3);
    // A stall of seconds does not jump it.
    f.weather.update(f.state.scene.weather, 30.0);
    GIANT_CHECK(f.weather.cloud_offset().x - before.x < 61.0);
    // Cleared: nothing shown.
    GIANT_CHECK(f.run("clear_weather").ok);
    f.weather.update(f.state.scene.weather, 0.1);
    GIANT_CHECK(!f.weather.shown().has_value());
}

GIANT_TEST("weather: the renderer's sky and light follow it, and stay flat without one") {
    world::Lighting l;
    const render::Sky none = weather_sky(l, std::nullopt, {});
    GIANT_CHECK(!none.on);
    const render::Lighting flat = weather_lighting(l, std::nullopt);
    GIANT_CHECK(flat.sun_light.x == l.sun_color.x * l.sun_intensity);
    world::Weather w = *world::weather_preset("storm");
    const render::Sky sky = weather_sky(l, w, {5.0, 0.0, 7.0});
    GIANT_CHECK(sky.on && sky.cloud_cover == 1.0f && sky.cloud_top == w.cloud_base + w.cloud_thickness);
    GIANT_CHECK(sky.cloud_offset.x == 5.0 && sky.fog_density > 0.0f && sky.fog_falloff > 0.0f);
    const render::Lighting under = weather_lighting(l, w);
    GIANT_CHECK(under.sun_light.z < flat.sun_light.z); // through the air
    GIANT_CHECK(std::fabs(under.sky_light.z - under.sky_light.x) < std::fabs(flat.sky_light.z - flat.sky_light.x));
}
