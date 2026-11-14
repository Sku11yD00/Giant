// Skinned entities playing their clips: the saved animation, the view's
// commands, the limit on how many animate, and their own posed copies drawn.
#include "../src/animation_state.h"
#include "../src/editor_commands.h"
#include "../src/hit_targets.h"
#include "../src/physics_state.h"
#include "../src/mesh_library.h"
#include "../src/scene_render.h"

#include "giant_test.h"
#include "gltf_test_files.h"

#include "giant/rhi/rhi.h"
#include "giant/world/scene.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace giant;
using namespace giant::editor;
using ai::Json;

namespace {

// The two-joint arm (geometry tests): clips "bend" (the elbow a quarter
// turn about z over 1 s, its top swinging to x = -1), "hop", "bend (2)".
std::string arm_file() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "giant_animation_tests";
    std::filesystem::create_directories(dir);
    const std::filesystem::path path = dir / "arm.glb";
    const std::vector<u8> bytes = geometry::testing::skinned_arm();
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                static_cast<std::streamsize>(bytes.size()));
    return path.generic_string();
}

std::string ball_file() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "giant_animation_tests";
    std::filesystem::create_directories(dir);
    const std::filesystem::path path = dir / "triangle.glb";
    // One triangle, not skinned.
    std::vector<u8> bin;
    geometry::testing::append(bin, std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1, 0});
    geometry::testing::append(bin, std::vector<u32>{0, 1, 2});
    const std::string json =
        R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],"buffers":[{"byteLength":48}],)"
        R"("bufferViews":[{"buffer":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":12}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
        R"({"bufferView":1,"componentType":5125,"count":3,"type":"SCALAR"}]})";
    const std::vector<u8> bytes = geometry::testing::make_glb(json, bin);
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                static_cast<std::streamsize>(bytes.size()));
    return path.generic_string();
}

// The commands over a scene; a null-device renderer when `gpu`.
struct Fixture {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<render::Renderer> renderer;
    std::unique_ptr<MeshLibrary> meshes;
    std::unique_ptr<AnimationState> animation;
    ai::CommandRegistry commands;
    EditorState state;
    SceneFrame frame;
    rhi::TextureHandle target;
    std::string arm = arm_file();
    explicit Fixture(bool gpu = false) {
        if (gpu) {
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
        }
        meshes = std::make_unique<MeshLibrary>(renderer.get());
        animation = std::make_unique<AnimationState>(renderer.get());
        state.meshes = meshes.get();
        state.animation = animation.get();
        register_scene_commands(commands, state);
        register_animation_commands(commands, state);
    }
    ~Fixture() {
        animation.reset(); // copies leave the renderer first
        meshes.reset();
    }
    ai::CommandResult run(const char* name, const Json& args = Json::object()) {
        ai::ExecuteOptions options;
        options.record_history = true;
        return commands.execute(name, args, options);
    }
    world::EntityId add_arm(double x = 0.0) {
        const auto r = run("create_entity", {{"shape", "mesh"}, {"mesh", arm}, {"position", {x, 0.0, 0.0}}});
        GIANT_CHECK(r.ok);
        return r.ok ? r.value["id"].get<world::EntityId>() : 0;
    }
    // A frame as main runs one: the animation follows, then the scene is drawn.
    // (Without a renderer, the scene's changes go to the animation directly.)
    void frame_step(double seconds) {
        if (renderer == nullptr) {
            animation->update(state.scene, *meshes, state.scene.take_changes(), seconds);
            return;
        }
        animation->update(state.scene, *meshes, frame.take_for_animation(), seconds);
        animation->follow_now(state.scene, *meshes, state.scene.untaken_changes()); // as main, before the cells
        const std::vector<world::EntityId> redraw = animation->take_redraw();
        scene_frame(state.scene, *renderer, *meshes, frame, nullptr, nullptr, nullptr, nullptr, redraw, nullptr,
                    nullptr, {}, &animation->copies());
        frame.take_for_followers();
        render::FrameInput input;
        input.view.position = {0.0, 1.0, 6.0};
        input.instances = frame.instances;
        input.cells = frame.cells;
        input.materials = frame.materials;
        rhi::CommandList* cmd = device->begin_frame(nullptr);
        renderer->render(*cmd, target, input);
        device->end_frame();
    }
    Json get(world::EntityId id) {
        const auto r = run("get_animation", {{"id", id}});
        GIANT_CHECK(r.ok);
        return r.value;
    }
};

