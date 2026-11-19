// The editor's people far from the camera, drawn as figures (crowds plan,
// CR2). Those near have entities (people_state.h): a character each, with
// a skeleton, a walker and senses. Everyone else out to `distance` (one
// near who has no entity too: `most` of those reached, or waiting to be
// unseen) is a figure: no entity, no skeleton, only the instances of a
// baked pose of its look's character (pose_bakes.h) put where the person
// is, on the ground there, so a town seen from its edge shows its people
// for the cost of a few meshes.
//
// A look's people stand in one pose: `clip` of the look's file at its
// start (the file's rest pose where it has no such clip), cut to
// `triangles`. One travelling (people_travel.h) is put along its way every
// frame, facing along it, and steps through `walk_poses` poses of
// `walk_clip` baked in place (pose_bakes.h): which one by how far it has
// gone, a round of them to the ground the clip covers in one (`stride`
// where the clip goes nowhere), so its feet keep to the ground at any
// speed. One doing what its routine has it do, where that has it be
// (people_routines.h), is held in the pose its look gives the activity
// (world::LookActivity: the clip's start, or its end for one played once),
// and is not drawn at all while the activity is `hidden` (asleep, indoors).
// A pose is asked for when someone within `distance` first needs it (a few
// a frame; baked on a worker: a look's people are made figures once its
// standing pose is there, and one whose walk or activity pose is not there
// yet, or cannot be had, stands meanwhile), and baked again when its file
// is imported again.
//
// Figures keep their slots, and the slots are drawn in chunks
// (scene_render.h: FigureDraws), so one coming, going or put elsewhere has
// its chunk of 64 sent again, no more. A person given an entity stops
// being a figure that frame.
//
// A frame's work is bounded: `scanned_a_frame` people are looked at (the
// list is gone through over frames), `made_a_frame` figures made, at most
// `most` kept, `moved_a_frame` of those travelling put on (in turn: with
// more travelling than that each moves every few frames; each chunk one
// is in is sent again that frame), k_asked_a_frame poses asked for, one
// look compared with the scene's. Only an edit does more at once, each a
// pass over the slots (16,384 at most): a look changed (its figures are
// put again), `most` lowered (the surplus go). And a pose baked (in play
// too: the first time one is needed), baked again or forgotten has one
// pass point the figures that show it at what they show now, those alone
// sent again.
// Standing poses are asked for before any other, and room is kept in
// PoseBakes for one of every look.
// Known limits: one setting off, arriving or beginning an activity shows
// it when the list comes round to it (a second or so with 65,536 people);
// a figure turns at once at a bend of its way; a look's poses count
// against PoseBakes' 1,024 (a standing one, the walk's, one for each
// activity met: past that a look's people stand whatever they do, a pose
// refused for want of room being asked for again only after an import or
// clear_baked_poses, and past 1,024 looks' standing poses a look is not
// drawn; a file imported has every pose asked for looked at and every
// walking look's round measured again that frame; poses asked for before a file
// was imported with clips of another length stay there too, until
// clear_baked_poses); a rig whose hips are not found by name stands as it
// goes; and one whose entity was just
// taken away is a figure again only when the list comes round (CR4: no
// gap, nothing seen to change).
//
// View state: not undone, not saved.
#pragma once

#include "people_travel.h"
#include "scene_render.h"

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/render/renderer.h"
#include "giant/world/scene.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace giant::editor {

class MeshLibrary;
class PeopleRoutines;
class PeopleState;
class PoseBakes;

class PeopleFigures {
public:
    static constexpr u32 k_max_figures = 16384;
    static constexpr u32 k_first_slots = 256;         // slots at first: doubled as more are needed
    static constexpr u32 k_gone_looked_a_frame = 256; // figures looked at a frame for their person having gone
    static constexpr u32 k_asked_a_frame = 4;         // poses asked for a frame
    static constexpr u32 k_max_walk_poses = 16;
    // What a figure shows (`kind_of`): standing, a pose of the walk (k_first_walk + its place in the
    // round), or the pose of an activity (k_first_activity + its place in the look's list).
    static constexpr u32 k_stand = 0;
    static constexpr u32 k_first_walk = 1;
    static constexpr u32 k_first_activity = k_first_walk + k_max_walk_poses;
    static constexpr u32 k_kinds = k_first_activity + static_cast<u32>(world::k_max_look_activities);
    static constexpr u32 k_no_kind = ~0u;

