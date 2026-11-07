// Physics in the editor: entities with bodies, simulating like a play
// button (things fall and rest; stopping puts them back), raycasts.
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"
#include "../src/physics_state.h"

#include "giant_test.h"

#include <cmath>

using namespace giant;
using namespace giant::editor;
using ai::Json;

namespace {

struct Fixture {
    ai::CommandRegistry commands;
    MeshLibrary meshes{nullptr};
    PhysicsState physics;
    EditorState state;
    Fixture() {
        state.meshes = &meshes;
        state.physics = &physics;
        register_scene_commands(commands, state);
        register_physics_commands(commands, state);
    }
    ai::CommandResult run(const char* name, const Json& args = Json::object()) {
        return commands.execute(name, args, {});
    }
    world::EntityId create(const Json& args) {
        const ai::CommandResult r = run("create_entity", args);
        GIANT_CHECK(r.ok);
        return r.ok ? r.value["id"].get<world::EntityId>() : 0;
    }
    // Where it is drawn: where simulating moved it, or where the scene has it.
    DVec3 at(world::EntityId id) {
        const Json e = run("get_entity", {{"id", id}}).value;
        const Json& p = e.contains("simulated") ? e["simulated"]["position"] : e["position"];
        return {p[0].get<double>(), p[1].get<double>(), p[2].get<double>()};
    }
    double y(world::EntityId id) { return at(id).y; }
};

} // namespace

GIANT_TEST("physics commands: a cube falls and rests, stopping puts it back, edits follow") {
    Fixture f;
    f.create({{"shape", "plane"}, {"scale", {100, 1, 100}}});
    const world::EntityId cube =
        f.create({{"shape", "cube"}, {"position", {0, 5, 0}}, {"body", "dynamic"}, {"mass", 10}});
    GIANT_CHECK(f.run("get_entity", {{"id", cube}}).value["body"] == "dynamic");
    // Not simulating: nothing to step.
    GIANT_CHECK(!f.run("step_physics", {{"seconds", 1}}).ok);
    ai::CommandResult r = f.run("simulate", {{"on", true}});
    GIANT_CHECK(r.ok && r.value["simulating"] == true);
    r = f.run("step_physics", {{"seconds", 3}});
    GIANT_CHECK(r.ok && r.value["stepped"] == 180 && r.value["entity_bodies"] == 2 && r.value["dynamic_bodies"] == 1);
    GIANT_CHECK(std::fabs(f.y(cube) - 0.5) < 0.03 && r.value["moved"] == 1);
    // The scene keeps where it was put: nothing to undo or save from it.
    GIANT_CHECK(f.state.scene.find(cube)->transform.position == DVec3(0.0, 5.0, 0.0));
    // An edit while simulating: put there, falls from there.
    GIANT_CHECK(f.run("set_entity", {{"id", cube}, {"position", {10, 8, 0}}}).ok);
    GIANT_CHECK(f.at(cube) == DVec3(10.0, 8.0, 0.0));
    GIANT_CHECK(f.run("step_physics", {{"seconds", 3}}).ok);
    GIANT_CHECK(std::fabs(f.y(cube) - 0.5) < 0.03 && std::fabs(f.at(cube).x - 10.0) < 0.05);
    // Stopping: drawn where the scene has it (the edit's place).
    GIANT_CHECK(f.run("simulate", {{"on", false}}).ok);
    GIANT_CHECK(f.at(cube) == DVec3(10.0, 8.0, 0.0) && f.run("get_physics").value["moved"] == 0);
    GIANT_CHECK(f.run("get_physics").value["simulating"] == false);
    GIANT_CHECK(f.run("undo").ok && f.at(cube) == DVec3(0.0, 5.0, 0.0));
}

