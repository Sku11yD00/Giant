// The editor's animation: skinned mesh entities playing their clips.
//
// An entity whose mesh is a skinned file (MeshLibrary::Mesh::skinned) and
// that has a clip to play (its saved Entity::animation, or one played by
// command) is animated: it gets its own copy of the character in the
// renderer (render::Renderer::add_skinned_mesh), posed each frame, which
// it draws instead of the file's shared rest-pose mesh once the copy is
// loaded. Copies of one file share its geometry in the renderer (the
// first uploads it, the rest are Renderer::add_skinned_copy). At most
// k_max_animated are animated at once: those animating
// keep their place, and a free one goes to the lowest id waiting; the rest
// stand frozen in the rest pose (as do skinned entities with nothing to
// play, or whose saved clip the file does not have). Copies are made a few
// a frame (making one uploads it), so loading a scene full of characters
// never stalls one frame.
//
// View state, as the physics' play button: what plays by command
// (play_animation, blend_animations) and where each clip is in time are
// not saved or undoable (a command's play lasts until set_animation, the
// entity hidden or drawing another file, or a new scene); the scene keeps
// only each entity's own clip, speed and loop (set_animation), which it
// starts playing again whenever that changes. Time moves on by the frame's time unless paused (step_animation
// moves it by hand); a clip started this frame starts at its start. Poses
// are worked out on worker threads and handed to the renderer on the main
// thread, only when they changed (a pose the same as the last one sent is
// not sent: it would redraw the shadows under it).
#pragma once

#include "mesh_library.h"

#include "giant/anim/player.h"
#include "giant/core/jobs.h"
#include "giant/render/renderer.h"
#include "giant/world/scene.h"

#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace giant::editor {

class AnimationState {
public:
    static constexpr usize k_max_animated = 64;

    // Without a renderer (tests) poses are worked out but drawn by nothing.
    explicit AnimationState(render::Renderer* renderer);
    ~AnimationState(); // the copies leave the renderer
    AnimationState(const AnimationState&) = delete;
    AnimationState& operator=(const AnimationState&) = delete;

    // Each frame, with what the scene changed since the last call (the
    // scene frame's changes for the animation): finds the entities to
    // animate, makes copies (at most copies_per_frame), moves time on by
    // `seconds` unless paused and poses what changed.
    void update(const world::Scene& scene, const MeshLibrary& meshes, const world::SceneChanges& changed,
                double seconds);
    u32 copies_per_frame = 4;

    bool paused() const { return m_paused; }
    void set_paused(bool paused) { m_paused = paused; }
    // Moves every animated entity's time on by `seconds` now (paused or
    // not), and poses them.
    void step(double seconds);
    // The scene's changes followed now, as update would (copies made, poses
    // worked out), without moving time on: a command's edit, or what the
    // scene has not handed out yet, before the scene frame is built (so no
    // frame draws an entity with a copy it should no longer have).
    void follow_now(const world::Scene& scene, const MeshLibrary& meshes, const world::SceneChanges& changed);
    // The entity's saved animation started over now, whatever a command
    // played (set_animation, even when the value saved is the same).
    void restart_saved(const world::Scene& scene, world::EntityId id);

    // By command. Each makes the entity animated now if it can be (the
    // copy made at once), and returns an error otherwise: no such entity,
    // not a skinned mesh, no such clip, or k_max_animated animated already.
    // Plays `clip` from `start` seconds (negative: from its start, or its
    // end when playing once backwards), fading from what showed over
    // `blend_seconds`, at `speed`; until the entity's saved animation is set,
    // the entity is hidden or draws another file (or the file is imported
    // again), or the scene is replaced. Both pose it at once.
    std::string play(const world::Scene& scene, const MeshLibrary& meshes, world::EntityId id, const std::string& clip,
                     float blend_seconds, bool loop, float speed, float start);
    // Holds clips `a` and `b` blended by `weight` (0 all a), b's time
    // following a's when `sync`.
    std::string blend(const world::Scene& scene, const MeshLibrary& meshes, world::EntityId id, const std::string& a,
                      const std::string& b, float weight, bool sync);

