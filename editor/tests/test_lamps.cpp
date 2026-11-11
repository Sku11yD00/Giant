// Lamps (world::Lamp): set by commands, drawn as the renderer's local
// lights where their entity or kit piece is drawn (offset scaled and
// turned with it), following swinging lanterns in place.
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"
#include "../src/physics_state.h"
#include "../src/scene_render.h"
#include "../src/swing_state.h"

#include "giant_test.h"

#include "giant/rhi/rhi.h"
#include "giant/world/scene.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace giant;
using namespace giant::editor;
using ai::Json;

namespace {

struct Fixture {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<render::Renderer> renderer;
    std::unique_ptr<MeshLibrary> meshes;
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
        swing = std::make_unique<SwingState>(renderer.get(), meshes.get());
        state.meshes = meshes.get();
        state.swing = swing.get();
        state.physics = &physics;
        physics.take_frame_changes = [this] { return frame.take_for_followers(); };
        register_scene_commands(commands, state);
        register_physics_commands(commands, state);
        register_swing_commands(commands, state);
    }
    ~Fixture() { meshes.reset(); }
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
    // A frame as main runs one; the renderer's stats after it.
    const render::RenderStats& frame_step(float wind_speed = 0.0f) {
        swing->follow(state.scene, state.scene.untaken_changes());
        std::vector<world::EntityId> moved = physics.take_redraw();
        const std::vector<world::EntityId> swung = swing->take_moved();
        moved.insert(moved.end(), swung.begin(), swung.end());
        scene_frame(state.scene, *renderer, *meshes, frame, nullptr, nullptr, nullptr, &physics.poses(), {},
                    &physics.piece_poses(), nullptr, {}, nullptr, nullptr, moved, &swing->poses());
        frame.take_for_animation();
        physics.update(state.scene, frame.take_for_followers(), 1.0 / 60.0);
        WindState w;
        w.velocity = {wind_speed, 0.0f, 0.0f};
        swing->update(w, {0.0, 1.0, 8.0}, 1.0 / 60.0);
        render::FrameInput input;
        input.view.position = {0.0, 1.0, 8.0};
        input.instances = frame.instances;
        input.cells = frame.cells;
        input.materials = frame.materials;
        input.lights = frame.lights;
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        renderer->render(*cmd, target, input);
        device->end_frame();
        return renderer->last_stats();
    }
};

bool near(const DVec3& a, const DVec3& b, double tolerance = 1e-4) { return length(a - b) < tolerance; }

} // namespace

