// Kits and decals through the commands, and what the renderer is handed.
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"
#include "../src/scene_render.h"

#include "giant_test.h"

#include "giant/rhi/rhi.h"

#include <cmath>
#include <filesystem>

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
    }
    ai::CommandResult run(const char* name, const Json& args = Json::object()) {
        return commands.execute(name, args, {});
    }
};
} // namespace

GIANT_TEST("kits: made from entities, placed, changed, and undone") {
    Fixture f;
    const auto wall = f.run("create_entity", {{"name", "wall"}, {"position", {10, 0, 0}}, {"scale", {4, 3, 0.2}}});
    const auto door = f.run("create_entity", {{"name", "door"}, {"position", {11, 0, 0}}, {"base_color", {0.4, 0.2, 0.1}}});
    GIANT_CHECK(wall.ok && door.ok);
    const u64 wall_id = wall.value["id"].get<u64>();
    const u64 door_id = door.value["id"].get<u64>();
    const auto made = f.run("create_kit", {{"name", "house"}, {"from_entities", {wall_id, door_id}}, {"replace_entities", true}});
    GIANT_CHECK(made.ok);
    GIANT_CHECK(made.value["pieces"] == 2 && made.value["placements"] == 1);
    GIANT_CHECK(f.state.scene.size() == 1); // the two became one placement
    const world::Kit* house = f.state.scene.find_kit("house");
    GIANT_CHECK(house != nullptr && house->pieces.size() == 2);
    GIANT_CHECK(house->pieces[1].transform.position.x == 1.0); // relative to the wall, the origin

    // Placing it again, and the name and shape checks.
    const auto placed = f.run("create_entities", {{"entities", {{{"kit", "house"}, {"position", {0, 0, 50}}},
                                                                {{"kit", "house"}, {"position", {0, 0, 80}}}}}});
    GIANT_CHECK(placed.ok);
    GIANT_CHECK(!f.run("create_entity", {{"kit", "castle"}}).ok);
    GIANT_CHECK(!f.run("create_kit", {{"name", "house"}, {"pieces", {Json::object()}}}).ok); // exists
    GIANT_CHECK(!f.run("create_kit", {{"name", "nest"}, {"from_entities", {placed.value["ids"][0]}}}).ok);
    GIANT_CHECK(!f.run("create_kit", {{"name", "x"}}).ok); // no pieces
    GIANT_CHECK(f.run("list_kits").value["kits"][0]["placements"] == 3);
    GIANT_CHECK(!f.run("delete_kit", {{"name", "house"}}).ok); // in use

    // Changing the kit changes every placement.
    GIANT_CHECK(f.run("create_kit", {{"name", "house"}, {"replace", true},
                                     {"pieces", {{{"shape", "sphere"}}, {{"shape", "cube"}}, {{"shape", "plane"}}}}}).ok);
    GIANT_CHECK(f.run("get_kit", {{"name", "house"}}).value["pieces"].size() == 3);
    GIANT_CHECK(f.commands.undo()); // back to two pieces
    GIANT_CHECK(f.state.scene.find_kit("house")->pieces.size() == 2);
    GIANT_CHECK(f.commands.undo()); // the two placements
    GIANT_CHECK(f.commands.undo()); // the kit, and the wall and door come back
    GIANT_CHECK(f.state.scene.kits.empty());
    GIANT_CHECK(f.state.scene.find(wall_id) != nullptr && f.state.scene.find(door_id) != nullptr);
    GIANT_CHECK(f.state.scene.size() == 2);
    GIANT_CHECK(f.commands.redo());
    GIANT_CHECK(f.state.scene.size() == 1 && f.state.scene.find(wall_id) == nullptr);

    // Kits survive a save and load.
    const std::string path = (std::filesystem::temp_directory_path() / "giant_kits.scene.json").string();
    GIANT_CHECK(f.run("save_scene", {{"path", path}}).ok);
    Fixture g;
    GIANT_CHECK(g.run("load_scene", {{"path", path}}).ok);
    GIANT_CHECK(g.state.scene.find_kit("house") != nullptr && g.state.scene.size() == 1);
    std::filesystem::remove(path);
}

GIANT_TEST("kits: a placement draws its pieces where the kit is, sharing materials") {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    auto device = rhi::create_device(desc);
    auto renderer = render::Renderer::create(*device);
    GIANT_CHECK(renderer != nullptr);
    if (renderer == nullptr) return;
    world::Scene scene;
    world::Kit kit;
    kit.name = "post";
    world::KitPiece base;
    base.transform.position = {1.0, 0.0, 0.0};
    world::KitPiece top = base;
    top.shape = world::Shape::sphere;
    top.transform.position = {0.0, 2.0, 0.0};
    world::KitPiece stain;
    stain.shape = world::Shape::decal;
    stain.transform.scale = {2.0f, 1.0f, 2.0f};
    kit.pieces = {base, top, stain};
    scene.kits.push_back(kit);
    for (int i = 0; i < 100; ++i) {
        world::Entity e;
        e.shape = world::Shape::kit;
        e.kit = "post";
        e.transform.position = {static_cast<double>(i) * 10.0, 0.0, 0.0};
        e.transform.rotation = quat_from_euler({0.0f, 90.0f, 0.0f}); // yaw: +X turns to -Z
        e.transform.scale = {2.0f, 2.0f, 2.0f};
        scene.add(e);
    }
    world::Entity lost;
    lost.shape = world::Shape::kit;
    lost.kit = "missing";
    scene.add(lost);
    MeshLibrary meshes(renderer.get());
    SceneFrame frame;
    scene_frame(scene, *renderer, meshes, frame);
    GIANT_CHECK(frame.instances.size() == 200 && frame.decals.size() == 100);
    GIANT_CHECK(frame.materials.size() == 1); // every piece has the default look
    // The first piece of the placement at x = 30: 1 m along the kit's x,
    // scaled by 2 and turned by the yaw.
    const render::Instance& i = frame.instances[6];
    GIANT_CHECK(std::fabs(i.position.x - 30.0) < 1e-4 && std::fabs(i.position.z + 2.0) < 1e-4);
    GIANT_CHECK(i.scale.x == 2.0f);
    GIANT_CHECK(frame.decals[3].size.x == 4.0f);
}
