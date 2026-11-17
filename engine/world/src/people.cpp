#include "giant/world/people.h"

#include "giant/core/assert.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace giant::world {

namespace {

bool finite_place(const DVec3& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) && std::fabs(p.x) <= 1e8 &&
           std::fabs(p.y) <= 1e8 && std::fabs(p.z) <= 1e8;
}

bool yaw_ok(float yaw) { return std::isfinite(yaw) && yaw >= -360.0f && yaw <= 360.0f; }

} // namespace

bool place_tag_ok(std::string_view tag) {
    if (tag.empty() || tag.size() > k_max_tag_bytes) return false;
    return std::all_of(tag.begin(), tag.end(),
                       [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

std::optional<std::string> check_place(const Place& place) {
    if (place.name.empty() || place.name.size() > k_max_name_bytes) {
        return "name must be 1 to " + std::to_string(k_max_name_bytes) + " bytes";
    }
    if (!finite_place(place.position)) return "position must be finite and within 100,000 km";
    if (!std::isfinite(place.radius) || place.radius < 0.25f || place.radius > 1000.0f) {
        return "radius must be from 0.25 to 1000 metres";
    }
    if (!yaw_ok(place.yaw)) return "yaw must be from -360 to 360 degrees";
    if (place.tags.size() > k_max_place_tags) return "at most " + std::to_string(k_max_place_tags) + " tags";
    for (usize i = 0; i < place.tags.size(); ++i) {
        if (!place_tag_ok(place.tags[i])) {
            return "tags must be 1 to " + std::to_string(k_max_tag_bytes) + " of a-z, 0-9 and _";
        }
        if (std::find(place.tags.begin(), place.tags.begin() + static_cast<std::ptrdiff_t>(i), place.tags[i]) !=
            place.tags.begin() + static_cast<std::ptrdiff_t>(i)) {
            return "tags names \"" + place.tags[i] + "\" twice";
        }
    }
    return std::nullopt;
}

std::optional<std::string> check_person_look(const PersonLook& look) {
    if (look.name.empty() || look.name.size() > k_max_name_bytes) {
        return "name must be 1 to " + std::to_string(k_max_name_bytes) + " bytes";
    }
    if (look.mesh.empty() || look.mesh.size() > k_max_mesh_path_bytes) {
        return "mesh must be a file's path of 1 to " + std::to_string(k_max_mesh_path_bytes) + " bytes";
    }
    if (!std::isfinite(look.height) || look.height < 0.1f || look.height > 10.0f) return "height must be from 0.1 to 10";
    if (look.parts.size() > k_max_entity_parts) return "at most " + std::to_string(k_max_entity_parts) + " parts' looks";
    for (usize i = 0; i < look.parts.size(); ++i) {
        if (look.parts[i].name.size() > k_max_name_bytes) return "a part's name is too long";
        for (usize j = 0; j < i; ++j) {
            if (look.parts[j].name == look.parts[i].name) return "parts names \"" + look.parts[i].name + "\" twice";
        }
    }
    if (look.matched.size() > k_max_matched_clips) return "at most " + std::to_string(k_max_matched_clips) + " matched clips";
    for (usize i = 0; i < look.matched.size(); ++i) {
        const std::string& clip = look.matched[i];
        if (clip.empty() || clip.size() > k_max_name_bytes) return "matched must hold clip names of 1 to " + std::to_string(k_max_name_bytes) + " bytes";
        if (clip == "*" && look.matched.size() > 1) return "matched must hold \"*\" (every clip) alone";
        if (std::find(look.matched.begin(), look.matched.begin() + static_cast<std::ptrdiff_t>(i), clip) !=
            look.matched.begin() + static_cast<std::ptrdiff_t>(i)) {
            return "matched names " + clip + " twice";
        }
    }
    if (look.activities.size() > k_max_look_activities) {
        return "at most " + std::to_string(k_max_look_activities) + " activities";
    }
    for (usize i = 0; i < look.activities.size(); ++i) {
        const LookActivity& a = look.activities[i];
        const std::string which = "activities[" + std::to_string(i) + "]: ";
        if (!place_tag_ok(a.activity)) return which + "activity must be 1 to " + std::to_string(k_max_tag_bytes) + " of a-z, 0-9 and _";
        if (a.clip.empty() || a.clip.size() > k_max_name_bytes) {
            return which + "clip must be a clip's name of 1 to " + std::to_string(k_max_name_bytes) + " bytes";
        }
        if (!std::isfinite(a.speed) || a.speed < 0.01f || a.speed > k_max_animation_speed) {
            return which + "speed must be from 0.01 to " + std::to_string(static_cast<int>(k_max_animation_speed));
        }
        for (usize j = 0; j < i; ++j) {
            if (look.activities[j].activity == a.activity) return "activities names \"" + a.activity + "\" twice";
        }
    }
    return std::nullopt;
}

std::optional<std::string> check_person(const Person& person) {
    if (person.id == 0) return "id must not be 0";
    if (person.name.size() > k_max_name_bytes) return "name must be at most " + std::to_string(k_max_name_bytes) + " bytes";
    if (person.look.empty() || person.look.size() > k_max_name_bytes) return "look must name a person look";
    if (person.home.size() > k_max_name_bytes) return "home must name a place";
    if (person.routine.size() > k_max_name_bytes) return "routine must name a routine";
    if (person.behaviour.size() > k_max_name_bytes) return "behaviour must name a behaviour";
    if (!finite_place(person.position)) return "position must be finite and within 100,000 km";
    if (!yaw_ok(person.yaw)) return "yaw must be from -360 to 360 degrees";
    return std::nullopt;
}

namespace {

// What is wrong with what an entry has one do (a routine's, an alarmed one's, a behaviour's state's), or nothing.
std::optional<std::string> check_does(const RoutineEntry& e) {
    if (!place_tag_ok(e.activity)) return "activity must be 1 to " + std::to_string(k_max_tag_bytes) + " of a-z, 0-9 and _";
    if (!std::isfinite(e.speed) || e.speed < 0.1f || e.speed > 20.0f) return "speed must be from 0.1 to 20 m/s";
    if (static_cast<u8>(e.at) > static_cast<u8>(RoutineAt::tag)) return "at must be stay, home, place or tag";
    switch (e.at) {
    case RoutineAt::stay:
    case RoutineAt::home:
        if (!e.name.empty()) return "name is given only with at place or tag";
        break;
    case RoutineAt::place:
        if (e.name.empty() || e.name.size() > k_max_name_bytes) return "name must be a place's name";
        break;
    case RoutineAt::tag:
        if (!place_tag_ok(e.name)) return "name must be a place's tag";
        break;
    }
    return std::nullopt;
}

} // namespace

std::optional<std::string> check_routine(const Routine& routine) {
    if (routine.name.empty() || routine.name.size() > k_max_name_bytes) {
        return "name must be 1 to " + std::to_string(k_max_name_bytes) + " bytes";
    }
    if (routine.entries.empty() || routine.entries.size() > k_max_routine_entries) {
        return "entries must be 1 to " + std::to_string(k_max_routine_entries);
    }
    if (routine.alarmed.size() > k_max_routine_alarms) return "alarmed must be at most " + std::to_string(k_max_routine_alarms);
    const usize days = routine.entries.size();
    for (usize i = 0; i < days + routine.alarmed.size(); ++i) {
        const bool alarm = i >= days;
        const RoutineEntry& e = alarm ? routine.alarmed[i - days].does : routine.entries[i];
        const std::string which = (alarm ? "alarmed[" + std::to_string(i - days) : "entries[" + std::to_string(i)) + "]: ";
        if (alarm) {
            const float over = routine.alarmed[i - days].over;
            if (!std::isfinite(over) || over <= 0.0f || over > 1.0f) return which + "over must be an alarm over 0, to 1";
            if (i > days && over <= routine.alarmed[i - days - 1].over) return which + "over must be more than the one's before it";
            if (e.from != 0.0f) return which + "from is not used (an alarmed entry has over)";
        } else {
            if (!std::isfinite(e.from) || e.from < 0.0f || e.from >= 24.0f) return which + "from must be an hour from 0 to under 24";
            if (i > 0 && e.from <= routine.entries[i - 1].from) return which + "from must be later than the entry's before it";
        }
        if (const auto bad = check_does(e)) return which + *bad;
    }
    return std::nullopt;
}

std::optional<std::string> check_behaviour(const Behaviour& behaviour) {
    if (behaviour.name.empty() || behaviour.name.size() > k_max_name_bytes) {
        return "name must be 1 to " + std::to_string(k_max_name_bytes) + " bytes";
    }
    if (behaviour.states.empty() || behaviour.states.size() > k_max_behaviour_states) {
        return "states must be 1 to " + std::to_string(k_max_behaviour_states);
    }
    for (usize i = 0; i < behaviour.states.size(); ++i) {
        const BehaviourState& s = behaviour.states[i];
        const std::string which = "states[" + std::to_string(i) + "]: ";
        if (!place_tag_ok(s.name)) return which + "name must be 1 to " + std::to_string(k_max_tag_bytes) + " of a-z, 0-9 and _";
        for (usize k = 0; k < i; ++k) {
            if (behaviour.states[k].name == s.name) return which + "two states are named \"" + s.name + "\"";
        }
        if (s.own) {
            if (s.does.from != 0.0f) return which + "from is not used (a state has no hour)";
            if (const auto bad = check_does(s.does)) return which + *bad;
        } else if (!(s.does == RoutineEntry{})) {
            return which + "what it does is given only with an activity (without one the routine says)";
        }
        if (s.transitions.size() > k_max_behaviour_transitions) {
            return which + "transitions must be at most " + std::to_string(k_max_behaviour_transitions);
        }
        for (usize t = 0; t < s.transitions.size(); ++t) {
            const BehaviourTransition& tr = s.transitions[t];
            const std::string where = which + "transitions[" + std::to_string(t) + "]: ";
            if (tr.to == s.name) return where + "to must be another state";
            if (!behaviour_state_named(behaviour, tr.to)) return where + "to must be a state of the behaviour (\"" + tr.to + "\" is not)";
            if (tr.conditions.empty() || tr.conditions.size() > k_max_behaviour_conditions) {
                return where + "when must be 1 to " + std::to_string(k_max_behaviour_conditions) + " conditions";
            }
            for (usize c = 0; c < tr.conditions.size(); ++c) {
                const BehaviourCondition& w = tr.conditions[c];
                const std::string cond = where + "when[" + std::to_string(c) + "]: ";
                if (static_cast<u8>(w.when) > static_cast<u8>(BehaviourWhen::hour)) return cond + "is not a condition";
                const bool alarm = w.when == BehaviourWhen::alarm_over || w.when == BehaviourWhen::alarm_under;
                if (alarm && !(std::isfinite(w.value) && w.value > 0.0f && w.value <= 1.0f)) return cond + "an alarm must be over 0, to 1";
                if (w.when == BehaviourWhen::after && !(std::isfinite(w.value) && w.value > 0.0f && w.value <= 86400.0f)) {
                    return cond + "after must be seconds over 0, to 86400";
                }
                if (w.when == BehaviourWhen::hour) {
                    const auto hour_ok = [](float h) { return std::isfinite(h) && h >= 0.0f && h < 24.0f; };
                    if (!hour_ok(w.value) || !hour_ok(w.until)) return cond + "hour must be from an hour to an hour, each 0 to under 24";
                    if (w.value == w.until) return cond + "hour must be from one hour to another";
                } else if (w.until != 0.0f) {
                    return cond + "until is given only with hour";
                }
                if (!alarm && w.when != BehaviourWhen::after && w.when != BehaviourWhen::hour && w.value != 0.0f) {
                    return cond + "a value is given only with alarm_over, alarm_under, after and hour";
                }
                if (w.when == BehaviourWhen::sensed) {
                    if (!place_tag_ok(w.kind)) return cond + "sensed must be a kind: 1 to " + std::to_string(k_max_tag_bytes) + " of a-z, 0-9 and _";
                } else if (!w.kind.empty()) {
                    return cond + "a kind is given only with sensed";
                }
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> check_behaviours(std::span<const Behaviour> behaviours, std::span<const Person> people) {
    if (behaviours.size() > k_max_behaviours) return "at most " + std::to_string(k_max_behaviours) + " behaviours";
    // Sorted, not hashed: n log n whatever the names (a file may be made to collide).
    std::vector<std::string_view> names;
    names.reserve(behaviours.size());
    for (const Behaviour& b : behaviours) names.push_back(b.name);
    std::sort(names.begin(), names.end());
    if (const auto twice = std::adjacent_find(names.begin(), names.end()); twice != names.end()) {
        return "two behaviours are named \"" + std::string(*twice) + "\"";
    }
    for (const Person& p : people) {
        if (!p.behaviour.empty() && !std::binary_search(names.begin(), names.end(), std::string_view(p.behaviour))) {
            return "person " + std::to_string(p.id) + "'s behaviour \"" + p.behaviour + "\" is not there";
        }
    }
    return std::nullopt;
}

const Behaviour* find_behaviour(std::span<const Behaviour> behaviours, std::string_view name) {
    const auto it = std::find_if(behaviours.begin(), behaviours.end(), [&](const Behaviour& b) { return b.name == name; });
    return it == behaviours.end() ? nullptr : &*it;
}

std::optional<usize> behaviour_state_named(const Behaviour& behaviour, std::string_view name) {
    for (usize i = 0; i < behaviour.states.size(); ++i) {
        if (behaviour.states[i].name == name) return i;
    }
    return std::nullopt;
}

const char* behaviour_when_name(BehaviourWhen when) {
    switch (when) {
    case BehaviourWhen::alarm_over: return "alarm_over";
    case BehaviourWhen::alarm_under: return "alarm_under";
    case BehaviourWhen::sensed: return "sensed";
    case BehaviourWhen::arrived: return "arrived";
    case BehaviourWhen::failed: return "failed";
    case BehaviourWhen::after: return "after";
    case BehaviourWhen::hour: return "hour";
    }
    return "arrived";
}

std::optional<BehaviourWhen> behaviour_when_named(std::string_view name) {
    for (const BehaviourWhen when : {BehaviourWhen::alarm_over, BehaviourWhen::alarm_under, BehaviourWhen::sensed, BehaviourWhen::arrived,
                                     BehaviourWhen::failed, BehaviourWhen::after, BehaviourWhen::hour}) {
        if (name == behaviour_when_name(when)) return when;
    }
    return std::nullopt;
}

std::optional<std::string> check_people(std::span<const Place> places, std::span<const PersonLook> looks,
                                        std::span<const Routine> routines, std::span<const Person> people,
                                        PersonId next_person_id) {
    if (routines.size() > k_max_routines) return "at most " + std::to_string(k_max_routines) + " routines";
    std::vector<std::string_view> routine_names;
    routine_names.reserve(routines.size());
    for (const Routine& r : routines) routine_names.push_back(r.name);
    std::sort(routine_names.begin(), routine_names.end());
    if (const auto twice = std::adjacent_find(routine_names.begin(), routine_names.end()); twice != routine_names.end()) {
        return "two routines are named \"" + std::string(*twice) + "\"";
    }
    if (places.size() > k_max_places) return "at most " + std::to_string(k_max_places) + " places";
    if (looks.size() > k_max_person_looks) return "at most " + std::to_string(k_max_person_looks) + " person looks";
    if (people.size() > k_max_people) return "at most " + std::to_string(k_max_people) + " people";
    // Sorted, not hashed: n log n whatever the names (a file may be made to collide).
    std::vector<std::string_view> place_names;
    place_names.reserve(places.size());
    for (const Place& p : places) place_names.push_back(p.name);
    std::sort(place_names.begin(), place_names.end());
    if (const auto twice = std::adjacent_find(place_names.begin(), place_names.end()); twice != place_names.end()) {
        return "two places are named \"" + std::string(*twice) + "\"";
    }
    std::vector<std::string_view> look_names;
    look_names.reserve(looks.size());
    for (const PersonLook& l : looks) look_names.push_back(l.name);
    std::sort(look_names.begin(), look_names.end());
    if (const auto twice = std::adjacent_find(look_names.begin(), look_names.end()); twice != look_names.end()) {
        return "two person looks are named \"" + std::string(*twice) + "\"";
    }
    PersonId last = 0;
    for (const Person& p : people) {
        if (p.id <= last) return "people must be in the order of their ids, each once (person " + std::to_string(p.id) + ")";
        last = p.id;
        if (!std::binary_search(look_names.begin(), look_names.end(), std::string_view(p.look))) return "person " + std::to_string(p.id) + "'s look \"" + p.look + "\" is not there";
        if (!p.home.empty() && !std::binary_search(place_names.begin(), place_names.end(), std::string_view(p.home))) {
            return "person " + std::to_string(p.id) + "'s home \"" + p.home + "\" is not there";
        }
        if (!p.routine.empty() && !std::binary_search(routine_names.begin(), routine_names.end(), std::string_view(p.routine))) {
            return "person " + std::to_string(p.id) + "'s routine \"" + p.routine + "\" is not there";
        }
    }
    if (next_person_id == 0 || last >= next_person_id) return "next_person_id must be past every person's id";
    return std::nullopt;
}

const Place* find_place(std::span<const Place> places, std::string_view name) {
    const auto it = std::find_if(places.begin(), places.end(), [&](const Place& p) { return p.name == name; });
    return it == places.end() ? nullptr : &*it;
}

const PersonLook* find_person_look(std::span<const PersonLook> looks, std::string_view name) {
    const auto it = std::find_if(looks.begin(), looks.end(), [&](const PersonLook& l) { return l.name == name; });
    return it == looks.end() ? nullptr : &*it;
}

const Person* find_person(std::span<const Person> people, PersonId id) {
    const auto it = std::lower_bound(people.begin(), people.end(), id, [](const Person& p, PersonId v) { return p.id < v; });
    return it == people.end() || it->id != id ? nullptr : &*it;
}

const char* routine_at_name(RoutineAt at) {
    switch (at) {
    case RoutineAt::stay: return "stay";
    case RoutineAt::home: return "home";
    case RoutineAt::place: return "place";
    case RoutineAt::tag: return "tag";
    }
    return "stay";
}

std::optional<RoutineAt> routine_at_named(std::string_view name) {
    for (const RoutineAt at : {RoutineAt::stay, RoutineAt::home, RoutineAt::place, RoutineAt::tag}) {
        if (name == routine_at_name(at)) return at;
    }
    return std::nullopt;
}

const Routine* find_routine(std::span<const Routine> routines, std::string_view name) {
    const auto it = std::find_if(routines.begin(), routines.end(), [&](const Routine& r) { return r.name == name; });
    return it == routines.end() ? nullptr : &*it;
}

usize routine_entry_at(const Routine& routine, double hour) {
    const usize count = routine.entries.size();
    if (count == 0) return 0;
    double h = std::isfinite(hour) ? std::fmod(hour, 24.0) : 0.0;
    if (h < 0.0) h += 24.0;
    // Compared as stored (float): an entry from 0.1 holds at hour 0.1.
    const float now = static_cast<float>(h);
    usize at = count - 1; // (before the first begins: the last, held over midnight)
    for (usize i = 0; i < count && routine.entries[i].from <= now; ++i) at = i;
    return at;
}

float routine_entry_ends(const Routine& routine, usize entry) {
    const usize count = routine.entries.size();
    if (count == 0) return 0.0f;
    GIANT_ASSERT(entry < count, "not one of the routine's entries");
    return routine.entries[(entry + 1) % count].from;
}

} // namespace giant::world
