#include "people_figures.h"

#include "mesh_library.h"
#include "people_routines.h"
#include "people_state.h"
#include "pose_bakes.h"

#include "giant/anim/motion.h"
#include "giant/anim/player.h"
#include "giant/anim/skeleton.h"
#include "giant/world/people.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>

namespace giant::editor {

namespace {

double square_distance(const DVec3& a, const DVec3& b) {
    const DVec3 d = a - b;
    return d.x * d.x + d.y * d.y + d.z * d.z;
}

// Past a place's edge one standing there still counts as at it: as far as an entity does (one
// stopped short by those standing there), so a person shows the same as a figure and as an entity.
constexpr double k_there_slack = 2.0 * PeopleState::k_walk_end;

// What of a look's activities its figures show: the rest (a clip's speed) shows on entities alone.
bool same_poses(const std::vector<world::LookActivity>& a, const std::vector<world::LookActivity>& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const world::LookActivity& x, const world::LookActivity& y) {
        return x.activity == y.activity && x.clip == y.clip && x.loop == y.loop;
    });
}
// A walk that carries the hips less than this over the ground in a round goes nowhere (metres, in the file).
constexpr float k_least_round = 0.01f;

} // namespace

void PeopleFigures::touch(u32 slot) { m_versions[slot / k_figure_chunk] = ++m_version; }

// A pose's meshes are others than they were (or none): the figures showing it are pointed again.
void PeopleFigures::changed(u32 index) {
    if (m_poses[index].changed) return;
    m_poses[index].changed = true;
    m_changed.push_back(index);
}

void PeopleFigures::clear() {
    m_stats.removed += m_of.size();
    m_slots.clear();
    m_info.clear();
    m_free.clear();
    m_moving.clear();
    m_of.clear();
    m_versions.clear();
    m_cursor = 0;
    m_gone_cursor = 0;
    m_move_cursor = 0;
    m_waiting_now = 0;
    m_hidden_now = 0;
    m_acting = 0;
    m_stats.figures = 0;
    m_stats.slots = 0;
    m_stats.waiting = 0;
    m_stats.hidden = 0;
    m_stats.walking = 0;
    m_stats.moved = 0;
    m_stats.acting = 0;
    m_draws = {};
}

const world::Transform* PeopleFigures::figure_of(world::PersonId id) const {
    const auto it = m_of.find(id);
    return it != m_of.end() ? &m_slots[it->second].at : nullptr;
}

u32 PeopleFigures::kind_of(world::PersonId id) const {
    const auto it = m_of.find(id);
    return it != m_of.end() ? m_info[it->second].kind : k_no_kind;
}

std::string_view PeopleFigures::activity_of(world::PersonId id) const {
    const auto it = m_of.find(id);
    if (it == m_of.end() || m_info[it->second].kind < k_first_activity) return {};
    const std::vector<world::LookActivity>& activities = m_looks[m_info[it->second].look].activities;
    const usize index = m_info[it->second].kind - k_first_activity;
    return index < activities.size() ? std::string_view(activities[index].activity) : std::string_view();
}

float PeopleFigures::yaw_of(world::PersonId id) const {
    const auto it = m_of.find(id);
    return it != m_of.end() ? m_info[it->second].at.yaw : 0.0f;
}

// What a figure of the look draws for `kind`: that pose, or the standing one while it is not there
// (a walk's while any pose of its round is not: one stepping through them would stand every other step).
const std::vector<render::MeshId>* PeopleFigures::shown(u32 look, u32 kind) const {
    const LookPose* poses_of = &m_poses[usize{look} * k_kinds];
    bool there = !poses_of[kind].meshes.empty();
    for (u32 k = k_first_walk; there && is_walk(kind) && k < k_first_walk + walk_poses(); ++k) there = !poses_of[k].meshes.empty();
    return there ? &poses_of[kind].meshes : &poses_of[k_stand].meshes;
}

