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

GIANT_TEST("editor audio: a rifle shot past the camera cracks before its report; a pistol's only bangs; hits knock") {
    Fixture f(true);
    f.state.camera.position = {300.0, 1.7, 20.0};
    register_physics_commands(f.commands, f.state);
    connect_sounds(f.state);
    // Fired along +x from the origin, passing 20 m off the camera 300 m down range.
    ai::CommandResult r = f.run("fire", {{"origin", {0.0, 1.7, 0.0}}, {"direction", {1.0, 0.0, 0.0}}, {"resolve", true}});
    GIANT_CHECK(r.ok && r.value.contains("heard"));
    const Json& heard = r.value["heard"];
    GIANT_CHECK(heard["crack"]["heard"] == true);
    const double report = heard["report"]["taps"][0]["delay"].get<double>();
    GIANT_CHECK(std::fabs(report - std::hypot(300.0, 20.0) / audio::k_speed_of_sound) < 1e-6);
    GIANT_CHECK(heard["crack"]["delay"].get<double>() < report - 0.3);
    GIANT_CHECK(heard["crack"]["direction"][2].get<double>() < -0.5); // from the flight's side
    GIANT_CHECK(std::fabs(heard["report_loudness"].get<double>() - 100.0) < 3.0);
    GIANT_CHECK(f.audio.shots() == 1);
    // A subsonic pistol round (300 m/s): the bang, no crack.
    r = f.run("fire", {{"origin", {0.0, 1.7, 0.0}}, {"direction", {1.0, 0.0, 0.0}}, {"speed", 300.0}, {"resolve", true}});
    GIANT_CHECK(r.ok && r.value["heard"]["crack"]["heard"] == false);
    GIANT_CHECK(r.value["heard"]["report_loudness"].get<double>() < 50.0);
    // A wall in the way at 50 m: the flight is cut there, so no crack reaches the camera at 300 m.
    GIANT_CHECK(f.run("create_entity", {{"shape", "cube"}, {"position", {50.0, 2.0, 0.0}}, {"scale", {1.0, 4.0, 4.0}},
                                       {"body", "fixed"}, {"surface", "concrete"}}).ok);
    r = f.run("fire", {{"origin", {0.0, 1.7, 0.0}}, {"direction", {1.0, 0.0, 0.0}}, {"resolve", true}});
    GIANT_CHECK(r.ok && r.value["heard"]["crack"]["heard"] == false);
    GIANT_CHECK(std::fabs(r.value["heard"]["range"].get<double>() - 49.5) < 0.1);
    // Simulating, its hit on the wall knocks (heard).
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    const u64 before = f.audio.impacts();
    r = f.run("fire", {{"origin", {0.0, 1.7, 0.0}}, {"direction", {1.0, 0.0, 0.0}}, {"resolve", true}});
    GIANT_CHECK(r.ok && r.value["hits"].size() >= 1);
    GIANT_CHECK(f.audio.impacts() > before);
    r = f.run("get_audio");
    GIANT_CHECK(r.value["shots"] == 4 && r.value["impacts"].get<u64>() >= 1);
    // Flying (not resolved): a wall made this frame at 30 m is met by the aim, so the flight ends there.
    GIANT_CHECK(f.run("create_entity", {{"shape", "cube"}, {"position", {30.0, 2.0, 5.0}}, {"scale", {1.0, 4.0, 4.0}},
                                       {"body", "fixed"}, {"surface", "concrete"}}).ok);
    r = f.run("fire", {{"origin", {0.0, 1.7, 5.0}}, {"direction", {1.0, 0.0, 0.0}}});
    GIANT_CHECK(r.ok && std::fabs(r.value["heard"]["range"].get<double>() - 29.5) < 0.1);
    GIANT_CHECK(r.value["heard"]["crack"]["heard"] == false);
}

GIANT_TEST("editor audio: a resolved shot through a thin board still cracks past the camera") {
    Fixture f(true);
    f.state.camera.position = {300.0, 1.7, 20.0};
    register_physics_commands(f.commands, f.state);
    connect_sounds(f.state);
    // A 2 cm wooden board at 50 m: a rifle round goes through it, and on past the camera.
    GIANT_CHECK(f.run("create_entity", {{"shape", "cube"}, {"position", {50.0, 2.0, 0.0}}, {"scale", {0.02, 4.0, 4.0}},
                                       {"body", "fixed"}, {"surface", "wood"}}).ok);
    const ai::CommandResult r =
        f.run("fire", {{"origin", {0.0, 1.7, 0.0}}, {"direction", {1.0, 0.0, 0.0}}, {"resolve", true}});
    GIANT_CHECK(r.ok && r.value["hits"].size() >= 1);
    GIANT_CHECK(r.value["hits"][0]["outcome"] == "passed");
    GIANT_CHECK(r.value["heard"]["crack"]["heard"] == true);
    GIANT_CHECK(r.value["heard"]["range"].get<double>() > 300.0);
}

