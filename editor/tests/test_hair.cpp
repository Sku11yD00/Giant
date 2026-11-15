// Hair on entities: grown from the top of a cube or sphere on a worker,
// handed to the renderer and drawn, simulated near the camera, grown again
// when what it grows from changes, gone with its entity; read by command.
#include "../src/editor_commands.h"
#include "../src/hair_state.h"
#include "../src/mesh_library.h"

#include "../src/animation_state.h"

#include "giant_test.h"
#include "gltf_test_files.h"

#include "giant/rhi/rhi.h"
#include "giant/world/scene.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>

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
    std::unique_ptr<HairState> hair;
    ai::CommandRegistry commands;
    EditorState state;
    rhi::TextureHandle target;
    DVec3 camera{0.0, 1.0, 4.0};
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
        hair = std::make_unique<HairState>(renderer.get());
        state.meshes = meshes.get();
        state.hair = hair.get();
        register_scene_commands(commands, state);
        register_hair_commands(commands, state);
    }
    ~Fixture() {
        hair.reset(); // grooms leave the renderer first
        meshes.reset();
    }
    ai::CommandResult run(const char* name, const Json& args = Json::object()) {
        ai::ExecuteOptions options;
        options.record_history = true;
        return commands.execute(name, args, options);
    }
    // A frame as main runs one: the hair follows, the frame is drawn with
    // what it listed last, then the hair moves on.
    render::RenderStats frame_step(float wind_speed = 0.0f) {
        hair->follow(state.scene, state.scene.untaken_changes());
        state.scene.take_changes();
        render::FrameInput input;
        input.view.position = camera;
        input.settings.gpu_culling = false;
        input.hair = hair->draws();
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        renderer->render(*cmd, target, input);
        device->end_frame();
        hair->update(wind(wind_speed), camera, 1.0 / 60.0);
        return renderer->last_stats();
    }
    world::EntityId head(const Json& hair_args, const char* shape = "sphere") {
        const ai::CommandResult r = run("create_entity", {{"name", "Head"},
                                                          {"shape", shape},
                                                          {"position", {0.0, 1.0, 0.0}},
                                                          {"scale", {0.3, 0.3, 0.3}},
                                                          {"hair", hair_args}});
        GIANT_CHECK(r.ok);
        return r.ok ? r.value["id"].get<world::EntityId>() : 0;
    }
};

} // namespace

