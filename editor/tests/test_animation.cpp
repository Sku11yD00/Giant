// Skinned entities playing their clips: the saved animation, the view's
// commands, the limit on how many animate, and their own posed copies drawn.
#include "../src/animation_state.h"
#include "../src/editor_commands.h"
#include "../src/hit_targets.h"
#include "../src/physics_state.h"
#include "../src/mesh_library.h"
#include "../src/nav_state.h"
#include "../src/scene_render.h"

#include "giant_test.h"
#include "gltf_test_files.h"

#include <cstring>
#include <span>

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
    GIANT_CHECK(f.state.scene.find(arm)->animation == (world::EntityAnimation{"bend", 0.5f, false, {}}));
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

GIANT_TEST("animation: at most 64 animate (as set); the rest stand frozen until a place frees") {
    Fixture f;
    f.animation->max_animated = 64;
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
    f.animation->max_animated = 64;
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

GIANT_TEST("character light: capsules of characters near the camera, where they are drawn, in their colour") {
    Fixture f;
    const world::EntityId arm = f.add_arm(3.0);
    const world::EntityId decor = f.add_arm(-3.0); // no body (as walkers): still shades the light
    const world::EntityId far_arm = f.add_arm(3000.0);
    const world::EntityId hidden = f.add_arm(6.0);
    const world::EntityId gone = f.add_arm(9.0);
    GIANT_CHECK(f.run("set_entity", {{"id", arm}, {"base_color", {1, 0, 0}}}).ok);
    GIANT_CHECK(f.run("set_entity", {{"id", decor}, {"body", "none"}}).ok);
    GIANT_CHECK(f.run("set_entity", {{"id", hidden}, {"visible", false}}).ok);
    GIANT_CHECK(f.run("delete_entities", {{"ids", {gone}}}).ok);
    GIANT_CHECK(f.run("play_animation", {{"id", arm}, {"clip", "bend"}, {"blend_seconds", 0}, {"loop", false}}).ok);
    GIANT_CHECK(f.run("step_animation", {{"seconds", 1.0}}).ok);
    f.frame_step(0.0);
    std::vector<render::Occluder> out(5); // (cleared first)
    const DVec3 camera{0.0, 1.0, 0.0};
    character_occluders(f.state.scene, *f.meshes, *f.animation, nullptr, nullptr, nullptr, nullptr, camera, out);
    // The two near arms' two bones each; not the far, hidden or deleted ones.
    GIANT_CHECK(out.size() == 4);
    usize red = 0;
    for (const render::Occluder& o : out) {
        GIANT_CHECK(o.radius > 0.05f && (near(o.a.x, 3.0, 1.5) || near(o.a.x, -3.0, 0.5)));
        if (o.albedo.x == 1.0f && o.albedo.y == 0.0f) {
            ++red;
            GIANT_CHECK(near(o.a.x, 3.0, 1.5));
        } else {
            GIANT_CHECK(o.albedo.x == 0.8f && near(o.a.x, -3.0, 0.5));
        }
    }
    GIANT_CHECK(red == 2);
    // The bent arm's elbow capsule as posed: swung over toward x = 2 (as its hit shape).
    float lowest_x = 10.0f;
    for (const render::Occluder& o : out) {
        if (near(o.a.x, 3.0, 1.5)) lowest_x = std::min({lowest_x, static_cast<float>(o.a.x), static_cast<float>(o.b.x)});
    }
    GIANT_CHECK(lowest_x < 2.5f);
    // Drawn elsewhere (walking): placed there.
    std::unordered_map<world::EntityId, world::Transform> walking;
    walking[decor] = f.state.scene.find(decor)->transform;
    walking[decor].position = {-3.0, 0.0, -20.0};
    character_occluders(f.state.scene, *f.meshes, *f.animation, nullptr, nullptr, &walking, nullptr, camera, out);
    usize moved = 0;
    for (const render::Occluder& o : out) moved += near(o.a.z, -20.0, 0.5) ? 1u : 0u;
    GIANT_CHECK(out.size() == 4 && moved == 2);
    // Moved by physics (or swinging): there; physics first, as drawn.
    std::unordered_map<world::EntityId, world::Transform> moving;
    moving[decor] = walking[decor];
    moving[decor].position = {-3.0, 0.0, -30.0};
    std::unordered_map<world::EntityId, world::Transform> swinging;
    swinging[arm] = f.state.scene.find(arm)->transform;
    swinging[arm].position = {3.0, 0.0, -40.0};
    character_occluders(f.state.scene, *f.meshes, *f.animation, nullptr, &moving, &walking, &swinging, camera, out);
    usize by_physics = 0;
    usize swung = 0;
    for (const render::Occluder& o : out) {
        by_physics += near(o.a.z, -30.0, 0.5) ? 1u : 0u;
        swung += near(o.a.z, -40.0, 1.5) ? 1u : 0u;
    }
    GIANT_CHECK(out.size() == 4 && by_physics == 2 && swung == 2);
    // By the far one: its two only; nobody near: none.
    character_occluders(f.state.scene, *f.meshes, *f.animation, nullptr, nullptr, nullptr, nullptr, DVec3{3000.0, 1.0, 0.0}, out);
    GIANT_CHECK(out.size() == 2 && near(out[0].a.x, 3000.0, 0.5));
    character_occluders(f.state.scene, *f.meshes, *f.animation, nullptr, nullptr, nullptr, nullptr, DVec3{1500.0, 1.0, 0.0}, out);
    GIANT_CHECK(out.empty());
    GIANT_CHECK(f.state.scene.find(far_arm) != nullptr);
    // The render setting and the frame's count over MCP.
    GIANT_CHECK(f.run("set_render_settings", {{"character_occlusion", false}}).ok);
    GIANT_CHECK(!f.state.render_settings.character_occlusion);
    const auto settings = f.run("get_render_settings");
    GIANT_CHECK(settings.ok && settings.value["character_occlusion"] == false &&
                settings.value["last_frame"].contains("occluders"));
    // Skin's spread, the same way.
    GIANT_CHECK(settings.ok && settings.value["subsurface_scattering"] == true &&
                settings.value["last_frame"].contains("subsurface_scattering"));
    GIANT_CHECK(f.run("set_render_settings", {{"subsurface_scattering", false}}).ok);
    GIANT_CHECK(!f.state.render_settings.subsurface_scattering);
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

GIANT_TEST("animation: LOD poses near characters every frame, far ones a few times a second, within a budget") {
    // By size on screen: every frame big, slower as it shrinks.
    GIANT_CHECK(AnimationState::pose_interval(500.0f) == 0.0 && AnimationState::pose_interval(100.0f) == 0.0);
    double last = 0.0;
    for (float px = 99.0f; px > 0.1f; px *= 0.8f) {
        const double i = AnimationState::pose_interval(px);
        GIANT_CHECK(i >= last && i <= 0.25);
        last = i;
    }
    GIANT_CHECK(AnimationState::pose_interval(1.0f) == 0.25);

    Fixture f;
    render::View view;
    view.position = {0.0, 1.0, 4.0};
    view.vertical_fov_degrees = 60.0f;
    // How big an arm is 400 m off, to place the rest by the size wanted.
    const world::EntityId probe = f.add_arm(400.0);
    GIANT_CHECK(f.run("set_animation", {{"id", probe}, {"clip", "bend"}}).ok);
    f.animation->set_view(view, 1080);
    f.frame_step(1.0 / 60.0);
    f.animation->set_view(view, 1080);
    f.frame_step(1.0 / 60.0);
    const float probe_pixels = f.animation->find(probe)->pixels;
    GIANT_CHECK(probe_pixels > 0.0f && probe_pixels < 100.0f);
    const auto at_pixels = [&](float px) { return 400.0 * static_cast<double>(probe_pixels / px); };
    // One arm by the camera; ten in each slower tier (1/15 s, 1/8 s, 1/4 s).
    std::vector<world::EntityId> ids;
    ids.push_back(f.add_arm(0.0));
    const double tiers[3] = {1.0 / 15.0, 1.0 / 8.0, 1.0 / 4.0};
    const float tier_pixels[3] = {25.0f, 9.0f, 2.0f};
    for (usize t = 0; t < 3; ++t) {
        for (usize i = 0; i < 10; ++i) ids.push_back(f.add_arm(at_pixels(tier_pixels[t]) + 0.01 * static_cast<double>(i)));
    }
    for (const world::EntityId id : ids) GIANT_CHECK(f.run("set_animation", {{"id", id}, {"clip", "bend"}}).ok);
    const auto frame = [&] {
        f.animation->set_view(view, 1080); // as main, each frame
        f.frame_step(1.0 / 60.0);
        // As main does after the update (before the cells): follows the
        // latest edits, posing only what is asked for now.
        f.animation->follow_now(f.state.scene, *f.meshes, world::SceneChanges{});
    };
    frame();
    frame();
    for (usize t = 0; t < 3; ++t) {
        for (usize i = 0; i < 10; ++i) GIANT_CHECK(f.get(ids[1 + 10 * t + i])["pose_interval"].get<double>() == tiers[t]);
    }
    std::vector<u64> before;
    for (const world::EntityId id : ids) before.push_back(f.animation->find(id)->poses);
    // A second: each posed about as often as its tier says, and no tier's
    // crowd posed all on the same frames (spread by id).
    u32 most[3] = {0, 0, 0};
    for (int frame_n = 0; frame_n < 60; ++frame_n) {
        std::vector<u64> was;
        for (const world::EntityId id : ids) was.push_back(f.animation->find(id)->poses);
        frame();
        for (usize t = 0; t < 3; ++t) {
            u32 n = 0;
            for (usize i = 0; i < 10; ++i) n += f.animation->find(ids[1 + 10 * t + i])->poses != was[1 + 10 * t + i];
            most[t] = std::max(most[t], n);
        }
    }
    const Json near_one = f.get(ids[0]);
    GIANT_CHECK(near_one["pose_interval"].get<double>() == 0.0 && near_one["pixels"].get<float>() >= 100.0f);
    GIANT_CHECK(f.animation->find(ids[0])->poses - before[0] >= 55); // every frame (bar a held key)
    for (usize t = 0; t < 3; ++t) {
        const double per_second = 1.0 / tiers[t];
        for (usize i = 0; i < 10; ++i) {
            const usize k = 1 + 10 * t + i;
            const double n = static_cast<double>(f.animation->find(ids[k])->poses - before[k]);
            GIANT_CHECK(n >= per_second * 0.7 - 1.0 && n <= per_second * 1.1 + 1.0);
        }
        GIANT_CHECK(most[t] <= 5); // 10 at once would be lock-step
    }
    // A crowd leaving the every-frame tier together (the camera walks away
    // from it) keeps its poses spread too.
    {
        render::View by_them = view;
        by_them.position = {at_pixels(2.0f), 1.0, 4.0};
        f.animation->set_view(by_them, 1080);
        f.frame_step(1.0 / 60.0);
        f.animation->follow_now(f.state.scene, *f.meshes, world::SceneChanges{});
        frame(); // back: far again
        frame();
        GIANT_CHECK(f.get(ids[21])["pose_interval"].get<double>() == tiers[2]);
        u32 most_far = 0;
        for (int frame_n = 0; frame_n < 30; ++frame_n) {
            std::vector<u64> was;
            for (usize i = 0; i < 10; ++i) was.push_back(f.animation->find(ids[21 + i])->poses);
            frame();
            u32 n = 0;
            for (usize i = 0; i < 10; ++i) n += f.animation->find(ids[21 + i])->poses != was[i];
            most_far = std::max(most_far, n);
        }
        GIANT_CHECK(most_far <= 5);
    }
    // The frame's stats are the frame's, not follow_now's (which posed nothing).
    GIANT_CHECK(f.animation->posed_last() >= 1);
    const auto stats = f.run("get_animation");
    GIANT_CHECK(stats.ok && stats.value["posed_last_frame"].get<u32>() == f.animation->posed_last());

    // A budget of 4 a frame, with more near ones than that: the near ones
    // still every frame; the far ones wait their turn (overdue), but a share
    // of the budget is always theirs, so all get posed over time.
    for (int i = 0; i < 5; ++i) {
        const world::EntityId id = f.add_arm(0.2 * (i + 1));
        GIANT_CHECK(f.run("set_animation", {{"id", id}, {"clip", "bend"}}).ok);
    }
    f.animation->poses_per_frame = 4;
    frame();
    for (usize k = 0; k < ids.size(); ++k) before[k] = f.animation->find(ids[k])->poses;
    u32 overdue = 0;
    for (int i = 0; i < 240; ++i) {
        frame();
        GIANT_CHECK(f.animation->posed_last() == 7); // 6 near + the 1 always theirs
        overdue = std::max(overdue, f.animation->overdue());
    }
    GIANT_CHECK(overdue > 0);
    GIANT_CHECK(f.animation->find(ids[0])->poses - before[0] >= 220);
    for (usize k = 1; k < ids.size(); ++k) GIANT_CHECK(f.animation->find(ids[k])->poses > before[k]);
    const auto summary = f.run("get_animation");
    GIANT_CHECK(summary.ok && summary.value["max_animated"].get<u32>() == AnimationState::k_max_animated &&
                summary.value["poses_per_frame"].get<u32>() == 4 && summary.value["overdue"].get<u32>() > 0);
    // A command's pose goes at once, whatever the LOD.
    const u64 far_poses = f.animation->find(ids[25])->poses;
    GIANT_CHECK(f.run("play_animation", {{"id", ids[25]}, {"clip", "hop"}, {"blend_seconds", 0.0}}).ok);
    GIANT_CHECK(f.animation->find(ids[25])->poses == far_poses + 1 && !f.animation->find(ids[25])->dirty);

    // Time standing still (paused): what moved since its last pose catches
    // up within the budget, then nothing more is posed.
    f.animation->poses_per_frame = 256;
    f.animation->set_paused(true);
    frame();
    for (int i = 0; i < 3; ++i) frame();
    for (const world::EntityId id : ids) GIANT_CHECK(!f.animation->find(id)->changed);
    frame();
    GIANT_CHECK(f.animation->posed_last() == 0);
}

namespace {

// The arm with morph targets ("smile" resting at 0.25, "blink" at 0) and
// clips animating them: "smile" (smile 0 to 1, blink 0 to 0.5 over 1 s)
// and "wink" (smile stepping 0.2 then 0.8).
std::string morph_arm_file(float smile_rest = 0.25f) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "giant_animation_tests";
    std::filesystem::create_directories(dir);
    const std::filesystem::path path = dir / (smile_rest == 0.25f ? "morph_arm.glb" : "big_morph_arm.glb");
    geometry::testing::SkinOptions o;
    o.morphs = true;
    o.morph_clip = true;
    o.smile_rest = smile_rest;
    const std::vector<u8> bytes = geometry::testing::skinned_arm(o);
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                static_cast<std::streamsize>(bytes.size()));
    return path.generic_string();
}

} // namespace

