// Physics in the editor: entities with bodies, simulating like a play
// button (things fall and rest; stopping puts them back), raycasts.
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"
#include "../src/physics_state.h"
#include "../src/play_state.h"

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
    PlayState play{physics};
    EditorState state;
    Fixture() {
        state.meshes = &meshes;
        state.physics = &physics;
        state.play = &play;
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

namespace {
DVec3 triple(const Json& j) { return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()}; }
} // namespace

GIANT_TEST("play commands: the player stands under the camera, walks where it looks, stops and gives the camera back") {
    Fixture f;
    const world::EntityId plane = f.create({{"shape", "plane"}, {"scale", {200, 1, 200}}});
    // A crate to push, a wall, a step.
    const world::EntityId crate =
        f.create({{"shape", "cube"}, {"position", {0, 0.5, -20}}, {"body", "dynamic"}, {"mass", 20}});
    const world::EntityId step = f.create({{"shape", "cube"}, {"position", {10, 0.15, 0}}, {"scale", {4, 0.3, 4}}});
    GIANT_CHECK(!f.run("set_player_input", {{"forward", 1}}).ok); // not playing
    GIANT_CHECK(!f.run("step_player", {{"seconds", 1}}).ok);
    f.state.camera.position = {0.0, 10.0, 0.0};
    f.state.camera.yaw_degrees = 0.0f; // looking down -z, at the crate
    f.state.camera.pitch_degrees = -30.0f;
    const EditorCamera before = f.state.camera;
    ai::CommandResult r = f.run("play", {{"on", true}});
    GIANT_CHECK(r.ok && r.value["playing"] == true && f.physics.simulating());
    // On the plane under the camera, eyes 1.65 m up.
    GIANT_CHECK(std::fabs(triple(r.value["position"]).y) < 0.05 && std::fabs(f.state.camera.position.y - 1.65) < 0.06);
    r = f.run("step_player", {{"seconds", 0.5}});
    GIANT_CHECK(r.ok && r.value["on_ground"] == true && r.value["standing_on"] == plane);
    // Rays hit the player.
    r = f.run("raycast", {{"origin", {0, 5, 0}}, {"direction", {0, -1, 0}}});
    GIANT_CHECK(r.ok && r.value["player"] == true);
    // Walking forward 4 m/s for 3 s: 12 m down -z.
    GIANT_CHECK(f.run("set_player_input", {{"forward", 1}}).ok);
    r = f.run("step_player", {{"seconds", 3}});
    DVec3 p = triple(r.value["position"]);
    GIANT_CHECK(r.ok && std::fabs(p.z + 12.0) < 0.3 && std::fabs(p.x) < 0.05);
    // On into the crate: pushed along.
    r = f.run("step_player", {{"seconds", 4}});
    GIANT_CHECK(f.at(crate).z < -21.0);
    // Again from beside the step, looking down +x (yaw -90): up it.
    GIANT_CHECK(f.run("play", {{"on", false}}).ok);
    f.state.camera.position = {6.0, 3.0, 0.0};
    GIANT_CHECK(f.run("play", {{"on", true}}).ok);
    GIANT_CHECK(f.run("set_player_input", {{"yaw", -90}, {"forward", 1}}).ok);
    r = f.run("step_player", {{"seconds", 1.25}});
    p = triple(r.value["position"]);
    GIANT_CHECK(p.x > 9.0 && p.x < 11.5 && std::fabs(p.y - 0.3) < 0.03 && r.value["standing_on"] == step);
    // A jump: up about 1.3 m.
    GIANT_CHECK(f.run("set_player_input", {{"forward", 0}, {"jump", true}}).ok);
    double top = 0.0;
    for (int i = 0; i < 40; ++i) top = std::max(top, triple(f.run("step_player", {{"seconds", 1.0 / 60.0}}).value["position"]).y);
    GIANT_CHECK(top > 1.4 && top < 1.75);
    // Stopping: the camera back, simulating as it was (off), the scene untouched.
    r = f.run("play", {{"on", false}});
    GIANT_CHECK(r.ok && r.value["playing"] == false && !f.physics.simulating());
    GIANT_CHECK(f.state.camera.position == DVec3(6.0, 3.0, 0.0) && f.state.camera.pitch_degrees == before.pitch_degrees);
    GIANT_CHECK(f.at(crate) == DVec3(0.0, 0.5, -20.0) && f.physics.world().characters() == 0);
    GIANT_CHECK(f.run("get_player").value["playing"] == false);
}