GIANT_TEST("hair: the scalp is the top of the shape, sized as its entity") {
    const geometry::TriangleMesh half = hair_scalp(world::Shape::sphere, {0.2f, 0.4f, 0.2f}, 0.5f);
    GIANT_CHECK(!half.indices.empty() && half.indices.size() % 3 == 0 && half.indices.size() == half.vertices.size());
    float lowest = 1.0f, highest = -1.0f, widest = 0.0f;
    for (const geometry::Vertex& v : half.vertices) {
        lowest = std::min(lowest, v.position.y);
        highest = std::max(highest, v.position.y);
        widest = std::max(widest, std::abs(v.position.x));
    }
    GIANT_CHECK_NEAR(highest, 0.2, 1e-4); // half its height of 0.4
    GIANT_CHECK_NEAR(lowest, 0.0, 1e-5);  // cut at its middle
    GIANT_CHECK(widest > 0.09f && widest <= 0.1001f);
    // Its normals are the sized shape's: unit, and on a ball pressed flat they stand up more.
    const geometry::TriangleMesh flat = hair_scalp(world::Shape::sphere, {1.0f, 0.1f, 1.0f}, 0.5f);
    for (const geometry::Vertex& v : flat.vertices) {
        GIANT_CHECK_NEAR(length(v.normal), 1.0, 1e-4);
        const Vec3 round = normalize_or_zero(Vec3{v.position.x, v.position.y * 10.0f, v.position.z});
        GIANT_CHECK(v.normal.y >= round.y - 1e-3f);
    }
    const geometry::TriangleMesh cap = hair_scalp(world::Shape::sphere, {1.0f, 1.0f, 1.0f}, 0.1f);
    GIANT_CHECK(!cap.indices.empty() && cap.indices.size() < half.indices.size());
    const geometry::TriangleMesh all = hair_scalp(world::Shape::sphere, {1.0f, 1.0f, 1.0f}, 1.0f);
    GIANT_CHECK(all.indices.size() == geometry::make_sphere().indices.size());
    // A cube's top face and its sides down to the cut, and no further (not
    // half of each side down to the corners); no scalp on other shapes.
    for (const float share : {0.1f, 0.5f}) {
        const geometry::TriangleMesh box = hair_scalp(world::Shape::cube, {1.0f, 1.0f, 1.0f}, share);
        float bottom = 1.0f;
        double area = 0.0;
        for (usize i = 0; i + 2 < box.vertices.size(); i += 3) {
            for (usize c = 0; c < 3; ++c) bottom = std::min(bottom, box.vertices[i + c].position.y);
            area += 0.5 * static_cast<double>(length(cross(box.vertices[i + 1].position - box.vertices[i].position,
                                                           box.vertices[i + 2].position - box.vertices[i].position)));
        }
        GIANT_CHECK_NEAR(bottom, 0.5f - share, 1e-5);
        GIANT_CHECK_NEAR(area, 1.0 + 4.0 * static_cast<double>(share), 1e-4);
    }
    // No size one way: flat, and still no number that is none in it.
    for (const geometry::Vertex& v : hair_scalp(world::Shape::cube, {0.0f, 1.0f, 1.0f}, 0.5f).vertices) {
        GIANT_CHECK(std::isfinite(v.normal.x) && std::isfinite(v.position.x));
    }
    GIANT_CHECK(hair_scalp(world::Shape::plane, {1.0f, 1.0f, 1.0f}, 0.5f).indices.empty());
    GIANT_CHECK(hair_scalp(world::Shape::mesh, {1.0f, 1.0f, 1.0f}, 0.5f).indices.empty());
    // Numbers that are none: the default share.
    GIANT_CHECK(hair_scalp(world::Shape::sphere, {1.0f, 1.0f, 1.0f}, std::nanf("")).indices.size() ==
                hair_scalp(world::Shape::sphere, {1.0f, 1.0f, 1.0f}, 0.5f).indices.size());
}

