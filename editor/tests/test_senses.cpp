// What the people hear (people_senses.h): noises spread at the speed of
// sound, each person in reach hears each once, alarm rises with how loud it
// is at one's ears and eases back; shots, strikes, blasts and splashes make
// their own.
#include "../src/audio_state.h"
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"
#include "../src/people_senses.h"
#include "../src/people_travel.h"
#include "../src/physics_state.h"

#include "giant_test.h"

#include "giant/world/people.h"

#include <cmath>
#include <string>

using namespace giant;
using namespace giant::editor;
using ai::Json;

namespace {

Json at(double x, double y, double z) { return Json::array({x, y, z}); }

struct Fixture {
    ai::CommandRegistry commands;
    MeshLibrary meshes{nullptr};
    EditorState state;
    PeopleTravel travel;
    PeopleSenses senses;
    double seconds = 0.0;
    u64 run_id = 1;
    Fixture() {
        state.meshes = &meshes;
        state.travel = &travel;
        state.senses = &senses;
        register_scene_commands(commands, state);
        register_people_commands(commands, state);
        register_route_commands(commands, state);
        world::Entity e;
        e.shape = world::Shape::mesh;
        e.mesh = "people/villager.glb";
        const world::EntityId model = state.scene.add(e);
        GIANT_CHECK(run("set_person_look", {{"name", "villager"}, {"entity", model}}).ok);
        GIANT_CHECK(run("delete_entities", {{"ids", {model}}}).ok);
    }
    ai::CommandResult run(const char* name, const Json& args = Json::object()) { return commands.execute(name, args, {}); }
    world::PersonId person(double x, double z) {
        const ai::CommandResult r = run("create_people", {{"people", {{{"look", "villager"}, {"position", at(x, 0, z)}}}}});
        GIANT_CHECK(r.ok);
        return r.ok ? r.value["ids"][0].get<world::PersonId>() : 0;
    }
    // `more` seconds simulated over `count` frames.
    void frames(double more = 0.0, int count = 4) {
        for (int i = 0; i < count; ++i) {
            seconds += more / count;
            travel.update(state.scene, run_id, seconds);
            senses.update(state.scene, travel);
        }
    }
    float alarm(world::PersonId id) { return senses.alarm(state.scene, id); }
    Json told(world::PersonId id) { return run("get_people", {{"ids", {id}}}).value["people"][0]; }
};

} // namespace

GIANT_TEST("senses: a noise spreads at the speed of sound; each in reach hears it once, as loud as it is there") {
    Fixture f;
    const world::PersonId near_by = f.person(10, 0), mid = f.person(343, 0), far_off = f.person(600, 0);
    // Not simulating: no noise is made.
    f.run_id = 0;
    f.frames();
    GIANT_CHECK(!f.senses.noise({0, 0, 0}, 100.0f, PeopleSenses::Kind::shot));
    f.run_id = 1;
    f.frames();
    // A rifle's report (100 at 1 m): heard to 500 m (0.2 there).
    GIANT_CHECK(f.senses.noise({0, 0, 0}, 100.0f, PeopleSenses::Kind::shot) && f.senses.stats().noises == 1);
    f.frames(0.1); // 34 m on: the one at 10 m has heard it, the one a second off has not
    PeopleSenses::Heard h;
    GIANT_CHECK(f.senses.heard(f.state.scene, near_by, h) && h.kind == PeopleSenses::Kind::shot && h.from == DVec3(0, 0, 0));
    GIANT_CHECK(std::fabs(h.level - 10.0f) < 1e-4f && h.alarm > 0.99f); // (10 at its ears: over what alarms fully)
    GIANT_CHECK(f.alarm(mid) == 0.0f && f.alarm(far_off) == 0.0f && f.senses.stats().heard == 1);
    f.frames(0.8);
    GIANT_CHECK(f.alarm(mid) == 0.0f); // 0.9 s: 309 m
    f.frames(0.2);
    // 1.1 s: it is there, at 100 / 343 = 0.29, a seventh of what alarms fully.
    GIANT_CHECK(f.senses.heard(f.state.scene, mid, h) && std::fabs(h.level - 100.0f / 343.0f) < 1e-4f);
    GIANT_CHECK(std::fabs(h.alarm - (100.0f / 343.0f) / 2.0f) < 0.02f && f.senses.stats().heard == 2);
    // Past its reach nobody hears it, and once it is through it is gone; nobody hears it twice.
    f.frames(1.0);
    f.frames(1.0);
    GIANT_CHECK(f.alarm(far_off) == 0.0f && f.senses.stats().noises == 0 && f.senses.stats().heard == 2);
    // get_people tells it.
    Json p = f.told(near_by);
    GIANT_CHECK(p["alarm"].get<double>() > 0.8 && p["heard"]["kind"] == "shot" && p["heard"]["from"] == at(0, 0, 0));
    GIANT_CHECK(f.told(far_off)["alarm"] == 0.0 && f.told(far_off)["heard"].is_null());
    // Alarm eases to nothing over calm_seconds (30): half gone after 15 more, all after 30.
    const float before = f.alarm(near_by);
    f.frames(15.0);
    GIANT_CHECK(std::fabs(f.alarm(near_by) - (before - 0.5f)) < 0.01f);
    f.frames(20.0);
    GIANT_CHECK(f.alarm(near_by) == 0.0f && !f.senses.heard(f.state.scene, near_by, h) && f.told(near_by)["heard"].is_null());
    // The simulation stopped: everyone is calm, nothing spreads.
    GIANT_CHECK(f.senses.noise({0, 0, 0}, 100.0f, PeopleSenses::Kind::shot));
    f.frames(0.1);
    GIANT_CHECK(f.alarm(near_by) > 0.9f);
    f.run_id = 0;
    f.frames();
    GIANT_CHECK(f.alarm(near_by) == 0.0f && f.senses.stats().noises == 0);
}

