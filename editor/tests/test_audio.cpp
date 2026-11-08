// The editor's sound: the terrain marched for echoes (a slope met where
// it rises), a cube met through the physics; play_sound heard at the
// camera with the cliff's answer; record_audio's file; load_sound.
#include "../src/audio_state.h"
#include "../src/editor_commands.h"
#include "../src/physics_state.h"

#include "giant_test.h"

#include "giant/audio/sounds.h"
#include "giant/world/scene.h"

#include <cmath>
#include <filesystem>

using namespace giant;
using namespace giant::editor;
using ai::Json;

namespace {

struct Fixture {
    PhysicsState physics;
    AudioState audio;
    ai::CommandRegistry commands;
    EditorState state;
    explicit Fixture(bool with_physics = false) {
        if (with_physics) state.physics = &physics;
        state.audio = &audio;
        register_scene_commands(commands, state);
        register_audio_commands(commands, state);
    }
    ai::CommandResult run(const char* name, const Json& args = Json::object()) {
        return commands.execute(name, args, ai::ExecuteOptions{});
    }
    // Flat ground at 0 over cells 0..5 on x, a cliff 300 m high from cell 4 on (x 256 on).
    void valley() {
        world::Terrain& t = state.scene.terrain;
        t.set_samples(8);
        for (int x = 0; x < 6; ++x)
            for (int z = -1; z <= 1; ++z) t.set({x, z}, world::Terrain::Heights(64, x >= 4 ? 300.0f : 0.0f));
    }
};

} // namespace

GIANT_TEST("editor audio: the terrain is met where it rises; nothing where there is none; a body through the physics") {
    Fixture f;
    f.valley();
    const world::Terrain& t = f.state.scene.terrain;
    // Level at 10 m toward +x: the rise from 0 at x 248 to 300 at x 256 is met near x 248.3.
    const auto hit = march_terrain(t, {10.0, 10.0, 32.0}, {1, 0, 0}, 1000.0);
    GIANT_CHECK(hit.has_value());
    if (hit) {
        GIANT_CHECK(std::fabs(hit->distance - (248.0 + 8.0 * 10.0 / 300.0 - 10.0)) < 0.05);
        GIANT_CHECK(hit->normal.x < -0.9f); // facing back at the ray
        GIANT_CHECK(hit->hardness > 0.7f);  // a cliff: bare and hard
    }
    // Down onto the flat: soft.
    const auto down = march_terrain(t, {100.0, 50.0, 32.0}, {0, -1, 0}, 1000.0);
    GIANT_CHECK(down && std::fabs(down->distance - 50.0) < 0.05 && down->hardness < 0.35f);
    // Up: nothing; short of the cliff: nothing; off the tiles: nothing.
    GIANT_CHECK(!march_terrain(t, {100.0, 10.0, 32.0}, {0, 1, 0}, 1000.0));
    GIANT_CHECK(!march_terrain(t, {10.0, 10.0, 32.0}, {1, 0, 0}, 100.0));
    GIANT_CHECK(!march_terrain(t, {10.0, 10.0, 500.0}, {0, 0, 1}, 1000.0));
    // Under the ground: met at once.
    const auto under = march_terrain(t, {100.0, -5.0, 32.0}, {1, 0, 0}, 1000.0);
    GIANT_CHECK(under && under->distance == 0.0);

    // A concrete wall through the physics: hard, met first.
    Fixture g(true);
    const ai::CommandResult made = g.run("create_entity", {{"shape", "cube"}, {"position", {20.0, 2.0, 0.0}},
                                                          {"scale", {1.0, 4.0, 10.0}}, {"body", "fixed"},
                                                          {"surface", "concrete"}});
    GIANT_CHECK(made.ok);
    g.physics.flush(g.state.scene);
    const audio::RayCast cast = g.audio.ray_cast(g.state.scene, &g.physics);
    const audio::RayHit h = cast({0.0, 2.0, 0.0}, {1, 0, 0}, 1000.0);
    GIANT_CHECK(h.hit && std::fabs(h.distance - 19.5) < 0.05 && h.hardness == hardness_of(world::Surface::concrete));
    GIANT_CHECK(h.normal.x < -0.9f);
    GIANT_CHECK(!cast({0.0, 2.0, 0.0}, {-1, 0, 0}, 1000.0).hit);
}