bool near(double a, double b, double tolerance = 1e-3) { return std::fabs(a - b) <= tolerance; }

} // namespace

GIANT_TEST("animation: a skinned file lists its clips and joints; others have none") {
    Fixture f;
    const auto listed = f.run("list_animations", {{"path", f.arm}});
    GIANT_CHECK(listed.ok);
    if (!listed.ok) {
        std::fprintf(stderr, "    %s\n", listed.error.c_str());
        return;
    }
    GIANT_CHECK(listed.value["skinned"].get<bool>() && listed.value["joints"].get<u32>() == 2);
    const Json& clips = listed.value["clips"];
    GIANT_CHECK(clips.size() == 3 && clips[0]["name"] == "bend" && clips[1]["name"] == "hop" &&
                clips[2]["name"] == "bend (2)");
    GIANT_CHECK(near(clips[0]["seconds"].get<double>(), 1.0) && near(clips[1]["seconds"].get<double>(), 0.5));
    const MeshLibrary::Mesh* mesh = f.meshes->find(f.arm);
    GIANT_CHECK(mesh != nullptr && mesh->skinned != nullptr && !mesh->skinned->clusters.mesh.empty() &&
                mesh->skinned->find_clip("hop") == 1 && mesh->skinned->find_clip("walk") == anim::k_no_clip);

    const std::string plain = ball_file();
    const auto none = f.run("list_animations", {{"path", plain}});
    GIANT_CHECK(none.ok && !none.value["skinned"].get<bool>() && none.value["clips"].empty());
    GIANT_CHECK(!f.run("list_animations", Json::object()).ok); // one of path or id
    const world::EntityId arm = f.add_arm();
    const auto by_id = f.run("list_animations", {{"id", arm}});
    GIANT_CHECK(by_id.ok && by_id.value["clips"].size() == 3);
}

GIANT_TEST("animation: set_animation saves the clip, undoes, and refuses what cannot play") {
    Fixture f;
    const world::EntityId arm = f.add_arm();
    const world::EntityId cube = f.run("create_entity").value["id"].get<world::EntityId>();
    const auto set = f.run("set_animation", {{"id", arm}, {"clip", "bend"}, {"speed", 0.5}, {"loop", false}});
    GIANT_CHECK(set.ok && set.value["animated"].get<bool>());
    GIANT_CHECK(f.state.scene.find(arm)->animation == (world::EntityAnimation{"bend", 0.5f, false}));
    const auto entity = f.run("get_entity", {{"id", arm}});
    GIANT_CHECK(entity.ok && entity.value["animation"]["clip"] == "bend");
    // Saved with the scene.
    const std::string text = world::save_scene(f.state.scene);
    world::Scene loaded;
    GIANT_CHECK(!world::load_scene(text, loaded).has_value() && loaded.find(arm)->animation.clip == "bend");

    GIANT_CHECK(f.commands.undo());
    GIANT_CHECK(f.state.scene.find(arm)->animation == world::EntityAnimation{});
    f.frame_step(0.0); // followed: nothing to play
    GIANT_CHECK(f.animation->find(arm) == nullptr && f.animation->animated() == 0);
    GIANT_CHECK(f.commands.redo());
    f.frame_step(0.0);
    GIANT_CHECK(f.animation->find(arm) != nullptr);

    const auto missing = f.run("set_animation", {{"id", arm}, {"clip", "walk"}});
    GIANT_CHECK(!missing.ok && missing.error.find("no clip named") != std::string::npos);
    GIANT_CHECK(!f.run("set_animation", {{"id", cube}, {"clip", "bend"}}).ok); // not a mesh
    GIANT_CHECK(!f.run("set_animation", {{"id", 999}, {"clip", "bend"}}).ok);
    GIANT_CHECK(!f.run("set_animation", {{"id", arm}, {"clip", "bend"}, {"speed", 1000}}).ok); // schema
    GIANT_CHECK(f.run("set_animation", {{"id", cube}, {"clip", ""}}).ok); // clearing is always fine
}

