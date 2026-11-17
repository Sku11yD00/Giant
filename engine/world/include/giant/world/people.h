// The people who live in the world (world::Place, PersonLook and Person in
// scene.h; living-world plan LW1): checking them, one at a time and
// together, and finding them by name or id. The limits are the ones the
// editor's commands and the scene's loading keep.
#pragma once

#include "giant/core/types.h"
#include "giant/world/scene.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace giant::world {

// What is wrong with one (its own fields alone), or nothing.
std::optional<std::string> check_place(const Place& place);
std::optional<std::string> check_person_look(const PersonLook& look);
std::optional<std::string> check_person(const Person& person);
// (A routine's named places need not be there: one whose place is gone stays where it is.)
std::optional<std::string> check_routine(const Routine& routine);
// A RoutineAt's word in files and commands (stay, home, place, tag), and back (none: not one).
const char* routine_at_name(RoutineAt at);
std::optional<RoutineAt> routine_at_named(std::string_view name);

// What is wrong among them all, or nothing: a place's, a look's or a
// routine's name had twice, a person's look, home or routine that is not there, an id had twice or
// not under `next_person_id`, more than the scene keeps. (Each one's own
// fields are check_place's, check_person_look's, check_routine's and
// check_person's: not looked at here.)
std::optional<std::string> check_people(std::span<const Place> places, std::span<const PersonLook> looks,
                                        std::span<const Routine> routines, std::span<const Person> people,
                                        PersonId next_person_id);

const Place* find_place(std::span<const Place> places, std::string_view name);
const PersonLook* find_person_look(std::span<const PersonLook> looks, std::string_view name);
// (People are kept in the order made, ids rising: found by halving.)
const Person* find_person(std::span<const Person> people, PersonId id);
const Routine* find_routine(std::span<const Routine> routines, std::string_view name);

// What is wrong with a behaviour (its own fields alone), or nothing: its
// name, its states' names (each once), what each does, and its
// transitions (each to another state of it, with conditions that can be).
std::optional<std::string> check_behaviour(const Behaviour& behaviour);
// What is wrong among the behaviours and the people who have them, or
// nothing: more than a scene keeps, a name had twice, a person's
// behaviour that is not there. (Each one's own fields are
// check_behaviour's: not looked at here.)
std::optional<std::string> check_behaviours(std::span<const Behaviour> behaviours, std::span<const Person> people);
const Behaviour* find_behaviour(std::span<const Behaviour> behaviours, std::string_view name);
// A state of a behaviour by name (its place in `states`), or none.
std::optional<usize> behaviour_state_named(const Behaviour& behaviour, std::string_view name);
// A BehaviourWhen's word in files and commands (alarm_over, alarm_under,
// sensed, arrived, failed, after, hour), and back (none: not one).
// Whether a word is a kind of thing sensed (a `sensed` condition's):
// shot, impact, blast, splash, voice, other, sight.
bool sensed_kind_ok(std::string_view kind);
const char* behaviour_when_name(BehaviourWhen when);
std::optional<BehaviourWhen> behaviour_when_named(std::string_view name);

// The entry of a routine (one that check_routine passes) that holds at
// `hour` (any finite hour: taken round the day): the last that has begun,
// or before the first begins, the last of all (held over midnight).
usize routine_entry_at(const Routine& routine, double hour);
// The hour (0 to under 24) the entry after `entry` (one of the routine's)
// begins: when that one ends. A routine's only entry never ends: this is
// its own `from` then.
float routine_entry_ends(const Routine& routine, usize entry);

// Whether `tag` may be a place's tag: 1 to k_max_tag_bytes of a-z, 0-9 and _.
bool place_tag_ok(std::string_view tag);

} // namespace giant::world
