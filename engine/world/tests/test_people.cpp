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
    GIANT_CHECK(!check_people(places, looks, people, 5));
    GIANT_CHECK(check_people(places, looks, people, 4)); // an id not under the next
    GIANT_CHECK(check_people(places, looks, people, 0));
    GIANT_CHECK(!check_people({}, {}, {}, 1) && !check_people({}, {}, {}, 9));
    people[1].look = "soldier";
    GIANT_CHECK(check_people(places, looks, people, 5));
    people[1] = person(4, "villager", "barn", 1);
    GIANT_CHECK(check_people(places, looks, people, 5));
    people[1] = person(1, "villager", "", 1); // an id twice
    GIANT_CHECK(check_people(places, looks, people, 5));
    people = {person(4, "villager", "", 1), person(1, "villager", "", 1)}; // out of order
    GIANT_CHECK(check_people(places, looks, people, 5));
    const std::vector<Place> twice{place("inn", 0, 0), place("inn", 5, 0)};
    GIANT_CHECK(check_people(twice, looks, {}, 1));
    const std::vector<PersonLook> looks_twice{look("villager"), look("villager")};
    GIANT_CHECK(check_people(places, looks_twice, {}, 1));

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