// The pose is asked for (in its turn) unless it is there, waiting, or could not be had.
// (A standing pose before any other: without it a look's people are not drawn at all. A walk's all
// together: one stepping through them meets a new one every eighth of a second.)
void PeopleFigures::need(u32 look, u32 kind) {
    const LookPose& wanted = m_poses[look * k_kinds + kind];
    const u64 refused = meshes->version() + poses->clears() + 1;
    if (wanted.pose != PoseBakes::k_no_pose || wanted.queued || wanted.refused_at == refused) return;
    const bool walk = is_walk(kind);
    const u32 first = walk ? k_first_walk : kind, count = walk ? walk_poses() : 1;
    for (u32 k = first; k < first + count; ++k) {
        LookPose& pose = m_poses[look * k_kinds + k];
        if (pose.pose != PoseBakes::k_no_pose || pose.queued || pose.refused_at == refused) continue;
        pose.queued = true;
        (k == k_stand ? m_to_ask_first : m_to_ask).push_back(look * k_kinds + k);
    }
}

// A pose is no longer the look's: its figures stand until it is asked for again and there.
void PeopleFigures::forget(u32 index) {
    LookPose& pose = m_poses[index];
    if (pose.pose != PoseBakes::k_no_pose) {
        const u32 last = m_asked.back();
        m_asked[pose.asked_at] = last;
        m_poses[last].asked_at = pose.asked_at;
        m_asked.pop_back();
        if (index % k_kinds == k_stand) --m_stands;
    }
    if (!pose.meshes.empty()) changed(index);
    const bool queued = pose.queued, was_changed = pose.changed;
    pose = {};
    pose.queued = queued;
    pose.changed = was_changed;
}

void PeopleFigures::forget_all() {
    for (LookPose& pose : m_poses) pose = {}; // (in place: nothing points at their meshes, the figures are gone)
    std::fill(m_round.begin(), m_round.end(), 0.0f);
    std::fill(m_walked.begin(), m_walked.end(), u8{0});
    forget_asking();
}

void PeopleFigures::forget_asking() {
    m_asked.clear();
    m_stands = 0;
    m_to_ask.clear();
    m_to_ask_first.clear();
    m_to_ask_head = 0;
    m_to_ask_first_head = 0;
    m_changed.clear();
}

// What a pose of a look is asked of PoseBakes as. False: there is none (the look's file has no such
// clip, the walk is shown in fewer poses, the look names no such activity).
bool PeopleFigures::asked_as(u32 look, u32 kind, Asked& out) const {
    const world::PersonLook& l = m_looks[look];
    const MeshLibrary::Mesh* file = meshes->find(l.mesh);
    const MeshLibrary::Skinned* skinned = file != nullptr ? file->skinned.get() : nullptr;
    const auto clip_of = [skinned](const std::string& name) {
        return skinned != nullptr && !name.empty() ? skinned->find_clip(name) : anim::k_no_clip;
    };
    out = {};
    if (kind == k_stand) {
        // The clip set, where the look's file has it; else its rest pose.
        if (clip_of(settings.clip) != anim::k_no_clip) out.clip = settings.clip;
        return true;
    }
    if (kind < k_first_activity) {
        const u32 count = walk_poses();
        const u32 clip = clip_of(settings.walk_clip);
        if (kind - k_first_walk >= count || clip == anim::k_no_clip) return false;
        // (A rig whose hips are not found by name cannot be held in place: a clip that carries it
        // along would carry each pose along too. Its people stand as they go.)
        if (anim::find_motion_joints(skinned->skeleton).hips == anim::k_no_joint) return false;
        out.clip = settings.walk_clip;
        out.time = skinned->clips[clip].duration * static_cast<float>(kind - k_first_walk) / static_cast<float>(count);
        out.in_place = true;
        return true;
    }
    if (kind - k_first_activity >= l.activities.size()) return false;
    const world::LookActivity& activity = l.activities[kind - k_first_activity];
    const u32 clip = clip_of(activity.clip);
    if (clip == anim::k_no_clip) return false;
    out.clip = activity.clip;
    out.time = activity.loop ? 0.0f : skinned->clips[clip].duration; // (one played once is held at its end)
    return true;
}

