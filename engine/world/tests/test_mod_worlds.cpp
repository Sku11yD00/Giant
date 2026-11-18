// What mods change of a world (world/mods.h): the mod's world file, and
// laying mods over a world's places, looks, routines and behaviours.
#include "giant/world/mods.h"
#include "giant/world/people.h"

#include "giant_test.h"

#include <string>
#include <utility>
#include <vector>

using namespace giant;
using namespace giant::world;

namespace {

Place place(const char* name, double x, const char* tag = "home") {
    Place p;
    p.name = name;
    p.position = {x, 0.0, -x};
    p.tags = {tag};
    return p;
}

PersonLook look(const char* name, float height = 1.0f) {
    PersonLook l;
    l.name = name;
    l.mesh = "people/villager.glb";
    l.height = height;
    return l;
}

Routine routine(const char* name, const char* place_name) {
    Routine r;
    r.name = name;
    r.entries = {{6.0f, "work", RoutineAt::place, place_name, 1.5f}, {20.0f, "sleep", RoutineAt::home, "", 1.2f}};
    return r;
}

Behaviour behaviour(const char* name, float hide_seconds = 60.0f) {
    Behaviour b;
    b.name = name;
    BehaviourState day;
    day.name = "day";
    day.transitions = {{"hide", {{BehaviourWhen::alarm_over, 0.5f, 0.0f, ""}}}};
    BehaviourState hide;
    hide.name = "hide";
    hide.own = true;
    hide.does = {0.0f, "hide", RoutineAt::home, "", 4.0f};
    hide.transitions = {{"day", {{BehaviourWhen::after, hide_seconds, 0.0f, ""}}}};
    b.states = {day, hide};
    return b;
}

Person person(PersonId id, const char* look_name, const char* home, const char* routine_name, const char* behaviour_name) {
    Person p;
    p.id = id;
    p.look = look_name;
    p.home = home;
    p.routine = routine_name;
    p.behaviour = behaviour_name;
    return p;
}

// A small world: two places, a look, a routine, a behaviour, two people using them all.
struct World {
    std::vector<Place> places = {place("cottage", 10), place("smithy", 50, "work")};
    std::vector<PersonLook> looks = {look("villager")};
    std::vector<Routine> routines = {routine("smith's day", "smithy")};
    std::vector<Behaviour> behaviours = {behaviour("wary")};
    std::vector<Person> people = {person(1, "villager", "cottage", "smith's day", "wary"), person(2, "villager", "", "", "")};
    PersonId next_person_id = 3;
    ModsApplied apply(const std::vector<const ModWorld*>& mods) {
        return apply_mod_worlds(mods, places, looks, routines, behaviours, people, next_person_id);
    }
    bool sound() const { return !check_people(places, looks, routines, people, next_person_id) && !check_behaviours(behaviours, people); }
};

std::string load_error(const std::string& body) {
    ModWorld out;
    const auto error = load_mod_world(R"({"format":"giant-mod-world","version":1)" + body + "}", out);
    return error ? error->message : std::string();
}

bool is(const ModChange& c, usize mod, ModThing thing, const char* name, ModChange::Did did, usize over = 0) {
    return c.mod == mod && c.thing == thing && c.name == name && c.did == did && c.over == over;
}

} // namespace