GIANT_TEST("editor audio: knocks and full shots are bounded a frame; a knock past 1 km is not made") {
    Fixture f;
    f.valley();
    f.state.camera.position = {60.0, 1.7, 0.0};
    f.audio.update(f.state.camera, 0.0);
    for (u32 k = 0; k < 20; ++k) f.audio.impact(f.state.scene, nullptr, {40.0, 0.0, 0.0}, 100.0f);
    GIANT_CHECK(f.audio.impacts() == AudioState::k_max_impacts);
    f.audio.update(f.state.camera, 1.0 / 60.0); // the next frame
    f.audio.impact(f.state.scene, nullptr, {40.0, 0.0, 0.0}, 100.0f);
    GIANT_CHECK(f.audio.impacts() == AudioState::k_max_impacts + 1);
    f.audio.impact(f.state.scene, nullptr, {2000.0, 0.0, 0.0}, 100.0f);
    GIANT_CHECK(f.audio.impacts() == AudioState::k_max_impacts + 1);
    // Shots: the first k_full_shots traced in full (echoes), the rest with the direct way only.
    f.audio.update(f.state.camera, 1.0 / 60.0);
    for (u32 k = 0; k < AudioState::k_full_shots + 2; ++k) {
        const AudioState::ShotHeard h =
            f.audio.shot(f.state.scene, nullptr, {0.0, 1.7, 0.0}, {300.0f, 0.0f, 0.0f}, 0.008f, 0.0f, 100.0);
        if (k < AudioState::k_full_shots) GIANT_CHECK(h.report.casts > 8);
        else GIANT_CHECK(h.report.casts <= 1 && h.report.taps.size() == 1);
    }
}

GIANT_TEST("editor audio: snow and fog take the highs sooner; snow on the ground deadens its echo") {
    Fixture f;
    f.valley();
    world::Weather w;
    GIANT_CHECK(f.audio.air() == 1.0f);
    w.fog_density = 1.0f;
    w.precipitation = 1.0f;
    w.temperature = -10.0f; // snow
    f.audio.set_weather(&w, world::Ground{0.0f, 1.0f});
    GIANT_CHECK(f.audio.air() > 3.5f && f.audio.snow_cover() == 1.0f);
    const audio::RayHit snowy = f.audio.ray_cast(f.state.scene, nullptr)({100.0, 50.0, 32.0}, {0, -1, 0}, 100.0);
    GIANT_CHECK(snowy.hit && snowy.hardness < 0.1f);
    f.state.camera.position = {50.0, 1.7, 32.0};
    ai::CommandResult r = f.run("play_sound", {{"at", {230.0, 1.7, 32.0}}, {"rays", 0}});
    const double dull = r.value["taps"][0]["lowpass"].get<double>();
    f.audio.set_weather(nullptr, world::Ground{});
    r = f.run("play_sound", {{"at", {230.0, 1.7, 32.0}}, {"rays", 0}});
    GIANT_CHECK(r.value["taps"][0]["lowpass"].get<double>() > dull * 2.0);
    GIANT_CHECK(f.run("get_audio").value["air"] == 1.0);
}

