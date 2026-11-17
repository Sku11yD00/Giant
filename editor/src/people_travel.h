// The editor's people travelling (living-world plan LW2b). While the
// physics simulates, a person sent somewhere (send_person) goes there along
// the world's roads and paths (world::Routes, nav/route.h): the way is
// looked for over frames (one shared graph, one search at a time in its own
// lists, a budget a frame), and the person then moves along it by the
// simulation's clock at a walking speed. Where one is at any time is worked
// out from when it set off, so thousands travelling cost nothing a frame
// but the few looked at for having arrived.
//
// The scene keeps where each person starts (world::Person); where one who
// has travelled is now is kept here (`where`), and is the simulation's:
// stopping the simulation (or another scene) puts everyone back.
//
// A person given an entity near the camera (people_state.h) stands still
// while it has one (`hold`): walking the ground from node to node is the
// next part.
//
// View state: not undone, not saved.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/nav/route.h"
#include "giant/world/scene.h"

#include <string>
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
        Phase phase = Phase::searching;
        DVec3 to{};        // where it is going
        std::string place; // the place asked for by name, if one was
        double speed = 1.4; // m/s
        float yaw = 0.0f;   // degrees: the way it faced when it set off (kept where its way has no length)
        world::RouteNodeId first = 0, last = 0; // the nodes it joins and leaves the roads at
        std::vector<DVec3> points; // travelling: the way, from where it set off to `to`
        std::vector<double> along; // metres to each point
        double from = 0.0;  // metres along at `since`
        double since = 0.0; // the clock then
        bool held = false;  // stands (an entity stands for it)
        const char* reason = ""; // failed: why
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
    // far from a road it and the goal may be. Empty: on its way (or its way
    // is being looked for); else why not. The graph is built here when the
    // routes have changed (a command's cost, not a frame's).
    std::string send(const world::Scene& scene, world::PersonId id, const DVec3& to, float speed, double within,
                     std::string place = {});
    // Ends a person's trip where it is. False: it had none.
    bool stop(world::PersonId id);
    void clear();

    // Where a person is now.
    Where where(const world::Person& person) const;
    const Trip* trip(world::PersonId id) const;
    // Metres a trip has gone and has in all (travelling).
    double gone(const Trip& trip) const;
    static double length(const Trip& trip) { return trip.along.empty() ? 0.0 : trip.along.back(); }
    // An entity stands for the person (it stands still), or no longer.
    void hold(world::PersonId id, bool held);
    Stats stats() const;

private:
    Trip* find(world::PersonId id);
    void end(usize index, bool arrived);
    void fail(Trip& trip, const char* reason);
    void found(const world::Scene& scene, Trip& trip);
    void set_off(Trip& trip, float yaw);
    void build(const world::Routes& routes);
    bool routes_changed(const world::Routes& routes);
    void update_rest(const world::Scene& scene);
    static Where at(const Trip& trip, double metres);

    u64 m_scene = 0;
    u64 m_run = 0;
    double m_seconds = 0.0;
    world::Routes m_routes; // as the graph was built from them
    nav::RouteGraph m_graph;
    std::vector<u32> m_island; // by the graph's node: which joined-up part of the roads it is in
    bool m_built = false;
    usize m_node_cursor = 0, m_link_cursor = 0; // in the routes: compared with m_routes a stretch a frame
    nav::RouteSearch m_search;
    world::PersonId m_searching = 0; // whose way m_search looks for (0: nobody's)
    std::vector<world::PersonId> m_queue; // waiting for their way, from m_queue_head
    usize m_queue_head = 0;
    std::vector<Trip> m_trips;
    std::unordered_map<world::PersonId, u32> m_index; // into m_trips
    usize m_cursor = 0;                               // in m_trips: looked at for having arrived
    std::unordered_map<world::PersonId, Where> m_places; // those at rest somewhere they travelled to
    usize m_place_bucket = 0; // in m_places: looked through for people gone, a few buckets a frame
    std::vector<world::PersonId> m_gone; // scratch
    std::unordered_set<world::PersonId> m_held;
    Stats m_stats;
};

} // namespace giant::editor