GIANT_TEST("hair: put on an entity it grows on a worker, is drawn, and moves in the wind near the camera") {
    Fixture f;
    const world::EntityId id = f.head({{"on", true}, {"strands", 2000}, {"guides", 32}, {"length", 0.3}, {"melanin", 0.9}});
    GIANT_CHECK(id != 0);
    f.frame_step();
    Json got = f.run("get_hair").value;
    GIANT_CHECK(got["wearing"] == 1);
    // Grown over the next frames (waited for here), then uploaded, then drawn.
    got = f.run("get_hair", {{"wait", true}, {"id", id}, {"points", true}}).value;
    GIANT_CHECK(got["grown"] == 1 && got["growing"] == 0 && got["strands"] == 2000);
    GIANT_CHECK(got["hair"]["grown"] == true && got["hair"]["guides"] == 32 && got["hair"]["points"].size() == 32 * 16);
    GIANT_CHECK(got["hair"]["longest"].get<float>() > 0.2f && got["hair"]["longest"].get<float>() <= 0.3001f);
    // Grown on the scaled shape: its roots on a ball 0.3 m wide, so within 0.15 m + its length of the middle.
    GIANT_CHECK(got["hair"]["bounds_max"][1].get<float>() > 0.14f && got["hair"]["bounds_max"][1].get<float>() < 0.46f);
    GIANT_CHECK(got["hair"]["bounds_min"][0].get<float>() > -0.46f);
    const Json rest = got["hair"]["points"];
    render::RenderStats stats;
    for (int i = 0; i < 6; ++i) stats = f.frame_step();
    GIANT_CHECK(stats.hair_grooms == 1 && stats.hair_strands > 0 && stats.hair_left_out == 0);
    got = f.run("get_hair", {{"id", id}}).value;
    GIANT_CHECK(got["drawn"] == 1 && got["simulated"] == 1 && got["hair"]["simulated"] == true);
    // A gale carries its tips downwind (+x); its roots stay on the scalp.
    for (int i = 0; i < 90; ++i) f.frame_step(25.0f);
    got = f.run("get_hair", {{"id", id}, {"points", true}}).value;
    const Json blown = got["hair"]["points"];
    double tips = 0.0, roots = 0.0;
    for (usize g = 0; g < 32; ++g) {
        tips += blown[g * 16 + 15][0].get<double>() - rest[g * 16 + 15][0].get<double>();
        roots += std::abs(blown[g * 16][0].get<double>() - rest[g * 16][0].get<double>());
    }
    GIANT_CHECK(tips / 32.0 > 0.02);
    GIANT_CHECK(roots / 32.0 < 1e-4);
    GIANT_CHECK(got["resets"] == 0);
    // Told to keep still, or far from the camera: it hangs as grown.
    GIANT_CHECK(f.run("set_entity", {{"id", id}, {"hair", {{"moves", false}}}}).ok);
    f.frame_step(25.0f);
    f.frame_step(25.0f);
    got = f.run("get_hair", {{"id", id}, {"points", true}}).value;
    GIANT_CHECK(got["simulated"] == 0 && got["hair"]["points"] == rest && got["drawn"] == 1);
    GIANT_CHECK(f.run("set_entity", {{"id", id}, {"hair", {{"moves", true}}}}).ok);
    f.camera = {0.0, 1.0, 200.0};
    f.frame_step(25.0f);
    f.frame_step(25.0f);
    GIANT_CHECK(f.run("get_hair").value["simulated"] == 0);
}