GIANT_TEST("animation: time moves the pose, pausing holds it, step moves it by hand") {
    Fixture f;
    const world::EntityId arm = f.add_arm();
    GIANT_CHECK(f.run("set_animation", {{"id", arm}, {"clip", "hop"}, {"loop", false}}).ok);
    Json a = f.get(arm);
    GIANT_CHECK(a["animated"].get<bool>() && a["main"]["clip"] == "hop" && near(a["main"]["time"].get<double>(), 0.0));
    const double rest_x = a["bounds"]["center"][0].get<double>();
    f.frame_step(0.25);
    a = f.get(arm);
    GIANT_CHECK(near(a["main"]["time"].get<double>(), 0.25) && near(a["bounds"]["center"][0].get<double>(), rest_x));
    f.frame_step(0.5); // once: held at the end, the shoulder stepped 1 m along x
    a = f.get(arm);
    GIANT_CHECK(near(a["main"]["time"].get<double>(), 0.5));
    GIANT_CHECK(near(a["bounds"]["center"][0].get<double>(), rest_x + 1.0));
    const u64 poses = a["poses"].get<u64>();
    f.frame_step(0.5); // held: nothing new to pose
    GIANT_CHECK(f.get(arm)["poses"].get<u64>() == poses);

    GIANT_CHECK(f.run("set_animation", {{"id", arm}, {"clip", "bend"}, {"loop", true}}).ok); // another: from its start
    GIANT_CHECK(f.run("step_animation", {{"paused", true}}).ok);
    f.frame_step(0.25);
    a = f.get(arm);
    GIANT_CHECK(near(a["main"]["time"].get<double>(), 0.0)); // paused
    const auto stepped = f.run("step_animation", {{"seconds", 0.25}});
    GIANT_CHECK(stepped.ok && stepped.value["paused"].get<bool>() && stepped.value["animated"].get<u32>() == 1);
    GIANT_CHECK(near(f.get(arm)["main"]["time"].get<double>(), 0.25));
    GIANT_CHECK(f.run("step_animation", {{"paused", false}}).ok);
    f.frame_step(1.0); // looping: wraps
    GIANT_CHECK(near(f.get(arm)["main"]["time"].get<double>(), 0.25));
}

