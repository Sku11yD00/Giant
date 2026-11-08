// Weather: presets, blending, checks, saving, and the light it gives.
#include "giant/world/weather.h"
#include "giant/world/wind.h"

#include "giant_test.h"

#include <cmath>
#include <string>

using namespace giant;
using namespace giant::world;

GIANT_TEST("weather: presets are valid and differ, unknown names are none") {
    for (const std::string_view name : weather_preset_names()) {
        const std::optional<Weather> w = weather_preset(name);
        GIANT_CHECK(w.has_value() && !check_weather(*w).has_value());
    }
    GIANT_CHECK(!weather_preset("hail of frogs").has_value());
    GIANT_CHECK(weather_preset("clear")->cloud_cover == 0.0f);
    GIANT_CHECK(weather_preset("overcast")->cloud_cover > 0.9f);
    GIANT_CHECK(weather_preset("storm")->precipitation > 0.0f);
    GIANT_CHECK(weather_preset("fog")->fog_density > weather_preset("clear")->fog_density);
}

GIANT_TEST("weather: blends value by value, the wind turning the short way") {
    Weather a = *weather_preset("clear");
    Weather b = *weather_preset("storm");
    GIANT_CHECK(blend(a, b, 0.0f) == a);
    GIANT_CHECK(blend(a, b, 1.0f) == b);
    GIANT_CHECK(blend(a, b, 7.0f) == b); // clamped
    const Weather half = blend(a, b, 0.5f);
    GIANT_CHECK(std::fabs(half.cloud_cover - 0.5f) < 1e-6f);
    GIANT_CHECK(!check_weather(half).has_value());
    a.wind_direction_degrees = 350.0f;
    b.wind_direction_degrees = 10.0f;
    const float d = blend(a, b, 0.5f).wind_direction_degrees;
    GIANT_CHECK(std::fabs(d) < 1e-3f || std::fabs(d - 360.0f) < 1e-3f); // through north, not south
    a.fog_height = 50.0f;
    b.fog_height = 5000.0f;
    GIANT_CHECK(std::fabs(blend(a, b, 0.5f).fog_height - 500.0f) < 0.5f);
}

GIANT_TEST("weather: checks ranges; wind blows where it says") {
    Weather w;
    GIANT_CHECK(!check_weather(w).has_value());
    w.cloud_cover = 1.5f;
    GIANT_CHECK(check_weather(w).has_value());
    w = Weather{};
    w.fog_height = 0.0f;
    GIANT_CHECK(check_weather(w).has_value());
    w = Weather{};
    w.haze = NAN;
    GIANT_CHECK(check_weather(w).has_value());
    w = Weather{};
    w.wind_direction_degrees = 90.0f; // east
    w.wind_speed = 5.0f;
    const Vec3 v = wind_velocity(w);
    GIANT_CHECK(std::fabs(v.x - 5.0f) < 1e-4f && std::fabs(v.z) < 1e-4f);
    w.wind_direction_degrees = 0.0f; // north: -Z
    GIANT_CHECK(wind_velocity(w).z < -4.99f);
}

GIANT_TEST("weather: the sun reddens low and in haze; clouds grey the sky") {
    const Vec3 high = air_transmittance(80.0f, 1.0f);
    const Vec3 low = air_transmittance(3.0f, 1.0f);
    GIANT_CHECK(high.x > 0.8f && high.x > high.z);           // blue scatters most
    GIANT_CHECK(low.x < high.x && low.z / low.x < high.z / high.x); // redder low down
    GIANT_CHECK(air_transmittance(30.0f, 8.0f).y < air_transmittance(30.0f, 0.5f).y);
    Lighting l;
    Weather clear = *weather_preset("clear");
    Weather overcast = *weather_preset("overcast");
    const WeatherLight a = weather_light(l, clear);
    const WeatherLight b = weather_light(l, overcast);
    // Clear: the sky is blue; overcast: nearly grey.
    GIANT_CHECK(a.sky_light.z > a.sky_light.x * 2.0f);
    GIANT_CHECK(b.sky_light.z < b.sky_light.x * 1.4f);
    GIANT_CHECK(b.ground_light.y < a.ground_light.y);
    // Night: no sun.
    l.sun_elevation_degrees = -20.0f;
    GIANT_CHECK(weather_light(l, clear).sun_light.y == 0.0f);
}

GIANT_TEST("weather: saved with scenes and worlds, strictly") {
    Scene scene;
    GIANT_CHECK(save_scene(scene).find("\"weather\"") == std::string::npos); // none: not written
    scene.weather = *weather_preset("storm");
    scene.weather->wind_direction_degrees = -45.5f;
    Scene loaded;
    GIANT_CHECK(!load_scene(save_scene(scene), loaded).has_value());
    GIANT_CHECK(loaded.weather == scene.weather);
    Scene copy = scene;
    GIANT_CHECK(copy.weather == scene.weather);
    WorldIndex world;
    world.weather = scene.weather;
    WorldIndex back;
    GIANT_CHECK(!load_world_index(save_world_index(world), back).has_value());
    GIANT_CHECK(back.weather == world.weather);
    std::string text = save_scene(scene);
    std::string bad = text;
    bad.replace(bad.find("\"cloud_cover\": 1"), 16, "\"cloud_cover\": 2");
    GIANT_CHECK(load_scene(bad, loaded).has_value());
    std::string unknown = text;
    unknown.replace(unknown.find("\"haze\""), 6, "\"hazy\"");
    GIANT_CHECK(load_scene(unknown, loaded).has_value());
}

