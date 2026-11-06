#include "giant/world/weather.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace giant::world {

namespace {

constexpr std::array<std::string_view, 6> k_presets = {"clear", "fair", "cloudy", "overcast", "storm", "fog"};

bool in(float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; }

float lerp(float a, float b, float t) { return a + (b - a) * t; }

float luminance(const Vec3& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

} // namespace

std::span<const std::string_view> weather_preset_names() { return k_presets; }

std::optional<Weather> weather_preset(std::string_view name) {
    Weather w;
    if (name == "clear") {
        w.cloud_cover = 0.0f;
        w.haze = 0.6f;
        w.wind_speed = 4.0f;
    } else if (name == "fair") {
        w.cloud_cover = 0.3f;
        w.cloud_density = 0.45f;
        w.cloud_thickness = 1200.0f;
    } else if (name == "cloudy") {
        w.cloud_cover = 0.6f;
        w.cloud_density = 0.6f;
        w.cloud_thickness = 2000.0f;
        w.wind_speed = 15.0f;
        w.haze = 1.5f;
    } else if (name == "overcast") {
        w.cloud_cover = 0.95f;
        w.cloud_density = 0.7f;
        w.cloud_base = 900.0f;
        w.cloud_thickness = 1500.0f;
        w.cloud_scale = 1.5f;
        w.haze = 2.5f;
        w.fog_density = 0.02f;
    } else if (name == "storm") {
        w.cloud_cover = 1.0f;
        w.cloud_density = 1.0f;
        w.cloud_base = 700.0f;
        w.cloud_thickness = 7000.0f;
        w.cloud_scale = 2.0f;
        w.wind_speed = 30.0f;
        w.haze = 3.0f;
        w.fog_density = 0.05f;
        w.fog_height = 400.0f;
        w.precipitation = 1.0f;
    } else if (name == "fog") {
        w.cloud_cover = 0.5f;
        w.cloud_density = 0.4f;
        w.cloud_base = 600.0f;
        w.wind_speed = 2.0f;
        w.fog_density = 0.6f;
        w.fog_height = 60.0f;
        w.haze = 4.0f;
    } else {
        return std::nullopt;
    }
    return w;
}

Weather blend(const Weather& a, const Weather& b, float t) {
    t = std::clamp(std::isfinite(t) ? t : 1.0f, 0.0f, 1.0f);
    if (t == 0.0f) return a; // exactly the start, and the end below
    Weather w;
    w.cloud_cover = lerp(a.cloud_cover, b.cloud_cover, t);
    w.cloud_density = lerp(a.cloud_density, b.cloud_density, t);
    w.cloud_base = lerp(a.cloud_base, b.cloud_base, t);
    w.cloud_thickness = lerp(a.cloud_thickness, b.cloud_thickness, t);
    w.cloud_scale = lerp(a.cloud_scale, b.cloud_scale, t);
    float turn = std::fmod(b.wind_direction_degrees - a.wind_direction_degrees, 360.0f);
    if (turn > 180.0f) turn -= 360.0f;
    if (turn < -180.0f) turn += 360.0f;
    w.wind_direction_degrees = std::fmod(a.wind_direction_degrees + turn * t, 360.0f);
    w.wind_speed = lerp(a.wind_speed, b.wind_speed, t);
    w.fog_density = lerp(a.fog_density, b.fog_density, t);
    // Heights blend in log space: 50 m to 5000 m passes 500 m halfway.
    w.fog_height = std::exp(lerp(std::log(a.fog_height), std::log(b.fog_height), t));
    w.haze = lerp(a.haze, b.haze, t);
    w.precipitation = lerp(a.precipitation, b.precipitation, t);
    if (t == 1.0f) w = b; // exactly the target at the end
    return w;
}

std::optional<std::string> check_weather(const Weather& w) {
    if (!in(w.cloud_cover, 0, 1) || !in(w.cloud_density, 0, 1)) return "cloud_cover and cloud_density are 0 to 1";
    if (!in(w.cloud_base, 100, 10000) || !in(w.cloud_thickness, 100, 10000)) {
        return "cloud_base and cloud_thickness are 100 to 10000 metres";
    }
    if (!in(w.cloud_scale, 0.25f, 4.0f)) return "cloud_scale is 0.25 to 4";
    if (!in(w.wind_direction_degrees, -360, 360)) return "wind_direction_degrees is -360 to 360";
    if (!in(w.wind_speed, 0, 200)) return "wind_speed is 0 to 200 metres a second";
    if (!in(w.fog_density, 0, 1)) return "fog_density is 0 to 1";
    if (!in(w.fog_height, 1, 10000)) return "fog_height is 1 to 10000 metres";
    if (!in(w.haze, 0, 10)) return "haze is 0 to 10";
    if (!in(w.precipitation, 0, 1)) return "precipitation is 0 to 1";
    return std::nullopt;
}

Vec3 wind_velocity(const Weather& w) {
    const float a = degrees_to_radians(w.wind_direction_degrees);
    return {std::sin(a) * w.wind_speed, 0.0f, -std::cos(a) * w.wind_speed};
}

Vec3 air_transmittance(float sun_elevation_degrees, float haze) {
    // Air mass (Kasten and Young): 1 overhead, about 38 at the horizon.
    const float zenith = std::clamp(90.0f - sun_elevation_degrees, 0.0f, 90.0f);
    const float mass =
        1.0f / (std::cos(degrees_to_radians(zenith)) + 0.50572f * std::pow(96.07995f - zenith, -1.6364f));
    // Optical depths straight up: air (Rayleigh, 8 km scale height) and
    // haze (Mie, 1.2 km), as the renderer's sky uses (sky.slang).
    const Vec3 rayleigh{5.802e-6f * 8000.0f, 13.558e-6f * 8000.0f, 33.1e-6f * 8000.0f};
    const float mie = 4.4e-6f * 1.11f * 1200.0f * std::max(haze, 0.0f);
    return {std::exp(-mass * (rayleigh.x + mie)), std::exp(-mass * (rayleigh.y + mie)),
            std::exp(-mass * (rayleigh.z + mie))};
}

WeatherLight weather_light(const Lighting& l, const Weather& w) {
    WeatherLight out;
    // Below the horizon the sun stops lighting (night); the sky dims with it.
    const float sun = l.sun_elevation_degrees > -2.0f ? l.sun_intensity : 0.0f;
    const Vec3 through = air_transmittance(std::max(l.sun_elevation_degrees, 0.0f), w.haze);
    out.sun_light = Vec3{l.sun_color.x * through.x, l.sun_color.y * through.y, l.sun_color.z * through.z} * sun;
    // Day: the sky's light follows the sun up from dusk.
    const float day = std::clamp((l.sun_elevation_degrees + 6.0f) / 16.0f, 0.05f, 1.0f);
    const Vec3 clear_sky = l.sky_color * (l.ambient_intensity * day);
    // Clouds take the blue out and pass the sun's light on, scattered.
    const float covered = std::clamp(w.cloud_cover, 0.0f, 1.0f) * (0.4f + 0.6f * std::clamp(w.cloud_density, 0.0f, 1.0f));
    const float grey = luminance(clear_sky) * 0.8f + luminance(out.sun_light) * 0.12f * covered;
    out.sky_light = clear_sky * (1.0f - covered) + Vec3{grey, grey, grey} * covered;
    // Haze greys it a little too.
    const float hazed = std::clamp(w.haze / 10.0f, 0.0f, 1.0f) * 0.5f;
    const float sky_grey = luminance(out.sky_light);
    out.sky_light = out.sky_light * (1.0f - hazed) + Vec3{sky_grey, sky_grey, sky_grey} * hazed;
    out.ground_light = l.ground_color * (l.ambient_intensity * day) * (1.0f - 0.5f * covered);
    return out;
}

} // namespace giant::world