GIANT_TEST("animation: commands play and blend in the view until the saved animation changes") {
    Fixture f;
    const world::EntityId arm = f.add_arm();
    // Nothing saved: a command makes it animate anyway.
    auto played = f.run("play_animation", {{"id", arm}, {"clip", "hop"}, {"blend_seconds", 0}, {"start", 0.25}});
    GIANT_CHECK(played.ok && played.value["by_command"].get<bool>() && played.value["main"]["clip"] == "hop" &&
                near(played.value["main"]["time"].get<double>(), 0.25));
    GIANT_CHECK(f.state.scene.find(arm)->animation == world::EntityAnimation{}); // not saved
    f.frame_step(0.1);
    GIANT_CHECK(f.animation->find(arm) != nullptr); // kept though nothing is saved
    // A fade from it to bend.
    played = f.run("play_animation", {{"id", arm}, {"clip", "bend"}, {"blend_seconds", 0.5}});
    GIANT_CHECK(played.ok && played.value["fading"].get<bool>() && played.value["other"]["clip"] == "hop");
    f.frame_step(0.6);
    Json a = f.get(arm);
    GIANT_CHECK(!a["fading"].get<bool>() && a["other_weight"].get<double>() == 0.0);
    // Held blended, in step.
    const auto blended = f.run("blend_animations", {{"id", arm}, {"a", "bend"}, {"b", "hop"}, {"weight", 0.25}});
    GIANT_CHECK(blended.ok && blended.value["blending"].get<bool>() && blended.value["synced"].get<bool>() &&
                near(blended.value["other_weight"].get<double>(), 0.25));
    GIANT_CHECK(!f.run("blend_animations", {{"id", arm}, {"a", "bend"}, {"b", "nope"}, {"weight", 0.5}}).ok);
    GIANT_CHECK(!f.run("play_animation", {{"id", arm}, {"clip", "nope"}}).ok);

    // Its own animation saved: that plays now.
    GIANT_CHECK(f.run("set_animation", {{"id", arm}, {"clip", "bend (2)"}}).ok);
    a = f.get(arm);
    GIANT_CHECK(!a["by_command"].get<bool>() && a["main"]["clip"] == "bend (2)" && !a["blending"].get<bool>());
    // Cleared: it stands at rest, no longer animated.
    GIANT_CHECK(f.run("set_animation", {{"id", arm}, {"clip", ""}}).ok);
    GIANT_CHECK(f.animation->find(arm) == nullptr);
    a = f.get(arm);
    GIANT_CHECK(!a["animated"].get<bool>() && a["why"].get<std::string>().find("rest pose") != std::string::npos);

    // A command's play goes with the scene.
    GIANT_CHECK(f.run("play_animation", {{"id", arm}, {"clip", "hop"}}).ok);
    world::Scene other = f.state.scene; // the same entities, a new scene
    f.state.scene = std::move(other);
    f.frame_step(0.0);
    GIANT_CHECK(f.animation->find(arm) == nullptr);
}

GIANT_TEST("animation: at most 64 animate; the rest stand frozen until a place frees") {
    Fixture f;
    std::vector<world::EntityId> ids;
    for (int i = 0; i < 66; ++i) ids.push_back(f.add_arm(3.0 * i));
    for (const world::EntityId id : ids) {
        world::Entity* e = f.state.scene.modify(id);
        e->animation.clip = "bend"; // as a loaded scene has them
    }
    f.animation->copies_per_frame = 16;
    f.frame_step(0.0);
    GIANT_CHECK(f.animation->animated() == 16 && f.animation->waiting() == 48 && f.animation->frozen() == 2);
    for (int i = 0; i < 4; ++i) f.frame_step(0.0);
    GIANT_CHECK(f.animation->animated() == 64 && f.animation->waiting() == 0 && f.animation->frozen() == 2);
    GIANT_CHECK(f.animation->find(ids[63]) != nullptr && f.animation->find(ids[64]) == nullptr); // lowest ids first
    const Json frozen = f.get(ids[65]);
    GIANT_CHECK(!frozen["animated"].get<bool>() && frozen["why"].get<std::string>().find("64") != std::string::npos);
    GIANT_CHECK(!f.run("play_animation", {{"id", ids[65]}, {"clip", "hop"}}).ok); // no place
    // One hidden: its place goes to the lowest waiting.
    GIANT_CHECK(f.run("set_entity", {{"id", ids[3]}, {"visible", false}}).ok);
    f.frame_step(0.0);
    GIANT_CHECK(f.animation->find(ids[3]) == nullptr && f.animation->find(ids[64]) != nullptr &&
                f.animation->frozen() == 1);
    // Removed: likewise.
    GIANT_CHECK(f.run("delete_entities", {{"ids", {ids[0]}}}).ok);
    f.frame_step(0.0);
    GIANT_CHECK(f.animation->find(ids[0]) == nullptr && f.animation->find(ids[65]) != nullptr &&
                f.animation->frozen() == 0 && f.animation->animated() == 64);
    const auto summary = f.run("get_animation");
    GIANT_CHECK(summary.ok && summary.value["animated"].get<u32>() == 64 && summary.value["max_animated"] == 64);
}