    struct Animated {
        world::EntityId id = 0;
        const MeshLibrary::Mesh* mesh = nullptr;
        std::shared_ptr<const MeshLibrary::Skinned> skinned; // as copied
        render::MeshId copy = render::k_no_mesh;
        anim::AnimationPlayer player;
        world::EntityAnimation saved; // the entity's own, as last started
        bool by_command = false;      // playing what a command said, not `saved`
        // Worked out on a worker thread.
        anim::PoseScratch scratch;
        std::vector<anim::JointTransform> local;
        std::vector<Mat4> model;
        std::vector<Mat4> skin;
        geometry::Sphere bounds;
        bool dirty = true;     // its pose must be worked out again
        bool posed = false;    // at least once
        bool drawn = false;    // its copy drawn (loaded, posed), in copies()
        bool fresh = true;     // (re)started this update: time moves on from the next
        std::vector<Mat4> sent; // the pose last handed to the renderer
        geometry::Sphere sent_bounds;
        u64 poses = 0;         // handed to the renderer
        u64 pose_serial = 0;   // when it last changed (as skinned()'s serials: never the same twice)
    };
    // The animated entity, or null (frozen, or not animated at all).
    const Animated* find(world::EntityId id) const;
    // What draws each animated entity instead of its file's mesh (the
    // scene frame's skinned meshes); and which to draw again (taken by the frame).
    const std::unordered_map<world::EntityId, render::MeshId>& copies() const { return m_copies; }
    std::vector<world::EntityId> take_redraw();

    usize animated() const { return m_animated.size(); }
    // Every visible entity drawing a skinned file (animated, frozen or with
    // nothing to play), with a serial that changes whenever it does (its
    // transform, its file): what hit shapes follow.
    const std::unordered_map<world::EntityId, u64>& skinned() const { return m_skinned; }
    // Moves on whenever skinned() or a pose_serial does (nothing changed: the same).
    u64 serial() const { return m_serial; }
    // Whether copies are drawn (a renderer): an animated entity shows its
    // pose once `drawn`, the file's rest pose until then.
    bool draws() const { return m_renderer != nullptr; }
    // Entities that would animate past the limit (standing in the rest pose).
    usize frozen() const;
    usize waiting() const; // to be copied (within the limit), a few a frame
    u64 poses() const { return m_poses; } // handed to the renderer, ever
    // Why the renderer refused the entity's copy (empty: it did not); not
    // tried again until the entity or its file changes.
    std::string refused(world::EntityId id) const;

private:
    // The skinned file an entity draws, or null.
    static const MeshLibrary::Mesh* skinned_mesh(const world::Entity& e, const MeshLibrary& meshes);
    // Whether the entity should animate now (a skinned mesh with something to play).
    bool wants(const world::Entity& e, const MeshLibrary& meshes) const;
    // `changed`: the entity itself changed (a refusal is tried again).
    void follow(const world::Scene& scene, const MeshLibrary& meshes, world::EntityId id, bool changed);
    void follow_all(const world::Scene& scene, const MeshLibrary& meshes);
    // The entity animated now (its copy made); null when it cannot be.
    Animated* start(const world::Scene& scene, const MeshLibrary& meshes, world::EntityId id, std::string& error);
    void stop(world::EntityId id);
    // The saved animation started over, fading over `fade` seconds (the
    // rest pose when it has none).
    static void play_saved(Animated& a, float fade);
    // Where a clip starts: its end when played once backwards.
    static float start_of(const anim::Clip& clip, bool loop, float speed);
    // Animated now (started if not yet), for a command.
    Animated* animate_now(const world::Scene& scene, const MeshLibrary& meshes, world::EntityId id,
                          std::string& error);
    void fill(const world::Scene& scene, const MeshLibrary& meshes);
    void advance(double seconds);
    void pose();

    render::Renderer* m_renderer;
    JobSystem m_jobs;
    std::set<world::EntityId> m_wanted;   // would animate (lowest ids get to)
    std::set<world::EntityId> m_commanded; // playing by command (kept wanted)
    std::map<world::EntityId, std::unique_ptr<Animated>> m_animated;
    std::unordered_map<world::EntityId, render::MeshId> m_copies;
    // Per skinned file (as copied), its live copies in the renderer: a new
    // one shares their geometry (Renderer::add_skinned_copy).
    std::unordered_map<const MeshLibrary::Skinned*, std::vector<render::MeshId>> m_copies_of;
    std::vector<world::EntityId> m_redraw;
    std::vector<Animated*> m_work; // scratch
    struct Refusal {
        std::shared_ptr<const MeshLibrary::Skinned> skinned; // the file refused
        std::string error;
    };
    std::unordered_map<world::EntityId, Refusal> m_refused;
    std::unordered_map<world::EntityId, u64> m_skinned;
    u64 m_serial = 0;              // skinned() and pose_serial
    u64 m_scene = 0;               // identity as last followed
    u64 m_meshes_version = ~u64{0};
    bool m_paused = false;
    u64 m_poses = 0;
};

} // namespace giant::editor
