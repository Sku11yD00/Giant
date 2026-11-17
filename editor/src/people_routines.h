// The editor's people following their routines (living-world plan LW3
// part 2). While the physics simulates, each person with a routine
// (world::Routine) is looked at in turn, a share a frame: when the entry
// that holds at the hour is another than the one it last acted on, it is
// sent where that entry says (PeopleTravel::send: along the roads by the
// clock, walked by its entity near the camera), or stopped where it is for
// one that says stay. Each person keeps its own time, late by up to
// `spread_minutes` (by its id: the same every day), so a village does not
// set off as one.
//
// Where an entry sends one: its home, the place named, or for a tag the
// place with it nearest its home (nearest where it starts, for one with no
// home), at a spot of its own within the place's radius. Places are found
// by name and by tag in lists of the scene's places sorted by a hash of
// each (no copy of them is kept: what is found is checked against the scene
// as it is, so a list out of date never gives a place without the name or
// the tag asked for; until it is made again a place may be missed, or for
// a tag one taken that is no longer the nearest).
// The lists are made again when the number of places changes, or one's
// name, tags or position (looked for 256 places a frame, so seen within
// places / 256 frames): those who could not be sent then try again, as
// they do when the roads change (PeopleTravel::builds).
//
// What is acted on is what an entry says (where, how fast), not which entry
// it is: one whose next entry sends it where it already goes is left alone.
// A person sent somewhere by hand (send_person) is not called back until
// its routine next says something else, and `stay` stops only a trip the
// routine itself began. Not followed until the next entry that says
// otherwise: a place moved after one set off for it, and spread_minutes
// changed while running (each acts on the entry its new hour gives).
//
// Bounds a frame: `looked_a_frame` people looked at, `sent_a_frame` sent
// (each a PeopleTravel::send: two nearest-node lookups; a trip sent takes
// memory for its way, a few small pieces each), and 16384 tagged places
// measured for the nearest: past that the rest wait a frame. The places
// with a tag are kept in the order of their x, so only those about as
// near as the nearest are measured, and answers are kept by home and tag,
// so those of one home ask once.
//
// Behaviours (living-world plan LW6 part 3). One with a behaviour
// (world::Behaviour) is in one of its states, the first to begin with on
// each run. When it is looked at, the state's transitions are gone through
// in order and the first whose conditions all hold is taken (one a look,
// so a chain of states takes a look each). Then the state says what it
// does: a state with its own activity is acted on as an entry is (in place
// of what its routine says, alarmed or not); one without leaves it to the
// routine (and one with no routine then stops the trip its behaviour
// began, and stands). So a person needs a routine or a behaviour to be
// followed, not both. `arrived` holds once the state has been acted on and
// no trip begun for it is still under way (one that had nowhere to go has
// arrived); `failed` once it could not be sent, or its trip found no way;
// `after` counts the simulation's seconds since the state was entered;
// `sensed` asks the senses what alarmed it most of late (calm: nothing);
// `hour` goes by the person's own hour (spread_minutes).
// A behaviour replaced while running (set_behaviour): one in a state whose
// place in the list is still there stays at that place (whatever state is
// there now), else begins again in the first.
//
// View state: not undone, not saved. Stopping the simulation puts everyone
// back (PeopleTravel), and each begins its routine afresh on the next run.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/world/scene.h"

#include <string>
#include <string_view>
#include <vector>

namespace giant::editor {

class PeopleSenses;
class PeopleTravel;

class PeopleRoutines {
public:
    PeopleRoutines();
    struct Settings {
        bool on = true;
        u32 looked_a_frame = 512;     // people looked at (1 to k_most_looked)
        u32 sent_a_frame = 32;        // of them, sent or stopped (1 to k_most_sent): the rest wait their next turn
        float spread_minutes = 20.0f; // how late the latest keeps its hours (0 to 120)
        float within = 500.0f;        // metres one and its goal may be from a road (0 to k_most_within)
    };
    Settings settings;
    static constexpr u32 k_most_looked = 8192;
    static constexpr u32 k_most_sent = 128;
    static constexpr double k_most_within = 1000.0;
    static constexpr float k_alarm_margin = 0.05f; // under an alarmed entry's `over` one stays on it by this much