GIANT_TEST("hair: it grows again when what it grows from changes, and not for its look; it goes with its entity") {
    Fixture f;
    const world::EntityId id = f.head({{"on", true}, {"strands", 500}, {"guides", 8}});
    f.frame_step();
    Json got = f.run("get_hair", {{"wait", true}, {"id", id}}).value;
    GIANT_CHECK(got["strands"] == 500);
    for (int i = 0; i < 4; ++i) f.frame_step();
    GIANT_CHECK(f.renderer->last_stats().hair_grooms == 1);
    // Its look: the same groom, no growing.
    GIANT_CHECK(f.run("set_entity", {{"id", id}, {"hair", {{"melanin", 0.1}, {"width", 0.0002}}}}).ok);
    f.frame_step();
    got = f.run("get_hair", {{"id", id}}).value;
    GIANT_CHECK(got["growing"] == 0 && got["grown"] == 1);
    // More strands: grown again; the old groom shows meanwhile.
    GIANT_CHECK(f.run("set_entity", {{"id", id}, {"hair", {{"strands", 900}}}}).ok);
    // (Nothing waited for: every frame until the new one shows draws hair, never none.)
    bool swapped = false;
    for (int i = 0; i < 2000 && !swapped; ++i) {
        const render::RenderStats drawn = f.frame_step();
        GIANT_CHECK(drawn.hair_grooms == 1 && drawn.hair_left_out == 0);
        got = f.run("get_hair", {{"id", id}}).value;
        GIANT_CHECK(got["wearing"] == 1 && got["hair"]["grown"] == true && got["drawn"] == 1);
        swapped = got["hair"]["strands"] == 900;
        GIANT_CHECK(swapped || (got["hair"]["strands"] == 500 && got["growing"] == 1));
    }
    GIANT_CHECK(swapped && got["growing"] == 0 && got["hair"]["ready"] == true);
    GIANT_CHECK(f.frame_step().hair_grooms == 1);
    // Waited for, it is read at once, and what is listed is what is kept:
    // a frame drawn before the next update names no groom that is gone.
    GIANT_CHECK(f.run("set_entity", {{"id", id}, {"hair", {{"strands", 700}}}}).ok);
    got = f.run("get_hair", {{"wait", true}, {"id", id}}).value;
    GIANT_CHECK(got["strands"] == 700 && got["growing"] == 0 && got["drawn"] == 1);
    for (int i = 0; i < 4; ++i) f.frame_step();
    GIANT_CHECK(f.renderer->last_stats().hair_grooms == 1 && f.renderer->last_stats().hair_left_out == 0);
    GIANT_CHECK(f.run("set_entity", {{"id", id}, {"hair", {{"strands", 900}}}}).ok);
    got = f.run("get_hair", {{"wait", true}, {"id", id}}).value;
    GIANT_CHECK(got["strands"] == 900 && got["growing"] == 0);
    // A new scale too (hair is grown on the shape as sized).
    const float before = got["hair"]["bounds_max"][0].get<float>();
    GIANT_CHECK(f.run("set_entity", {{"id", id}, {"scale", {0.6, 0.3, 0.3}}}).ok);
    f.frame_step();
    got = f.run("get_hair", {{"wait", true}, {"id", id}}).value;
    GIANT_CHECK(got["hair"]["bounds_max"][0].get<float>() > before + 0.1f);
    for (int i = 0; i < 4; ++i) f.frame_step();
    GIANT_CHECK(f.renderer->last_stats().hair_grooms == 1);
    // Shapes that grow none keep the settings and wear nothing.
    GIANT_CHECK(f.run("set_entity", {{"id", id}, {"shape", "plane"}}).ok);
    f.frame_step();
    GIANT_CHECK(f.run("get_hair").value["wearing"] == 0);
    GIANT_CHECK(!f.run("get_hair", {{"id", id}}).ok);
    GIANT_CHECK(f.renderer->last_stats().hair_grooms == 0 || f.frame_step().hair_grooms == 0);
    GIANT_CHECK(f.run("get_entity", {{"id", id}}).value["hair"]["strands"] == 900);
    GIANT_CHECK(f.run("set_entity", {{"id", id}, {"shape", "cube"}}).ok);
    f.frame_step();
    GIANT_CHECK(f.run("get_hair", {{"wait", true}}).value["grown"] == 1);
    // Hidden, off, removed while growing, undone: nothing left behind.
    GIANT_CHECK(f.run("set_entity", {{"id", id}, {"visible", false}}).ok);
    f.frame_step();
    GIANT_CHECK(f.run("get_hair").value["wearing"] == 0);
    GIANT_CHECK(f.run("set_entity", {{"id", id}, {"visible", true}, {"hair", {{"strands", 5000}}}}).ok);
    f.frame_step(); // (growing)
    GIANT_CHECK(f.run("delete_entities", {{"ids", {id}}}).ok);
    f.frame_step();
    got = f.run("get_hair").value;
    GIANT_CHECK(got["wearing"] == 0 && got["growing"] == 0 && got["drawn"] == 0);
    GIANT_CHECK(f.run("undo").ok);
    f.frame_step();
    GIANT_CHECK(f.run("get_hair", {{"wait", true}}).value["strands"] == 5000);
    // Out-of-range settings are refused whole.
    GIANT_CHECK(!f.run("set_entity", {{"id", id}, {"hair", {{"strands", 0}}}}).ok);
    GIANT_CHECK(!f.run("set_entity", {{"id", id}, {"hair", {{"length", 100.0}}}}).ok);
    GIANT_CHECK(!f.run("set_entity", {{"id", id}, {"hair", {{"colour", 1}}}}).ok);
}

