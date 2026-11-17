// The people who live in the world (world/people.h): checking places,
// looks and people, finding them, and saving them with the scene and the
// world.
#include "giant/world/people.h"

#include "giant_test.h"

#include <limits>
#include <utility>

#include <string>
#include <vector>

using namespace giant;
using namespace giant::world;

namespace {

Place place(const char* name, double x, double z) {
    Place p;
    p.name = name;
    p.position = {x, 0.0, z};
    p.tags = {"home"};
    return p;
}

PersonLook look(const char* name) {
    PersonLook l;
    l.name = name;
    l.mesh = "people/villager.glb";
    l.height = 1.1f;
    l.material.base_color = {0.5f, 0.4f, 0.3f};
    l.parts.push_back({"coat", Material{}});
    l.parts.back().material.roughness = 0.9f;
    l.hair.on = true;
    l.matched = {"walk", "idle"};
    return l;
}

Person person(PersonId id, const char* look_name, const char* home, double x) {
    Person p;
    p.id = id;
    p.name = "someone";
    p.look = look_name;
    p.home = home;
    p.position = {x, 1.5, -x};
    p.yaw = 90.0f;
    p.seed = id * 7u;
    return p;
}

std::string load_error(const std::string& text) {
    Scene scene;
    const auto error = load_scene(text, scene);
    return error ? error->message : std::string();
}

// A scene's text with `people` as its people's keys.
std::string scene_with(const std::string& people) {
    std::string text = save_scene(Scene{});
    return text.substr(0, text.rfind('}')) + "," + people + "}";
}

} // namespace

GIANT_TEST("people: a place, a look and a person are checked field by field") {
    GIANT_CHECK(!check_place(place("inn", 1, 2)));
    Place p = place("", 0, 0);
    GIANT_CHECK(check_place(p));
    p = place("inn", 0, 0);
    p.radius = 0.1f;
    GIANT_CHECK(check_place(p));
    p.radius = 2000.0f;
    GIANT_CHECK(check_place(p));
    p = place("inn", 2e8, 0);
    GIANT_CHECK(check_place(p));
    p = place("inn", 0, 0);
    p.yaw = 400.0f;
    GIANT_CHECK(check_place(p));
    p = place("inn", 0, 0);
    p.tags = {"home", "home"};
    GIANT_CHECK(check_place(p));
    p.tags = {"Home"};
    GIANT_CHECK(check_place(p));
    p.tags.assign(k_max_place_tags + 1, "a");
    GIANT_CHECK(check_place(p));
    GIANT_CHECK(place_tag_ok("work_2") && !place_tag_ok("") && !place_tag_ok("a b") &&
                !place_tag_ok(std::string(k_max_tag_bytes + 1, 'a')) && place_tag_ok(std::string(k_max_tag_bytes, 'a')));

    GIANT_CHECK(!check_person_look(look("villager")));
    PersonLook l = look("villager");
    l.mesh.clear();
    GIANT_CHECK(check_person_look(l));
    l = look("villager");
    l.height = 0.0f;
    GIANT_CHECK(check_person_look(l));
    l = look("villager");
    l.parts.push_back(l.parts.front());
    GIANT_CHECK(check_person_look(l));
    l = look("villager");
    l.matched = {"*", "walk"};
    GIANT_CHECK(check_person_look(l));
    l.matched = {"walk", "walk"};
    GIANT_CHECK(check_person_look(l));
    l.matched = {"*"};
    GIANT_CHECK(!check_person_look(l));

    GIANT_CHECK(!check_person(person(1, "villager", "", 3)));
    GIANT_CHECK(check_person(person(0, "villager", "", 3)));
    GIANT_CHECK(check_person(person(1, "", "", 3)));
    Person q = person(1, "villager", "", 3);
    q.yaw = -361.0f;
    GIANT_CHECK(check_person(q));
    q = person(1, "villager", "", 3);
    q.position.y = std::numeric_limits<double>::infinity();
    GIANT_CHECK(check_person(q));
}

