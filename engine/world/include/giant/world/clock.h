// The time of day: where the sun, the moon and the stars are over a
// scene, and the light they give. A scene's Clock, when on, places the sun
// by the date, the hour and the latitude (instead of Lighting's angles);
// at night the moon (lit by its phase) takes the sun's place as the light
// that casts shadows, and the stars turn overhead.
//
// The sky is worked out from a simple model, good to about a degree: the
// sun on a circular orbit (the equinox on day 80), the moon on the
// ecliptic at its phase's angle from the sun (new on day 1, every 29.53
// days), local solar time (no time zones or equation of time).
//
// Directions are the world's: +X east, -Z north, +Y up, then turned by the
// clock's north_degrees (the compass heading the world's -Z points to).
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/world/scene.h"

#include <array>
#include <optional>
#include <string>

namespace giant::world {

// How bright the moon's light is, full, against the sun's (the auto
// exposure brings a moonlit night up; the ratio keeps its shadows and
// colours readable), and the stars' and night air's own glow against the
// day sky's light.
inline constexpr float k_moon_light = 0.025f;
inline constexpr float k_star_light = 0.004f;
inline constexpr float k_lunar_month_days = 29.530588f;
// The sun's elevation (degrees) under which the moon is the light.
inline constexpr float k_moon_leads_below = -6.0f;

// Where the sun and moon are now, and the sky's turn.
struct SkyBodies {
    Vec3 sun{0.0f, 1.0f, 0.0f};  // unit, towards it
    Vec3 moon{0.0f, -1.0f, 0.0f};
    float sun_elevation_degrees = 90.0f; // above the horizon (negative: under it)
    float sun_azimuth_degrees = 0.0f;    // compass, as Lighting's (0 north, 90 east; world turned by north_degrees)
    float moon_elevation_degrees = -90.0f;
    float moon_azimuth_degrees = 0.0f;
    float moon_age = 0.0f; // 0 new, 0.5 full, up to 1
    float moon_lit = 0.0f; // the share of its disc lit (0 new, 1 full)
    // World direction to the sky's own frame (equatorial: x towards the
    // spring equinox, z towards the celestial north pole): rows of the
    // rotation, so stars fixed in that frame turn as the earth does.
    std::array<Vec3, 3> to_celestial{Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}};
};

SkyBodies sky_bodies(const Clock& clock);

// The clock `game_seconds` later (hours wrap into days, day 365 into 1).
// Not finite or negative: unchanged.
Clock advance(const Clock& clock, double game_seconds);

// Why it cannot be used (a value out of range), or none.
std::optional<std::string> check_clock(const Clock& clock);

// Whether the moon is the light (the sun under k_moon_leads_below).
bool moon_leads(const SkyBodies& bodies);

// The scene's lighting with its light source placed by the clock: the
// sun while it is up or near (down to 6 degrees under the horizon), else
// the moon, lit by its phase and fading in as the dusk deepens (to 12
// degrees under), in the moon's colour. Clock off: unchanged.
Lighting clock_lighting(const Lighting& lighting, const Clock& clock);

// How bright the stars are drawn (0 by day, 1 at night): coming out from
// the sun 4 degrees under the horizon to 12 under.
float star_brightness(const SkyBodies& bodies);

// The sky's ambient light at this time, against the day's (0 to 1): the
// sun's sky from 6 degrees under the horizon up to 10 above, the moon's
// (k_moon_light by its phase) likewise, and the stars' (k_star_light).
float ambient_scale(const SkyBodies& bodies);

} // namespace giant::world