GIANT_TEST("hair: more wear it than are kept or moved, the lowest ids and the nearest are; none needs a renderer") {
    ai::CommandRegistry commands;
    EditorState state;
    HairState hair(nullptr);
    state.hair = &hair;
    register_scene_commands(commands, state);
    register_hair_commands(commands, state);
    ai::ExecuteOptions options;
    options.record_history = true;
    // 260 small heads in a row along x, a metre apart.
    Json heads = Json::array();
    for (int i = 0; i < 260; ++i) {
        heads.push_back({{"shape", i % 2 == 0 ? "sphere" : "cube"},
                         {"position", {static_cast<double>(i), 0.0, 0.0}},
                         {"scale", {0.2, 0.2, 0.2}},
                         {"hair", {{"on", true}, {"strands", 64}, {"guides", 8}}}});
    }
    GIANT_CHECK(commands.execute("create_entities", {{"entities", heads}}, options).ok);
    Json got = commands.execute("get_hair", {{"wait", true}}, options).value;
    GIANT_CHECK(got["wearing"] == 260 && got["grown"] == 256 && got["left_out"] == 4 && got["growing"] == 0);
    GIANT_CHECK(got["strands"] == 256 * 64 && got["drawn"] == 0);
    // Those started moving: two an update, the nearest 16 in the end.
    WindState still;
    const DVec3 camera{100.0, 0.0, 0.0};
    hair.update(still, camera, 1.0 / 60.0);
    GIANT_CHECK(hair.stats().simulated == HairState::k_sims_made_per_update);
    for (int i = 0; i < 20; ++i) hair.update(still, camera, 1.0 / 60.0);
    GIANT_CHECK(hair.stats().simulated == HairState::k_max_simulated);
    // The camera a little along: those moving keep moving (no trading places at the edge).
    const u64 before = hair.stats().resets;
    for (int i = 0; i < 20; ++i) hair.update(still, DVec3{100.4 + 0.2 * (i % 2), 0.0, 0.0}, 1.0 / 60.0);
    GIANT_CHECK(hair.stats().simulated == HairState::k_max_simulated && hair.stats().resets == before);
    // The last ones are left out until others go: then they grow.
    HairState::View view;
    const world::EntityId last = state.scene.entities().back().id;
    GIANT_CHECK(hair.view(last, view) && view.left_out && !view.grown);
    Json gone = Json::array();
    for (usize i = 0; i < 10; ++i) gone.push_back(state.scene.entities()[i].id);
    GIANT_CHECK(commands.execute("delete_entities", {{"ids", gone}}, options).ok);
    got = commands.execute("get_hair", {{"wait", true}}, options).value;
    GIANT_CHECK(got["wearing"] == 250 && got["grown"] == 250 && got["left_out"] == 0);
    GIANT_CHECK(hair.view(last, view) && !view.left_out && view.grown && !view.ready);
    // A scene taken away while grooms grow leaves nothing.
    GIANT_CHECK(commands.execute("set_entity", {{"id", last}, {"hair", {{"strands", 20000}}}}, options).ok);
    hair.follow(state.scene, state.scene.untaken_changes());
    hair.update(still, camera, 1.0 / 60.0);
    state.scene = world::Scene{};
    hair.follow(state.scene, state.scene.untaken_changes());
    GIANT_CHECK(hair.stats().wearing == 0 && hair.stats().growing == 0 && hair.draws().empty());
}

namespace {

// The two-joint arm (geometry tests): a strip 0.2 m wide from y = 0 to 2,
// facing +z, its lower half the shoulder's and its upper the elbow's (at
// y = 1), which also carries a sword; clip "bend" turns the elbow a quarter turn about z over 1 s.
std::string hair_file(const char* name, const std::vector<u8>& bytes) {
    // (A folder of this run's own: two build trees may test at once.)
    static const std::string own = "giant_hair_tests_" + std::to_string(std::random_device{}());
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / own;
    std::filesystem::create_directories(dir);
    const std::filesystem::path path = dir / name;
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                static_cast<std::streamsize>(bytes.size()));
    return path.generic_string();
}

} // namespace