GIANT_TEST("people: together, names are unique, looks and homes are there, ids rise under the next") {
    const std::vector<Place> places{place("inn", 0, 0), place("mill", 50, 0)};
    const std::vector<PersonLook> looks{look("villager")};
    std::vector<Person> people{person(1, "villager", "inn", 0), person(4, "villager", "", 1)};
    GIANT_CHECK(!check_people(places, looks, {}, people, 5));
    GIANT_CHECK(check_people(places, looks, {}, people, 4)); // an id not under the next
    GIANT_CHECK(check_people(places, looks, {}, people, 0));
    GIANT_CHECK(!check_people({}, {}, {}, {}, 1) && !check_people({}, {}, {}, {}, 9));
    people[1].look = "soldier";
    GIANT_CHECK(check_people(places, looks, {}, people, 5));
    people[1] = person(4, "villager", "barn", 1);
    GIANT_CHECK(check_people(places, looks, {}, people, 5));
    people[1] = person(1, "villager", "", 1); // an id twice
    GIANT_CHECK(check_people(places, looks, {}, people, 5));
    people = {person(4, "villager", "", 1), person(1, "villager", "", 1)}; // out of order
    GIANT_CHECK(check_people(places, looks, {}, people, 5));
    const std::vector<Place> twice{place("inn", 0, 0), place("inn", 5, 0)};
    GIANT_CHECK(check_people(twice, looks, {}, {}, 1));
    const std::vector<PersonLook> looks_twice{look("villager"), look("villager")};
    GIANT_CHECK(check_people(places, looks_twice, {}, {}, 1));

    people = {person(1, "villager", "inn", 0), person(4, "villager", "", 1), person(9, "villager", "", 2)};
    GIANT_CHECK(find_person(people, 4) == &people[1] && find_person(people, 9) == &people[2]);
    GIANT_CHECK(find_person(people, 5) == nullptr && find_person(people, 0) == nullptr && find_person(people, 10) == nullptr);
    GIANT_CHECK(find_place(places, "mill") == &places[1] && find_place(places, "barn") == nullptr);
    GIANT_CHECK(find_person_look(looks, "villager") == &looks[0] && find_person_look(looks, "") == nullptr);
}

GIANT_TEST("people: saved and loaded with the scene and the world, every field") {
    Scene scene;
    scene.places = {place("inn", 0.5, -3), place("mill", 50, 1e6)};
    scene.places[1].radius = 12.5f;
    scene.places[1].yaw = -45.0f;
    scene.places[1].tags = {"work", "noisy_1"};
    scene.person_looks = {look("villager"), look("miller")};
    scene.person_looks[1].hair = Hair{};
    scene.person_looks[1].parts.clear();
    scene.person_looks[1].matched.clear();
    scene.people = {person(2, "villager", "inn", 0.25), person(3, "miller", "", 123456.789)};
    scene.people[1].name = "Märta \"the\" miller";
    scene.people[1].seed = 0xffffffffu;
    scene.next_person_id = 40;
    const std::string text = save_scene(scene);
    Scene loaded;
    GIANT_CHECK(!load_scene(text, loaded).has_value());
    GIANT_CHECK(loaded.places == scene.places && loaded.person_looks == scene.person_looks);
    GIANT_CHECK(loaded.people == scene.people && loaded.next_person_id == 40);
    GIANT_CHECK(save_scene(loaded) == text);
    // A copy and a moved scene keep them.
    Scene copy(loaded);
    GIANT_CHECK(copy.people == scene.people && copy.places == scene.places && copy.next_person_id == 40);
    Scene moved(std::move(copy));
    GIANT_CHECK(moved.people == scene.people && moved.person_looks == scene.person_looks && moved.next_person_id == 40);
    Scene assigned;
    assigned = std::move(moved);
    GIANT_CHECK(assigned.people == scene.people && assigned.places == scene.places && assigned.next_person_id == 40);

    // A scene without people writes none of their keys; ids once given are kept.
    GIANT_CHECK(save_scene(Scene{}).find("people") == std::string::npos &&
                save_scene(Scene{}).find("places") == std::string::npos);
    Scene emptied;
    emptied.next_person_id = 7;
    Scene emptied_loaded;
    GIANT_CHECK(!load_scene(save_scene(emptied), emptied_loaded).has_value() && emptied_loaded.next_person_id == 7);

    WorldIndex world;
    world.places = scene.places;
    world.person_looks = scene.person_looks;
    world.people = scene.people;
    world.next_person_id = 40;
    WorldIndex world_loaded;
    GIANT_CHECK(!load_world_index(save_world_index(world), world_loaded).has_value());
    GIANT_CHECK(world_loaded.places == world.places && world_loaded.person_looks == world.person_looks &&
                world_loaded.people == world.people && world_loaded.next_person_id == 40);
}

