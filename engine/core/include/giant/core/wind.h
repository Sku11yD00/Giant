// Wind, and how what stands in it moves.
//
//   WindClock clock;
//   clock.advance(world::ground_wind(scene.weather), delta); // each frame
//   const WindState wind = clock.state();                // to the renderer
//   const Vec3 at = wind_at(wind, position);             // m/s, gusts included
//   const Vec3 moved = sway_offset(wind, base, height, rest, m.sway, m.flutter);
//
// Gusts are a
// pattern 64 m across that drifts with the wind: they roll through grass
// and trees in waves. A thing that sways (Material::sway, flutter) bends
// from its base along the wind, further the harder it blows (the push goes
// with the speed squared), and rocks back and forth about that lean, faster
// in a storm and slower the taller it is; flutter shakes what stands out
// from its middle (leaves at the tips) in small fast motions.
//
// Everything here is the same function of a few numbers the renderer has
// too (render's common.slang, wind_offset): no state per thing, so
// millions sway for nothing but the shader work, and the simulation
// (cloth, swinging things, dust) feels the same gusts the trees show. The clocks wrap (gusts every
// 64 m, the clocks every 256 s, with every frequency a whole number of
// turns in that) so floats keep their precision however long it runs, and
// the pattern is the same at a point whatever the numbers' origin (the
// shader works camera-relative, with the camera's place modulo 64 m).
#pragma once

#include "giant/core/math.h"

namespace giant {

// A wind: where it blows and how it gusts (world::ground_wind makes one
// from the weather).
struct Wind {
    Vec3 velocity{};        // m/s (y 0)
    float gustiness = 0.0f; // 0 steady to 1: how much gusts come and go
    bool operator==(const Wind&) const = default;
};

// What the wind is now: the renderer and the simulation read this.
struct WindState {
    Vec3 velocity{};
    float gustiness = 0.0f;
    // The gust pattern's drift (metres, 0 to 64) and the sway and flutter
    // clocks (turns, 0 to 256).
    float gust_x = 0.0f;
    float gust_z = 0.0f;
    float sway_clock = 0.0f;
    float flutter_clock = 0.0f;
    bool operator==(const WindState&) const = default;
};

constexpr double k_gust_tile = 64.0;    // metres: the gust pattern repeats
constexpr double k_wind_period = 256.0; // turns: the clocks wrap
// The most a thing bends from its base, as a fraction of its height.
constexpr float k_max_sway_lean = 0.3f;

class WindClock {
public:
    // Moves the clocks on by `seconds` under `wind` (a bad number or a
    // step of over a minute: the wind is set, the clocks not moved).
    void advance(const Wind& wind, double seconds);
    WindState state() const;
    void reset() { *this = {}; }

private:
    Wind m_wind;
    double m_gust_x = 0.0;
    double m_gust_z = 0.0;
    double m_sway = 0.0;
    double m_flutter = 0.0;
};

// Gusts at a place: 0 (a lull) to 1 (the strongest).
float gust_at(const WindState& wind, double x, double z);
// The wind at a place, gusts included (m/s).
Vec3 wind_at(const WindState& wind, const DVec3& point);

// How far a point of a swaying thing moves from where it rests (metres):
// `base` is the thing's origin (its foot), `height` how tall it stands
// above that (> 0), `rest` the point; `sway` and `flutter` its material's.
// Zero at the base and for still things.
Vec3 sway_offset(const WindState& wind, const DVec3& base, float height, const DVec3& rest, float sway,
                 float flutter);
// The most sway_offset can be for a thing that tall under this wind, at
// any point and any gust (for its bounds).
float max_sway_offset(const WindState& wind, float height, float sway, float flutter);

} // namespace giant
