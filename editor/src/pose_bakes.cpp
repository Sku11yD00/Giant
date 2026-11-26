#include "pose_bakes.h"

#include "giant/anim/motion.h"
#include "giant/anim/player.h"
#include "giant/anim/skeleton.h"
#include "giant/geometry/mesh.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace giant::editor {

namespace {

// The original triangles of a skinned file's clusters (which hold every
// level of detail's): those of its depth 0 groups.
std::vector<u32> original_triangles(const geometry::ClusterMesh& mesh) {
    std::vector<u32> indices;
    indices.reserve(usize{mesh.source_triangles} * 3);
    for (const geometry::Cluster& c : mesh.clusters) {
        if (c.group >= mesh.groups.size() || mesh.groups[c.group].depth != 0) continue;
        const usize first = c.first_index, count = usize{c.triangle_count} * 3;
        if (first + count > mesh.indices.size()) continue;
        indices.insert(indices.end(), mesh.indices.begin() + static_cast<std::ptrdiff_t>(first),
                       mesh.indices.begin() + static_cast<std::ptrdiff_t>(first + count));
    }
    return indices;
}

// Poses are told apart to the millisecond.
i64 milliseconds(float seconds) { return std::llround(static_cast<double>(seconds) * 1000.0); }

} // namespace

const char* PoseBakes::to_string(State state) {
    switch (state) {
    case State::baking: return "baking";
    case State::ready: return "ready";
    case State::failed: return "failed";
    }
    return "failed";
}

PoseBakes::PoseBakes(render::Renderer* renderer, const MeshLibrary* meshes)
    : m_renderer(renderer), m_meshes(meshes), m_meshes_version(meshes != nullptr ? meshes->version() : 0) {
    m_poses.reserve(k_max_poses); // (a pose stays where it is while more are asked for)
    m_slots.reserve(k_max_poses);
}

PoseBakes::~PoseBakes() {
    for (usize i = 0; i < m_poses.size(); ++i) release(i);
}

usize PoseBakes::baking() const {
    return static_cast<usize>(std::count_if(m_poses.begin(), m_poses.end(),
                                            [](const Pose& p) { return p.state == State::baking; }));
}

void PoseBakes::release(usize index) {
    if (m_renderer != nullptr) {
        for (const render::MeshId id : m_poses[index].meshes) m_renderer->remove_mesh(id); // (k_no_mesh is ignored)
    }
    m_poses[index].meshes.clear();
    m_slots[index] = {};
}

void PoseBakes::clear() {
    for (usize i = 0; i < m_poses.size(); ++i) release(i);
    m_poses.clear();
    m_slots.clear();
    m_figures.clear(); // (a bake still running keeps its own)
    ++m_clears;
}

void PoseBakes::make(Figure& figure) {
    const MeshLibrary::Skinned& skinned = *figure.skinned;
    std::vector<const geometry::SkinnedClusterMesh*> sources;
    if (skinned.parts.empty()) {
        sources.push_back(&skinned.clusters);
    } else {
        for (const geometry::SkinnedClusterMesh& part : skinned.parts) sources.push_back(&part);
    }
    // Each part at bind, in the shape it rests in (its morph targets at their rest weights).
    std::vector<std::vector<geometry::Vertex>> vertices(sources.size());
    std::vector<std::vector<u32>> indices(sources.size());
    usize total = 0;
    for (usize p = 0; p < sources.size(); ++p) {
        vertices[p] = sources[p]->mesh.vertices;
        std::vector<float> rest;
        for (const geometry::MorphTarget& t : sources[p]->morphs) rest.push_back(t.weight);
        geometry::apply_morphs(vertices[p], sources[p]->morphs, rest);
        indices[p] = original_triangles(sources[p]->mesh);
        total += indices[p].size() / 3;
    }
    // Parts meet where they met: those corners stay while the rest can.
    std::vector<std::vector<u8>> seams(sources.size());
    if (sources.size() > 1) {
        std::vector<std::span<const geometry::Vertex>> spans(vertices.begin(), vertices.end());
        seams = geometry::seams_between(spans);
    }
    figure.parts.resize(sources.size());
    for (usize p = 0; p < sources.size(); ++p) {
        // Its share of the figure's triangles, by how many it has.
        const double share = total > 0 ? static_cast<double>(indices[p].size() / 3) / static_cast<double>(total) : 0.0;
        const u32 target = std::max<u32>(static_cast<u32>(std::lround(share * figure.triangles)), 16);
        geometry::SimplifiedSkin small =
            geometry::simplify_skinned(vertices[p], indices[p], sources[p]->weights, target, seams[p]);
        figure.error = std::max(figure.error, small.error);
        figure.seams_kept = figure.seams_kept && !small.unlocked;
        figure.parts[p] = std::move(small.mesh);
    }
    figure.made = true;
}

