#include "people_figures.h"

#include "mesh_library.h"
#include "people_state.h"
#include "pose_bakes.h"

#include "giant/anim/player.h"
#include "giant/world/people.h"

#include <algorithm>
#include <functional>
#include <optional>

namespace giant::editor {

namespace {

double square_distance(const DVec3& a, const DVec3& b) {
    const DVec3 d = a - b;
    return d.x * d.x + d.y * d.y + d.z * d.z;
}

} // namespace

void PeopleFigures::touch(u32 slot) { m_versions[slot / k_figure_chunk] = ++m_version; }

void PeopleFigures::touch_look(u32 look) {
    for (u32 s = 0; s < m_info.size(); ++s) {
        if (m_info[s].person != 0 && m_info[s].look == look) touch(s);
    }
}

void PeopleFigures::clear() {
    m_stats.removed += m_of.size();
    m_slots.clear();
    m_info.clear();
    m_free.clear();
    m_of.clear();
    m_versions.clear();
    m_cursor = 0;
    m_gone_cursor = 0;
    m_waiting_now = 0;
    m_stats.figures = 0;
    m_stats.slots = 0;
    m_stats.waiting = 0;
    m_draws = {};
}

const world::Transform* PeopleFigures::figure_of(world::PersonId id) const {
    const auto it = m_of.find(id);
    return it != m_of.end() ? &m_slots[it->second].at : nullptr;
}

void PeopleFigures::put(const world::Scene& scene, u32 slot, const world::Person& person, u32 look,
                        const PeopleTravel::Where& at) {
    FigureDraw& d = m_slots[slot];
    const std::optional<double> ground = scene.terrain.height_at(at.position.x, at.position.z);
    const float height = m_looks[look].height;
    d.at.position = {at.position.x, ground ? *ground : at.position.y, at.position.z};
    d.at.rotation = quat_from_axis_angle({0.0f, 1.0f, 0.0f}, degrees_to_radians(at.yaw));
    d.at.scale = {height, height, height};
    d.look = &m_looks[look];
    d.meshes = &m_poses[look].meshes;
    m_info[slot] = {person.id, look, at};
    touch(slot);
}

void PeopleFigures::remove(u32 slot) {
    m_of.erase(m_info[slot].person);
    m_slots[slot] = {};
    m_info[slot] = {};
    m_free.push_back(slot);
    std::push_heap(m_free.begin(), m_free.end(), std::greater<>());
    touch(slot);
    ++m_stats.removed;
}

bool PeopleFigures::grow() {
    const u32 limit = std::min(settings.most, k_max_figures);
    const u32 now = static_cast<u32>(m_slots.size());
    if (now >= limit) return false;
    const u32 next = std::min(limit, std::max(k_first_slots, now * 2));
    m_slots.resize(next);
    m_info.resize(next);
    // (A chunk the old slots ended in has more slots now: its version stays, nothing is drawn in the new ones yet.)
    m_versions.resize((next + k_figure_chunk - 1) / k_figure_chunk, 0);
    for (u64& v : m_versions) {
        if (v == 0) v = ++m_version;
    }
    for (u32 s = now; s < next; ++s) {
        m_free.push_back(s);
        std::push_heap(m_free.begin(), m_free.end(), std::greater<>());
    }
    return true;
}

void PeopleFigures::follow_looks(const world::Scene& scene) {
    const std::vector<world::PersonLook>& looks = scene.person_looks;
    if (looks.size() != m_looks.size()) {
        // A look made or removed (rare): everyone is made again over the next frames.
        clear();
        m_looks = looks;
        m_poses.assign(looks.size(), {});
        m_look_of.clear();
        for (u32 i = 0; i < looks.size(); ++i) m_look_of.emplace(looks[i].name, i);
        m_look_cursor = 0;
        m_pose_cursor = 0;
        return;
    }
    if (looks.empty()) return;
    // One a frame is compared: its figures are put again as the new one has them.
    const u32 i = static_cast<u32>(m_look_cursor++ % looks.size());
    const world::PersonLook& is = looks[i];
    world::PersonLook& was = m_looks[i];
    if (is.name == was.name && is.mesh == was.mesh && is.height == was.height && is.material == was.material &&
        is.parts == was.parts) {
        return; // (what else a look says shows on entities alone)
    }
    if (is.mesh != was.mesh) m_poses[i] = fresh(m_poses[i].wanted); // (its figures draw nothing until the new file's pose is there)
    if (is.name != was.name) {
        // Every look's name is taken as it is now (names may have changed places: nobody is left
        // with a name that finds no look, or another's); what else changed of the others waits its turn.
        m_look_of.clear();
        for (u32 k = 0; k < looks.size(); ++k) {
            m_looks[k].name = looks[k].name;
            m_look_of.emplace(looks[k].name, k);
        }
    }
    was = is; // (where it is: the slots' pointers to it hold)
    for (u32 s = 0; s < m_info.size(); ++s) {
        if (m_info[s].person == 0 || m_info[s].look != i) continue;
        m_slots[s].at.scale = {is.height, is.height, is.height};
        touch(s);
    }
}

// The clip a look's people stand in: the one set, where the look's file has it; else its rest pose.
std::string PeopleFigures::clip_of(const world::PersonLook& look) const {
    const MeshLibrary::Mesh* file = meshes->find(look.mesh);
    if (file != nullptr && file->skinned != nullptr && !settings.clip.empty() &&
        file->skinned->find_clip(settings.clip) != anim::k_no_clip) {
        return settings.clip;
    }
    return {};
}

void PeopleFigures::follow_poses() {
    const std::vector<PoseBakes::Pose>& baked = poses->poses();
    if (poses->clears() != m_clears_seen) {
        // Every pose was forgotten: the indices kept are other poses' now.
        m_clears_seen = poses->clears();
        for (u32 i = 0; i < m_poses.size(); ++i) {
            if (!m_poses[i].meshes.empty()) touch_look(i);
            m_poses[i] = fresh(m_poses[i].wanted);
        }
    }
    if (meshes->version() != m_meshes_seen) {
        // A file imported: a look whose file has the clip now (or no longer) stands in another pose.
        m_meshes_seen = meshes->version();
        for (u32 i = 0; i < m_poses.size(); ++i) {
            LookPose& p = m_poses[i];
            if (p.pose >= baked.size() || p.clip == clip_of(m_looks[i])) continue;
            if (!p.meshes.empty()) touch_look(i);
            p = fresh(p.wanted);
        }
    }
    // One look's pose a frame is asked for: one someone was near enough for, that has none or
    // whose bake failed, and not again until a file is imported if it could not be had.
    if (!m_poses.empty()) {
        const u32 i = static_cast<u32>(m_pose_cursor++ % m_poses.size());
        LookPose& p = m_poses[i];
        const bool failed = p.pose < baked.size() && baked[p.pose].state == PoseBakes::State::failed;
        if (p.wanted && (p.pose >= baked.size() || failed) && p.refused_at != meshes->version() + 1) {
            const std::string clip = clip_of(m_looks[i]);
            std::string error;
            const u32 pose = poses->want(
                m_looks[i].mesh, clip, 0.0f,
                std::clamp(settings.triangles, PoseBakes::k_min_triangles, PoseBakes::k_max_triangles), error);
            // (Refused: its file not loaded or not skinned, or no room for more poses. One that failed
            // is begun again by the asking; should it fail again it waits for a file too.)
            if (pose == PoseBakes::k_no_pose || failed) p.refused_at = meshes->version() + 1;
            if (pose != PoseBakes::k_no_pose) {
                p.pose = pose;
                p.clip = clip;
            }
        }
    }
    // Each look's meshes as its pose has them now: a mesh that left the renderer is not drawn again.
    m_stats.looks_ready = 0;
    m_stats.looks_baking = 0;
    m_stats.looks_without = 0;
    for (u32 i = 0; i < m_poses.size(); ++i) {
        LookPose& p = m_poses[i];
        const PoseBakes::Pose* pose = p.pose < baked.size() ? &baked[p.pose] : nullptr;
        if (pose != nullptr && pose->state == PoseBakes::State::ready) {
            if (p.meshes != pose->meshes) {
                p.meshes = pose->meshes; // (in place: the slots' pointers to it hold)
                touch_look(i);
            }
            ++m_stats.looks_ready;
        } else {
            if (!p.meshes.empty()) {
                p.meshes.clear();
                touch_look(i);
            }
            ++(pose != nullptr && pose->state == PoseBakes::State::baking ? m_stats.looks_baking : m_stats.looks_without);
        }
    }
}

void PeopleFigures::update(const world::Scene& scene, const DVec3& camera) {
    if (scene.identity() != m_scene) {
        m_scene = scene.identity();
        clear();
        m_looks.clear();
        m_poses.clear();
        m_look_of.clear();
        m_stats = {};
    }
    follow_looks(scene);
    if (settings.triangles != m_settings_seen.triangles || settings.clip != m_settings_seen.clip) {
        // Another pose for every look: asked for afresh (the old ones stay baked until clear_baked_poses).
        if (!m_slots.empty()) clear();
        for (LookPose& p : m_poses) p = fresh(p.wanted);
    }
    m_settings_seen = settings;
    const bool can = settings.on && !hold && poses != nullptr && meshes != nullptr;
    if (!can) {
        if (!m_slots.empty()) clear();
        m_stats.looks_ready = m_stats.looks_baking = m_stats.looks_without = 0;
        return;
    }
    follow_poses();

    // Those given an entity are figures no longer.
    if (bodies != nullptr && !m_of.empty()) {
        m_scratch.clear();
        bodies->held(m_scratch);
        for (const world::PersonId id : m_scratch) {
            const auto it = m_of.find(id);
            if (it != m_of.end()) remove(it->second);
        }
    }
    // Those whose person is no longer in the scene (a stretch of the slots a frame).
    for (u32 k = 0; k < k_gone_looked_a_frame && k < m_info.size(); ++k) {
        const u32 s = static_cast<u32>(m_gone_cursor++ % m_info.size());
        if (m_info[s].person != 0 && world::find_person(scene.people, m_info[s].person) == nullptr) remove(s);
    }
    // More than may be (`most` lowered): any go.
    const u32 most = std::min(settings.most, k_max_figures);
    while (m_of.size() > most) remove(m_of.begin()->second);

    // The next stretch of the list: those within `distance` are made figures, those gone off are figures no longer.
    const std::vector<world::Person>& people = scene.people;
    if (m_cursor >= people.size()) {
        m_cursor = 0;
        m_stats.waiting = m_waiting_now;
        m_waiting_now = 0;
    }
    const double made_within = std::clamp(static_cast<double>(settings.distance), 1.0, 4000.0);
    const double kept_within = made_within + std::clamp(static_cast<double>(settings.keep), 0.0, 1000.0);
    const usize end = std::min(people.size(), m_cursor + std::clamp<usize>(settings.scanned_a_frame, 1, 65536));
    const u32 a_frame = std::clamp<u32>(settings.made_a_frame, 1, 1024);
    u32 made = 0;
    for (; m_cursor < end; ++m_cursor) {
        const world::Person& person = people[m_cursor];
        const PeopleTravel::Where at = where(person);
        const double off = square_distance(at.position, camera);
        const auto it = m_of.find(person.id);
        const bool has = it != m_of.end();
        const double within = has ? kept_within : made_within;
        if (off > within * within || (bodies != nullptr && bodies->holds(person.id))) {
            if (has) remove(it->second);
            continue;
        }
        if (has) {
            const u32 slot = it->second;
            u32 look = m_info[slot].look;
            if (m_looks[look].name != person.look) { // (given another look)
                const auto named = m_look_of.find(person.look);
                if (named == m_look_of.end()) {
                    remove(slot);
                    continue;
                }
                look = named->second;
            }
            m_poses[look].wanted = true;
            // Put again where it is now (it travelled, was put elsewhere, or the ground under it changed).
            const std::optional<double> ground = scene.terrain.height_at(at.position.x, at.position.z);
            if (look != m_info[slot].look || !(at == m_info[slot].at) ||
                (ground ? *ground : at.position.y) != m_slots[slot].at.position.y) {
                put(scene, slot, person, look, at);
            }
            continue;
        }
        const auto named = m_look_of.find(person.look);
        if (named == m_look_of.end()) continue; // (no such look)
        m_poses[named->second].wanted = true;
        if (m_poses[named->second].meshes.empty()) continue; // (its pose not there yet)
        if (m_of.size() >= most) {
            ++m_waiting_now;
            continue;
        }
        if (made >= a_frame) break; // (this one first next frame)
        if (m_free.empty() && !grow()) {
            ++m_waiting_now;
            continue;
        }
        std::pop_heap(m_free.begin(), m_free.end(), std::greater<>());
        const u32 slot = m_free.back();
        m_free.pop_back();
        put(scene, slot, person, named->second, at);
        m_of.emplace(person.id, slot);
        ++made;
        ++m_stats.made;
    }

    m_draws.slots = m_slots;
    m_draws.versions = m_versions;
    m_stats.figures = static_cast<u32>(m_of.size());
    m_stats.slots = static_cast<u32>(m_slots.size());
}

} // namespace giant::editor