GIANT_TEST("physics commands: undo, new entities and saving while simulating keep where things were put") {
    Fixture f;
    f.create({{"shape", "plane"}, {"scale", {100, 1, 100}}});
    const world::EntityId cube = f.create({{"shape", "cube"}, {"position", {0, 5, 0}}, {"body", "dynamic"}});
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok && f.run("step_physics", {{"seconds", 0.5}}).ok);
    GIANT_CHECK(f.y(cube) < 4.5 && f.y(cube) > 1.0); // falling
    // A colour change mid-fall, then stop, then undo: back where it was put.
    GIANT_CHECK(f.run("set_entity", {{"id", cube}, {"base_color", {1, 0, 0}}}).ok);
    const world::EntityId late = f.create({{"shape", "sphere"}, {"position", {5, 6, 0}}, {"body", "dynamic"}});
    GIANT_CHECK(f.run("step_physics", {{"seconds", 3}}).ok && std::fabs(f.y(late) - 0.5) < 0.03);
    GIANT_CHECK(f.run("simulate", {{"on", false}}).ok);
    GIANT_CHECK(f.at(late) == DVec3(5.0, 6.0, 0.0)); // one added meanwhile goes back too
    GIANT_CHECK(f.run("undo").ok && f.run("undo").ok);
    GIANT_CHECK(f.at(cube) == DVec3(0.0, 5.0, 0.0));
    // Saved while simulating: where things were put.
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok && f.run("step_physics", {{"seconds", 3}}).ok);
    const std::string text = world::save_scene(f.state.scene);
    world::Scene loaded;
    GIANT_CHECK(!world::load_scene(text, loaded));
    bool found = false;
    for (const world::Entity& e : loaded.entities()) {
        if (e.body == world::BodyKind::dynamic) found = found || e.transform.position == DVec3(0.0, 5.0, 0.0);
    }
    GIANT_CHECK(found);
}

GIANT_TEST("physics commands: a support taken away lets what rested on it fall") {
    Fixture f;
    f.create({{"shape", "plane"}, {"scale", {100, 1, 100}}});
    const world::EntityId table = f.create({{"shape", "cube"}, {"position", {0, 1, 0}}, {"scale", {2, 2, 2}}});
    const world::EntityId crate = f.create({{"shape", "cube"}, {"position", {0, 2.5, 0}}, {"body", "dynamic"}});
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok && f.run("step_physics", {{"seconds", 4}}).ok);
    GIANT_CHECK(std::fabs(f.y(crate) - 2.5) < 0.03 && f.run("get_physics").value["active"] == 0);
    GIANT_CHECK(f.run("delete_entities", {{"ids", {table}}}).ok);
    GIANT_CHECK(f.run("step_physics", {{"seconds", 2}}).ok && std::fabs(f.y(crate) - 0.5) < 0.03);
}

GIANT_TEST("physics commands: raycasts hit entities, miss past max_distance") {
    Fixture f;
    const world::EntityId box = f.create({{"shape", "cube"}, {"position", {0, 10, 0}}, {"scale", {2, 2, 2}}});
    ai::CommandResult r = f.run("raycast", {{"origin", {0, 100, 0}}, {"direction", {0, -2, 0}}});
    GIANT_CHECK(r.ok && r.value["hit"] == true && r.value["entity"] == box);
    GIANT_CHECK(std::fabs(r.value["point"][1].get<double>() - 11.0) < 0.01 && std::fabs(r.value["distance"].get<double>() - 89.0) < 0.01);
    GIANT_CHECK(r.value["normal"][1].get<double>() > 0.99);
    r = f.run("raycast", {{"origin", {0, 100, 0}}, {"direction", {0, -1, 0}}, {"max_distance", 50}});
    GIANT_CHECK(r.ok && r.value["hit"] == false);
    GIANT_CHECK(!f.run("raycast", {{"origin", {0, 100, 0}}, {"direction", {0, 0, 0}}}).ok);
    // Body none: rays pass through.
    GIANT_CHECK(f.run("set_entity", {{"id", box}, {"body", "none"}}).ok);
    r = f.run("raycast", {{"origin", {0, 100, 0}}, {"direction", {0, -1, 0}}, {"max_distance", 95}});
    GIANT_CHECK(r.ok && r.value["hit"] == false);
    GIANT_CHECK(f.run("get_physics").value["not_colliding"] == 1);
}
