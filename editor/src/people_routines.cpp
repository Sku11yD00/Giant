#include "people_routines.h"

#include "people_travel.h"

#include "giant/world/people.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

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
    h = (h ^ static_cast<u32>(std::lround(static_cast<double>(e.speed) * 1000.0))) * 16777619u;
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
                     static_cast<u64>(reinterpret_cast<std::uintptr_t>(this)))) {}

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

void PeopleRoutines::update(const world::Scene& scene, PeopleTravel& travel, double hours) {
    bool afresh = travel.resets() != m_resets || travel.running() != m_running;
    if (scene.identity() != m_scene) { // (another scene: its people and places are others)
        m_scene = scene.identity();
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
        m_miscounted = false;
        m_builds = travel.builds();
        m_stats = Stats{};
    }
    const std::vector<world::Person>& people = scene.people;
    if (!settings.on || !m_running || people.empty() || scene.routines.empty()) {
        if (scene.routines.empty() || people.empty()) m_stats.following = 0;
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
    const usize looked = std::min<usize>(std::clamp<u32>(settings.looked_a_frame, 1, k_most_looked), people.size());
    const u32 most = std::clamp<u32>(settings.sent_a_frame, 1, k_most_sent);
    const double within = std::clamp(static_cast<double>(settings.within), 0.0, k_most_within);
    u32 acted = 0;
    for (usize n = 0; n < looked; ++n) {
        if (m_cursor >= people.size()) {
            m_cursor = 0;
            if (!m_miscounted) m_stats.following = m_counted;
            m_miscounted = false;
            m_counted = 0;
        }
        const world::Person& person = people[m_cursor];
        if (m_slots[m_cursor].id != person.id) { // (as many people as before, but others: made and removed)
            match_slots(people);
            continue;
        }
        Slot& slot = m_slots[m_cursor++];
        if (person.routine.empty()) {
            slot.acted = 0;
            slot.trip = 0;
            slot.failed = false;
            continue;
        }
        // Its routine: where it was last found, else looked for.
        if (slot.routine >= routines.size() || routines[slot.routine].name != person.routine) {
            const world::Routine* found = world::find_routine(routines, person.routine);
            if (found == nullptr) {
                slot.acted = 0;
                continue;
            }
            slot.routine = static_cast<u16>(found - routines.data());
        }
        ++m_counted;
        const world::Routine& routine = routines[slot.routine];
        if (routine.entries.empty()) continue;
        const usize now = world::routine_entry_at(routine, hour_of(person.id, hours));
        const world::RoutineEntry& entry = routine.entries[now];
        slot.entry = static_cast<u8>(now);
        const u32 says = said(person, entry);
        if (says == slot.acted) {
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
        };
        if (acted >= most) {
            wait();
            break;
        }
        const PeopleTravel::Trip* trip = travel.trip(person.id);
        if (entry.at == world::RoutineAt::stay) {
            ++acted;
            slot.acted = says;
            slot.failed = false;
            slot.place = 0;
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
        slot.failed = false;
        slot.place = 0;
        if (sought == Sought::none) { // (no home, or no such place: it stays where it is, or goes on where it went)
            slot.failed = true;
            ++m_stats.failed;
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
        }
    }
}

bool PeopleRoutines::doing(const world::Scene& scene, world::PersonId id, Doing& out) const {
    const world::Person* person = world::find_person(scene.people, id);
    if (person == nullptr) return false;
    const usize index = static_cast<usize>(person - scene.people.data());
    if (index >= m_slots.size()) return false;
    const Slot& slot = m_slots[index];
    if (slot.id != id || slot.acted == 0 || slot.routine >= scene.routines.size()) return false;
    const world::Routine& routine = scene.routines[slot.routine];
    if (routine.name != person->routine || slot.entry >= routine.entries.size()) return false;
    out.activity = routine.entries[slot.entry].activity;
    // Where it was sent: found again by the name's hash (its place in the list may be another since).
    out.place = {};
    for (auto it = std::lower_bound(m_by_name.begin(), m_by_name.end(), Key{slot.place, 0.0, 0});
         slot.place != 0 && it != m_by_name.end() && it->hash == slot.place; ++it) {
        if (it->place < scene.places.size() && hash_of(scene.places[it->place].name) == slot.place) {
            out.place = scene.places[it->place].name;
            break;
        }
    }
    out.failed = slot.failed;
    return true;
}

} // namespace giant::editor