GIANT_TEST("people: a file with wrong people is refused, saying where") {
    const std::string looks = R"("person_looks":[{"name":"v","mesh":"v.glb"}])";
    GIANT_CHECK(load_error(scene_with(looks + R"(,"people":{"next_id":3,"list":[[2,"a","v","",0,0,0,0,5]]})")).empty());
    const auto refused = [&](const std::string& people, const char* where) {
        const std::string error = load_error(scene_with(people));
        return !error.empty() && error.find(where) != std::string::npos;
    };
    GIANT_CHECK(refused(looks + R"(,"people":{"next_id":2,"list":[[2,"a","v","",0,0,0,0,5]]})", "next_person_id"));
    GIANT_CHECK(refused(looks + R"(,"people":{"list":[]})", "people.next_id"));
    GIANT_CHECK(refused(looks + R"(,"people":{"next_id":0,"list":[]})", "people.next_id"));
    GIANT_CHECK(refused(looks + R"(,"people":{"next_id":1})", "people.list"));
    GIANT_CHECK(refused(looks + R"(,"people":{"next_id":1,"list":{}})", "people.list"));
    GIANT_CHECK(refused(looks + R"(,"people":{"next_id":1,"list":[],"extra":1})", "people.extra"));
    GIANT_CHECK(refused(looks + R"(,"people":[])", "people"));
    GIANT_CHECK(refused(looks + R"(,"people":{"next_id":3,"list":[[2,"a","v","",0,0,0,0]]})", "people.list[0]"));
    GIANT_CHECK(refused(looks + R"(,"people":{"next_id":3,"list":[[2,"a","v","",0,0,0,0,-1]]})", "people.list[0]"));
    GIANT_CHECK(refused(looks + R"(,"people":{"next_id":3,"list":[[2.5,"a","v","",0,0,0,0,1]]})", "people.list[0]"));
    GIANT_CHECK(refused(looks + R"(,"people":{"next_id":3,"list":[[2,"a","v","",0,"0",0,0,1]]})", "people.list[0]"));
    GIANT_CHECK(refused(looks + R"(,"people":{"next_id":3,"list":[[2,"a","v","",0,0,0,720,1]]})", "people.list[0]"));
    GIANT_CHECK(refused(looks + R"(,"people":{"next_id":3,"list":[[2,"a","w","",0,0,0,0,1]]})", "look \"w\""));
    GIANT_CHECK(refused(looks + R"(,"people":{"next_id":3,"list":[[2,"a","v","inn",0,0,0,0,1]]})", "home \"inn\""));
    GIANT_CHECK(refused(looks + R"(,"people":{"next_id":9,"list":[[2,"a","v","",0,0,0,0,1],[2,"b","v","",0,0,0,0,1]]})",
                        "order"));
    GIANT_CHECK(refused(R"("person_looks":[{"name":"v"}])", "person_looks[0].mesh"));
    GIANT_CHECK(refused(R"("person_looks":[{"name":"v","mesh":"v.glb","height":50}])", "person_looks[0].height"));
    GIANT_CHECK(refused(R"("person_looks":[{"name":"v","mesh":"v.glb","colour":1}])", "person_looks[0].colour"));
    GIANT_CHECK(refused(R"("person_looks":[{"name":"v","mesh":"v.glb"},{"name":"v","mesh":"v.glb"}])", "\"v\""));
    GIANT_CHECK(refused(R"("person_looks":[{"name":"v","mesh":"v.glb","matched":["a","a"]}])", "matched"));
    GIANT_CHECK(refused(R"("person_looks":[{"name":"v","mesh":"v.glb","parts":[{"name":"a"}]}])", "parts[0]"));
    GIANT_CHECK(refused(R"("places":[{"name":"inn"}])", "places[0].position"));
    GIANT_CHECK(refused(R"("places":[{"name":"inn","position":[0,0,0],"radius":0}])", "places[0].radius"));
    GIANT_CHECK(refused(R"("places":[{"name":"inn","position":[0,0,0],"tags":["A"]}])", "places[0]"));
    GIANT_CHECK(refused(R"("places":[{"name":"inn","position":[0,0,0],"tags":[1]}])", "places[0].tags"));
    GIANT_CHECK(refused(R"("places":[{"name":"inn","position":[0,0,0]},{"name":"inn","position":[1,0,0]}])", "\"inn\""));
    GIANT_CHECK(refused(R"("places":{})", "places"));
    GIANT_CHECK(refused(looks + R"(,"people":{"next_id":3,"list":[[2,"a","v","",0,0,0,1e300,1]]})", "yaw"));
    GIANT_CHECK(refused(looks + R"(,"people":{"next_id":3,"list":[[2,"a","v","",0,0,0,0,4294967296]]})", "people.list[0]"));
    GIANT_CHECK(refused(looks + R"(,"people":{"next_id":4294967296,"list":[]})", "people.next_id"));
    GIANT_CHECK(load_error(scene_with(looks + R"(,"people":{"next_id":4294967295,"list":[[4294967294,"a","v","",0,0,0,360,4294967295]]})")).empty());

    // A world's index reads them the same way.
    std::string index = save_world_index(WorldIndex{});
    index = index.substr(0, index.rfind('}')) + "," + looks + R"(,"people":{"next_id":3,"list":[[2,"a","w","",0,0,0,0,1]]}})";
    WorldIndex world;
    const auto error = load_world_index(index, world);
    GIANT_CHECK(error && error->message.find("look \"w\"") != std::string::npos && world.person_looks.empty());
}

