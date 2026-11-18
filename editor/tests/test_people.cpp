// People in the editor: places, person looks and people made, changed,
// listed and removed by command (undoable, saved with the scene).
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"
#include "../src/people_state.h"

#include "giant_test.h"

#include "giant/world/people.h"

#include <cmath>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <system_error>
#include <utility>

using namespace giant;
using namespace giant::editor;
using ai::Json;

namespace {

struct Fixture {
    ai::CommandRegistry commands;
    MeshLibrary meshes{nullptr};
    EditorState state;
    world::EntityId model = 0;
    Fixture() {
        state.meshes = &meshes;
        register_scene_commands(commands, state);
        register_people_commands(commands, state);
        // A dressed character to take looks from (never drawn here).
        world::Entity e;
        e.shape = world::Shape::mesh;
        e.mesh = "people/villager.glb";
        e.transform.scale = {1.2f, 1.2f, 1.2f};
        e.material.roughness = 0.8f;
        e.parts.push_back({"coat", world::Material{}});
        e.hair.on = true;
        e.animation.matched = {"walk", "idle"};
        model = state.scene.add(e);
    }
    ai::CommandResult run(const char* name, const Json& args = Json::object()) {
        return commands.execute(name, args, {});
    }
};

Json at(double x, double y, double z) { return Json::array({x, y, z}); }

} // namespace

GIANT_TEST("people commands: places made, changed, listed by tag and nearness, removed, undone") {
    Fixture f;
    ai::CommandResult r = f.run("create_place", {{"name", "inn"}, {"position", at(10, 0, 5)}, {"tags", {"home", "rest"}}});
    GIANT_CHECK(r.ok && r.value["radius"] == 1.0 && r.value["tags"].size() == 2);
    GIANT_CHECK(f.run("create_place", {{"name", "mill"}, {"position", at(500, 2, 5)}, {"radius", 8.0}, {"yaw", 90.0}, {"tags", {"work"}}}).ok);
    GIANT_CHECK(f.run("create_place", {{"name", "bench"}, {"position", at(12, 0, 5)}, {"tags", {"rest"}}}).ok);
    const std::vector<world::Place>& places = f.state.scene.places;
    GIANT_CHECK(places.size() == 3 && places[1].radius == 8.0f && places[1].yaw == 90.0f);
    // Refused: a name had, no position, a tag in capitals, a tag twice.
    GIANT_CHECK(!f.run("create_place", {{"name", "inn"}, {"position", at(0, 0, 0)}}).ok);
    GIANT_CHECK(!f.run("create_place", {{"name", "barn"}}).ok);
    GIANT_CHECK(!f.run("create_place", {{"name", "barn"}, {"position", at(0, 0, 0)}, {"tags", {"Work"}}}).ok);
    GIANT_CHECK(!f.run("create_place", {{"name", "barn"}, {"position", at(0, 0, 0)}, {"tags", {"a", "a"}}}).ok);
    GIANT_CHECK(!f.run("create_place", {{"name", "barn"}, {"position", at(0, 0, 0)}, {"radius", 0.1}}).ok);
    GIANT_CHECK(places.size() == 3);
    // Changed: only what is given.
    r = f.run("set_place", {{"name", "inn"}, {"radius", 3.0}});
    GIANT_CHECK(r.ok && places[0].radius == 3.0f && places[0].tags.size() == 2 && places[0].position.x == 10.0);
    GIANT_CHECK(f.run("set_place", {{"name", "inn"}, {"tags", Json::array()}}).ok && places[0].tags.empty());
    GIANT_CHECK(!f.run("set_place", {{"name", "nowhere"}, {"radius", 3.0}}).ok);
    GIANT_CHECK(f.run("undo").ok && places[0].tags.size() == 2 && places[0].radius == 3.0f);
    GIANT_CHECK(f.run("undo").ok && places[0].radius == 1.0f);
    GIANT_CHECK(f.run("redo").ok && places[0].radius == 3.0f);
    // Listed.
    r = f.run("get_places");
    GIANT_CHECK(r.ok && r.value["count"] == 3 && r.value["total"] == 3 && r.value["places"][2]["name"] == "bench");
    r = f.run("get_places", {{"tag", "rest"}});
    GIANT_CHECK(r.value["count"] == 2 && r.value["places"][1]["name"] == "bench");
    r = f.run("get_places", {{"near", at(11, 0, 5)}, {"within", 1.5}});
    GIANT_CHECK(r.value["count"] == 2 && r.value["total"] == 3);
    r = f.run("get_places", {{"near", at(11, 0, 5)}, {"within", 1.5}, {"tag", "home"}});
    GIANT_CHECK(r.value["count"] == 1 && r.value["places"][0]["name"] == "inn");
    r = f.run("get_places", {{"offset", 1}, {"limit", 1}});
    GIANT_CHECK(r.value["count"] == 3 && r.value["places"].size() == 1 && r.value["places"][0]["name"] == "mill");
    r = f.run("get_places", {{"limit", 0}});
    GIANT_CHECK(r.value["count"] == 3 && r.value["places"].empty());
    // Removed from the middle, and put back where it was.
    GIANT_CHECK(f.run("remove_place", {{"name", "mill"}}).ok && places.size() == 2 && places[1].name == "bench");
    GIANT_CHECK(!f.run("remove_place", {{"name", "mill"}}).ok);
    GIANT_CHECK(f.run("undo").ok && places.size() == 3 && places[1].name == "mill" && places[1].radius == 8.0f);
    GIANT_CHECK(f.run("redo").ok && places.size() == 2);
    GIANT_CHECK(f.run("undo").ok && f.run("undo").ok && places.size() == 3 && places[0].radius == 1.0f);
}

GIANT_TEST("people commands: a look is taken from a dressed entity, changed, and kept while people have it") {
    Fixture f;
    GIANT_CHECK(!f.run("set_person_look", {{"name", "villager"}}).ok); // a new one needs an entity
    GIANT_CHECK(!f.run("set_person_look", {{"name", "villager"}, {"entity", 999}}).ok);
    const ai::CommandResult cube = f.run("create_entity", {{"shape", "cube"}});
    GIANT_CHECK(cube.ok && !f.run("set_person_look", {{"name", "villager"}, {"entity", cube.value["id"]}}).ok);
    ai::CommandResult r = f.run("set_person_look", {{"name", "villager"}, {"entity", f.model}});
    GIANT_CHECK(r.ok && r.value["mesh"] == "people/villager.glb" && r.value["hair"] == true && r.value["people"] == 0);
    GIANT_CHECK(r.value["parts"] == Json::array({"coat"}) && r.value["matched"].size() == 2);
    const std::vector<world::PersonLook>& looks = f.state.scene.person_looks;
    GIANT_CHECK(looks.size() == 1 && looks[0].height == 1.2f && looks[0].material.roughness == 0.8f &&
                looks[0].parts.size() == 1 && looks[0].hair.on);
    // The entity is not needed after; height and matched are set alone.
    GIANT_CHECK(f.run("delete_entities", {{"ids", {f.model}}}).ok);
    r = f.run("set_person_look", {{"name", "villager"}, {"height", 0.9}, {"matched", {"*"}}});
    GIANT_CHECK(r.ok && looks[0].height == 0.9f && looks[0].matched.size() == 1 && looks[0].mesh == "people/villager.glb");
    GIANT_CHECK(!f.run("set_person_look", {{"name", "villager"}, {"matched", {"*", "walk"}}}).ok);
    GIANT_CHECK(!f.run("set_person_look", {{"name", "villager"}, {"height", 20.0}}).ok);
    GIANT_CHECK(f.run("undo").ok && looks[0].height == 1.2f && looks[0].matched.size() == 2);
    GIANT_CHECK(f.run("redo").ok && looks[0].height == 0.9f);
    // What they show for their routine's activities: set whole, read back, undone; taking an entity's look leaves it.
    r = f.run("set_person_look", {{"name", "villager"}, {"activities", {{{"activity", "work"}, {"clip", "hammer"}, {"speed", 1.5}},
                                                                      {{"activity", "sleep"}, {"clip", "lie"}, {"loop", false}}}}});
    GIANT_CHECK(r.ok && looks[0].activities.size() == 2 && looks[0].activities[0].speed == 1.5f && looks[0].activities[0].loop &&
                !looks[0].activities[1].loop && looks[0].activities[1].speed == 1.0f);
    GIANT_CHECK(r.value["activities"][1] == Json({{"activity", "sleep"}, {"clip", "lie"}, {"speed", 1.0}, {"loop", false}}));
    GIANT_CHECK(f.run("get_person_looks").value["person_looks"][0]["activities"].size() == 2);
    GIANT_CHECK(!f.run("set_person_look", {{"name", "villager"}, {"activities", {{{"activity", "work"}, {"clip", "a"}}, {{"activity", "work"}, {"clip", "b"}}}}}).ok);
    GIANT_CHECK(!f.run("set_person_look", {{"name", "villager"}, {"activities", {{{"activity", "Work"}, {"clip", "a"}}}}}).ok);
    GIANT_CHECK(!f.run("set_person_look", {{"name", "villager"}, {"activities", {{{"activity", "work"}}}}}).ok && looks[0].activities.size() == 2);
    GIANT_CHECK(f.run("set_person_look", {{"name", "villager"}, {"height", 1.0}}).ok && looks[0].activities.size() == 2);
    GIANT_CHECK(f.run("set_person_look", {{"name", "villager"}, {"activities", Json::array()}}).ok && looks[0].activities.empty());
    GIANT_CHECK(f.run("undo").ok && looks[0].activities.size() == 2 && f.run("undo").ok && f.run("undo").ok && looks[0].activities.empty() &&
                looks[0].height == 0.9f);
    // Kept while people have it.
    GIANT_CHECK(f.run("create_people", {{"people", {{{"look", "villager"}, {"position", at(1, 0, 1)}}}}}).ok);
    GIANT_CHECK(f.run("get_person_looks").value["person_looks"][0]["people"] == 1);
    GIANT_CHECK(!f.run("remove_person_look", {{"name", "villager"}}).ok);
    GIANT_CHECK(f.run("remove_people", {{"ids", {1}}}).ok);
    GIANT_CHECK(f.run("remove_person_look", {{"name", "villager"}}).ok && looks.empty());
    GIANT_CHECK(!f.run("remove_person_look", {{"name", "villager"}}).ok);
    GIANT_CHECK(f.run("undo").ok && looks.size() == 1 && looks[0].height == 0.9f);
}