GIANT_TEST("senses: every kind's word is one a behaviour's `sensed` condition takes, and no other is") {
    usize kinds = 0;
    for (const PeopleSenses::Kind kind : {PeopleSenses::Kind::shot, PeopleSenses::Kind::impact, PeopleSenses::Kind::blast, PeopleSenses::Kind::splash,
                                          PeopleSenses::Kind::voice, PeopleSenses::Kind::other, PeopleSenses::Kind::sight}) {
        GIANT_CHECK(world::sensed_kind_ok(PeopleSenses::to_string(kind)));
        ++kinds;
    }
    // (A kind added to the senses must be added here and to world::sensed_kind_ok.)
    PeopleSenses::Kind out = PeopleSenses::Kind::other;
    usize known = 0;
    for (const char* word : {"shot", "impact", "blast", "splash", "voice", "other", "sight", "smell", "touch", "noise", "bang"}) {
        GIANT_CHECK(PeopleSenses::from_string(word, out) == world::sensed_kind_ok(word));
        if (world::sensed_kind_ok(word)) ++known;
    }
    GIANT_CHECK(known == kinds);
}

GIANT_TEST("senses: noises add up, the loudest is remembered; settings and make_noise by command") {
    Fixture f;
    const world::PersonId a = f.person(100, 0);
    f.frames();
    // By command: not while stopped; made, it says how far it reaches.
    f.run_id = 0;
    f.frames();
    GIANT_CHECK(!f.run("make_noise", {{"at", at(0, 0, 0)}, {"loudness", 50.0}}).ok);
    f.run_id = 1;
    f.frames();
    ai::CommandResult r = f.run("make_noise", {{"at", at(0, 0, 0)}, {"loudness", 50.0}, {"kind", "voice"}});
    GIANT_CHECK(r.ok && r.value["kind"] == "voice" && std::fabs(r.value["reach"].get<double>() - 250.0) < 1e-3 && r.value["noises"] == 1);
    GIANT_CHECK(!f.run("make_noise", {{"at", at(0, 0, 0)}, {"loudness", 0.1}}).ok); // too quiet for anyone
    GIANT_CHECK(!f.run("make_noise", {{"at", at(0, 0, 0)}, {"loudness", 50.0}, {"kind", "bang"}}).ok);
    f.frames(0.5);
    // 0.5 at its ears: a quarter of what alarms fully.
    GIANT_CHECK(std::fabs(f.alarm(a) - 0.25f) < 0.02f);
    // A second, quieter one from elsewhere takes its share of the calm left; the louder is what it remembers.
    GIANT_CHECK(f.run("make_noise", {{"at", at(100, 0, 100)}, {"loudness", 30.0}, {"kind", "impact"}}).ok);
    f.frames(0.5);
    PeopleSenses::Heard h;
    GIANT_CHECK(f.senses.heard(f.state.scene, a, h) && h.kind == PeopleSenses::Kind::voice && h.from == DVec3(0, 0, 0));
    GIANT_CHECK(h.alarm > 0.30f && h.alarm < 0.37f); // 1 - (1 - 0.23)(1 - 0.15)
    // A louder one is remembered in its place.
    GIANT_CHECK(f.run("make_noise", {{"at", at(100, 0, 50)}, {"loudness", 200.0}, {"kind", "blast"}}).ok);
    f.frames(0.5);
    GIANT_CHECK(f.senses.heard(f.state.scene, a, h) && h.kind == PeopleSenses::Kind::blast && h.alarm > 0.99f);
    // Settings: read, set, refused out of range; off, nothing is made and nothing spreads.
    r = f.run("set_sense_settings");
    GIANT_CHECK(r.ok && r.value["on"] == true && r.value["made"] == 3 && r.value["heard"] == 3 && r.value["calm_seconds"] == 30.0);
    r = f.run("set_sense_settings", {{"hear_loudness", 1.0}, {"alarm_loudness", 4.0}, {"calm_seconds", 10.0}, {"looked_a_frame", 8}});
    GIANT_CHECK(r.ok && f.senses.settings.hear_loudness == 1.0f && f.senses.settings.alarm_loudness == 4.0f &&
                f.senses.settings.calm_seconds == 10.0f && f.senses.settings.looked_a_frame == 8);
    GIANT_CHECK(!f.run("set_sense_settings", {{"alarm_loudness", 0.5}}).ok && !f.run("set_sense_settings", {{"calm_seconds", 0.0}}).ok);
    GIANT_CHECK(f.run("make_noise", {{"at", at(0, 0, 0)}, {"loudness", 50.0}}).value["reach"] == 50.0);
    GIANT_CHECK(f.run("set_sense_settings", {{"on", false}}).ok && !f.run("make_noise", {{"at", at(0, 0, 0)}, {"loudness", 50.0}}).ok);
    f.frames(0.1);
    GIANT_CHECK(f.senses.stats().noises == 0);
}