GIANT_TEST("play commands: the player climbs a wall and onto its top; an entity carries it; crates are pushed, not climbed") {
    Fixture f;
    f.create({{"shape", "plane"}, {"scale", {200, 1, 200}}});
    // A 4 m wall block, its face at z = -2.5, its top y = 4, 4 m deep.
    const world::EntityId wall = f.create({{"shape", "cube"}, {"position", {0, 2, -4.5}}, {"scale", {6, 4, 4}}});
    f.state.camera.position = {0.0, 10.0, 0.0};
    f.state.camera.yaw_degrees = 0.0f; // looking down -z, at the wall
    GIANT_CHECK(f.run("play", {{"on", true}}).ok);
    ai::CommandResult r = f.run("step_player", {{"seconds", 0.3}});
    GIANT_CHECK(r.value["climbing"] == false && r.value["input"]["climb"] == true && r.value["wall"].is_null());
    GIANT_CHECK(r.value["carried_by"].is_null());
    // Into it: up it.
    GIANT_CHECK(f.run("set_player_input", {{"forward", 1}}).ok);
    r = f.run("step_player", {{"seconds", 1.5}});
    DVec3 p = triple(r.value["position"]);
    GIANT_CHECK(r.value["climbing"] == true && r.value["wall"] == wall && p.y > 1.0 && p.y < 3.0 && p.z > -2.3);
    // On over the top, standing on it.
    r = f.run("step_player", {{"seconds", 1.5}});
    p = triple(r.value["position"]);
    GIANT_CHECK(r.value["climbing"] == false && r.value["mantling"] == false && r.value["standing_on"] == wall);
    GIANT_CHECK(std::fabs(p.y - 4.0) < 0.05 && p.z < -2.6);
    // Told not to climb, it stops at the wall.
    GIANT_CHECK(f.run("play", {{"on", false}}).ok);
    GIANT_CHECK(f.run("play", {{"on", true}}).ok);
    GIANT_CHECK(f.run("set_player_input", {{"forward", 1}, {"climb", false}}).ok);
    r = f.run("step_player", {{"seconds", 2}});
    p = triple(r.value["position"]);
    GIANT_CHECK(r.value["climbing"] == false && std::fabs(p.y) < 0.05 && p.z < -2.0); // at the wall
    GIANT_CHECK(r.value["input"]["climb"] == false);
    // A crate walked into is pushed, never climbed.
    GIANT_CHECK(f.run("play", {{"on", false}}).ok);
    const world::EntityId crate =
        f.create({{"shape", "cube"}, {"position", {30, 0.75, -3}}, {"scale", {1.5, 1.5, 1.5}}, {"body", "dynamic"}, {"mass", 20}});
    f.state.camera.position = {30.0, 10.0, 0.0};
    GIANT_CHECK(f.run("play", {{"on", true}}).ok);
    GIANT_CHECK(f.run("set_player_input", {{"forward", 1}}).ok);
    r = f.run("step_player", {{"seconds", 3}});
    GIANT_CHECK(r.value["climbing"] == false && std::fabs(triple(r.value["position"]).y) < 0.05 && f.at(crate).z < -4.0);
    // Carried: a platform it stands on, lifted 10 m (put there at once), takes it along.
    GIANT_CHECK(f.run("play", {{"on", false}}).ok);
    const world::EntityId platform =
        f.create({{"shape", "cube"}, {"position", {-30, 0.25, 0}}, {"scale", {2, 0.5, 2}}});
    f.state.camera.position = {-30.0, 10.0, 0.0};
    GIANT_CHECK(f.run("play", {{"on", true}}).ok);
    GIANT_CHECK(f.run("step_player", {{"seconds", 0.3}}).value["standing_on"] == platform);
    GIANT_CHECK(!f.run("carry_player").ok);                                          // neither
    GIANT_CHECK(!f.run("carry_player", {{"entity", platform}, {"release", true}}).ok); // both
    GIANT_CHECK(!f.run("carry_player", {{"entity", 999999}}).ok);                    // no such entity
    r = f.run("carry_player", {{"entity", platform}});
    GIANT_CHECK(r.ok && r.value["carried_by"] == platform);
    GIANT_CHECK(f.run("set_entity", {{"id", platform}, {"position", {-30, 10.25, 0}}}).ok);
    r = f.run("step_player", {{"seconds", 0.1}});
    p = triple(r.value["position"]);
    GIANT_CHECK(r.value["carried_by"] == platform && std::fabs(p.y - 10.5) < 0.05 && std::fabs(p.x + 30.0) < 0.05);
    // Carried away sideways (put there at once), then let go: it falls.
    GIANT_CHECK(f.run("set_entity", {{"id", platform}, {"position", {-40, 10.25, 0}}}).ok);
    r = f.run("step_player", {{"seconds", 0.1}});
    GIANT_CHECK(std::fabs(triple(r.value["position"]).x + 40.0) < 0.05);
    GIANT_CHECK(f.run("set_entity", {{"id", platform}, {"position", {-30, 0.25, 0}}}).ok); // out from under it
    r = f.run("carry_player", {{"release", true}});
    GIANT_CHECK(r.ok && r.value["carried_by"].is_null());
    r = f.run("step_player", {{"seconds", 3}});
    p = triple(r.value["position"]);
    GIANT_CHECK(r.value["on_ground"] == true && std::fabs(p.y) < 0.05 && std::fabs(p.x + 40.0) < 0.05);
    // Carried again (it need not stand on what carries it), the platform removed: let go.
    GIANT_CHECK(f.run("carry_player", {{"entity", platform}}).value["carried_by"] == platform);
    GIANT_CHECK(f.run("delete_entities", {{"ids", {platform}}}).ok);
    r = f.run("step_player", {{"seconds", 0.1}});
    GIANT_CHECK(r.value["carried_by"].is_null());
    GIANT_CHECK(f.run("play", {{"on", false}}).ok);
    GIANT_CHECK(!f.run("carry_player", {{"release", true}}).ok); // not playing
}

