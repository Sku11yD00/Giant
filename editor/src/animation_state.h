// The editor's animation: skinned mesh entities playing their clips.
//
// An entity whose mesh is a skinned file (MeshLibrary::Mesh::skinned) and
// that has a clip to play (its saved Entity::animation, or one played by
// command) is animated: it gets its own copy of the character in the
// renderer (render::Renderer::add_skinned_mesh), posed as time moves it, which
// it draws instead of the file's shared rest-pose mesh once the copy is
// loaded. Copies of one file share its geometry in the renderer (the
// first uploads it, the rest are Renderer::add_skinned_copy). At most
// max_animated (k_max_animated) are animated at once: those animating
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
// not sent: it would redraw the shadows under it), and how often by size on
// screen (animation LOD: pose_interval, within poses_per_frame).
//
// Motion matching. An entity whose saved animation names clips to be
// `matched` among is posed by a matcher (anim/matcher.h) instead: it plays
// whichever of them fits the way it is going (its goal, set each frame by
// whoever moves it; none set: standing). The clips' database is built once
// per file and list of clips, on a worker of its own (never the main
// thread), and shared by every entity of that file and list; until it is
// built, or when the file cannot be matched (Motion::problem), the saved
// `clip` plays as ever (one with no such clip then animates no more: it
// stands in its rest pose and takes no place). A command's play takes over
// (a cut: nothing fades between the matcher and a clip). The matcher is
// moved on when the entity is posed (so by animation LOD, on the pose
// workers): a far crowd searches four times a second each, not every
// frame; at most a second at once (a longer step_animation moves a matched
// entity on by one). Entities come to matching a few a frame
// (matches_per_frame), so a crowd whose database arrives is not all posed
// and searched on one frame. Morph weights rest while matching (the
// matched clips' morph channels do not play).
#pragma once

#include "mesh_library.h"

