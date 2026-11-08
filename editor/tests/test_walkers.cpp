// Walkers: entities sent walking with walk_to while simulating walk the
// navigation's way round a wall on a floor, as physics characters, drawn
// where they walk; two crossing keep apart; stopping lets them go.
#include "../src/editor_commands.h"
#include "../src/nav_state.h"
#include "../src/physics_state.h"
#include "../src/walker_state.h"

#include "giant_test.h"

#include "giant/world/scene.h"

#include <cmath>

using namespace giant;
using namespace giant::editor;
using ai::Json;

namespace {

struct Fixture {
    PhysicsState physics;
    NavState nav{nullptr, nullptr, nullptr};
    WalkerState walkers{physics, nav};
    ai::CommandRegistry commands;
    EditorState state;
    Fixture() {
        state.physics = &physics;
        state.nav = &nav;
        state.walkers = &walkers;
        register_scene_commands(commands, state);
        register_physics_commands(commands, state);
        register_nav_commands(commands, state);
        register_walker_commands(commands, state);
        connect_walkers(state);
    }
    ai::CommandResult run(const char* name, const Json& args = Json::object()) {
        ai::ExecuteOptions options;
        options.record_history = true;
        return commands.execute(name, args, options);
    }
    world::EntityId create(const Json& args) {
        const ai::CommandResult r = run("create_entity", args);
        GIANT_CHECK(r.ok);
        return r.ok ? r.value["id"].get<world::EntityId>() : 0;
    }
    world::EntityId person(double x, double z) {
        return create({{"shape", "cube"}, {"position", {x, 0.9, z}}, {"scale", {0.5, 1.8, 0.5}}, {"body", "none"}});
    }
};

double flat(const Json& a, double x, double z) { return std::hypot(a[0].get<double>() - x, a[2].get<double>() - z); }

} // namespace

GIANT_TEST("walkers: walk_to walks round a wall on a floor and stops there, drawn where it walks") {
    Fixture f;
    f.create({{"shape", "plane"}, {"position", {32.0, 0.0, 32.0}}, {"scale", {40.0, 1.0, 40.0}}});
    f.create({{"shape", "cube"}, {"position", {32.0, 1.5, 32.0}}, {"scale", {0.5, 3.0, 12.0}}});
    const world::EntityId p = f.person(24.0, 32.0);
    // Not simulating: refused. A body of its own: refused.
    GIANT_CHECK(!f.run("walk_to", {{"id", p}, {"to", {40.0, 0.0, 32.0}}}).ok);
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    const world::EntityId boxed = f.create({{"shape", "cube"}, {"position", {20.0, 0.5, 20.0}}});
    GIANT_CHECK(!f.run("walk_to", {{"id", boxed}, {"to", {40.0, 0.0, 32.0}}}).ok);

    ai::CommandResult r = f.run("walk_to", {{"id", p}, {"to", {40.0, 0.0, 32.0}}});
    GIANT_CHECK(r.ok);
    GIANT_CHECK(f.walkers.take_redraw().empty()); // drawn as moving once it has a pose (not before)
    GIANT_CHECK(r.value["phase"] == "walking"); // the way found at once
    GIANT_CHECK(r.value["path"].size() >= 3);
    // Walked: halfway, it is off the straight line (round the wall), moving at a walk.
    GIANT_CHECK(f.run("step_physics", {{"seconds", 0.1}}).ok);
    GIANT_CHECK(f.walkers.take_redraw() == std::vector<world::EntityId>{p});
    double widest = 0.0;
    for (int k = 0; k < 40 && f.run("get_walker", {{"id", p}}).value["walker"]["phase"] == "walking"; ++k) {
        GIANT_CHECK(f.run("step_physics", {{"seconds", 0.5}}).ok);
        const Json w = f.run("get_walker", {{"id", p}}).value["walker"];
        widest = std::max(widest, std::fabs(w["feet"][2].get<double>() - 32.0));
        GIANT_CHECK(!(std::fabs(w["feet"][0].get<double>() - 32.0) < 0.5 && std::fabs(w["feet"][2].get<double>() - 32.0) < 6.0));
    }
    const Json w = f.run("get_walker", {{"id", p}}).value["walker"];
    GIANT_CHECK(w["phase"] == "arrived");
    GIANT_CHECK(flat(w["feet"], 40.0, 32.0) < 0.7); // (the end snapped to a sample centre, then within `arrive`)
    GIANT_CHECK(std::fabs(w["feet"][1].get<double>()) < 0.1); // on the floor
    GIANT_CHECK(widest > 6.0);
    // Drawn there, its centre above its feet; the scene keeps where it was put.
    const auto pose = f.walkers.poses().find(p);
    GIANT_CHECK(pose != f.walkers.poses().end());
    GIANT_CHECK(std::fabs(pose->second.position.y - 0.9) < 0.15 && std::fabs(pose->second.position.x - 40.0) < 0.6);
    GIANT_CHECK(f.state.scene.find(p)->transform.position.x == 24.0);
    // Stopped: let go, drawn where the scene has it again.
    GIANT_CHECK(f.run("stop_walker", {{"id", p}}).ok);
    GIANT_CHECK(f.walkers.poses().empty() && f.walkers.size() == 0);
    GIANT_CHECK(!f.walkers.take_redraw().empty());
}