GIANT_TEST("animation: an animated entity draws its own posed copy; frozen ones the file's rest pose") {
    Fixture f(true);
    if (f.renderer == nullptr) return;
    const world::EntityId arm = f.add_arm();
    const world::EntityId still = f.add_arm(5.0);
    f.frame_step(0.0);
    const MeshLibrary::Mesh* mesh = f.meshes->find(f.arm);
    GIANT_CHECK(mesh != nullptr && mesh->id != render::k_no_mesh);
    const auto mesh_of = [&](world::EntityId id) {
        const world::Entity* e = f.state.scene.find(id);
        for (const render::Instance& i : f.frame.instances) {
            if (i.mesh != render::k_no_mesh && i.position.x == e->transform.position.x) return i.mesh;
        }
        return render::k_no_mesh;
    };
    GIANT_CHECK(mesh_of(arm) == mesh->id && mesh_of(still) == mesh->id);

    GIANT_CHECK(f.run("set_animation", {{"id", arm}, {"clip", "bend"}}).ok);
    const AnimationState::Animated* a = f.animation->find(arm);
    GIANT_CHECK(a != nullptr && a->copy != render::k_no_mesh && f.renderer->skinned(a->copy));
    for (int i = 0; i < 8 && !f.get(arm)["drawn"].get<bool>(); ++i) f.frame_step(1.0 / 60.0);
    GIANT_CHECK(f.get(arm)["drawn"].get<bool>());
    f.frame_step(1.0 / 60.0); // the cell drawn again with it
    a = f.animation->find(arm);
    GIANT_CHECK(a != nullptr && mesh_of(arm) == a->copy && mesh_of(still) == mesh->id);
    GIANT_CHECK(f.renderer->last_stats().skinned_meshes == 1);

    // Stopped: back to the file's mesh, the copy gone from the renderer.
    const render::MeshId copy = a->copy;
    GIANT_CHECK(f.run("set_animation", {{"id", arm}, {"clip", ""}}).ok);
    f.frame_step(0.0);
    GIANT_CHECK(mesh_of(arm) == mesh->id && !f.renderer->skinned(copy) && f.renderer->mesh(copy) == nullptr);

    // Reimported: copied again from the new file.
    GIANT_CHECK(f.run("set_animation", {{"id", arm}, {"clip", "bend"}}).ok);
    const std::shared_ptr<const MeshLibrary::Skinned> before = f.animation->find(arm)->skinned;
    GIANT_CHECK(f.meshes->reload(f.arm).mesh != nullptr);
    for (int i = 0; i < 4; ++i) f.frame_step(1.0 / 60.0);
    a = f.animation->find(arm);
    GIANT_CHECK(a != nullptr && a->skinned != before && a->skinned == f.meshes->find(f.arm)->skinned &&
                f.renderer->skinned(a->copy) && f.renderer->last_stats().skinned_meshes == 1);
}

GIANT_TEST("animation: set_animation always takes over from a command; once backwards starts at the end") {
    Fixture f;
    const world::EntityId arm = f.add_arm();
    GIANT_CHECK(f.run("set_animation", {{"id", arm}, {"clip", "bend"}}).ok);
    GIANT_CHECK(f.run("play_animation", {{"id", arm}, {"clip", "hop"}}).ok);
    // The same value again: the saved clip plays, not the command's.
    const auto again = f.run("set_animation", {{"id", arm}, {"clip", "bend"}});
    GIANT_CHECK(again.ok && !again.value["by_command"].get<bool>() && again.value["main"]["clip"] == "bend");

    GIANT_CHECK(f.run("set_animation", {{"id", arm}, {"clip", "hop"}, {"speed", -1}, {"loop", false}}).ok);
    Json a = f.get(arm);
    GIANT_CHECK(a["main"]["clip"] == "hop" && near(a["main"]["time"].get<double>(), 0.5)); // from its end
    f.frame_step(0.1);
    f.frame_step(0.2);
    GIANT_CHECK(near(f.get(arm)["main"]["time"].get<double>(), 0.2));
    f.frame_step(1.0);
    GIANT_CHECK(near(f.get(arm)["main"]["time"].get<double>(), 0.0)); // held at its start
    const auto played = f.run("play_animation", {{"id", arm}, {"clip", "bend"}, {"speed", -2}, {"loop", false}});
    GIANT_CHECK(played.ok && near(played.value["main"]["time"].get<double>(), 1.0));

    // Clearing a field on what cannot animate leaves no animation at all.
    const world::EntityId cube = f.run("create_entity").value["id"].get<world::EntityId>();
    GIANT_CHECK(f.run("set_animation", {{"id", cube}, {"clip", ""}, {"speed", 3}}).ok);
    GIANT_CHECK(f.state.scene.find(cube)->animation == world::EntityAnimation{});
}