GIANT_TEST("mod worlds: a mod's world file is saved and loaded as it was, and checked strictly") {
    ModWorld w;
    w.places = {place("tavern", 30, "rest"), place("well", -5)};
    w.person_looks = {look("guard", 1.15f)};
    w.routines = {routine("guard's day", "tavern")};
    w.behaviours = {behaviour("brave", 5.0f)};
    const std::string text = save_mod_world(w);
    GIANT_CHECK(text.find("\"format\": \"giant-mod-world\"") != std::string::npos && text.back() == '\n');
    ModWorld back;
    GIANT_CHECK(!load_mod_world(text, back) && back == w && save_mod_world(back) == text && !back.empty());
    // Nothing in it is a file too; and it may name what only the world has (a routine's place).
    ModWorld none;
    GIANT_CHECK(!load_mod_world(save_mod_world(ModWorld{}), none) && none.empty() && none == ModWorld{});
    GIANT_CHECK(load_error("").empty());
    // The example in docs/mods.md.
    ModWorld example;
    GIANT_CHECK(!load_mod_world(R"({"format": "giant-mod-world", "version": 1,
        "places": [{"name": "tavern", "position": [30, 0, -30], "radius": 4, "tags": ["rest"]}],
        "routines": [], "person_looks": [], "behaviours": []})", example) && example.places.size() == 1 && example.places[0].radius == 4.0f);
    // Refused: another format, unknown keys, people (a mod has none yet), a thing that is wrong, a name twice.
    ModWorld kept = w;
    GIANT_CHECK(load_mod_world(R"({"format":"giant-world","version":1})", kept) && load_mod_world("{", kept) && kept == w);
    GIANT_CHECK(load_error(R"(,"plcaes":[])").find("plcaes") != std::string::npos);
    GIANT_CHECK(load_error(R"(,"people":{"next_id":1,"list":[]})").find("people") != std::string::npos);
    GIANT_CHECK(load_error(R"(,"kits":[])").find("kits") != std::string::npos);
    GIANT_CHECK(load_error(R"(,"places":[{"name":"a","position":[0,0,0],"radius":5000}])").find("places[0]") != std::string::npos);
    GIANT_CHECK(load_error(R"(,"places":[{"name":"a","position":[0,0,0]},{"name":"a","position":[1,0,1]}])").find("two places are named \"a\"") !=
                std::string::npos);
    ModWorld twice = w;
    twice.behaviours.push_back(behaviour("brave"));
    GIANT_CHECK(load_mod_world(save_mod_world(twice), kept) && kept == w);
    twice = w;
    twice.routines.push_back(routine("guard's day", "well"));
    GIANT_CHECK(load_mod_world(save_mod_world(twice), kept));
    twice = w;
    twice.person_looks.push_back(look("guard"));
    GIANT_CHECK(load_mod_world(save_mod_world(twice), kept));
}

GIANT_TEST("mod worlds: a mod adds what is new and replaces what is named already, and each change is told") {
    World world;
    ModWorld winter;
    winter.places = {place("tavern", 30, "rest"), place("smithy", 55, "work")}; // one new, one the world's
    winter.person_looks = {look("villager", 1.2f)};                              // the world's, taller
    winter.routines = {routine("idle day", "tavern")};
    winter.behaviours = {behaviour("wary", 5.0f), behaviour("brave")};
    const ModsApplied applied = world.apply({&winter});
    GIANT_CHECK(applied.problems.empty() && applied.changes.size() == 6 && world.sound());
    GIANT_CHECK(is(applied.changes[0], 0, ModThing::place, "tavern", ModChange::Did::added));
    GIANT_CHECK(is(applied.changes[1], 0, ModThing::place, "smithy", ModChange::Did::replaced_world));
    GIANT_CHECK(is(applied.changes[2], 0, ModThing::person_look, "villager", ModChange::Did::replaced_world));
    GIANT_CHECK(is(applied.changes[3], 0, ModThing::routine, "idle day", ModChange::Did::added));
    GIANT_CHECK(is(applied.changes[4], 0, ModThing::behaviour, "wary", ModChange::Did::replaced_world));
    GIANT_CHECK(is(applied.changes[5], 0, ModThing::behaviour, "brave", ModChange::Did::added));
    // Replaced where it was (the order of the world's own holds); added at the end.
    GIANT_CHECK(world.places.size() == 3 && world.places[0].name == "cottage" && world.places[1] == winter.places[1] &&
                world.places[2] == winter.places[0]);
    GIANT_CHECK(world.looks.size() == 1 && world.looks[0].height == 1.2f && world.routines.size() == 2 && world.routines[1].name == "idle day");
    GIANT_CHECK(world.behaviours.size() == 2 && world.behaviours[0] == winter.behaviours[0] && world.behaviours[1].name == "brave");
    GIANT_CHECK(mod_thing_name(ModThing::place) == "place" && mod_thing_name(ModThing::person_look) == "person_look" &&
                mod_thing_name(ModThing::routine) == "routine" && mod_thing_name(ModThing::behaviour) == "behaviour");
}

