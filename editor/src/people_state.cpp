#include "people_state.h"

#include "world_streaming.h"

#include "giant/world/cell.h"
#include "giant/world/people.h"

#include <algorithm>
#include <optional>
#include <string>
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
    m_held.clear();
    m_stats.embodied = 0;
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

void PeopleState::update(world::Scene& scene, const DVec3& camera, const WorldStreaming* world) {
    if (scene.identity() != m_scene) take_up(scene);
    strays(scene);
    m_entities.clear();
    if (!settings.on) {
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
        if (!(looks[m_look_cursor] == m_looks[m_look_cursor])) {
            look_changed(m_looks[m_look_cursor].name);
            m_looks[m_look_cursor] = looks[m_look_cursor];
        }
        ++m_look_cursor;
    }

    // Those held: gone (the entity deleted, the person removed or changed,
    // or no longer where the entity stands: the simulation stopped and put
    // it back; the way it faces is the entity's own while it stands), too far, or in a cell whose file left the scene.
    m_scratch.clear();
    for (const auto& [id, held] : m_held) {
        const world::Person* person = world::find_person(scene.people, id);
        const bool there = scene.find(world::person_entity_id(id)) != nullptr && person != nullptr &&
                           *person == held.was && where(*person).position == held.at.position;
        if (!there || square_distance(held.at.position, camera) > let_go * let_go ||
            (world != nullptr && world->ground_coming(world::cell_of(held.at.position)))) {
            m_scratch.push_back(id);
        }
    }
    for (const world::PersonId id : m_scratch) release(id);
    // More held than may be (`most` lowered): any go.
    while (m_held.size() > most) release(m_held.begin()->first);
    flush(scene);

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
        if (travel != nullptr) travel->hold(person.id, true); // (it stands while it has an entity)
        m_held.emplace(person.id, Held{person, at});
        ++made;
        ++m_stats.made;
    }
    m_stats.embodied = static_cast<u32>(m_held.size());
}

} // namespace giant::editor
