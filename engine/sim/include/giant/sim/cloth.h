// Cloth: hanging sheets (world::Shape::cloth) that the wind blows about.
// Each sheet is a grid of points joined by springs (position-based
// dynamics: Verlet steps, then the springs' lengths put back a few times),
// held where it is pinned, pulled down by gravity and pushed by the wind
// on its face (the push grows with the wind's speed squared, so a storm
// throws a flag about far harder than a breeze), with ripples running away
// from what holds it, faster the harder it blows. It feels the same gusts
// the trees show (giant/core/wind.h: wind_at).
//
//   Cloths cloths;
//   cloths.set(id, entity.cloth, entity.transform); // each frame, or when it changes
//   cloths.step(seconds, wind, camera);             // each frame
//   cloths.push(at, radius, velocity);              // a blast, a bullet
//   const Cloths::Sheet* s = cloths.find(id);       // s->skin(): its pose to draw
//
// A sheet is simulated in its own space (its entity's x-y plane, hanging
// from its origin; wind and gravity turned into it), so moving its entity
// moves it whole. Its pose is one matrix per grid point (a skinned mesh's
// joints: giant/render/renderer.h, add_skinned_mesh), from where the point
// rests to where it is and how its little patch of cloth turns.
//
// Work is bounded (rule 9): at most max_simulated sheets move a frame (the
// nearest the camera within `distance`; the rest hold their last pose),
// each in fixed steps of `step_seconds`, at most max_steps a frame (time
// past that is dropped, so a slow frame never snowballs), and at most
// k_max_cloth_points points a sheet. A sheet that has stopped moving (no
// wind, nothing pushing it) sleeps until something moves it. View state,
// as the dust: nothing of it is saved.
#pragma once

#include "giant/core/jobs.h"
#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/core/wind.h"
#include "giant/world/scene.h"

#include <map>
#include <vector>

namespace giant::sim {

// The most points across and down a sheet (32 by 32: a skinned mesh's
// joint limit, 1024); finer cloth uses more of them, up to that.
inline constexpr u32 k_max_cloth_side = 32;
inline constexpr u32 k_max_cloth_points = k_max_cloth_side * k_max_cloth_side;
// The spacing aimed for between points (metres); a sheet gets at least 3
// a side.
inline constexpr float k_cloth_spacing = 0.15f;

struct ClothSettingsSim {
    u32 max_simulated = 32;     // sheets that move a frame, nearest first
    u32 max_points = 8192;      // and their points, in all (the nearest sheet always moves)
    double distance = 60.0;     // metres from the camera: farther ones hold still
    float step_seconds = 1.0f / 60.0f;
    u32 max_steps = 4;          // a frame
    u32 iterations = 6;         // the springs put back, a step
    float gravity = 9.81f;      // m/s²
    float air_density = 1.2f;   // kg/m³
};

struct ClothStats {
    u32 sheets = 0;
    u32 simulated = 0; // last step: sheets that moved
    u32 asleep = 0;    // last step: within reach but resting
    u32 steps = 0;     // last step: fixed steps taken
    u64 resets = 0;    // in all: sheets put back at rest after going wrong (numbers not finite)
};

class Cloths {
public:
    class Sheet {
    public:
        explicit Sheet(const world::ClothSettings& settings);

        const world::ClothSettings& settings() const { return m_settings; }
        u32 columns() const { return m_columns; }
        u32 rows() const { return m_rows; }
        // Point (column c, row r) is index r * columns + c; row 0 is the
        // top. Where each rests (x across from -width/2, y down from 0,
        // z 0, facing +z) and where it is now, in the sheet's space.
        const std::vector<Vec3>& rest() const { return m_rest; }
        const std::vector<Vec3>& points() const { return m_points; }
        bool pinned(u32 index) const { return m_inverse_mass[index] == 0.0f; }
        // Its pose: a matrix per point, from where it rests to where it
        // is, turned as its patch of cloth is (rest normal +z), and a
        // sphere around it (centre, radius) in the sheet's space.
        void skin(std::vector<Mat4>& out) const;
        Vec3 bounds_center() const { return m_center; }
        float bounds_radius() const { return m_radius; }
        // Changes whenever its points move (never the same twice).
        u64 version() const { return m_version; }
        bool asleep() const { return m_asleep; }

