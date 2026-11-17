// The editor's people near the camera (living-world plan LW1b). The scene
// keeps thousands of people as records (world/people.h); the few within
// `embody_distance` of the camera are given an entity each, made from the
// person's look where the person stands ("embodied"), and the entity is
// taken away again once the camera is `release_distance` off, the person
// goes or changes, or its cell's ground left the scene. Such an entity
// carries the person's id (Entity::person): it is never saved and the
// world's streaming does not count it as a cell's (the person is the
// world's data, not the entity).
//
// Nothing here is unbounded a frame: `scanned_a_frame` people are looked
// at (the list is gone through over frames), `made_a_frame` entities made,
// at most `most` held. View state: not undone, not saved.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/world/scene.h"

#include <unordered_map>
#include <vector>

namespace giant::editor {

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

    struct Stats {
        u32 embodied = 0;   // entities held now
        u64 made = 0;       // since the scene was taken up
        u64 released = 0;
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

    // The entity made for `person` of `look` (not added to a scene).
    static world::Entity entity_for(const world::Person& person, const world::PersonLook& look, double ground);

private:
    void take_up(world::Scene& scene);
    void release(world::PersonId id);
    void flush(world::Scene& scene);
    void strays(world::Scene& scene);

    u64 m_scene = 0; // the scene's identity
    // Those given an entity (its id: world::person_entity_id), each as it was when made.
    std::unordered_map<world::PersonId, world::Person> m_held;
    std::vector<world::PersonLook> m_looks; // as the entities were made from them
    usize m_look_cursor = 0;
    usize m_cursor = 0;      // in the scene's people
    u32 m_waiting_now = 0;   // counted this time through the list
    std::vector<world::PersonId> m_scratch;
    std::vector<world::EntityId> m_entities; // scratch: entities to take from the scene
    Stats m_stats;
};

} // namespace giant::editor