namespace {

Routine day() {
    Routine r;
    r.name = "villager's day";
    r.entries = {{6.5f, "work", RoutineAt::tag, "work", 1.6f},
                 {12.0f, "eat", RoutineAt::place, "inn", 1.4f},
                 {13.0f, "work", RoutineAt::tag, "work", 1.4f},
                 {18.0f, "rest", RoutineAt::stay, "", 1.4f},
                 {22.0f, "sleep", RoutineAt::home, "", 1.2f}};
    return r;
}

} // namespace

GIANT_TEST("routines: checked entry by entry; the entry that holds at an hour, over midnight too") {
    const Routine good = day();
    GIANT_CHECK(!check_routine(good));
    const auto bad = [&](auto&& change) {
        Routine r = day();
        change(r);
        return check_routine(r).has_value();
    };
    GIANT_CHECK(bad([](Routine& r) { r.name.clear(); }));
    GIANT_CHECK(bad([](Routine& r) { r.name.assign(k_max_name_bytes + 1, 'a'); }));
    GIANT_CHECK(bad([](Routine& r) { r.entries.clear(); }));
    GIANT_CHECK(bad([](Routine& r) {
        const RoutineEntry first = r.entries[0];
        r.entries.assign(k_max_routine_entries + 1, first);
    }));
    GIANT_CHECK(bad([](Routine& r) { r.entries[0].from = -0.5f; }));
    GIANT_CHECK(bad([](Routine& r) { r.entries[4].from = 24.0f; }));
    GIANT_CHECK(bad([](Routine& r) { r.entries[0].from = std::numeric_limits<float>::quiet_NaN(); }));
    GIANT_CHECK(bad([](Routine& r) { r.entries[2].from = 12.0f; })); // not later than the one before
    GIANT_CHECK(bad([](Routine& r) { r.entries[1].activity = "Eat"; }));
    GIANT_CHECK(bad([](Routine& r) { r.entries[1].activity.clear(); }));
    GIANT_CHECK(bad([](Routine& r) { r.entries[1].speed = 0.0f; }));
    GIANT_CHECK(bad([](Routine& r) { r.entries[1].speed = 25.0f; }));
    GIANT_CHECK(bad([](Routine& r) { r.entries[1].name.clear(); }));     // a place unnamed
    GIANT_CHECK(bad([](Routine& r) { r.entries[0].name = "At Work"; })); // not a tag
    GIANT_CHECK(bad([](Routine& r) { r.entries[3].name = "inn"; }));     // stay names nothing
    GIANT_CHECK(bad([](Routine& r) { r.entries[4].name = "inn"; }));     // nor home
    GIANT_CHECK(bad([](Routine& r) { r.entries[0].at = static_cast<RoutineAt>(9); }));

    // The entry at an hour: the last begun; before the first, the last of all.
    GIANT_CHECK(routine_entry_at(good, 6.5) == 0 && routine_entry_at(good, 11.99) == 0 && routine_entry_at(good, 12.0) == 1);
    GIANT_CHECK(routine_entry_at(good, 17.0) == 2 && routine_entry_at(good, 23.9) == 4);
    GIANT_CHECK(routine_entry_at(good, 0.0) == 4 && routine_entry_at(good, 6.49) == 4); // asleep since yesterday
    GIANT_CHECK(routine_entry_at(good, 30.5) == 0 && routine_entry_at(good, -1.0) == 4); // taken round the day
    GIANT_CHECK(routine_entry_at(good, std::numeric_limits<double>::infinity()) == 4);
    GIANT_CHECK(routine_entry_ends(good, 0) == 12.0f && routine_entry_ends(good, 4) == 6.5f);
    Routine one;
    one.name = "idle";
    one.entries = {{9.0f, "idle", RoutineAt::stay, "", 1.4f}};
    GIANT_CHECK(!check_routine(one) && routine_entry_at(one, 3.0) == 0 && routine_entry_at(one, 20.0) == 0);
    GIANT_CHECK(routine_entry_ends(one, 0) == 9.0f);
    GIANT_CHECK(routine_entry_at(Routine{}, 3.0) == 0 && routine_entry_ends(Routine{}, 0) == 0.0f); // (none: nothing read)

    // Together: names once, a person's routine there.
    const std::vector<PersonLook> looks = {look("villager")};
    std::vector<Routine> routines = {day()};
    std::vector<Person> people = {person(1, "villager", "", 0.0)};
    people[0].routine = "villager's day";
    GIANT_CHECK(!check_people({}, looks, routines, people, 2));
    GIANT_CHECK(find_routine(routines, "villager's day") == &routines[0] && find_routine(routines, "night") == nullptr);
    people[0].routine = "night";
    const auto missing = check_people({}, looks, routines, people, 2);
    GIANT_CHECK(missing && missing->find("routine \"night\"") != std::string::npos);
    people[0].routine.clear();
    routines.push_back(day());
    GIANT_CHECK(check_people({}, looks, routines, people, 2)); // a name twice
    people[0].routine.assign(k_max_name_bytes + 1, 'r');
    GIANT_CHECK(check_person(people[0]));
}