    struct Stats {
        u32 following = 0; // people with a routine that is there, as last counted through
        u64 sent = 0;      // since the simulation started
        u64 stayed = 0;    // entries begun that sent nobody anywhere (stay, or there already)
        u64 failed = 0;    // entries that could not be acted on (no such place, no road near, no way found)
        u64 changed = 0;   // times one went from a state of its behaviour to another
        u32 behaving = 0;  // of `following`: people with a behaviour that is there
    };

    // Each frame, after PeopleTravel::update. `hours`: the time of day (0 to 24).
    // `senses` (null: nobody is alarmed): one alarmed does what its routine's
    // `alarmed` says for its alarm instead of what the hour says (LW5c), and
    // what the hour says again when calm: acted on as any entry is, when its
    // turn comes.
    void update(const world::Scene& scene, PeopleTravel& travel, double hours, const PeopleSenses* senses = nullptr);

    // What a person does now: the entry of its routine, or its behaviour's
    // state's own (`own`), it last acted on (one whose turn to be sent has
    // not come yet still does the last). None: neither says anything, or it
    // was not looked at yet.
    struct Doing {
        std::string_view activity; // the entry's (the scene's string: until the routines change)
        std::string_view place;    // where it was sent, or was already ("" for none, or a place gone since)
        bool failed = false;       // it could not be acted on
        bool alarmed = false;      // the entry is one of its routine's `alarmed`
        float at = 0.0f;           // the hour the entry begins, or the alarm it holds over (a state's own: 0)
        bool own = false;          // what it does is its behaviour's state's own, not its routine's
    };
    bool doing(const world::Scene& scene, world::PersonId id, Doing& out) const;
    // The state of its behaviour a person is in. None: it has no behaviour
    // that is there, or was not looked at yet (or nothing is running).
    struct Behaving {
        std::string_view behaviour; // (the scene's strings: until the behaviours change)
        std::string_view state;
        usize index = 0;      // the state's place among the behaviour's
        double seconds = 0.0; // of the simulation's, since it entered the state
    };
    bool behaving(const world::Scene& scene, const PeopleTravel& travel, world::PersonId id, Behaving& out) const;
    // What a person's routine, or its behaviour's state, has it do and
    // where, as it last acted (a frame's, for many). None: neither says
    // anything, not acted on yet, one that could not be acted on, or
    // routines off or not running.
    struct Activity {
        std::string_view activity;
        const world::Place* place = nullptr; // where it was sent (null: nowhere, or a place gone since)
        bool anywhere = false;               // it does it where it stands (`stay`)
    };
    bool activity(const world::Scene& scene, world::PersonId id, Activity& out) const;