#include "giant/anim/matcher.h"
#include "giant/anim/player.h"
#include "giant/core/jobs.h"
#include "giant/render/renderer.h"
#include "giant/world/scene.h"

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace giant::editor {

class AnimationState {
public:
    static constexpr usize k_max_animated = 4096;
    usize max_animated = k_max_animated; // (tests lower it)
    // Animation LOD: how often a character is posed, by its size on screen
    // (pixels of its pose sphere's radius): every frame near, down to four
    // times a second far. Time still moves on every frame; only working out
    // and sending the pose waits.
    static double pose_interval(float pixels);
    // Poses worked out per frame for characters posed less than every
    // frame (most overdue first, a quarter of it kept for them however
    // many are posed every frame); those posed every frame, and poses a
    // command or a (re)start asks for, always go.
    u32 poses_per_frame = 256;
    // The view whose screen sizes decide (each frame, before update). Never
    // set (tests): every character is posed every frame.
    void set_view(const render::View& view, u32 height_pixels);

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
    // not a skinned mesh, no such clip, or max_animated animated already.
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
    // Holds morph targets of the entity's file at weights of its own, over
    // what its clips and rest weights give (a face held smiling while it
    // walks): by name, a weight (within +-k_max_morph_set) or released
    // (nullopt); `release_all` releases every one first. View state, as
    // play: it lasts as long as a command's play would.
    static constexpr float k_max_morph_set = 10.0f;
    std::string set_morphs(const world::Scene& scene, const MeshLibrary& meshes, world::EntityId id,
                           std::span<const std::pair<std::string, std::optional<float>>> weights, bool release_all);

    // How a matched entity is being moved, in its own space: metres a
    // second across (+x) and along (+z) its facing, and how far it will
    // have turned (radians about +y, +z toward +x) over the next second.
    // (Its mover does not wait for its clips: they catch up with it.)
    struct MotionGoal {
        float across = 0.0f;
        float along = 0.0f;
        float turn = 0.0f;
    };
    // For the next update (each frame, before it); one not set that frame
    // stands. Numbers that are none count as 0; an entity not animated
    // takes none.
    void set_goal(world::EntityId id, const MotionGoal& goal);
    // A walker's goal: its velocity in the world (as steered) seen from
    // where it faces (`yaw` radians about +y, from +z toward +x).
    static MotionGoal goal_of(const Vec3& velocity, float yaw);
    u32 matches_per_frame = 16;
    // Waits for the databases being built, held or not (tests; a frame never does).
    void finish_motions();
    // Builds wait until let go again (tests: what shows meanwhile).
    void hold_builds(bool hold) { m_hold->store(hold, std::memory_order_release); }
    // Why `e`'s file cannot be matched among its clips, once that is known (else empty).
    std::string motion_problem(const world::Entity& e, const MeshLibrary& meshes) const;
    usize motions() const { return m_motions.size(); } // databases held (built or building)

    // A skinned file's clips as a motion database, shared by the entities matched among them.
    struct Motion {
        std::shared_ptr<const MeshLibrary::Skinned> skinned;
        std::vector<std::string> names; // as the entities' `matched` has them
        std::atomic<bool> ready{false};
        // The build's, read once ready:
        anim::MotionDatabase db;
        std::string problem;              // why the file cannot be matched (empty: it can)
        std::vector<std::string> missing; // names the file has no clip for (left out)
        // The main thread's:
        u32 users = 0;
        bool reported = false;
    };

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
        bool dirty = true;     // its pose must be worked out again now
        bool changed = false;  // time moved it since its last pose (posed when due)
        double since_pose = 0.0; // seconds since its pose was last worked out
        double interval = 0.0;   // seconds between poses, by its size on screen
        float pixels = 0.0f;     // its size on screen (pose sphere radius), last update
        bool posed = false;    // at least once
        bool drawn = false;    // its copy drawn (loaded, posed), in copies()
        bool fresh = true;     // (re)started this update: time moves on from the next
        std::vector<Mat4> sent; // the pose last handed to the renderer
        geometry::Sphere sent_bounds;
        // Morph weights (one per target of the file; none without):
        // worked out with the pose, those held by set_morphs over them.
        std::vector<float> morphs;
        std::vector<float> held;      // set_morphs' weights
        std::vector<u8> holding;      // per target: held
        std::vector<float> sent_morphs;
        // Matching: its clips' database (building, or built), the matcher
        // posing it once that is ready (and no command plays), where it
        // should be going and the time the matcher is yet to be moved on.
        std::shared_ptr<Motion> motion;
        bool matching = false;
        anim::MotionMatcher matcher;
        MotionGoal goal;
        bool goal_set = false; // since the last frame's update
        double match_seconds = 0.0;
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
    u32 posed_last() const { return m_posed_last; }   // worked out by the last frame's update
    u32 overdue() const { return m_overdue; }         // due but left for a later frame (the budget)
    // Why the renderer refused the entity's copy (empty: it did not); not
    // tried again until the entity or its file changes.
    std::string refused(world::EntityId id) const;
    // Matched entities' state: "" (not matched at all), "building" (its
    // database), "ready" (built: it comes to matching within a few
    // frames), "matching", "command" (a command plays meanwhile) or
    // "problem" (the file cannot be matched: Motion::problem).
    static const char* motion_state(const Animated& a);

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
    // Its database goes with it: found (or its build begun) for the saved
    // `matched`, the old one let go.
    void play_saved(Animated& a, float fade);
    void release_motion(Animated& a);
    // Each animated entity matching or not, as its database and commands have it now.
    void match();
    // Where a clip starts: its end when played once backwards.
    static float start_of(const anim::Clip& clip, bool loop, float speed);
    // Animated now (started if not yet), for a command.
    Animated* animate_now(const world::Scene& scene, const MeshLibrary& meshes, world::EntityId id,
                          std::string& error);
    void fill(const world::Scene& scene, const MeshLibrary& meshes);
    void advance(double seconds);
    void lod(const world::Scene& scene); // each one's screen size and pose interval
    // The frame's update (by_frame), or follow_now (seconds 0).
    void run(const world::Scene& scene, const MeshLibrary& meshes, const world::SceneChanges& changed,
             double seconds, bool by_frame);
    // Which timed poses (those posed less than every frame) go: none (a
    // command, follow_now: only what is asked for now), those due (a frame
    // that moved time on), or all that moved since their last pose, budget
    // allowing (a frame where time stood still: paused, a menu open).
    enum class Timed : u8 { none, frame, still };
    void pose(Timed timed);

    render::Renderer* m_renderer;
    JobSystem m_jobs;
    // Databases are built on a worker of their own: the pose jobs' pool is
    // waited on by the main thread, which would then run a build itself.
    WaitGroup m_building;
    JobSystem m_build_jobs;
    // (One that cannot be matched is kept after its last entity, until a
    // new scene: it is not built again each time one asks.)
    std::map<std::pair<const MeshLibrary::Skinned*, std::vector<std::string>>, std::shared_ptr<Motion>> m_motions;
    std::shared_ptr<std::atomic<bool>> m_hold = std::make_shared<std::atomic<bool>>(false);
    std::vector<world::EntityId> m_unmatched; // scratch
    std::set<world::EntityId> m_wanted;   // would animate (lowest ids get to)
    std::set<world::EntityId> m_commanded; // playing by command (kept wanted)
    std::map<world::EntityId, std::unique_ptr<Animated>> m_animated;
    std::unordered_map<world::EntityId, render::MeshId> m_copies;
    // Per skinned file (as copied), its live copies in the renderer: a new
    // one shares their geometry (Renderer::add_skinned_copy).
    std::unordered_map<const MeshLibrary::Skinned*, std::vector<render::MeshId>> m_copies_of;
    std::vector<world::EntityId> m_redraw;
    std::vector<Animated*> m_work; // scratch
    std::vector<Animated*> m_due;  // scratch
    bool m_view_set = false;
    DVec3 m_eye{};
    float m_pixels_per_unit = 0.0f;
    u32 m_posed_last = 0;
    u32 m_overdue = 0;
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
