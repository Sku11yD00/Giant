// Baked poses: a character held in one pose as a small plain mesh (crowds
// plan, CR1; giant/geometry/pose_bake.h). A distant person needs no
// skeleton: it will be drawn as an instance of one of these. A pose is
// asked for by its file, a clip and a time in it ("" the rest pose), and
// is baked on a worker: the file's skinned mesh cut down once to a few
// thousand triangles (kept for the file's other poses of that size), moved
// into the pose, built into clusters; the main thread then only hands the
// result to the renderer, a few meshes a frame (`finish`, a command's
// `wait`, is the exception: the caller's thread helps bake and hands
// everything over at once). A file drawn in parts (its materials) is baked
// part by part, the parts meeting where they met while each can be cut to
// its share that way (`seams_kept`).
//
// Bounds: at most k_max_poses are kept (asking for more fails), a figure
// is cut to `triangles` (k_min_triangles to k_max_triangles) and
// `uploads_a_frame` meshes go to the renderer a frame. A file imported
// again has its poses baked again from the new file (they show nothing
// meanwhile); one whose file has lost the clip, or is no longer skinned,
// fails. A failed pose asked for again is tried again.
//
// View state: not saved, not undone.
#pragma once

#include "mesh_library.h"

#include "giant/core/jobs.h"
#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/geometry/clusters.h"
#include "giant/geometry/pose_bake.h"
#include "giant/render/renderer.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace giant::editor {

class PoseBakes {
public:
    static constexpr usize k_max_poses = 256;
    static constexpr u32 k_min_triangles = 64;
    static constexpr u32 k_max_triangles = 20000;
    static constexpr u32 k_default_triangles = 3000;
    static constexpr u32 k_no_pose = ~0u;

    u32 uploads_a_frame = 2; // meshes handed to the renderer a frame (1 to 64)
    // ... and, after a frame's first, only while this many triangles are not
    // passed (an upload costs by its size: the largest figure, 20,000
    // triangles in one part, is a frame's whole share alone).
    u32 triangles_a_frame = 8000;

    enum class State : u8 { baking, ready, failed };
    static const char* to_string(State state);

    struct Pose {
        std::string mesh;  // the file, as entities name it
        std::string clip;  // "": the rest pose
        float time = 0.0f; // seconds into the clip (within it)
        u32 triangles = k_default_triangles; // the whole figure is cut to about this
        State state = State::baking;
        std::string problem; // failed: why
        // Ready: one mesh for the file, or one for each of its parts in
        // their order (MeshLibrary::Mesh::parts); k_no_mesh each without a
        // renderer, and for a part cut to nothing.
        std::vector<render::MeshId> meshes;
        u32 baked_triangles = 0;  // in all its meshes
        u32 source_triangles = 0; // the file's
        float error = 0.0f;       // metres the cut moved the surface (bind pose): the worst part's, as the simplifier measured it
        bool seams_kept = true;   // false: a part's joins with the others were let go to reach its triangles
        Vec3 center{};            // a sphere round the posed figure, the file's space
        float radius = 0.0f;
        float seconds = 0.0f;     // the worker's time for it
    };

    // Without a renderer (tests) poses are baked and measured only.
    PoseBakes(render::Renderer* renderer, const MeshLibrary* meshes);
    ~PoseBakes(); // the meshes leave the renderer, then bakes still running end
    PoseBakes(const PoseBakes&) = delete;
    PoseBakes& operator=(const PoseBakes&) = delete;

    // The pose's index, baking begun unless it is kept already (the same
    // file, clip, time to the millisecond and triangles; a failed one is
    // begun again). k_no_pose with
    // `error`: the file is not loaded or not skinned, it has no such clip,
    // numbers out of range, or k_max_poses are kept.
    u32 want(std::string_view mesh, std::string_view clip, float time, u32 triangles, std::string& error);
    // Each frame: finished bakes' meshes go to the renderer, and poses of
    // files imported again are baked again.
    void update();
    // Waits for every bake and hands all of it over at once (tests, a command's `wait`).
    void finish();
    // Every pose forgotten, their meshes out of the renderer (indices begin again).
    void clear();

    const std::vector<Pose>& poses() const { return m_poses; }
    usize baking() const;
    u64 baked() const { return m_baked; } // bakes finished (failed ones too), ever
    u64 clears() const { return m_clears; } // times every pose was forgotten: an index kept from before is another pose's
    u64 cuts() const { return m_cuts; }   // figures cut down, ever (as seen when a pose of theirs was done)

private:
    // A file's figure: its skinned mesh cut down to one size, part by
    // part. Made once (by the first bake that needs it, on its worker) and
    // kept for the file's other poses of that size, until no pose is of
    // that file (as it was imported) any more.
    struct Figure {
        std::shared_ptr<const MeshLibrary::Skinned> skinned;
        u32 triangles = 0;
        std::mutex making;
        bool made = false;
        std::vector<geometry::SkinnedTriangleMesh> parts;
        float error = 0.0f;
        bool seams_kept = true;
        bool counted = false; // main thread: in cuts()
    };
    // One bake, shared with its job (which may outlive the pose).
    struct Bake {
        std::shared_ptr<Figure> figure;
        u32 clip = 0; // anim::k_no_clip: the rest pose
        float time = 0.0f;
        std::atomic<bool> done{false};
        std::vector<geometry::ClusterMesh> parts; // the job's result
        u32 baked_triangles = 0;
        Vec3 center{};
        float radius = 0.0f;
        float seconds = 0.0f;
        std::string problem;
    };
    struct Slot {
        std::shared_ptr<Bake> bake;                         // baking: its job's
        std::shared_ptr<const MeshLibrary::Skinned> skinned; // the file as it was baked from
        usize handed = 0;                                    // of the bake's parts, to the renderer
    };
    bool begin(usize index, std::string& error);
    void refresh(); // poses of files imported again are begun again
    void release(usize index);
    static void run(Bake& bake);
    static void make(Figure& figure);

    render::Renderer* m_renderer;
    const MeshLibrary* m_meshes;
    u64 m_meshes_version = 0;
    std::vector<Pose> m_poses;
    std::vector<Slot> m_slots; // beside m_poses
    std::vector<std::shared_ptr<Figure>> m_figures;
    u64 m_baked = 0;
    u64 m_cuts = 0;
    u64 m_clears = 0;
    WaitGroup m_group;
    JobSystem m_jobs{2}; // last: destroyed (finishing its jobs) first
};

} // namespace giant::editor
