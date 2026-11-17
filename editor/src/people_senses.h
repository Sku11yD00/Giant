// What the editor's people hear (living-world plan LW5a). While the physics
// simulates, a noise made somewhere (a shot's report, a bullet striking, a
// blast, a splash, one made by command) spreads at the speed of sound and
// is heard by every person it is loud enough for where the person is, near
// the camera or a kilometre off: its level falls with the distance (as the
// audio's does: `loudness` is the level at 1 m, a rifle's report about
// 100), `hear_loudness` is the quietest anyone notices and `alarm_loudness`
// what alarms one fully. What one heard raises its alarm (0 to 1), which
// eases back to nothing over `calm_seconds`.
//
// Nothing here is unbounded a frame: at most k_max_noises are kept (the
// quietest gives way), `looked_a_frame` people are looked at a frame while
// any noise is spreading (none when it is quiet), and what each heard is
// kept in a slot beside the scene's people (no memory taken as they hear).
// Each time through the list a person hears the noises that reached it
// since the time before, so each hears each once; the tests of a person
// against a noise a frame are bounded too (k_tests_a_frame: with many
// noises spreading fewer people are looked at).
//
// Known limits: where a noise has reached is worked out from where a
// person is when looked at, so one moving fast towards a noise may miss it
// and one moving away hear it twice (about its speed over sound's: a
// walker's chance under half a percent); nothing stands in a sound's way (walls and hills let it
// through; the listener's ears do bend and muffle, audio_state.h); the
// weather does not carry or drown it; what people do about it is LW5c.
//
// View state: not undone, not saved.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/world/scene.h"

#include <string_view>
#include <vector>

namespace giant::editor {

class PeopleTravel;

class PeopleSenses {
public:
    struct Settings {
        bool on = true;
        float hear_loudness = 0.2f;  // the level at its ears below which nobody notices (0.01 to 100)
        float alarm_loudness = 2.0f; // the level that alarms one fully (at least hear_loudness; to 1000)
        float calm_seconds = 30.0f;  // seconds a full alarm takes to ease to nothing (1 to 3600)
        u32 looked_a_frame = 4096;   // people looked at a frame while a noise spreads (1 to 16384)
    };
    Settings settings;
    bool connected = false; // connect_senses was called: shots, strikes, blasts and splashes make noises

    enum class Kind : u8 { shot, impact, blast, splash, voice, other };
    static const char* to_string(Kind kind);
    static bool from_string(std::string_view text, Kind& out);

    static constexpr usize k_max_noises = 64;
    static constexpr double k_sound_speed = 343.0;  // metres a second
    static constexpr double k_max_reach = 20000.0;  // metres a noise is followed to, however loud
    static constexpr float k_max_loudness = 10000.0f;
    static constexpr u32 k_max_looked = 16384;
    static constexpr usize k_tests_a_frame = 262144; // of a person against a noise

    PeopleSenses() { m_noises.reserve(k_max_noises); }

    // A noise made at `at` now, `loudness` its level at 1 m. False: not
    // simulating, off, not numbers, too quiet for anyone, or as many
    // louder ones are spreading as are kept. `reach`: the metres it is
    // heard within. Made by the clock of the last update or sync: sync
    // first when time may have been simulated since (a command's, a
    // physics step's).
    bool noise(const DVec3& at, float loudness, Kind kind, double* reach = nullptr);
    // A blast's level at 1 m from the push it gives (PhysicsState::on_impact's impulse).
    static float blast_loudness(float impulse);

    // Each frame, after the travel's update (its clock and where everyone is).
    void update(const world::Scene& scene, const PeopleTravel& travel);
    // The clock alone brought up to the travel's (nobody is looked at): a
    // run begun or time stepped on since the last frame counts for the
    // noises made and what is reported.
    void sync(const world::Scene& scene, const PeopleTravel& travel);

    // What a person last heard that alarmed it most.
    struct Heard {
        float alarm = 0.0f;   // as it is now (0: calm)
        Kind kind = Kind::other;
        DVec3 from{};         // where the noise was made
        float level = 0.0f;   // at its ears
        double seconds = 0.0; // since it heard that one
    };
    // False: calm (it heard nothing, or has calmed since).
    bool heard(const world::Scene& scene, world::PersonId id, Heard& out) const;
    float alarm(const world::Scene& scene, world::PersonId id) const;

    struct Stats {
        u32 noises = 0;  // spreading now
        u64 made = 0;    // since the simulation started
        u64 dropped = 0; // refused or pushed out: more spreading than are kept
        u64 heard = 0;   // times someone heard one
    };
    const Stats& stats() const { return m_stats; }
    void clear();
    void silence(); // the noises spreading go (what each heard stays)

private:
    struct Noise {
        DVec3 at{};
        float loudness = 0.0f;
        Kind kind = Kind::other;
        double made = 0.0;  // the clock then
        double reach = 0.0; // metres it is heard within
    };
    struct Slot {
        world::PersonId id = 0;
        float alarm = 0.0f; // as it was at `at` (it eases from then)
        float level = 0.0f;
        Kind kind = Kind::other;
        double at = 0.0;       // when it last heard anything
        double heard_at = 0.0; // when it heard the one remembered
        DVec3 from{};
    };
    void match_slots(const std::vector<world::Person>& people);
    float eased(const Slot& slot) const;

    u64 m_scene = 0;
    u64 m_resets = 0;
    bool m_running = false;
    double m_seconds = 0.0;      // the travel's clock
    std::vector<Noise> m_noises; // at most k_max_noises
    std::vector<Slot> m_slots, m_slots_before; // beside the scene's people (their ids rising)
    static constexpr usize k_done = ~usize{0};
    usize m_cursor = k_done;     // in the people (k_done: through the list, the next time has yet to begin)
    double m_heard_to = 0.0;     // the clock this time through the list hears to
    double m_heard_from = 0.0;   // and from (the time before's)
    Stats m_stats;
};

} // namespace giant::editor