GIANT_TEST("animation: morph targets: listed, played by clips, held by set_morphs and let go") {
    for (const bool gpu : {false, true}) {
        Fixture f(gpu);
        f.arm = morph_arm_file();
        const auto listed = f.run("list_animations", {{"path", f.arm}});
        GIANT_CHECK(listed.ok);
        if (!listed.ok) return;
        const Json& targets = listed.value["morph_targets"];
        GIANT_CHECK(targets.size() == 2 && targets[0]["name"] == "smile" && targets[1]["name"] == "blink");
        GIANT_CHECK(near(targets[0]["rest_weight"].get<double>(), 0.25) && targets[1]["vertices"].get<u32>() == 1);
        const Json& clips = listed.value["clips"];
        GIANT_CHECK(clips.size() == 6 && clips[3]["name"] == "smile" && clips[3]["morph_channels"].get<u32>() == 2 &&
                    clips[3]["channels"].get<u32>() == 0 && clips[0]["morph_channels"].get<u32>() == 0);
        // A plain arm lists none.
        const auto plain = f.run("list_animations", {{"path", arm_file()}});
        GIANT_CHECK(plain.ok && plain.value["morph_targets"].empty());

        const world::EntityId arm = f.add_arm();
        // Bending: no morph channels, so the rest weights.
        GIANT_CHECK(f.run("set_animation", {{"id", arm}, {"clip", "bend"}}).ok);
        f.frame_step(0.0);
        Json a = f.get(arm);
        GIANT_CHECK(near(a["morphs"]["smile"].get<double>(), 0.25) && near(a["morphs"]["blink"].get<double>(), 0.0));
        GIANT_CHECK(a["held_morphs"].empty());
        // Smiling: halfway in, half of each.
        GIANT_CHECK(f.run("play_animation", {{"id", arm}, {"clip", "smile"}, {"blend_seconds", 0}, {"start", 0.5}}).ok);
        a = f.get(arm);
        GIANT_CHECK(near(a["morphs"]["smile"].get<double>(), 0.5) && near(a["morphs"]["blink"].get<double>(), 0.25));
        // Held: blink stays where it is put while the clip runs; smile keeps playing.
        auto held = f.run("set_morphs", {{"id", arm}, {"weights", {{"blink", 0.9}}}});
        GIANT_CHECK(held.ok && near(held.value["morphs"]["blink"].get<double>(), 0.9) &&
                    near(held.value["held_morphs"]["blink"].get<double>(), 0.9));
        f.frame_step(0.25);
        a = f.get(arm);
        GIANT_CHECK(near(a["morphs"]["blink"].get<double>(), 0.9) && near(a["morphs"]["smile"].get<double>(), 0.75));
        // Let go (null): back to its clip.
        held = f.run("set_morphs", {{"id", arm}, {"weights", {{"blink", nullptr}, {"smile", -2.0}}}});
        GIANT_CHECK(held.ok && near(held.value["morphs"]["blink"].get<double>(), 0.375) &&
                    near(held.value["morphs"]["smile"].get<double>(), -2.0) && held.value["held_morphs"].size() == 1);
        GIANT_CHECK(f.run("set_morphs", {{"id", arm}, {"release_all", true}}).ok);
        GIANT_CHECK(f.get(arm)["held_morphs"].empty());
        // Refused, with nothing changed: a name it lacks, a weight past +-10, not a skinned mesh.
        GIANT_CHECK(f.run("set_morphs", {{"id", arm}, {"weights", {{"smile", 1.0}}}}).ok);
        const auto missing = f.run("set_morphs", {{"id", arm}, {"weights", {{"blink", 0.5}, {"frown", 1.0}}}});
        GIANT_CHECK(!missing.ok && missing.error.find("frown") != std::string::npos);
        GIANT_CHECK(!f.run("set_morphs", {{"id", arm}, {"weights", {{"blink", 11.0}}}}).ok);
        GIANT_CHECK(!f.run("set_morphs", {{"id", arm}, {"weights", {{"blink", "x"}}}}).ok);
        a = f.get(arm);
        GIANT_CHECK(a["held_morphs"].size() == 1 && near(a["held_morphs"]["smile"].get<double>(), 1.0));
        const world::EntityId cube = f.run("create_entity").value["id"].get<world::EntityId>();
        GIANT_CHECK(!f.run("set_morphs", {{"id", cube}, {"weights", {{"smile", 1.0}}}}).ok);
        GIANT_CHECK(!f.run("set_morphs", {{"id", 999}}).ok);
        // A new saved animation lets the held ones go.
        GIANT_CHECK(f.run("set_animation", {{"id", arm}, {"clip", "wink"}}).ok);
        f.frame_step(0.0);
        GIANT_CHECK(f.get(arm)["held_morphs"].empty());
        f.frame_step(0.5); // past its fade in, before the step
        a = f.get(arm);
        GIANT_CHECK(a["held_morphs"].empty() && near(a["morphs"]["smile"].get<double>(), 0.2));
        // A plain arm: no morphs, and set_morphs names none.
        f.arm = arm_file();
        const world::EntityId plain_arm = f.add_arm(3.0);
        GIANT_CHECK(f.run("set_animation", {{"id", plain_arm}, {"clip", "bend"}}).ok);
        GIANT_CHECK(f.get(plain_arm)["morphs"].empty());
        GIANT_CHECK(!f.run("set_morphs", {{"id", plain_arm}, {"weights", {{"smile", 1.0}}}}).ok);
        GIANT_CHECK(f.run("set_morphs", {{"id", plain_arm}}).ok); // nothing to hold: fine
        f.frame_step(0.1);
        // Nothing held on one not animating: it is not started for it.
        const world::EntityId idle = f.add_arm(6.0);
        GIANT_CHECK(f.run("set_morphs", {{"id", idle}, {"release_all", true}}).ok);
        GIANT_CHECK(f.animation->find(idle) == nullptr);
        // A hold on one not animating starts it, at its rest pose with the hold.
        f.arm = morph_arm_file();
        const world::EntityId still = f.add_arm(12.0);
        const auto started = f.run("set_morphs", {{"id", still}, {"weights", {{"blink", 0.5}}}});
        GIANT_CHECK(started.ok && started.value["animated"].get<bool>() &&
                    near(started.value["morphs"]["blink"].get<double>(), 0.5) &&
                    near(started.value["morphs"]["smile"].get<double>(), 0.25));
        // A file's weight past what the renderer takes (2000): held to its
        // limit, so the joints still pose.
        f.arm = morph_arm_file(2000.0f);
        const world::EntityId big = f.add_arm(9.0);
        GIANT_CHECK(f.run("set_animation", {{"id", big}, {"clip", "bend"}}).ok);
        f.frame_step(0.25);
        a = f.get(big);
        GIANT_CHECK(near(a["morphs"]["smile"].get<double>(), render::k_max_morph_weight));
        const u64 poses = a["poses"].get<u64>();
        f.frame_step(0.25);
        GIANT_CHECK(f.get(big)["poses"].get<u64>() > poses); // handed to the renderer
    }
}

