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
#include <random>
#include <string>

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
    // Just under it (rounding), heading down: met at once, not a step on.
    const auto just = march_terrain(t, {100.0, -0.03, 32.0}, {0.7071f, -0.7071f, 0}, 1000.0);
    GIANT_CHECK(just && just->distance < 0.1);
    // A narrow ridge 1 km off (one sample, 16 m across its foot, 50 m high): met from any start.
    {
        world::Terrain ridge;
        ridge.set_samples(8);
        for (int x = 0; x < 20; ++x) {
            world::Terrain::Heights h(64, 0.0f);
            if (x == 16)
                for (int j = 0; j < 8; ++j) h[static_cast<usize>(j * 8 + 4)] = 50.0f; // x = 1056 (from 1048, met at 10 m high near 1049.6)
            ridge.set({x, 0}, h);
        }
        int met = 0;
        for (int k = 0; k < 40; ++k) {
            const auto r = march_terrain(ridge, {k * 0.5, 10.0, 32.0}, {1, 0, 0}, 1500.0);
            if (r && std::fabs(r->distance + k * 0.5 - 1049.6) < 0.1) ++met;
        }
        GIANT_CHECK(met == 40);
    }

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

GIANT_TEST("editor audio: the player's capsule round the listener neither muffles nor swallows the echoes") {
    Fixture f(true);
    f.valley();
    f.state.camera.position = {50.0, 1.65, 32.0};
    f.physics.world().add_character({.position = {50.0, 0.0, 32.0}}); // the eyes inside it
    f.physics.flush(f.state.scene);
    ai::CommandResult r = f.run("play_sound", {{"at", {100.0, 1.65, 32.0}}, {"rays", 0}});
    GIANT_CHECK(r.ok && r.value["occluded"] == false);
    // A shot made at the eyes: its rays leave the capsule and meet the valley.
    r = f.run("play_sound", {{"at", {50.0, 1.6, 32.0}}, {"rays", 64}});
    GIANT_CHECK(r.ok && r.value["openness"].get<double>() > 0.2 && r.value["mean_free_path"].get<double>() > 1.0);
}

GIANT_TEST("editor audio: heard from the right in the right ear, as the camera turns") {
    Fixture f;
    f.state.camera.position = {0.0, 1.7, 0.0};
    const auto ears = [&](float yaw) {
        f.state.camera.yaw_degrees = yaw;
        const std::string path = (std::filesystem::temp_directory_path() /
                                  ("giant_audio_pan_" + std::to_string(std::random_device{}()) + ".wav")).string();
        const ai::CommandResult r = f.run("record_audio", {{"at", {20.0, 1.7, 0.0}}, {"seconds", 0.3}, {"path", path},
                                                           {"rays", 0}, {"loudness", 20.0}});
        GIANT_CHECK(r.ok);
        const std::optional<audio::Sound> mono = audio::load_wav(path); // (read back only to see it is there)
        GIANT_CHECK(mono.has_value());
        std::filesystem::remove(path);
        const AudioState::Recording rec =
            f.audio.record(f.state.scene, nullptr, *f.audio.find("gunshot"), {20.0, 1.7, 0.0}, 20.0f, {}, 0.3);
        double l = 0, rr = 0;
        for (usize k = 0; k + 1 < rec.frames.size(); k += 2) {
            l += rec.frames[k] * rec.frames[k];
            rr += rec.frames[k + 1] * rec.frames[k + 1];
        }
        return std::pair{l, rr};
    };
    const auto [l0, r0] = ears(0.0f);    // looking -Z: +X is right
    GIANT_CHECK(r0 > l0 * 100.0);
    const auto [l1, r1] = ears(180.0f);  // looking +Z: +X is left
    GIANT_CHECK(l1 > r1 * 100.0);
    const auto [l2, r2] = ears(90.0f);   // looking -X, at it: both alike
    GIANT_CHECK(std::fabs(l2 - r2) < 0.05 * (l2 + r2));
}

GIANT_TEST("editor audio: record_audio writes what arrives when; load_sound reads it back; bad names refused") {
    Fixture f;
    f.valley();
    f.state.camera.position = {50.0, 1.7, 32.0};
    f.audio.update(f.state.camera, 0.0);
    const std::string path =
        (std::filesystem::temp_directory_path() / ("giant_audio_test_" + std::to_string(std::random_device{}()) + ".wav")).string();
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
