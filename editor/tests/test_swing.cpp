// Swinging entities: made and changed by commands, swung by the wind,
// drawn turned about their pivots and moved in place in their cells (no
// rebuild, kept out of the voxel world); knocked by shots, thrown by
// blasts (with the cloth); the physics' pose wins.
#include "../src/cloth_state.h"
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"
#include "../src/physics_state.h"
#include "../src/scene_render.h"
#include "../src/swing_state.h"

#include "giant_test.h"

#include "giant/rhi/rhi.h"
#include "giant/world/scene.h"

#include <cmath>

using namespace giant;
using namespace giant::editor;
using ai::Json;

namespace {

WindState wind(float speed) {
    WindState w;
    w.velocity = {speed, 0.0f, 0.0f};
    return w;
}

struct Fixture {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<render::Renderer> renderer;
    std::unique_ptr<MeshLibrary> meshes;
    std::unique_ptr<ClothState> cloth;
    std::unique_ptr<SwingState> swing;
    PhysicsState physics;
    ai::CommandRegistry commands;
    EditorState state;
    SceneFrame frame;
    rhi::TextureHandle target;
    Fixture() {
        rhi::DeviceDesc desc;
        desc.backend = rhi::Backend::null;
        device = rhi::create_device(desc);
        renderer = render::Renderer::create(*device);
        rhi::TextureDesc t;
        t.width = 160;
        t.height = 90;
        t.format = rhi::Format::rgba8_srgb;
        t.usage = rhi::TextureUsage::render_target;
        target = device->create_texture(t);
        meshes = std::make_unique<MeshLibrary>(renderer.get());
        cloth = std::make_unique<ClothState>(renderer.get());
        swing = std::make_unique<SwingState>(renderer.get(), meshes.get());
        state.meshes = meshes.get();
        state.cloth = cloth.get();
        state.swing = swing.get();
        state.physics = &physics;
        physics.take_frame_changes = [this] { return frame.take_for_followers(); };
        register_scene_commands(commands, state);
        register_physics_commands(commands, state);
        register_cloth_commands(commands, state);
        register_swing_commands(commands, state);
        connect_blows(state);
    }
    ~Fixture() {
        cloth.reset(); // meshes leave the renderer first
        meshes.reset();
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
    // A frame as main runs one.
    void frame_step(float wind_speed = 10.0f) {
        cloth->follow(state.scene, state.scene.untaken_changes());
        swing->follow(state.scene, state.scene.untaken_changes());
        std::vector<world::EntityId> redraw = cloth->take_redraw();
        std::vector<world::EntityId> moved = physics.take_redraw();
        const std::vector<world::EntityId> swung = swing->take_moved();
        moved.insert(moved.end(), swung.begin(), swung.end());
        scene_frame(state.scene, *renderer, *meshes, frame, nullptr, nullptr, nullptr, &physics.poses(), redraw,
                    &physics.piece_poses(), nullptr, {}, nullptr, &cloth->meshes(), moved, &swing->poses(), nullptr,
                    &physics.moving());
        frame.take_for_animation();
        physics.update(state.scene, frame.take_for_followers(), 1.0 / 60.0);
        cloth->update(wind(wind_speed), {0.0, 1.0, 6.0}, 1.0 / 60.0);
        swing->update(wind(wind_speed), {0.0, 1.0, 6.0}, 1.0 / 60.0);
        render::FrameInput input;
        input.view.position = {0.0, 1.0, 6.0};
        input.instances = frame.instances;
        input.cells = frame.cells;
        input.materials = frame.materials;
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        renderer->render(*cmd, target, input);
        device->end_frame();
    }
    // The instances of the cell holding `id`'s scene position, as drawn.
    const SceneFrame::Cell& cell_of(world::EntityId id) const {
        return frame.grid.at(frame.where.at(id));
    }
    // The range the renderer is told of for that cell.
    const render::CellRange& range_of(world::EntityId id) const {
        const SceneFrame::Cell& c = cell_of(id);
        for (const render::CellRange& r : frame.cells) {
            if (r.first == c.first && r.version == c.version) return r;
        }
        static const render::CellRange none;
        return none;
    }
};

} // namespace

GIANT_TEST("swing: a lantern swings in the wind, drawn turned about its pivot, moved in place, kept out of voxels") {
    Fixture f;
    const world::EntityId post = f.create({{"shape", "cube"}, {"position", {1.0, 1.0, 0.0}}});
    const world::EntityId lantern = f.create({{"shape", "cube"},
                                              {"position", {0.0, 2.0, 0.0}},
                                              {"scale", {0.3, 0.4, 0.3}},
                                              {"swing", {{"on", true}, {"length", 0.6}, {"weight", 1.5}}}});
    const auto got = f.run("get_entity", {{"id", lantern}});
    GIANT_CHECK(got.value["swing"]["on"] == true && got.value["swing"]["hinge"] == "free");
    f.frame_step();
    const SceneFrame::Cell& cell = f.cell_of(lantern);
    GIANT_CHECK(cell.instances.size() == 2);
    GIANT_CHECK(cell.instances[1].moves && !cell.instances[0].moves); // created after the post
    const u64 built = cell.version;
    for (int i = 0; i < 120; ++i) f.frame_step(20.0f);
    const SceneFrame::Cell& now = f.cell_of(lantern);
    // Moved in place: the same cell, told of the lantern's instance alone.
    GIANT_CHECK(now.version != built);
    const render::CellRange& r = f.range_of(lantern);
    GIANT_CHECK(r.prev_version != 0 && r.changed_first == 1 && r.changed_count == 1);
    const render::Instance& i = now.instances[1];
    GIANT_CHECK(i.position.x > 0.05); // downwind
    GIANT_CHECK(std::fabs(length(i.position - DVec3{0.0, 2.6, 0.0}) - 0.6) < 1e-3); // on its rod
    GIANT_CHECK(f.frame.instances[now.first + 1].position == i.position);
    GIANT_CHECK(now.instances[0].position == DVec3(1.0, 1.0, 0.0)); // the post untouched
    const auto s = f.run("get_swing", {{"id", lantern}});
    GIANT_CHECK(s.ok && s.value["swings"] == 1 && s.value["swing"]["angle"].get<double>() > 5.0);
    GIANT_CHECK(s.value["swing"]["size"][1].get<double>() == 0.4f);
    // The scene keeps where it hangs at rest.
    GIANT_CHECK(f.state.scene.find(lantern)->transform.position == DVec3(0.0, 2.0, 0.0));
    // Swing off: drawn where the scene has it, not a mover.
    GIANT_CHECK(f.run("set_entity", {{"id", lantern}, {"swing", {{"on", false}}}}).ok);
    f.frame_step();
    GIANT_CHECK(f.cell_of(lantern).instances[1].position == DVec3(0.0, 2.0, 0.0));
    GIANT_CHECK(!f.cell_of(lantern).instances[1].moves);
    GIANT_CHECK(!f.run("get_swing", {{"id", lantern}}).ok);
    GIANT_CHECK(f.run("get_swing").value["swings"] == 0);
    (void)post;
}

GIANT_TEST("swing: a kit swings whole; a hinged sign only across its face; edits and pushes follow") {
    Fixture f;
    GIANT_CHECK(f.run("create_kit", {{"name", "sign"},
                                     {"pieces",
                                      {{{"shape", "cube"}, {"scale", {1.0, 0.6, 0.05}}},
                                       {{"shape", "cube"}, {"position", {0.0, 0.4, 0.0}}, {"scale", {1.1, 0.05, 0.05}}}}}})
                    .ok);
    const world::EntityId sign = f.create({{"shape", "kit"},
                                           {"kit", "sign"},
                                           {"position", {0.0, 3.0, 0.0}},
                                           {"swing", {{"on", true}, {"hinge", "x"}, {"length", 0.5}, {"weight", 4.0}}}});
    // Wind along z, across its face.
    f.frame_step();
    const auto size = f.run("get_swing", {{"id", sign}}).value["swing"]["size"];
    GIANT_CHECK(std::fabs(size[0].get<double>() - 1.1) < 1e-4 && std::fabs(size[1].get<double>() - 0.725) < 1e-4);
    GIANT_CHECK(f.run("push_swing", {{"position", {0.0, 3.0, 0.0}}, {"radius", 1.0}, {"velocity", {3.0, 0.0, 4.0}}})
                    .value["pushed"] == 1);
    for (int i = 0; i < 20; ++i) f.frame_step(0.0f);
    const SceneFrame::Cell& cell = f.cell_of(sign);
    GIANT_CHECK(cell.instances.size() == 2 && cell.instances[0].moves && cell.instances[1].moves);
    const render::CellRange& r = f.range_of(sign);
    GIANT_CHECK(r.prev_version != 0 && r.changed_first == 0 && r.changed_count == 2);
    // Across its face only (z), not along its hinge (x).
    GIANT_CHECK(std::fabs(cell.instances[0].position.x) < 1e-6 && cell.instances[0].position.z > 0.01);
    // The two pieces stay as the kit has them, turned together.
    const DVec3 gap = cell.instances[1].position - cell.instances[0].position;
    GIANT_CHECK(std::fabs(length(gap) - 0.4) < 1e-4);
    // A new place: hangs from there.
    GIANT_CHECK(f.run("set_entity", {{"id", sign}, {"position", {20.0, 3.0, 0.0}}}).ok);
    f.frame_step(0.0f);
    GIANT_CHECK(std::fabs(f.run("get_swing", {{"id", sign}}).value["swing"]["pivot"][0].get<double>() - 20.0) < 1e-9);
    GIANT_CHECK(std::fabs(f.cell_of(sign).instances[0].position.x - 20.0) < 1e-6);
    // Bad pushes.
    GIANT_CHECK(!f.run("push_swing", {{"position", {0.0, 0.0, 0.0}}, {"radius", 1.0}}).ok);
    GIANT_CHECK(!f.run("push_swing", {{"position", {0.0, 0.0, 0.0}}, {"radius", 1.0}, {"impulse", 5.0},
                                      {"velocity", {1.0, 0.0, 0.0}}})
                     .ok);
    GIANT_CHECK(f.run("push_swing", {{"position", {20.0, 3.0, 0.0}}, {"radius", 1.0}, {"impulse", 5.0}}).value["pushed"] ==
                1);
    // Commands see the scene's latest edits before a frame has.
    const world::EntityId bell = f.create({{"shape", "sphere"}, {"position", {40.0, 3.0, 0.0}}, {"swing", {{"on", true}}}});
    GIANT_CHECK(f.run("get_swing", {{"id", bell}}).ok);
    GIANT_CHECK(f.run("get_swing").value["swings"] == 2);
    // A kit with a decal piece (a painted name) swings in place too, its decal with it.
    GIANT_CHECK(f.run("create_kit", {{"name", "lamp"},
                                     {"pieces",
                                      {{{"shape", "cube"}, {"scale", {0.3, 0.4, 0.3}}},
                                       {{"shape", "decal"}, {"position", {0.0, 0.0, 0.2}}, {"scale", {0.2, 0.1, 0.2}}}}}})
                    .ok);
    const world::EntityId lamp = f.create({{"shape", "kit"},
                                           {"kit", "lamp"},
                                           {"position", {60.0, 3.0, 0.0}},
                                           {"swing", {{"on", true}}}});
    f.frame_step(0.0f);
    GIANT_CHECK(f.run("push_swing", {{"position", {60.0, 3.0, 0.0}}, {"radius", 1.0}, {"velocity", {0.0, 0.0, 3.0}}}).ok);
    for (int i = 0; i < 5; ++i) f.frame_step(0.0f);
    const SceneFrame::Cell& lit = f.cell_of(lamp);
    GIANT_CHECK(f.range_of(lamp).prev_version != 0); // moved in place, not rebuilt
    GIANT_CHECK(lit.decals.size() == 1 && lit.decals[0].position.z > 0.21);
    const render::Instance& body = lit.instances.back(); // the lamp's cube (the cell's last entity)
    GIANT_CHECK(std::fabs(body.position.x - 60.0) < 1e-6);
    GIANT_CHECK(std::fabs(length(lit.decals[0].position - body.position) - 0.2) < 1e-4);
    // Never swings: cloth, decals.
    const world::EntityId flag = f.create({{"shape", "cloth"}, {"swing", {{"on", true}}}});
    GIANT_CHECK(!f.run("get_swing", {{"id", flag}}).ok);
}

GIANT_TEST("swing: a shot knocks a lantern; a blast throws lanterns and cloth; the physics' pose wins") {
    Fixture f;
    f.create({{"shape", "plane"}, {"scale", {100, 1, 100}}});
    const world::EntityId lantern = f.create({{"shape", "cube"},
                                              {"position", {5.0, 2.0, 0.0}},
                                              {"scale", {0.3, 0.4, 0.3}},
                                              {"surface", "glass"},
                                              {"swing", {{"on", true}, {"weight", 1.0}}}});
    const world::EntityId flag = f.create({{"shape", "cloth"}, {"position", {-5.0, 3.0, 0.0}}});
    for (int i = 0; i < 300; ++i) f.frame_step(0.0f);
    GIANT_CHECK(f.swing->swings().find(lantern)->asleep);
    GIANT_CHECK(f.cloth->cloths().find(flag)->asleep());
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    // A rifle shot along +x through it: it swings away along +x.
    const auto shot = f.run("fire", {{"origin", {0.0, 2.0, 0.0}}, {"direction", {1.0, 0.0, 0.0}}, {"resolve", true}});
    GIANT_CHECK(shot.ok && shot.value["hits"].size() >= 1 && shot.value["hits"][0]["entity"] == lantern);
    GIANT_CHECK(!f.swing->swings().find(lantern)->asleep);
    GIANT_CHECK(f.swing->swings().find(lantern)->velocity.x > 0.0f);
    for (int i = 0; i < 6; ++i) f.frame_step(0.0f);
    GIANT_CHECK(f.cell_of(lantern).instances[1].position.x > 5.0);
    // A blast between them throws both away from it.
    for (int i = 0; i < 400; ++i) f.frame_step(0.0f);
    const auto blast = f.run("impact", {{"point", {0.0, 2.0, 0.0}}, {"radius", 10.0}, {"impulse", 20.0}});
    GIANT_CHECK(blast.ok);
    GIANT_CHECK(!f.cloth->cloths().find(flag)->asleep());
    for (int i = 0; i < 6; ++i) f.frame_step(0.0f);
    GIANT_CHECK(f.cell_of(lantern).instances[1].position.x > 5.02);
    // A dynamic swing: while simulating the physics draws it (it falls).
    GIANT_CHECK(f.run("set_entity", {{"id", lantern}, {"body", "dynamic"}}).ok);
    for (int i = 0; i < 60; ++i) f.frame_step(0.0f);
    GIANT_CHECK(f.cell_of(lantern).instances[1].position.y < 1.0);
    GIANT_CHECK(f.run("simulate", {{"on", false}}).ok);
    f.frame_step(0.0f);
    GIANT_CHECK(f.cell_of(lantern).instances[1].position.y > 1.5); // swinging again, where it hangs
}

GIANT_TEST("swing: falling bodies and loose pieces are kept out of voxels while they move, put back once still") {
    Fixture f;
    f.create({{"shape", "plane"}, {"scale", {100, 1, 100}}});
    const world::EntityId cube = f.create({{"shape", "cube"}, {"position", {3, 3, 0}}, {"body", "dynamic"}});
    const Json wall = {{"shape", "cube"}, {"position", {0, 0.5, 0}}, {"scale", {1, 1, 1}}};
    GIANT_CHECK(f.run("create_kit", {{"name", "block"}, {"pieces", {wall}}}).ok);
    GIANT_CHECK(f.run("set_kit_breakable", {{"name", "block"}, {"breakable", true}, {"strength", 100}}).ok);
    const world::EntityId block = f.create({{"kit", "block"}, {"position", {-3, 0, 0}}});
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    // (The plane shares the cube's cell: each entity's own instance.)
    const auto drawn = [&f](world::EntityId id) -> const render::Instance& {
        const SceneFrame::Cell& c = f.cell_of(id);
        for (const SceneFrame::Slot& slot : c.slots) {
            if (slot.id == id) return c.instances[slot.first];
        }
        GIANT_CHECK(false); // drawn: each has its slot
        static const render::Instance none;
        return none;
    };
    for (int i = 0; i < 10; ++i) f.frame_step(0.0f);
    // Falling: drawn moving (in place); the block standing: not.
    GIANT_CHECK(drawn(cube).moves && !drawn(block).moves);
    GIANT_CHECK(f.run("impact", {{"point", {-2.4, 0.5, 0}}, {"radius", 1}, {"impulse", 2000}}).value["loosened"] == 1);
    for (int i = 0; i < 3; ++i) f.frame_step(0.0f);
    GIANT_CHECK(drawn(block).moves); // knocked loose, tumbling
    // Down and still a while: back in the voxel world, both.
    for (int i = 0; i < 600 && (drawn(cube).moves || drawn(block).moves); ++i) {
        f.frame_step(0.0f);
    }
    GIANT_CHECK(!drawn(cube).moves && !drawn(block).moves);
    GIANT_CHECK(std::fabs(drawn(cube).position.y - 0.5) < 0.05);
    // Knocked again, then made fixed while moving, a command catching the
    // physics up between frames: drawn where the scene has it, in the voxels.
    const world::EntityId ball = f.create({{"shape", "sphere"}, {"position", {6, 3, 0}}, {"body", "dynamic"}});
    for (int i = 0; i < 6; ++i) f.frame_step(0.0f);
    GIANT_CHECK(drawn(ball).moves);
    GIANT_CHECK(f.run("set_entity", {{"id", ball}, {"body", "fixed"}}).ok);
    GIANT_CHECK(f.run("impact", {{"point", {40, 0.5, 40}}, {"radius", 1}, {"impulse", 10}}).ok); // (catches up)
    for (int i = 0; i < 3; ++i) f.frame_step(0.0f);
    GIANT_CHECK(!f.physics.moving().contains(ball) && !drawn(ball).moves);
    GIANT_CHECK(drawn(ball).position == DVec3(6.0, 3.0, 0.0));
}
