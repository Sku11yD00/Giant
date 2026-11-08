#include "giant/world/wind.h"

#include "giant/world/weather.h"

#include <algorithm>
#include <cmath>

namespace giant::world {

Wind ground_wind(const Weather& w) {
    Wind g;
    const float speed = std::isfinite(w.wind_speed) ? std::clamp(w.wind_speed, 0.0f, 200.0f) : 0.0f;
    Weather clean = w;
    clean.wind_speed = speed;
    if (!std::isfinite(clean.wind_direction_degrees)) clean.wind_direction_degrees = 0.0f;
    g.velocity = wind_velocity(clean) * 0.5f;
    g.gustiness = std::clamp(0.2f + speed / 50.0f, 0.0f, 1.0f);
    return g;
}

} // namespace giant::world