namespace {

// The walker (geometry tests): Hips, LeftFoot and RightFoot; clips "idle",
// "walk" (1.4 m/s), "run" (4 m/s) and "wave" (once).
std::string walker_file() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "giant_animation_tests";
    std::filesystem::create_directories(dir);
    const std::filesystem::path path = dir / "walker.glb";
    const std::vector<u8> bytes = geometry::testing::skinned_walker();
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                static_cast<std::streamsize>(bytes.size()));
    return path.generic_string();
}

// Frames of a thirtieth of a second for `seconds`, going `along` m/s (0: no goal set, which stands).
void go(Fixture& f, world::EntityId id, float along, double seconds) {
    for (int i = 0; i < static_cast<int>(seconds * 30.0 + 0.5); ++i) {
        if (along != 0.0f) f.animation->set_goal(id, {0.0f, along, 0.0f});
        f.frame_step(1.0 / 30.0);
    }
}

} // namespace

GIANT_TEST("animation: an entity matched among its clips stands, walks and runs as its goal has it") {
    Fixture f;
    f.arm = walker_file();
    const world::EntityId id = f.add_arm();
    const auto set = f.run("set_animation", {{"id", id}, {"clip", "idle"}, {"matched", {"idle", "walk", "run"}}});
    GIANT_CHECK(set.ok && set.value["animated"].get<bool>());
    if (!set.ok) return;
    GIANT_CHECK(set.value["saved"]["matched"] == Json({"idle", "walk", "run"}));
    // (Whichever the build's worker has got to: being built, built since the command looked, or taken up.)
    const std::string began = set.value["matching"]["state"].get<std::string>();
    GIANT_CHECK(began == "building" || began == "ready" || began == "matching");
    f.animation->finish_motions();
    f.frame_step(1.0 / 30.0);
    Json a = f.get(id);
    GIANT_CHECK(a["matching"]["state"] == "matching" && a["matching"]["clip"] == "idle");
    GIANT_CHECK(a["main"].is_null()); // the saved clip plays no more
    GIANT_CHECK(a["matching"]["clips"].size() == 3 && a["matching"]["clips"][1]["clip"] == "walk" &&
                a["matching"]["clips"][1]["loop"].get<bool>() && a["matching"]["clips"][1]["frames"].get<u32>() == 30);
    GIANT_CHECK(a["matching"]["frames"].get<u32>() == 30 + 30 + 15 && !a["matching"].contains("problem"));

    const AnimationState::Animated* walker = f.animation->find(id);
    GIANT_CHECK(walker != nullptr && walker->matching);
    if (walker == nullptr) return;
    const auto hips = [walker] { return walker->model[0].column(3); };
    const auto left = [walker] { return walker->model[1].column(3); };

    // Sent walking: the walk plays, its travel taken out (the hips stay over the origin) while the feet swing.
    go(f, id, 1.4f, 1.0);
    a = f.get(id);
    GIANT_CHECK(a["matching"]["clip"] == "walk" && near(a["matching"]["goal"]["along"].get<double>(), 1.4));
    GIANT_CHECK(near(a["matching"]["root_motion"]["along"].get<double>(), 1.4, 0.05));
    float reach = 0.0f;
    float drift = 0.0f;
    const u64 poses = walker->poses;
    for (int i = 0; i < 30; ++i) {
        go(f, id, 1.4f, 1.0 / 30.0);
        reach = std::max(reach, std::fabs(left().z));
        drift = std::max(drift, std::fabs(hips().z) + std::fabs(hips().x));
    }
    GIANT_CHECK(reach > 0.25f && reach < 0.35f);
    GIANT_CHECK(drift < 0.02f && near(hips().y, 1.0));
    GIANT_CHECK(walker->poses > poses + 20);
    GIANT_CHECK(f.get(id)["matching"]["clip"] == "walk");

    // Faster: the run. No goal: it stands again.
    go(f, id, 4.0f, 1.0);
    a = f.get(id);
    GIANT_CHECK(a["matching"]["clip"] == "run" && a["matching"]["jumps"].get<u64>() >= 2);
    go(f, id, 0.0f, 1.0);
    a = f.get(id);
    GIANT_CHECK(a["matching"]["clip"] == "idle" && near(a["matching"]["goal"]["along"].get<double>(), 0.0));
    // A goal that is no number stands too.
    f.animation->set_goal(id, {std::nanf(""), std::nanf(""), std::nanf("")});
    f.frame_step(1.0 / 30.0);
    GIANT_CHECK(near(f.get(id)["matching"]["goal"]["along"].get<double>(), 0.0) && std::isfinite(hips().y));

    // Goals are kept within what moves and turns.
    f.animation->set_goal(id, {-1000.0f, 1000.0f, 50.0f});
    f.frame_step(0.0);
    a = f.get(id);
    GIANT_CHECK(near(a["matching"]["goal"]["across"].get<double>(), -100.0) &&
                near(a["matching"]["goal"]["along"].get<double>(), 100.0) &&
                near(a["matching"]["goal"]["turn"].get<double>(), 6.2831853, 1e-4));
    // A walker's goal is its velocity seen from where it faces.
    const AnimationState::MotionGoal ahead = AnimationState::goal_of({1.0f, 0.3f, 1.0f}, 0.7853982f);
    GIANT_CHECK(near(ahead.along, std::sqrt(2.0)) && near(ahead.across, 0.0) && ahead.turn == 0.0f);
    const AnimationState::MotionGoal side = AnimationState::goal_of({2.0f, 0.0f, 0.0f}, 0.0f); // facing +z, pushed to +x
    GIANT_CHECK(near(side.across, 2.0) && near(side.along, 0.0));
    const AnimationState::MotionGoal behind = AnimationState::goal_of({2.0f, 0.0f, 0.0f}, -1.5707963f); // facing -x
    GIANT_CHECK(near(behind.along, -2.0) && near(behind.across, 0.0));

    // Paused, nothing moves; a step by hand moves the matcher.
    go(f, id, 1.4f, 1.0);
    GIANT_CHECK(f.run("step_animation", {{"paused", true}}).ok);
    const double before = f.get(id)["matching"]["time"].get<double>();
    go(f, id, 1.4f, 0.2);
    GIANT_CHECK(near(f.get(id)["matching"]["time"].get<double>(), before, 1e-6));
    GIANT_CHECK(f.run("step_animation", {{"seconds", 0.1}}).ok);
    const double after = f.get(id)["matching"]["time"].get<double>();
    GIANT_CHECK(near(std::fmod(after - before + 1.0, 1.0), 0.1, 0.035));
    GIANT_CHECK(f.run("step_animation", {{"paused", false}}).ok);

    // A command's play takes over; set_animation hands it back to the matcher.
    const auto played = f.run("play_animation", {{"id", id}, {"clip", "wave"}, {"blend_seconds", 0.0}});
    GIANT_CHECK(played.ok && played.value["matching"]["state"] == "command" && played.value["main"]["clip"] == "wave");
    GIANT_CHECK(!walker->matching && near(left().y, 0.0, 1e-4) && near(left().z, 0.0, 1e-4)); // the wave's start, at once
    go(f, id, 1.4f, 0.5);
    GIANT_CHECK(near(left().y, 0.5, 0.01) && f.get(id)["matching"]["state"] == "command");
    const auto back = f.run("set_animation", {{"id", id}, {"clip", "idle"}});
    GIANT_CHECK(back.ok && back.value["matching"]["state"] == "matching" && back.value["saved"]["matched"].size() == 3);
    go(f, id, 1.4f, 0.5);
    GIANT_CHECK(f.get(id)["matching"]["clip"] == "walk" && near(left().y, 0.0, 0.02));

    // An empty list: the plain clip again, its database let go.
    GIANT_CHECK(f.animation->motions() == 1);
    const auto plain = f.run("set_animation", {{"id", id}, {"clip", "walk"}, {"matched", Json::array()}});
    GIANT_CHECK(plain.ok && !plain.value.contains("matching") && plain.value["main"]["clip"] == "walk");
    GIANT_CHECK(f.animation->motions() == 0 && !walker->matching);
    GIANT_CHECK(f.commands.undo()); // matched again
    f.frame_step(0.0);
    f.animation->finish_motions();
    go(f, id, 0.0f, 0.2);
    GIANT_CHECK(f.get(id)["matching"]["state"] == "matching" && f.get(id)["matching"]["clip"] == "idle");
}