GIANT_TEST("people commands: people made in batches, changed, listed, removed; ids are never used again") {
    Fixture f;
    GIANT_CHECK(f.run("set_person_look", {{"name", "villager"}, {"entity", f.model}}).ok);
    GIANT_CHECK(f.run("set_person_look", {{"name", "miller"}, {"entity", f.model}}).ok);
    GIANT_CHECK(f.run("create_place", {{"name", "inn"}, {"position", at(10, 1, 5)}, {"yaw", 45.0}}).ok);
    GIANT_CHECK(f.run("create_place", {{"name", "mill"}, {"position", at(500, 2, 5)}}).ok);
    world::Scene& scene = f.state.scene;
    // Refused whole: a look not there, a home not there, neither a position nor a home.
    GIANT_CHECK(!f.run("create_people", {{"people", {{{"look", "villager"}, {"home", "inn"}}, {{"look", "soldier"}, {"home", "inn"}}}}}).ok);
    GIANT_CHECK(!f.run("create_people", {{"people", {{{"look", "villager"}, {"home", "barn"}}}}}).ok);
    GIANT_CHECK(!f.run("create_people", {{"people", {{{"look", "villager"}}}}}).ok);
    GIANT_CHECK(!f.run("create_people", {{"people", Json::array()}}).ok);
    GIANT_CHECK(scene.people.empty() && scene.next_person_id == 1);
    ai::CommandResult r = f.run("create_people",
                                {{"people",
                                  {{{"look", "villager"}, {"home", "inn"}, {"name", "Ada"}},
                                   {{"look", "miller"}, {"home", "mill"}, {"position", at(480, 2, 9)}, {"yaw", -90.0}, {"seed", 7}},
                                   {{"look", "villager"}, {"position", at(11, 1, 5)}}}}});
    GIANT_CHECK(r.ok && r.value["ids"] == Json::array({1, 2, 3}) && r.value["people"] == 3);
    GIANT_CHECK(scene.people.size() == 3 && scene.next_person_id == 4);
    // Given no position, one starts at its home, facing its way.
    GIANT_CHECK(scene.people[0].position == scene.places[0].position && scene.people[0].yaw == 45.0f);
    GIANT_CHECK(scene.people[1].position.x == 480.0 && scene.people[1].yaw == -90.0f && scene.people[1].seed == 7);
    GIANT_CHECK(scene.people[2].home.empty() && scene.people[0].seed != scene.people[2].seed);
    GIANT_CHECK(!world::check_people(scene.places, scene.person_looks, scene.routines, scene.people, scene.next_person_id));
    // A place is kept while it is someone's home.
    GIANT_CHECK(!f.run("remove_place", {{"name", "inn"}}).ok);
    // Changed: only what is given.
    r = f.run("set_person", {{"id", 2}, {"name", "Bo"}, {"home", ""}});
    GIANT_CHECK(r.ok && scene.people[1].name == "Bo" && scene.people[1].home.empty() && scene.people[1].position.x == 480.0);
    GIANT_CHECK(!f.run("set_person", {{"id", 2}, {"look", "soldier"}}).ok);
    GIANT_CHECK(!f.run("set_person", {{"id", 2}, {"home", "barn"}}).ok);
    GIANT_CHECK(!f.run("set_person", {{"id", 9}, {"name", "x"}}).ok);
    GIANT_CHECK(f.run("undo").ok && scene.people[1].name.empty() && scene.people[1].home == "mill");
    GIANT_CHECK(f.run("redo").ok && scene.people[1].name == "Bo");
    // Listed.
    r = f.run("get_people");
    GIANT_CHECK(r.ok && r.value["count"] == 3 && r.value["total"] == 3 && r.value["people"][0]["name"] == "Ada");
    r = f.run("get_people", {{"look", "villager"}});
    GIANT_CHECK(r.value["count"] == 2 && r.value["people"][1]["id"] == 3);
    r = f.run("get_people", {{"home", "inn"}});
    GIANT_CHECK(r.value["count"] == 1 && r.value["people"][0]["id"] == 1);
    r = f.run("get_people", {{"near", at(10, 1, 5)}, {"within", 2.0}});
    GIANT_CHECK(r.value["count"] == 2);
    r = f.run("get_people", {{"near", at(10, 1, 5)}, {"within", 2.0}, {"offset", 1}});
    GIANT_CHECK(r.value["count"] == 2 && r.value["people"].size() == 1 && r.value["people"][0]["id"] == 3);
    r = f.run("get_people", {{"ids", {3, 8, 1}}});
    GIANT_CHECK(r.ok && r.value["count"] == 2 && r.value["people"][0]["id"] == 3 && r.value["people"][1].is_null() &&
                r.value["people"][2]["name"] == "Ada");
    GIANT_CHECK(!f.run("get_people", {{"ids", {3}}, {"look", "villager"}}).ok);
    // Removed: whole or not at all; undone to the same order; the ids are never used again.
    GIANT_CHECK(!f.run("remove_people", {{"ids", {1, 9}}}).ok && !f.run("remove_people", {{"ids", {1, 1}}}).ok);
    GIANT_CHECK(scene.people.size() == 3);
    GIANT_CHECK(f.run("remove_people", {{"ids", {3, 1}}}).ok && scene.people.size() == 1 && scene.people[0].id == 2);
    r = f.run("create_people", {{"people", {{{"look", "miller"}, {"home", "mill"}}}}});
    GIANT_CHECK(r.ok && r.value["ids"] == Json::array({4}) && scene.next_person_id == 5);
    GIANT_CHECK(f.run("undo").ok && scene.people.size() == 1 && scene.next_person_id == 5); // never given back
    GIANT_CHECK(f.run("undo").ok && scene.people.size() == 3 && scene.people[0].id == 1 && scene.people[1].id == 2 &&
                scene.people[2].id == 3 && scene.people[0].name == "Ada");
    GIANT_CHECK(f.run("redo").ok && f.run("redo").ok && scene.people.size() == 2 && scene.people[1].id == 4);
    // Saved and loaded with the scene.
    world::Scene loaded;
    GIANT_CHECK(!world::load_scene(world::save_scene(scene), loaded).has_value());
    GIANT_CHECK(loaded.people == scene.people && loaded.places == scene.places &&
                loaded.person_looks == scene.person_looks && loaded.next_person_id == 5);
    // Undone to the start: nobody; the ids stay used, so new people take new ones.
    while (f.commands.undo()) {
    }
    GIANT_CHECK(scene.people.empty() && scene.places.empty() && scene.person_looks.empty() && scene.next_person_id == 5);
    GIANT_CHECK(f.commands.redo() && f.commands.redo() && f.commands.redo()); // the model, the looks
    r = f.run("create_people", {{"people", {{{"look", "villager"}, {"position", at(0, 0, 0)}}}}});
    GIANT_CHECK(r.ok && r.value["ids"] == Json::array({5}));
}

GIANT_TEST("people commands: thousands a call, up to what a scene keeps") {
    Fixture f;
    GIANT_CHECK(f.run("set_person_look", {{"name", "villager"}, {"entity", f.model}}).ok);
    Json batch = Json::array();
    for (int i = 0; i < 4096; ++i) batch.push_back({{"look", "villager"}, {"position", at(i, 0, i % 7)}});
    for (int call = 0; call < 16; ++call) GIANT_CHECK(f.run("create_people", {{"people", batch}}).ok);
    world::Scene& scene = f.state.scene;
    GIANT_CHECK(scene.people.size() == world::k_max_people && scene.next_person_id == 65537);
    GIANT_CHECK(!f.run("create_people", {{"people", {{{"look", "villager"}, {"position", at(0, 0, 0)}}}}}).ok);
    GIANT_CHECK(!world::check_people(scene.places, scene.person_looks, scene.routines, scene.people, scene.next_person_id));
    // Every other one of a stretch removed, and one in the middle changed.
    Json ids = Json::array();
    for (int id = 1000; id < 9000; id += 2) ids.push_back(id);
    GIANT_CHECK(f.run("remove_people", {{"ids", ids}}).ok && scene.people.size() == world::k_max_people - 4000);
    GIANT_CHECK(world::find_person(scene.people, 1000) == nullptr && world::find_person(scene.people, 1001) != nullptr);
    GIANT_CHECK(f.run("set_person", {{"id", 30000}, {"yaw", 12.0}}).ok && world::find_person(scene.people, 30000)->yaw == 12.0f);
    GIANT_CHECK(f.run("get_people", {{"near", at(3000, 0, 3)}, {"within", 10.0}, {"limit", 0}}).value["count"].get<int>() > 0);
    GIANT_CHECK(f.run("undo").ok && f.run("undo").ok && scene.people.size() == world::k_max_people);
    GIANT_CHECK(!world::check_people(scene.places, scene.person_looks, scene.routines, scene.people, scene.next_person_id));
    GIANT_CHECK(world::find_person(scene.people, 1000)->position.x == 999.0 && world::find_person(scene.people, 30000)->yaw == 0.0f);
    world::Scene loaded;
    GIANT_CHECK(!world::load_scene(world::save_scene(scene), loaded).has_value() && loaded.people == scene.people);
}