    private:
        friend class Cloths;
        struct Spring {
            u32 a = 0;
            u32 b = 0;
            float length = 0.0f;
            // Shares of the error each end takes back each time: the
            // spring's stiffness split by the ends' inverse masses.
            float share_a = 0.5f;
            float share_b = 0.5f;
        };
        struct Tether {
            u32 pin = 0;   // a pinned point
            u32 point = 0; // a free one, kept within `length` (a hair over) of it
            float length = 0.0f;
        };
        // One fixed step under gravity and wind (both in the sheet's space).
        // Returns the most any point moved.
        float advance(float dt, const Vec3& gravity, const Vec3& wind, float ripple_clock, float air_density,
                      u32 iterations);
        void normals();
        void bound();
        void reset();
        void wake() { m_asleep = false; m_still_steps = 0; }

        world::ClothSettings m_settings;
        u32 m_columns = 0;
        u32 m_rows = 0;
        float m_point_area = 0.0f; // square metres a point stands for
        std::vector<Vec3> m_rest;
        std::vector<Vec3> m_points;
        std::vector<Vec3> m_previous;
        std::vector<Vec3> m_normals;
        std::vector<float> m_inverse_mass;
        std::vector<float> m_from_pin; // metres from what holds it (ripples run away from it)
        std::vector<Spring> m_springs;
        std::vector<Tether> m_tethers;
        Vec3 m_center{};
        float m_radius = 0.0f;
        u64 m_version = 1;
        bool m_asleep = false;
        u32 m_still_steps = 0;
        float m_time = 0.0f; // seconds of steps not yet taken
    };

    explicit Cloths(const ClothSettingsSim& settings = {}) : m_settings(settings) {}

    // Adds the sheet or follows changes to it: new settings make it again
    // (at rest); a new place moves it whole. Bad numbers are clamped.
    void set(world::EntityId id, const world::ClothSettings& settings, const world::Transform& place);
    void remove(world::EntityId id);
    void clear();
    // Time moves on: the nearest sheets in reach move under `wind`.
    // With `jobs`, sheets step on its workers.
    void step(double seconds, const WindState& wind, const DVec3& camera, JobSystem* jobs = nullptr);
    // A blast or a bullet: every point within `radius` metres of `at`
    // (world) gains `velocity` (m/s, world), less towards the edge. Wakes
    // what it reaches.
    void push(const DVec3& at, float radius, const Vec3& velocity);
    // A blast: the same, but each point gains speed away from `at`: the
    // blow's `impulse` (N s) over the sheet's weight (at most 30 m/s), less
    // towards the edge.
    void blast(const DVec3& at, float radius, float impulse);

    const Sheet* find(world::EntityId id) const;
    const std::map<world::EntityId, Sheet>& sheets() const { return m_sheets; }
    ClothStats stats() const;
    const ClothSettingsSim& settings() const { return m_settings; }

private:
    ClothSettingsSim m_settings;
    std::map<world::EntityId, Sheet> m_sheets;
    std::map<world::EntityId, world::Transform> m_places;
    std::vector<std::pair<double, world::EntityId>> m_near; // scratch
    std::vector<Sheet*> m_moving;                             // scratch
    std::vector<Vec3> m_wind;                                 // scratch: each moving sheet's, in its space
    std::vector<Vec3> m_down;                                 // scratch: gravity, in its space
    std::vector<u32> m_steps;                                 // scratch: steps each took
    std::vector<u8> m_broke;                                  // scratch: each went wrong
    ClothStats m_stats;
};

// The settings as the simulation takes them (bad numbers clamped to their
// ranges in world::ClothSettings).
world::ClothSettings clean(const world::ClothSettings& settings);

} // namespace giant::sim