GIANT_TEST("animation: a file that cannot be matched plays its saved clip, and says why") {
    Fixture f;
    // No hips or feet by name: the arm.
    const world::EntityId arm = f.add_arm();
    GIANT_CHECK(f.run("set_animation", {{"id", arm}, {"clip", "bend"}, {"matched", {"*"}}}).ok);
    f.animation->finish_motions();
    f.frame_step(0.25);
    f.frame_step(0.25);
    Json a = f.get(arm);
    GIANT_CHECK(a["matching"]["state"] == "problem" && a["main"]["clip"] == "bend" && a["main"]["time"].get<double>() > 0.2);
    GIANT_CHECK(a["matching"]["problem"].get<std::string>().find("hips and feet") != std::string::npos);
    GIANT_CHECK(!f.animation->find(arm)->matching);

    // Only a clip played once, as long as the look ahead: no frame to start from.
    f.arm = walker_file();
    const world::EntityId waver = f.add_arm(3.0);
    GIANT_CHECK(f.run("set_animation", {{"id", waver}, {"clip", "wave"}, {"loop", false}, {"matched", {"wave"}}}).ok);
    f.animation->finish_motions();
    f.frame_step(0.25);
    a = f.get(waver);
    GIANT_CHECK(a["matching"]["state"] == "problem" && !a["matching"]["problem"].get<std::string>().empty() &&
                a["main"]["clip"] == "wave");

    // No clip to fall back on: it is animated all the same, and stands at rest until matched.
    const world::EntityId bare = f.add_arm(6.0);
    const auto set = f.run("set_animation", {{"id", bare}, {"clip", ""}, {"matched", {"*"}}});
    GIANT_CHECK(set.ok && set.value["animated"].get<bool>());
    f.animation->finish_motions();
    f.frame_step(0.1);
    a = f.get(bare);
    GIANT_CHECK(a["matching"]["state"] == "matching" && a["matching"]["clips"].size() == 4 &&
                !a["matching"]["clips"][3]["loop"].get<bool>() && a["matching"]["clip"] == "idle");
}

