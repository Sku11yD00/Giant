// Weather (the type is in scene.h): named presets, blending one weather
// into another, checks, and the light it gives the scene: the sun through
// the air (redder and dimmer low in the sky and in haze) and the sky's
// light, greyer and softer as clouds cover it. The renderer draws the sky,
// the clouds, their shadows and the fog from the same values.
#pragma once

#include "giant/core/math.h"
#include "giant/world/scene.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace giant::world {

// clear, fair, cloudy, overcast, rain, storm, snow, blizzard, fog; none
// for another name.
std::optional<Weather> weather_preset(std::string_view name);
std::span<const std::string_view> weather_preset_names();

// `a` at t = 0, `b` at 1, each value in between (the wind's direction
// turns the short way round).
Weather blend(const Weather& a, const Weather& b, float t);

// Why it cannot be used (a value out of range), or none.
std::optional<std::string> check_weather(const Weather& w);

// Where the wind blows the clouds, metres a second (x east, z south).
Vec3 wind_velocity(const Weather& w);

// The light the sun and sky give under a weather.
struct WeatherLight {
    Vec3 sun_light;    // the sun's colour times intensity, through the air
    Vec3 sky_light;    // ambient from above
    Vec3 ground_light; // ambient from below
};
WeatherLight weather_light(const Lighting& lighting, const Weather& weather);

// What falls: rain and snow, each 0 to 1 (together the precipitation),
// split by temperature (all snow at -1 degrees and below, all rain at 2
// and above, sleet between).
struct Falling {
    float rain = 0.0f;
    float snow = 0.0f;
};
Falling falling(const Weather& w);

// The ground under the weather: how wet it is and how much snow lies, 0 to 1.
struct Ground {
    float wetness = 0.0f;
    float snow_cover = 0.0f;
    bool operator==(const Ground&) const = default;
};
// `seconds` later (any step size gives about the same result): rain wets
// it in about 5 minutes and it dries in about 40 (faster when warm and
// windy, slower under cloud); snow covers it in about 20 minutes and melts
// above 0 degrees (in about 10 minutes at 10), wetting it as it goes.
Ground weather_ground(Ground ground, const Weather& w, double seconds);

// The fraction of sunlight that crosses the clear air at this elevation
// (degrees), per colour (red passes most), with `haze` 0 to 10.
Vec3 air_transmittance(float sun_elevation_degrees, float haze);

} // namespace giant::world