GIANT_TEST("animation: a clip started by a long frame starts at its start; a new scene is seen at once") {
    Fixture f;
    const world::EntityId arm = f.add_arm();
    f.state.scene.modify(arm)->animation.clip = "bend"; // as loaded, not by command
    f.frame_step(0.75); // a slow first frame
    GIANT_CHECK(near(f.get(arm)["main"]["time"].get<double>(), 0.0));
    f.frame_step(0.25);
    GIANT_CHECK(near(f.get(arm)["main"]["time"].get<double>(), 0.25));
    // Another scene put in place (load_scene): asked before any frame, the answer is the new scene's.
    world::Scene other;
    world::Entity plain;
    plain.shape = world::Shape::mesh;
    plain.mesh = ball_file();
    GIANT_CHECK(f.meshes->load(plain.mesh).mesh != nullptr);
    other.add(plain);
    f.state.scene = std::move(other);
    const Json a = f.get(arm);
    GIANT_CHECK(!a["animated"].get<bool>() && a["why"].get<std::string>().find("not skinned") != std::string::npos);
    GIANT_CHECK(f.run("get_animation").value["animated"].get<u32>() == 0);
}

GIANT_TEST("animation: copies on the renderer: frozen ones draw the rest pose, edits never draw a stale copy") {
    Fixture f(true);
    if (f.renderer == nullptr) return;
    std::vector<world::EntityId> ids;
    for (int i = 0; i < 65; ++i) ids.push_back(f.add_arm(3.0 * i));
    for (const world::EntityId id : ids) f.state.scene.modify(id)->animation.clip = "bend";
    f.animation->copies_per_frame = 64;
    const auto mesh_of = [&](world::EntityId id) {
        const world::Entity* e = f.state.scene.find(id);
        for (const render::Instance& i : f.frame.instances) {
            if (i.mesh != render::k_no_mesh && i.position.x == e->transform.position.x) return i.mesh;
        }
        return render::k_no_mesh;
    };
    for (int i = 0; i < 8; ++i) f.frame_step(1.0 / 60.0);
    const MeshLibrary::Mesh* mesh = f.meshes->find(f.arm);
    GIANT_CHECK(f.animation->animated() == 64 && f.animation->frozen() == 1 && f.animation->copies().size() == 64);
    GIANT_CHECK(mesh_of(ids[64]) == mesh->id); // frozen: the file's rest pose
    GIANT_CHECK(mesh_of(ids[10]) == f.animation->find(ids[10])->copy);
    GIANT_CHECK(f.renderer->last_stats().skinned_meshes == 64);

    // Held: a pose that does not change is not sent again (its shadows stay).
    GIANT_CHECK(f.run("set_animation", {{"id", ids[0]}, {"clip", "bend"}, {"loop", false}}).ok);
    for (int i = 0; i < 70; ++i) f.frame_step(1.0 / 60.0); // past its one second
    const u64 held = f.animation->find(ids[0])->poses;
    for (int i = 0; i < 4; ++i) f.frame_step(1.0 / 60.0);
    GIANT_CHECK(f.animation->find(ids[0])->poses == held);

    // Another file: the very next frame draws it, never the old copy.
    const std::string plain = ball_file();
    GIANT_CHECK(f.meshes->load(plain).mesh != nullptr);
    f.frame_step(0.0); // (the import rebuilds everything)
    GIANT_CHECK(f.run("set_entity", {{"id", ids[1]}, {"mesh", plain}}).ok);
    f.frame_step(1.0 / 60.0);
    GIANT_CHECK(mesh_of(ids[1]) == f.meshes->find(plain)->id && f.animation->find(ids[1]) == nullptr);

    // Imported again: drawn with a new copy once it is loaded.
    GIANT_CHECK(f.meshes->reload(f.arm).mesh != nullptr);
    for (int i = 0; i < 8; ++i) f.frame_step(1.0 / 60.0);
    const AnimationState::Animated* a = f.animation->find(ids[10]);
    GIANT_CHECK(a != nullptr && a->drawn && a->skinned == f.meshes->find(f.arm)->skinned &&
                mesh_of(ids[10]) == a->copy && f.renderer->skinned(a->copy));
}