GIANT_TEST("animation: set_animation checks the clips to match among; entities of a file and list share a database") {
    Fixture f;
    f.arm = walker_file();
    const world::EntityId one = f.add_arm();
    const world::EntityId two = f.add_arm(2.0);
    const world::EntityId three = f.add_arm(4.0);
    const world::EntityId cube = f.run("create_entity").value["id"].get<world::EntityId>();
    const auto refused = [&f](world::EntityId id, const Json& matched, const char* expected) {
        const auto r = f.run("set_animation", {{"id", id}, {"clip", ""}, {"matched", matched}});
        GIANT_CHECK(!r.ok && r.error.find(expected) != std::string::npos);
        if (r.ok || r.error.find(expected) == std::string::npos) std::fprintf(stderr, "    %s\n", r.error.c_str());
    };
    refused(one, {"idle", "trot"}, "has no clip named \"trot\"");
    refused(one, {"idle", "*"}, "goes alone");
    refused(one, {"idle", "walk", "idle"}, "\"idle\" is in `matched` twice");
    refused(cube, {"idle"}, "is not a mesh");
    GIANT_CHECK(!f.run("set_animation", {{"id", one}, {"clip", ""}, {"matched", {""}}}).ok);
    GIANT_CHECK(!f.run("set_animation", {{"id", one}, {"clip", ""}, {"matched", "idle"}}).ok);
    GIANT_CHECK(f.state.scene.find(one)->animation == world::EntityAnimation{} && f.animation->motions() == 0);
    // Nothing to clear on one that cannot animate.
    GIANT_CHECK(f.run("set_animation", {{"id", cube}, {"clip", ""}, {"matched", Json::array()}}).ok);

    const Json list{"idle", "walk"};
    GIANT_CHECK(f.run("set_animation", {{"id", one}, {"clip", ""}, {"matched", list}}).ok);
    GIANT_CHECK(f.run("set_animation", {{"id", two}, {"clip", "idle"}, {"matched", list}}).ok);
    GIANT_CHECK(f.animation->motions() == 1);
    GIANT_CHECK(f.animation->find(one)->motion == f.animation->find(two)->motion);
    GIANT_CHECK(f.run("set_animation", {{"id", three}, {"clip", ""}, {"matched", {"*"}}}).ok);
    GIANT_CHECK(f.animation->motions() == 2);
    // Saved with the scene, and shown on the entity.
    const auto entity = f.run("get_entity", {{"id", one}});
    GIANT_CHECK(entity.ok && entity.value["animation"]["matched"] == list);
    world::Scene loaded;
    GIANT_CHECK(!world::load_scene(world::save_scene(f.state.scene), loaded).has_value() &&
                loaded.find(one)->animation.matched == std::vector<std::string>({"idle", "walk"}));
    // Only speed changed: the same database, kept.
    const auto* motion = f.animation->find(one)->motion.get();
    GIANT_CHECK(f.run("set_animation", {{"id", one}, {"clip", ""}, {"speed", 2.0}}).ok);
    GIANT_CHECK(f.animation->find(one)->motion.get() == motion);
    // Let go with its last entity (hidden, removed, or matched no more).
    GIANT_CHECK(f.run("delete_entities", {{"ids", {one}}}).ok);
    f.frame_step(0.0);
    GIANT_CHECK(f.animation->motions() == 2 && f.animation->find(one) == nullptr);
    GIANT_CHECK(f.run("set_entity", {{"id", two}, {"visible", false}}).ok);
    f.frame_step(0.0);
    GIANT_CHECK(f.animation->motions() == 1);
    GIANT_CHECK(f.commands.undo());
    f.frame_step(0.0);
    GIANT_CHECK(f.animation->motions() == 2);
    f.animation->finish_motions();
    f.frame_step(0.1);
    GIANT_CHECK(f.get(two)["matching"]["state"] == "matching" && f.get(two)["matching"]["frames"].get<u32>() == 60);
    // A new scene lets all go (builds still running end unseen).
    GIANT_CHECK(f.run("new_scene").ok);
    f.frame_step(0.0);
    GIANT_CHECK(f.animation->motions() == 0 && f.animation->animated() == 0);
}

