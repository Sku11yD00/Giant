// The clock's sky: the sun by the date, the hour and the latitude, the
// moon by its phase, the stars' turn, the time moving on, and the light.
#include "giant/world/clock.h"

#include "giant_test.h"

#include <algorithm>
#include <cmath>

using namespace giant;
using namespace giant::world;

namespace {

Clock at(double hours, i32 day, float latitude, float north = 0.0f) {
    Clock c;
    c.on = true;
    c.hours = hours;
    c.day = day;
    c.latitude = latitude;
    c.north_degrees = north;
    return c;
}

bool near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

} // namespace

GIANT_TEST("clock: the sun at noon stands 90 - latitude up at the equinox, solstices tilt it 23.4 degrees") {
    for (const float lat : {0.0f, 30.0f, 51.5f, -33.9f}) {
        const SkyBodies b = sky_bodies(at(12.0, 80, lat));
        GIANT_CHECK(near(b.sun_elevation_degrees, 90.0f - std::fabs(lat), 1.0f));
        // Due south from the north, due north from the south.
        if (lat > 1.0f) GIANT_CHECK(near(b.sun_azimuth_degrees, 180.0f, 1.0f));
        if (lat < -1.0f) GIANT_CHECK(b.sun_azimuth_degrees < 1.0f || b.sun_azimuth_degrees > 359.0f);
    }
    GIANT_CHECK(near(sky_bodies(at(12.0, 172, 40.0f)).sun_elevation_degrees, 73.4f, 1.0f)); // midsummer
    GIANT_CHECK(near(sky_bodies(at(12.0, 355, 40.0f)).sun_elevation_degrees, 26.6f, 1.0f)); // midwinter
    // Midnight: under the horizon (save the midsummer arctic: the midnight sun).
    GIANT_CHECK(sky_bodies(at(0.0, 80, 40.0f)).sun_elevation_degrees < -45.0f);
    GIANT_CHECK(sky_bodies(at(0.0, 172, 75.0f)).sun_elevation_degrees > 0.0f);
}

GIANT_TEST("clock: the sun rises in the east about six and sets in the west, and north_degrees turns the world") {
    const SkyBodies rise = sky_bodies(at(6.0, 80, 45.0f));
    GIANT_CHECK(near(rise.sun_elevation_degrees, 0.0f, 1.5f));
    GIANT_CHECK(near(rise.sun_azimuth_degrees, 90.0f, 1.5f));
    GIANT_CHECK(rise.sun.x > 0.99f); // world +X is east
    const SkyBodies set = sky_bodies(at(18.0, 80, 45.0f));
    GIANT_CHECK(near(set.sun_azimuth_degrees, 270.0f, 1.5f));
    // Summer: rising north of east, up longer.
    GIANT_CHECK(sky_bodies(at(6.0, 172, 45.0f)).sun_elevation_degrees > 5.0f);
    GIANT_CHECK(sky_bodies(at(6.0, 172, 45.0f)).sun_azimuth_degrees < 80.0f);
    // The world's -Z pointing east: the rising sun is straight down -Z.
    const SkyBodies turned = sky_bodies(at(6.0, 80, 45.0f, 90.0f));
    GIANT_CHECK(turned.sun.z < -0.99f);
    GIANT_CHECK(near(turned.sun_elevation_degrees, rise.sun_elevation_degrees, 0.01f));
}

GIANT_TEST("clock: the moon's phase over the month, and where it stands") {
    // New on day 1: by the sun, dark.
    const SkyBodies newm = sky_bodies(at(12.0, 1, 45.0f));
    GIANT_CHECK(newm.moon_lit < 0.01f && dot(newm.moon, newm.sun) > 0.95f);
    // Full about 14.8 days on: opposite the sun, all lit, high at midnight.
    const SkyBodies full = sky_bodies(at(0.0, 16, 45.0f));
    GIANT_CHECK(full.moon_lit > 0.99f && near(full.moon_age, 0.5f, 0.02f));
    GIANT_CHECK(dot(full.moon, full.sun) < -0.95f && full.moon_elevation_degrees > 40.0f);
    // A first quarter: half lit, 90 degrees from the sun.
    const SkyBodies quarter = sky_bodies(at(18.0, 8, 45.0f));
    GIANT_CHECK(near(quarter.moon_lit, 0.5f, 0.06f) && std::fabs(dot(quarter.moon, quarter.sun)) < 0.15f);
    // The moon rises about 50 minutes later each day.
    const auto rise_hour = [](i32 day) {
        for (double h = 0.0; h < 24.0; h += 1.0 / 60.0) {
            if (sky_bodies(at(h, day, 0.0f)).moon_elevation_degrees < 0.0f &&
                sky_bodies(at(h + 1.0 / 60.0, day, 0.0f)).moon_elevation_degrees >= 0.0f)
                return h;
        }
        return -1.0;
    };
    const double a = rise_hour(100), b = rise_hour(101);
    GIANT_CHECK(a >= 0.0 && b >= 0.0 && b - a > 0.6 && b - a < 1.1);
}