GIANT_TEST("editor audio: the water is heard where it is: a lake's shore, a river by its speed, under the surface muffled") {
    // A lake from x 10 to 60 (z -40 to 40) at 0 m; a river along z = 30 from x -60 to -10, 8 m wide, at 0.5 m.
    std::vector<world::Water> water(2);
    water[0].name = "lake";
    water[0].kind = world::WaterKind::lake;
    water[0].outline = {{10.0, -40.0}, {60.0, -40.0}, {60.0, 40.0}, {10.0, 40.0}};
    water[1].name = "river";
    water[1].kind = world::WaterKind::river;
    water[1].path = {{-60.0, 30.0, 8.0f, 0.5f}, {-10.0, 30.0, 8.0f, 0.5f}};
    water[1].flow = 4.0f;
    const WaterLookup at = [&water](double x, double z) { return world::water_at(water, x, z); };
    const GroundLookup flat = [](double, double) -> std::optional<double> { return -1.0; }; // the ground under it all
    const auto hear = [&](const DVec3& listener, const std::function<bool(double)>& reached = {}) {
        WaterHearing h;
        return h.update(at, flat, listener, 4.0f, reached, k_water_ways); // every way at once
    };
    // 4 m west of the lake's shore: its lapping from the east, the river (30 m north-west) quieter.
    const WaterHeard shore = hear({6.0, 1.7, 0.0});
    std::printf("    by the shore: lapping %.2f from (%.2f %.2f %.2f) spread %.2f; river %.2f\n", shore.shore,
                shore.shore_dir.x, shore.shore_dir.y, shore.shore_dir.z, shore.shore_spread, shore.river);
    GIANT_CHECK(shore.shore > 0.3f && shore.shore_dir.x > 0.6f && shore.sea == 0.0f && !shore.under);
    // Further off it fades; past the reach, nothing.
    const WaterHeard back = hear({-5.0, 1.7, -20.0});
    GIANT_CHECK(back.shore > 0.0f && back.shore < shore.shore * 0.6f);
    const WaterHeard far = hear({-200.0, 1.7, -200.0});
    GIANT_CHECK(far.shore == 0.0f && far.river == 0.0f && far.sea == 0.0f);
    // By the river: heard from it, louder the faster it runs.
    const WaterHeard river = hear({-35.0, 1.7, 20.0});
    water[1].flow = 1.0f;
    const WaterHeard slow = hear({-35.0, 1.7, 20.0});
    std::printf("    by the river: %.2f at 4 m/s, %.2f at 1 m/s (from z %.2f)\n", river.river, slow.river, river.river_dir.z);
    GIANT_CHECK(river.river > 0.3f && river.river_dir.z > 0.6f && slow.river < river.river * 0.3f);
    // Standing in the river by its end: the lake's shore 22 m east still heard past the river's bank.
    water[1].flow = 4.0f;
    const WaterHeard in_river = hear({-12.0, 1.0, 30.0});
    std::printf("    in the river: lapping %.2f from x %.2f, river %.2f\n", in_river.shore, in_river.shore_dir.x,
                in_river.river);
    GIANT_CHECK(in_river.shore > 0.05f && in_river.shore_dir.x > 0.5f && in_river.river > 0.5f);
    // An island: the ground over the water there is dry (no water heard under foot).
    const GroundLookup island = [](double x, double z) -> std::optional<double> {
        return std::fabs(x - 35.0) < 5.0 && std::fabs(z) < 5.0 ? 2.0 : -1.0;
    };
    WaterHearing on_island;
    const WaterHeard isle = on_island.update(at, island, {35.0, 3.7, 0.0}, 4.0f, {}, k_water_ways);
    GIANT_CHECK(isle.shore > 0.3f && isle.shore_spread > 0.7f && !isle.under);
    // Under the lake: under, its depth; not where the water does not reach (a room under it).
    const WaterHeard under = hear({35.0, -2.0, 0.0});
    GIANT_CHECK(under.under && std::fabs(under.depth - 2.0f) < 1e-4f);
    const WaterHeard dry = hear({35.0, -2.0, 0.0}, [](double) { return false; });
    GIANT_CHECK(!dry.under);
    // A few ways a call: a quarter of them looked along each time, the rest kept.
    WaterHearing slowly;
    const WaterHeard part = slowly.update(at, flat, {6.0, 1.7, 0.0}, 4.0f, {}, 4);
    WaterHeard whole = part;
    for (int i = 0; i < 3; ++i) whole = slowly.update(at, flat, {6.0, 1.7, 0.0}, 4.0f, {}, 4);
    GIANT_CHECK(std::fabs(whole.shore - shore.shore) < 1e-5f && part.shore <= whole.shore);
    // Into the mixer: the lapping played round and round; under the surface, all of it muffled.
    AudioState audio;
    audio.set_water(shore);
    EditorCamera camera;
    for (int i = 0; i < 8; ++i) audio.update(camera, 0.1); // (no speakers: rendered and dropped)
    GIANT_CHECK(audio.mixer().stats().ambience == 2 && audio.mixer().stats().muffle > 19000.0f); // (the lake, the river)
    audio.set_water(under);
    for (int i = 0; i < 8; ++i) audio.update(camera, 0.1);
    std::printf("    under: %u ambience, muffle %.0f Hz\n", audio.mixer().stats().ambience, audio.mixer().stats().muffle);
    GIANT_CHECK(audio.mixer().stats().muffle < AudioState::k_under_muffle * 1.05f && audio.mixer().stats().ambience >= 1);
    // get_audio tells it.
    Fixture f;
    f.audio.set_water(under);
    for (int i = 0; i < 4; ++i) f.audio.update(f.state.camera, 0.1);
    const ai::CommandResult r = f.run("get_audio");
    GIANT_CHECK(r.ok && r.value["water"]["under"] == true && std::fabs(r.value["water"]["depth"].get<float>() - 2.0f) < 1e-4f);
    GIANT_CHECK(r.value["ambience"].get<u32>() >= 1 && r.value["muffle"].get<float>() < 1000.0f);
}
