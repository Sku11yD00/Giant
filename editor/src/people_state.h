// The editor's people near the camera (living-world plan LW1b). The scene
// keeps thousands of people as records (world/people.h); the few within
// `embody_distance` of the camera are given an entity each, made from the
// person's look where the person stands ("embodied"), and the entity is
// taken away again once the camera is `release_distance` off, the person
// goes, changes or is elsewhere (the simulation stopped and put it back),
// or its cell's ground left the scene. Such an entity
// carries the person's id (Entity::person): it is never saved and the
// world's streaming does not count it as a cell's (the person is the
// world's data, not the entity).
//
// Nothing here is unbounded a frame: `scanned_a_frame` people are looked
// at (the list is gone through over frames), `made_a_frame` entities made,
// at most `most` held. View state: not undone, not saved.
#pragma once

#include "people_travel.h"

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/world/scene.h"

#include <unordered_map>
#include <vector>

namespace giant::editor {

class RagdollState;
class WalkerState;
class WorldStreaming;

class PeopleState {
public:
    struct Settings {
        bool on = true;
        float embody_distance = 60.0f;  // metres from the camera (1 to 1000)
        float release_distance = 80.0f; // metres: farther, the entity goes (at least embody_distance)
        u32 most = 256;                 // entities held at once (0 to k_max_embodied)
        u32 made_a_frame = 2;           // entities made a frame (1 to 64)
        u32 scanned_a_frame = 1024;     // people looked at a frame (1 to 65536)
    };
    static constexpr u32 k_max_embodied = 1024;
    Settings settings;
    // Where the people are while the world runs (people_travel.h); set by
    // main (and tests). Null: where the scene has them. One given an
    // entity is held (its trip goes on as the entity walks) until the entity goes.
    PeopleTravel* travel = nullptr;
    // The walkers (walker_state.h); set by main (and tests). With them, one
    // on a trip who has an entity walks its way over the ground (the trip
    // goes on as far as it has walked); null: it stands.
    WalkerState* walkers = nullptr;
    // Characters gone limp (ragdoll_state.h); set by main. One lying is given no walker until it is up.
    RagdollState* ragdolls = nullptr;
    static constexpr double k_walk_reach = 3.0;     // metres on along its way one's walking counts a frame
    static constexpr double k_walk_end = 3.0;        // metres from its way's end one stopped by those standing there has arrived
    static constexpr double k_walk_wait = 2.0;       // seconds one whose walker was taken or stopped short waits
    static constexpr double k_walk_stuck_wait = 5.0; // and one whose walker failed
    static constexpr usize k_walkers_kept_back = 64; // of the walkers there can be: for walk_to and those getting up
    static constexpr double k_walk_ahead = 40.0;    // metres of its way a walker is given at a time
    static constexpr double k_walk_renew = 6.0;     // metres from that stretch's end the next is given
    static constexpr double k_walk_arrive = 0.3;    // metres from its trip's end one is there
    static constexpr double k_walk_astray = 20.0;   // metres from its way one's walking no longer counts
    static constexpr u32 k_walks_begun_a_frame = 16;

    struct Stats {
        u32 embodied = 0;   // entities held now
        u64 made = 0;       // since the scene was taken up
        u64 released = 0;
        u32 walking = 0;    // of those held: walking their trips' ways now
        u32 waiting = 0;    // near enough last time through the list, not made: `most` reached
    };

    // Each frame, before the scene is drawn. `world`: the open world whose
    // scene this is (people in a cell whose file is not in the scene yet
    // wait for it), or null.
    void update(world::Scene& scene, const DVec3& camera, const WorldStreaming* world);
    // Takes every entity away (they are made again as update finds them).
    void release_all(world::Scene& scene);

    // The entity standing for a person in `scene` now, or 0.
    world::EntityId entity_of(const world::Scene& scene, world::PersonId id) const;
    const Stats& stats() const { return m_stats; }

    // The entity made for `person` of `look`, standing at `at` (not added to a scene).
    static world::Entity entity_for(const world::Person& person, const world::PersonLook& look,
                                    const PeopleTravel::Where& at, double ground);

private:
    void take_up(world::Scene& scene);
    void release(world::PersonId id);
    void flush(world::Scene& scene);
    void strays(world::Scene& scene);

    u64 m_scene = 0; // the scene's identity
    struct Held {
        world::Person was;       // as it was when the entity was made
        PeopleTravel::Where at;  // where the person is (as last looked)
        u64 resets = 0;          // the travel's, then
        u64 trip = 0;            // the trip (its serial) the walker's path is of
        bool moved = false;      // its entity was put where it walked to (not where the person was made)
        bool walking = false;    // a walker walks it along its trip's way
        bool waited = false;     // its walker failed, and the wait before the next try was begun
        double path_end = 0.0;   // metres along the trip its walker's path goes to
        double retry_at = 0.0;   // the travel's clock before which no walk is begun again
    };
    void walk(world::Scene& scene);
    PeopleTravel::Where stand(world::Scene& scene, Held& held, world::EntityId entity);
    PeopleTravel::Where where(const world::Person& person) const {
        return travel != nullptr ? travel->where(person) : PeopleTravel::Where{person.position, person.yaw};
    }
    // Those given an entity (its id: world::person_entity_id).
    std::unordered_map<world::PersonId, Held> m_held;
    std::vector<world::PersonLook> m_looks; // as the entities were made from them
    usize m_look_cursor = 0;
    usize m_cursor = 0;      // in the scene's people
    u32 m_waiting_now = 0;   // counted this time through the list
    std::vector<world::PersonId> m_scratch;
    std::vector<world::EntityId> m_entities; // scratch: entities to take from the scene
    std::vector<DVec3> m_path;               // scratch: a walker's path
    Stats m_stats;
};

} // namespace giant::editor