GIANT_TEST("clock: the stars turn with the hours, the pole star fixed") {
    // The celestial pole stands latitude up, due north.
    const SkyBodies b = sky_bodies(at(22.0, 80, 50.0f));
    const float e = 50.0f * 3.14159265f / 180.0f;
    const Vec3 pole{0.0f, std::sin(e), -std::cos(e)};
    const auto to_sky = [](const SkyBodies& s, const Vec3& d) {
        return Vec3{dot(s.to_celestial[0], d), dot(s.to_celestial[1], d), dot(s.to_celestial[2], d)};
    };
    GIANT_CHECK(to_sky(b, pole).z > 0.9999f);
    const SkyBodies later = sky_bodies(at(23.0, 80, 50.0f));
    GIANT_CHECK(to_sky(later, pole).z > 0.9999f);
    // Overhead turns about 15 degrees in an hour (sidereal: a little more).
    const Vec3 up{0.0f, 1.0f, 0.0f};
    const float turned = std::acos(std::clamp(dot(to_sky(b, up), to_sky(later, up)), -1.0f, 1.0f)) * 180.0f / 3.14159265f;
    GIANT_CHECK(near(turned, 15.04f * std::cos(e), 0.3f)); // the arc at declination 50
    // The rows are a rotation.
    for (int i = 0; i < 3; ++i) GIANT_CHECK(near(length(b.to_celestial[static_cast<usize>(i)]), 1.0f, 1e-4f));
    GIANT_CHECK(std::fabs(dot(b.to_celestial[0], b.to_celestial[1])) < 1e-4f);
}

GIANT_TEST("clock: time moves on through midnight and the year's end; check_clock") {
    Clock c = at(23.0, 365, 45.0f);
    c = advance(c, 7200.0);
    GIANT_CHECK(near(static_cast<float>(c.hours), 1.0f, 1e-6f) && c.day == 1);
    c = advance(c, 86400.0 * 400.0);
    GIANT_CHECK(c.day == 36 && near(static_cast<float>(c.hours), 1.0f, 1e-6f));
    GIANT_CHECK(advance(c, -5.0) == c);
    GIANT_CHECK(advance(c, std::nan("")) == c);
    GIANT_CHECK(!check_clock(Clock{}).has_value());
    Clock bad;
    bad.hours = 24.5;
    GIANT_CHECK(check_clock(bad).has_value());
    bad = Clock{};
    bad.speed = -1.0f;
    GIANT_CHECK(check_clock(bad).has_value());
}

GIANT_TEST("clock: the light by day is the sun, by night the moon by its phase; the sky's light falls but not to black") {
    Lighting l;
    l.sun_intensity = 3.0f;
    // Off: unchanged.
    GIANT_CHECK(clock_lighting(l, Clock{}).sun_elevation_degrees == l.sun_elevation_degrees);
    const Lighting day = clock_lighting(l, at(10.0, 172, 45.0f));
    GIANT_CHECK(day.sun_intensity == 3.0f && day.sun_elevation_degrees > 40.0f);
    const SkyBodies noon = sky_bodies(at(12.0, 172, 45.0f));
    GIANT_CHECK(ambient_scale(noon) == 1.0f && !moon_leads(noon) && star_brightness(noon) == 0.0f);
    // A full moon's night against a new moon's.
    const SkyBodies full = sky_bodies(at(0.0, 16, 45.0f));
    const SkyBodies dark = sky_bodies(at(0.0, 1, 45.0f));
    GIANT_CHECK(moon_leads(full) && moon_leads(dark));
    const Lighting moonlit = clock_lighting(l, at(0.0, 16, 45.0f));
    GIANT_CHECK(near(moonlit.sun_intensity, 3.0f * k_moon_light, 0.01f));
    GIANT_CHECK(near(moonlit.sun_elevation_degrees, full.moon_elevation_degrees, 1e-4f));
    GIANT_CHECK(clock_lighting(l, at(0.0, 1, 45.0f)).sun_intensity < 0.001f);
    GIANT_CHECK(ambient_scale(full) > ambient_scale(dark) && ambient_scale(dark) >= k_star_light);
    GIANT_CHECK(ambient_scale(full) < 0.05f);
    GIANT_CHECK(star_brightness(full) == 1.0f);
    // The handover: the moon's light starts at nothing (the sun gave none from 2 degrees under).
    for (double h = 17.0; h < 21.0; h += 1.0 / 120.0) {
        const Clock c = at(h, 16, 45.0f);
        const SkyBodies b = sky_bodies(c);
        if (moon_leads(b) && b.sun_elevation_degrees > k_moon_leads_below - 0.2f)
            GIANT_CHECK(clock_lighting(l, c).sun_intensity < 0.01f);
    }
}
