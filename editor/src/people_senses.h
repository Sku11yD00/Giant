// What the editor's people hear and see (living-world plan LW5a, LW5b). While the physics
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
// Sight (LW5b): things to be seen are named and put somewhere (`watch`:
// the player while playing, others by command). A person sees one that is
// within `see_distance` of its eyes, within `see_angle` about the level
// way it faces (or within arm's reach, whichever way it faces) and with
// nothing between (`blocked`: a ray through the physics and the terrain,
// at most `rays_a_frame` of them a frame, none where seeing would change
// nothing; `seen_a_frame` people are looked at a frame while anything is
// watched, or anyone's alarm is still held by what it saw). Seeing one alarms as near as it is (fully within a fifth of
// see_distance, a tenth at its end) and holds the alarm there until the
// person is looked through again and no longer sees it.
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

#include <functional>
#include <optional>
#include <string>
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
        u32 looked_a_frame = 512;    // people looked at a frame while a noise spreads (1 to 16384): each costs
                                     // where it is on its trip, about 0.15 microseconds; 5,000 people are gone
                                     // through in ten frames, and each still hears each noise once (up to two
                                     // times through late: 65,536 people want it raised)
        bool sight = true;
        float see_distance = 60.0f;  // metres from its eyes a person sees a watched thing within (1 to 300)
        float see_angle = 120.0f;    // degrees across, about the way it faces (10 to 360)
        u32 seen_a_frame = 1024;     // people looked at a frame while anything is watched (1 to 16384)
        u32 rays_a_frame = 32;       // rays cast a frame for what stands between (1 to 128)
    };
    Settings settings;
    bool connected = false; // connect_senses was called: shots, strikes, blasts and splashes make noises

    enum class Kind : u8 { shot, impact, blast, splash, voice, other, sight }; // (sight: a watched thing seen, not a noise)
    static const char* to_string(Kind kind);
    static bool from_string(std::string_view text, Kind& out);

    static constexpr usize k_max_noises = 64;
    static constexpr double k_sound_speed = 343.0;  // metres a second
    static constexpr double k_max_reach = 20000.0;  // metres a noise is followed to, however loud
    static constexpr float k_max_loudness = 10000.0f;
    static constexpr u32 k_max_looked = 16384;
    static constexpr usize k_max_watched = 16;
    static constexpr double k_max_see = 300.0;
    static constexpr u32 k_max_rays = 128;
    static constexpr usize k_max_name = 64;       // a watched thing's name, bytes
    static constexpr double k_eye_height = 1.6;   // a person's eyes above where it stands
    static constexpr double k_arms_reach = 1.5;   // metres within which one is seen whichever way the person faces
    static constexpr usize k_tests_a_frame = 262144; // of a person against a noise

    PeopleSenses() {
        m_noises.reserve(k_max_noises);
        m_watched.reserve(k_max_watched);
    }

    // A noise made at `at` now, `loudness` its level at 1 m. False: not
    // simulating, off, not numbers, too quiet for anyone, or as many
    // louder ones are spreading as are kept. `reach`: the metres it is
    // heard within. Made by the clock of the last update or sync: sync
    // first when time may have been simulated since (a command's, a
    // physics step's).
    bool noise(const DVec3& at, float loudness, Kind kind, double* reach = nullptr);
    // A blast's level at 1 m from the push it gives (PhysicsState::on_impact's impulse).
    static float blast_loudness(float impulse);

    // A thing the people may see, at `at` (its middle or its eyes), by its
    // name: put there, or moved when it is watched already. False: not
    // simulating, off, not numbers, no name or too long a one, or
    // k_max_watched are watched already.
    // `clearance`: metres short of `at` the look stops (0 to 50): a
    // thing's own body, about its half size, so that it does not hide itself.
    bool watch(std::string_view name, const DVec3& at, float clearance = 0.0f);
    bool unwatch(std::string_view name);
    struct Watched {
        std::string name;
        DVec3 at{};
        float clearance = 0.0f;
    };
    // The player's eyes while playing (set each frame; none: not playing):
    // seen as the things shown are, in a place of its own.
    std::optional<DVec3> player;
    const std::vector<Watched>& watched() const { return m_watched; }
    // Whether something stands between two points (null: nothing ever does).
    std::function<bool(const DVec3& from, const DVec3& to)> blocked;

    // Each frame, after the travel's update (its clock and where everyone is).
    void update(const world::Scene& scene, const PeopleTravel& travel);
    // The clock alone brought up to the travel's (nobody is looked at): a
    // run begun or time stepped on since the last frame counts for the
    // noises made and what is reported.
    void sync(const world::Scene& scene, const PeopleTravel& travel);

    // What a person last heard or saw that alarmed it most.
    struct Heard {
        float alarm = 0.0f;   // as it is now (0: calm)
        Kind kind = Kind::other;
        DVec3 from{};         // where the noise was made (or the thing seen was)
        float level = 0.0f;   // at its ears (seen: the alarm it gave)
        double seconds = 0.0; // since it heard that one (or last saw it)
    };
    // False: calm (it heard nothing, or has calmed since).
    bool heard(const world::Scene& scene, world::PersonId id, Heard& out) const;
    float alarm(const world::Scene& scene, world::PersonId id) const;
    // The same for the person at `index` of the scene's people (a frame's, for many: no search
    // while the slots lie beside the people).
    float alarm_at(const world::Scene& scene, usize index) const;

    struct Stats {
        u32 noises = 0;  // spreading now
        u64 made = 0;    // since the simulation started
        u64 dropped = 0; // refused or pushed out: more spreading than are kept
        u64 heard = 0;   // times someone heard one
        u64 seen = 0;    // times someone saw a watched thing
        u64 rays = 0;    // cast for it
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
        float held = 0.0f;  // by what it sees now: its alarm is not under this (0: it sees nothing)
        float level = 0.0f;
        Kind kind = Kind::other;
        double at = 0.0;       // when it last heard anything
        double heard_at = 0.0; // when it heard the one remembered
        DVec3 from{};
    };
    void match_slots(const std::vector<world::Person>& people);
    void hear(const std::vector<world::Person>& people, const PeopleTravel& travel);
    void look(const world::Scene& scene, const PeopleTravel& travel);
    float eased_alone(const Slot& slot) const; // but for what it sees now
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
    std::vector<Watched> m_watched; // at most k_max_watched
    usize m_look = 0;            // in the people: the next whose eyes are looked through
    usize m_look_thing = 0;      // and the thing it was at when the frame's rays ran out (0: the player's place)
    float m_look_best = 0.0f;    // with what it had seen by then,
    DVec3 m_look_at{};           // and where
    usize m_holding = 0;         // people whose alarm is held by what they see
    bool m_look_player = false;  // the player was there to be seen at the last look
    Stats m_stats;
};

} // namespace giant::editor
