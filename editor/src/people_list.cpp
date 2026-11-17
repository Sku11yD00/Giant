#include "people_list.h"

#include "people_routines.h"
#include "people_senses.h"
#include "people_travel.h"
#include "play_state.h"

#include "giant/core/math.h"
#include "giant/world/people.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

namespace giant::editor {

namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// `what` is in `text`, letters' case aside (ASCII: names are bytes).
bool holds(std::string_view text, std::string_view what) {
    if (what.empty()) return true;
    if (what.size() > text.size()) return false;
    for (usize at = 0; at + what.size() <= text.size(); ++at) {
        usize same = 0;
        while (same < what.size() && lower(text[at + same]) == lower(what[same])) ++same;
        if (same == what.size()) return true;
    }
    return false;
}

std::string number(double value, const char* format = "%.2f") {
    char text[64];
    std::snprintf(text, sizeof(text), format, value);
    return text;
}

} // namespace

bool PeopleList::matches(const EditorState& state, const world::Person& person, usize index, const Filter& filter) {
    if (filter.alarmed && (state.senses == nullptr || state.senses->alarm_at(state.scene, index) <= 0.0f)) return false;
    if (filter.travelling) {
        const PeopleTravel::Trip* trip = state.travel != nullptr ? state.travel->trip(person.id) : nullptr;
        if (trip == nullptr || trip->phase == PeopleTravel::Phase::failed) return false;
    }
    if (filter.text.empty()) return true;
    if (holds(person.name, filter.text) || holds(person.look, filter.text) || holds(person.home, filter.text) ||
        holds(person.routine, filter.text)) {
        return true;
    }
    char id[24];
    std::snprintf(id, sizeof(id), "%llu", static_cast<unsigned long long>(person.id));
    return filter.text == id;
}

void PeopleList::update(const EditorState& state) {
    const std::vector<world::Person>& people = state.scene.people;
    if (!(filter == m_filter) || state.scene.identity() != m_scene) {
        m_filter = filter;
        m_scene = state.scene.identity();
        m_cursor = 0;
        m_count = 0;
        m_whole = false;
        m_matching = 0;
        m_shown.clear();
        m_next.clear();
    }
    if (m_next.capacity() < k_most_shown) {
        m_next.reserve(k_most_shown);
        m_shown.reserve(k_most_shown);
    }
    if (m_cursor > people.size()) m_cursor = people.size(); // (people removed since)
    usize left = k_looked_a_frame;
    while (left > 0 && m_cursor < people.size()) {
        if (matches(state, people[m_cursor], m_cursor, m_filter)) {
            if (m_next.size() < k_most_shown) m_next.push_back(people[m_cursor].id);
            ++m_count;
        }
        ++m_cursor;
        --left;
    }
    if (m_cursor >= people.size()) { // through: what was found is what is shown, and it begins again
        m_shown.swap(m_next);
        m_next.clear();
        m_matching = m_count;
        m_count = 0;
        m_cursor = 0;
        m_whole = true;
    }
}

std::string person_why(const EditorState& state, const world::Person& person) {
    std::string out;
    const usize index = static_cast<usize>(&person - state.scene.people.data());
    PeopleSenses::Heard heard;
    const bool alarmed = state.senses != nullptr && index < state.scene.people.size() && state.senses->heard(state.scene, person.id, heard);
    PeopleRoutines::Doing doing;
    const bool does = state.routines != nullptr && state.routines->doing(state.scene, person.id, doing);
    if (alarmed) {
        out += "Alarmed " + number(static_cast<double>(heard.alarm));
        if (heard.kind == PeopleSenses::Kind::sight) {
            out += " by what it saw " + number(heard.seconds, "%.0f") + " s ago. ";
        } else {
            out += std::string(" by a noise (") + PeopleSenses::to_string(heard.kind) + ") " + number(heard.seconds, "%.0f") + " s ago. ";
        }
    }
    if (person.routine.empty()) {
        out += "It has no routine. ";
    } else if (!does) {
        out += "Its routine \"" + person.routine + "\" has not had it do anything yet (the world is not running, or its turn has not come). ";
    } else {
        out += "Its routine \"" + person.routine + "\" says " + std::string(doing.activity);
        if (doing.alarmed) {
            out += " while its alarm is over " + number(static_cast<double>(doing.at)) + ". ";
        } else {
            const int minutes = static_cast<int>(std::lround(static_cast<double>(doing.at) * 60.0));
            char hour[16];
            std::snprintf(hour, sizeof(hour), "%02d:%02d", (minutes / 60) % 24, minutes % 60);
            out += std::string(" from ") + hour + ". ";
        }
        if (doing.failed) out += "It could not be sent there (no such place, no road near, or no way). ";
    }
    const PeopleTravel::Trip* trip = state.travel != nullptr ? state.travel->trip(person.id) : nullptr;
    if (trip != nullptr) {
        const std::string where = trip->place.empty() ? std::string("a point") : "\"" + trip->place + "\"";
        switch (trip->phase) {
        case PeopleTravel::Phase::searching: out += "Looking for its way to " + where + "."; break;
        case PeopleTravel::Phase::travelling:
            out += "On its way to " + where + ": " + number(state.travel->gone(*trip), "%.0f") + " of " +
                   number(PeopleTravel::length(*trip), "%.0f") + " m.";
            break;
        case PeopleTravel::Phase::failed: out += "Its trip to " + where + " failed: " + trip->reason + "."; break;
        }
    } else if (does && !doing.place.empty()) {
        out += "It is at \"" + std::string(doing.place) + "\".";
    } else {
        out += "It stands where it is.";
    }
    return out;
}

bool follow_person_camera(EditorState& state) {
    if (state.follow.person == 0) return false;
    const world::Person* person = world::find_person(state.scene.people, state.follow.person);
    if (person == nullptr || state.follow.scene != state.scene.identity() || (state.play != nullptr && state.play->playing())) {
        state.follow.person = 0;
        return false;
    }
    const PeopleTravel::Where here = state.travel != nullptr ? state.travel->where(*person) : PeopleTravel::Where{person->position, person->yaw};
    const std::optional<double> ground = state.scene.terrain.height_at(here.position.x, here.position.z);
    const double yaw = static_cast<double>(degrees_to_radians(state.camera.yaw_degrees));
    const double pitch = static_cast<double>(degrees_to_radians(state.camera.pitch_degrees));
    // (The way the camera looks: yaw 0 along -z, 90 along -x; up is positive.)
    const double fx = -std::sin(yaw) * std::cos(pitch), fy = std::sin(pitch), fz = -std::cos(yaw) * std::cos(pitch);
    const double back = std::clamp(static_cast<double>(state.follow.distance), 1.0, 200.0);
    state.camera.position = {here.position.x - fx * back, (ground ? *ground : here.position.y) + 1.5 - fy * back,
                             here.position.z - fz * back};
    return true;
}

} // namespace giant::editor
