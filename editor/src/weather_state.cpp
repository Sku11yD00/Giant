#include "weather_state.h"

#include "scene_render.h"

#include "giant/world/weather.h"

#include <algorithm>
#include <cmath>

namespace giant::editor {

void WeatherState::transition(double seconds) {
    m_pending = std::isfinite(seconds) ? std::clamp(seconds, 0.0, 3600.0) : 0.0;
    m_requested = true;
}

double WeatherState::progress() const {
    return m_duration > 0.0 ? std::clamp(m_elapsed / m_duration, 0.0, 1.0) : 1.0;
}

double WeatherState::remaining() const {
    return m_from && m_duration > 0.0 ? std::max(m_duration - m_elapsed, 0.0) : 0.0;
}

double cloud_tile_metres(const world::Weather& weather) {
    return 32000.0 * std::max(static_cast<double>(weather.cloud_scale), 0.03125);
}

void WeatherState::update(const std::optional<world::Weather>& target, double seconds, const DVec3& camera) {
    seconds = std::isfinite(seconds) ? std::clamp(seconds, 0.0, 1.0) : 0.0; // a stall does not jump the weather
    // A change, or a command asking again (the same weather at once ends a blend).
    if (!(target == m_target) || m_requested) {
        m_target = target;
        if (m_pending > 0.0 && m_shown && target) {
            m_from = m_shown;
            m_duration = m_pending;
            m_elapsed = 0.0;
        } else {
            m_from.reset();
            m_duration = 0.0;
            m_elapsed = 0.0;
        }
        m_pending = 0.0;
        m_requested = false;
    }
    if (!m_target) {
        m_shown.reset();
        m_from.reset();
        m_duration = 0.0;
        m_tile = 0.0;
        return;
    }
    if (m_from && m_duration > 0.0) {
        m_elapsed += seconds;
        const double t = m_elapsed / m_duration;
        if (t >= 1.0) {
            m_shown = m_target;
            m_from.reset();
            m_duration = 0.0;
        } else {
            m_shown = world::blend(*m_from, *m_target, static_cast<float>(t));
        }
    } else {
        m_shown = m_target;
    }
    // The pattern under the camera is at (camera - offset) / tile: a new
    // tile rescales the offset about the camera so that stays where it is.
    const double tile = cloud_tile_metres(*m_shown);
    if (m_tile > 0.0 && tile != m_tile) {
        const double r = tile / m_tile;
        m_offset = {camera.x - (camera.x - m_offset.x) * r, 0.0, camera.z - (camera.z - m_offset.z) * r};
    }
    m_tile = tile;
    const Vec3 wind = world::wind_velocity(*m_shown);
    m_offset = m_offset + DVec3{wind.x, 0.0, wind.z} * seconds; // doubles: exact for centuries of wind
}

render::Lighting weather_lighting(const world::Lighting& lighting, const std::optional<world::Weather>& weather) {
    render::Lighting out = scene_lighting(lighting);
    if (weather) {
        const world::WeatherLight w = world::weather_light(lighting, *weather);
        out.sun_light = w.sun_light;
        out.sky_light = w.sky_light;
        out.ground_light = w.ground_light;
    }
    return out;
}

render::Sky weather_sky(const world::Lighting& lighting, const std::optional<world::Weather>& weather,
                        const DVec3& cloud_offset) {
    render::Sky sky;
    if (!weather) {
        return sky;
    }
    const world::Weather& w = *weather;
    sky.on = true;
    const float sun = lighting.sun_elevation_degrees > -2.0f ? lighting.sun_intensity : 0.0f;
    sky.sun_top = lighting.sun_color * sun;
    sky.haze = w.haze;
    sky.cloud_cover = w.cloud_cover;
    sky.cloud_density = w.cloud_density;
    sky.cloud_base = w.cloud_base;
    sky.cloud_top = w.cloud_base + w.cloud_thickness;
    sky.cloud_tile = static_cast<float>(cloud_tile_metres(w));
    sky.cloud_offset = cloud_offset;
    // fog_density 1: things 150 m away mostly hidden (a few optical depths).
    sky.fog_density = w.fog_density * 0.02f;
    sky.fog_falloff = std::log(2.0f) / std::max(w.fog_height, 1.0f);
    return sky;
}

} // namespace giant::editor
