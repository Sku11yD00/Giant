// Dust: the clouds a collapse, a felled tree or a blow throws up. Each puff
// is a soft ball of dust that grows, rises a little, drifts with the wind
// and thins away over its life; the renderer draws the nearest ones in the
// lit image (render::DustPuff). View state, as the poses: nothing of it is
// saved, and a simulation stopping clears it.
//
//   Dust dust;
//   dust.emit(at, radius, density, color); // a collapse, a tree landing
//   dust.step(seconds, wind);              // each step simulating
//   dust.nearest(camera, 32, out);          // what to draw
//
// Work is bounded (rule 9): at most max_puffs live (a new puff takes the
// place of the faintest when full), at most emits_per_step are taken
// between steps (the rest are counted and left out), and a puff emitted
// inside a young one swells it instead of adding another (a street of
// houses falling at once is one big cloud, not a hundred).
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"

#include <vector>

namespace giant::sim {

struct DustPuff {
    DVec3 position{};  // its centre, world metres
    float radius = 1.0f;  // metres, now (the dust thins to nothing there)
    float density = 0.1f; // light it takes out per metre at the centre, now
    Vec3 color{0.55f, 0.5f, 0.44f}; // how much light its dust gives back (0 to 1)
    float age = 0.0f;  // seconds
    float life = 10.0f; // seconds it lasts
    float start_radius = 1.0f;
    float start_density = 0.1f;
};

struct DustSettings {
    u32 max_puffs = 512;
    u32 emits_per_step = 64;
    float rise = 0.4f;   // metres a second
    float drift = 0.6f;  // share of the wind's speed it moves with
    float grow = 2.0f;   // its radius grows to (1 + grow) times over its life
    float merge_age = 1.0f; // seconds a puff takes others emitted inside it
    float max_radius = 200.0f; // metres a puff swells to at most
};

struct DustStats {
    u32 puffs = 0;
    u32 emitted = 0; // in all
    u32 merged = 0;  // in all: emits that swelled a puff
    u32 dropped = 0; // in all: emits past emits_per_step, or fainter than all when full
};

// Dust thrown up from buildings and from the ground.
inline constexpr Vec3 k_dust_rubble{0.55f, 0.5f, 0.44f};
inline constexpr Vec3 k_dust_earth{0.42f, 0.37f, 0.3f};

class Dust {
public:
    explicit Dust(const DustSettings& settings = {}) : m_settings(settings) {}

    // A puff at `at`: `radius` metres, `density` per metre at its centre
    // (about 3 / radius hides what is behind its middle). Bad numbers are
    // left out. Its life follows its size (8 to 30 seconds).
    void emit(const DVec3& at, float radius, float density, const Vec3& color = k_dust_rubble);
    // Time moves on: puffs grow, rise, drift with `wind` (m/s), thin, and
    // those past their life go.
    void step(float seconds, const Vec3& wind);
    void clear();

    const std::vector<DustPuff>& puffs() const { return m_puffs; }
    // The `count` puffs nearest `camera` (its surface nearest first), into `out`.
    void nearest(const DVec3& camera, u32 count, std::vector<DustPuff>& out) const;
    DustStats stats() const;
    const DustSettings& settings() const { return m_settings; }

private:
    DustSettings m_settings;
    std::vector<DustPuff> m_puffs;
    u32 m_emits = 0; // since the last step
    u32 m_emitted = 0;
    u32 m_merged = 0;
    u32 m_dropped = 0;
};

} // namespace giant::sim
