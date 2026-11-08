// Swinging things: lanterns, signs, bells, lamps on chains
// (world::SwingSettings) that the wind swings about their pivots.
//
//   Swings swings;
//   swings.set(id, entity.swing, entity.transform, size); // when it changes
//   swings.step(seconds, wind, camera);                   // each frame
//   swings.push(at, radius, velocity);                    // a gust of a blast, a hand
//   swings.blast(at, radius, impulse);                    // a blast, outwards
//   swings.knock(id, impulse);                            // a bullet striking it
//   const world::Transform* pose = swings.pose(id);       // where to draw it
//
// Each is a pendulum: its origin (the thing's middle) on a rod of `length`
// metres from a pivot straight above where it rests, pulled down by
// gravity and pushed by the wind on its sides (a box of `size` metres,
// turned as it is; the push grows with the wind's speed squared, so a storm
// throws a lantern about far harder than a breeze), slowed by its pivot. It
// feels the same gusts the trees show (giant/core/wind.h: wind_at). A hinged
// one (world::SwingHinge::x) swings only about its own x axis (made level).
// Drawn turned about the pivot: no mesh of its own, so any shape swings.
//
// Work is bounded (rule 9): at most max_simulated move a frame (the
// nearest the camera within `distance`; the rest hold their last pose),
// and at most four times that many are looked at (sleepers waking), in
// fixed steps of `step_seconds`, at most max_steps a frame (time past that
// is dropped). One that has come to rest (no wind changing about it,
// nothing knocking it) sleeps until something does. View state: nothing of
// it is saved.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/core/wind.h"
#include "giant/world/scene.h"

#include <map>
#include <unordered_map>
#include <vector>

namespace giant::sim {

struct SwingSettingsSim {
    u32 max_simulated = 512;    // swings that move a frame, nearest first
    double distance = 100.0;    // metres from the camera: farther ones hold still
    float step_seconds = 1.0f / 60.0f;
    u32 max_steps = 4;          // a frame
    float gravity = 9.81f;      // m/s²
    float air_density = 1.2f;   // kg/m³
};

struct SwingStats {
    u32 swings = 0;
    u32 simulated = 0; // last step: swings that moved
    u32 asleep = 0;    // last step: within reach but resting
    u32 steps = 0;     // last step: fixed steps taken, the most any took
    u64 resets = 0;    // in all: put back at rest after going wrong (numbers not finite)
};

class Swings {
public:
    struct Swing {
        world::SwingSettings settings; // clamped
        world::Transform rest;         // as the scene has it
        Vec3 size{1.0f, 1.0f, 1.0f};   // metres: the box the wind pushes on
        DVec3 pivot{};
        Vec3 hinge{};                  // the level hinge axis (x hinges; 0: swings any way)
        Vec3 bob{};                    // from the pivot to where its origin is now
        Vec3 velocity{};               // of its origin, m/s
        Quat turn{};                   // from hanging at rest to as it hangs now
        world::Transform pose;         // where it is drawn
        bool asleep = false;
        u32 still_steps = 0;
        Vec3 slept_wind{};             // the wind it fell asleep in
        float time = 0.0f;             // seconds of steps not yet taken
        Vec3 drawn_bob{};              // bob as last posed
        bool queued = false;           // in the moved list
        // Degrees it hangs from straight down now.
        float angle() const;
    };

    explicit Swings(const SwingSettingsSim& settings = {}) : m_settings(settings) {}

    // Adds it or follows changes to it: new settings put it back at rest
    // (hanging straight); a new place or size keeps it swinging as it was
    // about its new pivot. Bad numbers are clamped. Its pose changes now.
    void set(world::EntityId id, const world::SwingSettings& settings, const world::Transform& rest,
             const Vec3& size);
    void remove(world::EntityId id);
    void clear();
    // Time moves on: the nearest in reach move under `wind`.
    void step(double seconds, const WindState& wind, const DVec3& camera);
    // Every swing whose origin is within `radius` metres of `at` gains
    // `velocity` (m/s, world), less towards the edge; or, for blast, takes
    // `impulse` (N s, less towards the edge) away from `at`: its speed
    // changes by that over its weight, at most max_knock. Wakes what it
    // reaches. Returns how many.
    usize push(const DVec3& at, float radius, const Vec3& velocity);
    usize blast(const DVec3& at, float radius, float impulse);
    // A blow on one (a bullet's impulse, N s, world): its speed changes by
    // impulse over its weight, at most max_knock m/s. False: no such swing.
    bool knock(world::EntityId id, const Vec3& impulse);
    float max_knock = 20.0f;

    const Swing* find(world::EntityId id) const;
    // Where each is drawn (rest turned about its pivot).
    const std::unordered_map<world::EntityId, world::Transform>& poses() const { return m_poses; }
    // Those whose pose changed since the last call (set, stepped, removed), by id.
    std::vector<world::EntityId> take_moved();
    SwingStats stats() const;
    const SwingSettingsSim& settings() const { return m_settings; }
    SwingSettingsSim& settings() { return m_settings; }

private:
    void place(world::EntityId id, Swing& s); // pose from bob, noted as moved
    void wake(Swing& s) {
        s.asleep = false;
        s.still_steps = 0;
    }

    SwingSettingsSim m_settings;
    std::map<world::EntityId, Swing> m_swings;
    std::unordered_map<world::EntityId, world::Transform> m_poses;
    std::vector<world::EntityId> m_moved;
    std::vector<std::pair<double, world::EntityId>> m_near; // scratch
    SwingStats m_stats;
};

// The settings as the simulation takes them (bad numbers clamped to their
// ranges in world::SwingSettings).
world::SwingSettings clean(const world::SwingSettings& settings);

// The shapes that swing when their settings are on (not decals or cloth).
bool can_swing(world::Shape shape);

} // namespace giant::sim
