// Cloth entities: made from commands, blown by the wind, drawn as their own
// skinned meshes once loaded, remade when their size changes, gone when
// hidden; pushed by command; never kit pieces.
#include "../src/cloth_state.h"
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"
#include "../src/scene_render.h"

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

// The commands over a scene, drawn by a null-device renderer.
struct Fixture {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<render::Renderer> renderer;
    std::unique_ptr<MeshLibrary> meshes;
    std::unique_ptr<ClothState> cloth;
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
        state.meshes = meshes.get();
        state.cloth = cloth.get();
        register_scene_commands(commands, state);
        register_cloth_commands(commands, state);
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
    // A frame as main runs one: the cloth follows, the cells are built, the
    // cloth moves on, then the frame is drawn.
    void frame_step(float wind_speed = 5.0f) {
        cloth->follow(state.scene, state.scene.untaken_changes());
        const std::vector<world::EntityId> redraw = cloth->take_redraw();
        scene_frame(state.scene, *renderer, *meshes, frame, nullptr, nullptr, nullptr, nullptr, redraw, nullptr,
                    nullptr, {}, nullptr, &cloth->meshes());
        frame.take_for_followers();
        frame.take_for_animation();
        cloth->update(wind(wind_speed), {0.0, 1.0, 6.0}, 1.0 / 60.0);
        render::FrameInput input;
        input.view.position = {0.0, 1.0, 6.0};
        input.instances = frame.instances;
        input.cells = frame.cells;
        input.materials = frame.materials;
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        renderer->render(*cmd, target, input);
        device->end_frame();
    }
    // The instance drawing `mesh`, or null.
    const render::Instance* drawing(render::MeshId mesh) const {
        for (const render::Instance& i : frame.instances) {
            if (i.mesh == mesh) return &i;
        }
        return nullptr;
    }
};

} // namespace

GIANT_TEST("cloth: a sheet's mesh is its grid, both faces, each vertex its own point's") {
    world::ClothSettings s;
    s.width = 0.3f;
    s.height = 0.3f; // 3 x 3 points
    const sim::Cloths::Sheet sheet(s);
    const geometry::SkinnedTriangleMesh m = cloth_mesh(sheet);
    GIANT_CHECK(m.mesh.vertices.size() == 18 && m.weights.size() == 18);
    GIANT_CHECK(m.mesh.indices.size() == 2 * 4 * 6); // 4 squares, 2 triangles a face, 2 faces
    GIANT_CHECK(m.weights[10].joints[0] == 1 && m.weights[10].weights[0] == 255); // the back's second
    GIANT_CHECK(m.mesh.vertices[0].normal.z == 1.0f && m.mesh.vertices[9].normal.z == -1.0f);
    // The front faces +z: its first triangle winds counter-clockwise seen from there.
    const Vec3 a = m.mesh.vertices[m.mesh.indices[0]].position, b = m.mesh.vertices[m.mesh.indices[1]].position,
               c = m.mesh.vertices[m.mesh.indices[2]].position;
    GIANT_CHECK(cross(b - a, c - a).z > 0.0f);
    const Vec3 d = m.mesh.vertices[m.mesh.indices[6]].position, e = m.mesh.vertices[m.mesh.indices[7]].position,
               f = m.mesh.vertices[m.mesh.indices[8]].position;
    GIANT_CHECK(cross(e - d, f - d).z < 0.0f);
    const geometry::SkinnedClusterMesh clusters = geometry::build_skinned_clusters(m);
    GIANT_CHECK(!clusters.mesh.empty());
}

