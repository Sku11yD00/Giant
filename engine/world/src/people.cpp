#include "giant/world/people.h"

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
    return std::nullopt;
}

std::optional<std::string> check_person(const Person& person) {
    if (person.id == 0) return "id must not be 0";
    if (person.name.size() > k_max_name_bytes) return "name must be at most " + std::to_string(k_max_name_bytes) + " bytes";
    if (person.look.empty() || person.look.size() > k_max_name_bytes) return "look must name a person look";
    if (person.home.size() > k_max_name_bytes) return "home must name a place";
    if (!finite_place(person.position)) return "position must be finite and within 100,000 km";
    if (!yaw_ok(person.yaw)) return "yaw must be from -360 to 360 degrees";
    return std::nullopt;
}

std::optional<std::string> check_people(std::span<const Place> places, std::span<const PersonLook> looks,
                                        std::span<const Person> people, PersonId next_person_id) {
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

} // namespace giant::world
