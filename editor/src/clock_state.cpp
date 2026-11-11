#include "clock_state.h"

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
    m_shown.hours = hours >= 24.0 ? 0.0 : hours;
    m_shown.day = day;
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
    sky.stars = world::star_brightness(b);
    sky.celestial = b.to_celestial;
    sky.moon = world::moon_leads(b);
    sky.moon_sun = b.sun;
}

} // namespace giant::editor