GIANT_TEST("play commands: Esc stops, a new scene stops, simulate off stops; simulating kept as it was") {
    Fixture f;
    f.create({{"shape", "plane"}, {"scale", {100, 1, 100}}});
    f.state.camera.position = {0.0, 3.0, 0.0};
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    GIANT_CHECK(f.run("play", {{"on", true}}).ok && f.play.playing());
    GIANT_CHECK(!f.run("set_camera", {{"position", {0, 50, 0}}}).ok); // the camera is the player's eyes
    // Esc, then the next frame's step stops it; simulating stays on (it was on).
    platform::InputEvent esc;
    esc.type = platform::InputEvent::Type::key;
    esc.key = platform::Key::escape;
    esc.down = true;
    f.play.handle_input({esc}, false);
    GIANT_CHECK(!f.play.before_step(f.state.scene, f.state.camera, f.state.scene_generation, 0.0));
    GIANT_CHECK(!f.play.playing() && f.physics.simulating() && f.state.camera.position == DVec3(0.0, 3.0, 0.0));
    // The keys walk it; a key let go stops it.
    GIANT_CHECK(f.run("play", {{"on", true}}).ok);
    platform::InputEvent w;
    w.type = platform::InputEvent::Type::key;
    w.key = platform::Key::w;
    w.down = true;
    f.play.handle_input({w}, false);
    GIANT_CHECK(f.play.input().forward == 1.0f);
    w.down = false;
    f.play.handle_input({w}, true); // a release counts even over a panel
    GIANT_CHECK(f.play.input().forward == 0.0f);
    // Mouse look turns it.
    platform::InputEvent move;
    move.type = platform::InputEvent::Type::mouse_move;
    move.dx = 100.0f;
    const float yaw = f.play.yaw();
    f.play.handle_input({move}, false);
    GIANT_CHECK(f.play.yaw() < yaw - 10.0f);
    // A new scene: stops on the next step.
    GIANT_CHECK(f.run("new_scene").ok);
    GIANT_CHECK(f.run("step_player", {{"seconds", 0.1}}).ok && !f.play.playing());
    // simulate off while playing stops play.
    f.create({{"shape", "plane"}, {"scale", {100, 1, 100}}});
    GIANT_CHECK(f.run("play", {{"on", true}}).ok);
    GIANT_CHECK(f.run("simulate", {{"on", false}}).ok && !f.play.playing() && !f.physics.simulating());
}

