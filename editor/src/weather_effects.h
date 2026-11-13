// The weather's particles near the camera (effects plan FX4b): rain
// splashing on what it falls on (the first surface under it: a roof, a deck,
// a slab, else the terrain; not on water, nor on snow), and in a storm
// leaves and grit blown along the ground (not where something is overhead;
// grit only off dry ground). Given each frame to EffectsState within
// the weather's own budgets (EffectsState::add_burst), on the game clock
// (paused: none), deterministic (each from a hash of a count), nothing made
// on the frame path.
#pragma once

#include "effects_state.h"

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/core/wind.h"
#include "giant/world/scene.h"
#include "giant/world/weather.h"

#include <functional>
#include <optional>

namespace giant::sim {
class PhysicsWorld;
}

namespace giant::editor {

class WeatherEffects {
public:
    static constexpr double k_splash_radius = 20.0;   // metres round the camera
    static constexpr double k_splash_rate = 600.0;    // a second in the hardest rain
    static constexpr u32 k_max_splashes = 64;         // a frame (each a ray down: the first surface)
    static constexpr double k_debris_radius = 25.0;   // metres round a point upwind of the camera
    static constexpr double k_upwind = 12.0;          // metres upwind of the camera (blown past it)
    static constexpr float k_storm_wind = 8.0f;       // m/s: none below
    static constexpr float k_gale_wind = 20.0f;       // m/s: the most from here up
    static constexpr double k_leaf_rate = 40.0;       // a second in a gale
    static constexpr double k_grit_rate = 120.0;      // a second in a gale, off dry bare ground
    static constexpr u32 k_max_debris = 16;           // a frame, of each

    static constexpr double k_splash_top = 100.0;     // metres over the camera (or the ground there) rain is looked down from
    static constexpr double k_splash_under = 30.0;    // metres the ground may be over the camera (else in a tunnel: none)
    static constexpr double k_splash_over = 100.0;    // metres the camera may be over the ground (else too high to see them)
    static constexpr double k_debris_top = 3.0;       // metres over the terrain a storm's debris looks down from (a slab's top)

    struct Stats {
        u32 splashes = 0;  // given this frame
        u32 on_water = 0;  // splashes left out: water there (its own rings, W5c)
        u32 leaves = 0;
        u32 grit = 0;
        u32 sheltered = 0; // leaves and grit left out: something overhead (indoors)
        u32 cut = 0;       // left out by the budgets
    };

    // The ground's height at (x, z), or none.
    using Ground = EffectsState::Ground;
    // The water's level at (x, z), or none.
    using Water = std::function<std::optional<double>(double x, double z)>;
    // The first surface under (x, from_y, z) within `distance` (none: nothing there).
    using Surface = std::function<std::optional<double>(double x, double z, double from_y, double distance)>;
    // Whether something is over this point (indoors, under a bridge).
    using Sheltered = std::function<bool(const DVec3& point)>;

    // `seconds` of the game clock (0: paused, none). `shown`: the weather
    // shown (none: the default's, which neither rains nor storms).
    void update(EffectsState& effects, const std::optional<world::Weather>& shown, const world::Ground& ground_state,
                const WindState& wind, const DVec3& camera, double seconds, const Ground& ground, const Water& water,
                const Surface& surface, const Sheltered& sheltered);
    const Stats& stats() const { return m_stats; }

    // How hard it rains (not snows), 0 to 1, as the weather draws it
    // (world::falling: all rain from 2 degrees, all snow at -1 and below).
    static float rain(const world::Weather& w);
    // How much of a storm the wind makes, 0 below k_storm_wind to 1 at k_gale_wind.
    static float storm(const WindState& wind);

    // The probes from the physics' world (main's; tests'): the first surface
    // a ray down meets, a ray up meeting anything, the water's level.
    static std::optional<double> surface_in(const sim::PhysicsWorld& physics, double x, double z, double from_y,
                                            double distance);
    static bool sheltered_in(const sim::PhysicsWorld& physics, const DVec3& point);
    static std::optional<double> water_in(const sim::PhysicsWorld& physics, double x, double z);

private:
    double m_splash_carry = 0.0;
    double m_leaf_carry = 0.0;
    double m_grit_carry = 0.0;
    u64 m_count = 0; // bursts given, all told (each one's seed)
    Stats m_stats;
};

} // namespace giant::editor