// The ground a round of the look's walk covers, as its clip carries the hips (0: it goes nowhere, or there is none).
void PeopleFigures::measure(u32 look) {
    m_round[look] = 0.0f;
    const MeshLibrary::Mesh* file = meshes->find(m_looks[look].mesh);
    if (file == nullptr || file->skinned == nullptr || settings.walk_clip.empty()) return;
    const u32 clip = file->skinned->find_clip(settings.walk_clip);
    if (clip == anim::k_no_clip) return;
    const float round = length(PoseBakes::travel(*file->skinned, clip));
    if (round >= k_least_round) m_round[look] = round;
}

// Where a person is now and what its figure shows: walking along its way, at what its routine has it
// do (where that has it be), or standing.
PeopleFigures::Pick PeopleFigures::pick(const world::Scene& scene, const world::Person& person, u32 look) const {
    Pick out;
    const PeopleTravel::Trip* trip = travel != nullptr ? travel->trip(person.id) : nullptr;
    if (trip != nullptr && trip->phase == PeopleTravel::Phase::travelling) {
        const double gone = travel->gone(*trip);
        out.at = PeopleTravel::at(*trip, gone);
        out.moving = true;
        if (!settings.walk_clip.empty()) {
            // The pose of the round it is at: a round to the ground the walk covers in one, at its height.
            const u32 count = walk_poses();
            const double round = static_cast<double>(m_round[look] > 0.0f ? m_round[look] : std::clamp(settings.stride, 0.1f, 10.0f)) *
                                 std::max(static_cast<double>(m_looks[look].height), 0.01);
            const double rounds = gone / round;
            out.kind = k_first_walk + std::min(static_cast<u32>((rounds - std::floor(rounds)) * count), count - 1);
        }
        return out;
    }
    out.at = where(person);
    PeopleRoutines::Activity doing;
    if (routines == nullptr || !routines->activity(scene, person.id, doing) ||
        !PeopleRoutines::there(doing, out.at.position, k_there_slack)) {
        return out;
    }
    if (!settings.hidden.empty() && doing.activity == settings.hidden) {
        out.hidden = true;
        return out;
    }
    const std::vector<world::LookActivity>& activities = m_looks[look].activities;
    for (u32 i = 0; i < activities.size() && i < world::k_max_look_activities; ++i) {
        if (activities[i].activity == doing.activity) {
            out.kind = k_first_activity + i;
            break;
        }
    }
    return out;
}

// A look's material in each of its tints, kept where figures' draws point (those past its tints: plain).
void PeopleFigures::tint(u32 look) {
    const world::PersonLook& l = m_looks[look];
    for (u32 t = 0; t < world::k_max_look_tints; ++t) {
        world::Material& m = m_tinted[usize{look} * world::k_max_look_tints + t];
        if (t < l.tints.size()) {
            m = world::person_material(l, t);
        } else {
            m = {};
        }
    }
}

// What a figure of that look and seed draws its first part with, when not the look's own (null).
const world::Material* PeopleFigures::material(u32 look, u32 seed) const {
    const world::PersonLook& l = m_looks[look];
    if (l.tints.empty()) return nullptr;
    return &m_tinted[usize{look} * world::k_max_look_tints + world::person_tint(l, seed)];
}

void PeopleFigures::put(const world::Scene& scene, u32 slot, const world::Person& person, u32 person_at, u32 look,
                        const Pick& pick) {
    FigureDraw now;
    const std::optional<double> ground = scene.terrain.height_at(pick.at.position.x, pick.at.position.z);
    const float height = m_looks[look].height;
    now.at.position = {pick.at.position.x, ground ? *ground : pick.at.position.y, pick.at.position.z};
    now.at.rotation = quat_from_axis_angle({0.0f, 1.0f, 0.0f}, degrees_to_radians(pick.at.yaw));
    now.at.scale = {height, height, height};
    now.look = &m_looks[look];
    need(look, pick.kind);
    now.meshes = shown(look, pick.kind);
    now.material = material(look, person.seed);
    Slot& info = m_info[slot];
    if (info.person != 0 && info.kind >= k_first_activity) --m_acting;
    if (pick.kind >= k_first_activity) ++m_acting;
    info.person = person.id;
    info.look = look;
    info.at = pick.at;
    info.kind = pick.kind;
    info.person_at = person_at;
    info.seed = person.seed;
    FigureDraw& drawn = m_slots[slot];
    if (drawn == now) return; // (as it is drawn)
    drawn = now;
    touch(slot);
}

