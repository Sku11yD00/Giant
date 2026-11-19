// The editor's people travelling (living-world plan LW2b). While the
// physics simulates, a person sent somewhere (send_person) goes there along
// the world's roads and paths (world::Routes, nav/route.h): the way is
// looked for over frames (one shared graph, one search at a time in its own
// lists, a budget a frame: many sent at once to places apart wait their
// turn, a search each), and the person then moves along it by the
// simulation's clock at a walking speed. Where one is at any time is worked
// out from when it set off, so thousands travelling cost nothing a frame
// but the few looked at for having arrived.
//
// The scene keeps where each person starts (world::Person); where one who
// has travelled is now is kept here (`where`), and is the simulation's:
// stopping the simulation (or another scene) puts everyone back.
//
// A person given an entity near the camera (people_state.h) is held
// (`hold`): its trip then goes on only as far as the entity has walked
// (`walked`: PeopleState walks it along the way as a walker), and by the
// clock again from there once the entity goes.
//
// View state: not undone, not saved.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/nav/route.h"
#include "giant/world/scene.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace giant::editor {

class PhysicsState;

class PeopleTravel {
public:
    struct Settings {
        u32 search_work_a_frame = 4096; // of route search (nav::RouteSearch::advance's count; 256 to 1 << 20)
        u32 looked_a_frame = 1024;      // trips looked at for having arrived (1 to 65536)
    };
    Settings settings;

    enum class Phase : u8 { searching, travelling, failed };
    static const char* to_string(Phase phase);

    struct Trip {
        world::PersonId person = 0;
        u64 serial = 0; // each trip's own (one sent again has another)
        Phase phase = Phase::searching;
        DVec3 to{};        // where it is going
        std::string place; // the place asked for by name, if one was
        double speed = 1.4; // m/s
        double within = 500.0; // metres it and its goal might be from a road when it was sent
        float yaw = 0.0f;   // degrees: the way it faced when it set off (kept where its way has no length)
        world::RouteNodeId first = 0, last = 0; // the nodes it joins and leaves the roads at
        std::vector<DVec3> points; // travelling: the way, from where it set off to `to`
        std::vector<double> along; // metres to each point
        double from = 0.0;  // metres along at `since`
        double since = 0.0; // the clock then
        bool held = false;  // an entity stands for it: it goes on as that walks (PeopleState)
        const char* reason = ""; // failed: why
        u32 person_at = 0;  // where its person was last found in the scene's list (tried first: no search while nobody is added or removed)
    };
    struct Where {
        DVec3 position{};
        float yaw = 0.0f; // degrees
        bool operator==(const Where&) const = default;
    };
    struct Stats {
        u32 trips = 0;     // searching, travelling and failed
        u32 searching = 0; // waiting for their way
        u32 moved = 0;     // people sent since the simulation started: where each is is kept here
        u64 arrived = 0;   // since the simulation started
        u64 failed = 0;
        u64 searches = 0;
        u64 opened = 0; // route nodes the searches that came to an end looked at
        u64 reused = 0; // ways taken from one found before between the same nodes (no search)
    };

    static constexpr usize k_max_trips = 65536; // (a scene's most people)
    static constexpr double k_join_within = 4096.0; // metres a person or a goal may be from a road

    // Each frame, before the people near the camera are given entities.
    // `run`: the physics' run now (PhysicsState::runs), 0 while it does not
    // simulate; `seconds`: the time simulated in it. Another run or scene:
    // everyone is back where the scene has them.
    void update(const world::Scene& scene, u64 run, double seconds);
    // The clock alone brought up to now (a command's, between frames: time
    // stepped on since the last frame counts for what it does).
    void sync(const world::Scene& scene, u64 run, double seconds);
    // The run and the seconds the physics gives both (the frame's and a command's clock are this one).
    struct Clock {
        u64 run = 0;
        double seconds = 0.0;
    };
    static Clock clock_of(const PhysicsState& physics);
    bool running() const { return m_run != 0; }
    double seconds() const { return m_seconds; }

    // Sends a person to `to` at `speed` (m/s), `within` metres being how
    // far from a road it and the goal may be (one going along the roads, or
    // stopped part way along them, may be on a long stretch far from its
    // ends: it joins at the nearest node within k_join_within). Empty: on its way (or its way
    // is being looked for); else why not. The graph is built when a run
    // starts, and here when the routes have changed since (a command's
    // cost; in a frame only for the first sent after another scene was
    // opened while running). `in_frame`
    // (routines send many a frame): the routes are taken as `update` last
    // saw them (it compares a stretch of them a frame, so a node moved is
    // seen a few frames late), and why not is a few words.
    std::string send(const world::Scene& scene, world::PersonId id, const DVec3& to, float speed, double within,
                     std::string_view place = {}, bool in_frame = false);
    // Ends a person's trip where it is. False: it had none.
    bool stop(world::PersonId id);
    // Puts a person at `at` (a loaded game's: where it had got to), ending
    // any trip it has. False: not simulating, or no such person.
    bool put(const world::Scene& scene, world::PersonId id, const Where& at);
    // Those kept somewhere they travelled to or were put (those on a trip too: where they set off).
    const std::unordered_map<world::PersonId, Where>& moved() const { return m_places; }
    void clear();

