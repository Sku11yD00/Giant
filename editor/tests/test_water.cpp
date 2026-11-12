// Water in the editor: lakes, rivers and the sea made, changed and
// removed by command (undoable, saved with the scene and the world), and
// asked where water is.
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"

#include "giant_test.h"

#include "giant/world/water.h"

#include <cmath>

using namespace giant;
using namespace giant::editor;
using ai::Json;

namespace {

struct Fixture {
    ai::CommandRegistry commands;
    MeshLibrary meshes{nullptr};
    EditorState state;
    Fixture() {
        state.meshes = &meshes;
        register_scene_commands(commands, state);
        register_terrain_commands(commands, state);
        register_water_commands(commands, state);
    }
    ai::CommandResult run(const char* name, const Json& args = Json::object()) {
        return commands.execute(name, args, {});
    }
};

Json square(double size) { return Json::array({{0, 0}, {size, 0}, {size, size}, {0, size}}); }

} // namespace

GIANT_TEST("water commands: made, changed, renamed, removed, undone, refused when wrong") {
    Fixture f;
    ai::CommandResult r = f.run("create_water", {{"name", "pond"}, {"kind", "lake"}, {"level", 2.0}, {"outline", square(20)}});
    GIANT_CHECK(r.ok && r.value["kind"] == "lake" && r.value["bounds"]["to"][0] == 20.0);
    GIANT_CHECK(f.state.scene.water.size() == 1 && f.state.scene.water[0].level == 2.0f);
    // Refused: the same name, a lake without an outline, a river with one point, a bad kind.
    GIANT_CHECK(!f.run("create_water", {{"name", "pond"}, {"kind", "sea"}}).ok);
    GIANT_CHECK(!f.run("create_water", {{"name", "b"}, {"kind", "lake"}, {"level", 1.0}}).ok);
    GIANT_CHECK(!f.run("create_water", {{"name", "b"}, {"kind", "river"},
                                        {"path", {{{"x", 0}, {"z", 0}, {"width", 4}, {"level", 1}}}}}).ok);
    GIANT_CHECK(!f.run("create_water", {{"name", "b"}, {"kind", "pool"}}).ok);
    GIANT_CHECK(f.state.scene.water.size() == 1);
    r = f.run("create_water", {{"name", "brook"},
                               {"kind", "river"},
                               {"flow", 3.0},
                               {"path", {{{"x", 0}, {"z", 50}, {"width", 4}, {"level", 3}},
                                         {{"x", 40}, {"z", 50}, {"width", 8}, {"level", 1}}}}});
    GIANT_CHECK(r.ok && f.state.scene.water.size() == 2);
    // Changed: only what is given; renamed; a new kind drops the old points.
    GIANT_CHECK(f.run("set_water", {{"name", "pond"}, {"level", 2.5}, {"color", {0.1, 0.2, 0.3}}}).ok);
    GIANT_CHECK(f.state.scene.water[0].level == 2.5f && f.state.scene.water[0].outline.size() == 4 &&
                f.state.scene.water[0].color.z == 0.3f);
    GIANT_CHECK(!f.run("set_water", {{"name", "pond"}, {"new_name", "brook"}}).ok);
    GIANT_CHECK(f.run("set_water", {{"name", "pond"}, {"new_name", "mere"}}).ok && f.state.scene.water[0].name == "mere");
    GIANT_CHECK(!f.run("set_water", {{"name", "mere"}, {"kind", "river"}}).ok); // a river needs a path
    GIANT_CHECK(f.run("set_water", {{"name", "mere"}, {"kind", "sea"}}).ok && f.state.scene.water[0].outline.empty());
    GIANT_CHECK(!f.run("set_water", {{"name", "nothing"}, {"level", 1.0}}).ok);
    // Undone step by step, and done again.
    GIANT_CHECK(f.run("undo").ok && f.state.scene.water[0].kind == world::WaterKind::lake);
    GIANT_CHECK(f.run("undo").ok && f.state.scene.water[0].name == "pond");
    GIANT_CHECK(f.run("remove_water", {{"name", "brook"}}).ok && f.state.scene.water.size() == 1);
    GIANT_CHECK(!f.run("remove_water", {{"name", "brook"}}).ok);
    GIANT_CHECK(f.run("undo").ok && f.state.scene.water.size() == 2 && f.state.scene.water[1].name == "brook");
    // Saved and loaded with the scene.
    world::Scene loaded;
    GIANT_CHECK(!world::load_scene(world::save_scene(f.state.scene), loaded).has_value());
    GIANT_CHECK(loaded.water == f.state.scene.water);
}

GIANT_TEST("water commands: where water is, its level, depth over the ground and a river's flow") {
    Fixture f;
    // Flat ground at 0 over one 64 m cell.
    GIANT_CHECK(f.run("create_terrain", {{"from", {0, 0}}, {"to", {63, 63}}, {"samples", 16}}).ok);
    GIANT_CHECK(f.run("create_water", {{"name", "pond"}, {"kind", "lake"}, {"level", 2.0}, {"outline", square(20)}}).ok);
    GIANT_CHECK(f.run("create_water", {{"name", "brook"},
                                       {"kind", "river"},
                                       {"flow", 3.0},
                                       {"path", {{{"x", 0}, {"z", 50}, {"width", 4}, {"level", 3}},
                                                 {{"x", 40}, {"z", 50}, {"width", 8}, {"level", 1}}}}})
                    .ok);
    GIANT_CHECK(f.run("create_water", {{"name", "low"}, {"kind", "sea"}, {"level", -1.0}}).ok); // under the ground
    const ai::CommandResult r = f.run("get_water", {{"points", {{5, 5}, {20, 50}, {30, 30}, {500, 500}}}});
    GIANT_CHECK(r.ok && r.value["water"].size() == 3);
    const Json& at = r.value["at"];
    GIANT_CHECK(at[0]["name"] == "pond" && at[0]["level"] == 2.0 && std::fabs(at[0]["depth"].get<double>() - 2.0) < 1e-6);
    GIANT_CHECK(at[1]["name"] == "brook" && std::fabs(at[1]["level"].get<double>() - 2.0) < 1e-5 &&
                std::fabs(at[1]["flow"][0].get<double>() - 3.0) < 1e-5 && std::fabs(at[1]["width"].get<double>() - 6.0) < 1e-5);
    GIANT_CHECK(at[2].is_null()); // only the sea, under the ground
    GIANT_CHECK(at[3]["name"] == "low" && at[3]["depth"].is_null()); // no ground there
}