GIANT_TEST("routines: saved and loaded with the scene and the world; older files without them read as before") {
    Scene scene;
    scene.places = {place("inn", 0.5, -3)};
    scene.person_looks = {look("villager")};
    scene.routines = {day()};
    Routine night;
    night.name = "watch";
    night.entries = {{0.0f, "watch", RoutineAt::place, "a place not there", 0.1f}, {23.5f, "walk", RoutineAt::tag, "gate_2", 20.0f}};
    scene.routines.push_back(night); // (a place that is gone is kept: one stays where it is then)
    scene.people = {person(2, "villager", "inn", 0.25), person(3, "villager", "", 4.0)};
    scene.people[0].routine = "villager's day";
    scene.next_person_id = 4;
    const std::string text = save_scene(scene);
    Scene loaded;
    GIANT_CHECK(!load_scene(text, loaded).has_value());
    GIANT_CHECK(loaded.routines == scene.routines && loaded.people == scene.people);
    GIANT_CHECK(save_scene(loaded) == text);
    // One with a routine has it in its row (the name is written twice: the routine's and the person's);
    // without, the people are written as they ever were.
    const auto times = [](const std::string& in, const std::string& what) {
        usize n = 0;
        for (usize at = in.find(what); at != std::string::npos; at = in.find(what, at + 1)) ++n;
        return n;
    };
    GIANT_CHECK(times(text, "villager's day") == 2);
    Scene plain = scene, bare = scene;
    plain.people[0].routine.clear();
    bare.routines.clear();
    bare.people[0].routine.clear();
    GIANT_CHECK(times(save_scene(plain), "villager's day") == 1);
    const std::string bare_text = save_scene(bare), plain_text = save_scene(plain);
    GIANT_CHECK(bare_text.find("routines") == std::string::npos &&
                plain_text.substr(plain_text.find("\"people\"")) .find("someone") != std::string::npos);
    // Hours and speeds that are not whole binary fractions come back as they were.
    Scene odd;
    odd.routines = {day()};
    odd.routines[0].entries[0].from = 0.1f;
    odd.routines[0].entries[1].from = 12.3f;
    odd.routines[0].entries[4].from = 23.999f;
    Scene odd_loaded;
    GIANT_CHECK(!load_scene(save_scene(odd), odd_loaded).has_value() && odd_loaded.routines == odd.routines);
    GIANT_CHECK(routine_entry_at(odd.routines[0], 0.1) == 0 && routine_entry_at(odd.routines[0], 0.09) == 4 &&
                routine_entry_at(odd.routines[0], 12.3) == 1 && routine_entry_at(odd.routines[0], 23.999) == 4);
    Scene copy(loaded);
    Scene moved(std::move(copy));
    Scene assigned;
    assigned = std::move(moved);
    GIANT_CHECK(assigned.routines == scene.routines);
    GIANT_CHECK(save_scene(Scene{}).find("routines") == std::string::npos);

    WorldIndex world;
    world.person_looks = scene.person_looks;
    world.routines = scene.routines;
    world.people = scene.people;
    world.people[0].home.clear();
    world.next_person_id = 4;
    WorldIndex world_loaded;
    GIANT_CHECK(!load_world_index(save_world_index(world), world_loaded).has_value());
    GIANT_CHECK(world_loaded.routines == world.routines && world_loaded.people == world.people);

    // Refused, saying where.
    const std::string looks = R"("person_looks":[{"name":"v","mesh":"v.glb"}])";
    const auto refused = [&](const std::string& people, const char* where) {
        const std::string error = load_error(scene_with(people));
        return !error.empty() && error.find(where) != std::string::npos;
    };
    const std::string r = R"("routines":[{"name":"d","entries":[{"from":8,"activity":"work","at":"tag","name":"work"}]}])";
    GIANT_CHECK(load_error(scene_with(looks + "," + r + R"(,"people":{"next_id":3,"list":[[2,"a","v","",0,0,0,0,5,"d"]]})")).empty());
    GIANT_CHECK(refused(looks + "," + r + R"(,"people":{"next_id":3,"list":[[2,"a","v","",0,0,0,0,5,"e"]]})", "routine \"e\""));
    GIANT_CHECK(refused(looks + "," + r + R"(,"people":{"next_id":3,"list":[[2,"a","v","",0,0,0,0,5,7]]})", "people.list[0]"));
    GIANT_CHECK(refused(looks + "," + r + R"(,"people":{"next_id":3,"list":[[2,"a","v","",0,0,0,0,5,"d",1]]})", "people.list[0]"));
    GIANT_CHECK(refused(R"("routines":{})", "routines"));
    GIANT_CHECK(refused(R"("routines":[{"name":"d"}])", "routines[0].entries"));
    GIANT_CHECK(refused(R"("routines":[{"name":"d","entries":[]}])", "routines[0].entries"));
    GIANT_CHECK(refused(R"("routines":[{"entries":[{"from":8,"activity":"work","at":"stay"}]}])", "routines[0].name"));
    GIANT_CHECK(refused(R"("routines":[{"name":"d","entries":[{"activity":"work","at":"stay"}]}])", "entries[0].from"));
    GIANT_CHECK(refused(R"("routines":[{"name":"d","entries":[{"from":25,"activity":"work","at":"stay"}]}])", "entries[0].from"));
    GIANT_CHECK(refused(R"("routines":[{"name":"d","entries":[{"from":24,"activity":"work","at":"stay"}]}])", "entries[0]"));
    GIANT_CHECK(refused(R"("routines":[{"name":"d","entries":[{"from":8,"at":"stay"}]}])", "entries[0].activity"));
    GIANT_CHECK(load_error(scene_with(R"("routines":[{"name":"d","entries":[{"from":8,"activity":"work"}]}])")).empty()); // (at: stay)
    GIANT_CHECK(refused(R"("routines":[{"name":"d","entries":[{"from":8,"activity":"work","at":3}]}])", "entries[0].at"));
    GIANT_CHECK(refused(R"("routines":[{"name":"d","entries":[{"from":"8","activity":"work"}]}])", "entries[0].from"));
    GIANT_CHECK(refused(R"("routines":[{"name":"d","entries":[{"from":8,"activity":"work","speed":"fast"}]}])", "entries[0].speed"));
    GIANT_CHECK(refused(R"("routines":[{"name":"d","entries":[7]}])", "entries[0]"));
    GIANT_CHECK(refused(R"("routines":[{"name":"d","entries":{}}])", "routines[0].entries"));
    GIANT_CHECK(refused(looks + "," + r + R"(,"people":{"next_id":3,"list":[[2,"a","v","",0,0,0,0,5,""]]})", "people.list[0]"));
    {
        std::string many = R"("routines":[{"name":"d","entries":[)";
        for (usize i = 0; i <= k_max_routine_entries; ++i) {
            many += std::string(i == 0 ? "" : ",") + R"({"from":)" + std::to_string(i) + R"(,"activity":"a"})";
        }
        GIANT_CHECK(refused(many + "]}]", "routines[0].entries"));
        std::string lots = R"("routines":[)";
        for (usize i = 0; i <= k_max_routines; ++i) {
            lots += std::string(i == 0 ? "" : ",") + R"({"name":"r)" + std::to_string(i) + R"(","entries":[{"from":1,"activity":"a"}]})";
        }
        GIANT_CHECK(refused(lots + "]", "routines must be an array"));
    }
    GIANT_CHECK(refused(R"("routines":[{"name":"d","entries":[{"from":8,"activity":"work","at":"away"}]}])", "entries[0].at"));
    GIANT_CHECK(refused(R"("routines":[{"name":"d","entries":[{"from":8,"activity":"work","at":"place"}]}])", "entries[0]"));
    GIANT_CHECK(refused(R"("routines":[{"name":"d","entries":[{"from":8,"activity":"work","at":"stay","speed":50}]}])", "entries[0].speed"));
    GIANT_CHECK(refused(R"("routines":[{"name":"d","entries":[{"from":8,"activity":"work","at":"stay","pace":1}]}])", "entries[0].pace"));
    GIANT_CHECK(refused(R"("routines":[{"name":"d","entries":[{"from":8,"activity":"a","at":"stay"},{"from":8,"activity":"b","at":"stay"}]}])", "entries[1]"));
    GIANT_CHECK(refused(R"("routines":[{"name":"d","entries":[{"from":8,"activity":"a","at":"stay"}],"extra":1}])", "routines[0].extra"));
    GIANT_CHECK(refused(r.substr(0, r.size() - 1) + "," + r.substr(r.find('[') + 1), "\"d\""));
}

