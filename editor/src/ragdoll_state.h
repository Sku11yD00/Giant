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
// walks no more.
//
// Bullets meet the bones' bodies (as flesh, naming the entity) instead of
// the character's hit capsules, and push them as any body.
//
// Its morph weights stay as they were when it went limp (its clips'
// weight tracks are not played under a held pose).
//
// View state: not undone, not saved. Stopping the simulation (or another
// scene) lets every ragdoll go: drawn where the scene has it, playing its
// clips again.
#pragma once

#include "animation_state.h"
#include "mesh_library.h"
#include "physics_state.h"
#include "walker_state.h"

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
        // Scratch (kept: nothing allocated as it moves).
        std::vector<Mat4> placed;   // per bone, in the entity's space
        std::vector<Mat4> model;    // per joint
    };

    static constexpr usize k_max_ragdolls = 256;

    // Makes `id` a ragdoll as it is drawn now, every bone moving at
    // `velocity` (m/s; none: a walker's own, else at rest). Empty: done;
    // else why not (not simulating, not a visible skinned mesh, no bone
    // capsules, a ragdoll already, too many, a scale of nothing).
    std::string start(const world::Scene& scene, const MeshLibrary& meshes, world::EntityId id,
                      std::optional<Vec3> velocity = std::nullopt);
    // Lets it go (drawn where the scene has it, its clips playing again). False: not a ragdoll.
    bool stop(const world::Scene& scene, const MeshLibrary& meshes, world::EntityId id);
    void clear(const world::Scene& scene, const MeshLibrary& meshes);
    const Ragdoll* find(world::EntityId id) const;
    // Of this scene and the physics' run now (else they are let go at the next step).
    bool current(const world::Scene& scene) const {
        return scene.identity() == m_scene && m_physics.runs() == m_run && m_physics.simulating();
    }
    usize size() const { return m_ragdolls.size(); }
    const std::unordered_map<world::EntityId, Ragdoll>& ragdolls() const { return m_ragdolls; }

    // Each frame (and fixed step) before the physics steps: those whose
    // entity went (removed, hidden, another file) are let go, and all of
    // them when the simulation stopped or the scene is another.
    void before_step(const world::Scene& scene, const MeshLibrary& meshes);
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
};

} // namespace giant::editor