namespace {

// Where the hips are over the ground at `time` of a clip, in the file's space.
Vec3 hips_at(const MeshLibrary::Skinned& skinned, const anim::Clip& clip, u32 hips, float time,
             std::vector<anim::JointTransform>& local, std::vector<Mat4>& model) {
    local = skinned.skeleton.rest;
    local.resize(skinned.skeleton.size());
    model.resize(skinned.skeleton.size());
    anim::sample(skinned.skeleton, clip, time, local);
    anim::model_matrices(skinned.skeleton, local, model);
    return {model[hips].at(0, 3), 0.0f, model[hips].at(2, 3)};
}

} // namespace

Vec3 PoseBakes::travel(const MeshLibrary::Skinned& skinned, u32 clip) {
    const u32 hips = anim::find_motion_joints(skinned.skeleton).hips;
    if (clip >= skinned.clips.size() || hips >= skinned.skeleton.size()) return {};
    std::vector<anim::JointTransform> local;
    std::vector<Mat4> model;
    const Vec3 from = hips_at(skinned, skinned.clips[clip], hips, 0.0f, local, model);
    const Vec3 to = hips_at(skinned, skinned.clips[clip], hips, skinned.clips[clip].duration, local, model);
    return to - from;
}

void PoseBakes::run(Bake& bake) {
    const auto start = std::chrono::steady_clock::now();
    Figure& figure = *bake.figure;
    {
        const std::lock_guard<std::mutex> lock(figure.making); // (the file's other poses wait for the one figure)
        if (!figure.made) make(figure);
    }
    const MeshLibrary::Skinned& skinned = *figure.skinned;
    const usize joints = skinned.skeleton.size();
    std::vector<anim::JointTransform> local = skinned.skeleton.rest;
    local.resize(joints);
    if (bake.clip < skinned.clips.size()) anim::sample(skinned.skeleton, skinned.clips[bake.clip], bake.time, local);
    std::vector<Mat4> model(joints), skin(joints);
    anim::model_matrices(skinned.skeleton, local, model);
    if (bake.in_place && bake.clip < skinned.clips.size() && skinned.clips[bake.clip].duration > 0.0f) {
        // What the clip has carried it by now is taken out: the share of its whole travel the time is of
        // its length (a walk goes on steadily: its sway about that stays).
        const Vec3 back = travel(skinned, bake.clip) * (-bake.time / skinned.clips[bake.clip].duration);
        for (Mat4& m : model) {
            m.at(0, 3) += back.x;
            m.at(2, 3) += back.z;
        }
    }
    anim::skin_matrices(skinned.skeleton, model, skin);
    Vec3 lo{}, hi{};
    bool any = false;
    bake.parts.resize(figure.parts.size());
    // Every part posed first: a corner where two parts meet grows one way
    // in both (geometry::Growing), so the others' vertices count there.
    std::vector<geometry::TriangleMesh> posed_parts(figure.parts.size());
    std::vector<geometry::Vertex> whole;
    for (usize p = 0; p < figure.parts.size(); ++p) {
        posed_parts[p] = geometry::bake_pose(figure.parts[p], skin);
        whole.insert(whole.end(), posed_parts[p].vertices.begin(), posed_parts[p].vertices.end());
    }
    for (usize p = 0; p < figure.parts.size(); ++p) {
        const geometry::TriangleMesh& posed = posed_parts[p];
        for (const geometry::Vertex& v : posed.vertices) {
            const Vec3& at = v.position;
            lo = any ? Vec3{std::min(lo.x, at.x), std::min(lo.y, at.y), std::min(lo.z, at.z)} : at;
            hi = any ? Vec3{std::max(hi.x, at.x), std::max(hi.y, at.y), std::max(hi.z, at.z)} : at;
            any = true;
        }
        bake.baked_triangles += static_cast<u32>(posed.indices.size() / 3);
        // (empty when the part came to nothing)
        bake.parts[p] = geometry::build_clusters(posed, {}, geometry::Growing{true, whole});
    }
    if (!any || bake.baked_triangles == 0) {
        bake.problem = "nothing was left of its mesh";
    } else {
        bake.center = (lo + hi) * 0.5f;
        bake.radius = length(hi - lo) * 0.5f;
    }
    bake.seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
    bake.done.store(true, std::memory_order_release);
}

