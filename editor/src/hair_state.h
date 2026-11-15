// The editor's hair: entities that wear hair (world::Hair) have a groom
// grown from the top of their shape (cubes and spheres) or from a
// character's skin round a joint (mesh entities with a skinned file)
// (geometry::generate_groom) and packed for the renderer on a worker,
// handed to the renderer (render::Renderer::add_groom), simulated near the
// camera (giant/sim/hair.h) and named to the renderer each frame
// (render::FrameInput::hair).
//
// A groom is grown from the shape as the entity scales it, so the entity's
// scale stretches the head and not the strands; it is drawn where the
// entity is drawn (place(): the physics', a walker's or a swing's pose,
// else its transform). A character's is grown from the skin the joint and
// the joints under it move (by half its weight or more) as it stands at
// rest, about the joint, sized by the entity's largest scale; it rides the
// joint and turns as the joint has turned since rest; the body's capsules nearest the joint push it aside. Changing what it grows from grows it again: the
// old groom shows (and moves) until the new one is grown and the renderer
// has all of it, then they are swapped. One job an entity at a time; a
// result no longer wanted when it arrives is thrown away and the next
// started, so hair being changed every frame keeps its old groom until the
// changes stop.
//
// Bounds (rule 9): entities wear hair lowest ids first, up to k_max_hair of
// them and k_max_strands strands in all; the rest are left out (nothing
// grown or kept for them). The nearest with `moves` on are simulated:
// within k_simulate_within metres, at most k_max_simulated of them and
// k_max_simulated_guides guides in all, stepped side by side on two
// workers; the rest hang as grown. One that was moving keeps its place
// until another is clearly nearer (no trading places every frame at the
// edge), and at most k_sims_made_per_update start moving in one update.
//
// View state, as the cloth: nothing of it is saved or undoable.
#pragma once

#include "hit_targets.h"
#include "mesh_library.h"

#include "giant/core/jobs.h"
#include "giant/core/wind.h"
#include "giant/geometry/groom.h"
#include "giant/geometry/mesh.h"
#include "giant/render/renderer.h"
#include "giant/sim/hair.h"
#include "giant/world/scene.h"

#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace giant::editor {

// The part of a shape hair grows from: the cube or sphere (1 m, centred,
// sized by `scale`) above the top `top` share of its height, its triangles
// cut at that height, its normals those of the sized shape. Other shapes:
// empty.
geometry::TriangleMesh hair_scalp(world::Shape shape, const Vec3& scale, float top);
// A character's: the triangles of its skin that `joint` and the joints
// under it move (half the triangle's weight or more), as they stand at
// rest in model space (y up) about the joint's place, above the top `top`
// share of their height, times `scale`. A joint past the skeleton: empty.
geometry::TriangleMesh hair_scalp(const MeshLibrary::Skinned& character, u32 joint, float scale, float top);
// The joint hair grows round: the one of that name, or for no name the
// first with "head" in its name (any case). None: k_no_hair_joint.
inline constexpr u32 k_no_hair_joint = ~0u;
u32 hair_joint(const anim::Skeleton& skeleton, const std::string& name);
geometry::GroomParams hair_params(const world::Hair& hair);
render::HairLook hair_look(const world::Hair& hair);
sim::HairSettings hair_motion(const world::Hair& hair);

struct HairStats {
    u32 wearing = 0;   // visible entities with hair on of a shape that may wear it (a mesh's may yet have a problem)
    u32 grown = 0;     // with a groom
    u32 growing = 0;   // with a job running, or a new groom on its way to the GPU
    u32 simulated = 0; // moved in the last update
    u32 drawn = 0;     // named to the renderer
    u64 strands = 0;   // of the grown grooms
    u32 left_out = 0;  // past k_max_hair or k_max_strands
    u64 resets = 0;    // simulations put back at rest, ever (sim::HairStats::resets)
};

class HairState {
public:
    static constexpr usize k_max_hair = 256;
    static constexpr u64 k_max_strands = 2000000;
    static constexpr usize k_max_simulated = 16;
    static constexpr usize k_max_simulated_guides = 4096;
    static constexpr u32 k_sims_made_per_update = 2;
    static constexpr u32 k_taken_per_update = 2; // grown grooms handed to the renderer
    static constexpr double k_simulate_within = 30.0;
    static constexpr double k_keep_simulating_within = 33.0; // one already moving