GIANT_TEST("animation: matched entities are posed for the renderer, far ones less often") {
    Fixture f(true);
    f.arm = walker_file();
    const world::EntityId id = f.add_arm();
    GIANT_CHECK(f.run("set_animation", {{"id", id}, {"clip", "idle"}, {"matched", {"*"}}}).ok);
    f.animation->finish_motions();
    go(f, id, 1.4f, 1.0);
    const AnimationState::Animated* a = f.animation->find(id);
    GIANT_CHECK(a != nullptr && a->matching && a->drawn && f.get(id)["matching"]["clip"] == "walk");
    if (a == nullptr) return;
    const u64 poses = a->poses;
    go(f, id, 1.4f, 1.0);
    GIANT_CHECK(a->poses >= poses + 25);
    // Small on screen: four poses a second, the matcher moved on by the time between.
    render::View view;
    view.position = {0.0, 1.0, 2000.0};
    f.animation->set_view(view, 1080);
    go(f, id, 1.4f, 0.5);
    const u64 far_poses = a->poses;
    const double time = f.get(id)["matching"]["time"].get<double>();
    go(f, id, 1.4f, 1.0);
    GIANT_CHECK(a->poses - far_poses >= 3 && a->poses - far_poses <= 5);
    GIANT_CHECK(near(f.get(id)["matching"]["time"].get<double>(), time, 0.3)); // a second on: round the walk once
    GIANT_CHECK(f.get(id)["matching"]["clip"] == "walk");
}