bool PoseBakes::begin(usize index, std::string& error) {
    Pose& pose = m_poses[index];
    const MeshLibrary::Mesh* mesh = m_meshes != nullptr ? m_meshes->find(pose.mesh) : nullptr;
    if (mesh == nullptr || mesh->skinned == nullptr) {
        error = pose.mesh + (mesh == nullptr ? " is not loaded (import_mesh)" : " is not a skinned mesh");
        return false;
    }
    u32 clip = anim::k_no_clip;
    if (!pose.clip.empty()) {
        clip = mesh->skinned->find_clip(pose.clip);
        if (clip == anim::k_no_clip) {
            error = pose.mesh + " has no clip \"" + pose.clip + "\"";
            return false;
        }
        // (Within the clip: every time past its end is its last pose. A file
        // imported again may have made the clip shorter.)
        pose.time = std::min(pose.time, mesh->skinned->clips[clip].duration);
    }
    std::shared_ptr<Figure> figure;
    for (const std::shared_ptr<Figure>& f : m_figures) {
        if (f->skinned == mesh->skinned && f->triangles == pose.triangles) figure = f;
    }
    if (figure == nullptr) {
        figure = std::make_shared<Figure>();
        figure->skinned = mesh->skinned;
        figure->triangles = pose.triangles;
        m_figures.push_back(figure);
    }
    Slot& slot = m_slots[index];
    slot.skinned = mesh->skinned;
    slot.handed = 0;
    slot.bake = std::make_shared<Bake>();
    slot.bake->figure = std::move(figure);
    slot.bake->clip = clip;
    slot.bake->time = pose.time;
    slot.bake->in_place = pose.in_place;
    pose.state = State::baking;
    pose.problem.clear();
    pose.source_triangles = mesh->triangles;
    m_jobs.submit(m_group, [bake = slot.bake] { run(*bake); });
    return true;
}

u32 PoseBakes::want(std::string_view mesh, std::string_view clip, float time, u32 triangles, std::string& error,
                    bool in_place) {
    if (triangles < k_min_triangles || triangles > k_max_triangles) {
        error = "triangles must be " + std::to_string(k_min_triangles) + " to " + std::to_string(k_max_triangles);
        return k_no_pose;
    }
    if (!std::isfinite(time) || time < 0.0f || time > anim::k_max_clip_seconds) {
        error = "time must be 0 to " + std::to_string(static_cast<int>(anim::k_max_clip_seconds)) + " seconds";
        return k_no_pose;
    }
    refresh(); // (a file imported again this frame: its poses are of the old one)
    const std::string path = MeshLibrary::normalize(mesh);
    const MeshLibrary::Mesh* loaded = m_meshes != nullptr ? m_meshes->find(path) : nullptr;
    if (loaded != nullptr && loaded->skinned != nullptr && !clip.empty()) {
        // (Within the clip: every time past its end is its last pose, one bake.)
        const u32 index = loaded->skinned->find_clip(clip);
        if (index != anim::k_no_clip) time = std::min(time, loaded->skinned->clips[index].duration);
    }
    if (clip.empty()) {
        time = 0.0f;
        in_place = false; // (the rest pose has gone nowhere)
    }
    for (usize i = 0; i < m_poses.size(); ++i) {
        const Pose& p = m_poses[i];
        if (p.mesh != path || p.clip != clip || milliseconds(p.time) != milliseconds(time) || p.triangles != triangles ||
            p.in_place != in_place) {
            continue;
        }
        if (p.state == State::failed) {
            // Asked for again, a failed pose is tried again where it is.
            release(i);
            if (!begin(i, error)) {
                m_poses[i].problem = error;
                return k_no_pose;
            }
        }
        return static_cast<u32>(i);
    }
    if (m_poses.size() >= k_max_poses) {
        error = "as many poses are kept as can be (" + std::to_string(k_max_poses) + "): clear_baked_poses";
        return k_no_pose;
    }
    Pose pose;
    pose.mesh = path;
    pose.clip = std::string(clip);
    pose.time = time;
    pose.triangles = triangles;
    pose.in_place = in_place;
    m_poses.push_back(std::move(pose));
    m_slots.emplace_back();
    if (!begin(m_poses.size() - 1, error)) {
        m_poses.pop_back();
        m_slots.pop_back();
        return k_no_pose;
    }
    return static_cast<u32>(m_poses.size() - 1);
}