    // What a game save keeps of a person's routine: what it last acted on,
    // so that a loaded game does not send everyone again. (What a
    // behaviour's state had it do is not kept yet: `kept` gives none.)
    struct Kept {
        u32 said = 0;          // what it acted on (never 0)
        u8 entry = 0;          // its routine's entry then
        bool failed = false;
        bool own_trip = false; // the trip it is on is the routine's
        std::string place;     // where it was sent ("" for nowhere)
    };
    // Before `kept` is asked of many (a save's): the slots as the people are now (people made or
    // removed since the last frame: an edit's cost).
    void ready(const world::Scene& scene, const PeopleTravel& travel);
    // The person at `index` of the scene's people: false when it has acted on nothing.
    bool kept(const world::Scene& scene, const PeopleTravel& travel, usize index, Kept& out) const;
    // Puts back what a person had acted on (after its trip, if any, was sent again). False: no such
    // person, or it has no routine with that entry now, or the entry says something else now.
    bool restore(const world::Scene& scene, const PeopleTravel& travel, world::PersonId id, const Kept& had);
    // While set nobody is looked at (a loaded game's people are being put back: game_saves.h).
    bool hold = false;
    // The hour a person keeps at `hours` (late by its share of spread_minutes; 0 to under 24).
    double hour_of(world::PersonId id, double hours) const;
    // Where `entry` sends `person`, or none (stay, no home, no such place).
    // A command's: for a tag, every place with it is measured.
    struct Goal {
        usize place = 0; // its index in the scene's places
        DVec3 spot{};    // the person's own spot within it
    };
    bool goal(const world::Scene& scene, const world::Person& person, const world::RoutineEntry& entry, Goal& out) const;
    Stats stats() const { return m_stats; }

private:
    struct Slot {
        world::PersonId id = 0;
        u64 trip = 0;    // the trip the routine began for it (PeopleTravel::Trip::serial); 0: none
        u64 place = 0;   // the name, hashed, of the place it was sent to (0: none)
        u32 acted = 0;   // what it last acted on (what the entry says, hashed); 0: nothing yet
        u16 routine = 0; // its routine's index in the scene's (as last found)
        u16 behaviour = k_no_behaviour; // its behaviour's index in the scene's (as last found); k_no_behaviour: none
        u32 behaviour_name = 0;         // that behaviour's name, hashed (found at another index: the same one or another)
        double entered = 0.0;           // the simulation's seconds when it entered `state`
        u8 state = 0;                   // the state of its behaviour it is in
        bool by_state = false;          // what it last acted on was a state's own (`entry`: the state then), not its routine's
        bool settled = false;           // the state it is in has been acted on (or looked at, for one that leaves it to the routine)
        u8 entry = 0;    // that routine's entry it last acted on (or the one saying the same that holds now);
                         // from the number of its entries on: one of its `alarmed`
        bool failed = false;
    };
    struct Key { // a place by the hash of its name, or of one of its tags (those of a tag by their x)
        u64 hash = 0;
        double x = 0.0; // tags: the place's, when listed
        u32 place = 0;
        bool operator<(const Key& other) const {
            if (hash != other.hash) return hash < other.hash;
            return x != other.x ? x < other.x : place < other.place;
        }
    };
    struct Near { // the place with a tag nearest a home, as last measured
        u64 tag = 0;
        u32 home = 0;
        u32 place = 0; // + 1 (0: none kept)
    };
    static constexpr u16 k_no_behaviour = 0xffff;
    // The entry a slot last acted on, by what it keeps (null: none such now).
    const world::RoutineEntry* acted_on(const world::Scene& scene, const world::Person& person, const Slot& slot) const;
    enum class Sought : u8 { none, found, later };
    u64 hash_of(std::string_view text) const;
    u32 sign_of(const world::Place& place) const;
    void index_places(const world::Scene& scene);
    void follow_places(const world::Scene& scene);
    void match_slots(const std::vector<world::Person>& people);
    void try_failed_again();
    void follow(const world::Scene& scene, const PeopleTravel& travel);
    const world::Place* sent_to(const world::Scene& scene, const Slot& slot) const;
    const world::Place* named(const world::Scene& scene, std::string_view name) const;
    const world::Place* tagged_nearest(const world::Scene& scene, std::string_view tag, const DVec3& from, i64& measured) const;
    Sought seek(const world::Scene& scene, const world::Person& person, const world::RoutineEntry& entry, Goal& out, bool in_frame);

    u64 m_scene = 0;
    u64 m_resets = 0;
    bool m_running = false;
    std::vector<Slot> m_slots, m_slots_before; // by the person's place in the scene's list (ids rising)
    usize m_cursor = 0;
    u32 m_counted = 0;
    u32 m_behaving_counted = 0;
    bool m_miscounted = false; // people came or went while this pass counted: its count is not given out
    u64 m_builds = 0;          // PeopleTravel::builds as last seen
    // The scene's places by name and by tag, and what each was when they were listed.
    u64 m_seed = 0;
    std::vector<Key> m_by_name, m_by_tag;
    std::vector<u32> m_signs;
    std::vector<Near> m_near;
    bool m_indexed = false;
    usize m_place_cursor = 0;
    i64 m_measure_left = 0; // tagged places that may still be measured this frame
    Stats m_stats;
};

} // namespace giant::editor