GIANT_TEST("lamps: an entity's lamp is drawn where it is, offset with it, and goes when off or hidden") {
    Fixture f;
    const world::EntityId post =
        f.create({{"shape", "cube"},
                  {"position", {1.0, 0.0, 0.0}},
                  {"scale", {2.0, 2.0, 2.0}},
                  {"rotation", {{"yaw", 90.0}}}, // +x turns to -z
                  {"lamp",
                   {{"on", true}, {"kind", "spot"}, {"intensity", 20.0}, {"range", 12.0},
                    {"offset", {1.0, 2.0, 0.0}}, {"inner_degrees", 20.0}, {"outer_degrees", 40.0},
                    {"color", {1.0, 0.5, 0.25}}}}});
    const auto got = f.run("get_entity", {{"id", post}});
    GIANT_CHECK(got.ok && got.value["lamp"]["on"] == true && got.value["lamp"]["kind"] == "spot");
    GIANT_CHECK(f.frame_step().lights == 1);
    GIANT_CHECK(f.frame.lights.size() == 1);
    if (f.frame.lights.size() == 1) {
        const render::LocalLight& l = f.frame.lights[0];
        // The offset scaled by 2 and turned by the yaw: (2, 4, 0) -> (0, 4, -2).
        GIANT_CHECK(near(l.position, {1.0, 4.0, -2.0}));
        GIANT_CHECK(l.kind == render::LightKind::spot && std::fabs(l.direction.y + 1.0f) < 1e-5f);
        GIANT_CHECK(l.intensity == 20.0f && l.range == 12.0f && l.color.y == 0.5f);
        GIANT_CHECK(std::fabs(l.outer_angle - 40.0f * 3.14159265f / 180.0f) < 1e-5f);
    }
    // Aimed by turning the entity: pitched 90 degrees, its -y lies along z.
    GIANT_CHECK(f.run("set_entity", {{"id", post}, {"rotation", {{"yaw", 0.0}, {"pitch", 90.0}}}}).ok);
    f.frame_step();
    GIANT_CHECK(f.frame.lights.size() == 1 && std::fabs(std::fabs(f.frame.lights[0].direction.z) - 1.0f) < 1e-4f);
    // Off: none (its settings kept). Hidden: none.
    GIANT_CHECK(f.run("set_entity", {{"id", post}, {"lamp", {{"on", false}}}}).ok);
    GIANT_CHECK(f.frame_step().lights == 0 && f.frame.lights.empty());
    GIANT_CHECK(f.run("get_entity", {{"id", post}}).value["lamp"]["range"] == 12.0);
    GIANT_CHECK(f.run("set_entity", {{"id", post}, {"lamp", {{"on", true}}}, {"visible", false}}).ok);
    f.frame_step();
    GIANT_CHECK(f.frame.lights.empty());
    GIANT_CHECK(f.run("set_entity", {{"id", post}, {"visible", true}}).ok);
    f.frame_step();
    GIANT_CHECK(f.frame.lights.size() == 1);
    // Undo twice: hidden again, then the lamp back off.
    GIANT_CHECK(f.run("undo").ok && f.run("undo").ok);
    f.frame_step();
    GIANT_CHECK(f.frame.lights.empty() && f.run("get_entity", {{"id", post}}).value["lamp"]["on"] == false);
    // Out of range values are refused.
    GIANT_CHECK(!f.run("set_entity", {{"id", post}, {"lamp", {{"range", 0.0}}}}).ok);
    GIANT_CHECK(!f.run("set_entity", {{"id", post}, {"lamp", {{"kind", "area"}}}}).ok);
    GIANT_CHECK(!f.run("set_entity", {{"id", post}, {"lamp", {{"outer_degrees", 95.0}}}}).ok);
}

GIANT_TEST("lamps: a kit piece's lamp lights every placement; changing the kit changes them all") {
    Fixture f;
    const auto made = f.run("create_kit",
                            {{"name", "lamp post"},
                             {"pieces",
                              {{{"shape", "cube"}, {"scale", {0.2, 4.0, 0.2}}, {"position", {0.0, 2.0, 0.0}}},
                               {{"shape", "sphere"},
                                {"scale", {0.3, 0.3, 0.3}},
                                {"position", {0.0, 4.2, 0.0}},
                                {"lamp", {{"on", true}, {"intensity", 8.0}}}}}}});
    GIANT_CHECK(made.ok);
    GIANT_CHECK(f.create({{"kit", "lamp post"}, {"position", {-3.0, 0.0, 0.0}}}) != 0);
    GIANT_CHECK(f.create({{"kit", "lamp post"}, {"position", {3.0, 0.0, 0.0}}}) != 0);
    GIANT_CHECK(f.frame_step().lights == 2);
    GIANT_CHECK(f.frame.lights.size() == 2);
    bool left = false, right = false;
    for (const render::LocalLight& l : f.frame.lights) {
        left = left || near(l.position, {-3.0, 4.2, 0.0});
        right = right || near(l.position, {3.0, 4.2, 0.0});
        GIANT_CHECK(l.intensity == 8.0f);
    }
    GIANT_CHECK(left && right);
    const auto kit = f.run("get_kit", {{"name", "lamp post"}});
    GIANT_CHECK(kit.ok && kit.value["pieces"][1]["lamp"]["on"] == true);
    // The kit's bulb dimmed: both placements follow.
    Json pieces = kit.value["pieces"];
    pieces[1]["lamp"] = {{"on", true}, {"intensity", 2.0}};
    const auto replaced = f.run("create_kit", {{"name", "lamp post"}, {"replace", true}, {"pieces", pieces}});
    GIANT_CHECK(replaced.ok);
    f.frame_step();
    GIANT_CHECK(f.frame.lights.size() == 2);
    for (const render::LocalLight& l : f.frame.lights) GIANT_CHECK(l.intensity == 2.0f);
}