GIANT_TEST("breakage commands: a breakable house, an impact knocks a wall out, stopping mends it; saved and undone") {
    Fixture f;
    f.create({{"shape", "plane"}, {"scale", {100, 1, 100}}});
    const Json wall_x = {{"shape", "cube"}, {"position", {1.9, 1.5, 0}}, {"scale", {0.2, 3, 3.6}}};
    const Json wall_w = {{"shape", "cube"}, {"position", {-1.9, 1.5, 0}}, {"scale", {0.2, 3, 3.6}}};
    const Json roof = {{"shape", "cube"}, {"position", {0, 3.1, 0}}, {"scale", {4.2, 0.2, 4}}};
    ai::CommandResult r = f.run("create_kit", {{"name", "hut"}, {"pieces", {wall_x, wall_w, roof}}});
    GIANT_CHECK(r.ok);
    const world::EntityId hut = f.create({{"kit", "hut"}, {"position", {0, 0, 0}}});
    GIANT_CHECK(f.run("get_kit", {{"name", "hut"}}).value["breakable"] == false);
    // Not breakable yet: an impact loosens nothing. Not simulating: refused.
    GIANT_CHECK(!f.run("impact", {{"point", {2.3, 1.5, 0}}, {"radius", 1}, {"impulse", 1e5}}).ok);
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    r = f.run("impact", {{"point", {2.3, 1.5, 0}}, {"radius", 1}, {"impulse", 1e5}});
    GIANT_CHECK(r.ok && r.value["loosened"] == 0);
    r = f.run("set_kit_breakable", {{"name", "hut"}, {"breakable", true}, {"strength", 2000}});
    GIANT_CHECK(r.ok && r.value["strength"] == 2000.0);
    // The east wall out: the roof still on the west wall.
    r = f.run("impact", {{"point", {2.3, 1.5, 0}}, {"radius", 0.5}, {"impulse", 8000}});
    GIANT_CHECK(r.ok && r.value["loosened"] == 1 && r.value["broken_kits"] == 1 && r.value["loose_pieces"] == 1);
    GIANT_CHECK(f.run("step_physics", {{"seconds", 2}}).ok);
    // Then the west: the roof comes down with it.
    r = f.run("impact", {{"point", {-2.3, 1.5, 0}}, {"radius", 0.5}, {"impulse", 8000}});
    GIANT_CHECK(r.ok && r.value["loosened"] == 2 && r.value["loose_pieces"] == 3);
    GIANT_CHECK(f.physics.piece_poses().count(hut) == 1 && f.physics.piece_poses().at(hut).size() == 3);
    // The scene keeps the kit where it was put: saving is untouched by the break.
    GIANT_CHECK(f.state.scene.find(hut)->transform.position == DVec3(0.0, 0.0, 0.0));
    const std::string text = world::save_scene(f.state.scene);
    world::Scene loaded;
    GIANT_CHECK(!world::load_scene(text, loaded) && loaded.find_kit("hut") != nullptr &&
                loaded.find_kit("hut")->breakable && loaded.find_kit("hut")->strength == 2000.0f);
    // Stopping: whole again.
    GIANT_CHECK(f.run("simulate", {{"on", false}}).ok);
    r = f.run("get_physics");
    GIANT_CHECK(r.value["broken_kits"] == 0 && r.value["loose_pieces"] == 0 && f.physics.piece_poses().empty());
    r = f.run("raycast", {{"origin", {0, 10, 0}}, {"direction", {0, -1, 0}}});
    GIANT_CHECK(r.ok && r.value["entity"] == hut && std::fabs(r.value["point"][1].get<double>() - 3.2) < 0.01);
    // Undo the breakable setting.
    GIANT_CHECK(f.run("undo").ok && !f.state.scene.find_kit("hut")->breakable);
}