GIANT_TEST("people commands: thousands of people find their homes among tens of thousands of places") {
    Fixture f;
    GIANT_CHECK(f.run("set_person_look", {{"name", "villager"}, {"entity", f.model}}).ok);
    world::Scene& scene = f.state.scene;
    for (usize i = 0; i < world::k_max_places; ++i) {
        world::Place p;
        p.name = "house " + std::to_string(i);
        p.position = {static_cast<double>(i), 0.0, 0.0};
        scene.places.push_back(std::move(p));
    }
    GIANT_CHECK(!f.run("create_place", {{"name", "one more"}, {"position", at(0, 0, 0)}}).ok);
    Json batch = Json::array();
    for (usize i = 0; i < 4096; ++i) {
        batch.push_back({{"look", "villager"}, {"home", "house " + std::to_string(world::k_max_places - 1 - i)}});
    }
    GIANT_CHECK(f.run("create_people", {{"people", batch}}).ok && scene.people.size() == 4096);
    GIANT_CHECK(scene.people[0].position.x == 65535.0 && scene.people[4095].position.x == 61440.0);
    batch.back()["home"] = "house 70000";
    GIANT_CHECK(!f.run("create_people", {{"people", batch}}).ok && scene.people.size() == 4096);
    GIANT_CHECK(!world::check_people(scene.places, scene.person_looks, scene.routines, scene.people, scene.next_person_id));
    // The largest seed is kept; one past it is refused.
    GIANT_CHECK(f.run("set_person", {{"id", 1}, {"seed", 4294967295.0}}).ok && scene.people[0].seed == 0xffffffffu);
    GIANT_CHECK(!f.run("set_person", {{"id", 1}, {"seed", 4294967296.0}}).ok && !f.run("set_person", {{"id", 1}, {"seed", -1}}).ok);
    GIANT_CHECK(!f.run("set_person", {{"id", 4294967296.0}, {"yaw", 1.0}}).ok);
}

GIANT_TEST("people commands: undo after commands outside the history never leaves a scene that cannot be loaded") {
    Fixture f;
    const auto script = [&](const char* name, const Json& args) { // as C# scripts run commands
        ai::ExecuteOptions options;
        options.record_history = false;
        return f.commands.execute(name, args, options).ok;
    };
    world::Scene& scene = f.state.scene;
    GIANT_CHECK(f.run("set_person_look", {{"name", "villager"}, {"entity", f.model}}).ok);
    GIANT_CHECK(f.run("create_place", {{"name", "inn"}, {"position", at(1, 0, 1)}}).ok);
    // The place is replaced by another at its index, with someone living there.
    GIANT_CHECK(script("remove_place", {{"name", "inn"}}) && script("create_place", {{"name", "mill"}, {"position", at(9, 0, 9)}}));
    GIANT_CHECK(script("create_people", {{"people", {{{"look", "villager"}, {"home", "mill"}}}}}));
    GIANT_CHECK(f.run("undo").ok); // the inn's making: it is gone already, and the mill is not it
    GIANT_CHECK(scene.places.size() == 1 && scene.places[0].name == "mill");
    GIANT_CHECK(f.run("redo").ok && scene.places.size() == 2 && scene.places[0].name == "inn");
    // Someone moves in outside the history: undoing the inn's making leaves it.
    GIANT_CHECK(script("set_person", {{"id", 1}, {"home", "inn"}}) && f.run("undo").ok);
    GIANT_CHECK(scene.places.size() == 2 && !world::check_people(scene.places, scene.person_looks, scene.routines, scene.people, scene.next_person_id));
    // People removed, then their home and look taken away outside the history: undo brings back
    // only what the scene can keep.
    GIANT_CHECK(f.run("create_people", {{"people", {{{"look", "villager"}, {"home", "mill"}}}}}).ok);
    GIANT_CHECK(f.run("remove_people", {{"ids", {1, 2}}}).ok && scene.people.empty());
    GIANT_CHECK(script("remove_place", {{"name", "mill"}}));
    GIANT_CHECK(f.run("undo").ok && scene.people.size() == 2 && scene.people[0].home == "inn" && scene.people[1].home.empty());
    GIANT_CHECK(!world::check_people(scene.places, scene.person_looks, scene.routines, scene.people, scene.next_person_id));
    GIANT_CHECK(f.run("redo").ok && scene.people.empty() && script("remove_person_look", {{"name", "villager"}}));
    GIANT_CHECK(f.run("undo").ok && scene.people.empty()); // their look is gone: they stay away
    world::Scene loaded;
    GIANT_CHECK(!world::load_scene(world::save_scene(scene), loaded).has_value());
}