GIANT_TEST("senses: thousands are gone through a share a frame and each hears once; more noises than are kept, the quietest give way") {
    Fixture f;
    Json batch = Json::array();
    for (int i = 0; i < 3000; ++i) batch.push_back({{"look", "villager"}, {"position", at(10.0 + (i % 100), 0, i / 100)}});
    GIANT_CHECK(f.run("create_people", {{"people", batch}}).ok);
    f.senses.settings.looked_a_frame = 500;
    f.frames();
    GIANT_CHECK(f.senses.noise({0, 0, 0}, 100.0f, PeopleSenses::Kind::blast));
    // All within 115 m: reached within a third of a second, heard as the list is gone through (6
    // frames a time), the first time through beginning with the first frame after it was made.
    f.frames(0.5, 1);
    GIANT_CHECK(f.senses.stats().heard == 500);
    f.frames(0.0, 5);
    GIANT_CHECK(f.senses.stats().heard == 3000);
    f.frames(1.0, 20);
    GIANT_CHECK(f.senses.stats().heard == 3000);
    f.frames(2.0, 40);
    GIANT_CHECK(f.senses.stats().heard == 3000 && f.senses.stats().noises == 0);
    for (const world::PersonId id : {world::PersonId{1}, world::PersonId{1500}, world::PersonId{3000}}) GIANT_CHECK(f.alarm(id) > 0.2f);
    // People removed and made while one spreads (those removed had heard it, the one made is in
    // its reach): everyone hears it once, the one looked at next staying the same person.
    GIANT_CHECK(f.senses.noise({0, 0, 0}, 100.0f, PeopleSenses::Kind::blast));
    f.frames(0.2, 2);
    GIANT_CHECK(f.run("remove_people", {{"ids", {5, 6, 7}}}).ok);
    f.person(20, 5);
    f.frames(2.0, 40);
    GIANT_CHECK(f.senses.stats().heard == 3000 + 3001);
    // 80 at once, louder each time: 64 are kept, the 16 quietest gave way.
    for (int i = 0; i < 80; ++i) GIANT_CHECK(f.senses.noise({0, 0, 0}, 10.0f + static_cast<float>(i), PeopleSenses::Kind::other));
    GIANT_CHECK(f.senses.stats().noises == 64 && f.senses.stats().dropped == 16);
    // A quieter one than all kept is refused.
    GIANT_CHECK(!f.senses.noise({0, 0, 0}, 5.0f, PeopleSenses::Kind::other) && f.senses.stats().dropped == 17);
    // Not numbers: refused.
    GIANT_CHECK(!f.senses.noise({std::nan(""), 0, 0}, 50.0f, PeopleSenses::Kind::other));
    GIANT_CHECK(!f.senses.noise({0, 0, 0}, std::nanf(""), PeopleSenses::Kind::other));
}