// One travelling is put on along its way every frame (in its turn); one that is not, when the list comes round.
void PeopleFigures::set_moving(u32 slot, bool moving) {
    Slot& info = m_info[slot];
    if (moving == (info.moving != ~0u)) return;
    if (moving) {
        info.moving = static_cast<u32>(m_moving.size());
        m_moving.push_back(slot);
        return;
    }
    const u32 last = m_moving.back();
    m_moving[info.moving] = last;
    m_info[last].moving = info.moving;
    m_moving.pop_back();
    info.moving = ~0u;
}

// A person is made a figure in the lowest free slot. False: no slot is to be had (`most` are kept).
bool PeopleFigures::make(const world::Scene& scene, const world::Person& person, u32 person_at, u32 look, const Pick& pick) {
    if (m_free.empty() && !grow()) return false;
    std::pop_heap(m_free.begin(), m_free.end(), std::greater<>());
    const u32 slot = m_free.back();
    m_free.pop_back();
    put(scene, slot, person, person_at, look, pick);
    m_of.emplace(person.id, slot);
    set_moving(slot, pick.moving);
    ++m_stats.made;
    return true;
}

void PeopleFigures::remove(u32 slot) {
    set_moving(slot, false);
    if (m_info[slot].kind >= k_first_activity) --m_acting;
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
        m_poses.assign(looks.size() * k_kinds, {});
        m_tinted.assign(looks.size() * world::k_max_look_tints, {});
        for (u32 i = 0; i < looks.size(); ++i) tint(i);
        m_round.assign(looks.size(), 0.0f);
        m_walked.assign(looks.size(), 0);
        forget_asking();
        m_look_of.clear();
        for (u32 i = 0; i < looks.size(); ++i) m_look_of.emplace(looks[i].name, i);
        m_look_cursor = 0;
        return;
    }
    if (looks.empty()) return;
    // One a frame is compared: its figures are put again as the new one has them.
    const u32 i = static_cast<u32>(m_look_cursor++ % looks.size());
    const world::PersonLook& is = looks[i];
    world::PersonLook& was = m_looks[i];
    const bool activities = !same_poses(is.activities, was.activities);
    if (is.name == was.name && is.mesh == was.mesh && is.height == was.height && is.material == was.material &&
        is.parts == was.parts && is.tints == was.tints && !activities) {
        was.activities = is.activities; // (what else a look says shows on entities alone)
        return;
    }
    // Another file: every pose of it is asked for afresh (its figures draw nothing until the standing
    // one is there). Other activities: their poses are (those at one stand until the list comes round).
    const bool file = is.mesh != was.mesh;
    for (u32 kind = file ? 0 : k_first_activity; kind < k_kinds && (file || activities); ++kind) forget(i * k_kinds + kind);
    if (file) {
        m_round[i] = 0.0f;
        m_walked[i] = 0;
    }
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
    tint(i);
    for (u32 s = 0; s < m_info.size(); ++s) {
        Slot& info = m_info[s];
        if (info.person == 0 || info.look != i) continue;
        if (activities && info.kind >= k_first_activity) {
            info.kind = k_stand;
            --m_acting;
        }
        m_slots[s].at.scale = {is.height, is.height, is.height};
        m_slots[s].meshes = shown(i, info.kind);
        m_slots[s].material = material(i, info.seed);
        touch(s);
    }
}