GIANT_TEST("people commands: routines made, replaced, listed with the entry at an hour, given to people, kept while had, undone") {
    Fixture f;
    world::Scene& scene = f.state.scene;
    GIANT_CHECK(f.run("create_place", {{"name", "inn"}, {"position", at(10, 0, 5)}, {"tags", {"home"}}}).ok);
    GIANT_CHECK(f.run("set_person_look", {{"name", "villager"}, {"entity", f.model}}).ok);
    const Json entries = Json::array({{{"from", 6.5}, {"activity", "work"}, {"at", "tag"}, {"name", "work"}, {"speed", 1.6}},
                                      {{"from", 12}, {"activity", "eat"}, {"at", "place"}, {"name", "inn"}},
                                      {{"from", 18}, {"activity", "rest"}},
                                      {{"from", 22}, {"activity", "sleep"}, {"at", "home"}}});
    ai::CommandResult r = f.run("set_routine", {{"name", "day"}, {"entries", entries}});
    GIANT_CHECK(r.ok && r.value["entries"].size() == 4 && r.value["people"] == 0);
    GIANT_CHECK(r.value["entries"][2]["at"] == "stay" && r.value["entries"][2]["speed"].get<float>() == 1.4f);
    GIANT_CHECK(scene.routines.size() == 1 && scene.routines[0].entries[0].at == world::RoutineAt::tag &&
                scene.routines[0].entries[0].speed == 1.6f && scene.routines[0].entries[1].name == "inn");
    // Refused: no entries, hours not rising, 24, a word in capitals, a place not there, a place unnamed, a name with stay.
    const auto refused = [&](Json wrong) { return !f.run("set_routine", {{"name", "bad"}, {"entries", std::move(wrong)}}).ok; };
    GIANT_CHECK(refused(Json::array()));
    GIANT_CHECK(refused(Json::array({{{"from", 8}, {"activity", "a"}}, {{"from", 8}, {"activity", "b"}}})));
    GIANT_CHECK(refused(Json::array({{{"from", 24}, {"activity", "a"}}})));
    GIANT_CHECK(refused(Json::array({{{"from", 8}, {"activity", "Work"}}})));
    GIANT_CHECK(refused(Json::array({{{"from", 8}, {"activity", "a"}, {"at", "place"}, {"name", "mill"}}})));
    GIANT_CHECK(refused(Json::array({{{"from", 8}, {"activity", "a"}, {"at", "place"}}})));
    GIANT_CHECK(refused(Json::array({{{"from", 8}, {"activity", "a"}, {"name", "inn"}}})));
    GIANT_CHECK(refused(Json::array({{{"from", 8}, {"activity", "a"}, {"at", "nowhere"}}})));
    GIANT_CHECK(refused(Json::array({{{"from", 8}, {"activity", "a"}, {"speed", 30}}})));
    GIANT_CHECK(scene.routines.size() == 1);
    // Given to people when made and after; one not there is refused.
    r = f.run("create_people", {{"people", {{{"look", "villager"}, {"home", "inn"}, {"routine", "day"}}, {{"look", "villager"}, {"home", "inn"}}}}});
    GIANT_CHECK(r.ok && scene.people[0].routine == "day" && scene.people[1].routine.empty());
    const world::PersonId second = r.value["ids"][1].get<world::PersonId>();
    GIANT_CHECK(!f.run("set_person", {{"id", second}, {"routine", "night"}}).ok);
    r = f.run("set_person", {{"id", second}, {"routine", "day"}});
    GIANT_CHECK(r.ok && r.value["routine"] == "day" && scene.people[1].routine == "day");
    GIANT_CHECK(f.run("set_person", {{"id", second}, {"routine", ""}}).ok && scene.people[1].routine.empty());
    GIANT_CHECK(f.run("undo").ok && scene.people[1].routine == "day");
    r = f.run("get_people", {{"routine", "day"}});
    GIANT_CHECK(r.ok && r.value["count"] == 2 && r.value["people"][0]["routine"] == "day");
    // Listed, with the entry that holds at an hour.
    r = f.run("get_routines", {{"hour", 3.0}});
    GIANT_CHECK(r.ok && r.value["routines"].size() == 1 && r.value["routines"][0]["people"] == 2 && r.value["routines"][0]["now"] == 3);
    GIANT_CHECK(f.run("get_routines", {{"hour", 12.5}}).value["routines"][0]["now"] == 1);
    GIANT_CHECK(f.run("get_routines", {{"hour", 0.0}}).value["routines"][0]["now"] == 3 &&
                f.run("get_routines", {{"hour", 24.0}}).value["routines"][0]["now"] == 3 &&
                f.run("get_routines", {{"hour", 6.5}}).value["routines"][0]["now"] == 0);
    GIANT_CHECK(!f.run("get_routines").value["routines"][0].contains("now"));
    // Replaced whole; undone and done again.
    r = f.run("set_routine", {{"name", "day"}, {"entries", Json::array({{{"from", 0}, {"activity", "idle"}}})}});
    GIANT_CHECK(r.ok && r.value["people"] == 2 && scene.routines.size() == 1 && scene.routines[0].entries.size() == 1);
    GIANT_CHECK(f.run("undo").ok && scene.routines[0].entries.size() == 4);
    GIANT_CHECK(f.run("redo").ok && scene.routines[0].entries.size() == 1);
    GIANT_CHECK(f.run("undo").ok);
    // Kept while people have it; removed once they have not; undo brings it back.
    GIANT_CHECK(!f.run("remove_routine", {{"name", "day"}}).ok && !f.run("remove_routine", {{"name", "night"}}).ok);
    GIANT_CHECK(f.run("set_person", {{"id", second}, {"routine", ""}}).ok);
    GIANT_CHECK(f.run("set_person", {{"id", scene.people[0].id}, {"routine", ""}}).ok);
    GIANT_CHECK(f.run("remove_routine", {{"name", "day"}}).ok && scene.routines.empty());
    GIANT_CHECK(f.run("undo").ok && scene.routines.size() == 1 && scene.routines[0].entries.size() == 4);
    // Undone back to people having it, then on to the start: nothing is left that a file could not hold.
    GIANT_CHECK(f.run("undo").ok && scene.people[0].routine == "day");
    GIANT_CHECK(!world::check_people(scene.places, scene.person_looks, scene.routines, scene.people, scene.next_person_id));
    // People removed, then their routine taken away outside the history (a script): undo brings them back without it.
    const world::PersonId first = scene.people[0].id;
    GIANT_CHECK(f.run("remove_people", {{"ids", {first}}}).ok && scene.people.size() == 1 && scene.people[0].id == second);
    ai::ExecuteOptions outside;
    outside.record_history = false;
    GIANT_CHECK(f.commands.execute("remove_routine", {{"name", "day"}}, outside).ok && scene.routines.empty());
    GIANT_CHECK(f.run("undo").ok && scene.people.size() == 2 && scene.people[0].id == first && scene.people[0].routine.empty());
    GIANT_CHECK(!world::check_people(scene.places, scene.person_looks, scene.routines, scene.people, scene.next_person_id));
    // A routine removed, another of its name made outside the history: undo leaves that one be.
    GIANT_CHECK(f.run("set_routine", {{"name", "day"}, {"entries", entries}}).ok && f.run("remove_routine", {{"name", "day"}}).ok);
    GIANT_CHECK(f.commands.execute("set_routine", {{"name", "day"}, {"entries", Json::array({{{"from", 1}, {"activity", "idle"}}})}}, outside).ok);
    GIANT_CHECK(f.run("undo").ok && scene.routines.size() == 1 && scene.routines[0].entries.size() == 1);
    // As many as a scene keeps, one removed, one more made outside the history: undo does not put a 1025th back.
    scene.routines.clear();
    for (usize i = 0; i < world::k_max_routines; ++i) {
        world::Routine filler;
        filler.name = "r" + std::to_string(i);
        filler.entries = {{0.0f, "idle", world::RoutineAt::stay, "", 1.4f}};
        scene.routines.push_back(filler);
    }
    GIANT_CHECK(!f.run("set_routine", {{"name", "one more"}, {"entries", entries}}).ok);
    GIANT_CHECK(f.run("remove_routine", {{"name", "r5"}}).ok && scene.routines.size() == world::k_max_routines - 1);
    GIANT_CHECK(f.commands.execute("set_routine", {{"name", "one more"}, {"entries", entries}}, outside).ok);
    GIANT_CHECK(f.run("undo").ok && scene.routines.size() == world::k_max_routines);
    GIANT_CHECK(!world::check_people(scene.places, scene.person_looks, scene.routines, scene.people, scene.next_person_id));
    r = f.run("get_routines", {{"hour", 24.0}});
    GIANT_CHECK(r.ok && r.value["routines"].size() == world::k_max_routines && r.value["routines"][0]["now"] == 0);
    // Looks likewise.
    const world::PersonLook villager = scene.person_looks[0];
    while (scene.person_looks.size() < world::k_max_person_looks) {
        scene.person_looks.push_back(villager);
        scene.person_looks.back().name = "look " + std::to_string(scene.person_looks.size());
    }
    GIANT_CHECK(f.run("remove_person_look", {{"name", "look 7"}}).ok);
    GIANT_CHECK(f.commands.execute("set_person_look", {{"name", "another"}, {"entity", f.model}}, outside).ok);
    GIANT_CHECK(f.run("undo").ok && scene.person_looks.size() == world::k_max_person_looks);
    GIANT_CHECK(!world::check_people(scene.places, scene.person_looks, scene.routines, scene.people, scene.next_person_id));
    // People made with a routine not there are refused.
    GIANT_CHECK(!f.run("create_people", {{"people", {{{"look", "villager"}, {"home", "inn"}, {"routine", "night"}}}}}).ok);
    // The place a routine names may go: the routine is kept, and still loads.
    scene.routines.clear();
    for (world::Person& p : scene.people) p.routine.clear();
    GIANT_CHECK(f.run("set_routine", {{"name", "lunch"}, {"entries", Json::array({{{"from", 12}, {"activity", "eat"}, {"at", "place"}, {"name", "inn"}}})}}).ok);
    for (world::Person& p : scene.people) p.home.clear();
    GIANT_CHECK(f.run("remove_place", {{"name", "inn"}}).ok && scene.routines.size() == 1);
    world::Scene loaded;
    GIANT_CHECK(!world::load_scene(world::save_scene(scene), loaded).has_value() && loaded.routines == scene.routines);
}

