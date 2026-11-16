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

// What is wrong among them all, or nothing: a place's or a look's name
// had twice, a person's look or home that is not there, an id had twice or
// not under `next_person_id`, more than the scene keeps. (Each one's own
// fields are check_place's, check_person_look's and check_person's.)
std::optional<std::string> check_people(std::span<const Place> places, std::span<const PersonLook> looks,
                                        std::span<const Person> people, PersonId next_person_id);

const Place* find_place(std::span<const Place> places, std::string_view name);
const PersonLook* find_person_look(std::span<const PersonLook> looks, std::string_view name);
// (People are kept in the order made, ids rising: found by halving.)
const Person* find_person(std::span<const Person> people, PersonId id);

// Whether `tag` may be a place's tag: 1 to k_max_tag_bytes of a-z, 0-9 and _.
bool place_tag_ok(std::string_view tag);

} // namespace giant::world