GIANT_TEST("hit zones: a skinned arm's bones have capsules, named by zone, posed as drawn; bullets report them") {
    Fixture f;
    HitTargets hits;
    PhysicsState physics;
    physics.set_hit_targets(&hits);
    f.state.hits = &hits;
    f.state.physics = &physics;
    register_physics_commands(f.commands, f.state);
    const world::EntityId arm = f.add_arm(3.0);
    // Standing in the rest pose: the shoulder's capsule up from the origin, the elbow's above.
    ai::CommandResult r = f.run("get_hit_zones", {{"id", arm}});
    GIANT_CHECK(r.ok && r.value["default"] == true && r.value["capsules"].size() == 2);
    if (r.ok && r.value["capsules"].size() == 2) {
        const Json& c = r.value["capsules"];
        GIANT_CHECK(c[0]["bone"] == "shoulder" && c[0]["zone"] == "torso");
        GIANT_CHECK(c[1]["bone"] == "elbow" && c[1]["zone"] == "arm");
        GIANT_CHECK(near(c[0]["a"][0].get<double>(), 3.0, 0.01) && near(c[0]["radius"].get<double>(), 0.1, 0.01));
    }
    // A shot across the forearm (1.8 m up) meets the elbow's capsule: flesh, zone arm.
    r = f.run("fire", {{"origin", {0, 1.8, 0}}, {"direction", {1, 0, 0}}, {"resolve", true}});
    GIANT_CHECK(r.ok && !r.value["hits"].empty());
    if (r.ok && !r.value["hits"].empty()) {
        const Json& h = r.value["hits"][0];
        GIANT_CHECK(h["entity"] == arm && h["character"] == true && h["zone"] == "arm" && h["bone"] == "elbow" &&
                    h["surface"] == "flesh" && h["outcome"] == "passed");
    }
    // Zones named otherwise for this file; reset back.
    r = f.run("set_hit_zones", {{"mesh", f.arm}, {"zones", {{{"zone", "limb"}, {"match", {"ELBOW", "shoulder"}}}}}});
    GIANT_CHECK(r.ok && r.value["zones"].size() == 1);
    r = f.run("fire", {{"origin", {0, 1.8, 0}}, {"direction", {1, 0, 0}}, {"resolve", true}});
    GIANT_CHECK(r.ok && !r.value["hits"].empty() && r.value["hits"][0]["zone"] == "limb");
    GIANT_CHECK(f.run("set_hit_zones", {{"mesh", f.arm}, {"reset", true}}).ok);
    // The hits kept keep the names they had when they happened.
    r = f.run("get_ballistics", {{"hits", 1}});
    GIANT_CHECK(r.ok && r.value["recent_hits"].size() == 1 && r.value["recent_hits"][0]["zone"] == "limb");
    GIANT_CHECK(!f.run("set_hit_zones", {{"mesh", f.arm}}).ok);
    // Bent: the elbow's capsule swings over toward x = 2 (the top at -1 from the elbow, 1 m up).
    GIANT_CHECK(f.run("play_animation", {{"id", arm}, {"clip", "bend"}, {"blend_seconds", 0}, {"loop", false}}).ok);
    GIANT_CHECK(f.run("step_animation", {{"seconds", 1.0}}).ok);
    r = f.run("get_hit_zones", {{"id", arm}});
    GIANT_CHECK(r.ok && r.value["capsules"].size() == 2);
    if (r.ok && r.value["capsules"].size() == 2) {
        const Json& elbow = r.value["capsules"][1];
        GIANT_CHECK(elbow["a"][0].get<double>() < 2.5 && elbow["b"][0].get<double>() < 2.5);
        GIANT_CHECK(elbow["a"][1].get<double>() < 1.4 && elbow["b"][1].get<double>() < 1.4);
    }
    r = f.run("fire", {{"origin", {3, 5, 0}}, {"direction", {0, -1, 0}}, {"resolve", true}});
    GIANT_CHECK(r.ok && !r.value["hits"].empty() && r.value["hits"][0]["bone"] == "shoulder");
    // Decor (no body): not hit.
    GIANT_CHECK(f.run("set_entity", {{"id", arm}, {"body", "none"}}).ok);
    GIANT_CHECK(!f.run("get_hit_zones", {{"id", arm}}).ok);
    GIANT_CHECK(f.run("set_entity", {{"id", arm}, {"body", "fixed"}}).ok);
    GIANT_CHECK(f.run("get_hit_zones", {{"id", arm}}).ok);
    // Hidden: no hit shapes.
    GIANT_CHECK(f.run("set_entity", {{"id", arm}, {"visible", false}}).ok);
    GIANT_CHECK(!f.run("get_hit_zones", {{"id", arm}}).ok);
    r = f.run("fire", {{"origin", {0, 1.0, 0}}, {"direction", {1, 0, 0}}, {"resolve", true}});
    GIANT_CHECK(r.ok && r.value["hits"].empty());
}

