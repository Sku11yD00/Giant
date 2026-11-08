#include "animation_state.h"

#include "giant/core/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace giant::editor {

namespace {

// Working out a pose is a few microseconds a character: two workers are plenty.
constexpr u32 k_workers = 2;
// A saved animation changing fades into the new one (no pop), once posed.
constexpr float k_saved_fade_seconds = 0.2f;

} // namespace

AnimationState::AnimationState(render::Renderer* renderer) : m_renderer(renderer), m_jobs(k_workers) {}

AnimationState::~AnimationState() {
    if (m_renderer != nullptr) {
        for (const auto& [id, a] : m_animated) m_renderer->remove_mesh(a->copy);
    }
}

const MeshLibrary::Mesh* AnimationState::skinned_mesh(const world::Entity& e, const MeshLibrary& meshes) {
    if (e.shape != world::Shape::mesh || !e.visible) return nullptr;
    const MeshLibrary::Mesh* mesh = meshes.find_normalized(e.mesh);
    return mesh != nullptr && mesh->skinned != nullptr ? mesh : nullptr;
}

bool AnimationState::wants(const world::Entity& e, const MeshLibrary& meshes) const {
    const MeshLibrary::Mesh* mesh = skinned_mesh(e, meshes);
    if (mesh == nullptr) return false;
    if (m_commanded.contains(e.id)) return true;
    return !e.animation.clip.empty() && mesh->skinned->find_clip(e.animation.clip) != anim::k_no_clip;
}

void AnimationState::follow(const world::Scene& scene, const MeshLibrary& meshes, world::EntityId id,
                            bool changed) {
    const world::Entity* e = scene.find(id);
    // Every entity drawing a skinned file, animated or not (its hit shapes).
    if (e != nullptr && skinned_mesh(*e, meshes) != nullptr) {
        m_skinned[id] = ++m_serial;
    } else if (m_skinned.erase(id) != 0) {
        ++m_serial;
    }
    if (const auto r = m_refused.find(id); r != m_refused.end()) {
        const MeshLibrary::Mesh* mesh = e != nullptr ? skinned_mesh(*e, meshes) : nullptr;
        if (changed || mesh == nullptr || mesh->skinned != r->second.skinned) m_refused.erase(r);
    }
    const auto it = m_animated.find(id);
    if (e != nullptr && it != m_animated.end()) {
        Animated& a = *it->second;
        const MeshLibrary::Mesh* mesh = skinned_mesh(*e, meshes);
        if (mesh != a.mesh || (mesh != nullptr && mesh->skinned != a.skinned)) {
            // Another file, or the same one imported again: copied again (if still wanted).
            m_commanded.erase(id);
            stop(id);
        } else if (!(e->animation == a.saved)) {
            // Its own animation changed: that plays now, whatever a command played.
            a.saved = e->animation;
            a.by_command = false;
            m_commanded.erase(id);
            play_saved(a, a.posed ? k_saved_fade_seconds : 0.0f);
        }
    }
    if (e != nullptr && wants(*e, meshes) && !m_refused.contains(id)) {
        m_wanted.insert(id);
        return;
    }
    m_wanted.erase(id);
    m_commanded.erase(id);
    stop(id);
}

