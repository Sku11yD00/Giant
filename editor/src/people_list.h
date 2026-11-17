// The People panel's list, and what it and get_people say of a person
// (living-world plan LW6): the people that match a filter, found a share a
// frame (a scene has up to 65,536, and who is alarmed or travelling changes
// while the world runs, so the list is gone through again and again); what a
// person does and why, in words; and the camera following one.
//
// View state: not undone, not saved.
#pragma once

#include "editor_commands.h"

#include "giant/core/types.h"
#include "giant/world/scene.h"

#include <string>
#include <vector>

namespace giant::editor {

class PeopleList {
public:
    struct Filter {
        std::string text;        // in its name, its look, its home or its routine (letters' case aside), or its id; empty: any
        bool alarmed = false;    // only those alarmed now
        bool travelling = false; // only those on a trip now
        bool operator==(const Filter&) const = default;
    };
    Filter filter;

    static constexpr usize k_looked_a_frame = 4096;
    static constexpr usize k_most_shown = 1024; // the first so many that match are kept (their ids rising)

    // Whether a person matches (a command's too: get_people).
    static bool matches(const EditorState& state, const world::Person& person, usize index, const Filter& filter);

    // Each frame the panel is open: a share of the people looked at. The
    // list shown is the last whole time through; another filter or scene
    // begins again at once (the list empty until the first time through).
    // People removed or put back meanwhile neither hide nor double anyone:
    // it goes on from the id it last looked at.
    void update(const EditorState& state);
    // Begins again, the list empty (the panel was not open for a while:
    // half a time through from then is not put with half from now).
    void restart();
    const std::vector<world::PersonId>& shown() const { return m_shown; }
    usize matching() const { return m_matching; } // in the last whole time through (more than are shown, maybe)
    bool whole() const { return m_whole; }        // a whole time through has been made with this filter

private:
    Filter m_filter;
    u64 m_scene = 0;
    world::PersonId m_last = 0; // the id last looked at this time through (0: none yet)
    usize m_count = 0;
    bool m_whole = false;
    usize m_matching = 0;
    std::vector<world::PersonId> m_shown, m_next;
};

// What a person does now and why, in a sentence or two: what its routine
// has it do (the hour's entry, or the one for its alarm), what alarmed it,
// and where its trip has got to. For people to read: the words may change.
std::string person_why(const EditorState& state, const world::Person& person);

// The camera kept `EditorState::follow.distance` behind the person
// followed, looking the way the camera is turned (so turning it looks
// round the person), never under the ground. Each frame, playing or not,
// before the scene is drawn. False: nobody is followed (the one followed
// is gone, another scene was taken up, or playing took the camera:
// following ends, and does not come back when play stops).
bool follow_person_camera(EditorState& state);

} // namespace giant::editor