GIANT_TEST("people: an entity standing for a person has the person's own id, comes last and is never saved") {
    Scene scene;
    Entity post;
    post.name = "post";
    const EntityId first = scene.add(post);
    Entity stand;
    stand.person = 7;
    stand.id = 99; // (whatever it is given, it takes its person's)
    const EntityId seven = scene.add(stand, usize{0});
    GIANT_CHECK(seven == person_entity_id(7) && seven == k_person_entity_base + 7 && scene.next_id() == first + 1);
    GIANT_CHECK(scene.add(stand) == 0 && scene.size() == 2); // there already
    stand.person = 3;
    GIANT_CHECK(scene.add(stand) == person_entity_id(3));
    // The scene's own go before them, wherever asked; their ids go on from the scene's.
    Entity gate;
    gate.name = "gate";
    const EntityId second = scene.add(gate);
    Entity wall;
    wall.name = "wall";
    const EntityId third = scene.add(wall, usize{100});
    GIANT_CHECK(second == first + 1 && third == first + 2 && scene.authored() == 3 && scene.size() == 5);
    GIANT_CHECK(scene.entities()[1].name == "gate" && scene.entities()[2].name == "wall" && scene.entities()[3].person == 7 &&
                scene.entities()[4].person == 3);
    // Taken out and put back, with one of the scene's: each where it belongs, the next id untouched.
    const EntityId out[] = {seven, second};
    std::vector<Scene::Removed> removed = scene.remove_many(out);
    GIANT_CHECK(removed.size() == 2 && scene.size() == 3 && scene.authored() == 2);
    GIANT_CHECK(scene.remove(person_entity_id(3)).has_value() && scene.authored() == 2 && scene.size() == 2);
    scene.restore_many(std::move(removed));
    GIANT_CHECK(scene.size() == 4 && scene.authored() == 3 && scene.entities()[1].name == "gate" &&
                scene.entities()[3].person == 7 && scene.next_id() == first + 3);
    GIANT_CHECK(scene.find(seven) != nullptr && scene.find(seven)->person == 7);
    // One put back where one of the scene's own has been added since still comes last.
    Scene small;
    small.add(post);
    stand.person = 7;
    const EntityId again = small.add(stand);
    std::vector<Scene::Removed> one;
    one.push_back(*small.remove(again));
    GIANT_CHECK(one[0].index == 1 && small.add(gate) != 0);
    small.restore_many(std::move(one));
    GIANT_CHECK(small.size() == 3 && small.authored() == 2 && small.entities()[1].name == "gate" &&
                small.entities()[2].person == 7 && small.find(again) == &small.entities()[2]);
    // Many of the scene's own added while people stand: each found, the people still last.
    for (PersonId person = 20; person < 30; ++person) {
        stand.person = person;
        GIANT_CHECK(small.add(stand) == person_entity_id(person));
    }
    for (int i = 0; i < 25; ++i) GIANT_CHECK(small.add(wall) != 0);
    GIANT_CHECK(small.size() == 38 && small.authored() == 27);
    for (usize i = 0; i < small.size(); ++i) {
        const Entity& e = small.entities()[i];
        GIANT_CHECK(small.find(e.id) == &e && (e.person != 0) == (i >= 27));
    }
    // The scene's own ids stop under the people's.
    Entity far_id;
    far_id.id = k_max_entity_id;
    GIANT_CHECK(k_max_entity_id + 1 == k_person_entity_base && small.add(far_id) == k_max_entity_id && small.add(wall) == 0);
    GIANT_CHECK(person_entity_id(0xffffffffu) <= k_max_any_entity_id);

    // Saved: the scene's own alone, in a scene's text and in a cell's file.
    const std::string text = save_scene(scene);
    Scene loaded;
    GIANT_CHECK(!load_scene(text, loaded).has_value() && loaded.size() == 3 && loaded.authored() == 3 &&
                loaded.next_id() == scene.next_id());
    const std::string cell = save_cell({0, 0}, scene.entities(), nullptr, nullptr, nullptr);
    GIANT_CHECK(cell.find("gate") != std::string::npos && cell.find(std::to_string(seven)) == std::string::npos);
    // A copy keeps them (whoever follows the copy takes them out); cleared, none.
    Scene copy(scene);
    GIANT_CHECK(copy.size() == 4 && copy.authored() == 3);
    copy.clear();
    GIANT_CHECK(copy.size() == 0 && copy.authored() == 0);
}
