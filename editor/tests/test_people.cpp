// People in the editor: places, person looks and people made, changed,
// listed and removed by command (undoable, saved with the scene).
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"

#include "giant_test.h"

#include "giant/world/people.h"

#include <filesystem>
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
    GIANT_CHECK(!world::check_people(scene.places, scene.person_looks, scene.people, scene.next_person_id));
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
    GIANT_CHECK(!world::check_people(scene.places, scene.person_looks, scene.people, scene.next_person_id));
    // Every other one of a stretch removed, and one in the middle changed.
    Json ids = Json::array();
    for (int id = 1000; id < 9000; id += 2) ids.push_back(id);
    GIANT_CHECK(f.run("remove_people", {{"ids", ids}}).ok && scene.people.size() == world::k_max_people - 4000);
    GIANT_CHECK(world::find_person(scene.people, 1000) == nullptr && world::find_person(scene.people, 1001) != nullptr);
    GIANT_CHECK(f.run("set_person", {{"id", 30000}, {"yaw", 12.0}}).ok && world::find_person(scene.people, 30000)->yaw == 12.0f);
    GIANT_CHECK(f.run("get_people", {{"near", at(3000, 0, 3)}, {"within", 10.0}, {"limit", 0}}).value["count"].get<int>() > 0);
    GIANT_CHECK(f.run("undo").ok && f.run("undo").ok && scene.people.size() == world::k_max_people);
    GIANT_CHECK(!world::check_people(scene.places, scene.person_looks, scene.people, scene.next_person_id));
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
    GIANT_CHECK(!world::check_people(scene.places, scene.person_looks, scene.people, scene.next_person_id));
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
    GIANT_CHECK(scene.places.size() == 2 && !world::check_people(scene.places, scene.person_looks, scene.people, scene.next_person_id));
    // People removed, then their home and look taken away outside the history: undo brings back
    // only what the scene can keep.
    GIANT_CHECK(f.run("create_people", {{"people", {{{"look", "villager"}, {"home", "mill"}}}}}).ok);
    GIANT_CHECK(f.run("remove_people", {{"ids", {1, 2}}}).ok && scene.people.empty());
    GIANT_CHECK(script("remove_place", {{"name", "mill"}}));
    GIANT_CHECK(f.run("undo").ok && scene.people.size() == 2 && scene.people[0].home == "inn" && scene.people[1].home.empty());
    GIANT_CHECK(!world::check_people(scene.places, scene.person_looks, scene.people, scene.next_person_id));
    GIANT_CHECK(f.run("redo").ok && scene.people.empty() && script("remove_person_look", {{"name", "villager"}}));
    GIANT_CHECK(f.run("undo").ok && scene.people.empty()); // their look is gone: they stay away
    world::Scene loaded;
    GIANT_CHECK(!world::load_scene(world::save_scene(scene), loaded).has_value());
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