GIANT_TEST("animation: the saved clip plays while a database is built; a crowd comes to matching a few a frame") {
    Fixture f;
    f.arm = walker_file();
    f.animation->hold_builds(true);
    std::vector<world::EntityId> ids;
    for (int i = 0; i < 5; ++i) ids.push_back(f.add_arm(2.0 * i));
    for (const world::EntityId id : ids) {
        GIANT_CHECK(f.run("set_animation", {{"id", id}, {"clip", "walk"}, {"matched", {"idle", "walk", "run"}}}).ok);
    }
    for (int i = 0; i < 6; ++i) f.frame_step(0.1);
    Json a = f.get(ids[0]);
    GIANT_CHECK(a["matching"]["state"] == "building" && !a["matching"].contains("frames"));
    GIANT_CHECK(a["main"]["clip"] == "walk" && near(a["main"]["time"].get<double>(), 0.5, 0.11));
    GIANT_CHECK(!f.animation->find(ids[0])->matching && f.animation->motions() == 1);
    // A step by hand takes a database that has arrived up at once.
    f.animation->matches_per_frame = 2;
    f.animation->finish_motions();
    const auto matching = [&f, &ids] {
        usize n = 0;
        for (const world::EntityId id : ids) n += f.animation->find(id)->matching ? usize{1} : usize{0};
        return n;
    };
    GIANT_CHECK(matching() == 0);
    f.frame_step(0.1);
    GIANT_CHECK(matching() == 2 && f.animation->find(ids[0])->matching && !f.animation->find(ids[4])->matching &&
                f.animation->find(ids[4])->player.main().clip == 1); // the walk still
    f.frame_step(0.1);
    GIANT_CHECK(matching() == 4);
    GIANT_CHECK(f.run("step_animation", {{"seconds", 0.1}}).ok);
    GIANT_CHECK(matching() == 5 && f.get(ids[4])["matching"]["clip"] == "idle" && f.get(ids[4])["main"].is_null());

    // One let go before its build began is not built at all.
    f.animation->hold_builds(true);
    const world::EntityId late = f.add_arm(20.0);
    GIANT_CHECK(f.run("set_animation", {{"id", late}, {"clip", "walk"}, {"matched", {"run"}}}).ok);
    const std::weak_ptr<AnimationState::Motion> motion = f.animation->find(late)->motion;
    const AnimationState::Motion* raw = f.animation->find(late)->motion.get();
    GIANT_CHECK(f.run("set_animation", {{"id", late}, {"clip", "walk"}, {"matched", Json::array()}}).ok);
    GIANT_CHECK(f.animation->motions() == 1 && motion.use_count() == 1); // the job's alone
    GIANT_CHECK(!raw->ready.load() && raw->db.frames() == 0);
    f.animation->finish_motions();
    GIANT_CHECK(motion.expired());
}

GIANT_TEST("animation: one that cannot be matched and has no clip takes no place") {
    Fixture f;
    f.animation->max_animated = 1;
    f.animation->hold_builds(true);
    const world::EntityId arm = f.add_arm(); // no hips or feet by name
    const auto set = f.run("set_animation", {{"id", arm}, {"clip", ""}, {"matched", {"*"}}});
    GIANT_CHECK(set.ok && set.value["animated"].get<bool>()); // until that is known
    f.arm = walker_file();
    const world::EntityId walker = f.add_arm(3.0);
    GIANT_CHECK(f.run("set_animation", {{"id", walker}, {"clip", ""}, {"matched", {"*"}}}).ok);
    Json waiting = f.get(walker);
    GIANT_CHECK(!waiting["animated"].get<bool>() && waiting["why"].get<std::string>().find("waiting for a place") == 0);
    f.animation->finish_motions();
    f.frame_step(0.1);
    f.frame_step(0.1);
    Json a = f.get(arm);
    GIANT_CHECK(!a["animated"].get<bool>() && a["why"].get<std::string>().find("cannot be matched (its hips and feet") !=
                                                   std::string::npos);
    GIANT_CHECK(f.animation->find(arm) == nullptr && f.animation->frozen() == 0 && f.animation->waiting() == 0);
    f.animation->finish_motions(); // (the walker's, begun when it got the place)
    f.frame_step(0.1);
    GIANT_CHECK(f.get(walker)["matching"]["state"] == "matching"); // its place went to the one that can
    // Not built again each time it is asked for: what could not be done is kept (until a new scene).
    GIANT_CHECK(f.animation->motions() == 2);
    GIANT_CHECK(f.run("set_animation", {{"id", arm}, {"clip", ""}, {"matched", {"*"}}}).ok);
    f.frame_step(0.1);
    GIANT_CHECK(f.animation->find(arm) == nullptr && f.animation->motions() == 2);
    // A crowd of them, wanted while that was not yet known, is not started to be stopped again.
    {
        Fixture crowd;
        crowd.animation->max_animated = 2;
        crowd.animation->hold_builds(true);
        std::vector<world::EntityId> ids;
        for (int i = 0; i < 12; ++i) {
            ids.push_back(crowd.add_arm(2.0 * i));
            crowd.state.scene.modify(ids.back())->animation.matched = {"*"}; // as loaded
        }
        crowd.frame_step(0.1);
        GIANT_CHECK(crowd.animation->animated() == 2 && crowd.animation->frozen() == 10);
        crowd.animation->finish_motions();
        crowd.frame_step(0.1); // the two stopped
        crowd.frame_step(0.1); // the rest let go, none started
        GIANT_CHECK(crowd.animation->animated() == 0 && crowd.animation->frozen() == 0 && crowd.animation->waiting() == 0);
        GIANT_CHECK(crowd.animation->motions() == 1);
        // Meshes imported again: what was kept for the old file goes.
        GIANT_CHECK(crowd.run("import_mesh", {{"path", crowd.arm}, {"reload", true}}).ok);
        crowd.animation->hold_builds(true);
        crowd.frame_step(0.1);
        GIANT_CHECK(crowd.animation->motions() == 1 && crowd.animation->animated() == 2); // the new file's, asked afresh
        crowd.animation->finish_motions();
    }
    // A hidden one says so only when it would have played.
    GIANT_CHECK(f.run("set_entity", {{"id", arm}, {"visible", false}}).ok);
    GIANT_CHECK(f.get(arm)["why"] == "nothing to play: it stands in its rest pose");
    GIANT_CHECK(f.run("set_entity", {{"id", walker}, {"visible", false}}).ok);
    GIANT_CHECK(f.get(walker)["why"] == "hidden");
    GIANT_CHECK(f.run("new_scene").ok);
    f.frame_step(0.0);
    GIANT_CHECK(f.animation->motions() == 0);
}