GIANT_TEST("senses: a shot's report, the bullet striking, and a blast are noises the people hear") {
    Fixture f;
    PhysicsState physics;
    f.state.physics = &physics;
    register_physics_commands(f.commands, f.state);
    connect_senses(f.state);
    connect_senses(f.state); // (twice is once)
    const world::PersonId by_gun = f.person(0, 30), by_wall = f.person(200, 20), away = f.person(0, 5000);
    GIANT_CHECK(f.run("create_entity", {{"shape", "cube"}, {"position", {200.0, 2.0, 0.0}}, {"scale", {1.0, 4.0, 4.0}},
                                       {"body", "fixed"}, {"surface", "concrete"}}).ok);
    const auto frames = [&](double more, int count = 4) {
        for (int i = 0; i < count; ++i) {
            if (more > 0.0) GIANT_CHECK(f.run("step_physics", {{"seconds", more / count}}).ok);
            const PeopleTravel::Clock now = PeopleTravel::clock_of(physics);
            f.travel.update(f.state.scene, now.run, now.seconds);
            f.senses.update(f.state.scene, f.travel);
        }
    };
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    // (No frame since the simulation started: the noises of a run's first frame are made all the same.)
    // A rifle fired along +x into a wall 200 m off: its report at the muzzle, its strike at the wall.
    ai::CommandResult r = f.run("fire", {{"origin", {0.0, 1.7, 0.0}}, {"direction", {1.0, 0.0, 0.0}}, {"resolve", true}});
    GIANT_CHECK(r.ok && r.value["hits"].size() >= 1);
    GIANT_CHECK(f.senses.stats().made == 2);
    frames(0.5);
    PeopleSenses::Heard h;
    GIANT_CHECK(f.senses.heard(f.state.scene, by_gun, h) && h.kind == PeopleSenses::Kind::shot && h.alarm > 0.9f);
    GIANT_CHECK(h.level > 2.0f && h.from == DVec3(0.0, 1.7, 0.0)); // (about 100 at 1 m, 30 m off)
    // The one by the wall hears the strike first (20 m off), then the report from 200 m: the louder at its ears is remembered.
    GIANT_CHECK(f.senses.heard(f.state.scene, by_wall, h) && h.kind == PeopleSenses::Kind::impact);
    frames(0.5);
    GIANT_CHECK(f.senses.heard(f.state.scene, by_wall, h) && f.senses.stats().heard >= 3);
    GIANT_CHECK(f.senses.alarm(f.state.scene, away) == 0.0f);
    // A blast by the one far away: it hears that.
    GIANT_CHECK(f.run("impact", {{"point", {10.0, 0.0, 5000.0}}, {"radius", 5.0}, {"impulse", 400.0}}).ok);
    frames(0.2);
    GIANT_CHECK(f.senses.heard(f.state.scene, away, h) && h.kind == PeopleSenses::Kind::blast && h.from == DVec3(10.0, 0.0, 5000.0));
    GIANT_CHECK(PeopleSenses::blast_loudness(100.0f) == 100.0f && PeopleSenses::blast_loudness(0.0f) == 0.0f);
    GIANT_CHECK(AudioState::impact_loudness(0.0f) == 0.0f && AudioState::shot_loudness(0.0f, 100.0f) == 0.0f);
    // Sight through the physics: the wall hides what is behind it from one side, not from the other.
    const auto one = [&](double x, double yaw) {
        const ai::CommandResult made = f.run("create_people", {{"people", {{{"look", "villager"}, {"position", at(x, 0, 0)}, {"yaw", yaw}}}}});
        GIANT_CHECK(made.ok);
        return made.ok ? made.value["ids"][0].get<world::PersonId>() : world::PersonId{0};
    };
    const world::PersonId before_wall = one(190, 90), behind_wall = one(215, -90);
    frames(60.0, 8); // (everyone calm again)
    GIANT_CHECK(f.senses.alarm(f.state.scene, by_gun) == 0.0f);
    GIANT_CHECK(f.run("show_to_people", {{"name", "crate"}, {"at", at(205, 1.6, 0)}}).ok);
    frames(0.1);
    GIANT_CHECK(f.senses.alarm(f.state.scene, before_wall) == 0.0f);
    GIANT_CHECK(f.senses.heard(f.state.scene, behind_wall, h) && h.kind == PeopleSenses::Kind::sight && h.from == DVec3(205, 1.6, 0));
    GIANT_CHECK(f.senses.stats().rays >= 2);
    // Moved over the wall's top, both see it.
    GIANT_CHECK(f.run("show_to_people", {{"name", "crate"}, {"at", at(205, 8.0, 0)}}).ok);
    frames(0.1);
    GIANT_CHECK(f.senses.heard(f.state.scene, before_wall, h) && h.kind == PeopleSenses::Kind::sight);
}