GIANT_TEST("people commands: behaviours made, replaced, listed, given to people, kept while had, undone") {
    Fixture f;
    world::Scene& scene = f.state.scene;
    GIANT_CHECK(f.run("create_place", {{"name", "inn"}, {"position", at(10, 0, 5)}, {"tags", {"home"}}}).ok);
    GIANT_CHECK(f.run("set_person_look", {{"name", "villager"}, {"entity", f.model}}).ok);
    const auto when = [](std::initializer_list<Json> conditions) { return Json(Json::array_t(conditions)); };
    const Json states = Json::array(
        {{{"name", "day"},
          {"transitions",
           Json::array({{{"to", "flee"}, {"when", when({{{"is", "sensed"}, {"kind", "shot"}}, {{"is", "alarm_over"}, {"value", 0.5}}})}},
                        {{"to", "night"}, {"when", when({{{"is", "hour"}, {"value", 22}, {"until", 6}}})}}})}},
         {{"name", "flee"},
          {"activity", "flee"},
          {"at", "home"},
          {"speed", 5.0},
          {"transitions", Json::array({{{"to", "hide"}, {"when", when({{{"is", "arrived"}}})}}, {{"to", "day"}, {"when", when({{{"is", "failed"}}})}}})}},
         {{"name", "hide"},
          {"activity", "hide"},
          {"transitions", Json::array({{{"to", "day"}, {"when", when({{{"is", "after"}, {"value", 120}}, {{"is", "alarm_under"}, {"value", 0.1}}})}}})}},
         {{"name", "night"}, {"activity", "sleep"}, {"at", "place"}, {"place", "inn"}}});
    ai::CommandResult r = f.run("set_behaviour", {{"name", "wary"}, {"states", states}});
    GIANT_CHECK(r.ok && r.value["states"].size() == 4 && r.value["people"] == 0);
    // Every field comes back: a state with no activity has none, one with has where and how fast.
    GIANT_CHECK(!r.value["states"][0].contains("activity") && r.value["states"][0]["transitions"].size() == 2);
    GIANT_CHECK(r.value["states"][0]["transitions"][0]["when"][0] == Json({{"is", "sensed"}, {"kind", "shot"}}));
    GIANT_CHECK(r.value["states"][0]["transitions"][1]["when"][0]["until"] == 6.0);
    GIANT_CHECK(r.value["states"][2]["at"] == "stay" && r.value["states"][2]["speed"].get<float>() == 1.4f && !r.value["states"][2].contains("place"));
    GIANT_CHECK(r.value["states"][3]["transitions"].empty() && r.value["states"][3]["place"] == "inn");
    GIANT_CHECK(scene.behaviours.size() == 1 && scene.behaviours[0].states[1].own && !scene.behaviours[0].states[0].own &&
                scene.behaviours[0].states[1].does.at == world::RoutineAt::home && scene.behaviours[0].states[1].does.speed == 5.0f &&
                scene.behaviours[0].states[2].transitions[0].conditions[0].value == 120.0f &&
                scene.behaviours[0].states[0].transitions[0].conditions[0].kind == "shot");
    GIANT_CHECK(!world::check_behaviour(scene.behaviours[0]) && !world::check_behaviours(scene.behaviours, scene.people));
    // What it gives is what it takes (but for the count).
    Json again = f.run("get_behaviours").value["behaviours"][0];
    again.erase("people");
    GIANT_CHECK(f.run("set_behaviour", again).ok && scene.behaviours.size() == 1);
    GIANT_CHECK(f.run("undo").ok);
    // Refused.
    const auto refused = [&](Json wrong) { return !f.run("set_behaviour", {{"name", "bad"}, {"states", std::move(wrong)}}).ok; };
    const auto one = [&](Json condition) {
        return Json::array({{{"name", "a"}, {"transitions", Json::array({{{"to", "b"}, {"when", Json::array({std::move(condition)})}}})}}, {{"name", "b"}}});
    };
    GIANT_CHECK(!refused(one({{"is", "arrived"}})));
    GIANT_CHECK(f.run("undo").ok && scene.behaviours.size() == 1);
    GIANT_CHECK(refused(Json::array()));
    GIANT_CHECK(refused(Json::array({{{"name", "A"}}})));
    GIANT_CHECK(refused(Json::array({{{"name", "a"}}, {{"name", "a"}}})));
    GIANT_CHECK(refused(Json::array({{{"name", "a"}, {"at", "home"}}})));  // where, and nothing to do there
    GIANT_CHECK(refused(Json::array({{{"name", "a"}, {"speed", 2.0}}})));
    GIANT_CHECK(refused(Json::array({{{"name", "a"}, {"place", "inn"}}})));
    GIANT_CHECK(refused(Json::array({{{"name", "a"}, {"activity", "x"}, {"at", "place"}, {"place", "mill"}}}))); // no such place
    GIANT_CHECK(refused(Json::array({{{"name", "a"}, {"activity", "x"}, {"at", "place"}}})));
    GIANT_CHECK(refused(Json::array({{{"name", "a"}, {"activity", "x"}, {"place", "inn"}}})));
    GIANT_CHECK(refused(Json::array({{{"name", "a"}, {"activity", "x"}, {"at", "away"}}})));
    GIANT_CHECK(refused(Json::array({{{"name", "a"}, {"activity", "x"}, {"speed", 30}}})));
    GIANT_CHECK(refused(Json::array({{{"name", "a"}, {"transitions", Json::array({{{"to", "a"}, {"when", Json::array({{{"is", "arrived"}}})}}})}}})));
    GIANT_CHECK(refused(Json::array({{{"name", "a"}, {"transitions", Json::array({{{"to", "c"}, {"when", Json::array({{{"is", "arrived"}}})}}})}}})));
    GIANT_CHECK(refused(Json::array({{{"name", "a"}, {"transitions", Json::array({{{"to", "a"}, {"when", Json::array()}}})}}})));
    GIANT_CHECK(refused(one({{"is", "soon"}})) && refused(one({{"value", 1}})));
    GIANT_CHECK(refused(one({{"is", "alarm_over"}})) && refused(one({{"is", "alarm_over"}, {"value", 0}})) && refused(one({{"is", "alarm_under"}, {"value", 1.5}})));
    GIANT_CHECK(refused(one({{"is", "alarm_over"}, {"value", 0.5}, {"kind", "shot"}})));
    GIANT_CHECK(refused(one({{"is", "sensed"}})) && refused(one({{"is", "sensed"}, {"kind", "Shot"}})) && refused(one({{"is", "sensed"}, {"kind", "gunshot"}})) && refused(one({{"is", "sensed"}, {"kind", "shot"}, {"value", 1}})));
    GIANT_CHECK(refused(one({{"is", "arrived"}, {"value", 1}})) && refused(one({{"is", "failed"}, {"until", 1}})));
    GIANT_CHECK(refused(one({{"is", "after"}})) && refused(one({{"is", "after"}, {"value", 0}})) && refused(one({{"is", "after"}, {"value", 90000}})));
    GIANT_CHECK(refused(one({{"is", "hour"}, {"value", 22}})) && refused(one({{"is", "hour"}, {"until", 6}})) &&
                refused(one({{"is", "hour"}, {"value", 24}, {"until", 6}})) && refused(one({{"is", "hour"}, {"value", 6}, {"until", 6}})));
    GIANT_CHECK(!refused(one({{"is", "hour"}, {"value", 22}, {"until", 0}})) && f.run("undo").ok);
    GIANT_CHECK(scene.behaviours.size() == 1);
    // Given to people when made and after; one not there is refused.
    r = f.run("create_people", {{"people", {{{"look", "villager"}, {"home", "inn"}, {"behaviour", "wary"}}, {{"look", "villager"}, {"home", "inn"}}}}});
    GIANT_CHECK(r.ok && scene.people[0].behaviour == "wary" && scene.people[1].behaviour.empty());
    const world::PersonId second = r.value["ids"][1].get<world::PersonId>();
    GIANT_CHECK(!f.run("set_person", {{"id", second}, {"behaviour", "bold"}}).ok);
    GIANT_CHECK(!f.run("create_people", {{"people", {{{"look", "villager"}, {"home", "inn"}, {"behaviour", "bold"}}}}}).ok);
    r = f.run("set_person", {{"id", second}, {"behaviour", "wary"}});
    GIANT_CHECK(r.ok && r.value["behaviour"] == "wary" && scene.people[1].behaviour == "wary");
    GIANT_CHECK(f.run("set_person", {{"id", second}, {"behaviour", ""}}).ok && scene.people[1].behaviour.empty());
    GIANT_CHECK(f.run("undo").ok && scene.people[1].behaviour == "wary");
    r = f.run("get_people", {{"behaviour", "wary"}});
    GIANT_CHECK(r.ok && r.value["count"] == 2 && r.value["people"][0]["behaviour"] == "wary");
    GIANT_CHECK(f.run("get_people", {{"behaviour", "bold"}}).value["count"] == 0);
    GIANT_CHECK(f.run("get_people", {{"text", "WAR"}}).value["count"] == 2); // (its behaviour is among what text looks in)
    GIANT_CHECK(f.run("get_behaviours").value["behaviours"][0]["people"] == 2);
    // Replaced whole; undone and done again.
    r = f.run("set_behaviour", {{"name", "wary"}, {"states", Json::array({{{"name", "only"}}})}});
    GIANT_CHECK(r.ok && r.value["people"] == 2 && scene.behaviours.size() == 1 && scene.behaviours[0].states.size() == 1);
    GIANT_CHECK(f.run("undo").ok && scene.behaviours[0].states.size() == 4);
    GIANT_CHECK(f.run("redo").ok && scene.behaviours[0].states.size() == 1);
    GIANT_CHECK(f.run("undo").ok);
    // Kept while people have it; removed once they have not; undo brings it back.
    GIANT_CHECK(!f.run("remove_behaviour", {{"name", "wary"}}).ok && !f.run("remove_behaviour", {{"name", "bold"}}).ok);
    GIANT_CHECK(f.run("set_person", {{"id", second}, {"behaviour", ""}}).ok);
    GIANT_CHECK(f.run("set_person", {{"id", scene.people[0].id}, {"behaviour", ""}}).ok);
    GIANT_CHECK(f.run("remove_behaviour", {{"name", "wary"}}).ok && scene.behaviours.empty());
    GIANT_CHECK(f.run("get_behaviours").value["behaviours"].empty());
    GIANT_CHECK(f.run("undo").ok && scene.behaviours.size() == 1 && scene.behaviours[0].states.size() == 4);
    GIANT_CHECK(f.run("undo").ok && scene.people[0].behaviour == "wary");
    GIANT_CHECK(!world::check_behaviours(scene.behaviours, scene.people));
    // People removed, then their behaviour taken away outside the history (a script): undo brings them back without it.
    const world::PersonId first = scene.people[0].id;
    GIANT_CHECK(f.run("remove_people", {{"ids", {first}}}).ok && scene.people.size() == 1);
    ai::ExecuteOptions outside;
    outside.record_history = false;
    GIANT_CHECK(f.commands.execute("remove_behaviour", {{"name", "wary"}}, outside).ok && scene.behaviours.empty());
    GIANT_CHECK(f.run("undo").ok && scene.people.size() == 2 && scene.people[0].id == first && scene.people[0].behaviour.empty());
    GIANT_CHECK(!world::check_behaviours(scene.behaviours, scene.people));
    // One removed, another of its name made outside the history: undo leaves that one be.
    GIANT_CHECK(f.run("set_behaviour", {{"name", "wary"}, {"states", states}}).ok && f.run("remove_behaviour", {{"name", "wary"}}).ok);
    GIANT_CHECK(f.commands.execute("set_behaviour", {{"name", "wary"}, {"states", Json::array({{{"name", "only"}}})}}, outside).ok);
    GIANT_CHECK(f.run("undo").ok && scene.behaviours.size() == 1 && scene.behaviours[0].states.size() == 1);
    // As many as a scene keeps, one removed, one more made outside the history: undo does not put a 257th back.
    scene.behaviours.clear();
    for (usize i = 0; i < world::k_max_behaviours; ++i) {
        world::Behaviour filler;
        filler.name = "b" + std::to_string(i);
        filler.states.resize(1);
        filler.states[0].name = "only";
        scene.behaviours.push_back(filler);
    }
    GIANT_CHECK(!f.run("set_behaviour", {{"name", "one more"}, {"states", states}}).ok);
    GIANT_CHECK(f.run("set_behaviour", {{"name", "b9"}, {"states", states}}).ok && f.run("undo").ok); // (one there may be replaced)
    GIANT_CHECK(f.run("remove_behaviour", {{"name", "b5"}}).ok && scene.behaviours.size() == world::k_max_behaviours - 1);
    GIANT_CHECK(f.commands.execute("set_behaviour", {{"name", "one more"}, {"states", states}}, outside).ok);
    GIANT_CHECK(f.run("undo").ok && scene.behaviours.size() == world::k_max_behaviours);
    GIANT_CHECK(!world::check_behaviours(scene.behaviours, scene.people));
    // The place a state names may go: the behaviour is kept, and the scene still loads, with who has what.
    GIANT_CHECK(f.run("set_person", {{"id", first}, {"behaviour", "one more"}}).ok);
    for (world::Person& p : scene.people) p.home.clear();
    GIANT_CHECK(f.run("remove_place", {{"name", "inn"}}).ok);
    world::Scene loaded;
    GIANT_CHECK(!world::load_scene(world::save_scene(scene), loaded).has_value() && loaded.behaviours == scene.behaviours &&
                loaded.people == scene.people);
}