void AnimationState::follow_all(const world::Scene& scene, const MeshLibrary& meshes) {
    std::vector<world::EntityId> ids(m_wanted.begin(), m_wanted.end());
    for (const auto& [id, a] : m_animated) ids.push_back(id);
    for (const auto& [id, serial] : m_skinned) ids.push_back(id);
    for (const world::Entity& e : scene.entities()) {
        if (e.shape == world::Shape::mesh) ids.push_back(e.id);
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    for (const world::EntityId id : ids) follow(scene, meshes, id, false);
}

void AnimationState::follow_now(const world::Scene& scene, const MeshLibrary& meshes,
                                const world::SceneChanges& changed) {
    update(scene, meshes, changed, 0.0);
}

void AnimationState::restart_saved(const world::Scene& scene, world::EntityId id) {
    m_commanded.erase(id);
    const world::Entity* e = scene.find(id);
    const auto it = m_animated.find(id);
    if (e == nullptr || it == m_animated.end()) return;
    Animated& a = *it->second;
    a.saved = e->animation;
    a.by_command = false;
    play_saved(a, a.posed ? k_saved_fade_seconds : 0.0f);
    a.fresh = false;
}

AnimationState::Animated* AnimationState::start(const world::Scene& scene, const MeshLibrary& meshes,
                                                world::EntityId id, std::string& error) {
    const world::Entity* e = scene.find(id);
    const MeshLibrary::Mesh* mesh = e != nullptr ? skinned_mesh(*e, meshes) : nullptr;
    if (mesh == nullptr) {
        error = "entity " + std::to_string(id) + " does not draw a skinned mesh";
        return nullptr;
    }
    auto a = std::make_unique<Animated>();
    a->id = id;
    a->mesh = mesh;
    a->skinned = mesh->skinned;
    if (m_renderer != nullptr) {
        a->copy = m_renderer->add_skinned_mesh(a->skinned->clusters, static_cast<u32>(a->skinned->skeleton.size()));
        if (a->copy == render::k_no_mesh) {
            error = mesh->path + ": the renderer refused its skinned mesh (see the log)";
            m_refused[id] = {mesh->skinned, error};
            return nullptr;
        }
    }
    const usize joints = a->skinned->skeleton.size();
    a->local.resize(joints);
    a->model.resize(joints);
    a->skin.resize(joints);
    a->saved = e->animation;
    a->sent.assign(joints, Mat4{});
    play_saved(*a, 0.0f);
    Animated* out = a.get();
    m_animated[id] = std::move(a);
    return out;
}

void AnimationState::stop(world::EntityId id) {
    const auto it = m_animated.find(id);
    if (it == m_animated.end()) return;
    if (it->second->drawn) {
        m_copies.erase(id);
        m_redraw.push_back(id);
    }
    if (m_renderer != nullptr) m_renderer->remove_mesh(it->second->copy);
    m_animated.erase(it);
}

float AnimationState::start_of(const anim::Clip& clip, bool loop, float speed) {
    return !loop && speed < 0.0f ? clip.duration : 0.0f;
}

void AnimationState::play_saved(Animated& a, float fade) {
    const MeshLibrary::Skinned& s = *a.skinned;
    const u32 clip = a.saved.clip.empty() ? anim::k_no_clip : s.find_clip(a.saved.clip);
    if (clip == anim::k_no_clip) {
        a.player.stop();
    } else {
        a.player.play(s.skeleton, s.clips, clip, fade, a.saved.loop, start_of(s.clips[clip], a.saved.loop, a.saved.speed));
    }
    a.player.set_speed(a.saved.speed);
    a.dirty = true;
    a.fresh = true;
}

AnimationState::Animated* AnimationState::animate_now(const world::Scene& scene, const MeshLibrary& meshes,
                                                      world::EntityId id, std::string& error) {
    if (scene.identity() != m_scene || meshes.version() != m_meshes_version) {
        update(scene, meshes, {}, 0.0); // a new scene or meshes first
    }
    const auto it = m_animated.find(id);
    if (it != m_animated.end()) return it->second.get();
    if (m_animated.size() >= k_max_animated) {
        error = "at most " + std::to_string(k_max_animated) + " entities animate at once";
        return nullptr;
    }
    return start(scene, meshes, id, error);
}

std::string AnimationState::play(const world::Scene& scene, const MeshLibrary& meshes, world::EntityId id,
                                 const std::string& clip, float blend_seconds, bool loop, float speed, float start) {
    const world::Entity* e = scene.find(id);
    if (e == nullptr) return "no entity " + std::to_string(id);
    const MeshLibrary::Mesh* mesh = skinned_mesh(*e, meshes);
    if (mesh == nullptr) return "entity " + std::to_string(id) + " does not draw a skinned mesh (or is hidden)";
    const u32 index = mesh->skinned->find_clip(clip);
    if (index == anim::k_no_clip) return mesh->path + " has no clip named \"" + clip + "\"";
    std::string error;
    Animated* a = animate_now(scene, meshes, id, error);
    if (a == nullptr) return error;
    m_commanded.insert(id);
    m_wanted.insert(id);
    a->by_command = true;
    const float from = start >= 0.0f ? start : start_of(a->skinned->clips[index], loop, speed);
    a->player.play(a->skinned->skeleton, a->skinned->clips, index, blend_seconds, loop, from);
    a->player.set_speed(speed);
    a->dirty = true;
    a->fresh = false; // between frames: the next frame's time counts
    pose();
    return {};
}

std::string AnimationState::blend(const world::Scene& scene, const MeshLibrary& meshes, world::EntityId id,
                                  const std::string& a_name, const std::string& b_name, float weight, bool sync) {
    const world::Entity* e = scene.find(id);
    if (e == nullptr) return "no entity " + std::to_string(id);
    const MeshLibrary::Mesh* mesh = skinned_mesh(*e, meshes);
    if (mesh == nullptr) return "entity " + std::to_string(id) + " does not draw a skinned mesh (or is hidden)";
    const u32 ia = mesh->skinned->find_clip(a_name);
    const u32 ib = mesh->skinned->find_clip(b_name);
    if (ia == anim::k_no_clip) return mesh->path + " has no clip named \"" + a_name + "\"";
    if (ib == anim::k_no_clip) return mesh->path + " has no clip named \"" + b_name + "\"";
    std::string error;
    Animated* a = animate_now(scene, meshes, id, error);
    if (a == nullptr) return error;
    m_commanded.insert(id);
    m_wanted.insert(id);
    a->by_command = true;
    a->player.set_blend(a->skinned->clips, ia, ib, weight, sync);
    a->dirty = true;
    a->fresh = false;
    pose();
    return {};
}

void AnimationState::fill(const world::Scene& scene, const MeshLibrary& meshes) {
    if (m_animated.size() >= k_max_animated || m_wanted.size() <= m_animated.size()) return;
    u32 made = 0;
    std::vector<world::EntityId> failed;
    for (const world::EntityId id : m_wanted) {
        if (m_animated.size() >= k_max_animated || made >= copies_per_frame) break;
        if (m_animated.contains(id) || m_refused.contains(id)) continue;
        std::string error;
        if (start(scene, meshes, id, error) == nullptr) {
            log_warning("editor", "animation: " + error);
            failed.push_back(id); // tried again when the entity changes
        }
        ++made;
    }
    for (const world::EntityId id : failed) {
        m_wanted.erase(id);
        m_commanded.erase(id);
    }
}

void AnimationState::advance(double seconds) {
    if (seconds <= 0.0) return;
    for (const auto& [id, a] : m_animated) {
        if (a->fresh) continue; // started this update: from its start
        const anim::ClipState main = a->player.main();
        const anim::ClipState other = a->player.other();
        const float weight = a->player.other_weight();
        a->player.advance(static_cast<float>(seconds), a->skinned->clips);
        const anim::ClipState& m = a->player.main();
        const anim::ClipState& o = a->player.other();
        if (m.clip != main.clip || m.time != main.time || o.clip != other.clip || o.time != other.time ||
            a->player.other_weight() != weight) {
            a->dirty = true;
        }
    }
}

void AnimationState::step(double seconds) {
    for (const auto& [id, a] : m_animated) a->fresh = false; // by hand: all of them
    advance(seconds);
    pose();
}

void AnimationState::pose() {
    m_work.clear();
    for (const auto& [id, a] : m_animated) {
        if (a->dirty) m_work.push_back(a.get());
    }
    if (!m_work.empty()) {
        m_jobs.parallel_for(m_work.size(), 4, [this](usize begin, usize end) {
            for (usize i = begin; i < end; ++i) {
                Animated& a = *m_work[i];
                const MeshLibrary::Skinned& s = *a.skinned;
                a.player.evaluate(s.skeleton, s.clips, a.scratch, a.local);
                anim::model_matrices(s.skeleton, a.local, a.model);
                anim::skin_matrices(s.skeleton, a.model, a.skin);
                const anim::Bounds b = anim::pose_bounds(a.model, a.skin, s.joint_radius);
                a.bounds = {b.center, b.radius};
            }
        });
        for (Animated* a : m_work) {
            a->dirty = false;
            const bool same = a->posed && a->sent.size() == a->skin.size() &&
                              std::memcmp(a->sent.data(), a->skin.data(), a->skin.size() * sizeof(Mat4)) == 0 &&
                              a->sent_bounds.center == a->bounds.center &&
                              a->sent_bounds.radius == a->bounds.radius;
            a->posed = true;
            if (same) continue; // a clip that moves nothing (or time that moved within one key)
            if (m_renderer != nullptr && !m_renderer->set_pose(a->copy, a->skin, a->bounds)) {
                continue; // a pose the renderer refuses (not finite): the last one stays
            }
            a->sent = a->skin;
            a->sent_bounds = a->bounds;
            ++a->poses;
            ++m_poses;
            a->pose_serial = ++m_serial;
        }
    }
    // Copies loaded and posed are drawn instead of the file's mesh.
    if (m_renderer != nullptr) {
        for (const auto& [id, a] : m_animated) {
            if (!a->drawn && a->posed && m_renderer->mesh_ready(a->copy)) {
                a->drawn = true;
                a->pose_serial = ++m_serial; // drawn posed from now: what is hit
                m_copies[id] = a->copy;
                m_redraw.push_back(id);
            }
        }
    }
}

void AnimationState::update(const world::Scene& scene, const MeshLibrary& meshes,
                            const world::SceneChanges& changed, double seconds) {
    if (scene.identity() != m_scene) {
        // A new scene: what commands played goes with the old one.
        m_scene = scene.identity();
        m_meshes_version = meshes.version();
        m_commanded.clear();
        std::vector<world::EntityId> ids;
        for (const auto& [id, a] : m_animated) ids.push_back(id);
        for (const world::EntityId id : ids) stop(id);
        m_wanted.clear();
        m_refused.clear();
        m_skinned.clear();
        follow_all(scene, meshes);
    } else if (changed.all || meshes.version() != m_meshes_version) {
        m_meshes_version = meshes.version();
        follow_all(scene, meshes);
    } else {
        for (const world::EntityId id : changed.ids) follow(scene, meshes, id, true);
    }
    fill(scene, meshes);
    if (!m_paused) advance(seconds);
    pose();
    for (const auto& [id, a] : m_animated) a->fresh = false;
}

std::string AnimationState::refused(world::EntityId id) const {
    const auto it = m_refused.find(id);
    return it == m_refused.end() ? std::string() : it->second.error;
}

const AnimationState::Animated* AnimationState::find(world::EntityId id) const {
    const auto it = m_animated.find(id);
    return it == m_animated.end() ? nullptr : it->second.get();
}

std::vector<world::EntityId> AnimationState::take_redraw() {
    std::vector<world::EntityId> out = std::move(m_redraw);
    m_redraw.clear();
    return out;
}

usize AnimationState::frozen() const { return m_wanted.size() - m_animated.size() - waiting(); }

usize AnimationState::waiting() const {
    const usize rest = m_wanted.size() - m_animated.size();
    return std::min(rest, k_max_animated - m_animated.size());
}

} // namespace giant::editor
