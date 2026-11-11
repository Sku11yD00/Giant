#include "giant/world/weather.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace giant::world {

namespace {

constexpr std::array<std::string_view, 9> k_presets = {"clear", "fair",  "cloudy",   "overcast", "rain",
                                                        "storm", "snow", "blizzard", "fog"};

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
        w.temperature = 14.0f;
        w.wetness = 1.0f;
    } else if (name == "rain") {
        w.cloud_cover = 0.9f;
        w.cloud_density = 0.65f;
        w.cloud_base = 800.0f;
        w.cloud_thickness = 2500.0f;
        w.cloud_scale = 1.5f;
        w.wind_speed = 8.0f;
        w.haze = 2.5f;
        w.fog_density = 0.04f;
        w.fog_height = 300.0f;
        w.precipitation = 0.45f;
        w.temperature = 11.0f;
        w.wetness = 1.0f;
    } else if (name == "snow") {
        w.cloud_cover = 0.9f;
        w.cloud_density = 0.55f;
        w.cloud_base = 900.0f;
        w.cloud_thickness = 2000.0f;
        w.cloud_scale = 1.5f;
        w.wind_speed = 3.0f;
        w.haze = 2.0f;
        w.fog_density = 0.05f;
        w.fog_height = 300.0f;
        w.precipitation = 0.5f;
        w.temperature = -4.0f;
        w.snow_cover = 1.0f;
    } else if (name == "blizzard") {
        w.cloud_cover = 1.0f;
        w.cloud_density = 0.85f;
        w.cloud_base = 500.0f;
        w.cloud_thickness = 4000.0f;
        w.cloud_scale = 2.0f;
        w.wind_speed = 20.0f;
        w.haze = 4.0f;
        w.fog_density = 0.35f;
        w.fog_height = 200.0f;
        w.precipitation = 1.0f;
        w.temperature = -12.0f;
        w.snow_cover = 1.0f;
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
    w.temperature = lerp(a.temperature, b.temperature, t);
    // The ground's starting state is the target's: the editor carries the
    // ground on from what it is, not from a blend of starts.
    w.wetness = b.wetness;
    w.snow_cover = b.snow_cover;
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
    if (!in(w.temperature, -50, 50)) return "temperature is -50 to 50 degrees";
    if (!in(w.wetness, 0, 1) || !in(w.snow_cover, 0, 1)) return "wetness and snow_cover are 0 to 1";
    return std::nullopt;
}

Vec3 wind_velocity(const Weather& w) {
    const float a = degrees_to_radians(w.wind_direction_degrees);
    return {std::sin(a) * w.wind_speed, 0.0f, -std::cos(a) * w.wind_speed};
}

Falling falling(const Weather& w) {
    const float p = std::isfinite(w.precipitation) ? std::clamp(w.precipitation, 0.0f, 1.0f) : 0.0f;
    const float t = std::isfinite(w.temperature) ? w.temperature : 10.0f;
    const float snow_share = std::clamp((2.0f - t) / 3.0f, 0.0f, 1.0f);
    return {p * (1.0f - snow_share), p * snow_share};
}

namespace {

// One step of weather_ground, short enough that the coupling between
// melting and wetting stays close to the continuous one.
Ground ground_step(Ground ground, const Weather& w, double seconds) {
    const Falling f = falling(w);
    const double t = std::isfinite(w.temperature) ? w.temperature : 10.0;
    const double warm = std::clamp((t + 5.0) / 30.0, 0.0, 1.0);
    const double wind = std::isfinite(w.wind_speed) ? std::clamp(static_cast<double>(w.wind_speed), 0.0, 200.0) : 0.0;
    const double cloud = std::isfinite(w.cloud_cover) ? std::clamp(static_cast<double>(w.cloud_cover), 0.0, 1.0) : 0.0;
    // Rates a second, each applied as a steady approach to its end
    // (exponential within the step).
    const double wet_rate = f.rain / 300.0; // wetting: 5 minutes at full rain
    const double dry_rate = (0.3 + 0.7 * warm) * (1.0 + wind / 10.0) * (1.0 - 0.5 * cloud) / 2400.0;
    const double snow_rate = f.snow / 1200.0;                // 20 minutes to cover
    const double melt_rate = t > 0.0 ? t / 6000.0 : 0.0;     // 10 minutes at 10 degrees
    double wet = std::clamp(static_cast<double>(ground.wetness), 0.0, 1.0);
    double snow = std::clamp(static_cast<double>(ground.snow_cover), 0.0, 1.0);
    // Snow: towards 1 while it falls, melting away when warm.
    const double snow_before = snow;
    if (const double sum = snow_rate + melt_rate; sum > 0.0) {
        const double target = snow_rate / sum;
        snow = target + (snow - target) * std::exp(-sum * seconds);
    }
    // Melt water wets the ground (twice what melted: slush is wet).
    const double melted = std::max(snow_before - snow, 0.0);
    wet = std::min(wet + 2.0 * melted, 1.0);
    // Wetness: towards rain's share of rain and drying (a drizzle on a warm
    // day stays damp, not soaked); lying snow keeps what is under it.
    const double wet_out = dry_rate * (1.0 - snow) * (1.0 - f.rain);
    if (const double sum = wet_rate + wet_out; sum > 0.0) {
        const double target = wet_rate / sum;
        wet = target + (wet - target) * std::exp(-sum * seconds);
    }
    return {static_cast<float>(std::clamp(wet, 0.0, 1.0)), static_cast<float>(std::clamp(snow, 0.0, 1.0))};
}

} // namespace

Ground weather_ground(Ground ground, const Weather& w, double seconds) {
    if (!std::isfinite(seconds) || seconds <= 0.0) return ground;
    // Steps of at most 10 s (a minute's worth of steps for long ones past
    // a day), so any step size the caller takes gives about the same.
    const double longest = std::max(10.0, seconds / 8640.0);
    const int steps = static_cast<int>(std::ceil(seconds / longest));
    const double step = seconds / steps;
    for (int i = 0; i < steps; ++i) ground = ground_step(ground, w, step);
    return ground;
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

float sun_share(float elevation_degrees) {
    return std::isfinite(elevation_degrees) ? std::clamp((elevation_degrees + 2.0f) * 0.5f, 0.0f, 1.0f) : 0.0f;
}

WeatherLight weather_light(const Lighting& l, const Weather& w, std::optional<float> daylight) {
    WeatherLight out;
    // Below the horizon the sun stops lighting (night); the sky dims with it.
    const float sun = l.sun_intensity * sun_share(l.sun_elevation_degrees);
    const Vec3 through = air_transmittance(std::max(l.sun_elevation_degrees, 0.0f), w.haze);
    out.sun_light = Vec3{l.sun_color.x * through.x, l.sun_color.y * through.y, l.sun_color.z * through.z} * sun;
    // Day: the sky's light follows the sun up from dusk.
    const float day = daylight && std::isfinite(*daylight) ? std::clamp(*daylight, 0.0f, 1.0f)
                                                           : std::clamp((l.sun_elevation_degrees + 6.0f) / 16.0f, 0.05f, 1.0f);
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