GIANT_TEST("animation: copies of one file share its geometry, whichever uploaded it goes first") {
    Fixture f(true);
    if (f.renderer == nullptr) return;
    std::vector<world::EntityId> ids;
    for (int i = 0; i < 3; ++i) ids.push_back(f.add_arm(3.0 * i));
    for (const world::EntityId id : ids) GIANT_CHECK(f.run("set_animation", {{"id", id}, {"clip", "bend"}}).ok);
    f.frame_step(0.0);
    const auto copy_of = [&](world::EntityId id) {
        const AnimationState::Animated* a = f.animation->find(id);
        return a != nullptr ? a->copy : render::k_no_mesh;
    };
    const render::MeshId first = copy_of(ids[0]);
    GIANT_CHECK(first != render::k_no_mesh && copy_of(ids[1]) != first && copy_of(ids[2]) != first);
    // One CPU copy of the clusters for all three.
    GIANT_CHECK(f.renderer->mesh(first) == f.renderer->mesh(copy_of(ids[1])) &&
                f.renderer->mesh(first) == f.renderer->mesh(copy_of(ids[2])));
    // The one that uploaded stops; a new one still shares (copied from those left).
    GIANT_CHECK(f.run("set_animation", {{"id", ids[0]}, {"clip", ""}}).ok);
    f.frame_step(0.0);
    GIANT_CHECK(!f.renderer->skinned(first));
    const world::EntityId late = f.add_arm(20.0);
    GIANT_CHECK(f.run("set_animation", {{"id", late}, {"clip", "bend"}}).ok);
    f.frame_step(0.0);
    GIANT_CHECK(copy_of(late) != render::k_no_mesh &&
                f.renderer->mesh(copy_of(late)) == f.renderer->mesh(copy_of(ids[1])));
    for (int i = 0; i < 8 && !f.get(late)["drawn"].get<bool>(); ++i) f.frame_step(1.0 / 60.0);
    GIANT_CHECK(f.get(late)["drawn"].get<bool>());
    // All stopped, then one again: uploaded afresh.
    for (const world::EntityId id : {ids[1], ids[2], late}) {
        GIANT_CHECK(f.run("set_animation", {{"id", id}, {"clip", ""}}).ok);
    }
    f.frame_step(0.0);
    GIANT_CHECK(f.renderer->last_stats().skinned_meshes == 0);
    GIANT_CHECK(f.run("set_animation", {{"id", ids[1]}, {"clip", "bend"}}).ok);
    f.frame_step(0.0);
    GIANT_CHECK(f.renderer->skinned(copy_of(ids[1])));
}