GIANT_TEST("senses: a person sees what is shown within its distance and the way it faces; alarm holds while it is in sight") {
    Fixture f;
    const auto one = [&](double x, double z, double yaw) {
        const ai::CommandResult made = f.run("create_people", {{"people", {{{"look", "villager"}, {"position", at(x, 0, z)}, {"yaw", yaw}}}}});
        GIANT_CHECK(made.ok);
        return made.ok ? made.value["ids"][0].get<world::PersonId>() : world::PersonId{0};
    };
    // The thing at (0, 1.6, 20): one faces it from 20 m, one has it 20 m behind, one is too far, one
    // has it behind but within arm's reach, one faces it from the side (90 degrees off: out of 120 across).
    const world::PersonId facing = one(0, 0, 0), turned = one(0, 40, 0), too_far = one(0, -100, 0), touching = one(0, 21, 0),
                          side = one(20, 20, 0);
    f.run_id = 0;
    f.frames();
    GIANT_CHECK(!f.run("show_to_people", {{"name", "thing"}, {"at", at(0, 1.6, 20)}}).ok); // not simulating
    f.run_id = 1;
    f.frames();
    ai::CommandResult r = f.run("show_to_people", {{"name", "thing"}, {"at", at(0, 1.6, 20)}});
    GIANT_CHECK(r.ok && r.value["shown"] == 1);
    f.frames(0.1);
    PeopleSenses::Heard h;
    GIANT_CHECK(f.senses.heard(f.state.scene, facing, h) && h.kind == PeopleSenses::Kind::sight && h.from == DVec3(0, 1.6, 20));
    GIANT_CHECK(std::fabs(h.alarm - 0.85f) < 0.01f); // 1.125 (1 - 20 / 60) + 0.1
    GIANT_CHECK(f.alarm(turned) == 0.0f && f.alarm(too_far) == 0.0f && f.alarm(side) == 0.0f && f.alarm(touching) > 0.99f);
    GIANT_CHECK(f.told(facing)["heard"]["kind"] == "sight" && f.told(turned)["heard"].is_null());
    // In sight it stays as alarmed, however seldom each is looked through (one a frame here, five
    // people); hidden, it eases as after a noise (half of calm_seconds: 0.5 gone).
    f.senses.settings.seen_a_frame = 1;
    for (int i = 0; i < 40; ++i) {
        f.frames(0.5, 1);
        GIANT_CHECK(f.alarm(facing) == h.alarm);
    }
    f.senses.settings.seen_a_frame = 1024;
    f.frames(0.1);
    GIANT_CHECK(std::fabs(f.alarm(facing) - 0.85f) < 0.01f && f.senses.heard(f.state.scene, facing, h) && h.seconds < 1.0);
    GIANT_CHECK(f.run("hide_from_people", {{"name", "thing"}}).ok && !f.run("hide_from_people", {{"name", "thing"}}).ok);
    f.frames(15.0, 30);
    GIANT_CHECK(std::fabs(f.alarm(facing) - 0.36f) < 0.04f);
    f.frames(30.0, 30);
    GIANT_CHECK(f.alarm(facing) == 0.0f);
    // Shown again by the same name it moves (still one); turned about, the one it was behind sees it; a wider look takes the side in.
    GIANT_CHECK(f.run("show_to_people", {{"name", "thing"}, {"at", at(0, 1.6, 20)}}).ok);
    r = f.run("show_to_people", {{"name", "thing"}, {"at", at(0, 1.6, 60)}});
    GIANT_CHECK(r.ok && r.value["shown"] == 1);
    f.frames(0.1);
    GIANT_CHECK(f.alarm(turned) > 0.5f);
    GIANT_CHECK(f.run("show_to_people", {{"name", "thing"}, {"at", at(0, 1.6, 20)}}).ok);
    f.frames(60.0, 30);
    GIANT_CHECK(f.alarm(side) == 0.0f && f.alarm(turned) == 0.0f && f.alarm(facing) > 0.8f);
    r = f.run("set_sense_settings", {{"see_angle", 200.0}, {"see_distance", 80.0}});
    GIANT_CHECK(r.ok && r.value["see_angle"] == 200.0 && r.value["watched"].size() == 1 && r.value["watched"][0]["name"] == "thing");
    f.frames(0.1);
    GIANT_CHECK(f.alarm(side) > 0.5f);
    GIANT_CHECK(!f.run("set_sense_settings", {{"see_angle", 5.0}}).ok && !f.run("set_sense_settings", {{"rays_a_frame", 0}}).ok);
    // Sight off: nobody sees; what they saw eases.
    GIANT_CHECK(f.run("set_sense_settings", {{"sight", false}}).ok);
    const u64 seen = f.senses.stats().seen;
    f.frames(1.0);
    GIANT_CHECK(f.senses.stats().seen == seen);
    GIANT_CHECK(f.run("set_sense_settings", {{"sight", true}}).ok);
    // Something between (here: anything crossing z = 10): the rays are cast a few a frame, each person looked through in turn.
    f.frames(60.0, 30);
    f.senses.blocked = [](const DVec3& from, const DVec3& to) { return (from.z - 10.0) * (to.z - 10.0) < 0.0; };
    f.senses.settings.rays_a_frame = 1;
    const u64 rays = f.senses.stats().rays;
    f.frames(0.5, 5);
    GIANT_CHECK(f.senses.stats().rays == rays + 5); // (one a frame, however many would look)
    f.senses.settings.rays_a_frame = 32;
    f.frames(60.0, 30);
    f.frames(0.1);
    GIANT_CHECK(f.alarm(facing) == 0.0f && f.alarm(touching) > 0.99f && f.alarm(side) > 0.5f); // (the one at z = 0 has it hidden)
    // 16 are shown at most; the simulation stopped, all are forgotten.
    for (int i = 0; i < 15; ++i) GIANT_CHECK(f.run("show_to_people", {{"name", "more " + std::to_string(i)}, {"at", at(1000, 0, 1000)}}).ok);
    GIANT_CHECK(!f.run("show_to_people", {{"name", "one too many"}, {"at", at(1000, 0, 1000)}}).ok);
    f.run_id = 0;
    f.frames();
    GIANT_CHECK(f.senses.watched().empty() && f.alarm(touching) == 0.0f);
}

