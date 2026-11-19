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
// Nobody appears or goes close by in view (LW4): with what the camera sees
// given (`view`), one it sees is made only farther off than
// `embody_distance` (out to `view_distance`, so those walked or looked
// towards are there long before they are near) and, while the world runs,
// one near is made only while unseen; an entity it sees stays until
// `view_distance` and the gap between the two distances is passed. After
// what is seen changed at a stroke anyway (a load, a new scene, a run
// stopped, turned on, the camera put elsewhere, an edit to a look or a
// person, an entity deleted, ground streamed in) the list is gone through
// once with everyone near made whether seen or not. When `most` are held
// the farthest held gives its entity up for one near, or for one 20 m
// nearer than it: those held are the nearest there are.
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
#include <unordered_set>
#include <vector>

namespace giant::editor {

class AnimationState;
class MeshLibrary;
class PeopleRoutines;
class RagdollState;
class WalkerState;
class WorldStreaming;

class PeopleState {
public:
    struct Settings {
        bool on = true;
        float embody_distance = 60.0f;  // metres from the camera (1 to 1000)
        float release_distance = 80.0f; // metres: farther, the entity goes (at least embody_distance)
        float view_distance = 200.0f;   // metres: one the camera looks towards gets its entity this far off (`view` on)
        u32 most = 256;                 // entities held at once (0 to k_max_embodied)
        u32 made_a_frame = 2;           // entities made a frame (1 to 64)
        u32 scanned_a_frame = 1024;     // people looked at a frame (1 to 65536)
    };
    static constexpr u32 k_max_embodied = 1024;
    Settings settings;
    // What the camera sees (set by main each frame; off: nothing counts as seen, and only
    // embody_distance and release_distance tell who has an entity).
    struct View {
        bool on = false;
        Vec3 forward{0.0f, 0.0f, -1.0f}; // the way it looks (unit)
        float half_angle = 0.6f;         // radians from that to the view's corner
    };
    View view;
    // The view of a camera turned `yaw_degrees` (0 looks down -Z, 90 down -X) and `pitch_degrees`
    // up, seeing `vertical_fov_degrees` in a picture `aspect` wide for its height.
    static View view_of(float yaw_degrees, float pitch_degrees, float vertical_fov_degrees, float aspect);
    static constexpr float k_seen_margin = 0.09f;  // radians past the view's corner one still counts as seen
    static constexpr float k_ahead_margin = 0.45f; // radians past it one is made far off (before it is turned to)
    static constexpr float k_kept_margin = 0.6f;   // radians past it one made far off is kept
    static constexpr double k_seen_made_within = 0.85; // of embody_distance: nearer than this one in view is not made
    static constexpr double k_swap_gap = 20.0;     // metres farther off one held must be to give its entity up for one not near
    static constexpr double k_set_off_within = 1.0; // seconds after its entity is made one part way along its trip begins at its pace
    static constexpr double k_camera_jump = 50.0;  // metres in a frame: the camera did not move there, it was put there
    // While set nobody has an entity (a loaded game's people are being put back where they were: game_saves.h).
    bool hold = false;
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
    // What each does by its routine (people_routines.h) and the animation
    // that shows it (animation_state.h, with the meshes it plays from); set
    // by main (and tests). With all three, one standing where its routine
    // has it plays the clip its look gives the activity (LookActivity), and
    // is motion matched again when it walks or the activity has no clip.
    // One just given its entity is in that clip from its first frame (it
    // stood so as a figure: people_figures.h; nothing fades from the rest
    // pose or the matching into a clip); with more made a frame than clips
    // are begun (below), the rest stand until their turn, a frame or a few.
    const PeopleRoutines* routines = nullptr;
    AnimationState* animation = nullptr;
    const MeshLibrary* meshes = nullptr;
    static constexpr u32 k_acts_begun_a_frame = 8;  // clips begun a frame (each poses its character at once)
    static constexpr u32 k_acts_made_a_frame = 2;   // of them, on entities not animated yet (each makes a copy of its character)
    static constexpr double k_act_wait = 2.0;       // seconds before a clip that could not be played is tried again
    static constexpr float k_act_fade = 0.3f;       // seconds a clip fades in over
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
        u32 waiting = 0;    // wanting one last time through the list (near, or far where the camera looks), not made: `most` reached
        u32 acting = 0;     // of those held: playing their activity's clip now
        u32 unseen = 0;     // near and in view last time through the list, not made: waiting to be out of sight
        u64 swapped = 0;    // entities far off given up for someone near (`most` held)
        // While the world runs: entities made, and taken away, close by in the camera's view (nearer
        // than k_seen_made_within of embody_distance, where one is otherwise made only while unseen).
        // What the rules above let through: the pass after everything changed at a stroke, an edit
        // (to a person, a look, the looks' number), ground streamed in or out under one, `most`
        // lowered. Not counted: everyone taken at once with the people turned off or held back (a
        // loaded game), or with another scene. A flight that counts none, with none of those in it,
        // saw nobody appear or go close by.
        u64 appeared_in_view = 0;
        u64 vanished_in_view = 0;
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
    // Whether a person has an entity now, and all who do (added to `out`).
    bool holds(world::PersonId id) const { return m_held.contains(id); }
    void held(std::vector<world::PersonId>& out) const {
        for (const auto& [id, h] : m_held) out.push_back(id);
    }
    // Those whose entity was taken away in the last update (people_figures.h draws them from that
    // frame on: nobody is missing between the two).
    const std::vector<world::PersonId>& let_go() const { return m_let_go; }