GIANT_TEST("animation: a matched character is no obstacle to walk round, as one playing a clip is none") {
    Fixture f;
    f.arm = walker_file();
    const world::EntityId id = f.add_arm();
    std::vector<nav::Obstacle> out;
    nav_obstacles(f.state.scene, *f.state.scene.find(id), nullptr, f.meshes.get(), out);
    GIANT_CHECK(!out.empty()); // standing at rest: a thing in the way
    GIANT_CHECK(f.run("set_animation", {{"id", id}, {"clip", ""}, {"matched", {"*"}}}).ok);
    out.clear();
    nav_obstacles(f.state.scene, *f.state.scene.find(id), nullptr, f.meshes.get(), out);
    GIANT_CHECK(out.empty());
}

GIANT_TEST("animation: a pose held from outside shows instead of the clips', until released") {
    Fixture f;
    const world::EntityId arm = f.add_arm();
    GIANT_CHECK(f.run("set_animation", {{"id", arm}, {"clip", "bend"}}).ok);
    f.frame_step(0.1);
    const AnimationState::Animated* a = f.animation->find(arm);
    GIANT_CHECK(a != nullptr && !a->held_pose && !f.animation->holds_pose(arm));
    if (a == nullptr) return;
    // Held: what it showed stays, while its clip's time moves on underneath.
    const u64 serial = f.animation->serial();
    GIANT_CHECK(f.animation->hold_pose(f.state.scene, *f.meshes, arm).empty());
    GIANT_CHECK(a->held_pose && f.animation->holds_pose(arm) && f.animation->serial() != serial);
    GIANT_CHECK(f.get(arm)["held"].get<bool>());
    const std::vector<Mat4> held = a->model;
    const u64 poses = a->poses;
    const float time = a->player.main().time;
    f.frame_step(0.2);
    GIANT_CHECK(a->player.main().time != time);
    GIANT_CHECK(std::memcmp(a->model.data(), held.data(), held.size() * sizeof(Mat4)) == 0 && a->poses == poses);
    // A pose given: shown from the next frame; the same again is no new pose; not one a joint is none.
    std::vector<Mat4> given = held;
    given[1].at(1, 3) += 0.25f;
    GIANT_CHECK(!f.animation->set_held_pose(arm, std::span<const Mat4>(given).first(1)));
    GIANT_CHECK(f.animation->set_held_pose(arm, given));
    f.frame_step(1.0 / 60.0);
    GIANT_CHECK(std::memcmp(a->model.data(), given.data(), given.size() * sizeof(Mat4)) == 0 && a->poses == poses + 1);
    GIANT_CHECK(f.animation->set_held_pose(arm, given));
    f.frame_step(1.0 / 60.0);
    GIANT_CHECK(a->poses == poses + 1);
    // Its size on screen is told from where it is drawn when that is given (a ragdoll far from where the scene has it).
    {
        render::View view;
        view.position = {0.0, 1.0, 4.0};
        view.vertical_fov_degrees = 60.0f;
        const DVec3 far_off{2000.0, 0.0, 0.0}, here{0.0, 0.0, 0.0};
        f.animation->set_view(view, 1080);
        f.frame_step(1.0 / 60.0);
        GIANT_CHECK(a->interval == 0.0); // near: every frame
        GIANT_CHECK(f.animation->set_held_pose(arm, given, &far_off));
        f.animation->set_view(view, 1080);
        f.frame_step(1.0 / 60.0);
        GIANT_CHECK(a->interval > 0.1);
        GIANT_CHECK(f.animation->set_held_pose(arm, given, &here));
        f.animation->set_view(view, 1080);
        f.frame_step(1.0 / 60.0);
        GIANT_CHECK(a->interval == 0.0 && a->poses == poses + 1);
    }
    // Its own animation set meanwhile plays underneath: still held.
    GIANT_CHECK(f.run("set_animation", {{"id", arm}, {"clip", "hop"}}).ok);
    f.frame_step(0.1);
    GIANT_CHECK(f.animation->find(arm) == a && a->held_pose && std::memcmp(a->model.data(), given.data(), given.size() * sizeof(Mat4)) == 0);
    // Released: its clips pose it again.
    f.animation->release_pose(f.state.scene, *f.meshes, arm);
    GIANT_CHECK(!f.animation->holds_pose(arm) && f.animation->find(arm) == a && !a->held_pose);
    f.frame_step(0.1);
    GIANT_CHECK(std::memcmp(a->model.data(), given.data(), given.size() * sizeof(Mat4)) != 0);
    GIANT_CHECK(!f.animation->set_held_pose(arm, given)); // (held no more)

    // One with nothing to play is animated only while held.
    const world::EntityId bare = f.add_arm(3.0);
    f.frame_step(0.1);
    GIANT_CHECK(f.animation->find(bare) == nullptr && f.animation->animated() == 1);
    GIANT_CHECK(f.animation->hold_pose(f.state.scene, *f.meshes, bare).empty());
    GIANT_CHECK(f.animation->find(bare) != nullptr && f.animation->animated() == 2);
    f.frame_step(0.1);
    GIANT_CHECK(f.animation->find(bare) != nullptr && f.animation->holds_pose(bare)); // (kept, with nothing of its own to play)
    f.animation->release_pose(f.state.scene, *f.meshes, bare);
    GIANT_CHECK(f.animation->find(bare) == nullptr && f.animation->animated() == 1);
    // Hidden, the hold goes with its copy. Not a skinned mesh, no room: refused.
    GIANT_CHECK(f.animation->hold_pose(f.state.scene, *f.meshes, bare).empty());
    GIANT_CHECK(f.run("set_entity", {{"id", bare}, {"visible", false}}).ok);
    f.frame_step(0.1);
    GIANT_CHECK(!f.animation->holds_pose(bare) && f.animation->find(bare) == nullptr);
    GIANT_CHECK(!f.animation->hold_pose(f.state.scene, *f.meshes, bare).empty());
    GIANT_CHECK(!f.animation->hold_pose(f.state.scene, *f.meshes, 987654).empty());
    const world::EntityId more = f.add_arm(6.0);
    f.animation->max_animated = 1;
    GIANT_CHECK(!f.animation->hold_pose(f.state.scene, *f.meshes, more).empty() && !f.animation->holds_pose(more));
}