    // Where a person is now.
    Where where(const world::Person& person) const;
    const Trip* trip(world::PersonId id) const;
    // Metres a trip has gone and has in all (travelling).
    double gone(const Trip& trip) const;
    static double length(const Trip& trip) { return trip.along.empty() ? 0.0 : trip.along.back(); }
    // An entity stands for the person (its trip goes on only as `walked`
    // says: people_state.h walks it along), or no longer.
    void hold(world::PersonId id, bool held);
    // A held person's entity has walked to `metres` along its way (never back).
    void walked(world::PersonId id, double metres);
    // A held person's entity is at its trip's end, standing at `there`: the trip is over.
    void arrive(world::PersonId id, const Where& there);
    // Where a trip is `metres` along its way, facing along it.
    static Where at(const Trip& trip, double metres);
    // Counts the times everyone was put back (another run or scene): one
    // made an entity before is not where that entity stands.
    u64 resets() const { return m_resets; }
    // Counts the times someone's place changed other than by travelling: put somewhere (`put`: a
    // loaded game's people), or forgotten with its person gone (an undo brings it back where the
    // scene has it). Between two of these and of `resets`, nobody is anywhere it did not travel
    // to, at `fastest` at most.
    u64 puts() const { return m_puts; }
    // The highest speed (m/s) anyone was sent at since everyone was last put back: 0, nobody was.
    double fastest() const { return m_fastest; }
    static constexpr float k_max_speed = 100.0f; // m/s a trip may be sent at
    // Counts the times the graph was built (the roads changed: one with no road near may have one now).
    u64 builds() const { return m_builds; }
    Stats stats() const;

private:
    Trip* find(world::PersonId id);
    void end(usize index, bool arrived);
    void fail(Trip& trip, const char* reason);
    void found(const world::Scene& scene, Trip& trip, const std::vector<u32>& path);
    // A way found before from one node to another, kept for the next going the same (most go
    // where others go: a village to its mill): its nodes are copied into each trip that takes
    // it. Forgotten when the graph is built again.
    struct Way {
        u32 from = 0, to = 0;
        u64 used = 0; // m_way_clock when last taken (0: empty)
        std::vector<u32> nodes;
    };
    static constexpr usize k_ways_kept = 256;
    const Way* way(u32 from, u32 to);
    void keep_way(u32 from, u32 to, const std::vector<u32>& nodes);
    void set_off(Trip& trip, float yaw);
    void build(const world::Routes& routes);
    bool routes_changed(const world::Routes& routes);
    void update_rest(const world::Scene& scene);

    u64 m_scene = 0;
    u64 m_run = 0;
    u64 m_resets = 0;
    u64 m_puts = 0;
    double m_fastest = 0.0;
    u64 m_serial = 0;
    u64 m_builds = 0;
    double m_seconds = 0.0;
    world::Routes m_routes; // as the graph was built from them
    nav::RouteGraph m_graph;
    std::vector<u32> m_island; // by the graph's node: which joined-up part of the roads it is in
    bool m_built = false;
    usize m_node_cursor = 0, m_link_cursor = 0; // in the routes: compared with m_routes a stretch a frame
    std::vector<Way> m_ways;
    u64 m_way_clock = 0;
    nav::RouteSearch m_search;
    u32 m_search_from = 0, m_search_to = 0; // the graph's nodes m_search goes between
    world::PersonId m_searching = 0; // whose way m_search looks for (0: nobody's)
    std::vector<world::PersonId> m_queue; // waiting for their way, from m_queue_head
    usize m_queue_head = 0;
    std::vector<Trip> m_trips;
    std::unordered_map<world::PersonId, u32> m_index; // into m_trips
    usize m_cursor = 0;                               // in m_trips: looked at for having arrived
    std::unordered_map<world::PersonId, Where> m_places; // those at rest somewhere they travelled to
    std::unordered_set<world::PersonId> m_on_road; // those stopped part way along the roads (or put: a loaded game's)
    usize m_place_bucket = 0; // in m_places: looked through for people gone, a few buckets a frame
    std::vector<world::PersonId> m_gone; // scratch
    std::unordered_set<world::PersonId> m_held;
    Stats m_stats;
};

} // namespace giant::editor