GIANT_TEST("editor audio: a shot in the valley is heard late from its side, the cliff answering after it") {
    Fixture f;
    f.valley();
    f.state.camera.position = {50.0, 1.7, 32.0};
    f.state.camera.yaw_degrees = 0.0f; // looking -Z: +X on the right
    f.audio.update(f.state.camera, 0.0);
    ai::CommandResult r = f.run("play_sound", {{"at", {150.0, 1.7, 32.0}}, {"loudness", 100.0}, {"rays", 128}});
    GIANT_CHECK(r.ok);
    GIANT_CHECK(std::fabs(r.value["distance"].get<double>() - 100.0) < 1e-6);
    const Json taps = r.value["taps"]; // (r is reused below)
    GIANT_CHECK(taps.size() >= 2);
    GIANT_CHECK(std::fabs(taps[0]["delay"].get<double>() - 100.0 / 343.0) < 1e-6);
    GIANT_CHECK(taps[0]["direction"][0].get<double>() > 0.99);
    GIANT_CHECK(r.value["occluded"] == false);
    // The cliff (x 248) answers: 98 m out, 198 m back.
    bool cliff = false;
    for (usize k = 1; k < taps.size(); ++k) {
        const double d = taps[k]["delay"].get<double>();
        if (std::fabs(d - 296.0 / 343.0) < 0.06 && taps[k]["direction"][0].get<double>() > 0.7) cliff = true;
    }
    GIANT_CHECK(cliff);
    GIANT_CHECK(r.value["openness"].get<double>() > 0.2 && r.value["openness"].get<double>() < 0.9);
    GIANT_CHECK(f.audio.played() == 1);
    f.audio.update(f.state.camera, 0.25); // no speakers: rendered with the frames
    r = f.run("get_audio");
    GIANT_CHECK(r.ok && r.value["played"] == 1 && r.value["frames"].get<u64>() == audio::k_sample_rate / 4);
    GIANT_CHECK(r.value["voices"].get<u32>() == 1); // still to be heard
    GIANT_CHECK(r.value["heard"]["taps"].size() == taps.size());

    // High over the cliff top: seen. Beyond its edge, low: the direct way through rock, muffled.
    r = f.run("play_sound", {{"at", {300.0, 600.0, 32.0}}, {"rays", 0}});
    GIANT_CHECK(r.ok && r.value["occluded"] == false && r.value["taps"].size() == 1);
    r = f.run("play_sound", {{"at", {300.0, 320.0, 32.0}}, {"rays", 0}});
    GIANT_CHECK(r.ok && r.value["occluded"] == true);
    GIANT_CHECK(r.value["taps"][0]["lowpass"].get<double>() <= 900.0);
}

GIANT_TEST("editor audio: record_audio writes what arrives when; load_sound reads it back; bad names refused") {
    Fixture f;
    f.valley();
    f.state.camera.position = {50.0, 1.7, 32.0};
    f.audio.update(f.state.camera, 0.0);
    const std::string path = (std::filesystem::temp_directory_path() / "giant_audio_test.wav").string();
    ai::CommandResult r = f.run("record_audio", {{"at", {150.0, 1.7, 32.0}}, {"loudness", 100.0}, {"seconds", 2.0},
                                                 {"path", path}});
    GIANT_CHECK(r.ok);
    GIANT_CHECK(std::fabs(r.value["first_heard"].get<double>() - 100.0 / 343.0) < 0.003);
    GIANT_CHECK(r.value["peak"].get<double>() > 0.05 && r.value["peak"].get<double>() <= 1.0);
    GIANT_CHECK(std::filesystem::file_size(path) == 44 + 2 * 2 * 2 * audio::k_sample_rate);
    // Not on the speakers.
    GIANT_CHECK(f.audio.played() == 0);

    r = f.run("load_sound", {{"name", "shot_in_valley"}, {"path", path}});
    GIANT_CHECK(r.ok && std::fabs(r.value["seconds"].get<double>() - 2.0) < 1e-6);
    GIANT_CHECK(!f.run("load_sound", {{"name", "shot_in_valley"}, {"path", path}}).ok);
    GIANT_CHECK(!f.run("load_sound", {{"name", "nothing"}, {"path", path + ".missing"}}).ok);
    r = f.run("get_audio");
    bool listed = false;
    for (const Json& n : r.value["sounds"]) listed = listed || n == "shot_in_valley";
    GIANT_CHECK(listed);
    GIANT_CHECK(r.value["device"] == "");
    GIANT_CHECK(f.run("play_sound", {{"sound", "shot_in_valley"}, {"at", {60.0, 1.7, 32.0}}}).ok);
    GIANT_CHECK(!f.run("play_sound", {{"sound", "no_such"}, {"at", {60.0, 1.7, 32.0}}}).ok);
    GIANT_CHECK(f.run("stop_sounds").ok);
    std::filesystem::remove(path);

    // No audio: refused.
    EditorState none;
    ai::CommandRegistry commands;
    register_audio_commands(commands, none);
    GIANT_CHECK(!commands.execute("get_audio", Json::object(), ai::ExecuteOptions{}).ok);
}