GIANT_TEST("walkers: two crossing keep apart and both arrive; stopping the simulation lets every walker go") {
    Fixture f;
    f.create({{"shape", "plane"}, {"position", {32.0, 0.0, 32.0}}, {"scale", {40.0, 1.0, 40.0}}});
    const world::EntityId a = f.person(22.0, 32.0);
    const world::EntityId b = f.person(42.0, 32.0);
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    GIANT_CHECK(f.run("walk_to", {{"id", a}, {"to", {42.0, 0.0, 32.0}}}).ok);
    GIANT_CHECK(f.run("walk_to", {{"id", b}, {"to", {22.0, 0.0, 32.0}}}).ok);
    double closest = 1e9;
    for (int k = 0; k < 400; ++k) {
        GIANT_CHECK(f.run("step_physics", {{"seconds", 0.1}}).ok);
        const Json wa = f.run("get_walker", {{"id", a}}).value["walker"];
        const Json wb = f.run("get_walker", {{"id", b}}).value["walker"];
        closest = std::min(closest, std::hypot(wa["feet"][0].get<double>() - wb["feet"][0].get<double>(),
                                               wa["feet"][2].get<double>() - wb["feet"][2].get<double>()));
        if (wa["phase"] == "arrived" && wb["phase"] == "arrived") break;
    }
    const Json all = f.run("get_walker").value;
    GIANT_CHECK(all["walkers"] == 2 && all["arrived"] == 2);
    GIANT_CHECK(closest > 0.85); // stepped aside: more than touching (0.5 m: their characters are 0.25 m round)

    // A ray at one names it.
    const Json wa = f.run("get_walker", {{"id", a}}).value["walker"];
    const ai::CommandResult hit = f.run("raycast", {{"origin", {wa["feet"][0].get<double>(), 5.0, wa["feet"][2].get<double>()}},
                                                    {"direction", {0.0, -1.0, 0.0}}});
    GIANT_CHECK(hit.ok && hit.value["entity"] == a);
    // Off and on again at once: let go all the same.
    GIANT_CHECK(f.run("simulate", {{"on", false}}).ok);
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    f.walkers.before_step(f.state.scene, 1.0 / 60.0);
    GIANT_CHECK(f.walkers.size() == 0 && f.walkers.poses().empty());
    GIANT_CHECK(f.physics.world().stats().characters == 0);
    GIANT_CHECK(f.run("walk_to", {{"id", a}, {"to", {30.0, 0.0, 32.0}}}).ok);
    GIANT_CHECK(f.run("simulate", {{"on", false}}).ok);
    f.walkers.before_step(f.state.scene, 1.0 / 60.0);
    GIANT_CHECK(f.walkers.size() == 0 && f.walkers.poses().empty());
    GIANT_CHECK(f.physics.world().stats().characters == 0);
}