GIANT_TEST("people near the camera are given entities from their looks, a few a frame, and lose them far off") {
    Fixture f;
    PeopleState people;
    f.state.people = &people;
    world::Scene& scene = f.state.scene;
    GIANT_CHECK(f.run("set_person_look", {{"name", "villager"}, {"entity", f.model}}).ok);
    GIANT_CHECK(f.run("delete_entities", {{"ids", {f.model}}}).ok && scene.size() == 0);
    // Ten in a row 10 m apart, and one named, facing east.
    Json batch = Json::array();
    for (int i = 0; i < 10; ++i) batch.push_back({{"look", "villager"}, {"position", at(10.0 * i, 0, 0)}});
    batch.push_back({{"look", "villager"}, {"position", at(5, 2, 5)}, {"name", "Ada"}, {"yaw", 90.0}});
    GIANT_CHECK(f.run("create_people", {{"people", batch}}).ok);
    GIANT_CHECK(f.run("set_people_settings", {{"embody_distance", 25.0}, {"release_distance", 35.0}}).ok);
    GIANT_CHECK(!f.run("set_people_settings", {{"release_distance", 5.0}}).ok);
    const DVec3 camera{0.0, 0.0, 0.0};
    const world::EntityId before_id = scene.next_id();
    const std::string before_text = world::save_scene(scene);
    people.update(scene, camera, nullptr);
    GIANT_CHECK(scene.size() == 2 && people.stats().embodied == 2); // two a frame
    people.update(scene, camera, nullptr);
    people.update(scene, camera, nullptr);
    GIANT_CHECK(scene.size() == 4 && people.stats().embodied == 4 && people.stats().made == 4); // 0, 10, 20 m and Ada
    // Made from the look, where the person stands, carrying its id.
    const world::EntityId ada = people.entity_of(scene, 11);
    const world::Entity* e = scene.find(ada);
    GIANT_CHECK(e != nullptr && e->person == 11 && e->name == "Ada" && e->mesh == "people/villager.glb");
    GIANT_CHECK(ada == world::person_entity_id(11) && scene.next_id() == before_id); // its person's own id: none of the scene's used
    GIANT_CHECK(e->shape == world::Shape::mesh && e->transform.scale.y == 1.2f && e->material.roughness == 0.8f &&
                e->parts.size() == 1 && e->hair.on && e->animation.matched.size() == 2 && e->body == world::BodyKind::none);
    GIANT_CHECK(e->transform.position.x == 5.0 && e->transform.position.y == 2.0 && std::fabs(e->transform.rotation.y - 0.70710678f) < 1e-5f);
    GIANT_CHECK(scene.find(people.entity_of(scene, 1))->name == "person 1" && people.entity_of(scene, 5) == 0);
    ai::CommandResult r = f.run("get_people", {{"ids", {11, 5}}});
    GIANT_CHECK(r.ok && r.value["people"][0]["entity"] == ada && r.value["people"][1]["entity"].is_null());
    // Given a routine, its entity is kept (nothing it shows changed); undone too.
    GIANT_CHECK(f.run("set_routine", {{"name", "day"}, {"entries", Json::array({{{"from", 8}, {"activity", "work"}}})}}).ok);
    GIANT_CHECK(f.run("set_person", {{"id", 11}, {"routine", "day"}}).ok);
    people.update(scene, camera, nullptr);
    GIANT_CHECK(people.entity_of(scene, 11) == ada && people.stats().made == 4 && people.stats().released == 0);
    GIANT_CHECK(f.run("undo").ok && f.run("undo").ok);
    people.update(scene, camera, nullptr);
    GIANT_CHECK(people.entity_of(scene, 11) == ada && people.stats().made == 4 && scene.routines.empty());
    // Given a behaviour likewise.
    GIANT_CHECK(f.run("set_behaviour", {{"name", "still"}, {"states", Json::array({{{"name", "only"}}})}}).ok);
    GIANT_CHECK(f.run("set_person", {{"id", 11}, {"behaviour", "still"}}).ok);
    people.update(scene, camera, nullptr);
    GIANT_CHECK(people.entity_of(scene, 11) == ada && people.stats().made == 4 && people.stats().released == 0);
    GIANT_CHECK(f.run("undo").ok && f.run("undo").ok);
    people.update(scene, camera, nullptr);
    GIANT_CHECK(people.entity_of(scene, 11) == ada && people.stats().made == 4 && scene.behaviours.empty());
    GIANT_CHECK(f.run("get_entity", {{"id", ada}}).value["person"] == 11);
    GIANT_CHECK(!f.run("set_entity", {{"id", ada}, {"name", "Eve"}}).ok); // the person is what to change
    // Never saved: the person is.
    world::Scene loaded;
    GIANT_CHECK(world::save_scene(scene) == before_text && scene.authored() == 0); // looking round changes nothing saved
    GIANT_CHECK(!world::load_scene(world::save_scene(scene), loaded).has_value() && loaded.size() == 0 && loaded.people.size() == 11);
    // Entities made meanwhile go before them; one deleted comes back where it was whoever is near.
    const ai::CommandResult post = f.run("create_entity", {{"shape", "cube"}, {"name", "post"}});
    const ai::CommandResult gate = f.run("create_entity", {{"shape", "cube"}, {"name", "gate"}});
    GIANT_CHECK(post.ok && gate.ok && scene.authored() == 2 && scene.entities()[0].name == "post" && scene.entities()[2].person != 0);
    GIANT_CHECK(f.run("delete_entities", {{"ids", {post.value["id"]}}}).ok);
    // The camera moves on: those behind go once past the release distance, those ahead come.
    const DVec3 on{60.0, 0.0, 0.0};
    for (int i = 0; i < 6; ++i) people.update(scene, on, nullptr);
    GIANT_CHECK(people.entity_of(scene, 11) == 0 && people.entity_of(scene, 1) == 0 && scene.find(ada) == nullptr);
    GIANT_CHECK(f.run("undo").ok && scene.entities()[0].name == "post" && scene.entities()[1].name == "gate");
    GIANT_CHECK(f.run("delete_entities", {{"ids", {post.value["id"], gate.value["id"]}}}).ok && scene.authored() == 0);
    GIANT_CHECK(people.stats().embodied == 5 && scene.size() == 5); // 40 to 80 m
    GIANT_CHECK(people.entity_of(scene, 4) == 0 && people.entity_of(scene, 5) != 0 && people.entity_of(scene, 9) != 0 && people.entity_of(scene, 10) == 0);
    // A person changed is made again as it is now; one removed goes.
    const world::EntityId five = people.entity_of(scene, 5);
    GIANT_CHECK(f.run("set_person", {{"id", 5}, {"name", "Bo"}}).ok && f.run("remove_people", {{"ids", {6}}}).ok);
    people.update(scene, on, nullptr);
    people.update(scene, on, nullptr);
    GIANT_CHECK(people.entity_of(scene, 5) == five && scene.find(five)->name == "Bo"); // the same id every time
    GIANT_CHECK(people.entity_of(scene, 6) == 0 && people.stats().embodied == 4);
    // An entity deleted by hand is made again; undoing the deletion leaves one, not two.
    const world::EntityId seven = people.entity_of(scene, 7);
    GIANT_CHECK(f.run("delete_entities", {{"ids", {seven}}}).ok);
    people.update(scene, on, nullptr);
    people.update(scene, on, nullptr);
    GIANT_CHECK(people.entity_of(scene, 7) == seven && scene.size() == 4);
    GIANT_CHECK(f.run("undo").ok && scene.size() == 4);
    people.update(scene, on, nullptr);
    GIANT_CHECK(scene.size() == 4 && scene.find(seven) != nullptr);
    // Deleted and asked about before the next frame: not there. Its person removed outside the
    // history, the deletion undone: a stray, taken away.
    GIANT_CHECK(f.run("delete_entities", {{"ids", {seven}}}).ok && people.entity_of(scene, 7) == 0);
    GIANT_CHECK(f.run("get_people", {{"ids", {7}}}).value["people"][0]["entity"].is_null());
    ai::ExecuteOptions unrecorded;
    unrecorded.record_history = false;
    GIANT_CHECK(f.commands.execute("remove_people", {{"ids", {7}}}, unrecorded).ok);
    people.update(scene, on, nullptr);
    GIANT_CHECK(scene.size() == 3 && f.run("undo").ok && scene.size() == 4 && scene.find(seven) != nullptr);
    people.update(scene, on, nullptr);
    GIANT_CHECK(scene.size() == 3 && scene.find(seven) == nullptr);
    // A look changed: its people are made again from it.
    GIANT_CHECK(f.run("set_person_look", {{"name", "villager"}, {"height", 0.9}}).ok);
    for (int i = 0; i < 4; ++i) people.update(scene, on, nullptr);
    GIANT_CHECK(people.stats().embodied == 3 && scene.find(people.entity_of(scene, 8))->transform.scale.x == 0.9f);
    // No more than `most` (lowered: some go at once); the rest wait. Off: all go.
    GIANT_CHECK(f.run("set_people_settings", {{"most", 2}}).ok);
    people.update(scene, on, nullptr);
    GIANT_CHECK(scene.size() == 2);
    for (int i = 0; i < 4; ++i) people.update(scene, on, nullptr);
    r = f.run("set_people_settings");
    GIANT_CHECK(scene.size() == 2 && r.value["embodied"] == 2 && r.value["waiting"] == 1);
    GIANT_CHECK(f.run("set_people_settings", {{"on", false}}).ok);
    people.update(scene, on, nullptr);
    GIANT_CHECK(scene.size() == 0 && people.stats().embodied == 0);
    // Held (a loaded game's people are being put back: game_saves.h): nobody has an entity until let go.
    GIANT_CHECK(f.run("set_people_settings", {{"on", true}, {"most", 256}}).ok);
    for (int i = 0; i < 4; ++i) people.update(scene, on, nullptr);
    GIANT_CHECK(scene.size() == 3);
    people.hold = true;
    for (int i = 0; i < 2; ++i) people.update(scene, on, nullptr);
    GIANT_CHECK(scene.size() == 0 && people.stats().embodied == 0);
    people.hold = false;
    for (int i = 0; i < 4; ++i) people.update(scene, on, nullptr);
    GIANT_CHECK(scene.size() == 3);
    GIANT_CHECK(f.run("set_people_settings", {{"on", false}}).ok);
    people.update(scene, on, nullptr);
    GIANT_CHECK(scene.size() == 0);
    // Another scene (a copy taken while they stood): nothing held, strays gone.
    GIANT_CHECK(f.run("set_people_settings", {{"on", true}, {"most", 256}}).ok);
    for (int i = 0; i < 4; ++i) people.update(scene, on, nullptr);
    GIANT_CHECK(scene.size() == 3);
    world::Scene copy(scene);
    scene = std::move(copy);
    people.update(scene, on, nullptr);
    GIANT_CHECK(scene.size() == 2 && people.stats().embodied == 2);
    for (const world::Entity& x : scene.entities()) GIANT_CHECK(people.entity_of(scene, x.person) == x.id);
    // A look made (their number changes): everyone is made again; the same scene cleared: they come back.
    for (int i = 0; i < 4; ++i) people.update(scene, on, nullptr);
    GIANT_CHECK(scene.size() == 3);
    world::PersonLook other = scene.person_looks[0];
    other.name = "miller";
    scene.person_looks.push_back(other);
    people.update(scene, on, nullptr);
    GIANT_CHECK(scene.size() == 2 && people.stats().embodied == 2);
    people.update(scene, on, nullptr);
    scene.clear();
    for (int i = 0; i < 3; ++i) people.update(scene, on, nullptr);
    GIANT_CHECK(scene.size() == 3 && people.stats().embodied == 3);
}