GIANT_TEST("hair: on a character it grows from the skin its joint moves, rides the joint and feels the body") {
    const std::string arm = hair_file("arm.glb", geometry::testing::skinned_arm());
    MeshLibrary meshes(nullptr);
    AnimationState animation(nullptr);
    HairState hair(nullptr, &meshes);
    ai::CommandRegistry commands;
    EditorState state;
    state.meshes = &meshes;
    state.animation = &animation;
    state.hair = &hair;
    register_scene_commands(commands, state);
    register_animation_commands(commands, state);
    register_hair_commands(commands, state);
    ai::ExecuteOptions options;
    options.record_history = true;
    const auto run = [&](const char* name, const Json& args) { return commands.execute(name, args, options); };

    // The scalp: the elbow's half of the strip, its upper half by `top`, in
    // the elbow's space (its origin at y = 1), twice the size at scale 2.
    const MeshLibrary::Loaded loaded = meshes.load(arm);
    GIANT_CHECK(loaded.mesh != nullptr && loaded.mesh->skinned != nullptr);
    if (loaded.mesh == nullptr || loaded.mesh->skinned == nullptr) return;
    const MeshLibrary::Skinned& character = *loaded.mesh->skinned;
    const u32 elbow = hair_joint(character.skeleton, "elbow");
    const u32 shoulder = hair_joint(character.skeleton, "shoulder");
    GIANT_CHECK(elbow != k_no_hair_joint && shoulder != k_no_hair_joint && elbow != shoulder);
    GIANT_CHECK(hair_joint(character.skeleton, "") == k_no_hair_joint); // none named like a head
    GIANT_CHECK(hair_joint(character.skeleton, "Elbow") == k_no_hair_joint);
    const geometry::TriangleMesh scalp = hair_scalp(character, elbow, 2.0f, 0.5f);
    float low = 10.0f, high = -10.0f, wide = 0.0f;
    double area = 0.0;
    for (usize i = 0; i + 2 < scalp.vertices.size(); i += 3) {
        for (usize c = 0; c < 3; ++c) {
            const geometry::Vertex& v = scalp.vertices[i + c];
            low = std::min(low, v.position.y);
            high = std::max(high, v.position.y);
            wide = std::max(wide, std::abs(v.position.x));
            GIANT_CHECK_NEAR(v.normal.z, 1.0, 1e-4);
        }
        area += 0.5 * static_cast<double>(length(cross(scalp.vertices[i + 1].position - scalp.vertices[i].position,
                                                       scalp.vertices[i + 2].position - scalp.vertices[i].position)));
    }
    // (With the sword the elbow carries, its skin runs from y = 1 to 2.5 as
    // bound: the upper half from 1.75, so 0.75 to 1.5 above the joint, x 2.)
    GIANT_CHECK_NEAR(low, 1.5, 1e-4);
    GIANT_CHECK_NEAR(high, 3.0, 1e-4);
    GIANT_CHECK(wide >= 0.2f - 1e-4f && area > 0.1);
    // A skeleton under a scaled node with its joints' own axes turned (inverse binds 100 times and
    // turned back, joints a hundredth and turned: as exported in centimetres, z up) grows the same
    // scalp: the size it is drawn, upright as it stands.
    MeshLibrary::Skinned exported = character;
    const Quat quarter = quat_from_axis_angle({1.0f, 0.0f, 0.0f}, 1.5707963f);
    const Mat4 turned = mat4_from_transform({}, quarter, {0.01f, 0.01f, 0.01f});
    const Mat4 back = mat4_from_transform({}, Quat{-quarter.x, -quarter.y, -quarter.z, quarter.w}, {100.0f, 100.0f, 100.0f});
    for (Mat4& m : exported.skeleton.inverse_bind) m = back * m;
    for (Mat4& m : exported.rest_model) m = m * turned;
    const geometry::TriangleMesh same = hair_scalp(exported, elbow, 2.0f, 0.5f);
    GIANT_CHECK(same.vertices.size() == scalp.vertices.size());
    for (usize i = 0; i < same.vertices.size() && i < scalp.vertices.size(); ++i) {
        GIANT_CHECK(length(same.vertices[i].position - scalp.vertices[i].position) < 1e-3f);
    }
    // From the shoulder down it is the whole arm's skin; a joint past the skeleton has none.
    GIANT_CHECK(hair_scalp(character, shoulder, 1.0f, 1.0f).indices.size() > hair_scalp(character, elbow, 1.0f, 1.0f).indices.size());
    GIANT_CHECK(hair_scalp(character, 99, 1.0f, 0.5f).indices.empty());

    // On an entity: grown in the joint's space, drawn at the joint.
    ai::CommandResult made = run("create_entity", {{"shape", "mesh"},
                                                   {"mesh", arm},
                                                   {"position", {10.0, 0.0, 0.0}},
                                                   {"hair", {{"on", true}, {"joint", "elbow"}, {"strands", 300}, {"guides", 8}, {"length", 0.2}}}});
    GIANT_CHECK(made.ok);
    const world::EntityId id = made.ok ? made.value["id"].get<world::EntityId>() : 0;
    Json got = run("get_hair", {{"wait", true}, {"id", id}}).value;
    GIANT_CHECK(got["grown"] == 1 && got["hair"]["strands"] == 300 && got["hair"]["problem"] == "" && got["hair"]["joint"] == "elbow");
    GIANT_CHECK_NEAR(got["hair"]["position"][0].get<double>(), 10.0, 1e-4);
    GIANT_CHECK_NEAR(got["hair"]["position"][1].get<double>(), 1.0, 1e-4);
    GIANT_CHECK_NEAR(got["hair"]["bounds_min"][1].get<double>(), 0.75, 0.21); // roots from 0.75 up, hanging at most 0.2 below
    GIANT_CHECK(got["hair"]["capsules"].get<u32>() <= 16);
    GIANT_CHECK(got["hair"]["capsules"].get<usize>() == std::min<usize>(character.capsules.size(), 16));

    // Animated, it rides the joint: bent a quarter turn about z, the joint's up points along -x.
    GIANT_CHECK(run("set_animation", {{"id", id}, {"clip", "bend"}, {"loop", false}}).ok);
    for (int i = 0; i < 80; ++i) animation.update(state.scene, meshes, state.scene.untaken_changes(), 1.0 / 60.0);
    hair.follow(state.scene, state.scene.untaken_changes());
    hair.place(&animation, nullptr, nullptr, nullptr);
    HairState::View view;
    GIANT_CHECK(hair.view(id, view));
    Vec3 up = rotate(view.rotation, Vec3{0.0f, 1.0f, 0.0f});
    GIANT_CHECK(up.x < -0.95f && std::abs(up.y) < 0.2f);
    GIANT_CHECK_NEAR(view.position.x, 10.0, 1e-3);
    GIANT_CHECK_NEAR(view.position.y, 1.0, 1e-3);
    // Carried (as the physics or a walker moves its entity) it goes along; then it moves, pushed by the body.
    DrawnPoses carried;
    world::Transform there;
    there.position = {20.0, 3.0, 0.0};
    carried[id] = there;
    hair.place(&animation, nullptr, &carried, nullptr);
    GIANT_CHECK(hair.view(id, view));
    GIANT_CHECK_NEAR(view.position.x, 20.0, 1e-3);
    GIANT_CHECK_NEAR(view.position.y, 4.0, 1e-3);
    WindState still;
    for (int i = 0; i < 30; ++i) hair.update(still, DVec3{20.0, 4.0, 2.0}, 1.0 / 60.0);
    // Another file loaded meanwhile (every character is looked up again) leaves it where it was placed.
    std::vector<u8> other;
    geometry::testing::append(other, std::vector<float>{0, 0, 0, 2, 0, 0, 0, 2, 0});
    geometry::testing::append(other, std::vector<u32>{0, 1, 2});
    const std::string loaded_since = hair_file(
        "other.glb",
        geometry::testing::make_glb(
            R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
            R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],"buffers":[{"byteLength":48}],)"
            R"("bufferViews":[{"buffer":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":12}],)"
            R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[2,2,0]},)"
            R"({"bufferView":1,"componentType":5125,"count":3,"type":"SCALAR"}]})",
            other));
    GIANT_CHECK(meshes.load(loaded_since).mesh != nullptr);
    hair.update(still, DVec3{20.0, 4.0, 2.0}, 1.0 / 60.0);
    GIANT_CHECK(hair.view(id, view));
    GIANT_CHECK_NEAR(view.position.x, 20.0, 1e-3);
    GIANT_CHECK_NEAR(view.position.y, 4.0, 1e-3);
    GIANT_CHECK(hair.stats().simulated == 1 && hair.stats().resets == 0);
    GIANT_CHECK(hair.view(id, view) && view.simulated && view.guide_points.size() == 8 * 16);
    for (const Vec3& p : view.guide_points) GIANT_CHECK(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));

    // Another joint: grown again from that joint's skin (the whole arm, so lower in its space). Until
    // it is grown the old groom stays on the joint it was grown round.
    GIANT_CHECK(run("set_entity", {{"id", id}, {"hair", {{"joint", "shoulder"}, {"top", 1.0}}}}).ok);
    hair.follow(state.scene, state.scene.untaken_changes());
    hair.place(&animation, nullptr, &carried, nullptr);
    GIANT_CHECK(hair.view(id, view) && view.grown);
    GIANT_CHECK_NEAR(view.position.y, 4.0, 1e-3); // the elbow's
    got = run("get_hair", {{"wait", true}, {"id", id}}).value;
    hair.place(&animation, nullptr, &carried, nullptr);
    GIANT_CHECK(hair.view(id, view));
    GIANT_CHECK_NEAR(view.position.y, 3.0, 1e-3); // the shoulder's
    GIANT_CHECK(got["hair"]["joint"] == "shoulder" && got["hair"]["grown"] == true);
    GIANT_CHECK(got["hair"]["bounds_min"][1].get<double>() < 0.1);
    // No such joint, none named like a head, a file without a skeleton: nothing grown, and why.
    GIANT_CHECK(run("set_entity", {{"id", id}, {"hair", {{"joint", "nose"}}}}).ok);
    got = run("get_hair", {{"wait", true}, {"id", id}}).value;
    GIANT_CHECK(got["wearing"] == 1 && got["grown"] == 0 && got["growing"] == 0 && got["left_out"] == 0);
    GIANT_CHECK(got["hair"]["problem"].get<std::string>().find("nose") != std::string::npos && got["hair"]["grown"] == false);
    GIANT_CHECK(run("set_entity", {{"id", id}, {"hair", {{"joint", ""}}}}).ok);
    got = run("get_hair", {{"wait", true}, {"id", id}}).value;
    GIANT_CHECK(got["hair"]["problem"].get<std::string>().find("head") != std::string::npos);
    std::vector<u8> bin;
    geometry::testing::append(bin, std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1, 0});
    geometry::testing::append(bin, std::vector<u32>{0, 1, 2});
    const std::string plain = hair_file(
        "triangle.glb",
        geometry::testing::make_glb(
            R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
            R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],"buffers":[{"byteLength":48}],)"
            R"("bufferViews":[{"buffer":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":12}],)"
            R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
            R"({"bufferView":1,"componentType":5125,"count":3,"type":"SCALAR"}]})",
            bin));
    GIANT_CHECK(run("set_entity", {{"id", id}, {"mesh", plain}}).ok);
    got = run("get_hair", {{"wait", true}, {"id", id}}).value;
    GIANT_CHECK(got["hair"]["problem"].get<std::string>().find("skeleton") != std::string::npos && got["grown"] == 0);
    // Back on the arm with its joint named: it grows again.
    GIANT_CHECK(run("set_entity", {{"id", id}, {"mesh", arm}, {"hair", {{"joint", "elbow"}}}}).ok);
    got = run("get_hair", {{"wait", true}, {"id", id}}).value;
    GIANT_CHECK(got["grown"] == 1 && got["hair"]["problem"] == "" && got["hair"]["strands"] == 300);
}