GIANT_TEST("lamps: a kit placement's own lamp and its pieces' swing with it, moved in place") {
    Fixture f;
    GIANT_CHECK(f.run("create_kit", {{"name", "cage"},
                                     {"pieces",
                                      {{{"shape", "cube"}, {"scale", {0.3, 0.3, 0.3}}},
                                       {{"shape", "sphere"},
                                        {"scale", {0.1, 0.1, 0.1}},
                                        {"position", {0.0, -0.2, 0.0}},
                                        {"lamp", {{"on", true}, {"intensity", 1.0}}}}}}})
                    .ok);
    const world::EntityId cage = f.create({{"kit", "cage"},
                                           {"position", {0.0, 2.0, 0.0}},
                                           {"swing", {{"on", true}, {"length", 0.5}}},
                                           {"lamp", {{"on", true}, {"intensity", 5.0}, {"offset", {0.0, 0.1, 0.0}}}}});
    f.frame_step();
    GIANT_CHECK(f.frame.lights.size() == 2);
    for (int i = 0; i < 120; ++i) f.frame_step(20.0f);
    const SceneFrame::Cell& cell = f.frame.grid.at(f.frame.where.at(cage));
    GIANT_CHECK(cell.prev_version != 0 && cell.lights.size() == 2 && f.frame.lights.size() == 2);
    GIANT_CHECK(cell.instances.size() == 2);
    if (cell.instances.size() == 2 && cell.lights.size() == 2) {
        // The placement's own first, then the piece's, each where it hangs now.
        const render::Instance& box = cell.instances[0];
        GIANT_CHECK(box.position.x > 0.05);
        GIANT_CHECK(cell.lights[0].intensity == 5.0f && cell.lights[1].intensity == 1.0f);
        const Vec3 own = rotate(box.rotation, Vec3{0.0f, 0.1f, 0.0f});
        GIANT_CHECK(near(cell.lights[1].position, cell.instances[1].position));
        GIANT_CHECK(near(cell.lights[0].position, box.position + DVec3{own.x, own.y, own.z}));
    }
}

GIANT_TEST("lamps: a swinging lantern's light swings with it, moved in place") {
    Fixture f;
    const world::EntityId lantern = f.create({{"shape", "cube"},
                                              {"position", {0.0, 2.0, 0.0}},
                                              {"scale", {0.25, 0.35, 0.25}},
                                              {"swing", {{"on", true}, {"length", 0.5}, {"weight", 1.5}}},
                                              {"lamp", {{"on", true}, {"offset", {0.0, -0.2, 0.0}}}}});
    f.frame_step();
    const u64 built = f.frame.grid.at(f.frame.where.at(lantern)).version;
    for (int i = 0; i < 120; ++i) f.frame_step(20.0f);
    const SceneFrame::Cell& cell = f.frame.grid.at(f.frame.where.at(lantern));
    GIANT_CHECK(cell.version != built && cell.instances.size() == 1 && cell.lights.size() == 1);
    GIANT_CHECK(cell.prev_version != 0); // moved in place, not rebuilt
    GIANT_CHECK(f.frame.lights.size() == 1);
    if (cell.instances.size() == 1 && f.frame.lights.size() == 1) {
        const render::Instance& i = cell.instances[0];
        GIANT_CHECK(i.position.x > 0.05); // swung downwind
        // The light at the lantern's offset, as it hangs now (scaled and turned with it).
        const Vec3 off = rotate(i.rotation, Vec3{0.0f, -0.2f * 0.35f, 0.0f});
        GIANT_CHECK(near(f.frame.lights[0].position, i.position + DVec3{off.x, off.y, off.z}));
    }
}