GIANT_TEST("people near the camera: a long list is gone through over frames, on the ground, every clip matched") {
    Fixture f;
    PeopleState people;
    f.state.people = &people;
    world::Scene& scene = f.state.scene;
    scene.modify(f.model)->animation.matched.clear(); // a look with no list: every clip
    GIANT_CHECK(f.run("set_person_look", {{"name", "villager"}, {"entity", f.model}}).ok);
    GIANT_CHECK(f.run("delete_entities", {{"ids", {f.model}}}).ok);
    // 3000 far off, then three near at the end of the list.
    Json batch = Json::array();
    for (int i = 0; i < 3000; ++i) batch.push_back({{"look", "villager"}, {"position", at(5000.0 + i, 0, 0)}});
    for (int i = 0; i < 3; ++i) batch.push_back({{"look", "villager"}, {"position", at(1.0 + i, 7.0, 1.0)}});
    GIANT_CHECK(f.run("create_people", {{"people", batch}}).ok);
    // Ground under them at 2.5 m: they stand on it, not at the height they were given.
    world::Terrain::Heights heights(static_cast<usize>(scene.terrain.samples()) * scene.terrain.samples(), 2.5f);
    scene.terrain.set({0, 0}, std::move(heights));
    people.settings.most = 2;
    const DVec3 camera{0.0, 0.0, 0.0};
    people.update(scene, camera, nullptr); // 1 to 1024
    people.update(scene, camera, nullptr); // to 2048
    GIANT_CHECK(scene.size() == 0 && people.stats().embodied == 0);
    people.update(scene, camera, nullptr); // to 3003: two made, one over `most`
    GIANT_CHECK(scene.size() == 2 && people.stats().waiting == 0);
    people.update(scene, camera, nullptr); // from the start again: the count of the last time through is told
    GIANT_CHECK(people.stats().waiting == 1 && people.stats().embodied == 2);
    const world::Entity* e = scene.find(world::person_entity_id(3001));
    GIANT_CHECK(e != nullptr && e->transform.position.y == 2.5 && e->animation.matched == std::vector<std::string>{"*"});
    // The list shrinks under the cursor: nothing is read past its end.
    Json ids = Json::array();
    for (int id = 1; id <= 3000; ++id) ids.push_back(id);
    GIANT_CHECK(f.run("remove_people", {{"ids", ids}}).ok);
    people.settings.most = 256;
    for (int i = 0; i < 3; ++i) people.update(scene, camera, nullptr);
    GIANT_CHECK(scene.size() == 3 && people.stats().embodied == 3 && people.stats().waiting == 0);
}

