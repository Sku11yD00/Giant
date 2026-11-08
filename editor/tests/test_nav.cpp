// The editor's navigation: tiles built from the scene's terrain and fixed
// bodies on workers, rebuilt when they change; searches over frames that
// wait for missing tiles; find_path and get_nav.
#include "../src/editor_commands.h"
#include "../src/nav_state.h"

#include "giant_test.h"

#include "giant/world/scene.h"

#include <cmath>
#include <chrono>
#include <thread>

using namespace giant;
using namespace giant::editor;
using ai::Json;

namespace {

struct Fixture {
    NavState nav{nullptr, nullptr, nullptr};
    ai::CommandRegistry commands;
    EditorState state;
    Fixture() {
        state.nav = &nav;
        register_scene_commands(commands, state);
        register_nav_commands(commands, state);
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
    // Frames until the map has nothing to build (at most `frames`).
    void settle(int frames = 2000) {
        for (int k = 0; k < frames; ++k) {
            nav.follow(state.scene, state.scene.take_changes());
            nav.update(state.scene);
            if (nav.building() == 0 && nav.stale() == 0 && k > 2) return;
            if (nav.building() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(1)); // the workers' turn
        }
    }
};

// Whether the path passes through the box (x, z) grown by `r`.
bool crosses(const Json& path, double cx, double cz, double hx, double hz, double r) {
    for (usize k = 1; k < path.size(); ++k) {
        const double ax = path[k - 1][0].get<double>(), az = path[k - 1][2].get<double>();
        const double bx = path[k][0].get<double>(), bz = path[k][2].get<double>();
        for (int t = 0; t <= 200; ++t) {
            const double x = ax + (bx - ax) * t / 200.0, z = az + (bz - az) * t / 200.0;
            if (std::fabs(x - cx) < hx + r && std::fabs(z - cz) < hz + r) return true;
        }
    }
    return false;
}

} // namespace

GIANT_TEST("editor nav: find_path walks a floor around a wall, and the wall moved, around it there") {
    Fixture f;
    // A floor plane 40 m a side (no terrain), a wall across the way.
    f.create({{"shape", "plane"}, {"position", {32.0, 0.0, 32.0}}, {"scale", {40.0, 1.0, 40.0}}});
    const world::EntityId wall =
        f.create({{"shape", "cube"}, {"position", {32.0, 1.5, 32.0}}, {"scale", {0.5, 3.0, 12.0}}});
    ai::CommandResult r = f.run("find_path", {{"from", {24.0, 0.0, 32.0}}, {"to", {40.0, 0.0, 32.0}}});
    GIANT_CHECK(r.ok);
    GIANT_CHECK(r.value["state"] == "found");
    GIANT_CHECK(r.value["tiles_built"].get<u32>() >= 1);
    const Json& path = r.value["path"];
    GIANT_CHECK(path.size() >= 3); // turns round the wall
    GIANT_CHECK(!crosses(path, 32.0, 32.0, 0.25, 6.0, 0.3));
    GIANT_CHECK(std::fabs(path[0][1].get<double>()) < 1e-3); // stands on the floor
    GIANT_CHECK(r.value["length"].get<double>() > 16.0);

    // The wall turned along the way: straight across now (the tile built again).
    GIANT_CHECK(f.run("set_entity", {{"id", wall}, {"position", {32.0, 1.5, 50.0}}}).ok);
    r = f.run("find_path", {{"from", {24.0, 0.0, 32.0}}, {"to", {40.0, 0.0, 32.0}}});
    GIANT_CHECK(r.value["state"] == "found");
    GIANT_CHECK(r.value["path"].size() == 2);
    GIANT_CHECK(std::fabs(r.value["length"].get<double>() - 16.0) < 0.6);

    // Off the floor: nothing to stand on.
    r = f.run("find_path", {{"from", {24.0, 0.0, 32.0}}, {"to", {5.0, 0.0, 5.0}}});
    GIANT_CHECK(r.value["state"] == "failed");

    // get_nav at a point: on the floor, in the wall.
    r = f.run("get_nav", {{"point", {20.0, 5.0, 20.0}}});
    GIANT_CHECK(r.ok && r.value["point"]["walkable"] == true);
    GIANT_CHECK(std::fabs(r.value["point"]["height"].get<double>()) < 1e-3);
    r = f.run("get_nav", {{"point", {32.0, 0.0, 50.0}}});
    GIANT_CHECK(r.value["point"]["walkable"] == false);
    GIANT_CHECK(r.value["tiles"].get<usize>() >= 1);
}

GIANT_TEST("editor nav: a search over frames waits for its tiles; edits make built tiles stale, rebuilt on workers") {
    Fixture f;
    world::Scene& scene = f.state.scene;
    scene.terrain.set_samples(8);
    for (int x = 0; x < 3; ++x) scene.terrain.set({x, 0}, world::Terrain::Heights(64, 2.0f));
    const NavState::SearchId id = f.nav.find_path({10.0, 0.0, 20.0}, {170.0, 0.0, 20.0});
    for (int k = 0; k < 5000; ++k) {
        f.nav.follow(scene, scene.take_changes());
        f.nav.update(scene);
        GIANT_CHECK(f.nav.building() <= f.nav.tiles_in_flight);
        if (f.nav.building() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const nav::PathSearch* s = f.nav.search(id);
        if (s->state() == nav::SearchState::found || s->state() == nav::SearchState::failed) break;
    }
    const nav::PathSearch* s = f.nav.search(id);
    GIANT_CHECK(s->state() == nav::SearchState::found);
    GIANT_CHECK(s->path().size() == 2 && std::fabs(s->path().back().y - 2.0) < 1e-4);
    GIANT_CHECK(f.nav.map().find({1, 0}) != nullptr);
    f.nav.cancel(id);
    GIANT_CHECK(f.nav.searches() == 0);

    // A terrain tile raised: the tile and its neighbours built again on their own.
    f.settle();
    const u64 before = f.nav.tiles_built();
    scene.terrain.set({1, 0}, world::Terrain::Heights(64, 5.0f));
    f.settle();
    GIANT_CHECK(f.nav.tiles_built() >= before + 3); // (0,0), (1,0), (2,0): their edges see it
    GIANT_CHECK(std::fabs(f.nav.map().at(100.0, 20.0).height - 5.0f) < 1e-4f);

    // A crate put down: its cell built again, blocked there.
    const u64 crate_before = f.nav.tiles_built();
    f.create({{"shape", "cube"}, {"position", {100.0, 6.0, 20.0}}, {"scale", {2.0, 2.0, 2.0}}});
    f.settle();
    GIANT_CHECK(f.nav.tiles_built() > crate_before);
    GIANT_CHECK(!f.nav.map().at(100.0, 20.0).walkable);
    // Undone: walkable again.
    GIANT_CHECK(f.run("undo").ok);
    f.settle();
    GIANT_CHECK(f.nav.map().at(100.0, 20.0).walkable);

    // Dynamic bodies and hidden things stand in nobody's way.
    f.create({{"shape", "cube"}, {"position", {110.0, 6.0, 20.0}}, {"scale", {2.0, 2.0, 2.0}}, {"body", "dynamic"}});
    f.create({{"shape", "cube"}, {"position", {120.0, 6.0, 20.0}}, {"scale", {2.0, 2.0, 2.0}}, {"visible", false}});
    f.settle();
    GIANT_CHECK(f.nav.map().at(110.0, 20.0).walkable && f.nav.map().at(120.0, 20.0).walkable);
}

GIANT_TEST("editor nav: the oldest unused tiles go past max_tiles; a new scene's tiles are built again") {
    Fixture f;
    world::Scene& scene = f.state.scene;
    scene.terrain.set_samples(8);
    for (int x = 0; x < 12; ++x) scene.terrain.set({x, 0}, world::Terrain::Heights(64, 1.0f));
    for (int x = 0; x < 12; ++x) f.nav.tile_now(scene, {x, 0});
    GIANT_CHECK(f.nav.map().size() == 12);
    f.nav.max_tiles = 4;
    for (int k = 0; k < 300; ++k) f.nav.update(scene); // past the recent-use guard
    GIANT_CHECK(f.nav.map().size() == 4);

    // Another scene: what was built is checked against it and built again.
    f.settle();
    const u64 before = f.nav.tiles_built();
    world::Scene other = scene;
    other.terrain.set({0, 0}, world::Terrain::Heights(64, 3.0f));
    f.state.scene = other;
    f.settle();
    GIANT_CHECK(f.nav.tiles_built() > before);
}
