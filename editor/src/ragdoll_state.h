// The editor's ragdolls: skinned characters gone limp (ragdoll_entity)
// while the physics simulates. Each is a sim ragdoll (sim/physics.h) made
// from the character's bone capsules (the hit shapes fitted at import) as
// it is posed and stands at that moment: a body for each bone worth one (a
// capsule at least a twenty-fifth of the character's size; fingers and
// toes ride the hand and the foot), joined where the joints are, with
// limits by the bones' names (elbows and knees fold one way, as hinges; a
// spine leans a little; the rest lean in cones about the rest pose). From
// then on its bones pose it: each joint follows its bone's body (its own,
// or the nearest above it), the pose handed to the animation
// (AnimationState::hold_pose) instead of its clips', and it is drawn where
// its first bone (the hips) has gone (handed to the scene's drawing as
// the walkers' poses are: moved in place, kept out of the voxel world);
// the scene keeps where it was put. A walker goes limp where it walks and
// walks no more. Each bone starts moving as its joint was (the animation's
// last two poses: an arm mid-swing swings on), over the character's own
// velocity.
//
// Bullets meet the bones' bodies (as flesh, naming the entity, and its bone
// and zone: PhysicsState::ragdoll_joint) instead of the character's hit
// capsules, and push them as any body.
//
// A shot that gives a standing character's capsule `Settings::shot_drop`
// N s or more drops it (knock), and so does a blast that reaches it with
// `blast_drop` (blast, which also throws those lying about): asked for
// then, they go limp at the next before_step, at most
// `Settings::starts_per_step` of them a step (the rest wait their turn,
// oldest first:
// making a ragdoll is no small thing, and a blast in a crowd would be a
// hitch), each pushed by what dropped it, where it struck.
//
// Its morph weights stay as they were when it went limp (its clips'
// weight tracks are not played under a held pose).
//
// View state: not undone, not saved. Stopping the simulation (or another
// scene) lets every ragdoll go: drawn where the scene has it, playing its
// clips again.
#pragma once

#include "animation_state.h"
#include "hit_targets.h"
#include "mesh_library.h"
#include "physics_state.h"
#include "walker_state.h"

#include "giant/sim/hit_shapes.h"
#include "giant/sim/physics.h"
#include "giant/world/scene.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace giant::editor {

class RagdollState {
public:
    // `walkers` may be null (none walk).
    RagdollState(PhysicsState& physics, AnimationState& animation, WalkerState* walkers);
    ~RagdollState();
    RagdollState(const RagdollState&) = delete;
    RagdollState& operator=(const RagdollState&) = delete;

    // What a bone is, by its joint's name: how its joint may turn.
    enum class Kind : u8 { other, spine, neck, head, collar, upper_arm, forearm, hand, thigh, shin, foot };
    static const char* to_string(Kind kind);
    static Kind kind_of(std::string_view joint_name);
    // A kind's joint: how far it leans from the middle of its range and
    // twists either way (degrees), and for a hinge how far it folds one
    // way from straight (0: no hinge, a cone).
    struct Limits {
        float swing = 30.0f;
        float twist = 20.0f;
        float fold = 0.0f;
    };
    static Limits limits_of(Kind kind);

    struct Bone {
        u32 joint = 0;   // of the file's skeleton
        i32 parent = -1; // an earlier bone (-1: the first)
        Kind kind = Kind::other;
        float mass = 0.0f;
        bool hinge = false;
    };
    struct Ragdoll {
        world::EntityId entity = 0;
        sim::RagdollId id = sim::k_no_ragdoll;
        std::shared_ptr<const MeshLibrary::Skinned> skinned; // as made from
        std::vector<Bone> bones;
        std::vector<u32> bone_of;   // per joint: the bone it follows
        std::vector<Mat4> hold;     // per joint: where it is in its bone's space
        world::Transform start;     // where the entity was drawn as it fell
        DVec3 root_start{};         // its first bone's place then
        bool posed = false;         // its pose handed on at least once
        bool asleep = false;        // its bodies at rest (as last looked)
        double lying = 0.0;         // seconds at rest, on end
        // Scratch (kept: nothing allocated as it moves).
        std::vector<Mat4> placed;   // per bone, in the entity's space
        std::vector<Mat4> model;    // per joint
    };

    static constexpr usize k_max_ragdolls = 256;
    static constexpr usize k_max_waiting = 1024; // drops asked for and not yet made (more are forgotten)

    struct Settings {
        float shot_drop = 3.0f;    // N s a shot gives a standing character to drop it (0: shots drop none)
        float blast_drop = 150.0f; // N s of a blast where it reaches one (0: blasts drop none)
        u32 starts_per_step = 4;   // ragdolls made of those dropped, a before_step (1 to 64)
        float get_up_seconds = 0.0f; // one that can (get_up) rises by itself after lying still this long (0: never)
        float get_up_fade = 0.8f;    // seconds it takes from the pose it lay in to its animation's (0 to 5)
    };
    // Empty: set; else why not (a number that is none, or out of range).
    std::string set_settings(const Settings& settings);
    const Settings& settings() const { return m_settings; }
    // Where a ragdoll is told its entity went limp (its hit capsules forgotten at once). Null: no one.
    void set_hit_targets(HitTargets* targets) { m_targets = targets; }

