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
// `triangles`. The pose is asked for when someone of the look is first
// within `distance` (one look a frame; baked on a worker: its people are
// made figures once it is there), and baked again when its file is
// imported again.
//
// Figures keep their slots, and the slots are drawn in chunks
// (scene_render.h: FigureDraws), so one coming, going or put elsewhere has
// its chunk of 64 sent again, no more. A person given an entity stops
// being a figure that frame.
//
// A frame's work is bounded: `scanned_a_frame` people are looked at (the
// list is gone through over frames), `made_a_frame` figures made, at most
// `most` kept, one look's pose asked for, one look compared with the
// scene's. Only an edit does more at once, each a pass over the slots
// (16,384 at most): a look changed (its figures are put again), `most`
// lowered (the surplus go), a pose baked again or forgotten (its figures
// draw nothing until it is back). Known limits (the next parts of the
// plan): a figure stands (one travelling is put on along its way when the
// list comes round to it: CR3 has them walk), and one whose entity was
// just taken away is a figure again only when the list comes round (CR4:
// no gap, nothing seen to change).
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
#include <unordered_map>
#include <vector>

namespace giant::editor {

class MeshLibrary;
class PeopleState;
class PoseBakes;

class PeopleFigures {
public:
    struct Settings {
        // Off until the plan's later parts (CR4) have a figure and its
        // entity change places unseen: on, one may be seen to.
        bool on = false;
        float distance = 400.0f;    // metres from the camera a person is drawn out to (1 to 4000)
        float keep = 40.0f;         // metres farther one drawn stays so (0 to 1000)
        u32 most = 4096;            // figures at once (0 to k_max_figures)
        u32 made_a_frame = 64;      // figures made a frame (1 to 1024)
        u32 scanned_a_frame = 1024; // people looked at a frame (1 to 65536)
        u32 triangles = 1500;       // each look's pose is cut to (PoseBakes' range)
        std::string clip = "idle";  // the clip whose start they stand in (at most world::k_max_name_bytes)
        bool operator==(const Settings&) const = default;
    };
    static constexpr u32 k_max_figures = 16384;
    static constexpr u32 k_first_slots = 256;       // slots at first: doubled as more are needed
    static constexpr u32 k_gone_looked_a_frame = 256; // figures looked at a frame for their person having gone
    Settings settings;
    // While set nobody is a figure (as PeopleState::hold: a loaded game's people are being put back).
    bool hold = false;
    // Set by main (and tests). `travel` null: where the scene has them.
    // `bodies` null: nobody has an entity. `poses` or `meshes` null: no figures.
    const PeopleTravel* travel = nullptr;
    const PeopleState* bodies = nullptr;
    PoseBakes* poses = nullptr;
    const MeshLibrary* meshes = nullptr;

    struct Stats {
        u32 figures = 0;       // kept now (each drawn while its look's pose is there)
        u32 slots = 0;         // kept for them
        u64 made = 0;          // since the scene was taken up
        u64 removed = 0;
        u32 waiting = 0;       // wanted last time through the list, not made: `most` reached
        // The scene's looks by their pose, as of the last frame figures were on:
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

private:
    struct LookPose {
        u32 pose = ~0u;                     // in PoseBakes (k_no_pose: not asked for yet, or it could not be)
        bool wanted = false;                // someone of the look was within `distance`
        // The mesh library's version when it was last asked for and could not be had, + 1 (0: never):
        // not asked for again until a file is imported.
        u64 refused_at = 0;
        std::string clip;                   // the clip it was asked for in ("": the rest pose)
        std::vector<render::MeshId> meshes; // the pose's, when ready (else empty: its people are not drawn)
    };
    struct Slot {
        world::PersonId person = 0; // 0: free
        u32 look = 0;               // in m_looks
        PeopleTravel::Where at;     // where the person was when it was put
    };
    // A look with no pose (asked for again while `wanted`).
    static LookPose fresh(bool wanted) {
        LookPose p;
        p.wanted = wanted;
        return p;
    }
    PeopleTravel::Where where(const world::Person& person) const {
        return travel != nullptr ? travel->where(person) : PeopleTravel::Where{person.position, person.yaw};
    }
    void follow_looks(const world::Scene& scene);
    void follow_poses();
    void put(const world::Scene& scene, u32 slot, const world::Person& person, u32 look, const PeopleTravel::Where& at);
    void remove(u32 slot);
    void touch(u32 slot);
    void touch_look(u32 look);
    std::string clip_of(const world::PersonLook& look) const;
    bool grow();

    u64 m_scene = 0;
    Settings m_settings_seen; // as the poses were asked for
    std::vector<world::PersonLook> m_looks; // as the figures were made from them (FigureDraw::look points here)
    std::vector<LookPose> m_poses;          // beside m_looks
    usize m_look_cursor = 0;                // compared with the scene's, one a frame
    usize m_pose_cursor = 0;                // asked for, one a frame
    u64 m_clears_seen = 0;                  // the PoseBakes', last frame
    std::vector<FigureDraw> m_slots;
    std::vector<Slot> m_info; // beside m_slots
    std::vector<u32> m_free;  // slots free: a heap, the lowest first (those in use stay close together)
    std::unordered_map<std::string, u32> m_look_of; // a look's name -> its index in m_looks
    std::unordered_map<world::PersonId, u32> m_of;
    usize m_cursor = 0;      // in the scene's people
    usize m_gone_cursor = 0; // in the slots
    u32 m_waiting_now = 0;
    std::vector<u64> m_versions; // each chunk of slots': FigureDraws::versions
    u64 m_version = 0;           // the last given
    u64 m_meshes_seen = 0;       // the mesh library's version, last frame
    FigureDraws m_draws;
    std::vector<world::PersonId> m_scratch;
    Stats m_stats;
};

} // namespace giant::editor