void PeopleFigures::follow_poses() {
    const std::vector<PoseBakes::Pose>& baked = poses->poses();
    if (poses->clears() != m_clears_seen) {
        // Every pose was forgotten: the indices kept are other poses' now.
        m_clears_seen = poses->clears();
        while (!m_asked.empty()) forget(m_asked.back());
    }
    if (meshes->version() != m_meshes_seen) {
        // A file imported: a pose its look would be asked for otherwise now (the file has the clip
        // now, or no longer, or it is another length) is asked for afresh; PoseBakes bakes the rest again.
        m_meshes_seen = meshes->version();
        for (usize k = m_asked.size(); k-- > 0;) {
            const u32 index = m_asked[k];
            const u32 look = index / k_kinds, kind = index % k_kinds;
            LookPose& pose = m_poses[index];
            Asked now;
            if (!asked_as(look, kind, now) || now.clip != pose.clip || now.time != pose.time || now.in_place != pose.in_place) {
                forget(index);
                continue;
            }
            pose.retried = false;
        }
        // The ground a round of the walk covers, of each look that has been seen walking, as its file has it now.
        for (u32 look = 0; look < m_looks.size(); ++look) {
            if (m_walked[look] != 0) measure(look);
        }
    }
    // A few a frame are asked for.
    const u32 triangles = std::clamp(settings.triangles, PoseBakes::k_min_triangles, PoseBakes::k_max_triangles);
    for (u32 n = 0; n < k_asked_a_frame; ++n) {
        // The standing ones first.
        const bool first = m_to_ask_first_head < m_to_ask_first.size();
        if (!first && m_to_ask_head >= m_to_ask.size()) break;
        const u32 index = first ? m_to_ask_first[m_to_ask_first_head++] : m_to_ask[m_to_ask_head++];
        LookPose& pose = m_poses[index];
        pose.queued = false;
        if (pose.pose != PoseBakes::k_no_pose) continue;
        const u32 look = index / k_kinds, kind = index % k_kinds;
        Asked as;
        std::string error;
        // Room is kept for a standing pose of every look that has none yet: another pose is not had at its cost.
        const bool room = kind == k_stand || baked.size() + (m_looks.size() - m_stands) < PoseBakes::k_max_poses;
        const u32 got = room && asked_as(look, kind, as)
                            ? poses->want(m_looks[look].mesh, as.clip, as.time, triangles, error, as.in_place)
                            : PoseBakes::k_no_pose;
        if (got == PoseBakes::k_no_pose) {
            // Refused: its file not loaded or not skinned, no such clip, or no room for more poses.
            // Not asked for again until a file is imported or the poses are cleared.
            pose.refused_at = meshes->version() + poses->clears() + 1;
            continue;
        }
        if (kind == k_stand) ++m_stands;
        pose.pose = got;
        pose.clip = as.clip;
        pose.time = as.time;
        pose.in_place = as.in_place;
        pose.retried = false;
        pose.asked_at = static_cast<u32>(m_asked.size());
        m_asked.push_back(index);
        if (is_walk(kind) && m_walked[look] == 0) {
            m_walked[look] = 1;
            measure(look);
        }
    }
    if (m_to_ask_head == m_to_ask.size()) {
        m_to_ask.clear();
        m_to_ask_head = 0;
    }
    if (m_to_ask_first_head == m_to_ask_first.size()) {
        m_to_ask_first.clear();
        m_to_ask_first_head = 0;
    }
    // Each pose's meshes as PoseBakes has them now: a mesh that left the renderer is not drawn again.
    // One whose bake failed is begun again, once (one a frame), then waits for its file.
    bool begun = false;
    for (const u32 index : m_asked) {
        LookPose& pose = m_poses[index];
        if (pose.pose >= baked.size()) continue;
        const PoseBakes::Pose& from = baked[pose.pose];
        if (from.state == PoseBakes::State::ready) {
            if (pose.meshes != from.meshes) {
                pose.meshes = from.meshes; // (in place: the slots' pointers to it hold)
                changed(index);
            }
            continue;
        }
        if (!pose.meshes.empty()) {
            pose.meshes.clear();
            changed(index);
        }
        if (from.state == PoseBakes::State::failed && !pose.retried && !begun) {
            pose.retried = true;
            begun = true;
            std::string error;
            poses->want(m_looks[index / k_kinds].mesh, pose.clip, pose.time, from.triangles, error, pose.in_place);
        }
    }
    m_stats.looks_ready = 0;
    m_stats.looks_baking = 0;
    m_stats.looks_without = 0;
    for (usize i = 0; i < m_looks.size(); ++i) {
        const LookPose& stand = m_poses[i * k_kinds];
        const PoseBakes::Pose* from = stand.pose < baked.size() ? &baked[stand.pose] : nullptr;
        if (from != nullptr && from->state == PoseBakes::State::ready) {
            ++m_stats.looks_ready;
        } else {
            ++(from != nullptr && from->state == PoseBakes::State::baking ? m_stats.looks_baking : m_stats.looks_without);
        }
    }
}