    struct Settings {
        // Off until the plan's later parts (CR4) have a figure and its
        // entity change places unseen: on, one may be seen to.
        bool on = false;
        float distance = 400.0f;    // metres from the camera a person is drawn out to (1 to 4000)
        float keep = 40.0f;         // metres farther one drawn stays so (0 to 1000)
        u32 most = 4096;            // figures at once (0 to k_max_figures)
        u32 made_a_frame = 64;      // figures made a frame (1 to 1024)
        u32 scanned_a_frame = 1024; // people looked at a frame (1 to 65536)
        u32 moved_a_frame = 2048;   // of those travelling, put on along their way a frame (1 to k_max_figures)
        u32 triangles = 1500;       // each pose is cut to (PoseBakes' range)
        std::string clip = "idle";  // the clip whose start they stand in (at most world::k_max_name_bytes)
        // The clip one travelling steps through ("", or the look's file has none: it stands as it goes).
        std::string walk_clip = "walk";
        u32 walk_poses = 8;         // poses a round of the walk is shown in (2 to k_max_walk_poses)
        // Metres a round of the walk covers where the clip itself goes nowhere (one made walking on the
        // spot), for a look of height 1 (0.1 to 10).
        float stride = 1.4f;
        // The activity whose people are not drawn while they are at it where it has them be ("": none).
        std::string hidden = "sleep";
        bool operator==(const Settings&) const = default;
    };
    Settings settings;
    // While set nobody is a figure (as PeopleState::hold: a loaded game's people are being put back).
    bool hold = false;
    // Set by main (and tests). `travel` null: where the scene has them, nobody travelling.
    // `routines` null: nobody is doing anything. `bodies` null: nobody has an entity.
    // `poses` or `meshes` null: no figures.
    const PeopleTravel* travel = nullptr;
    const PeopleRoutines* routines = nullptr;
    const PeopleState* bodies = nullptr;
    PoseBakes* poses = nullptr;
    const MeshLibrary* meshes = nullptr;

    struct Stats {
        u32 figures = 0;       // kept now (each drawn while its look's pose is there)
        u32 slots = 0;         // kept for them
        u64 made = 0;          // since the scene was taken up
        u64 removed = 0;
        u32 waiting = 0;       // wanted last time through the list, not made: `most` reached
        u32 hidden = 0;        // not drawn last time through the list: at the `hidden` activity
        u32 walking = 0;       // of `figures`: travelling, put on along their way in turn
        u32 moved = 0;         // of those, put on this frame
        u32 acting = 0;        // of `figures`: at an activity their look names (held in its pose where the file has the clip)
        u32 poses = 0;         // asked of PoseBakes for the looks, kept now
        // The scene's looks by their standing pose, as of the last frame figures were on:
        u32 looks_ready = 0;   // baked
        u32 looks_baking = 0;  // being baked
        // None: nobody of the look has been within `distance`, or it could not be had (its file not
        // loaded or not skinned, the bake failed, or as many poses are kept as can be).
        u32 looks_without = 0;
    };

