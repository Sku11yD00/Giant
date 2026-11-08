#include "giant/core/wind.h"

#include <algorithm>
#include <cmath>

// Keep in step with engine/render/shaders/common.slang (wind_offset,
// wind_gust, wind_reach): the same numbers in the same order.

namespace giant {

namespace {

constexpr float k_tau = 6.28318530717958647692f;

double wrap(double v, double period) {
    const double r = v - period * std::floor(v / period);
    return r >= period ? 0.0 : r;
}

bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// The gust pattern at a place already moved by the drift and wrapped.
float gust_pattern(float u, float v) {
    const float k = k_tau / static_cast<float>(k_gust_tile);
    return 0.5f + 0.25f * std::sin(k * (2.0f * u + v)) + 0.15f * std::sin(k * (-3.0f * u + 5.0f * v) + 1.0f) +
           0.1f * std::sin(k * (7.0f * u + 4.0f * v) + 2.0f);
}

// The wind's speed scaled by a gust (1 - 0.6 gustiness to 1 + 0.6 gustiness).
float gust_scale(float gust, float gustiness) { return 1.0f + gustiness * (gust - 0.5f) * 1.2f; }

// How far it leans from its base, per metre of height, at this speed.
float lean_of(float sway, float speed) { return sway * (0.004f * speed + 0.00018f * speed * speed); }

// How far its tips shake (metres) at this speed.
float shake_of(float flutter, float speed) {
    return std::min(flutter * (0.002f * speed + 0.00008f * speed * speed), 0.12f * flutter);
}

// How fast it rocks, against the sway clock: slower when taller, in
// sixteenths (a whole number of turns in the clock's period).
float rock_rate(float height) {
    const float m = std::round(16.0f * 2.0f / std::sqrt(std::max(height, 0.25f))) / 16.0f;
    return std::clamp(m, 0.25f, 4.0f);
}

} // namespace

void WindClock::advance(const Wind& wind, double seconds) {
    m_wind = wind;
    if (!finite(m_wind.velocity)) m_wind.velocity = {};
    m_wind.velocity.y = 0.0f;
    m_wind.gustiness = std::isfinite(m_wind.gustiness) ? std::clamp(m_wind.gustiness, 0.0f, 1.0f) : 0.0f;
    if (!std::isfinite(seconds) || seconds <= 0.0 || seconds > 60.0) return;
    const double speed = static_cast<double>(length(m_wind.velocity));
    // The gusts drift at most of the wind's speed; the clocks run faster
    // the harder it blows.
    m_gust_x = wrap(m_gust_x + static_cast<double>(m_wind.velocity.x) * 0.8 * seconds, k_gust_tile);
    m_gust_z = wrap(m_gust_z + static_cast<double>(m_wind.velocity.z) * 0.8 * seconds, k_gust_tile);
    m_sway = wrap(m_sway + (0.3 + 0.03 * speed) * seconds, k_wind_period);
    m_flutter = wrap(m_flutter + (1.0 + 0.15 * speed) * seconds, k_wind_period);
}

WindState WindClock::state() const {
    WindState s;
    s.velocity = m_wind.velocity;
    s.gustiness = m_wind.gustiness;
    s.gust_x = static_cast<float>(m_gust_x);
    s.gust_z = static_cast<float>(m_gust_z);
    s.sway_clock = static_cast<float>(m_sway);
    s.flutter_clock = static_cast<float>(m_flutter);
    return s;
}

float gust_at(const WindState& wind, double x, double z) {
    const float u = static_cast<float>(wrap(x - static_cast<double>(wind.gust_x), k_gust_tile));
    const float v = static_cast<float>(wrap(z - static_cast<double>(wind.gust_z), k_gust_tile));
    return std::clamp(gust_pattern(u, v), 0.0f, 1.0f);
}

Vec3 wind_at(const WindState& wind, const DVec3& point) {
    return wind.velocity * gust_scale(gust_at(wind, point.x, point.z), wind.gustiness);
}

Vec3 sway_offset(const WindState& wind, const DVec3& base, float height, const DVec3& rest, float sway,
                 float flutter) {
    const float speed0 = length(wind.velocity);
    if (!(height > 0.0f) || !std::isfinite(height) || !(speed0 > 1e-4f) || !(sway > 0.0f || flutter > 0.0f)) {
        return {};
    }
    sway = std::clamp(sway, 0.0f, 4.0f);
    flutter = std::clamp(flutter, 0.0f, 4.0f);
    const Vec3 dir = wind.velocity * (1.0f / speed0);
    const Vec3 side{-dir.z, 0.0f, dir.x};
    const float speed = speed0 * gust_scale(gust_at(wind, base.x, base.z), wind.gustiness);
    const Vec3 rel{static_cast<float>(rest.x - base.x), static_cast<float>(rest.y - base.y),
                   static_cast<float>(rest.z - base.z)};
    const float h = std::clamp(rel.y / height, 0.0f, 1.0f);
    const float bend = h * h;

    // Leaning and rocking: the base's place sets its phase.
    const float bx = static_cast<float>(wrap(base.x, k_gust_tile));
    const float bz = static_cast<float>(wrap(base.z, k_gust_tile));
    const float phase = k_tau * (7.0f * bx + 13.0f * bz) / static_cast<float>(k_gust_tile);
    const float lean = lean_of(sway, speed);
    const float rock = lean * (0.3f + 0.6f * wind.gustiness);
    const float m = rock_rate(height);
    const float t = wind.sway_clock;
    const float swing = std::sin(k_tau * m * t + phase) + 0.3f * std::sin(k_tau * 2.0f * m * t + 2.0f * phase);
    float along = std::clamp(lean + rock * swing, -k_max_sway_lean, k_max_sway_lean);
    float across = rock * 0.35f * std::sin(k_tau * 1.5f * m * t + 3.0f * phase);
    // Never further than the most it leans, whichever way.
    const float bent = std::sqrt(along * along + across * across);
    const float hold = bent > k_max_sway_lean ? k_max_sway_lean / bent : 1.0f;
    along *= hold;
    across *= hold;
    Vec3 offset = (dir * along + side * across) * (height * bend);
    // Bending, not stretching: what leans drops a little.
    const float up = std::max(rel.y, 0.0f);
    offset.y = -std::min(dot(offset, offset) / (2.0f * std::max(up, 0.01f)), 0.5f * up);

    if (flutter > 0.0f) {
        const float shake = shake_of(flutter, speed);
        const float spread = std::sqrt(rel.x * rel.x + rel.z * rel.z);
        const float weight = h * std::clamp(spread / (0.25f * height + 0.01f), 0.0f, 1.0f);
        const float px = static_cast<float>(wrap(rest.x, k_gust_tile));
        const float py = static_cast<float>(wrap(rest.y, k_gust_tile));
        const float pz = static_cast<float>(wrap(rest.z, k_gust_tile));
        const float k = k_tau / static_cast<float>(k_gust_tile);
        const float f = wind.flutter_clock;
        const Vec3 n{std::sin(k_tau * 3.0f * f + k * (37.0f * px + 41.0f * py + 23.0f * pz)),
                     0.5f * std::sin(k_tau * 4.0f * f + k * (-29.0f * px + 43.0f * py + 17.0f * pz)),
                     std::sin(k_tau * 5.0f * f + k * (19.0f * px - 31.0f * py + 47.0f * pz))};
        offset = offset + n * (shake * weight);
    }
    return offset;
}

float max_sway_offset(const WindState& wind, float height, float sway, float flutter) {
    const float speed0 = length(wind.velocity);
    if (!(height > 0.0f) || !std::isfinite(height) || !(speed0 > 1e-4f) || !(sway > 0.0f || flutter > 0.0f)) {
        return 0.0f;
    }
    sway = std::clamp(sway, 0.0f, 4.0f);
    flutter = std::clamp(flutter, 0.0f, 4.0f);
    const float speed = speed0 * (1.0f + 0.6f * wind.gustiness);
    const float lean = lean_of(sway, speed);
    const float rock = lean * (0.3f + 0.6f * wind.gustiness);
    const float along = std::min(lean + 1.3f * rock, k_max_sway_lean);
    const float across = 0.35f * rock;
    // The lean is held to k_max_sway_lean, so the drop is at most 0.15 of it (see sway_offset).
    const float lean_metres = height * std::min(std::sqrt(along * along + across * across), k_max_sway_lean) * 1.05f;
    return lean_metres + 1.5f * shake_of(flutter, speed);
}

} // namespace giant