    // Without a renderer (tests) hair grows and moves but nothing draws it.
    // Without a mesh library no character grows hair.
    explicit HairState(render::Renderer* renderer, const MeshLibrary* meshes = nullptr);
    ~HairState(); // the grooms leave the renderer; jobs not yet started are not run
    HairState(const HairState&) = delete;
    HairState& operator=(const HairState&) = delete;

    // The scene's changes followed (hair put on, changed, moved, gone).
    void follow(const world::Scene& scene, const world::SceneChanges& changed);
    // Before a frame is drawn: where each is drawn (the first of `moving`,
    // `walking`, `swinging` that poses its entity, else its transform; a
    // character's at its joint as `animation` poses it, or at rest), and
    // its body's capsules. Each may be null. Until first called hair is at
    // its entity's transform (a character's at its joint at rest); from
    // then on only this moves it. update() steps with the place of the
    // frame just drawn, so hair answers its head a frame late (its roots
    // stay on).
    void place(const AnimationState* animation, const DrawnPoses* moving, const DrawnPoses* walking,
               const DrawnPoses* swinging);
    // Each frame: grooms grown since are taken, the nearest move under
    // `wind`, and the frame's draws are listed.
    void update(const WindState& wind, const DVec3& camera, double seconds);
    // Waits for every groom being grown and shows it at once (tests,
    // commands that read them): a groom that replaces another is then shown
    // before the renderer has all of it, so the hair is not drawn for the
    // few frames that takes.
    void finish();

    // What the renderer draws this frame (valid until the next follow,
    // place, update or finish).
    std::span<const render::HairDraw> draws() const { return m_draws; }
    HairStats stats() const;

    struct Worn; // (hair_state.cpp)
    // One entity's hair as it is: false if it wears none.
    struct View {
        bool grown = false;
        bool growing = false;
        bool ready = false; // drawn from now on: the renderer has all of its groom
        bool simulated = false;
        bool left_out = false;
        std::string problem; // why it cannot grow (a mesh file not loaded or without a skeleton, no such joint); empty: none
        std::string joint;   // a character's: the joint it rides
        DVec3 position{};    // where it is drawn (as last placed; never placed: at its entity, a character's joint at rest)
        Quat rotation{};
        u32 capsules = 0;    // of its body, felt
        u32 strands = 0;
        u32 guides = 0;
        float longest = 0.0f;
        Vec3 bounds_min{};
        Vec3 bounds_max{};
        std::span<const Vec3> guide_points; // as simulated (or at rest), the groom's space
    };
    bool view(world::EntityId id, View& out) const;

private:
    struct Grown; // a job's result
    void follow_one(const world::Scene& scene, world::EntityId id);
    void drop(world::EntityId id);
    void shed(Worn& worn);
    void rank();
    void resolve(Worn& worn);
    void grow(Worn& worn);
    void take(Worn& worn);
    void swap(Worn& worn, bool now);
    void list_draws();

    render::Renderer* m_renderer;
    const MeshLibrary* m_meshes;
    u64 m_meshes_seen = ~u64{0}; // the library's version as last resolved
    std::map<world::EntityId, std::unique_ptr<Worn>> m_worn;
    std::vector<std::shared_ptr<Grown>> m_abandoned; // jobs of hair since removed: kept until they finish
    std::vector<render::HairDraw> m_draws;
    std::vector<std::pair<double, Worn*>> m_near; // scratch
    std::vector<Worn*> m_stepped;                 // scratch
    u64 m_scene = 0; // identity as last followed
    u32 m_simulated = 0;
    u32 m_left_out = 0;
    u64 m_resets = 0; // of simulations since dropped
    // (Last, so the workers are gone before what their jobs may touch.)
    JobSystem m_jobs{2};   // grows grooms
    JobSystem m_movers{2}; // steps simulations (apart: a thread waiting on a
                           // pool runs its queued jobs, and growing a groom takes a second)
};

} // namespace giant::editor