GIANT_TEST("mod worlds: a later mod wins, and every clash between mods is told with whom") {
    World world;
    ModWorld first, nothing, second, third;
    first.places = {place("tavern", 30, "rest"), place("smithy", 55, "work")};
    second.places = {place("tavern", 31, "rest"), place("smithy", 56, "work"), place("mill", 90, "work")};
    third.places = {place("tavern", 32, "rest")};
    third.routines = {routine("smith's day", "mill")};
    const ModsApplied applied = world.apply({&first, &nothing, nullptr, &second, &third});
    GIANT_CHECK(applied.problems.empty() && applied.changes.size() == 7 && world.sound());
    GIANT_CHECK(is(applied.changes[2], 3, ModThing::place, "tavern", ModChange::Did::replaced_mod, 0)); // a mod's own addition, replaced
    GIANT_CHECK(is(applied.changes[3], 3, ModThing::place, "smithy", ModChange::Did::replaced_mod, 0)); // the world's, set twice
    GIANT_CHECK(is(applied.changes[4], 3, ModThing::place, "mill", ModChange::Did::added));
    GIANT_CHECK(is(applied.changes[5], 4, ModThing::place, "tavern", ModChange::Did::replaced_mod, 3)); // over the latest to set it
    GIANT_CHECK(is(applied.changes[6], 4, ModThing::routine, "smith's day", ModChange::Did::replaced_world));
    GIANT_CHECK(find_place(world.places, "tavern")->position.x == 32.0 && find_place(world.places, "smithy")->position.x == 56.0 &&
                world.places.size() == 4);
    // In the other order the other wins: the order given is the order laid.
    World other;
    other.apply({&third, &second, &first});
    GIANT_CHECK(find_place(other.places, "tavern")->position.x == 30.0);
    // No mods: nothing changes.
    World same;
    GIANT_CHECK(same.apply({}) == ModsApplied{} && same.apply({&nothing, nullptr}) == ModsApplied{} && same.places == World().places);
}

GIANT_TEST("mod worlds: a mod whose changes would break the world is left out whole, and the rest go in") {
    World world;
    ModWorld good, many, twice, after;
    good.places = {place("tavern", 30, "rest")};
    // More looks than a world keeps: none of the mod goes in, not even its place and routine.
    many.places = {place("mill", 90, "work"), place("tavern", 99, "rest")};
    many.routines = {routine("miller's day", "mill")};
    for (usize i = 0; i < k_max_person_looks; ++i) {
        many.person_looks.push_back(look(("look " + std::to_string(i)).c_str()));
    }
    // A name twice (not from a file, which would have been refused): left out too.
    twice.behaviours = {behaviour("brave"), behaviour("brave")};
    after.places = {place("well", -5)};
    after.behaviours = {behaviour("brave")};
    const std::vector<Place> places_then = {world.places[0], world.places[1], good.places[0]};
    const ModsApplied applied = world.apply({&good, &many, &twice, &after});
    GIANT_CHECK(applied.problems.size() == 2 && applied.problems[0].mod == 1 && applied.problems[1].mod == 2);
    GIANT_CHECK(applied.problems[0].message.find("left out") != std::string::npos &&
                applied.problems[0].message.find("person looks") != std::string::npos);
    GIANT_CHECK(applied.problems[1].message.find("brave") != std::string::npos);
    GIANT_CHECK(world.sound() && world.looks.size() == 1 && world.routines.size() == 1 && world.places.size() == 4 &&
                world.places[2] == places_then[2] && world.places[3].name == "well");
    // No change of a mod left out is told, and what it had set does not make the next a clash.
    GIANT_CHECK(applied.changes.size() == 3 && is(applied.changes[0], 0, ModThing::place, "tavern", ModChange::Did::added) &&
                is(applied.changes[1], 3, ModThing::place, "well", ModChange::Did::added) &&
                is(applied.changes[2], 3, ModThing::behaviour, "brave", ModChange::Did::added));
    GIANT_CHECK(world.behaviours.size() == 2 && find_place(world.places, "tavern")->position.x == 30.0);
}
