#include "clock_state.h"

#include <algorithm>
#include <cmath>

namespace giant::editor {

void ClockState::update(const world::Clock& clock, double game_seconds, bool running, u64 scene) {
    if (!m_seen || !(clock == m_scene) || scene != m_generation) {
        m_seen = true;
        m_scene = clock;
        m_shown = clock;
        m_generation = scene;
    }
    if (running && m_shown.on && std::isfinite(game_seconds) && game_seconds > 0.0 &&
        std::isfinite(m_shown.speed) && m_shown.speed > 0.0f) {
        m_shown = world::advance(m_shown, game_seconds * static_cast<double>(m_shown.speed));
    }
}

void ClockState::set_time(double hours, i32 day) {
    if (!std::isfinite(hours) || hours < 0.0 || hours > 24.0 || day < 1 || day > 365) return;
    m_shown.hours = hours;
    m_shown.day = day;
    m_shown = world::midnight_wrapped(m_shown);
}

void ClockState::show(const world::Clock& clock) {
    m_seen = true;
    m_scene = clock;
    m_shown = clock;
}

ClockLight clock_light(const world::Lighting& lighting, const world::Clock& clock) {
    ClockLight out{lighting, std::nullopt};
    if (!clock.on) return out;
    out.lighting = world::clock_lighting(lighting, clock);
    out.daylight = world::ambient_scale(world::sky_bodies(clock));
    return out;
}

void clock_sky(render::Sky& sky, const world::Clock& clock) {
    if (!clock.on) return;
    const world::SkyBodies b = world::sky_bodies(clock);
    // Under a moon up, brighter (the sky it lights would hide them: the
    // moon's light is far brighter against the stars' than the real one's).
    const float moon_up = std::clamp((b.moon_elevation_degrees + 6.0f) / 16.0f, 0.0f, 1.0f);
    sky.stars = world::star_brightness(b) * (1.0f + 4.0f * b.moon_lit * moon_up);
    sky.celestial = b.to_celestial;
    sky.moon = world::moon_leads(b);
    sky.moon_lit = b.moon_lit;
    sky.moon_sun = b.sun;
}

} // namespace giant::editor