GIANT_TEST("senses: the player has its own place; up and down; rays only where they matter; a thing's own body; eyes on the ground") {
    Fixture f;
    const auto one = [&](double x, double z, double yaw) {
        const ai::CommandResult made = f.run("create_people", {{"people", {{{"look", "villager"}, {"position", at(x, 0, z)}, {"yaw", yaw}}}}});
        GIANT_CHECK(made.ok);
        return made.ok ? made.value["ids"][0].get<world::PersonId>() : world::PersonId{0};
    };
    const world::PersonId a = one(0, 0, 0);
    f.frames();
    // The player is seen without being shown, and with as many shown as can be; its name is its own.
    for (int i = 0; i < 16; ++i) GIANT_CHECK(f.run("show_to_people", {{"name", "far " + std::to_string(i)}, {"at", at(5000, 0, 5000)}}).ok);
    GIANT_CHECK(!f.run("show_to_people", {{"name", "player"}, {"at", at(0, 1.6, 10)}}).ok);
    f.senses.player = DVec3{0, 1.6, 30};
    f.frames(0.1);
    PeopleSenses::Heard h;
    GIANT_CHECK(f.senses.heard(f.state.scene, a, h) && h.kind == PeopleSenses::Kind::sight && h.from == DVec3(0, 1.6, 30));
    ai::CommandResult r = f.run("set_sense_settings");
    GIANT_CHECK(r.ok && r.value["watched"].size() == 17 && r.value["watched"][0]["name"] == "player");
    // Dead ahead but 45 degrees up: within 120 across, not within 60.
    f.senses.player = DVec3{0, 21.6, 20};
    f.frames(0.1);
    GIANT_CHECK(f.senses.heard(f.state.scene, a, h) && h.from == DVec3(0, 21.6, 20));
    GIANT_CHECK(f.run("set_sense_settings", {{"see_angle", 60.0}}).ok);
    f.frames(0.1);
    f.frames(40.0, 20);
    GIANT_CHECK(f.alarm(a) == 0.0f);
    GIANT_CHECK(f.run("set_sense_settings", {{"see_angle", 120.0}}).ok);
    f.senses.player.reset();
    // The senses off: what is shown is forgotten.
    r = f.run("set_sense_settings", {{"on", false}});
    GIANT_CHECK(r.ok && r.value["watched"].empty() && f.senses.watched().empty());
    GIANT_CHECK(f.run("set_sense_settings", {{"on", true}}).ok);
    f.frames(0.1);
    // Two things ahead, the farther shown first, one ray a frame: the look goes on where the rays ran
    // out, and the person is as alarmed as the nearer makes it once both are looked at.
    u64 casts = 0;
    f.senses.blocked = [&](const DVec3&, const DVec3&) {
        ++casts;
        return false;
    };
    f.senses.settings.rays_a_frame = 1;
    GIANT_CHECK(f.run("show_to_people", {{"name", "farther"}, {"at", at(0, 1.6, 40)}}).ok);
    GIANT_CHECK(f.run("show_to_people", {{"name", "nearer"}, {"at", at(0, 1.6, 20)}}).ok);
    f.frames(0.1, 1);
    GIANT_CHECK(casts == 1 && f.alarm(a) == 0.0f);
    f.frames(0.1, 1);
    GIANT_CHECK(casts == 2 && f.senses.heard(f.state.scene, a, h) && h.from == DVec3(0, 1.6, 20) && std::fabs(h.alarm - 0.85f) < 0.01f);
    // The nearer first: the farther could change nothing, so no ray is spent on it.
    GIANT_CHECK(f.run("hide_from_people", {{"name", "farther"}}).ok && f.run("show_to_people", {{"name", "farther"}, {"at", at(0, 1.6, 40)}}).ok);
    casts = 0;
    f.frames(0.1, 1);
    GIANT_CHECK(casts == 1);
    // More alarmed by a noise than seeing makes it: the noise is what it remembers; holding what it
    // sees it is still looked through (one ray), and one that held nothing has no ray spent on it.
    GIANT_CHECK(f.senses.noise({0, 0, 1}, 100.0f, PeopleSenses::Kind::blast));
    f.frames(0.1, 2);
    GIANT_CHECK(f.senses.heard(f.state.scene, a, h) && h.kind == PeopleSenses::Kind::blast && h.alarm > 0.99f);
    casts = 0;
    f.frames(0.1, 1);
    GIANT_CHECK(casts == 1);
    GIANT_CHECK(f.run("hide_from_people", {{"name", "farther"}}).ok && f.run("hide_from_people", {{"name", "nearer"}}).ok);
    f.frames(0.1, 1); // (it lets go of what it saw)
    GIANT_CHECK(f.run("show_to_people", {{"name", "nearer"}, {"at", at(0, 1.6, 20)}}).ok);
    casts = 0;
    f.frames(0.1, 1);
    GIANT_CHECK(casts == 0 && f.alarm(a) > 0.9f);
    GIANT_CHECK(f.run("hide_from_people", {{"name", "nearer"}}).ok);
    f.frames(60.0, 20);
    GIANT_CHECK(f.alarm(a) == 0.0f);
    // Seen near, then only something far: the alarm eases from where it was, not at once.
    f.senses.blocked = nullptr;
    f.senses.player = DVec3{0, 1.6, 5};
    GIANT_CHECK(f.run("show_to_people", {{"name", "far thing"}, {"at", at(0, 1.6, 55)}}).ok);
    f.frames(0.1);
    GIANT_CHECK(f.alarm(a) > 0.99f);
    f.senses.player.reset();
    f.frames(0.3, 3);
    GIANT_CHECK(f.alarm(a) > 0.95f);
    f.frames(15.0, 30);
    GIANT_CHECK(std::fabs(f.alarm(a) - 0.5f) < 0.05f);
    f.frames(30.0, 30);
    GIANT_CHECK(f.senses.heard(f.state.scene, a, h) && std::fabs(h.alarm - 0.19375f) < 0.01f && h.kind == PeopleSenses::Kind::sight);
    // A quiet noise heard while it sees does not take the sight's place, and it goes on seeing.
    const u64 seen_before = f.senses.stats().seen;
    GIANT_CHECK(f.senses.noise({0, 0, 3}, 1.0f, PeopleSenses::Kind::voice)); // (0.33 at its ears: a sixth of full)
    f.frames(0.2, 4);
    GIANT_CHECK(f.senses.heard(f.state.scene, a, h) && h.kind == PeopleSenses::Kind::sight && h.alarm > 0.3f && h.seconds < 0.11);
    GIANT_CHECK(f.senses.stats().seen == seen_before + 4);
    GIANT_CHECK(f.run("hide_from_people", {{"name", "far thing"}}).ok);
    f.frames(60.0, 20);
    GIANT_CHECK(f.alarm(a) == 0.0f);
    // A thing whose own body stands in the way within a metre of its middle: hidden by itself, but for its clearance.
    f.senses.settings.rays_a_frame = 32;
    f.senses.blocked = [](const DVec3&, const DVec3& to) { return giant::length(to - DVec3{0, 1.6, 20}) < 1.0; };
    GIANT_CHECK(f.run("show_to_people", {{"name", "crate"}, {"at", at(0, 1.6, 20)}}).ok);
    f.frames(0.1);
    GIANT_CHECK(f.alarm(a) == 0.0f);
    GIANT_CHECK(f.run("show_to_people", {{"name", "crate"}, {"at", at(0, 1.6, 20)}, {"clearance", 1.5}}).ok);
    f.frames(0.1);
    GIANT_CHECK(f.alarm(a) > 0.8f);
    GIANT_CHECK(f.run("hide_from_people", {{"name", "crate"}}).ok);
    // On terrain a person's eyes are over the ground where it stands, whatever height it was made at.
    world::Terrain& terrain = f.state.scene.terrain;
    world::Terrain::Heights heights(static_cast<usize>(terrain.samples()) * terrain.samples(), 20.0f);
    terrain.set({0, 0}, std::move(heights));
    const world::PersonId on_ground = one(30, 30, 0);
    f.senses.blocked = [&](const DVec3& from, const DVec3& to) {
        const DVec3 way = to - from;
        const double far_off = giant::length(way);
        const DVec3 unit = way / far_off;
        return march_terrain(terrain, from, {static_cast<float>(unit.x), static_cast<float>(unit.y), static_cast<float>(unit.z)}, far_off).has_value();
    };
    GIANT_CHECK(f.run("show_to_people", {{"name", "on the ground"}, {"at", at(30, 21.6, 50)}}).ok);
    f.frames(0.1);
    GIANT_CHECK(f.senses.heard(f.state.scene, on_ground, h) && h.kind == PeopleSenses::Kind::sight);
}

