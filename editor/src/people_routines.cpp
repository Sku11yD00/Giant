#include "people_routines.h"

#include "people_senses.h"
#include "people_travel.h"

#include "giant/world/people.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace giant::editor {

namespace {

u32 mixed(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// 0 to under 1, a person's own (and another for each `salt`).
double share(world::PersonId id, u32 salt) { return static_cast<double>(mixed(id * 0x9e3779b9u + salt)) / 4294967296.0; }

u32 hashed(u32 h, std::string_view text) {
    for (const char c : text) h = (h ^ static_cast<u8>(c)) * 16777619u;
    return (h ^ 0xffu) * 16777619u; // (where it ends counts: "ab" then "c" is not "a" then "bc")
}

// What an entry says to this person, hashed: where to be and how fast to go
// there (never 0). Not which entry it is, nor what is done there: the next
// entry at the same place is nothing new to act on.
u32 said(const world::Person& person, const world::RoutineEntry& e) {
    u32 h = 2166136261u;
    h = (h ^ static_cast<u32>(e.at)) * 16777619u;
    h = hashed(h, e.name);
    // (Staying says nothing of a speed: two that stay say the same.)
    if (e.at != world::RoutineAt::stay) h = (h ^ static_cast<u32>(std::lround(static_cast<double>(e.speed) * 1000.0))) * 16777619u;
    if (e.at == world::RoutineAt::home || e.at == world::RoutineAt::tag) h = hashed(h, person.home);
    return h == 0 ? 1 : h;
}

u64 mixed64(u64 x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

// A person's own spot in a place: within most of the radius, evenly over the disc.
DVec3 spot_in(const world::Place& place, world::PersonId id) {
    const double r = 0.8 * static_cast<double>(place.radius) * std::sqrt(share(id, 2));
    const double a = 6.283185307179586 * share(id, 3);
    return {place.position.x + r * std::cos(a), place.position.y, place.position.z + r * std::sin(a)};
}

constexpr usize k_near_kept = 1024;        // answers kept: the place with a tag nearest a home
constexpr i64 k_measured_a_frame = 16384;  // tagged places measured for the nearest, a frame
constexpr usize k_places_a_frame = 256;    // places looked at for having changed, a frame

} // namespace

// (The lists' hashes are this editor's own: no file's names can be made to share one.)
PeopleRoutines::PeopleRoutines()
    : m_seed(mixed64(static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
                     static_cast<u64>(reinterpret_cast<std::uintptr_t>(this)))),
      m_changes(k_changes_kept) {}

void PeopleRoutines::changes_since(const world::Scene& scene, u64& told, usize most, std::vector<Change>& out) const {
    told = std::min(std::max(told, changes_from() - 1), m_changes_made); // (one told of more than happened: of all)
    for (usize gathered = 0; told < m_changes_made && gathered < most;) {
        Change c;
        if (!change(scene, ++told, c)) continue;
        out.push_back(c);
        ++gathered;
    }
}

void PeopleRoutines::changed(const world::Behaviour& behaviour, const Slot& slot, u8 from, double seconds) {
    Changed& c = m_changes[static_cast<usize>(m_changes_made % k_changes_kept)];
    ++m_changes_made;
    c.person = slot.id;
    c.seconds = seconds;
    c.behaviour_name = slot.behaviour_name;
    c.behaviour = slot.behaviour;
    c.from = from;
    c.to = slot.state;
    c.from_name = from != k_no_state ? static_cast<u32>(hash_of(behaviour.states[from].name)) : 0;
    c.to_name = static_cast<u32>(hash_of(behaviour.states[slot.state].name));
}

u64 PeopleRoutines::changes_from() const {
    const u64 kept = m_changes_made > k_changes_kept ? m_changes_made - k_changes_kept + 1 : 1;
    return std::max(kept, m_changes_from);
}

bool PeopleRoutines::change(const world::Scene& scene, u64 serial, Change& out) const {
    if (serial < changes_from() || serial > m_changes_made || scene.identity() != m_scene) return false;
    const Changed& c = m_changes[static_cast<usize>((serial - 1) % k_changes_kept)];
    if (c.behaviour >= scene.behaviours.size()) return false;
    const world::Behaviour& behaviour = scene.behaviours[c.behaviour];
    if (static_cast<u32>(hash_of(behaviour.name)) != c.behaviour_name || c.to >= behaviour.states.size()) return false;
    if (c.from != k_no_state && c.from >= behaviour.states.size()) return false;
    // (The behaviour set again since, with other states at those places: not told by their names.)
    if (static_cast<u32>(hash_of(behaviour.states[c.to].name)) != c.to_name) return false;
    if (c.from != k_no_state && static_cast<u32>(hash_of(behaviour.states[c.from].name)) != c.from_name) return false;
    out.serial = serial;
    out.person = c.person;
    out.behaviour = behaviour.name;
    out.from = c.from != k_no_state ? std::string_view(behaviour.states[c.from].name) : std::string_view();
    out.to = behaviour.states[c.to].name;
    out.seconds = c.seconds;
    return true;
}

u64 PeopleRoutines::hash_of(std::string_view text) const {
    u64 h = m_seed;
    for (const char c : text) h = (h ^ static_cast<u8>(c)) * 1099511628211ull;
    return mixed64(h ^ text.size());
}

// What of a place the lists and the answers kept depend on: its name, its tags, where it is.
u32 PeopleRoutines::sign_of(const world::Place& place) const {
    u64 h = hash_of(place.name);
    for (const std::string& tag : place.tags) h = mixed64(h ^ hash_of(tag));
    h = mixed64(h ^ std::bit_cast<u64>(place.position.x));
    h = mixed64(h ^ std::bit_cast<u64>(place.position.y));
    h = mixed64(h ^ std::bit_cast<u64>(place.position.z));
    return static_cast<u32>(h ^ (h >> 32));
}

double PeopleRoutines::hour_of(world::PersonId id, double hours) const {
    const double spread = std::clamp(static_cast<double>(settings.spread_minutes), 0.0, 120.0) / 60.0;
    double h = (std::isfinite(hours) ? std::fmod(hours, 24.0) : 0.0) - spread * share(id, 1);
    if (h < 0.0) h += 24.0;
    return h >= 24.0 ? 0.0 : h;
}

void PeopleRoutines::index_places(const world::Scene& scene) {
    const std::vector<world::Place>& places = scene.places;
    m_by_name.clear();
    m_by_tag.clear();
    m_signs.clear();
    m_by_name.reserve(places.size());
    m_signs.reserve(places.size());
    for (u32 i = 0; i < places.size(); ++i) {
        m_by_name.push_back({hash_of(places[i].name), 0.0, i});
        for (const std::string& tag : places[i].tags) m_by_tag.push_back({hash_of(tag), places[i].position.x, i});
        m_signs.push_back(sign_of(places[i]));
    }
    std::sort(m_by_name.begin(), m_by_name.end());
    std::sort(m_by_tag.begin(), m_by_tag.end());
    m_near.assign(k_near_kept, Near{});
    m_indexed = true;
    m_place_cursor = 0;
}

// The lists follow the scene's places: made again at once when their number
// changes, and when one of the stretch looked at this frame is no longer
// what it was (an edit's cost). Those who could not be sent then try again.
void PeopleRoutines::follow_places(const world::Scene& scene) {
    const std::vector<world::Place>& places = scene.places;
    bool changed = m_indexed && m_signs.size() != places.size();
    if (m_indexed && !changed) {
        if (m_place_cursor >= places.size()) m_place_cursor = 0;
        const usize end = std::min(places.size(), m_place_cursor + k_places_a_frame);
        for (usize i = m_place_cursor; i < end && !changed; ++i) changed = sign_of(places[i]) != m_signs[i];
        m_place_cursor = end;
    }
    if (m_indexed && !changed) return;
    index_places(scene);
    if (changed) try_failed_again();
}

// Those who could not be sent act on their entry again when next looked at (the places or the roads changed).
void PeopleRoutines::try_failed_again() {
    for (Slot& slot : m_slots) {
        if (!slot.failed) continue;
        slot.failed = false;
        slot.acted = 0;
        slot.settled = false; // (not acted on as things are now: it has neither arrived nor failed)
    }
}

// The slots put by the people as they are now, each keeping its own (both
// lists are in the order of the ids): people made or removed, an edit's cost.
void PeopleRoutines::match_slots(const std::vector<world::Person>& people) {
    m_slots_before.swap(m_slots);
    m_slots.clear();
    m_slots.reserve(people.size());
    usize had = 0;
    for (const world::Person& person : people) {
        while (had < m_slots_before.size() && m_slots_before[had].id < person.id) ++had;
        Slot slot;
        if (had < m_slots_before.size() && m_slots_before[had].id == person.id) slot = m_slots_before[had];
        slot.id = person.id;
        m_slots.push_back(slot);
    }
    if (m_cursor != 0) m_miscounted = true; // (part of a pass was counted among the people as they were)
    if (m_cursor >= m_slots.size()) m_cursor = 0;
}

const world::Place* PeopleRoutines::named(const world::Scene& scene, std::string_view name) const {
    const std::vector<world::Place>& places = scene.places;
    const u64 hash = hash_of(name);
    for (auto it = std::lower_bound(m_by_name.begin(), m_by_name.end(), Key{hash, 0.0, 0}); it != m_by_name.end() && it->hash == hash; ++it) {
        if (it->place < places.size() && places[it->place].name == name) return &places[it->place];
    }
    return nullptr;
}

// The place with the tag nearest `from` (of two as near, the one made
// first). Those with the tag are listed in the order of their x: looked at
// outward from `from`'s both ways, each way until x alone is farther than
// the nearest found, so few are measured however many have the tag.
const world::Place* PeopleRoutines::tagged_nearest(const world::Scene& scene, std::string_view tag, const DVec3& from,
                                                   i64& measured) const {
    const std::vector<world::Place>& places = scene.places;
    const u64 hash = hash_of(tag);
    const auto begin = std::lower_bound(m_by_tag.begin(), m_by_tag.end(), Key{hash, -std::numeric_limits<double>::infinity(), 0});
    auto end = begin;
    while (end != m_by_tag.end() && end->hash == hash) ++end; // (counted, not measured: a step each)
    const world::Place* nearest = nullptr;
    double best = 0.0;
    const auto measure = [&](const Key& key) {
        ++measured;
        if (key.place >= places.size()) return;
        const world::Place& p = places[key.place];
        if (std::find(p.tags.begin(), p.tags.end(), tag) == p.tags.end()) return;
        const double dx = p.position.x - from.x, dy = p.position.y - from.y, dz = p.position.z - from.z;
        const double d = dx * dx + dy * dy + dz * dz;
        if (nearest == nullptr || d < best || (d == best && &p < nearest)) {
            nearest = &p;
            best = d;
        }
    };
    const auto too_far = [&](const Key& key) { return nearest != nullptr && (key.x - from.x) * (key.x - from.x) > best; };
    const auto middle = std::lower_bound(begin, end, from.x, [](const Key& key, double x) { return key.x < x; });
    for (auto it = middle; it != end && !too_far(*it); ++it) measure(*it);
    for (auto it = middle; it != begin && !too_far(*(it - 1)); --it) measure(*(it - 1));
    return nearest;
}

PeopleRoutines::Sought PeopleRoutines::seek(const world::Scene& scene, const world::Person& person, const world::RoutineEntry& entry,
                                            Goal& out, bool in_frame) {
    const std::vector<world::Place>& places = scene.places;
    const world::Place* place = nullptr;
    switch (entry.at) {
    case world::RoutineAt::stay: return Sought::none;
    case world::RoutineAt::home:
        place = person.home.empty() ? nullptr : named(scene, person.home);
        break;
    case world::RoutineAt::place:
        place = named(scene, entry.name);
        break;
    case world::RoutineAt::tag: {
        // The nearest with the tag to its home (to where it starts, with none): kept by home and tag in a frame.
        const world::Place* home = person.home.empty() ? nullptr : named(scene, person.home);
        Near* kept = nullptr;
        const u64 tag = hash_of(entry.name);
        const u32 home_index = home != nullptr ? static_cast<u32>(home - places.data()) : 0;
        if (in_frame && home != nullptr && m_near.size() == k_near_kept) {
            kept = &m_near[static_cast<usize>(mixed64(tag ^ (home_index * 0x9e3779b97f4a7c15ull)) % k_near_kept)];
            if (kept->place != 0 && kept->tag == tag && kept->home == home_index && kept->place - 1 < places.size()) {
                const world::Place& p = places[kept->place - 1];
                if (std::find(p.tags.begin(), p.tags.end(), std::string_view(entry.name)) != p.tags.end()) place = &p;
            }
        }
        if (place == nullptr) {
            if (in_frame && m_measure_left <= 0) return Sought::later;
            i64 measured = 0;
            place = tagged_nearest(scene, entry.name, home != nullptr ? home->position : person.position, measured);
            if (in_frame) m_measure_left -= measured;
            if (kept != nullptr && place != nullptr) *kept = {tag, home_index, static_cast<u32>(place - places.data()) + 1};
        }
        break;
    }
    }
    if (place == nullptr) return Sought::none;
    out.place = static_cast<usize>(place - places.data());
    out.spot = spot_in(*place, person.id);
    return Sought::found;
}

bool PeopleRoutines::goal(const world::Scene& scene, const world::Person& person, const world::RoutineEntry& entry, Goal& out) const {
    const world::Place* place = nullptr;
    switch (entry.at) {
    case world::RoutineAt::stay: return false;
    case world::RoutineAt::home:
        place = person.home.empty() ? nullptr : named(scene, person.home);
        break;
    case world::RoutineAt::place:
        place = named(scene, entry.name);
        break;
    case world::RoutineAt::tag: {
        const world::Place* home = person.home.empty() ? nullptr : named(scene, person.home);
        i64 measured = 0;
        place = tagged_nearest(scene, entry.name, home != nullptr ? home->position : person.position, measured);
        break;
    }
    }
    if (place == nullptr) return false;
    out.place = static_cast<usize>(place - scene.places.data());
    out.spot = spot_in(*place, person.id);
    return true;
}

// Another scene, another run, or the simulation stopped: followed (each frame, and before anything is put back).
void PeopleRoutines::follow(const world::Scene& scene, const PeopleTravel& travel) {
    bool afresh = travel.resets() != m_resets || travel.running() != m_running;
    if (scene.identity() != m_scene) { // (another scene: its people and places are others)
        m_scene = scene.identity();
        m_changes_from = m_changes_made + 1;
        m_slots.clear();
        m_indexed = false;
        afresh = true;
    }
    // Another run, or the simulation stopped: everyone is back, and begins afresh.
    if (afresh) {
        m_resets = travel.resets();
        m_running = travel.running();
        for (Slot& s : m_slots) {
            const world::PersonId id = s.id;
            s = Slot{};
            s.id = id;
        }
        m_cursor = 0;
        m_counted = 0;
        m_behaving_counted = 0;
        m_miscounted = false;
        m_builds = travel.builds();
        m_stats = Stats{};
    }
}

namespace {
// A routine's entry by the number a slot keeps: the day's, then those for the alarmed. Null: none such.
const world::RoutineEntry* entry_of(const world::Routine& routine, usize index) {
    if (index < routine.entries.size()) return &routine.entries[index];
    index -= routine.entries.size();
    return index < routine.alarmed.size() ? &routine.alarmed[index].does : nullptr;
}
// (What a state's own entry is acted on beside: no routine's entries.)
const world::Routine k_no_routine;
} // namespace

const world::RoutineEntry* PeopleRoutines::acted_on(const world::Scene& scene, const world::Person& person, const Slot& slot) const {
    if (slot.acted == 0) return nullptr;
    if (slot.by_state) {
        if (slot.behaviour >= scene.behaviours.size()) return nullptr;
        const world::Behaviour& behaviour = scene.behaviours[slot.behaviour];
        if (behaviour.name != person.behaviour || slot.entry >= behaviour.states.size() || !behaviour.states[slot.entry].own) return nullptr;
        return &behaviour.states[slot.entry].does;
    }
    if (slot.routine >= scene.routines.size() || scene.routines[slot.routine].name != person.routine) return nullptr;
    return entry_of(scene.routines[slot.routine], slot.entry);
}

void PeopleRoutines::update(const world::Scene& scene, PeopleTravel& travel, double hours, const PeopleSenses* senses) {
    follow(scene, travel);
    if (hold) return;
    const std::vector<world::Person>& people = scene.people;
    if (!settings.on || !m_running || people.empty() || (scene.routines.empty() && scene.behaviours.empty())) {
        if ((scene.routines.empty() && scene.behaviours.empty()) || people.empty()) {
            m_stats.following = 0;
            m_stats.behaving = 0;
        }
        return;
    }
    if (m_slots.size() != people.size()) match_slots(people);
    follow_places(scene);
    if (travel.builds() != m_builds) { // (the roads changed: one with no road near may have one now)
        m_builds = travel.builds();
        try_failed_again();
    }
    m_measure_left = k_measured_a_frame;
    const std::vector<world::Routine>& routines = scene.routines;
    const std::vector<world::Behaviour>& behaviours = scene.behaviours;
    const double seconds = travel.seconds();
    const usize looked = std::min<usize>(std::clamp<u32>(settings.looked_a_frame, 1, k_most_looked), people.size());
    const u32 most = std::clamp<u32>(settings.sent_a_frame, 1, k_most_sent);
    const double within = std::clamp(static_cast<double>(settings.within), 0.0, k_most_within);
    u32 acted = 0;
    for (usize n = 0; n < looked; ++n) {
        if (m_cursor >= people.size()) {
            m_cursor = 0;
            if (!m_miscounted) {
                m_stats.following = m_counted;
                m_stats.behaving = m_behaving_counted;
            }
            m_miscounted = false;
            m_counted = 0;
            m_behaving_counted = 0;
        }
        const world::Person& person = people[m_cursor];
        if (m_slots[m_cursor].id != person.id) { // (as many people as before, but others: made and removed)
            match_slots(people);
            continue;
        }
        Slot& slot = m_slots[m_cursor++];
        // Its behaviour: where it was last found, else looked for (none, or another: it begins in the first state).
        const world::Behaviour* behaviour = nullptr;
        if (!person.behaviour.empty()) {
            if (slot.behaviour < behaviours.size() && behaviours[slot.behaviour].name == person.behaviour) {
                behaviour = &behaviours[slot.behaviour];
            } else {
                behaviour = world::find_behaviour(behaviours, person.behaviour);
            }
            if (behaviour != nullptr) {
                // (The same one at another place in the list, one before it removed: it is in the state
                // it was in. Another, though found where the last was: the first.)
                const u32 name = static_cast<u32>(hash_of(person.behaviour));
                const bool began = slot.behaviour == k_no_behaviour || slot.behaviour_name != name;
                slot.behaviour = static_cast<u16>(behaviour - behaviours.data());
                slot.behaviour_name = name;
                if (began) {
                    slot.state = 0;
                    slot.entered = seconds;
                    slot.settled = false;
                    changed(*behaviour, slot, k_no_state, seconds);
                }
            }
        }
        if (behaviour == nullptr) {
            slot.behaviour = k_no_behaviour;
        } else if (slot.state >= behaviour->states.size()) { // (replaced by one with fewer states)
            slot.state = 0;
            slot.entered = seconds;
            slot.settled = false;
            changed(*behaviour, slot, k_no_state, seconds);
        }
        // Its routine: where it was last found, else looked for.
        const world::Routine* routine_found = nullptr;
        if (!person.routine.empty()) {
            if (slot.routine < routines.size() && routines[slot.routine].name == person.routine) {
                routine_found = &routines[slot.routine];
            } else if ((routine_found = world::find_routine(routines, person.routine)) != nullptr) {
                slot.routine = static_cast<u16>(routine_found - routines.data());
                // (Another routine, or its place in the list another: what it did was the old one's.)
                if (!slot.by_state) {
                    slot.acted = 0;
                    slot.failed = false;
                    slot.settled = false;
                }
            }
        }
        // The trip begun for what it last acted on, if it is still its trip.
        const auto own_trip = [&]() -> const PeopleTravel::Trip* {
            if (slot.trip == 0) return nullptr;
            const PeopleTravel::Trip* on = travel.trip(person.id);
            return on != nullptr && on->serial == slot.trip ? on : nullptr;
        };
        // What says nothing any more (no routine, no behaviour, or a state that leaves it to a routine
        // it has not): the trip begun for what it did is stopped, and it stands.
        const auto nothing = [&] {
            if (own_trip() != nullptr) travel.stop(person.id);
            slot.acted = 0;
            slot.trip = 0;
            slot.place = 0;
            slot.failed = false;
            slot.by_state = false;
        };
        if (routine_found == nullptr && behaviour == nullptr) {
            if (slot.by_state) {
                nothing(); // (its behaviour was taken away)
            } else if (person.routine.empty()) { // (as ever for one with no routine: a trip it is on goes on)
                slot.acted = 0;
                slot.trip = 0;
                slot.failed = false;
            } else { // (its routine is not there)
                slot.acted = 0;
            }
            continue;
        }
        ++m_counted;
        if (behaviour != nullptr) {
            ++m_behaving_counted;
            // The first transition whose conditions all hold is taken (one a look).
            // (A state's own is acted on before it is left: one whose turn to be sent has not come is not passed by.)
            const world::BehaviourState& in = behaviour->states[slot.state];
            if (!in.transitions.empty() && (slot.settled || !in.own)) {
                const PeopleTravel::Trip* on = own_trip();
                const bool trip_failed = on != nullptr && on->phase == PeopleTravel::Phase::failed;
                if (slot.settled && trip_failed && !slot.failed) { // (no way found: told once, though the state is left now)
                    slot.failed = true;
                    ++m_stats.failed;
                }
                const bool could_not = slot.settled && (slot.failed || trip_failed);
                const bool there = slot.settled && !could_not && on == nullptr;
                const float alarm = senses != nullptr ? senses->alarm_at(scene, m_cursor - 1) : 0.0f;
                const float hour = static_cast<float>(hour_of(person.id, hours)); // (compared as stored, as a routine's)
                bool asked = false, alarmed = false; // (what alarmed it: asked once, when a condition wants it)
                std::string_view kind;
                for (const world::BehaviourTransition& t : in.transitions) {
                    bool all = true;
                    for (const world::BehaviourCondition& c : t.conditions) {
                        bool holds = false;
                        switch (c.when) {
                        case world::BehaviourWhen::alarm_over: holds = alarm >= c.value; break;
                        case world::BehaviourWhen::alarm_under: holds = alarm < c.value; break;
                        case world::BehaviourWhen::sensed:
                            if (!asked) {
                                asked = true;
                                PeopleSenses::Heard heard;
                                alarmed = senses != nullptr && alarm > 0.0f && senses->heard(scene, person.id, heard);
                                if (alarmed) kind = PeopleSenses::to_string(heard.kind);
                            }
                            holds = alarmed && kind == c.kind;
                            break;
                        case world::BehaviourWhen::arrived: holds = there; break;
                        case world::BehaviourWhen::failed: holds = could_not; break;
                        case world::BehaviourWhen::after: holds = seconds - slot.entered >= static_cast<double>(c.value); break;
                        case world::BehaviourWhen::hour: // (from over until: a range over midnight)
                            holds = c.value < c.until ? hour >= c.value && hour < c.until : hour >= c.value || hour < c.until;
                            break;
                        }
                        if (!holds) {
                            all = false;
                            break;
                        }
                    }
                    if (!all) continue;
                    if (const std::optional<usize> to = world::behaviour_state_named(*behaviour, t.to)) {
                        const u8 from = slot.state;
                        slot.state = static_cast<u8>(*to);
                        changed(*behaviour, slot, from, seconds);
                        slot.entered = seconds;
                        slot.settled = false;
                        ++m_stats.changed;
                    }
                    break;
                }
            }
        }
        // What it does now: its state's own, else its routine's.
        const world::BehaviourState* state = behaviour != nullptr ? &behaviour->states[slot.state] : nullptr;
        const bool by_state = state != nullptr && state->own;
        if (!by_state && (routine_found == nullptr || routine_found->entries.empty())) {
            if (behaviour == nullptr && !slot.by_state) continue; // (a routine with no entries: as ever)
            if (slot.acted != 0 || slot.trip != 0 || slot.by_state) nothing();
            slot.failed = false; // (nothing was tried: a routine not there before is not this state's failing)
            slot.settled = true;
            continue;
        }
        const world::Routine& routine = by_state ? k_no_routine : *routine_found;
        usize now = by_state ? slot.state : world::routine_entry_at(routine, hour_of(person.id, hours));
        if (!by_state && senses != nullptr && !routine.alarmed.empty()) {
            // Alarmed: the last of `alarmed` its alarm is at or over holds instead.
            // What it does for its alarm already it leaves only when calmer by a margin (an alarm
            // hovering about an `over` does not have it start and stop).
            const float alarm = senses->alarm_at(scene, m_cursor - 1);
            const usize days = routine.entries.size();
            const usize doing = slot.acted != 0 && !slot.by_state && slot.entry >= days ? slot.entry - days : routine.alarmed.size();
            for (usize a = routine.alarmed.size(); a-- > 0 && alarm > 0.0f;) {
                if (alarm >= routine.alarmed[a].over || (a == doing && alarm >= routine.alarmed[a].over - k_alarm_margin)) {
                    now = days + a;
                    break;
                }
            }
        }
        const world::RoutineEntry& entry = by_state ? state->does : *entry_of(routine, now);
        // An entry for its alarm, or a state's own: one that cannot be done is not walked on past.
        const bool pressing = by_state || now >= routine.entries.size();
        const u32 says = said(person, entry);
        // (A state's own just entered is acted on though it says what was last acted on: what that
        // began may be over, or have failed.)
        const bool was_state = slot.by_state;
        const bool again = !slot.settled && (by_state || was_state);
        if (says == slot.acted && !again) {
            slot.entry = static_cast<u8>(now);
            slot.by_state = by_state; // (the same said by another entry: what it does there may be another)
            slot.settled = true;
            // The trip it began for this: over, another's since, or failed (no way found: told once).
            if (slot.trip != 0) {
                const PeopleTravel::Trip* trip = travel.trip(person.id);
                if (trip == nullptr || trip->serial != slot.trip) {
                    slot.trip = 0;
                } else if (trip->phase == PeopleTravel::Phase::failed && !slot.failed) {
                    slot.failed = true;
                    ++m_stats.failed;
                }
            }
            continue;
        }
        const auto wait = [&] { // (its turn comes again next frame)
            --m_cursor;
            --m_counted;
            if (behaviour != nullptr) --m_behaving_counted;
        };
        if (acted >= most) {
            wait();
            break;
        }
        const PeopleTravel::Trip* trip = travel.trip(person.id);
        if (entry.at == world::RoutineAt::stay) {
            ++acted;
            slot.acted = says;
            slot.entry = static_cast<u8>(now);
            slot.by_state = by_state;
            slot.failed = false;
            slot.place = 0;
            slot.settled = true;
            // (Only a trip the routine began: one sent by hand goes on.)
            if (slot.trip != 0 && trip != nullptr && trip->serial == slot.trip) travel.stop(person.id);
            slot.trip = 0;
            ++m_stats.stayed;
            continue;
        }
        Goal to;
        const Sought sought = seek(scene, person, entry, to, true);
        if (sought == Sought::later) {
            wait();
            break;
        }
        ++acted;
        slot.acted = says;
        slot.entry = static_cast<u8>(now);
        slot.by_state = by_state;
        slot.failed = false;
        slot.place = 0;
        slot.settled = true;
        if (sought == Sought::none && pressing && !by_state) {
            // Alarmed with nowhere to go (no home, no such place): it stays where it is, as for `stay`.
            if (slot.trip != 0 && trip != nullptr && trip->serial == slot.trip) travel.stop(person.id);
            slot.trip = 0;
            ++m_stats.stayed;
            continue;
        }
        if (sought == Sought::none) { // (no home, or no such place: it stays where it is, or goes on where it went)
            slot.failed = true;
            ++m_stats.failed;
            if (by_state || was_state) { // (a state's own that cannot be done, or what follows one: it does not go on with what the last had it do)
                if (slot.trip != 0 && trip != nullptr && trip->serial == slot.trip) travel.stop(person.id);
                slot.trip = 0;
            }
            continue;
        }
        const world::Place& place = scene.places[to.place];
        slot.place = hash_of(place.name);
        // (As PeopleTravel keeps a speed: to a few places.)
        const double pace = std::round(static_cast<double>(entry.speed) * 1000.0) / 1000.0;
        if (trip == nullptr) {
            const DVec3 here = travel.where(person).position;
            const double dx = here.x - place.position.x, dz = here.z - place.position.z;
            const double radius = static_cast<double>(place.radius);
            if (dx * dx + dz * dz <= radius * radius) { // (there already)
                slot.trip = 0;
                ++m_stats.stayed;
                continue;
            }
        } else if (trip->phase != PeopleTravel::Phase::failed && trip->to == to.spot && trip->speed == pace) { // (on its way there already)
            slot.trip = trip->serial;
            ++m_stats.stayed;
            continue;
        }
        if (travel.send(scene, person.id, to.spot, entry.speed, within, place.name, true).empty()) {
            slot.trip = travel.trip(person.id)->serial;
            ++m_stats.sent;
        } else {
            slot.failed = true;
            ++m_stats.failed;
            // (Alarmed and not able to go where that says: it does not walk on with its day either.)
            if (pressing || was_state) {
                const PeopleTravel::Trip* still = travel.trip(person.id);
                if (slot.trip != 0 && still != nullptr && still->serial == slot.trip) travel.stop(person.id);
                slot.trip = 0;
            }
        }
    }
}

// The place a slot's person was sent to: found again by the name's hash (its place in the list may be another since).
const world::Place* PeopleRoutines::sent_to(const world::Scene& scene, const Slot& slot) const {
    if (slot.place == 0) return nullptr;
    for (auto it = std::lower_bound(m_by_name.begin(), m_by_name.end(), Key{slot.place, 0.0, 0});
         it != m_by_name.end() && it->hash == slot.place; ++it) {
        if (it->place < scene.places.size() && hash_of(scene.places[it->place].name) == slot.place) return &scene.places[it->place];
    }
    return nullptr;
}

bool PeopleRoutines::activity(const world::Scene& scene, world::PersonId id, Activity& out) const {
    if (!settings.on || !m_running) return false;
    const world::Person* person = world::find_person(scene.people, id);
    if (person == nullptr) return false;
    const usize index = static_cast<usize>(person - scene.people.data());
    if (index >= m_slots.size()) return false;
    const Slot& slot = m_slots[index];
    if (slot.id != id || slot.failed) return false;
    const world::RoutineEntry* entry = acted_on(scene, *person, slot);
    if (entry == nullptr) return false;
    out.activity = entry->activity;
    out.place = sent_to(scene, slot);
    out.anywhere = slot.place == 0;
    return true;
}

void PeopleRoutines::ready(const world::Scene& scene, const PeopleTravel& travel) {
    follow(scene, travel);
    bool same = m_slots.size() == scene.people.size();
    for (usize i = 0; same && i < m_slots.size(); ++i) same = m_slots[i].id == scene.people[i].id;
    if (!same) match_slots(scene.people);
}

bool PeopleRoutines::in_later_state(usize index) const {
    if (!m_running || index >= m_slots.size()) return false;
    const Slot& slot = m_slots[index];
    return slot.behaviour != k_no_behaviour && (slot.state != 0 || (slot.acted != 0 && slot.by_state));
}

bool PeopleRoutines::kept(const world::Scene& scene, const PeopleTravel& travel, usize index, Kept& out) const {
    out = Kept{};
    if (!m_running || index >= m_slots.size() || index >= scene.people.size()) return false;
    const Slot& slot = m_slots[index];
    const world::Person& person = scene.people[index];
    if (slot.id != person.id) return false;
    // The state it is in, if its behaviour is still the one the slot found.
    const world::Behaviour* behaviour = slot.behaviour < scene.behaviours.size() ? &scene.behaviours[slot.behaviour] : nullptr;
    if (behaviour != nullptr && behaviour->name != person.behaviour) behaviour = nullptr;
    // (At another place in the list since it was last looked at, one before it removed: the same one still.)
    if (behaviour == nullptr && slot.behaviour != k_no_behaviour && !person.behaviour.empty() &&
        static_cast<u32>(hash_of(person.behaviour)) == slot.behaviour_name) {
        behaviour = world::find_behaviour(scene.behaviours, person.behaviour);
    }
    if (behaviour != nullptr && slot.state >= behaviour->states.size()) behaviour = nullptr;
    // (Told whatever else is: a trip that was the state's or the routine's is not written as one sent by hand.)
    const PeopleTravel::Trip* trip = travel.trip(slot.id);
    out.own_trip = slot.trip != 0 && trip != nullptr && trip->serial == slot.trip;
    if (behaviour != nullptr) {
        out.behaviour = behaviour->name;
        out.state = behaviour->states[slot.state].name;
        out.in_state = std::max(0.0, travel.seconds() - slot.entered);
        out.settled = slot.settled;
    }
    // (What a state's own had it do is kept only with the state.)
    if (slot.acted == 0 || (slot.by_state && (behaviour == nullptr || slot.entry != slot.state))) return behaviour != nullptr;
    // (One on an entry for its alarm is kept as any: a loaded game is calm, so the hour's entry is
    // then something else to act on, and the trip its alarm began is the routine's to stop or turn.
    // Whether the entry is still its routine's is restore's to tell.)
    out.said = slot.acted;
    out.entry = slot.entry;
    out.own = slot.by_state;
    out.failed = slot.failed;
    const world::Place* place = sent_to(scene, slot);
    out.place = place != nullptr ? place->name : std::string();
    return true;
}

bool PeopleRoutines::restore(const world::Scene& scene, const PeopleTravel& travel, world::PersonId id, const Kept& had,
                             bool state_only) {
    follow(scene, travel);
    const world::Person* person = world::find_person(scene.people, id);
    if (!m_running || person == nullptr) return false;
    // The state: its behaviour still has one of that name.
    const world::Behaviour* behaviour = nullptr;
    std::optional<usize> state;
    if (!had.behaviour.empty() && person->behaviour == had.behaviour) {
        behaviour = world::find_behaviour(scene.behaviours, person->behaviour);
        if (behaviour != nullptr) state = world::behaviour_state_named(*behaviour, had.state);
    }
    // What it had acted on: the state's own, or its routine's entry, if that says the same still.
    const world::Routine* routine = nullptr;
    bool acted = false;
    const u32 had_said = state_only ? 0 : had.said;
    if (had_said != 0 && had.own) {
        acted = state.has_value() && behaviour->states[*state].own && said(*person, behaviour->states[*state].does) == had.said;
    } else if (had_said != 0 && !person->routine.empty()) {
        routine = world::find_routine(scene.routines, person->routine);
        const world::RoutineEntry* entry = routine != nullptr ? entry_of(*routine, had.entry) : nullptr;
        // (Its routine changed since: it says something else now, and is acted on afresh.)
        acted = entry != nullptr && said(*person, *entry) == had.said;
    }
    if (!acted && !state.has_value()) return false;
    const usize index = static_cast<usize>(person - scene.people.data());
    if (m_slots.size() != scene.people.size() || m_slots[index].id != id) match_slots(scene.people);
    Slot& slot = m_slots[index];
    if (state.has_value()) { // (found again, not begun: nothing is told of it)
        slot.behaviour = static_cast<u16>(behaviour - scene.behaviours.data());
        slot.behaviour_name = static_cast<u32>(hash_of(behaviour->name));
        slot.state = static_cast<u8>(*state);
        slot.entered = travel.seconds() - std::max(0.0, had.in_state);
        // (What it had done is not put back: the state is acted on again, and until then it has
        // neither arrived nor failed. One that had done nothing its state did not say is as it was.)
        slot.settled = had.settled && !state_only && (acted || (had.said == 0 && !behaviour->states[*state].own));
    }
    if (!acted) return had_said == 0;
    slot.acted = had.said;
    slot.entry = had.own ? slot.state : had.entry;
    slot.by_state = had.own;
    if (routine != nullptr) slot.routine = static_cast<u16>(routine - scene.routines.data());
    slot.failed = had.failed;
    slot.place = had.place.empty() ? 0 : hash_of(had.place);
    const PeopleTravel::Trip* trip = travel.trip(id);
    slot.trip = had.own_trip && trip != nullptr ? trip->serial : 0;
    return true;
}

bool PeopleRoutines::doing(const world::Scene& scene, world::PersonId id, Doing& out) const {
    const world::Person* person = world::find_person(scene.people, id);
    if (person == nullptr) return false;
    const usize index = static_cast<usize>(person - scene.people.data());
    if (index >= m_slots.size()) return false;
    const Slot& slot = m_slots[index];
    if (slot.id != id) return false;
    const world::RoutineEntry* entry = acted_on(scene, *person, slot);
    if (entry == nullptr) return false;
    out.activity = entry->activity;
    out.own = slot.by_state;
    if (slot.by_state) {
        out.alarmed = false;
        out.at = 0.0f;
    } else {
        const world::Routine& routine = scene.routines[slot.routine];
        out.alarmed = slot.entry >= routine.entries.size();
        out.at = out.alarmed ? routine.alarmed[slot.entry - routine.entries.size()].over : entry->from;
    }
    const world::Place* place = sent_to(scene, slot);
    out.place = place != nullptr ? std::string_view(place->name) : std::string_view();
    out.failed = slot.failed;
    return true;
}

bool PeopleRoutines::behaving(const world::Scene& scene, const PeopleTravel& travel, world::PersonId id, Behaving& out) const {
    if (!settings.on || !m_running) return false;
    const world::Person* person = world::find_person(scene.people, id);
    if (person == nullptr) return false;
    const usize index = static_cast<usize>(person - scene.people.data());
    if (index >= m_slots.size()) return false;
    const Slot& slot = m_slots[index];
    if (slot.id != id || slot.behaviour >= scene.behaviours.size()) return false;
    const world::Behaviour& behaviour = scene.behaviours[slot.behaviour];
    if (behaviour.name != person->behaviour || slot.state >= behaviour.states.size()) return false;
    out.behaviour = behaviour.name;
    out.state = behaviour.states[slot.state].name;
    out.index = slot.state;
    out.seconds = std::max(0.0, travel.seconds() - slot.entered);
    return true;
}

} // namespace giant::editor