GIANT_TEST("lamps: shadows on give each lamp its own id, the same frame to frame; the renderer casts them") {
    Fixture f;
    GIANT_CHECK(f.run("create_kit", {{"name", "pair"},
                                     {"pieces",
                                      {{{"shape", "sphere"},
                                        {"scale", {0.1, 0.1, 0.1}},
                                        {"position", {-0.5, 0.0, 0.0}},
                                        {"lamp", {{"on", true}, {"shadows", true}}}},
                                       {{"shape", "sphere"},
                                        {"scale", {0.1, 0.1, 0.1}},
                                        {"position", {0.5, 0.0, 0.0}},
                                        {"lamp", {{"on", true}, {"shadows", true}}}}}}})
                    .ok);
    const world::EntityId floor = f.create({{"shape", "plane"}, {"scale", {20.0, 1.0, 20.0}}});
    const world::EntityId a = f.create({{"kit", "pair"}, {"position", {-2.0, 2.0, 0.0}},
                                        {"lamp", {{"on", true}, {"shadows", true}, {"shadow_near", 0.4}}}});
    const world::EntityId b = f.create({{"shape", "cube"},
                                        {"position", {2.0, 2.0, 0.0}},
                                        {"scale", {0.2, 0.2, 0.2}},
                                        {"lamp", {{"on", true}}}}); // no shadows
    GIANT_CHECK(floor != 0 && a != 0 && b != 0);
    const auto got = f.run("get_entity", {{"id", a}});
    GIANT_CHECK(got.ok && got.value["lamp"]["shadows"] == true && got.value["lamp"]["shadow_near"] == 0.4f);
    const render::RenderStats& st = f.frame_step();
    GIANT_CHECK(st.lights == 4 && st.local_shadows == 3 && st.local_shadow_drawn == 18);
    GIANT_CHECK(f.frame.lights.size() == 4);
    std::vector<u32> ids;
    for (const render::LocalLight& l : f.frame.lights) {
        if (l.shadow_id != 0) ids.push_back(l.shadow_id);
        if (l.shadow_id != 0 && near(l.position, {-2.0, 2.0, 0.0})) GIANT_CHECK(std::fabs(l.shadow_near - 0.4f) < 1e-6f);
    }
    GIANT_CHECK(ids.size() == 3);
    std::sort(ids.begin(), ids.end());
    GIANT_CHECK(std::unique(ids.begin(), ids.end()) == ids.end());
    // Moved: the same ids, drawn again; a frame later nothing to draw.
    GIANT_CHECK(f.run("set_entity", {{"id", a}, {"position", {-2.0, 2.5, 0.0}}}).ok);
    GIANT_CHECK(f.frame_step().local_shadow_drawn == 18);
    std::vector<u32> again;
    for (const render::LocalLight& l : f.frame.lights)
        if (l.shadow_id != 0) again.push_back(l.shadow_id);
    std::sort(again.begin(), again.end());
    GIANT_CHECK(again == ids);
    GIANT_CHECK(f.frame_step().local_shadow_drawn == 0);
    // Turned off: no shadows from it, the light stays.
    GIANT_CHECK(f.run("set_entity", {{"id", a}, {"lamp", {{"shadows", false}}}}).ok);
    const render::RenderStats& off = f.frame_step();
    GIANT_CHECK(off.lights == 4 && off.local_shadows == 2);
    GIANT_CHECK(!f.run("set_entity", {{"id", a}, {"lamp", {{"shadow_near", 0.0}}}}).ok);
}