    // The entity made for `person` of `look`, standing at `at` (not added to a scene).
    static world::Entity entity_for(const world::Person& person, const world::PersonLook& look,
                                    const PeopleTravel::Where& at, double ground);

private:
    void take_up(world::Scene& scene);
    void release(world::PersonId id);
    void again(world::PersonId id);
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
        bool set_off = false;    // its entity has been walked along its trip (the first time, part way, it begins moving)
        double made_at = 0.0;    // the travel's clock when its entity was made
        bool waited = false;     // its walker failed, and the wait before the next try was begun
        double path_end = 0.0;   // metres along the trip its walker's path goes to
        double retry_at = 0.0;   // the travel's clock before which no walk is begun again
        u32 look = 0;            // its look's index in m_looks (as last found)
        u8 acting = 0;           // the look's activity whose clip its entity plays, + 1 (0: none, it is motion matched)
        u8 act_failed = 0;       // the look's activity, + 1, whose clip the file does not have (0: none found so)
        double act_at = 0.0;     // the travel's clock before which no clip is tried again
    };
    void walk(world::Scene& scene);
    void act(const world::Scene& scene);
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
    u32 m_unseen_now = 0;
    static constexpr usize k_most_again = 2048;
    static constexpr u32 k_ground_wait_frames = 600; // frames a pass made whether seen or not waits for ground to stream in
    std::unordered_set<world::PersonId> m_again; // those whose entity an edit took: made again though seen
    u32 m_free_frames = 0;  // frames since such a pass was begun
    u32 m_most_seen = 0;    // `most`, last frame
    bool m_ground_waited = false; // one near waited for its ground this time through the list
    u32 m_free_passes = 2;   // times the list's end is still to be passed with everyone near made, seen or not
    bool m_was_off = true;   // nobody could have an entity last frame (off, held back)
    bool m_camera_known = false;
    DVec3 m_camera{};        // where the camera was last frame
    u64 m_resets_seen = 0;   // the travel's, last frame
    std::vector<world::PersonId> m_scratch;
    std::vector<world::EntityId> m_entities; // scratch: entities to take from the scene
    std::vector<world::PersonId> m_let_go;   // those released in the last update
    std::vector<DVec3> m_path;               // scratch: a walker's path
    Stats m_stats;
};

} // namespace giant::editor