    // Each frame, after the people near the camera were given their
    // entities and before the scene is drawn.
    void update(const world::Scene& scene, const DVec3& camera);
    // What scene_frame draws of them (good until the next update or clear).
    const FigureDraws& draws() const { return m_draws; }
    // Every figure gone (they are made again as update finds them).
    void clear();
    const Stats& stats() const { return m_stats; }
    // The figure standing for a person now: where it stands, or null.
    const world::Transform* figure_of(world::PersonId id) const;
    // ... and what it is to show (k_stand, a walk's pose, an activity's: it stands while that pose is
    // not there). k_no_kind: it has no figure.
    u32 kind_of(world::PersonId id) const;
    // ... the activity that is, by name ("": it is at none its look names; good until the next update) ...
    std::string_view activity_of(world::PersonId id) const;
    // ... and the way it faces, degrees (0 with no figure).
    float yaw_of(world::PersonId id) const;

private:
    // One pose of a look: m_poses holds k_kinds for each look, in their order.
    struct LookPose {
        u32 pose = ~0u;        // in PoseBakes (k_no_pose: not asked for, or it could not be had)
        u32 asked_at = 0;      // its place in m_asked, while it has a pose
        bool queued = false;   // in m_to_ask
        bool retried = false;  // its bake failed and was begun again once
        bool changed = false;  // its meshes are others than its figures were last pointed at (in m_changed)
        // The mesh library's version when it was last asked for and could not be had, + 1 (0: never):
        // not asked for again until a file is imported.
        u64 refused_at = 0;
        std::string clip;      // as it was asked for ("": the rest pose)
        float time = 0.0f;
        bool in_place = false;
        std::vector<render::MeshId> meshes; // the pose's, when ready (else empty)
    };
    struct Slot {
        world::PersonId person = 0; // 0: free
        u32 look = 0;               // in m_looks
        PeopleTravel::Where at;     // where the person was when it was put
        u32 kind = k_stand;
        u32 moving = ~0u;           // its place in m_moving (~0: it is not travelling)
        u32 person_at = 0;          // where its person was last found in the scene's list
    };
    // Where a person is and what its figure is to show.
    struct Pick {
        PeopleTravel::Where at;
        u32 kind = k_stand;
        bool moving = false; // travelling: put on every frame
        bool hidden = false; // not drawn
    };
    // What a pose is asked of PoseBakes as.
    struct Asked {
        std::string clip;
        float time = 0.0f;
        bool in_place = false;
    };
    PeopleTravel::Where where(const world::Person& person) const {
        return travel != nullptr ? travel->where(person) : PeopleTravel::Where{person.position, person.yaw};
    }
    static bool is_walk(u32 kind) { return kind >= k_first_walk && kind < k_first_activity; }
    u32 walk_poses() const { return settings.walk_poses < 2 ? 2 : settings.walk_poses > k_max_walk_poses ? k_max_walk_poses : settings.walk_poses; }
    Pick pick(const world::Scene& scene, const world::Person& person, u32 look) const;
    bool asked_as(u32 look, u32 kind, Asked& out) const;
    void need(u32 look, u32 kind);
    void forget(u32 index);
    void forget_all();
    void forget_asking();
    void changed(u32 index);
    void measure(u32 look);
    const std::vector<render::MeshId>* shown(u32 look, u32 kind) const;
    void follow_looks(const world::Scene& scene);
    void follow_poses();
    void move(const world::Scene& scene);
    void put(const world::Scene& scene, u32 slot, const world::Person& person, u32 person_at, u32 look, const Pick& pick);
    void set_moving(u32 slot, bool moving);
    void remove(u32 slot);
    void touch(u32 slot);
    bool grow();

    u64 m_scene = 0;
    Settings m_settings_seen; // as the poses were asked for
    std::vector<world::PersonLook> m_looks; // as the figures were made from them (FigureDraw::look points here)
    std::vector<LookPose> m_poses;          // k_kinds for each of m_looks (FigureDraw::meshes points here)
    std::vector<float> m_round;             // beside m_looks: the ground a round of its walk covers at height 1 (0: not known)
    std::vector<u8> m_walked;               // beside m_looks: a pose of its walk was asked for (its round is measured)
    std::vector<u32> m_asked;               // the poses (in m_poses) that have one in PoseBakes
    u32 m_stands = 0;                       // of those, standing poses
    std::vector<u32> m_to_ask;              // those waiting to be asked for, from m_to_ask_head
    usize m_to_ask_head = 0;
    std::vector<u32> m_to_ask_first;        // ... the standing ones, asked for before any of the others
    usize m_to_ask_first_head = 0;
    std::vector<u32> m_changed;             // the poses whose meshes changed since the figures were pointed
    usize m_look_cursor = 0;                // compared with the scene's, one a frame
    u64 m_clears_seen = 0;                  // the PoseBakes', last frame
    std::vector<FigureDraw> m_slots;
    std::vector<Slot> m_info; // beside m_slots
    std::vector<u32> m_free;  // slots free: a heap, the lowest first (those in use stay close together)
    std::vector<u32> m_moving; // the slots of those travelling
    usize m_move_cursor = 0;
    std::unordered_map<std::string, u32> m_look_of; // a look's name -> its index in m_looks
    std::unordered_map<world::PersonId, u32> m_of;
    usize m_cursor = 0;      // in the scene's people
    usize m_gone_cursor = 0; // in the slots
    u32 m_waiting_now = 0;
    u32 m_hidden_now = 0;
    u32 m_acting = 0;
    std::vector<u64> m_versions; // each chunk of slots': FigureDraws::versions
    u64 m_version = 0;           // the last given
    u64 m_meshes_seen = 0;       // the mesh library's version, last frame
    FigureDraws m_draws;
    std::vector<world::PersonId> m_scratch;
    Stats m_stats;
};

} // namespace giant::editor