GIANT_TEST("weather: rain or snow by temperature; the ground wets, dries, is covered and thaws") {
    Weather w;
    w.precipitation = 0.8f;
    w.temperature = 10.0f;
    GIANT_CHECK(falling(w).rain == 0.8f && falling(w).snow == 0.0f);
    w.temperature = -5.0f;
    GIANT_CHECK(falling(w).rain == 0.0f && falling(w).snow == 0.8f);
    w.temperature = 0.5f; // sleet: some of each, adding up
    const Falling sleet = falling(w);
    GIANT_CHECK(sleet.rain > 0.0f && sleet.snow > 0.0f && std::fabs(sleet.rain + sleet.snow - 0.8f) < 1e-6f);
    GIANT_CHECK(weather_preset("snow")->snow_cover == 1.0f && falling(*weather_preset("snow")).snow > 0.0f);
    GIANT_CHECK(falling(*weather_preset("rain")).rain > 0.0f && falling(*weather_preset("clear")).rain == 0.0f);

    // Rain: wet within minutes; then dry: drier within the hour.
    Weather rain = *weather_preset("rain");
    rain.precipitation = 1.0f;
    Ground g;
    g = weather_ground(g, rain, 600.0);
    GIANT_CHECK(g.wetness > 0.8f && g.snow_cover == 0.0f);
    const Weather sun = *weather_preset("clear");
    const Ground dried = weather_ground(g, sun, 3600.0);
    GIANT_CHECK(dried.wetness < 0.4f * g.wetness);
    // Any step size gives about the same.
    Ground steps = g;
    for (int i = 0; i < 3600; ++i) steps = weather_ground(steps, sun, 1.0);
    GIANT_CHECK(std::fabs(steps.wetness - dried.wetness) < 1e-3f);
    // Snow covers in tens of minutes and stays while cold.
    Weather snow = *weather_preset("snow");
    Ground s = weather_ground({}, snow, 3600.0); // half-strength snow: 40 minutes
    GIANT_CHECK(s.snow_cover > 0.7f && s.wetness == 0.0f);
    snow.precipitation = 0.0f;
    GIANT_CHECK(weather_ground(s, snow, 7200.0).snow_cover == s.snow_cover);
    // Warm: it melts, wetting the ground.
    Weather thaw = sun;
    thaw.temperature = 10.0f;
    const Ground melted = weather_ground(s, thaw, 1200.0);
    GIANT_CHECK(melted.snow_cover < 0.2f * s.snow_cover && melted.wetness > 0.5f);
    // Thawing too: one big step and many small ones agree.
    Ground thaw_steps = s;
    for (int i = 0; i < 3600; ++i) thaw_steps = weather_ground(thaw_steps, thaw, 1.0);
    const Ground thaw_once = weather_ground(s, thaw, 3600.0);
    GIANT_CHECK(std::fabs(thaw_steps.wetness - thaw_once.wetness) < 0.01f &&
                std::fabs(thaw_steps.snow_cover - thaw_once.snow_cover) < 0.01f);
    // A drizzle on a warm day keeps the ground damp, not soaked.
    Weather drizzle = sun;
    drizzle.temperature = 20.0f;
    drizzle.precipitation = 0.01f;
    const Ground damp = weather_ground({0.3f, 0.0f}, drizzle, 6.0 * 3600.0);
    GIANT_CHECK(damp.wetness < 0.3f);
    // Nothing for no time or a bad step.
    GIANT_CHECK(weather_ground(s, thaw, 0.0) == s && weather_ground(s, thaw, std::nan("")) == s);
    // The new fields are checked and saved.
    Weather bad;
    bad.temperature = 80.0f;
    GIANT_CHECK(check_weather(bad).has_value());
    bad = Weather{};
    bad.snow_cover = 1.5f;
    GIANT_CHECK(check_weather(bad).has_value());
    Scene scene;
    scene.weather = *weather_preset("blizzard");
    scene.weather->wetness = 0.25f;
    Scene loaded;
    GIANT_CHECK(!load_scene(save_scene(scene), loaded).has_value() && loaded.weather == scene.weather);
}

GIANT_TEST("weather: the wind near the ground it blows half the clouds' wind, gustier in a storm") {
    const Wind calm = ground_wind(*weather_preset("clear"));
    const Wind storm = ground_wind(*weather_preset("storm"));
    GIANT_CHECK(std::abs(length(calm.velocity) - 0.5f * weather_preset("clear")->wind_speed) < 1e-4f);
    GIANT_CHECK(length(storm.velocity) > 3.0f * length(calm.velocity));
    GIANT_CHECK(storm.gustiness > calm.gustiness && storm.gustiness <= 1.0f);
    // Along the clouds' wind (east here).
    Weather east;
    east.wind_direction_degrees = 90.0f;
    GIANT_CHECK(ground_wind(east).velocity.x > 0.0f && std::abs(ground_wind(east).velocity.z) < 1e-4f);
    Weather broken;
    broken.wind_speed = NAN;
    GIANT_CHECK(ground_wind(broken).velocity == Vec3{});
}
