// What mods change of a world (world/mods.h): the mod's world file, and
// laying mods over a world's places, looks, routines and behaviours.
#include "giant/world/mods.h"
#include "giant/world/people.h"

#include "giant_test.h"

#include <initializer_list>
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
    std::vector<u32> slots = {0, 1, 2, 3}; // (each mod's, by its place in the list)
    ModsApplied apply(const std::vector<const ModWorld*>& mods) {
        return apply_mod_worlds(mods, slots, places, looks, routines, behaviours, people, next_person_id);
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
    GIANT_CHECK(load_error(R"(,"people":{"next_id":1,"list":[]})").find("next_id") != std::string::npos);
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

GIANT_TEST("mod worlds: a mod left out leaves no trace of what it set, and one setting a thing twice is left out") {
    World world;
    ModWorld bad, later, twice;
    // Replaces the world's smithy and adds a tavern, then cannot go in (a name twice among its looks).
    bad.places = {place("smithy", 77, "work"), place("tavern", 30, "rest")};
    bad.person_looks = {look("a"), look("a")};
    later.places = {place("smithy", 56, "work"), place("tavern", 31, "rest")};
    // Sets a thing the world has twice: not a clash with itself, a mistake.
    twice.places = {place("cottage", 1), place("cottage", 2)};
    const std::vector<Place> before = world.places;
    const ModsApplied applied = world.apply({&bad, &later, &twice});
    GIANT_CHECK(applied.problems.size() == 2 && applied.problems[0].mod == 0 && applied.problems[1].mod == 2 &&
                applied.problems[1].message.find("sets the place \"cottage\" twice") != std::string::npos);
    // The later mod's are over the world's, not over the mod left out; the lists are the world's with the later mod's.
    GIANT_CHECK(applied.changes.size() == 2 && is(applied.changes[0], 1, ModThing::place, "smithy", ModChange::Did::replaced_world) &&
                is(applied.changes[1], 1, ModThing::place, "tavern", ModChange::Did::added));
    GIANT_CHECK(world.places.size() == 3 && world.places[0] == before[0] && world.places[1] == later.places[0] &&
                world.places[2] == later.places[1] && world.looks.size() == 1 && world.sound());
    // Left out after an earlier mod set the same things: they are that mod's again, and the next clash is with it.
    World again;
    const ModsApplied second = again.apply({&later, &bad, &later});
    GIANT_CHECK(second.problems.size() == 1 && second.changes.size() == 4 &&
                is(second.changes[2], 2, ModThing::place, "smithy", ModChange::Did::replaced_mod, 0) &&
                is(second.changes[3], 2, ModThing::place, "tavern", ModChange::Did::replaced_mod, 0) && again.places.size() == 3);
}

GIANT_TEST("mod worlds: the world's own is told from the mods' whatever was done to the mods' things") {
    // The file's own; what mods set (the smithy replaced, the tavern added); what there is now.
    const std::vector<Place> own = {place("cottage", 10), place("smithy", 50, "work"), place("pond", 5)};
    const std::vector<Place> brought = {place("smithy", 55, "work"), place("tavern", 30, "rest")};
    const auto save = [&](const std::vector<Place>& now, std::vector<std::string>* not_kept = nullptr) {
        return world_own<Place>(own, brought, now, not_kept);
    };
    // As the mods left it: the file's own, nothing to tell.
    const std::vector<Place> laid = {own[0], brought[0], own[2], brought[1]};
    std::vector<std::string> not_kept;
    GIANT_CHECK(save(laid, &not_kept) == own && not_kept.empty());
    // The world's own changed, removed and made: saved so, in the file's order and then the new ones.
    std::vector<Place> now = {place("well", -5), place("cottage", 11), brought[0], brought[1], place("mill", 90, "work")};
    GIANT_CHECK(save(now, &not_kept) == std::vector<Place>({now[1], own[1], now[0], now[4]}) && not_kept.empty());
    // The mods' things changed or removed: the file's own of those names stays as it was, and they are named.
    now = {own[0], place("smithy", 99, "work"), own[2]};
    GIANT_CHECK(save(now, &not_kept) == own && not_kept == std::vector<std::string>({"smithy", "tavern"}));
    // A mods' thing put to exactly what the world's own is: still the mods' name, still the file's own, and said.
    not_kept.clear();
    now = {own[0], own[1], own[2], brought[1]};
    GIANT_CHECK(save(now, &not_kept) == own && not_kept == std::vector<std::string>{"smithy"});
    // No mods: what there is now, the file's order first.
    GIANT_CHECK(world_own<Place>(own, {}, std::vector<Place>{own[2], place("new", 1), own[0]}) == std::vector<Place>({own[0], own[2], place("new", 1)}));
    GIANT_CHECK(world_own<Place>({}, {}, {}).empty() && world_own<Place>({}, brought, laid) == std::vector<Place>({own[0], own[2]}));
    // The other kinds go the same way.
    const std::vector<Routine> routines = {routine("day", "smithy")};
    const std::vector<Routine> brought_routines = {routine("day", "tavern"), routine("night", "tavern")};
    not_kept.clear();
    GIANT_CHECK(world_own<Routine>(routines, brought_routines, std::vector<Routine>{routine("day", "pond")}, &not_kept) == routines &&
                not_kept == std::vector<std::string>({"day", "night"}));
    // One made anew under a name a mod set is the mod's still: not written, and named when it is not as the mod has it.
    not_kept.clear();
    now = {own[0], own[1], own[2], place("tavern", 77)};
    GIANT_CHECK(save(now, &not_kept) == own && not_kept == std::vector<std::string>({"smithy", "tavern"}));
    const std::vector<Behaviour> behaviours = {behaviour("wary")};
    GIANT_CHECK(world_own<Behaviour>(behaviours, {}, std::vector<Behaviour>{behaviour("wary", 5.0f), behaviour("brave")}).size() == 2);
    const std::vector<PersonLook> looks = {look("villager")};
    GIANT_CHECK(world_own<PersonLook>(looks, std::vector<PersonLook>{look("guard")}, std::vector<PersonLook>{look("villager", 1.3f), look("guard")}) ==
                std::vector<PersonLook>{look("villager", 1.3f)});
}

GIANT_TEST("mod worlds: a mod's people are numbered in its file and loaded strictly") {
    ModWorld mod;
    mod.person_looks = {look("guard")};
    mod.people = {person(1, "guard", "barracks", "", ""), person(7, "villager", "", "smith's day", "wary")};
    mod.people[0].name = "Captain";
    mod.people[0].position = {5.0, 1.0, -3.0};
    const std::string text = save_mod_world(mod);
    ModWorld back;
    GIANT_CHECK(!load_mod_world(text, back) && back == mod && save_mod_world(back) == text && !mod.empty());
    GIANT_CHECK(text.find("next_id") == std::string::npos);
    // (What they use need not be in the mod: "villager" is the world's.)
    const auto people = [](const std::string& rows) { return load_error(R"(,"people":{"list":[)" + rows + "]}"); };
    GIANT_CHECK(people(R"([1,"a","v","",0,0,0,0,5])").empty() && people("").empty());
    GIANT_CHECK(people(R"([0,"a","v","",0,0,0,0,5])").find("id must not be 0") != std::string::npos);
    GIANT_CHECK(people(R"([65536,"a","v","",0,0,0,0,5])").find("numbered from 1 to 65535") != std::string::npos);
    GIANT_CHECK(people(R"([2,"a","v","",0,0,0,0,5],[2,"b","v","",0,0,0,0,5])").find("in order, each once") != std::string::npos);
    GIANT_CHECK(people(R"([2,"a","v","",0,0,0,0,5],[1,"b","v","",0,0,0,0,5])").find("in order, each once") != std::string::npos);
    GIANT_CHECK(people(R"([1,"a","","",0,0,0,0,5])").find("look") != std::string::npos);
    GIANT_CHECK(!load_error(R"(,"people":{"next_id":3,"list":[]})").empty() && !load_error(R"(,"people":[])").empty() &&
                !load_error(R"(,"people":{})").empty());
}

GIANT_TEST("mod worlds: a mod's people are added under the ids of its slot, among the world's in order") {
    World w;
    w.slots = {3, 0, k_no_mod_slot};
    w.people.push_back(person(mod_person_id(1, 4), "villager", "", "", "")); // (one of a mod not on now, kept by the world)
    GIANT_CHECK(w.sound());
    const std::vector<Person> world_people = w.people;
    ModWorld guards, traders, plain;
    guards.person_looks = {look("guard")};
    guards.places = {place("barracks", 70)};
    guards.people = {person(1, "guard", "barracks", "smith's day", "wary"), person(9, "villager", "cottage", "", "")};
    traders.people = {person(2, "guard", "barracks", "", "")}; // (an earlier mod's look and place)
    plain.places = {place("well", 5)};                          // (no people: needs no slot)
    const ModsApplied applied = w.apply({&guards, &traders, &plain});
    GIANT_CHECK(applied.problems.empty() && applied.people == std::vector<ModPeople>({{0, 2}, {1, 1}}) && w.sound());
    // By id: the world's own, then slot 0's, the one of slot 1 that was there, slot 3's.
    GIANT_CHECK(w.people.size() == 6 && w.people[0].id == 1 && w.people[1].id == 2 && w.people[2].id == mod_person_id(0, 2) &&
                w.people[3].id == mod_person_id(1, 4) && w.people[4].id == mod_person_id(3, 1) && w.people[5].id == mod_person_id(3, 9));
    GIANT_CHECK(w.people[4].look == "guard" && w.people[4].home == "barracks" && w.people[5].home == "cottage");
    GIANT_CHECK(mod_person_id(0, 1) == k_mod_person_base + 1 && mod_person_id(3, 9) == k_mod_person_base + 3 * 65536 + 9 &&
                mod_person_id(k_max_mod_slots - 1, k_max_mod_people) == 1073741824u + 33554432u - 1u);
    // The same mods, the same slots, another order of loading: the same ids.
    World again;
    again.slots = {0, 3};
    again.people.push_back(world_people.back());
    ModWorld traders_own = traders;
    traders_own.people[0] = person(2, "villager", "", "", "");
    GIANT_CHECK(again.apply({&traders_own, &guards}).problems.empty() && again.people.size() == 6 && again.people[2].id == w.people[2].id &&
                again.people[5].id == w.people[5].id);
}

GIANT_TEST("mod worlds: a mod whose people cannot be added is left out whole, with why") {
    World w;
    const World before = w;
    const auto same = [&] {
        return w.places == before.places && w.looks == before.looks && w.routines == before.routines && w.behaviours == before.behaviours &&
               w.people == before.people;
    };
    const auto refused = [&](const ModWorld& mod, const char* why) {
        const ModsApplied applied = w.apply({&mod});
        return applied.problems.size() == 1 && applied.problems[0].message.find(why) != std::string::npos && applied.people.empty() &&
               applied.changes.empty() && same();
    };
    ModWorld mod;
    mod.places = {place("barracks", 70)};
    mod.people = {person(1, "nobody's look", "", "", "")};
    GIANT_CHECK(refused(mod, "look \"nobody's look\" is not there"));
    mod.people = {person(1, "villager", "nowhere", "", "")};
    GIANT_CHECK(refused(mod, "home \"nowhere\" is not there"));
    mod.people = {person(1, "villager", "barracks", "no day", "")};
    GIANT_CHECK(refused(mod, "routine \"no day\" is not there"));
    mod.people = {person(1, "villager", "barracks", "", "fearless")};
    GIANT_CHECK(refused(mod, "fearless"));
    // No slot; numbers out of order (never from a file: load refuses them).
    mod.people = {person(1, "villager", "barracks", "", "")};
    w.slots = {k_no_mod_slot};
    GIANT_CHECK(refused(mod, "has no slot"));
    w.slots = {};
    GIANT_CHECK(refused(mod, "has no slot"));
    w.slots = {k_max_mod_slots};
    GIANT_CHECK(refused(mod, "has no slot"));
    w.slots = {2};
    mod.people = {person(2, "villager", "", "", ""), person(2, "villager", "", "", "")};
    GIANT_CHECK(refused(mod, "numbered from 1"));
    // Left out from among the world's (one after them by id), not only from the end.
    {
        World among;
        among.slots = {2};
        among.people.push_back(person(mod_person_id(7, 1), "villager", "", "", ""));
        const std::vector<Person> were = among.people;
        mod.people = {person(1, "villager", "", "", ""), person(2, "nobody's look", "", "", "")};
        GIANT_CHECK(among.apply({&mod}).problems.size() == 1 && among.people == were && among.places == before.places);
        // Two mods of one slot (never from the world's list, which gives each slot once): the second is left out.
        ModWorld first, second;
        first.people = {person(1, "villager", "", "", "")};
        second.people = {person(2, "villager", "", "", "")};
        among.slots = {2, 2};
        const ModsApplied shared = among.apply({&first, &second});
        GIANT_CHECK(shared.people == std::vector<ModPeople>({{0, 1}}) && shared.problems.size() == 1 && shared.problems[0].mod == 1 &&
                    shared.problems[0].message.find("same slot") != std::string::npos && among.people.size() == 4 && among.sound());
    }
    // The world has a person of its ids already.
    mod.people = {person(1, "villager", "barracks", "", "")};
    w.people.push_back(person(mod_person_id(2, 500), "villager", "", "", ""));
    const ModsApplied clash = w.apply({&mod});
    GIANT_CHECK(clash.problems.size() == 1 && clash.problems[0].message.find("already a person") != std::string::npos && w.people.size() == 3 &&
                w.places == before.places);
    // Its ids are those of its slot, from number 0 to the last: a person just outside them is no clash.
    for (const PersonId taken : {mod_person_id(2, 0), mod_person_id(2, k_max_mod_people)}) {
        w.people.back().id = taken;
        GIANT_CHECK(w.apply({&mod}).problems.size() == 1 && w.people.size() == 3);
    }
    w.people.back().id = mod_person_id(3, 0);
    w.people.insert(w.people.begin() + 2, person(mod_person_id(2, 0) - 1, "villager", "", "", ""));
    GIANT_CHECK(w.apply({&mod}).problems.empty() && w.people.size() == 5 && w.people[3].id == mod_person_id(2, 1) && w.sound());
    // More people than a world keeps.
    w = before;
    w.slots = {0, 1};
    ModWorld crowd, small;
    for (u32 i = 1; i <= k_max_mod_people; ++i) {
        crowd.people.push_back(person(i, "villager", "", "", ""));
    }
    small.people = {person(1, "villager", "", "", "")};
    const ModsApplied full = w.apply({&small, &crowd}); // (2 + 1 + 65535 are more than 65536)
    GIANT_CHECK(full.people == std::vector<ModPeople>({{0, 1}}) && full.problems.size() == 1 && full.problems[0].mod == 1 && w.people.size() == 3 && w.sound());
}

GIANT_TEST("mod worlds: the world's own people are told from the mods', and its ids never reach theirs") {
    const std::vector<Person> brought = {person(mod_person_id(0, 1), "guard", "", "", ""), person(mod_person_id(0, 2), "guard", "", "", ""),
                                         person(mod_person_id(4, 1), "guard", "", "", "")};
    const std::vector<Person> own = {person(1, "villager", "", "", ""), person(5, "villager", "", "", "")};
    std::vector<Person> now = {own[0], own[1], brought[0], brought[1], brought[2]};
    std::vector<std::string> not_kept;
    GIANT_CHECK(world_own_people(brought, now, &not_kept) == own && not_kept.empty() && world_own_people_are(own, brought, now));
    // A mod's person changed, one removed: neither is the world's, and both are named. One of the world's own made, one changed.
    now[2].home = "cottage";
    now.erase(now.begin() + 4);
    now.insert(now.begin() + 2, person(6, "villager", "", "", ""));
    now[0].name = "Ann";
    const std::vector<Person> expected = {now[0], now[1], now[2]};
    GIANT_CHECK(world_own_people(brought, now, &not_kept) == expected &&
                not_kept == std::vector<std::string>({"person " + std::to_string(mod_person_id(0, 1)), "person " + std::to_string(mod_person_id(4, 1))}));
    GIANT_CHECK(!world_own_people_are(own, brought, now) && world_own_people_are(expected, brought, now));
    // All of the mods' removed; none of the world's own; no mods.
    not_kept.clear();
    GIANT_CHECK(world_own_people(brought, own, &not_kept) == own && not_kept.size() == 3 && world_own_people_are(own, brought, own));
    GIANT_CHECK(world_own_people(brought, brought).empty() && world_own_people_are({}, brought, brought) && !world_own_people_are(own, brought, brought));
    GIANT_CHECK(world_own_people({}, now) == now && world_own_people_are(now, {}, now) && !world_own_people_are(own, {}, now));
    // A person of a mod that is not on (kept by the world's own file) is the world's.
    const std::vector<Person> kept = {own[0], person(mod_person_id(1, 1), "villager", "", "", "")};
    GIANT_CHECK(world_own_people(brought, kept) == kept);
    // The world's next id is past its own people's only, and never among the mods'.
    const std::vector<PersonLook> looks = {look("villager"), look("guard")};
    now[3].home.clear();
    GIANT_CHECK(!check_people({}, looks, {}, now, 7) && check_people({}, looks, {}, now, 6) && !check_people({}, looks, {}, now, k_mod_person_base) &&
                check_people({}, looks, {}, now, k_mod_person_base + 1) && check_people({}, looks, {}, own, 0));
}
