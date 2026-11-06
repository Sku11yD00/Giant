// Weather: presets, blending, checks, saving, and the light it gives.
#include "giant/world/weather.h"

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