// Those travelling are put on along their way, each in the pose of the walk it has come to: so many a frame, in turn.
void PeopleFigures::move(const world::Scene& scene) {
    m_stats.moved = 0;
    const std::vector<world::Person>& people = scene.people;
    const usize turns = std::min<usize>(m_moving.size(), std::clamp(settings.moved_a_frame, 1u, k_max_figures));
    for (usize turn = 0; turn < turns && !m_moving.empty(); ++turn) {
        if (m_move_cursor >= m_moving.size()) m_move_cursor = 0;
        const u32 slot = m_moving[m_move_cursor];
        const Slot& info = m_info[slot];
        const world::Person* person = info.person_at < people.size() && people[info.person_at].id == info.person
                                          ? &people[info.person_at]
                                          : world::find_person(people, info.person);
        if (person == nullptr) { // (gone from the scene)
            remove(slot);
            continue;
        }
        const Pick now = pick(scene, *person, info.look);
        if (now.hidden) {
            remove(slot);
            continue;
        }
        put(scene, slot, *person, static_cast<u32>(person - people.data()), info.look, now);
        ++m_stats.moved;
        if (now.moving) {
            ++m_move_cursor;
        } else {
            set_moving(slot, false); // (arrived, or stopped: another takes its place in the turn)
        }
    }
}

