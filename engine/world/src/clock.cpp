#include "giant/world/clock.h"

#include <algorithm>
#include <cmath>

namespace giant::world {

namespace {

constexpr double k_pi_d = 3.14159265358979323846;
constexpr double k_obliquity = 23.44 * k_pi_d / 180.0;
constexpr double k_year_days = 365.2422;

bool in(double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; }

struct D3 {
    double x, y, z;
};

// A point on the ecliptic at longitude `l` (radians), in the equatorial frame.
D3 ecliptic(double l) {
    return {std::cos(l), std::cos(k_obliquity) * std::sin(l), std::sin(k_obliquity) * std::sin(l)};
}

// The rows of the equatorial-to-world rotation (each row: a world axis's
// components in the equatorial frame), for latitude `lat` and local
// sidereal angle `lst` (radians), the world turned by `north` (radians).
struct Frame {
    D3 x, y, z; // world +X, +Y, +Z as equatorial vectors
};

Frame frame_of(double lat, double lst, double north) {
    const double sl = std::sin(lat), cl = std::cos(lat), st = std::sin(lst), ct = std::cos(lst);
    // East, north and up as equatorial vectors.
    const D3 east{-st, ct, 0.0};
    const D3 nth{-sl * ct, -sl * st, cl};
    const D3 up{cl * ct, cl * st, sl};
    // The world's -Z heads `north` round from north (towards east), +X a
    // quarter turn on: world = (E cos n - N sin n, U, -(N cos n + E sin n)).
    const double cn = std::cos(north), sn = std::sin(north);
    Frame f;
    f.x = {east.x * cn - nth.x * sn, east.y * cn - nth.y * sn, east.z * cn - nth.z * sn};
    f.y = up;
    f.z = {-(nth.x * cn + east.x * sn), -(nth.y * cn + east.y * sn), -(nth.z * cn + east.z * sn)};
    return f;
}

double dot(const D3& a, const D3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

Vec3 to_world(const Frame& f, const D3& e) {
    return normalize_or_zero(Vec3{static_cast<float>(dot(f.x, e)), static_cast<float>(dot(f.y, e)),
                                  static_cast<float>(dot(f.z, e))});
}

// Elevation and compass azimuth (as Lighting's: the world's own compass,
// north = -Z) of a world direction.
void angles_of(const Vec3& d, float& elevation, float& azimuth) {
    elevation = static_cast<float>(std::asin(std::clamp(static_cast<double>(d.y), -1.0, 1.0)) * 180.0 / k_pi_d);
    double a = std::atan2(static_cast<double>(d.x), static_cast<double>(-d.z)) * 180.0 / k_pi_d;
    if (a < 0.0) a += 360.0;
    azimuth = static_cast<float>(a);
}

float smooth(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// The day sky's light by a body's elevation, as weather_light's dusk.
float daylight(float elevation_degrees) { return std::clamp((elevation_degrees + 6.0f) / 16.0f, 0.0f, 1.0f); }

} // namespace

SkyBodies sky_bodies(const Clock& clock) {
    SkyBodies b;
    const double hours = std::isfinite(clock.hours) ? std::clamp(clock.hours, 0.0, 24.0) : 12.0;
    const double day = static_cast<double>(std::clamp(clock.day, 1, 365));
    const double lat = (std::isfinite(clock.latitude) ? std::clamp(static_cast<double>(clock.latitude), -90.0, 90.0) : 0.0) *
                       k_pi_d / 180.0;
    const double north = (std::isfinite(clock.north_degrees) ? static_cast<double>(clock.north_degrees) : 0.0) * k_pi_d / 180.0;
    const double days = day - 1.0 + hours / 24.0;
    // The sun: on the ecliptic, at the spring equinox on day 80.
    const double sun_l = 2.0 * k_pi_d * (day - 80.0 + hours / 24.0) / k_year_days;
    const D3 sun = ecliptic(sun_l);
    // Local solar time: the sun's hour angle is 15 degrees an hour from noon,
    // so the sky's (sidereal) angle is that plus the sun's right ascension.
    const double sun_ra = std::atan2(sun.y, sun.x);
    const double lst = (hours - 12.0) * k_pi_d / 12.0 + sun_ra;
    const Frame f = frame_of(lat, lst, north);
    // The moon: its phase's angle on from the sun along the ecliptic (new on day 1).
    const double age = days / static_cast<double>(k_lunar_month_days);
    b.moon_age = static_cast<float>(age - std::floor(age));
    b.moon_lit = static_cast<float>(0.5 * (1.0 - std::cos(2.0 * k_pi_d * b.moon_age)));
    const D3 moon = ecliptic(sun_l + 2.0 * k_pi_d * b.moon_age);
    b.sun = to_world(f, sun);
    b.moon = to_world(f, moon);
    angles_of(b.sun, b.sun_elevation_degrees, b.sun_azimuth_degrees);
    angles_of(b.moon, b.moon_elevation_degrees, b.moon_azimuth_degrees);
    // World to equatorial: the world axes' equatorial vectors are the
    // columns of that rotation; its rows are what the shader dots with.
    b.to_celestial = {Vec3{static_cast<float>(f.x.x), static_cast<float>(f.y.x), static_cast<float>(f.z.x)},
                      Vec3{static_cast<float>(f.x.y), static_cast<float>(f.y.y), static_cast<float>(f.z.y)},
                      Vec3{static_cast<float>(f.x.z), static_cast<float>(f.y.z), static_cast<float>(f.z.z)}};
    return b;
}

Clock advance(const Clock& clock, double game_seconds) {
    if (!std::isfinite(game_seconds) || game_seconds <= 0.0 || !std::isfinite(clock.hours)) return clock;
    Clock c = clock;
    const double total = c.hours + game_seconds / 3600.0;
    const double whole_days = std::floor(total / 24.0);
    c.hours = total - whole_days * 24.0;
    if (c.hours >= 24.0) c.hours = 0.0; // rounding
    const double day = static_cast<double>(std::clamp(c.day, 1, 365)) - 1.0 + whole_days;
    c.day = static_cast<i32>(day - std::floor(day / 365.0) * 365.0) + 1;
    return c;
}

std::optional<std::string> check_clock(const Clock& c) {
    if (!in(c.hours, 0.0, 24.0)) return "hours must be 0 to 24";
    if (c.day < 1 || c.day > 365) return "day must be 1 to 365";
    if (!in(c.latitude, -90.0, 90.0)) return "latitude must be -90 to 90";
    if (!in(c.north_degrees, 0.0, 360.0)) return "north_degrees must be 0 to 360";
    if (!in(c.speed, 0.0, 86400.0)) return "speed must be 0 to 86400";
    return std::nullopt;
}

Lighting clock_lighting(const Lighting& lighting, const Clock& clock) {
    if (!clock.on) return lighting;
    const SkyBodies b = sky_bodies(clock);
    Lighting out = lighting;
    if (!moon_leads(b)) {
        out.sun_elevation_degrees = b.sun_elevation_degrees;
        out.sun_azimuth_degrees = b.sun_azimuth_degrees;
        return out;
    }
    // Night: the moon is the light (under the horizon it lights nothing,
    // as the sun), faded in as the dusk deepens (the sun gave none from 2
    // degrees under, so the handover shows no jump).
    out.sun_elevation_degrees = b.moon_elevation_degrees;
    out.sun_azimuth_degrees = b.moon_azimuth_degrees;
    out.sun_color = {0.78f, 0.84f, 1.0f};
    const float dusk = smooth(k_moon_leads_below, k_moon_leads_below - 6.0f, b.sun_elevation_degrees);
    out.sun_intensity = lighting.sun_intensity * k_moon_light * b.moon_lit * dusk;
    return out;
}

bool moon_leads(const SkyBodies& b) { return !(b.sun_elevation_degrees > k_moon_leads_below); }

float star_brightness(const SkyBodies& b) { return smooth(-4.0f, -12.0f, b.sun_elevation_degrees); }

float ambient_scale(const SkyBodies& b) {
    const float sun = daylight(b.sun_elevation_degrees);
    const float moon = k_moon_light * b.moon_lit * daylight(b.moon_elevation_degrees);
    return std::min(sun + moon + k_star_light, 1.0f);
}

} // namespace giant::world
