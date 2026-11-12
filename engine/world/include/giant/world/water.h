// Lakes, rivers and the sea (world::Water in scene.h): checking a body
// and asking where water is. A point is in a lake when it is inside its
// shore, in a river when it is within half the river's width of its path
// (level, width and flow taken along it), and in the sea anywhere: what
// is under the water there is the caller's (water over the ground is the
// level above the ground, none where the ground rises over it). Hidden
// bodies are not there. Where bodies overlap the highest surface wins.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/world/scene.h"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace giant::world {

// What is wrong with a body (limits as the editor's commands and the
// scene's loading keep them), or nothing.
std::optional<std::string> check_water(const Water& water);

std::string_view water_kind_name(WaterKind kind);
std::optional<WaterKind> water_kind_from_name(std::string_view name);

// The water at a point (x, z metres).
struct WaterHere {
    const Water* water = nullptr;
    float level = 0.0f;  // the surface's height
    float flow_x = 0.0f; // its flow (metres a second; rivers only)
    float flow_z = 0.0f;
    float width = 0.0f;  // rivers: the width there
    // The surface's slope there (rise a metre along x and z): the sea's
    // swell (physics' water; water_at's: level, 0).
    float slope_x = 0.0f;
    float slope_z = 0.0f;
};
std::optional<WaterHere> water_at(std::span<const Water> waters, double x, double z);

// One part of a body, for a caller keeping its own index of where the parts
// are (physics): a lake's shore or the sea (`segment` not looked at), or a
// river's stretch from path point `segment` to the next. `nearness` is a
// river's distance from its middle over its half width there (0 to 1):
// water_at takes the nearest stretch. Hidden: none.
std::optional<WaterHere> water_part_at(const Water& water, usize segment, double x, double z, double& nearness);

// The ground a body covers, x and z (a sea: everywhere, false).
bool water_bounds(const Water& water, double& min_x, double& min_z, double& max_x, double& max_z);

// The sea's swell (water plan W5a): a few long waves the wind raises,
// the surface rising and falling under them. Physics floats bodies and
// swimmers on it and the renderer draws it, from the same waves, so what
// floats rides what is seen. A sea's `waves` scales it (0: still).
// Each wave's height at (x, z) at `seconds` is
//   amplitude * sin(k (dir_x x + dir_z z) - sqrt(g k) seconds + phase)
// (deep water's waves: k radians a metre move at sqrt(g k) / k). A wave
// with a bad number, no amplitude, no length or no direction is not there.
// A swell holds the wind's set of waves (k_swell_set, sea_swell) and,
// while the wind changes, the set before it fading out (SwellTrack).
inline constexpr u32 k_swell_set = 4;
inline constexpr u32 k_swell_waves = 2 * k_swell_set;
inline constexpr double k_swell_gravity = 9.81;
struct SwellWave {
    float dir_x = 1.0f; // where it runs (unit, level)
    float dir_z = 0.0f;
    float k = 0.0f;         // radians a metre (2 pi over its length)
    float amplitude = 0.0f; // metres up and down (0: none)
    float phase = 0.0f;     // radians at the origin at 0 seconds
    bool operator==(const SwellWave&) const = default;
};
struct Swell {
    std::array<SwellWave, k_swell_waves> waves{};
    bool operator==(const Swell&) const = default;
};

// The swell under wind `velocity` (m/s, its level part), in the first
// k_swell_set waves: longer and higher the stronger the wind (a fully
// grown sea's, Pierson-Moskowitz: its peak about 0.83 U^2 metres long,
// kept within 40 to 250 m, the fourth wave, from far off, 1.4 times that;
// significant height 0.21 U^2 / g, kept within 0.3 to 4 m), the main waves
// running with it. A calm keeps a low swell from far off (0.3 m).
Swell sea_swell(const Vec3& velocity);

// The swell following a changing wind without its waves jumping: a
// wave's length and direction held while the wind's own stay near them
// (within a tenth of the length, 15 degrees), only its height following
// (easing towards the wind's, k_swell_ease seconds to come most of the way);
// past that a new set starts, faded in over k_swell_fade seconds as the
// old one fades out (the same sines throughout: a wave's phase never
// moves but by its own speed). A change while a fade runs waits for it.
inline constexpr double k_swell_fade = 20.0;
inline constexpr double k_swell_ease = 10.0;
class SwellTrack {
public:
    // The wind at `seconds` (the water's clock).
    void update(const Vec3& velocity, double seconds);
    const Swell& swell() const { return m_swell; }

private:
    std::array<SwellWave, k_swell_set> m_now{};
    std::array<SwellWave, k_swell_set> m_old{};
    double m_fade_start = 0.0;
    double m_last_seconds = 0.0;
    bool m_started = false;
    bool m_fading = false;
    Swell m_swell;
};
// The swell's height at (x, z) at `seconds` (metres above the still level,
// before the sea's `waves`; worked out in double, so far out and late on it
// keeps its millimetres). Not a number in: 0.
double swell_height(const Swell& swell, double x, double z, double seconds);
// Its height and slope (rise a metre along x and z) there.
struct SwellHere {
    double height = 0.0;
    double slope_x = 0.0;
    double slope_z = 0.0;
};
SwellHere swell_at(const Swell& swell, double x, double z, double seconds);
// The most it rises (its waves' amplitudes summed).
float swell_most(const Swell& swell);
// How much of the swell a sea raises (its `waves`, 0 to 4; a bad number: 0).
float swell_scale(const Water& water);

} // namespace giant::world