GIANT_TEST("people commands: a look's mesh is one of the scene's, spelt one way when loaded") {
    Fixture f;
    world::PersonLook look;
    look.name = "villager";
    look.mesh = "./people\\villager.glb";
    f.state.scene.person_looks.push_back(look);
    const std::string path = (std::filesystem::temp_directory_path() / "giant_people_look.scene.json").generic_string();
    GIANT_CHECK(f.run("save_scene", {{"path", path}}).ok);
    const ai::CommandResult loaded = f.run("load_scene", {{"path", path}});
    GIANT_CHECK(loaded.ok && f.state.scene.person_looks.size() == 1);
    GIANT_CHECK(f.state.scene.person_looks[0].mesh == MeshLibrary::normalize("./people\\villager.glb"));
    GIANT_CHECK(f.state.scene.person_looks[0].mesh == "people/villager.glb");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

GIANT_TEST("people near the camera: nobody appears or goes close by in view; those looked towards are made far off") {
    Fixture f;
    PeopleState people;
    PeopleTravel travel;
    people.travel = &travel;
    f.state.people = &people;
    world::Scene& scene = f.state.scene;
    GIANT_CHECK(f.run("set_person_look", {{"name", "villager"}, {"entity", f.model}}).ok);
    GIANT_CHECK(f.run("delete_entities", {{"ids", {f.model}}}).ok && scene.size() == 0);
    // The view of a camera: the way it looks, and the angle to its picture's corner.
    PeopleState::View west = PeopleState::view_of(90.0f, 0.0f, 60.0f, 16.0f / 9.0f);
    GIANT_CHECK(west.on && std::fabs(west.forward.x + 1.0f) < 1e-5f && std::fabs(west.forward.z) < 1e-5f);
    GIANT_CHECK(std::fabs(west.half_angle - 0.8668f) < 1e-3f);
    GIANT_CHECK(std::fabs(PeopleState::view_of(0.0f, 90.0f, 60.0f, 1.0f).forward.y - 1.0f) < 1e-5f);
    const PeopleState::View up = PeopleState::view_of(0.0f, 45.0f, 60.0f, 1.0f);
    GIANT_CHECK(std::fabs(up.forward.y - 0.70711f) < 1e-4f && std::fabs(up.forward.z + 0.70711f) < 1e-4f);
    const PeopleState::View north = PeopleState::view_of(0.0f, 0.0f, 60.0f, 16.0f / 9.0f); // down -Z
    const PeopleState::View south = PeopleState::view_of(180.0f, 0.0f, 60.0f, 16.0f / 9.0f);
    GIANT_CHECK(std::fabs(north.forward.z + 1.0f) < 1e-5f && std::fabs(south.forward.z - 1.0f) < 1e-5f);
    people.view = north;
    DVec3 camera{0.0, 0.0, 0.0};
    u64 run = 0;
    const auto frames = [&](int count = 4) {
        for (int i = 0; i < count; ++i) {
            travel.update(scene, run, 1.0);
            people.update(scene, camera, nullptr);
        }
    };
    const auto has = [&](world::PersonId id) { return people.entity_of(scene, id) != 0; };
    const auto make = [&](std::initializer_list<double> zs) {
        Json batch = Json::array();
        for (const double z : zs) batch.push_back({{"look", "villager"}, {"position", at(0, 0, z)}});
        GIANT_CHECK(f.run("create_people", {{"people", batch}}).ok);
    };
    frames();
    // The world stopped: one made in front of the camera is there at once (an edit's doing, nobody walked in).
    make({-20.0});
    frames();
    GIANT_CHECK(has(1) && people.stats().unseen == 0);
    // The world running (everyone put back: once through the list as it is). From here what is seen counts.
    run = 1;
    frames();
    // 2: near, ahead. 3: near, behind. 4: far ahead. 5: far behind. 6: farther ahead than anyone is
    // made. 7: ahead, just within embody_distance (one that crossed it before its turn came).
    make({-30.0, 30.0, -150.0, 150.0, -400.0, -55.0});
    frames();
    // The one near and in view waits to be out of sight; the one behind is made; the one far ahead too.
    GIANT_CHECK(has(1) && !has(2) && has(3) && has(4) && !has(5) && !has(6) && has(7) && people.stats().unseen == 1);
    ai::CommandResult r = f.run("set_people_settings");
    GIANT_CHECK(r.ok && r.value["unseen"] == 1 && r.value["view_distance"] == 200.0 && r.value["embodied"] == 4);
    // Turned round: the one that waited is made (unseen now), the one far behind goes (unseen), the one far ahead is made.
    people.view = south;
    frames();
    GIANT_CHECK(has(2) && has(3) && !has(4) && has(5) && people.stats().unseen == 0);
    // Turned back: the near ones stay (they have theirs), the far ones change places.
    people.view = north;
    frames();
    GIANT_CHECK(has(2) && has(3) && has(4) && !has(5));
    // Its entity deleted while looked at, itself edited: made again where it stands (it was there
    // to be seen). Only it: another near and in view (8, made while the world runs) still waits.
    make({-25.0});
    frames();
    GIANT_CHECK(!has(8) && people.stats().unseen == 1);
    const u64 made_before = people.stats().made;
    GIANT_CHECK(f.run("delete_entities", {{"ids", {people.entity_of(scene, 2)}}}).ok);
    frames();
    GIANT_CHECK(has(2) && !has(8) && people.stats().made == made_before + 1);
    GIANT_CHECK(f.run("set_person", {{"id", 2}, {"name", "Ada"}}).ok);
    frames();
    GIANT_CHECK(has(2) && !has(8) && scene.find(people.entity_of(scene, 2))->name == "Ada");
    // Fewer may be held, then as many again: those that had to go come back, seen or not (and
    // with them the one that waited: everyone near, once).
    people.settings.most = 1;
    frames();
    GIANT_CHECK(people.stats().embodied == 1);
    people.settings.most = 256;
    frames();
    GIANT_CHECK(has(2) && has(3) && has(7) && has(8) && has(4));
    // Walking backwards from one in view: it stays past view_distance by the gap between the two
    // distances (200 + 20), then goes; it is not made again until within view_distance.
    camera.z = 30.0;
    frames();
    camera.z = 65.0; // 215 m from 4
    frames();
    GIANT_CHECK(has(4));
    camera.z = 75.0; // 225 m
    frames();
    GIANT_CHECK(!has(4));
    camera.z = 55.0; // 205 m
    frames();
    GIANT_CHECK(!has(4));
    camera.z = 45.0; // 195 m
    frames();
    GIANT_CHECK(has(4));
    // The camera put somewhere else (a load, a teleport): those near are made though seen, once.
    GIANT_CHECK(!has(6));
    camera.z = -370.0; // 30 m from 6, looking at it
    frames();
    GIANT_CHECK(has(6) && people.stats().unseen == 0);
    // Held back (a loaded game's people being put back) and let go: the same.
    people.hold = true;
    frames(1);
    GIANT_CHECK(!has(6));
    people.hold = false;
    frames();
    GIANT_CHECK(has(6));
    // A shorter view_distance by command: nobody far ahead is made.
    GIANT_CHECK(f.run("set_people_settings", {{"view_distance", 60.0}}).ok && people.settings.view_distance == 60.0f);
    GIANT_CHECK(!f.run("set_people_settings", {{"view_distance", 5000.0}}).ok);
    camera.z = -340.0;
    frames();
    camera.z = -310.0;
    frames();
    camera.z = -280.0; // 4 is 130 m ahead
    frames();
    GIANT_CHECK(!has(4));
    // No view given: as before, by distance alone.
    people.view = PeopleState::View{};
    camera.z = -185.0; // 35 m from 4, which is ahead
    frames();
    GIANT_CHECK(has(4));
}

GIANT_TEST("people near the camera: with all the entities there may be held, the farthest gives its up for one nearer") {
    Fixture f;
    PeopleState people;
    f.state.people = &people;
    world::Scene& scene = f.state.scene;
    GIANT_CHECK(f.run("set_person_look", {{"name", "villager"}, {"entity", f.model}}).ok);
    GIANT_CHECK(f.run("delete_entities", {{"ids", {f.model}}}).ok);
    people.view = PeopleState::view_of(0.0f, 0.0f, 60.0f, 16.0f / 9.0f);
    people.settings.most = 2;
    const DVec3 camera{0.0, 0.0, 0.0};
    const auto has = [&](world::PersonId id) { return people.entity_of(scene, id) != 0; };
    const auto frames = [&] {
        for (int i = 0; i < 6; ++i) people.update(scene, camera, nullptr);
    };
    const auto make = [&](double x, double z) {
        GIANT_CHECK(f.run("create_people", {{"people", {{{"look", "villager"}, {"position", at(x, 0, z)}}}}}).ok);
    };
    frames();
    // Two far ahead take both. One 110 m ahead: not 20 m nearer than either, it waits.
    make(0, -100);
    make(0, -120);
    make(0, -110);
    frames();
    GIANT_CHECK(has(1) && has(2) && !has(3) && people.stats().swapped == 0 && people.stats().waiting == 1);
    // One 70 m ahead: the farthest (120 m) gives its up. Nothing goes back and forth.
    make(0, -70);
    frames();
    GIANT_CHECK(has(1) && !has(2) && !has(3) && has(4) && people.stats().swapped == 1 && people.stats().waiting == 2);
    // One near, behind the camera: the farthest beyond embody_distance (100 m) gives its up.
    make(0, 20);
    frames();
    GIANT_CHECK(!has(1) && has(4) && has(5) && people.stats().swapped == 2 && scene.size() == 2);
    // Two more near: one takes the 70 m one's; the other finds nobody beyond embody_distance and waits.
    make(5, 20);
    make(-5, 20);
    frames();
    GIANT_CHECK(!has(4) && has(5) && has(6) && !has(7) && people.stats().swapped == 3 && scene.size() == 2);
    GIANT_CHECK(people.stats().embodied == 2 && people.stats().waiting == 5);
}

GIANT_TEST("people near the camera: one near gets its entity however many far ones wait before it in the list") {
    Fixture f;
    PeopleState people;
    f.state.people = &people;
    world::Scene& scene = f.state.scene;
    GIANT_CHECK(f.run("set_person_look", {{"name", "villager"}, {"entity", f.model}}).ok);
    GIANT_CHECK(f.run("delete_entities", {{"ids", {f.model}}}).ok);
    people.view = PeopleState::view_of(0.0f, 0.0f, 60.0f, 16.0f / 9.0f);
    people.settings.most = 2;
    const DVec3 camera{0.0, 0.0, 0.0};
    const auto has = [&](world::PersonId id) { return people.entity_of(scene, id) != 0; };
    for (int i = 0; i < 4; ++i) people.update(scene, camera, nullptr);
    // Far ahead at 100, 120, 110, 115 and 112 m, and last in the list one near behind the camera.
    // The first two take both entities; the three after find nobody 20 m farther than themselves
    // each time through; the near one must still have the farthest's.
    Json batch = Json::array();
    for (const double z : {-100.0, -120.0, -110.0, -115.0, -112.0, 20.0}) batch.push_back({{"look", "villager"}, {"position", at(0, 0, z)}});
    GIANT_CHECK(f.run("create_people", {{"people", batch}}).ok);
    people.update(scene, camera, nullptr);
    GIANT_CHECK(has(1) && has(2) && !has(6)); // (two made a frame)
    for (int i = 0; i < 6; ++i) people.update(scene, camera, nullptr);
    GIANT_CHECK(has(1) && !has(2) && has(6) && !has(3) && !has(4) && !has(5) && people.stats().swapped == 1);
    GIANT_CHECK(scene.size() == 2 && people.stats().waiting == 4);
}

GIANT_TEST("people commands: a new person's id is never one of a mod's people's") {
    Fixture f;
    GIANT_CHECK(f.run("set_person_look", {{"name", "villager"}, {"entity", f.model}}).ok);
    world::Scene& scene = f.state.scene;
    // A mod's person is there (as when a world is opened with mods on): new people come before it, in order.
    world::Person of_mod;
    of_mod.id = world::mod_person_id(0, 1);
    of_mod.look = "villager";
    scene.people.push_back(of_mod);
    const Json one = {{{"look", "villager"}, {"position", at(0, 0, 0)}}};
    GIANT_CHECK(f.run("create_people", {{"people", one}}).ok && scene.people.size() == 2 && scene.people[0].id == 1 &&
                scene.people[1].id == of_mod.id && scene.next_person_id == 2);
    GIANT_CHECK(!world::check_people(scene.places, scene.person_looks, scene.routines, scene.people, scene.next_person_id));
    // The last id of the world's own can be given; the first of the mods' cannot.
    scene.next_person_id = world::k_mod_person_base - 1;
    GIANT_CHECK(!f.run("create_people", {{"people", {one[0], one[0]}}}).ok && scene.people.size() == 2);
    GIANT_CHECK(f.run("create_people", {{"people", one}}).ok && scene.people.size() == 3 && scene.people[1].id == world::k_mod_person_base - 1 &&
                scene.next_person_id == world::k_mod_person_base);
    GIANT_CHECK(!world::check_people(scene.places, scene.person_looks, scene.routines, scene.people, scene.next_person_id));
    GIANT_CHECK(!f.run("create_people", {{"people", one}}).ok && scene.people.size() == 3);
}