void PeopleFigures::update(const world::Scene& scene, const DVec3& camera) {
    if (scene.identity() != m_scene) {
        m_scene = scene.identity();
        clear();
        m_looks.clear();
        m_poses.clear();
        m_round.clear();
        m_walked.clear();
        forget_asking();
        m_look_of.clear();
        m_stats = {};
    }
    follow_looks(scene);
    if (settings.triangles != m_settings_seen.triangles || settings.clip != m_settings_seen.clip ||
        settings.walk_clip != m_settings_seen.walk_clip || settings.walk_poses != m_settings_seen.walk_poses) {
        // Other poses for every look: asked for afresh (the old ones stay baked until clear_baked_poses).
        if (!m_slots.empty()) clear();
        forget_all();
    }
    m_settings_seen = settings;
    const bool can = settings.on && !hold && poses != nullptr && meshes != nullptr;
    if (!can) {
        if (!m_slots.empty()) clear();
        m_stats.looks_ready = m_stats.looks_baking = m_stats.looks_without = 0;
        return;
    }
    follow_poses();
    if (!m_changed.empty()) {
        // Poses came, went or were baked again: the figures that show one of them, or would now, are
        // pointed at what they show and sent again (one pass, however many poses).
        for (u32 s = 0; s < m_info.size(); ++s) {
            const Slot& info = m_info[s];
            if (info.person == 0) continue;
            const std::vector<render::MeshId>* now = shown(info.look, info.kind);
            const bool own = now == &m_poses[usize{info.look} * k_kinds + info.kind].meshes;
            if (now == m_slots[s].meshes && !m_poses[usize{info.look} * k_kinds + (own ? info.kind : k_stand)].changed) continue;
            m_slots[s].meshes = now;
            touch(s);
        }
        for (const u32 index : m_changed) m_poses[index].changed = false;
        m_changed.clear();
    }

    // Those given an entity are figures no longer.
    if (bodies != nullptr && !m_of.empty()) {
        m_scratch.clear();
        bodies->held(m_scratch);
        for (const world::PersonId id : m_scratch) {
            const auto it = m_of.find(id);
            if (it != m_of.end()) remove(it->second);
        }
    }
    // Those whose entity was taken away this frame are figures from this frame on, where they are and
    // as they are (walking, at what they do): nobody is missing between the two. (As many as were
    // let go: at most those held.)
    const u32 most = std::min(settings.most, k_max_figures);
    const double made_within = std::clamp(static_cast<double>(settings.distance), 1.0, 4000.0);
    const double kept_within = made_within + std::clamp(static_cast<double>(settings.keep), 0.0, 1000.0);
    if (bodies != nullptr) {
        for (const world::PersonId id : bodies->let_go()) {
            if (m_of.size() >= most || m_of.contains(id) || bodies->holds(id)) continue;
            const world::Person* person = world::find_person(scene.people, id);
            const auto named = person != nullptr ? m_look_of.find(person->look) : m_look_of.end();
            if (named == m_look_of.end()) continue;
            const u32 look = named->second;
            need(look, k_stand);
            if (m_poses[usize{look} * k_kinds].meshes.empty()) continue; // (made when the list comes round, its pose there)
            const Pick now = pick(scene, *person, look);
            if (now.hidden || square_distance(now.at.position, camera) > kept_within * kept_within) continue;
            if (make(scene, *person, static_cast<u32>(person - scene.people.data()), look, now)) ++m_stats.taken_over;
        }
    }
    // Those whose person is no longer in the scene (a stretch of the slots a frame).
    for (u32 k = 0; k < k_gone_looked_a_frame && k < m_info.size(); ++k) {
        const u32 s = static_cast<u32>(m_gone_cursor++ % m_info.size());
        if (m_info[s].person != 0 && world::find_person(scene.people, m_info[s].person) == nullptr) remove(s);
    }
    // More than may be (`most` lowered): any go.
    while (m_of.size() > most) remove(m_of.begin()->second);

    move(scene);

    // The next stretch of the list: those within `distance` are made figures, those gone off are
    // figures no longer, and each one kept is put as it is now (where it is, what it does).
    const std::vector<world::Person>& people = scene.people;
    if (m_cursor >= people.size()) {
        m_cursor = 0;
        m_stats.waiting = m_waiting_now;
        m_stats.hidden = m_hidden_now;
        m_waiting_now = 0;
        m_hidden_now = 0;
    }
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
        u32 look = has ? m_info[it->second].look : ~0u;
        if (!has || m_looks[look].name != person.look) { // (new, or given another look)
            const auto named = m_look_of.find(person.look);
            if (named == m_look_of.end()) { // (no such look)
                if (has) remove(it->second);
                continue;
            }
            look = named->second;
        }
        need(look, k_stand);
        // (One travelling is put on in its turn, above: where it has got to, and that it has arrived.)
        if (has && m_info[it->second].moving != ~0u && look == m_info[it->second].look) continue;
        if (!has && m_poses[usize{look} * k_kinds].meshes.empty()) continue; // (its standing pose not there yet)
        const Pick now = pick(scene, person, look);
        if (now.hidden) {
            if (has) remove(it->second);
            ++m_hidden_now;
            continue;
        }
        const u32 person_at = static_cast<u32>(m_cursor);
        if (has) {
            // (It travelled, was put elsewhere, does something else, or the ground under it changed.)
            put(scene, it->second, person, person_at, look, now);
            set_moving(it->second, now.moving);
            continue;
        }
        if (m_of.size() >= most) {
            ++m_waiting_now;
            continue;
        }
        if (made >= a_frame) break; // (this one first next frame)
        if (!make(scene, person, person_at, look, now)) {
            ++m_waiting_now;
            continue;
        }
        ++made;
    }

    m_draws.slots = m_slots;
    m_draws.versions = m_versions;
    m_stats.figures = static_cast<u32>(m_of.size());
    m_stats.slots = static_cast<u32>(m_slots.size());
    m_stats.walking = static_cast<u32>(m_moving.size());
    m_stats.acting = m_acting;
    m_stats.poses = static_cast<u32>(m_asked.size());
}

} // namespace giant::editor