void PoseBakes::refresh() {
    if (m_meshes == nullptr || m_meshes->version() == m_meshes_version) return;
    m_meshes_version = m_meshes->version();
    // Files imported again: their poses baked again from the new file.
    for (usize i = 0; i < m_poses.size(); ++i) {
        const MeshLibrary::Mesh* mesh = m_meshes->find(m_poses[i].mesh);
        if (mesh != nullptr && mesh->skinned == m_slots[i].skinned) continue;
        release(i);
        std::string error;
        if (!begin(i, error)) {
            m_poses[i].state = State::failed;
            m_poses[i].problem = error;
        }
    }
    // The figures of files no pose is of any more are let go.
    std::erase_if(m_figures, [this](const std::shared_ptr<Figure>& f) {
        return std::none_of(m_slots.begin(), m_slots.end(), [&f](const Slot& s) { return s.skinned == f->skinned; });
    });
}

void PoseBakes::update() {
    refresh();
    u32 left = std::clamp<u32>(uploads_a_frame, 1, 64);
    // An upload's cost is its size: after a frame's first mesh, only what fits in the triangles left.
    u32 triangles_left = triangles_a_frame;
    bool any_uploaded = false;
    for (usize i = 0; i < m_poses.size() && left > 0; ++i) {
        Pose& pose = m_poses[i];
        Slot& slot = m_slots[i];
        if (pose.state != State::baking || slot.bake == nullptr || !slot.bake->done.load(std::memory_order_acquire)) continue;
        Bake& bake = *slot.bake;
        if (!bake.problem.empty()) {
            pose.state = State::failed;
            pose.problem = bake.problem;
            slot.bake.reset();
            ++m_baked;
            continue;
        }
        bool refused = false;
        while (slot.handed < bake.parts.size() && left > 0 && !refused) {
            geometry::ClusterMesh& part = bake.parts[slot.handed];
            render::MeshId id = render::k_no_mesh;
            if (m_renderer != nullptr && !part.empty()) {
                if (any_uploaded && part.source_triangles > triangles_left) {
                    left = 0; // the rest next frame
                    break;
                }
                triangles_left -= std::min(triangles_left, part.source_triangles);
                any_uploaded = true;
                id = m_renderer->add_mesh(std::move(part));
                refused = id == render::k_no_mesh;
                --left;
            }
            pose.meshes.push_back(id);
            ++slot.handed;
        }
        if (refused) {
            const std::shared_ptr<const MeshLibrary::Skinned> from = slot.skinned;
            release(i);
            m_slots[i].skinned = from; // (not baked again until its file changes)
            pose.state = State::failed;
            pose.problem = "the renderer refused its mesh (out of GPU memory?)";
            ++m_baked;
            continue;
        }
        if (slot.handed < bake.parts.size()) continue; // the rest next frame
        pose.baked_triangles = bake.baked_triangles;
        pose.error = bake.figure->error; // (made: its bake is done)
        pose.seams_kept = bake.figure->seams_kept;
        if (!bake.figure->counted) {
            bake.figure->counted = true;
            ++m_cuts;
        }
        pose.center = bake.center;
        pose.radius = bake.radius;
        pose.seconds = bake.seconds;
        pose.state = State::ready;
        slot.bake.reset();
        ++m_baked;
    }
}

void PoseBakes::finish() {
    const u32 budget = uploads_a_frame, triangles = triangles_a_frame;
    uploads_a_frame = 64;
    triangles_a_frame = ~0u;
    // (A file imported again begins bakes in update: those are waited for too.)
    // Every pass hands over up to 64 meshes: the bound is never met.
    for (usize pass = 0; pass < k_max_poses * 4 && (pass == 0 || baking() > 0); ++pass) {
        m_jobs.wait(m_group);
        update();
    }
    uploads_a_frame = budget;
    triangles_a_frame = triangles;
}

} // namespace giant::editor