GIANT_TEST("senses: what each heard stays its own when people are removed; a noise made part way through the list; none left to hear") {
    Fixture f;
    Json batch = Json::array();
    for (int i = 0; i < 10; ++i) batch.push_back({{"look", "villager"}, {"position", at(10.0 + i, 0, 0)}});
    ai::CommandResult made = f.run("create_people", {{"people", batch}});
    GIANT_CHECK(made.ok);
    const world::PersonId first = made.value["ids"][0].get<world::PersonId>(), last = made.value["ids"][9].get<world::PersonId>();
    f.senses.settings.looked_a_frame = 4;
    f.frames();
    // One noise, and another made when the first has been heard by four of the ten: each hears each once.
    GIANT_CHECK(f.senses.noise({0, 0, 0}, 20.0f, PeopleSenses::Kind::voice));
    f.frames(0.1, 1);
    GIANT_CHECK(f.senses.stats().heard == 4);
    GIANT_CHECK(f.run("make_noise", {{"at", at(0, 0, 0)}, {"loudness", 4.0}, {"kind", "impact"}}).ok);
    f.frames(2.0, 40);
    GIANT_CHECK(f.senses.stats().heard == 20 && f.senses.stats().noises == 0);
    // The quieter, later one did not take the louder's place, nor its time: `seconds` is since the one remembered.
    PeopleSenses::Heard h;
    GIANT_CHECK(f.senses.heard(f.state.scene, last, h) && h.kind == PeopleSenses::Kind::voice && h.seconds > 1.85 && h.seconds < 2.15);
    const float before = h.alarm;
    // While it is quiet the first is removed: the others keep what they heard, told at once.
    GIANT_CHECK(f.run("remove_people", {{"ids", {first}}}).ok);
    GIANT_CHECK(f.senses.heard(f.state.scene, last, h) && h.alarm == before && !f.senses.heard(f.state.scene, first, h));
    GIANT_CHECK(f.told(last)["alarm"].get<double>() > 0.0 && f.told(last)["heard"]["kind"] == "voice");
    f.frames(0.1);
    GIANT_CHECK(f.alarm(last) > 0.0f && f.alarm(last) < before);
    // One made just where a person stands is heard by it (the next time through), once.
    const u64 heard_before = f.senses.stats().heard;
    GIANT_CHECK(f.run("make_noise", {{"at", at(19, 0, 0)}, {"loudness", 0.5}}).ok); // (reaches 2.5 m: the last and the two by it)
    f.frames(0.5, 10);
    GIANT_CHECK(f.senses.stats().heard == heard_before + 3);
    GIANT_CHECK(!f.run("make_noise", {{"at", at(1e9, 0, 0)}, {"loudness", 5.0}}).ok);
    // Nobody left: a noise spreads its reach and goes all the same.
    Json ids = Json::array();
    for (const world::Person& p : f.state.scene.people) ids.push_back(p.id);
    GIANT_CHECK(f.run("remove_people", {{"ids", ids}}).ok);
    GIANT_CHECK(f.senses.noise({0, 0, 0}, 20.0f, PeopleSenses::Kind::voice));
    f.frames(0.1);
    GIANT_CHECK(f.senses.stats().noises == 1);
    f.frames(1.0);
    GIANT_CHECK(f.senses.stats().noises == 0);
}
