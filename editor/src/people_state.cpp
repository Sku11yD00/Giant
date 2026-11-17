#include "people_state.h"

#include "animation_state.h"
#include "mesh_library.h"
#include "people_routines.h"
#include "ragdoll_state.h"
#include "walker_state.h"
#include "world_streaming.h"

#include "giant/world/cell.h"
#include "giant/world/people.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace giant::editor {

namespace {

double square_distance(const DVec3& a, const DVec3& b) {
    const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

} // namespace

world::Entity PeopleState::entity_for(const world::Person& person, const world::PersonLook& look,
                                      const PeopleTravel::Where& at, double ground) {
    world::Entity e;
    e.name = person.name.empty() ? "person " + std::to_string(person.id) : person.name;
    e.shape = world::Shape::mesh;
    e.mesh = look.mesh;
    e.transform.position = {at.position.x, ground, at.position.z};
    e.transform.rotation = quat_from_axis_angle({0.0f, 1.0f, 0.0f}, degrees_to_radians(at.yaw));
    e.transform.scale = {look.height, look.height, look.height};
    e.material = look.material;
    e.parts = look.parts;
    e.hair = look.hair;
    e.animation.matched = look.matched;
    if (e.animation.matched.empty()) e.animation.matched = {"*"}; // (a look's empty list is every clip; an entity's is none)
    e.body = world::BodyKind::none; // (it walks as a character, not as a body standing where it was put)
    e.person = person.id;
    return e;
}

world::EntityId PeopleState::entity_of(const world::Scene& scene, world::PersonId id) const {
    if (scene.identity() != m_scene || !m_held.contains(id)) return 0;
    const world::EntityId entity = world::person_entity_id(id);
    return scene.find(entity) != nullptr ? entity : 0; // (one deleted since the last frame is not there)
}

// Lets a person's entity go: taken from the scene with the frame's others, at once (flush).
void PeopleState::release(world::PersonId id) {
    if (m_held.erase(id) == 0) return;
    if (travel != nullptr) travel->hold(id, false);
    if (walkers != nullptr) walkers->stop(world::person_entity_id(id)); // (else it would walk on as the next entity of that id)
    m_entities.push_back(world::person_entity_id(id));
    ++m_stats.released;
}

void PeopleState::flush(world::Scene& scene) {
    if (!m_entities.empty()) scene.remove_many(m_entities); // one pass through the scene's list, however many
    m_entities.clear();
}

void PeopleState::release_all(world::Scene& scene) {
    if (scene.identity() == m_scene) {
        m_entities.clear();
        for (const auto& [id, held] : m_held) m_entities.push_back(world::person_entity_id(id));
        m_stats.released += m_entities.size();
        flush(scene);
    }
    if (travel != nullptr) {
        for (const auto& [id, held] : m_held) travel->hold(id, false);
    }
    if (walkers != nullptr) {
        for (const auto& [id, held] : m_held) walkers->stop(world::person_entity_id(id));
    }
    m_held.clear();
    m_stats.embodied = 0;
    m_stats.walking = 0;
}

// Another scene (a new one, one loaded, one an undo brought back): nothing
// is held in it; entities standing for people that came with it (a copy
// taken while they stood) go.
void PeopleState::take_up(world::Scene& scene) {
    m_scene = scene.identity();
    if (travel != nullptr) {
        for (const auto& [id, held] : m_held) travel->hold(id, false);
    }
    m_held.clear();
    m_looks = scene.person_looks;
    m_look_cursor = 0;
    m_cursor = 0;
    m_waiting_now = 0;
    m_stats = {};
    m_entities.clear();
    for (const world::Entity& e : scene.entities()) {
        if (e.person != 0) m_entities.push_back(e.id);
    }
    flush(scene);
}

// Entities standing for people nobody is held for (put back by an undo
// after their person went or moved off): taken away. Only what changed
// since the last frame is looked at.
void PeopleState::strays(world::Scene& scene) {
    const world::SceneChanges& changes = scene.untaken_changes();
    m_entities.clear();
    if (changes.all) {
        for (const world::Entity& e : scene.entities()) {
            if (e.person != 0 && !m_held.contains(e.person)) m_entities.push_back(e.id);
        }
    } else {
        for (const world::EntityId id : changes.ids) {
            const world::Entity* e = scene.find(id);
            if (e != nullptr && e->person != 0 && !m_held.contains(e->person)) m_entities.push_back(id);
        }
    }
    flush(scene);
}

namespace {

double flat_distance(const DVec3& a, const DVec3& b) { return std::hypot(a.x - b.x, a.z - b.z); }

// How far along its trip's way one at `feet` is: the nearest point of the
// stretches that begin within k_walk_reach of where it had got to (the one
// it is on, and those a corner cut or a short link has it on already),
// never behind `gone`, and no more than k_walk_reach on at a time (a way
// that passes near itself is not jumped along). One far off its way has
// got no farther.
double along_way(const PeopleTravel::Trip& trip, double gone, const DVec3& feet) {
    const std::vector<DVec3>& points = trip.points;
    const std::vector<double>& along = trip.along;
    if (points.size() < 2) return gone;
    usize k = static_cast<usize>(std::upper_bound(along.begin(), along.end(), gone) - along.begin());
    k = std::clamp<usize>(k, 1, points.size() - 1);
    const double reach = gone + PeopleState::k_walk_reach;
    double best = gone, nearest = PeopleState::k_walk_astray;
    for (usize j = k; j < points.size() && along[j - 1] <= reach; ++j) {
        const DVec3& a = points[j - 1];
        const DVec3& b = points[j];
        const double dx = b.x - a.x, dz = b.z - a.z;
        const double square = dx * dx + dz * dz;
        const double t = square > 0.0 ? std::clamp(((feet.x - a.x) * dx + (feet.z - a.z) * dz) / square, 0.0, 1.0) : 1.0;
        const double off = std::hypot(feet.x - (a.x + dx * t), feet.z - (a.z + dz * t));
        if (off < nearest) {
            nearest = off;
            best = along[j - 1] + t * (along[j] - along[j - 1]);
        }
    }
    return std::clamp(best, gone, reach);
}

} // namespace

// One who was walking stands where it has got to: the scene's entity is
// put there (it was drawn there by its walker), and the walker let go.
PeopleTravel::Where PeopleState::stand(world::Scene& scene, Held& held, world::EntityId entity) {
    PeopleTravel::Where there = held.at;
    const WalkerState::Walker* walker = walkers != nullptr ? walkers->find(entity) : nullptr;
    if (walker != nullptr) {
        WalkerState::Walker now = *walker; // (its feet as the physics has them now, not as last steered)
        if (const std::optional<DVec3> feet = walkers->feet_of(scene, entity)) now.feet = *feet;
        if (world::Entity* e = scene.modify(entity)) e->transform = WalkerState::stands(now);
        there.position = now.feet;
        there.yaw = radians_to_degrees(now.yaw);
        walkers->stop(entity);
        held.moved = true;
    }
    held.walking = false;
    held.waited = false;
    held.path_end = 0.0;
    return there;
}

// Those held who are on a trip walk its way: each as a walker given the
// next stretch of the way ahead (straight between the way's points: the
// physics keeps it on the ground and out of walls, walkers step round each
// other), the trip told how far it has got.
void PeopleState::walk(world::Scene& scene) {
    m_stats.walking = 0;
    if (travel == nullptr || walkers == nullptr || !travel->running()) return;
    const double now = travel->seconds();
    u32 begun = 0;
    for (auto& [id, held] : m_held) {
        const world::EntityId entity = world::person_entity_id(id);
        const PeopleTravel::Trip* trip = travel->trip(id);
        if (trip == nullptr || trip->phase != PeopleTravel::Phase::travelling) {
            if (held.walking) stand(scene, held, entity); // (stopped on its way)
            continue;
        }
        const WalkerState::Walker* walker = walkers->find(entity);
        if (held.walking && walker == nullptr) {
            // Its walker was taken (struck down, hidden, let go by command): it waits before it is given another.
            held.walking = false;
            held.path_end = 0.0;
            held.retry_at = now + k_walk_wait;
        }
        const bool fresh = held.trip != trip->serial;
        if (fresh) { // (sent elsewhere: the path it has, and any wait it was in, are the old trip's)
            held.path_end = 0.0;
            held.waited = false;
            if (walker != nullptr) held.retry_at = 0.0;
        }
        held.trip = trip->serial;
        const std::optional<DVec3> feet = walkers->feet_of(scene, entity);
        if (!feet) continue;
        const double length = PeopleTravel::length(*trip);
        double gone = travel->gone(*trip);
        if (held.walking) {
            gone = along_way(*trip, gone, *feet);
            travel->walked(id, gone);
        }
        // There: at its way's end, or stopped by its follower on the way's
        // last stretch (as near the end as it comes: others stand there).
        const bool stopped = held.walking && !fresh && walker->phase == WalkerState::Phase::arrived;
        const double to_end = flat_distance(*feet, trip->points.back());
        if ((length - gone <= k_walk_arrive && to_end <= k_walk_end) ||
            (stopped && held.path_end >= length && to_end <= 2.0 * k_walk_end)) {
            const bool walked = held.walking;
            const PeopleTravel::Where there = stand(scene, held, entity);
            travel->arrive(id, walked ? there : PeopleTravel::at(*trip, length));
            continue;
        }
        const bool failed = held.walking && !fresh && walker->phase == WalkerState::Phase::failed;
        if (held.walking && !failed && !stopped && held.path_end > 0.0 &&
            (held.path_end >= length || held.path_end - gone >= k_walk_renew)) {
            ++m_stats.walking;
            continue; // (walking, with way enough ahead)
        }
        if (failed && !held.waited) { // (stuck: it waits a while before it tries again)
            held.waited = true;
            held.retry_at = now + k_walk_stuck_wait;
        } else if (stopped && !held.waited) { // (stopped short of its way's end, or far from it: likewise, sooner)
            held.waited = true;
            held.retry_at = now + k_walk_wait;
        }
        if (now < held.retry_at || begun >= k_walks_begun_a_frame) continue;
        // Not one hidden or lying struck down (its walker would be taken again at once), and not
        // the last walkers there are (kept for walk_to and for those getting up).
        const world::Entity* e = scene.find(entity);
        const bool lying = ragdolls != nullptr && ragdolls->find(entity) != nullptr;
        if (e == nullptr || !e->visible || lying ||
            (walker == nullptr && walkers->size() + k_walkers_kept_back >= WalkerState::k_max_walkers)) {
            held.retry_at = now + k_walk_wait;
            continue;
        }
        // The next stretch: from its feet through the way's points to k_walk_ahead on.
        const double end = std::min(length, gone + k_walk_ahead);
        m_path.clear();
        m_path.push_back(*feet);
        const std::vector<double>& along = trip->along;
        for (usize j = static_cast<usize>(std::upper_bound(along.begin(), along.end(), gone + 0.5) - along.begin());
             j < along.size() && along[j] < end - 0.5; ++j) {
            m_path.push_back(trip->points[j]);
        }
        m_path.push_back(PeopleTravel::at(*trip, end).position);
        ++begun;
        const float speed = static_cast<float>(std::min(trip->speed, 20.0));
        // (One standing in a stretch that is not the way's last is walked round, not stopped at.)
        const float pack = end >= length ? static_cast<float>(k_walk_end) : 0.0f;
        if (!walkers->walk_to(scene, entity, m_path.back(), speed, &m_path, pack).empty()) {
            held.walking = false;
            held.retry_at = now + k_walk_wait; // (no character for it: it stands meanwhile)
            continue;
        }
        held.walking = true;
        held.waited = false;
        held.path_end = end;
        ++m_stats.walking;
    }
}

// Those held who stand where their routine has them show what they do
// there: the clip their look gives the activity, played as a command would
// (it takes over from the motion matching, which is given back when one
// walks again, does something its look has no clip for, or the world stops).
void PeopleState::act(const world::Scene& scene) {
    m_stats.acting = 0;
    if (animation == nullptr || meshes == nullptr) return;
    const bool running = routines != nullptr && travel != nullptr && travel->running();
    const double now = travel != nullptr ? travel->seconds() : 0.0;
    u32 begun = 0, made = 0;
    for (auto& [id, held] : m_held) {
        if (held.acting == 0 && !running) continue;
        const world::EntityId entity = world::person_entity_id(id);
        if (held.acting != 0) {
            // What it played was taken from it (set_animation, its file read again): it plays nothing of
            // ours, and its clip is begun again after a wait. (A clip played on it by hand, play_animation,
            // is not told from ours: it is left until what the person does next changes.)
            const AnimationState::Animated* a = animation->find(entity);
            if (a == nullptr || !a->by_command) {
                held.acting = 0;
                held.act_at = now + k_act_wait;
            }
        }
        // Its look (where it was last found, else looked for) and what the look shows for what it does.
        if (held.look >= m_looks.size() || m_looks[held.look].name != held.was.look) {
            const world::PersonLook* look = world::find_person_look(m_looks, held.was.look);
            held.look = look != nullptr ? static_cast<u32>(look - m_looks.data()) : 0;
        }
        const bool found = held.look < m_looks.size() && m_looks[held.look].name == held.was.look;
        const std::span<const world::LookActivity> shown =
            found ? std::span<const world::LookActivity>(m_looks[held.look].activities) : std::span<const world::LookActivity>();
        if (shown.empty() && held.acting == 0) continue;
        u8 wanted = 0;
        PeopleRoutines::Activity doing;
        // (A walker left standing, one got up again or walked by hand, is no walk.)
        const WalkerState::Walker* walker = walkers != nullptr ? walkers->find(entity) : nullptr;
        const bool walks = walker != nullptr && walker->phase != WalkerState::Phase::arrived;
        if (running && !shown.empty() && !held.walking && !walks && travel->trip(id) == nullptr &&
            (ragdolls == nullptr || ragdolls->find(entity) == nullptr) && routines->activity(scene, id, doing)) {
            // There: told to stay where it is, or within reach of the place it was sent to (one stopped
            // by those standing there has arrived up to 2 k_walk_end short of its spot: walk).
            const double reach = doing.place != nullptr ? static_cast<double>(doing.place->radius) + 2.0 * k_walk_end : 0.0;
            const bool there = doing.anywhere || (doing.place != nullptr && std::hypot(held.at.position.x - doing.place->position.x,
                                                                                    held.at.position.z - doing.place->position.z) <= reach);
            for (usize i = 0; i < shown.size() && there; ++i) {
                if (shown[i].activity == doing.activity) {
                    wanted = static_cast<u8>(i + 1);
                    break;
                }
            }
        }
        if (wanted == held.acting) {
            m_stats.acting += wanted != 0 ? 1 : 0;
            continue;
        }
        if (held.acting != 0) { // (what it played is over: matched again at once, whatever is begun next)
            animation->restart_saved(scene, entity);
            held.acting = 0;
        }
        if (wanted == 0 || wanted == held.act_failed || now < held.act_at || begun >= k_acts_begun_a_frame) continue;
        // One not animated yet (its file cannot be matched, or its turn has not come) is made its copy
        // of the character by the play: a few of those a frame, beside the animation's own.
        const bool animated = animation->find(entity) != nullptr;
        if (!animated && made >= k_acts_made_a_frame) continue;
        const world::LookActivity& a = shown[wanted - 1];
        switch (AnimationState::playable(scene, *meshes, entity, a.clip)) {
        case AnimationState::Playable::no_clip: // (the look names a clip its file does not have: not tried again this run, or until the look's list changes)
            held.act_failed = wanted;
            continue;
        case AnimationState::Playable::not_ready:
            held.act_at = now + k_act_wait;
            continue;
        case AnimationState::Playable::yes: break;
        }
        ++begun;
        made += animated ? 0 : 1;
        if (animation->play(scene, *meshes, entity, a.clip, k_act_fade, a.loop, a.speed, -1.0f).empty()) {
            held.acting = wanted;
            ++m_stats.acting;
        } else { // (no room to animate one more: tried again later)
            held.act_at = now + k_act_wait;
        }
    }
}

void PeopleState::update(world::Scene& scene, const DVec3& camera, const WorldStreaming* world) {
    if (scene.identity() != m_scene) take_up(scene);
    strays(scene);
    m_entities.clear();
    if (!settings.on || hold) {
        if (!m_held.empty()) release_all(scene);
        m_stats.waiting = 0;
        return;
    }
    const double embody = std::clamp(static_cast<double>(settings.embody_distance), 1.0, 1000.0);
    const double let_go = std::max(embody, static_cast<double>(settings.release_distance));
    const u32 most = std::min(settings.most, k_max_embodied);

    // A look changed (one a frame is compared; all at once when their number
    // changed): the entities made from it go, and are made again from the new one.
    const std::vector<world::PersonLook>& looks = scene.person_looks;
    const auto look_changed = [&](const std::string& name) {
        m_scratch.clear();
        for (const auto& [id, held] : m_held) {
            if (held.was.look == name) m_scratch.push_back(id);
        }
        for (const world::PersonId id : m_scratch) release(id);
    };
    if (looks.size() != m_looks.size()) {
        release_all(scene); // (a look made or removed: rare; everyone is made again over the next frames)
        m_looks = looks;
        m_look_cursor = 0;
    } else if (!looks.empty()) {
        m_look_cursor = m_look_cursor % looks.size();
        const world::PersonLook& is = looks[m_look_cursor];
        world::PersonLook& was = m_looks[m_look_cursor];
        if (is.name == was.name && is.mesh == was.mesh && is.height == was.height && is.material == was.material &&
            is.parts == was.parts && is.hair == was.hair && is.matched == was.matched) {
            // Only what its people show for their activities, if anything: the entities stay, and
            // those playing a clip of the old list are matched again (and begin the new one's).
            if (!(is.activities == was.activities)) {
                for (auto& [id, held] : m_held) {
                    if (held.was.look != was.name) continue;
                    if (held.acting != 0 && animation != nullptr) animation->restart_saved(scene, world::person_entity_id(id));
                    held.acting = 0;
                    held.act_failed = 0;
                    held.act_at = 0.0;
                }
                was.activities = is.activities;
            }
        } else {
            look_changed(was.name);
            was = is;
        }
        ++m_look_cursor;
    }

    // Those held: gone (the entity deleted, the person removed or changed,
    // or put back somewhere else: the simulation stopped), too far, or in a
    // cell whose file left the scene. Where each is is followed (one
    // walking its trip moves).
    m_scratch.clear();
    const u64 resets = travel != nullptr ? travel->resets() : 0;
    for (auto& [id, held] : m_held) {
        const world::Person* person = world::find_person(scene.people, id);
        if (person != nullptr && person->routine != held.was.routine) held.was.routine = person->routine; // (nothing its entity shows)
        bool there = scene.find(world::person_entity_id(id)) != nullptr && person != nullptr && *person == held.was;
        if (there) {
            const PeopleTravel::Where now = where(*person);
            if (resets != held.resets && (held.moved || now.position != held.at.position)) {
                there = false; // (everyone was put back, and this one or its entity was elsewhere)
            } else {
                if (resets != held.resets) { // (another run: its clock begins again, nothing of the last holds)
                    if (held.walking && walkers != nullptr) walkers->stop(world::person_entity_id(id)); // (begun, never stepped)
                    held.walking = false;
                    held.waited = false;
                    held.path_end = 0.0;
                    held.retry_at = 0.0;
                    held.act_at = 0.0;
                    held.act_failed = 0;
                }
                held.at.position = now.position;
                held.resets = resets;
            }
        }
        if (!there || square_distance(held.at.position, camera) > let_go * let_go ||
            (world != nullptr && world->ground_coming(world::cell_of(held.at.position)))) {
            m_scratch.push_back(id);
        }
    }
    for (const world::PersonId id : m_scratch) release(id);
    // More held than may be (`most` lowered): any go.
    while (m_held.size() > most) release(m_held.begin()->first);
    flush(scene);
    walk(scene);
    act(scene);

    // The next stretch of the list: those near enough are made, a few a frame.
    const std::vector<world::Person>& people = scene.people;
    if (m_cursor >= people.size()) {
        m_cursor = 0;
        m_stats.waiting = m_waiting_now;
        m_waiting_now = 0;
    }
    const usize end = std::min(people.size(), m_cursor + std::clamp<usize>(settings.scanned_a_frame, 1, 65536));
    u32 made = 0;
    const u32 a_frame = std::clamp<u32>(settings.made_a_frame, 1, 64);
    for (; m_cursor < end; ++m_cursor) {
        const world::Person& person = people[m_cursor];
        const PeopleTravel::Where at = where(person);
        if (square_distance(at.position, camera) > embody * embody || m_held.contains(person.id)) continue;
        if (world != nullptr && world->ground_coming(world::cell_of(at.position))) continue; // its ground first
        if (m_held.size() >= most) {
            ++m_waiting_now;
            continue;
        }
        if (made >= a_frame) break; // (this one first next frame)
        const world::PersonLook* look = world::find_person_look(m_looks, person.look);
        if (look == nullptr) continue; // (a look not taken up yet: next time through)
        const std::optional<double> ground = scene.terrain.height_at(at.position.x, at.position.z);
        if (scene.add(entity_for(person, *look, at, ground ? *ground : at.position.y)) == 0) continue;
        // (One made again in the frame its last was taken, under the same id: what that one played is not this one's.)
        if (animation != nullptr) animation->restart_saved(scene, world::person_entity_id(person.id));
        if (travel != nullptr) travel->hold(person.id, true); // (its trip goes on as its entity walks)
        Held held;
        held.was = person;
        held.at = at;
        held.resets = resets;
        m_held.emplace(person.id, std::move(held));
        ++made;
        ++m_stats.made;
    }
    m_stats.embodied = static_cast<u32>(m_held.size());
}

} // namespace giant::editor