GIANT_TEST("cloth: a cloth entity is drawn as its own mesh once loaded, moving in the wind") {
    Fixture f;
    const auto made = f.run("create_entity", {{"shape", "cloth"},
                                              {"position", {0.0, 3.0, 0.0}},
                                              {"scale", {4.0, 4.0, 4.0}},
                                              {"cloth", {{"width", 2.0}, {"height", 1.0}, {"pin", "left"}}}});
    GIANT_CHECK(made.ok);
    const world::EntityId id = made.value["id"].get<world::EntityId>();
    GIANT_CHECK(made.value["cloth"]["pin"] == "left" && made.value["cloth"]["width"] == 2.0);
    for (int i = 0; i < 20 && f.cloth->meshes().count(id) == 0; ++i) f.frame_step();
    GIANT_CHECK(f.cloth->meshes().count(id) == 1);
    f.frame_step(); // the cell drawn again with it
    const render::Instance* drawn = f.drawing(f.cloth->meshes().at(id));
    GIANT_CHECK(drawn != nullptr);
    if (drawn != nullptr) {
        GIANT_CHECK(drawn->scale.x == 1.0f && drawn->scale.y == 1.0f); // its own size, not the scale
        GIANT_CHECK(drawn->position.y == 3.0);
    }
    GIANT_CHECK(f.renderer->skinned(f.cloth->meshes().at(id)));
    const auto got = f.run("get_cloth", {{"id", id}});
    GIANT_CHECK(got.ok && got.value["sheets"] == 1 && got.value["drawn"] == 1);
    GIANT_CHECK(got.value["sheet"]["columns"] == 14 && got.value["sheet"]["rows"] == 8);
    GIANT_CHECK(got.value["sheet"]["points"].size() == 14 * 8);
    GIANT_CHECK(f.cloth->poses() > 1);
    // A new size makes a new mesh, the old one showing until it is loaded
    // (resized again meanwhile, as a drag does, it still shows); hiding it
    // takes it away.
    const render::MeshId first = f.cloth->meshes().at(id);
    for (int i = 0; i < 3; ++i) {
        GIANT_CHECK(f.run("set_entity", {{"id", id}, {"cloth", {{"height", 2.0 + 0.1 * i}}}}).ok);
        f.frame_step();
        GIANT_CHECK(f.cloth->meshes().count(id) == 1 && f.cloth->meshes().at(id) == first);
        GIANT_CHECK(f.drawing(first) != nullptr);
    }
    for (int i = 0; i < 20 && f.cloth->meshes().at(id) == first; ++i) f.frame_step();
    const render::MeshId second = f.cloth->meshes().at(id);
    GIANT_CHECK(second != first);
    f.frame_step();
    GIANT_CHECK(f.drawing(second) != nullptr && f.renderer->skinned(second));
    GIANT_CHECK(f.run("get_cloth", {{"id", id}}).value["sheet"]["rows"] == 16); // 2.2 m
    GIANT_CHECK(f.run("set_entity", {{"id", id}, {"visible", false}}).ok);
    f.frame_step();
    GIANT_CHECK(f.cloth->meshes().empty() && f.cloth->cloths().stats().sheets == 0);
    // Shown again, then turned into a cube: no sheet.
    GIANT_CHECK(f.run("set_entity", {{"id", id}, {"visible", true}}).ok);
    f.frame_step();
    GIANT_CHECK(f.cloth->cloths().stats().sheets == 1);
    GIANT_CHECK(f.run("set_entity", {{"id", id}, {"shape", "cube"}}).ok);
    f.frame_step();
    GIANT_CHECK(f.cloth->cloths().stats().sheets == 0);
    const auto cube = f.run("get_entity", {{"id", id}});
    GIANT_CHECK(cube.ok && cube.value.contains("cloth")); // kept for when it is cloth again
}

GIANT_TEST("cloth: push_cloth wakes and throws a sheet; cloth is never a kit piece") {
    Fixture f;
    const auto made = f.run("create_entity", {{"shape", "cloth"}});
    GIANT_CHECK(made.ok);
    const world::EntityId id = made.value["id"].get<world::EntityId>();
    for (int i = 0; i < 400; ++i) f.frame_step(0.0f);
    GIANT_CHECK(f.cloth->cloths().find(id)->asleep());
    GIANT_CHECK(f.run("push_cloth", {{"position", {0.0, -0.75, 1.0}}, {"radius", 2.0}, {"velocity", {0.0, 0.0, -10.0}}}).ok);
    GIANT_CHECK(!f.cloth->cloths().find(id)->asleep());
    const u64 poses = f.cloth->poses();
    f.frame_step(0.0f);
    GIANT_CHECK(f.cloth->poses() > poses);
    GIANT_CHECK(!f.run("push_cloth", {{"position", {0.0, 0.0, 0.0}}, {"radius", 0.0}, {"velocity", {0.0, 0.0, 0.0}}}).ok);
    const auto kit = f.run("create_kit", {{"name", "k"}, {"from_entities", {id}}});
    GIANT_CHECK(!kit.ok && kit.error.find("cloth") != std::string::npos);
    GIANT_CHECK(!f.run("get_cloth", {{"id", id + 100}}).ok);
    // Commands see the scene's latest edits before a frame has.
    const auto other = f.run("create_entity", {{"shape", "cloth"}, {"position", {10.0, 0.0, 0.0}}});
    GIANT_CHECK(other.ok);
    const world::EntityId second = other.value["id"].get<world::EntityId>();
    GIANT_CHECK(f.run("get_cloth", {{"id", second}}).ok);
    GIANT_CHECK(f.run("push_cloth", {{"position", {10.0, -0.75, 0.5}}, {"radius", 1.0}, {"velocity", {0.0, 0.0, -5.0}}}).ok);
    GIANT_CHECK(f.run("delete_entities", {{"ids", {second}}}).ok);
    GIANT_CHECK(!f.run("get_cloth", {{"id", second}}).ok);
}