GIANT_TEST("ballistics commands: shots go through wood, stop in concrete, push crates and shoot out kit pieces") {
    Fixture f;
    f.create({{"shape", "plane"}, {"scale", {100, 1, 100}}});
    const world::EntityId board =
        f.create({{"shape", "cube"}, {"position", {5, 1, 0}}, {"scale", {0.1, 2, 2}}, {"surface", "wood"}});
    const world::EntityId block =
        f.create({{"shape", "cube"}, {"position", {10, 1, 0}}, {"scale", {1, 2, 2}}, {"surface", "concrete"}});
    GIANT_CHECK(f.run("get_entity", {{"id", board}}).value["surface"] == "wood");
    ai::CommandResult r = f.run("raycast", {{"origin", {0, 1, 0}}, {"direction", {1, 0, 0}}});
    GIANT_CHECK(r.ok && r.value["entity"] == board && r.value["surface"] == "wood");
    r = f.run("raycast", {{"origin", {0, 5, 0}}, {"direction", {0, -1, 0}}});
    GIANT_CHECK(r.ok && r.value["surface"] == "standard"); // the plane: an entity, as it is set
    // Not simulating: only a resolved shot, and it only reports.
    const Json pistol = {{"origin", {0, 1, 0}}, {"direction", {1, 0, 0}}, {"speed", 360}, {"mass", 0.008}};
    GIANT_CHECK(!f.run("fire", pistol).ok);
    Json resolved = pistol;
    resolved["resolve"] = true;
    r = f.run("fire", resolved);
    GIANT_CHECK(r.ok && r.value["hits"].size() == 2);
    if (r.ok && r.value["hits"].size() == 2) {
        const Json& h = r.value["hits"];
        GIANT_CHECK(h[0]["entity"] == board && h[0]["outcome"] == "passed" && h[0]["surface"] == "wood");
        GIANT_CHECK(h[1]["entity"] == block && h[1]["outcome"] == "stopped" && h[1]["surface"] == "concrete");
        GIANT_CHECK(h[0]["speed_out"].get<double>() < h[0]["speed_in"].get<double>());
    }
    GIANT_CHECK(!f.run("fire", {{"origin", {0, 1, 0}}, {"direction", {0, 0, 0}}, {"resolve", true}}).ok);
    GIANT_CHECK(!f.run("fire", {{"origin", {0, 1, 0}}, {"direction", {1, 0, 0}}, {"mass", 0}, {"resolve", true}}).ok);

    // A crate is pushed by what it stops.
    const world::EntityId crate = f.create(
        {{"shape", "cube"}, {"position", {0, 0.5, 8}}, {"body", "dynamic"}, {"mass", 1}, {"surface", "metal"}});
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    GIANT_CHECK(f.run("step_physics", {{"seconds", 1}}).ok);
    const DVec3 before = f.at(crate);
    r = f.run("fire", {{"origin", {0, 0.5, 0}}, {"direction", {0, 0, 1}}, {"mass", 0.05}, {"speed", 600}, {"resolve", true}});
    GIANT_CHECK(r.ok && r.value["hits"].size() == 1 && r.value["hits"][0]["entity"] == crate);
    GIANT_CHECK(f.run("step_physics", {{"seconds", 0.5}}).ok);
    GIANT_CHECK(f.at(crate).z > before.z + 0.5);

    // A wooden hut wall worn through by shots: out once the energy left in it comes to its strength.
    const Json wall = {{"shape", "cube"}, {"position", {0, 1.5, 0}}, {"scale", {0.3, 3, 3}}, {"surface", "wood"}};
    const Json roof = {{"shape", "cube"}, {"position", {0, 3.1, 0}}, {"scale", {1, 0.2, 3}}, {"surface", "metal"}};
    GIANT_CHECK(f.run("create_kit", {{"name", "hut"}, {"pieces", {wall, roof}}}).ok);
    const world::EntityId hut = f.create({{"kit", "hut"}, {"position", {-20, 0, 0}}});
    GIANT_CHECK(f.run("set_kit_breakable", {{"name", "hut"}, {"breakable", true}, {"strength", 300}}).ok);
    const Json at_wall = {{"origin", {-25, 1.5, 0}}, {"direction", {1, 0, 0}}, {"speed", 360}, {"resolve", true}};
    r = f.run("fire", at_wall); // ~518 J stopped in 0.3 m of wood: 259 N s of wear, short of 300
    GIANT_CHECK(r.ok && r.value["hits"].size() >= 1 && r.value["hits"][0]["entity"] == hut &&
                r.value["hits"][0]["piece"] == 0 && r.value["hits"][0]["outcome"] == "stopped" &&
                !r.value["hits"][0].contains("loosened"));
    r = f.run("fire", at_wall);
    GIANT_CHECK(r.ok && r.value["hits"].size() >= 1 && r.value["hits"][0]["loosened"] == 2); // and the roof on it
    GIANT_CHECK(f.run("get_physics").value["loose_pieces"] == 2);

    // A bullet in flight: it hits in the steps, shown in get_ballistics.
    r = f.run("fire", {{"origin", {0, 1, 0}}, {"direction", {1, 0, 0}}});
    GIANT_CHECK(r.ok && r.value["bullet"].get<u32>() != 0);
    GIANT_CHECK(f.run("get_ballistics").value["flying"] == 1);
    GIANT_CHECK(f.run("step_physics", {{"seconds", 0.1}}).ok);
    r = f.run("get_ballistics", {{"hits", 2}});
    GIANT_CHECK(r.ok && r.value["flying"] == 0 && r.value["recent_hits"].size() == 2);
    GIANT_CHECK(r.ok && r.value["recent_hits"][1]["entity"] == block && r.value["surfaces"]["wood"]["resistance"] == 3000.0);
    // Tuned: wood that takes almost nothing; a surface not known is refused.
    r = f.run("set_ballistics", {{"surfaces", {{"concrete", {{"resistance", 10}}}}}});
    GIANT_CHECK(r.ok && r.value["surfaces"]["concrete"]["resistance"] == 10.0);
    GIANT_CHECK(!f.run("set_ballistics", {{"surfaces", {{"steel", {{"resistance", 10}}}}}}).ok);
    GIANT_CHECK(!f.run("set_ballistics", {{"surfaces", {{"wood", {{"resistance", 0}}}}}}).ok);
    GIANT_CHECK(!f.run("set_ballistics", {{"surfaces", {{"wood", {{"hardness", 1}}}}}}).ok);
    r = f.run("fire", resolved);
    GIANT_CHECK(r.ok && r.value["hits"].size() >= 2 && r.value["hits"][1]["outcome"] == "passed");
    // Stopping: no bullets left, pieces back.
    GIANT_CHECK(f.run("fire", {{"origin", {0, 50, 0}}, {"direction", {0, 1, 0}}}).ok);
    GIANT_CHECK(f.run("simulate", {{"on", false}}).ok);
    r = f.run("get_ballistics");
    GIANT_CHECK(r.value["flying"] == 0 && f.run("get_physics").value["loose_pieces"] == 0);
}