    // Makes `id` a ragdoll as it is drawn now, every bone moving at
    // `velocity` (m/s; none: a walker's own, else at rest). Empty: done;
    // else why not (not simulating, not a visible skinned mesh, no bone
    // capsules, a ragdoll already, too many, a scale of nothing).
    std::string start(const world::Scene& scene, const MeshLibrary& meshes, world::EntityId id,
                      std::optional<Vec3> velocity = std::nullopt);
    // A ragdoll gets up where it lies: let go as a walker standing there
    // (WalkerState::resume, arrived: its feet on what is under its hips,
    // facing the way its hips do, or toward its feet lying on its back, its
    // head on its front), its pose fading from how it lay into its
    // animation's over Settings::get_up_fade. Only an entity that can walk
    // (body none): another's body stands where the scene has it. Empty:
    // done; else why not (not a ragdoll, cannot walk, no walkers, the
    // walkers refused: it lies on).
    std::string get_up(const world::Scene& scene, const MeshLibrary& meshes, world::EntityId id);
    u64 risen() const { return m_risen; } // got up, ever
    // Lets it go (drawn where the scene has it, its clips playing again). False: not a ragdoll.
    bool stop(const world::Scene& scene, const MeshLibrary& meshes, world::EntityId id);
    void clear(const world::Scene& scene, const MeshLibrary& meshes);
    const Ragdoll* find(world::EntityId id) const;
    // A shot's hit on a standing character, in the scene of identity
    // `scene`: on its capsule round `joint`, or on a walker's body
    // (`joint` k_nearest_bone: the bone nearest `point` is pushed). Hard
    // enough (Settings::shot_drop), it drops at the next before_step,
    // pushed there. True: it will.
    static constexpr u32 k_nearest_bone = ~u32{0};
    bool knock(u64 scene, world::EntityId id, u32 joint, const Vec3& impulse, const DVec3& point);
    // A blast (as PhysicsState::impact: `impulse` N s at `point`, none at
    // `radius`): ragdolls in reach are thrown from it (each bone by its
    // share of the mass and how near it is), and standing characters it
    // reaches hard enough (Settings::blast_drop: those with capsules among
    // `standing` by the nearest, walkers by their middle) drop at the
    // coming before_steps, thrown likewise. No blast throws a body faster
    // than 12 m/s. Returns those thrown and those to drop.
    struct Blasted {
        usize thrown = 0;
        usize dropped = 0;
    };
    Blasted blast(u64 scene, const sim::HitShapes& standing, const DVec3& point, float radius, float impulse);
    // Asked to drop, not yet made.
    usize waiting() const { return m_waiting.size(); }
    u64 dropped() const { return m_dropped; } // made of a shot or a blast, ever
    // A bone's joint's name (empty: no such ragdoll or bone).
    std::string joint_name(world::EntityId id, u32 bone) const;
    // Of this scene and the physics' run now (else they are let go at the next step).
    bool current(const world::Scene& scene) const {
        return scene.identity() == m_scene && m_physics.runs() == m_run && m_physics.simulating();
    }
    usize size() const { return m_ragdolls.size(); }
    const std::unordered_map<world::EntityId, Ragdoll>& ragdolls() const { return m_ragdolls; }

    // Each frame (and fixed step) before the physics steps (`seconds` of
    // it to come): those whose entity went (removed, hidden, another file)
    // are let go, and all of them when the simulation stopped or the scene
    // is another; those struck down are made; those lying still long enough
    // get up (Settings::get_up_seconds; two a step at most).
    void before_step(const world::Scene& scene, const MeshLibrary& meshes, double seconds = 0.0);
    // After it steps: each one that moved posed by its bones.
    void after_step();

    // Where each is drawn (its entity's transform carried with its first bone).
    const std::unordered_map<world::EntityId, world::Transform>& poses() const { return m_poses; }
    // Moved since the last call (moved in place); begun or let go since
    // (their cells built again).
    std::vector<world::EntityId> take_moved();
    std::vector<world::EntityId> take_redraw();

private:
    void drop(const world::Scene& scene, const MeshLibrary& meshes, world::EntityId id);
    // The physics' world made afresh (its ragdolls gone with it): ours forgotten.
    void follow_world(const world::Scene& scene, const MeshLibrary& meshes);
    void pose(Ragdoll& r);
    // Those gone or of another scene or run let go (before_step's first part).
    void follow(const world::Scene& scene, const MeshLibrary& meshes);
    // One asked to drop: a shot's push on a joint's bone, or a blast's on all of it.
    struct Waiting {
        world::EntityId entity = 0;
        bool whole = false; // a blast: each bone by its share
        u32 joint = 0;      // a shot: the joint struck (k_nearest_bone: the bone nearest `point`)
        Vec3 impulse{};     // a shot: N s; a blast: the way it throws (unit)
        DVec3 point{};
        float blast = 0.0f; // a blast: its impulse, and how much of it reaches this one (0 to 1)
        float reach = 0.0f;
    };
    // Those waiting are of this scene and run, from here on (others forgotten).
    void ask_in(u64 scene);
    void strike(const Ragdoll& r, const Waiting& w);
    void make_waiting(const world::Scene& scene, const MeshLibrary& meshes);

    PhysicsState& m_physics;
    AnimationState& m_animation;
    WalkerState* m_walkers;
    u64 m_scene = 0;
    u64 m_run = 0;    // the physics' run they were made in
    u64 m_clears = 0; // the physics world's, as last seen
    std::unordered_map<world::EntityId, Ragdoll> m_ragdolls;
    std::unordered_map<world::EntityId, world::Transform> m_poses;
    std::vector<world::EntityId> m_moved;
    std::vector<world::EntityId> m_redraw;
    std::vector<world::EntityId> m_gone; // scratch
    Settings m_settings;
    HitTargets* m_targets = nullptr;
    std::vector<Waiting> m_waiting; // oldest first
    u64 m_waiting_scene = 0;        // the scene and run they were asked in
    u64 m_waiting_run = 0;
    u64 m_dropped = 0;
    u64 m_risen = 0;
};

} // namespace giant::editor
