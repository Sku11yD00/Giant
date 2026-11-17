// People following their routines (people_routines.h): sent where the
// entry that holds at the hour says, each at its own time, a share a
// frame, and afresh on every run.
#include "../src/animation_state.h"
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"
#include "../src/people_routines.h"
#include "../src/people_state.h"
#include "../src/people_travel.h"

#include "giant_test.h"
#include "gltf_test_files.h"

#include "giant/world/people.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

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
    PeopleRoutines routines;
    double seconds = 0.0;
    Fixture() {
        state.meshes = &meshes;
        state.travel = &travel;
        state.routines = &routines;
        register_scene_commands(commands, state);
        register_people_commands(commands, state);
        register_route_commands(commands, state);
        world::Entity e;
        e.shape = world::Shape::mesh;
        e.mesh = "people/villager.glb";
        const world::EntityId model = state.scene.add(e);
        GIANT_CHECK(run("set_person_look", {{"name", "villager"}, {"entity", model}}).ok);
        GIANT_CHECK(run("delete_entities", {{"ids", {model}}}).ok);
        // A road east 100 m then north 100 m; a cottage by its start, an inn half way, a mill past its end, another far off.
        GIANT_CHECK(run("create_route", {{"points", {at(0, 0, 0), at(100, 0, 0), at(100, 0, 100)}}, {"kind", "road"}}).ok);
        GIANT_CHECK(run("create_place", {{"name", "cottage"}, {"position", at(0, 0, -5)}, {"tags", {"home"}}}).ok);
        GIANT_CHECK(run("create_place", {{"name", "inn"}, {"position", at(50, 0, 3)}, {"radius", 2.0}, {"tags", {"rest"}}}).ok);
        GIANT_CHECK(run("create_place", {{"name", "far mill"}, {"position", at(100, 0, 3000)}, {"tags", {"work"}}}).ok);
        GIANT_CHECK(run("create_place", {{"name", "mill"}, {"position", at(100, 0, 110)}, {"radius", 4.0}, {"tags", {"work"}}}).ok);
        GIANT_CHECK(run("set_routine", {{"name", "day"}, {"entries", {{{"from", 6}, {"activity", "work"}, {"at", "tag"}, {"name", "work"}, {"speed", 2.0}},
                                                                      {{"from", 12}, {"activity", "eat"}, {"at", "place"}, {"name", "inn"}},
                                                                      {{"from", 13}, {"activity", "rest"}},
                                                                      {{"from", 20}, {"activity", "sleep"}, {"at", "home"}}}}}).ok);
        routines.settings.spread_minutes = 0.0f;
    }
    ai::CommandResult run(const char* name, const Json& args = Json::object()) { return commands.execute(name, args, {}); }
    world::PersonId person(const char* home = "cottage", const char* routine = "day") {
        const ai::CommandResult r = run("create_people", {{"people", {{{"look", "villager"}, {"home", home}, {"routine", routine}}}}});
        GIANT_CHECK(r.ok);
        return r.ok ? r.value["ids"][0].get<world::PersonId>() : 0;
    }
    // Frames of run `run_id`: `seconds` more simulated over them, at the hour.
    void frames(double hours, double more = 0.0, int count = 4, u64 run_id = 1) {
        for (int i = 0; i < count; ++i) {
            seconds += more / count;
            travel.update(state.scene, run_id, seconds);
            routines.update(state.scene, travel, hours);
        }
    }
    DVec3 where(world::PersonId id) { return travel.where(*world::find_person(state.scene.people, id)).position; }
    double from(world::PersonId id, double x, double z) { return std::hypot(where(id).x - x, where(id).z - z); }
    Json doing(world::PersonId id) { return run("get_people", {{"ids", {id}}}).value["people"][0]["doing"]; }
};

} // namespace

GIANT_TEST("routines: one is sent where each entry says as the hours pass, stays where told, and goes home") {
    Fixture f;
    const world::PersonId id = f.person();
    // Not simulating: nobody acts, nothing is being done.
    f.frames(7.0, 0.0, 2, 0);
    GIANT_CHECK(f.routines.stats().sent == 0 && f.doing(id).is_null() && f.travel.trip(id) == nullptr);
    // Seven o'clock: to work, the mill (the place tagged work nearest its home), at the entry's speed.
    f.frames(7.0);
    const PeopleTravel::Trip* trip = f.travel.trip(id);
    GIANT_CHECK(trip != nullptr && trip->place == "mill" && trip->speed == 2.0 && f.routines.stats().sent == 1);
    GIANT_CHECK(std::hypot(trip->to.x - 100.0, trip->to.z - 110.0) <= 0.8 * 4.0 + 1e-9); // a spot of its own within it
    Json d = f.doing(id);
    GIANT_CHECK(d["activity"] == "work" && d["place"] == "mill" && d["failed"] == false);
    // By the clock it gets there (about 215 m at 2 m/s) and is sent no more while the entry holds.
    f.frames(7.5, 150.0);
    GIANT_CHECK(f.travel.trip(id) == nullptr && f.from(id, 100, 110) < 4.0 && f.routines.stats().sent == 1);
    f.frames(11.9, 10.0);
    GIANT_CHECK(f.routines.stats().sent == 1 && f.travel.trip(id) == nullptr);
    // Noon: to the inn by name. On its way at one: told to stay, it stops where it is.
    f.frames(12.0);
    GIANT_CHECK(f.travel.trip(id) != nullptr && f.travel.trip(id)->place == "inn" && f.routines.stats().sent == 2);
    f.frames(12.5, 30.0);
    const DVec3 on_road = f.where(id);
    GIANT_CHECK(f.travel.trip(id) != nullptr && f.from(id, 50, 3) > 20.0);
    f.frames(13.0);
    GIANT_CHECK(f.travel.trip(id) == nullptr && f.routines.stats().stayed == 1 && f.doing(id)["activity"] == "rest" &&
                f.doing(id)["place"] == "");
    f.frames(15.0, 60.0);
    GIANT_CHECK(f.where(id) == on_road);
    // Evening: home. Over midnight it stays; at six it sets off again.
    f.frames(20.0);
    GIANT_CHECK(f.travel.trip(id) != nullptr && f.travel.trip(id)->place == "cottage" && f.routines.stats().sent == 3);
    f.frames(21.0, 400.0);
    GIANT_CHECK(f.travel.trip(id) == nullptr && f.from(id, 0, -5) < 1.0);
    f.frames(23.9);
    f.frames(0.0);
    f.frames(5.9);
    GIANT_CHECK(f.routines.stats().sent == 3 && f.doing(id)["activity"] == "sleep");
    f.frames(6.0);
    GIANT_CHECK(f.routines.stats().sent == 4 && f.travel.trip(id)->place == "mill");
    // The simulation stopped: everyone is back, nothing is being done; started, each begins afresh.
    f.frames(6.0, 0.0, 2, 0);
    GIANT_CHECK(f.doing(id).is_null() && f.routines.stats().sent == 0 && f.where(id).z == -5.0);
    f.frames(21.0, 0.0, 2, 2); // at home when home is where to be: sent nowhere
    GIANT_CHECK(f.routines.stats().sent == 0 && f.routines.stats().stayed == 1 && f.doing(id)["place"] == "cottage");
    // Off: nobody is sent.
    ai::CommandResult r = f.run("set_routine_settings", {{"on", false}});
    GIANT_CHECK(r.ok && r.value["on"] == false && r.value["following"] == 1 && r.value["stayed"] == 1);
    f.frames(7.0, 0.0, 2, 2);
    GIANT_CHECK(f.travel.trip(id) == nullptr);
    GIANT_CHECK(f.run("set_routine_settings", {{"on", true}, {"spread_minutes", 30.0}, {"sent_a_frame", 8}, {"looked_a_frame", 64}, {"within", 100.0}}).ok);
    GIANT_CHECK(f.routines.settings.spread_minutes == 30.0f && f.routines.settings.sent_a_frame == 8 &&
                f.routines.settings.looked_a_frame == 64 && f.routines.settings.within == 100.0f);
    GIANT_CHECK(!f.run("set_routine_settings", {{"spread_minutes", 500.0}}).ok);
    // Most a frame: what keeps a frame's work bounded.
    GIANT_CHECK(f.run("set_routine_settings", {{"looked_a_frame", 8192}, {"sent_a_frame", 128}, {"within", 1000.0}}).ok);
    GIANT_CHECK(!f.run("set_routine_settings", {{"looked_a_frame", 8193}}).ok && !f.run("set_routine_settings", {{"sent_a_frame", 129}}).ok &&
                !f.run("set_routine_settings", {{"within", 1001.0}}).ok && !f.run("set_routine_settings", {{"looked_a_frame", 0}}).ok);
    GIANT_CHECK(f.routines.settings.looked_a_frame == 8192 && f.routines.settings.sent_a_frame == 128);
}

GIANT_TEST("routines: a village sets off over its spread, a few a frame, on ways found once") {
    Fixture f;
    Json batch = Json::array();
    for (int i = 0; i < 200; ++i) batch.push_back({{"look", "villager"}, {"home", "cottage"}, {"routine", "day"}});
    const ai::CommandResult made = f.run("create_people", {{"people", batch}});
    GIANT_CHECK(made.ok);
    f.routines.settings.spread_minutes = 60.0f;
    // Each keeps its own hour, the same whenever asked, late by up to the spread.
    double latest = 0.0;
    usize before_half = 0;
    for (const Json& id : made.value["ids"]) {
        const world::PersonId p = id.get<world::PersonId>();
        const double late = 6.0 - f.routines.hour_of(p, 6.0);
        GIANT_CHECK(late >= 0.0 && late < 1.0 && f.routines.hour_of(p, 6.0) == f.routines.hour_of(p, 30.0));
        latest = std::max(latest, late);
        before_half += late <= 0.5 ? 1 : 0;
    }
    GIANT_CHECK(latest > 0.9 && before_half > 70 && before_half < 130);
    GIANT_CHECK(f.routines.hour_of(1, 0.25) > 23.0 || f.routines.hour_of(1, 0.25) <= 0.25); // round midnight
    // Half past six: those whose hour has come are sent, the rest sleep on (at home: sent nowhere).
    f.frames(6.5, 0.0, 12);
    GIANT_CHECK(f.routines.stats().sent == before_half && f.routines.stats().following == 200);
    // Seven: all of them, 32 a frame.
    f.routines.update(f.state.scene, f.travel, 7.0);
    GIANT_CHECK(f.routines.stats().sent == before_half + 32);
    f.frames(7.0, 0.0, 12);
    GIANT_CHECK(f.routines.stats().sent == 200 && f.routines.stats().failed == 0);
    // Their ways: all from the one node to the other, so found by one search and taken again by the rest.
    f.frames(7.0, 1.0, 8);
    const PeopleTravel::Stats t = f.travel.stats();
    GIANT_CHECK(t.searching == 0 && t.searches == 1 && t.reused == 199 && t.trips == 200);
    // Each to a spot of its own within the mill.
    f.frames(7.5, 300.0, 8);
    double spread_x = 0.0;
    for (const Json& id : made.value["ids"]) {
        const world::PersonId p = id.get<world::PersonId>();
        GIANT_CHECK(f.from(p, 100, 110) <= 0.8 * 4.0 + 1e-6);
        spread_x = std::max(spread_x, std::fabs(f.where(p).x - 100.0));
    }
    GIANT_CHECK(spread_x > 2.0 && f.travel.stats().trips == 0);
    // The roads changed: ways kept are forgotten (the next is searched for again).
    GIANT_CHECK(f.run("create_route", {{"points", {at(100, 0, 100), at(200, 0, 100)}}}).ok);
    f.routines.settings.spread_minutes = 0.0f;
    f.frames(12.0, 2.0, 24);
    // (The inn lies half way between two of the road's nodes: a way is found to each.)
    GIANT_CHECK(f.travel.stats().searches == 3 && f.travel.stats().reused == 199 + 198 && f.routines.stats().sent == 400);
}

GIANT_TEST("routines: what cannot be done is told; changes to routines, places and people are followed") {
    Fixture f;
    GIANT_CHECK(f.run("set_routine", {{"name", "homebody"}, {"entries", {{{"from", 0}, {"activity", "sleep"}, {"at", "home"}}}}}).ok);
    const world::PersonId a = f.person();
    // One with no home told to go home, and one far from any road: they stay, and it is told.
    ai::CommandResult r = f.run("create_people", {{"people", {{{"look", "villager"}, {"position", at(0, 0, 2)}, {"routine", "homebody"}},
                                                               {{"look", "villager"}, {"position", at(0, 0, 9000)}, {"routine", "day"}}}}});
    GIANT_CHECK(r.ok);
    const world::PersonId homeless = r.value["ids"][0].get<world::PersonId>(), lost = r.value["ids"][1].get<world::PersonId>();
    f.frames(7.0);
    GIANT_CHECK(f.routines.stats().sent == 1 && f.routines.stats().failed == 2);
    GIANT_CHECK(f.doing(homeless)["failed"] == true && f.doing(homeless)["place"] == "" && f.doing(lost)["failed"] == true &&
                f.doing(lost)["place"] == "far mill" && f.travel.trip(lost) == nullptr);
    f.frames(7.0, 1.0, 8);
    GIANT_CHECK(f.routines.stats().failed == 2); // told once, not every frame
    // A nearer place to work is made: those sent already keep theirs; the next entry that asks takes it.
    GIANT_CHECK(f.run("create_place", {{"name", "smithy"}, {"position", at(20, 0, 4)}, {"tags", {"work"}}}).ok);
    f.frames(7.0, 1.0, 8);
    GIANT_CHECK(f.travel.trip(a)->place == "mill");
    // The routine changed (work is quicker gone to): acted on again, now to the smithy.
    GIANT_CHECK(f.run("set_routine", {{"name", "day"}, {"entries", {{{"from", 6}, {"activity", "work"}, {"at", "tag"}, {"name", "work"}, {"speed", 3.0}},
                                                                    {{"from", 20}, {"activity", "sleep"}, {"at", "home"}}}}}).ok);
    f.frames(7.0);
    GIANT_CHECK(f.travel.trip(a)->place == "smithy" && f.travel.trip(a)->speed == 3.0 && f.routines.stats().sent == 2);
    // Given another routine: that one's. Given none: nothing more is done (its trip goes on).
    GIANT_CHECK(f.run("set_person", {{"id", a}, {"routine", "homebody"}}).ok);
    f.frames(7.0);
    GIANT_CHECK(f.travel.trip(a)->place == "cottage" && f.doing(a)["activity"] == "sleep");
    GIANT_CHECK(f.run("set_person", {{"id", a}, {"routine", ""}}).ok);
    f.frames(9.0, 1.0);
    GIANT_CHECK(f.doing(a).is_null() && f.travel.trip(a) != nullptr && f.routines.stats().following == 2);
    // A place named by a routine is taken away: told, and the person stays.
    GIANT_CHECK(f.run("set_routine", {{"name", "regular"}, {"entries", {{{"from", 0}, {"activity", "eat"}, {"at", "place"}, {"name", "inn"}}}}}).ok);
    GIANT_CHECK(f.run("remove_place", {{"name", "inn"}}).ok);
    f.frames(9.0); // (the places changed: those who could not be sent try again, and are told again)
    GIANT_CHECK(f.run("set_person", {{"id", a}, {"routine", "regular"}}).ok);
    const u64 failed = f.routines.stats().failed;
    f.frames(9.0);
    GIANT_CHECK(f.routines.stats().failed == failed + 1 && f.doing(a)["failed"] == true && f.doing(a)["activity"] == "eat");
    // People removed before it in the list: it is still looked at as itself (and others are not taken for it).
    GIANT_CHECK(f.run("set_person", {{"id", lost}, {"position", at(0, 0, 1)}, {"routine", "homebody"}, {"home", "cottage"}}).ok);
    GIANT_CHECK(f.run("remove_people", {{"ids", {a, homeless}}}).ok);
    f.frames(9.0);
    GIANT_CHECK(f.doing(lost)["activity"] == "sleep" && f.doing(lost)["place"] == "cottage" && f.travel.trip(lost) != nullptr);
    // Everyone removed, and every routine: nothing is followed.
    GIANT_CHECK(f.run("remove_people", {{"ids", {lost}}}).ok);
    f.frames(9.0);
    GIANT_CHECK(f.routines.stats().following == 0);
}

GIANT_TEST("routines: people removed and brought back leave the others as they were") {
    Fixture f;
    // (Each with its own routine: one taken for another would act on that one's.)
    GIANT_CHECK(f.run("set_routine", {{"name", "innkeeper"}, {"entries", {{{"from", 0}, {"activity", "serve"}, {"at", "place"}, {"name", "inn"}}}}}).ok);
    GIANT_CHECK(f.run("set_routine", {{"name", "miller"}, {"entries", {{{"from", 0}, {"activity", "mill"}, {"at", "place"}, {"name", "mill"}, {"speed", 3.0}}}}}).ok);
    const world::PersonId a = f.person(), b = f.person("cottage", "innkeeper"), c = f.person("cottage", "miller");
    f.frames(7.0);
    GIANT_CHECK(f.routines.stats().sent == 3 && f.travel.trip(c) != nullptr && f.travel.trip(b)->place == "inn");
    const u64 serial_a = f.travel.trip(a)->serial, serial_c = f.travel.trip(c)->serial;
    // The one between them removed: the one after it is not taken for another, nor sent again.
    GIANT_CHECK(f.run("remove_people", {{"ids", {b}}}).ok);
    f.frames(7.0, 1.0);
    GIANT_CHECK(f.routines.stats().sent == 3 && f.travel.trip(a)->serial == serial_a && f.travel.trip(c)->serial == serial_c);
    GIANT_CHECK(f.doing(c)["activity"] == "mill" && f.doing(c)["place"] == "mill" && f.doing(c)["failed"] == false);
    GIANT_CHECK(f.routines.stats().following == 2); // (counted among the people as they are)
    // Brought back by undo: it alone begins afresh.
    GIANT_CHECK(f.run("undo").ok && f.state.scene.people.size() == 3);
    f.frames(7.0, 1.0);
    GIANT_CHECK(f.routines.stats().sent == 4 && f.travel.trip(b)->place == "inn" && f.travel.trip(a)->serial == serial_a &&
                f.travel.trip(c)->serial == serial_c);
    // As many as before, but others (one removed, one made in the same frame): each is still itself.
    GIANT_CHECK(f.run("remove_people", {{"ids", {a}}}).ok);
    const world::PersonId d = f.person();
    f.frames(7.0, 1.0);
    GIANT_CHECK(f.routines.stats().sent == 5 && f.travel.trip(d) != nullptr && f.travel.trip(c)->serial == serial_c &&
                f.doing(d)["place"] == "mill" && f.routines.stats().following == 3);
}

GIANT_TEST("routines: places removed, moved and made among many are followed; what one does names its own place") {
    Fixture f;
    // Many places (more than are looked at a frame for having changed), none of them where anyone goes.
    for (int i = 0; i < 600; ++i) {
        GIANT_CHECK(f.run("create_place", {{"name", "field " + std::to_string(i)}, {"position", at(-500.0 - i, 0, 0)}, {"tags", {"farm"}}}).ok);
    }
    const world::PersonId a = f.person();
    f.frames(7.0);
    GIANT_CHECK(f.travel.trip(a) != nullptr && f.doing(a)["place"] == "mill");
    // A place before the mill in the list is removed: the mill is still the mill.
    GIANT_CHECK(f.run("remove_place", {{"name", "far mill"}}).ok);
    GIANT_CHECK(f.doing(a)["place"] == "mill" || f.doing(a)["place"] == ""); // (never another's name)
    f.frames(7.0, 1.0);
    GIANT_CHECK(f.doing(a)["place"] == "mill" && f.routines.stats().sent == 1 && f.routines.stats().failed == 0);
    // A place to work is moved next to its home (as many places as before): seen within a few frames,
    // and the next entry that says so sends it there, not where the mill was nearest.
    GIANT_CHECK(f.run("create_place", {{"name", "smithy"}, {"position", at(100, 0, 2000)}, {"tags", {"work"}}}).ok);
    f.frames(7.0, 1.0);
    GIANT_CHECK(f.run("set_place", {{"name", "smithy"}, {"position", at(20, 0, 4)}}).ok);
    f.frames(7.0, 1.0, 8);
    GIANT_CHECK(f.travel.trip(a)->place == "mill"); // (on its way already: it keeps on)
    GIANT_CHECK(f.run("set_routine", {{"name", "day"}, {"entries", {{{"from", 6}, {"activity", "work"}, {"at", "tag"}, {"name", "work"}, {"speed", 3.0}}}}}).ok);
    f.frames(7.0);
    GIANT_CHECK(f.travel.trip(a)->place == "smithy" && f.doing(a)["place"] == "smithy" && f.routines.stats().sent == 2);
    GIANT_CHECK(std::hypot(f.travel.trip(a)->to.x - 20.0, f.travel.trip(a)->to.z - 4.0) <= 0.8 + 1e-9);
    // A place's tag taken away (as many places as before): no longer gone to for it.
    GIANT_CHECK(f.run("set_place", {{"name", "smithy"}, {"tags", Json::array()}}).ok);
    f.frames(7.0, 0.0, 8);
    GIANT_CHECK(f.run("set_routine", {{"name", "day"}, {"entries", {{{"from", 6}, {"activity", "work"}, {"at", "tag"}, {"name", "work"}, {"speed", 2.5}}}}}).ok);
    f.frames(7.0);
    GIANT_CHECK(f.travel.trip(a)->place == "mill" && f.routines.stats().sent == 3);
    // One who could not be sent (nowhere to work at is tagged so) is, once such a place is made.
    GIANT_CHECK(f.run("set_routine", {{"name", "herd"}, {"entries", {{{"from", 0}, {"activity", "herd"}, {"at", "tag"}, {"name", "pasture"}}}}}).ok);
    GIANT_CHECK(f.run("set_person", {{"id", a}, {"routine", "herd"}}).ok);
    f.frames(7.0);
    GIANT_CHECK(f.doing(a)["failed"] == true && f.routines.stats().failed == 1);
    GIANT_CHECK(f.run("create_place", {{"name", "meadow"}, {"position", at(60, 0, -4)}, {"tags", {"pasture"}}}).ok);
    f.frames(7.0);
    GIANT_CHECK(f.doing(a)["failed"] == false && f.travel.trip(a)->place == "meadow" && f.routines.stats().sent == 4);
}

GIANT_TEST("routines: one sent by hand is left to it, and a trip that fails on the way is told once") {
    Fixture f;
    const world::PersonId a = f.person();
    f.frames(12.0);
    GIANT_CHECK(f.travel.trip(a) != nullptr && f.travel.trip(a)->place == "inn");
    // Sent to the mill by hand: the routine does not call it back while its entry holds, and `stay`
    // at one stops only a trip the routine began.
    GIANT_CHECK(f.run("send_person", {{"id", a}, {"to", "mill"}}).ok);
    f.frames(12.5, 1.0);
    GIANT_CHECK(f.travel.trip(a)->place == "mill" && f.routines.stats().sent == 1);
    f.frames(13.0, 1.0);
    GIANT_CHECK(f.travel.trip(a) != nullptr && f.travel.trip(a)->place == "mill" && f.routines.stats().stayed == 1 &&
                f.doing(a)["activity"] == "rest");
    // The routine's next word is followed again: home at eight.
    f.frames(20.0);
    GIANT_CHECK(f.travel.trip(a)->place == "cottage" && f.routines.stats().sent == 2);
    // Going where the next entry sends it already: left alone. (Both entries say the same: nothing new.)
    GIANT_CHECK(f.run("set_routine", {{"name", "day"}, {"entries", {{{"from", 0}, {"activity", "sleep"}, {"at", "home"}},
                                                                    {{"from", 21}, {"activity", "dream"}, {"at", "home"}}}}}).ok);
    const u64 serial = f.travel.trip(a)->serial;
    f.frames(20.5, 0.2);
    f.frames(21.5, 0.2);
    GIANT_CHECK(f.travel.trip(a)->serial == serial && f.routines.stats().sent == 2 && f.doing(a)["activity"] == "dream");

    // A way that cannot be found after all (the road is taken away while it is looked for): told, once.
    Fixture g;
    const world::PersonId b = g.person();
    g.frames(7.0, 0.0, 1);
    GIANT_CHECK(g.routines.stats().sent == 1 && g.travel.trip(b)->phase == PeopleTravel::Phase::searching);
    GIANT_CHECK(g.run("remove_route_nodes", {{"ids", {g.state.scene.routes.nodes[2].id}}}).ok);
    g.frames(7.0, 1.0, 6);
    GIANT_CHECK(g.travel.trip(b) != nullptr && g.travel.trip(b)->phase == PeopleTravel::Phase::failed);
    GIANT_CHECK(g.routines.stats().failed == 1 && g.doing(b)["failed"] == true && g.routines.stats().sent == 1);
}

GIANT_TEST("routines: a way found for one is taken by another from where that one stands; the nearest place is measured once a village") {
    Fixture f;
    ai::CommandResult r = f.run("create_people", {{"people", {{{"look", "villager"}, {"position", at(0, 0, 2)}, {"home", "cottage"}, {"routine", "day"}},
                                                               {{"look", "villager"}, {"position", at(6, 0, -3)}, {"home", "cottage"}, {"routine", "day"}}}}});
    GIANT_CHECK(r.ok);
    const world::PersonId a = r.value["ids"][0].get<world::PersonId>(), b = r.value["ids"][1].get<world::PersonId>();
    f.frames(7.0, 0.5, 6);
    const PeopleTravel::Trip* first = f.travel.trip(a);
    const PeopleTravel::Trip* second = f.travel.trip(b);
    GIANT_CHECK(first != nullptr && second != nullptr && f.travel.stats().searches == 1 && f.travel.stats().reused == 1);
    GIANT_CHECK(first->points.front() == DVec3(0, 0, 2) && second->points.front() == DVec3(6, 0, -3));
    // (The second joins the road a node on: nearer to it than the road's own way there. The rest is the same way.)
    GIANT_CHECK(first->points.size() == 5 && second->points.size() == 4 && first->points[2] == second->points[1] &&
                first->points[3] == second->points[2]);

    // Many places to work at, all as far east (so each must be measured), and many with no home
    // (each measures for itself): a frame measures so many and no more, the rest wait; those of
    // one home ask once.
    Fixture g;
    for (int i = 0; i < 600; ++i) {
        GIANT_CHECK(g.run("create_place", {{"name", "bench " + std::to_string(i)}, {"position", at(50, 0, 6.0 + 0.01 * i)}, {"tags", {"craft"}}}).ok);
    }
    GIANT_CHECK(g.run("set_routine", {{"name", "crafter"}, {"entries", {{{"from", 0}, {"activity", "craft"}, {"at", "tag"}, {"name", "craft"}}}}}).ok);
    Json homeless = Json::array(), housed = Json::array();
    for (int i = 0; i < 100; ++i) {
        homeless.push_back({{"look", "villager"}, {"position", at(1.0 + 0.1 * i, 0, 1)}, {"routine", "crafter"}});
        housed.push_back({{"look", "villager"}, {"home", "cottage"}, {"routine", "crafter"}});
    }
    GIANT_CHECK(g.run("create_people", {{"people", homeless}}).ok);
    g.routines.settings.sent_a_frame = 128;
    g.frames(7.0, 0.0, 1);
    const u64 once = g.routines.stats().sent;
    GIANT_CHECK(once >= 27 && once <= 28); // 16384 measured a frame, 600 each
    g.frames(7.0, 0.0, 3);
    GIANT_CHECK(g.routines.stats().sent == 100 && g.routines.stats().failed == 0);
    GIANT_CHECK(g.run("create_people", {{"people", housed}}).ok);
    g.frames(7.0, 0.0, 1);
    GIANT_CHECK(g.routines.stats().sent == 200);
}

GIANT_TEST("routines: roads changed while stopped or while running are followed; what one does is where it was sent") {
    Fixture f;
    // One too far from the road to be sent.
    ai::CommandResult r = f.run("create_people", {{"people", {{{"look", "villager"}, {"position", at(0, 0, -900)}, {"routine", "day"}}}}});
    GIANT_CHECK(r.ok);
    const world::PersonId far = r.value["ids"][0].get<world::PersonId>();
    f.frames(7.0);
    GIANT_CHECK(f.routines.stats().failed == 1 && f.doing(far)["failed"] == true);
    // Stopped, and the road's first node moved near it (as many nodes and links as before): the next run sends it.
    f.frames(7.0, 0.0, 2, 0);
    GIANT_CHECK(f.run("set_route_node", {{"id", f.state.scene.routes.nodes[0].id}, {"position", at(0, 0, -800)}}).ok);
    f.frames(7.0, 0.0, 2, 2);
    GIANT_CHECK(f.routines.stats().sent == 1 && f.routines.stats().failed == 0 && f.doing(far)["failed"] == false);
    // While running: moved away again and one made afresh fails; moved back, it is tried again and sent.
    GIANT_CHECK(f.run("set_route_node", {{"id", f.state.scene.routes.nodes[0].id}, {"position", at(0, 0, 0)}}).ok);
    r = f.run("create_people", {{"people", {{{"look", "villager"}, {"position", at(3, 0, -900)}, {"routine", "day"}}}}});
    GIANT_CHECK(r.ok);
    const world::PersonId later = r.value["ids"][0].get<world::PersonId>();
    f.frames(7.0, 0.1, 4, 2);
    GIANT_CHECK(f.doing(later)["failed"] == true && f.routines.stats().failed == 1);
    GIANT_CHECK(f.run("set_route_node", {{"id", f.state.scene.routes.nodes[0].id}, {"position", at(0, 0, -800)}}).ok);
    f.frames(7.0, 0.1, 4, 2);
    GIANT_CHECK(f.doing(later)["failed"] == false && f.travel.trip(later) != nullptr && f.routines.stats().sent == 2);
    // A nearer place to work made since: what it does still names where it was sent.
    GIANT_CHECK(f.run("create_place", {{"name", "smithy"}, {"position", at(0, 0, -850)}, {"tags", {"work"}}}).ok);
    f.frames(7.0, 0.1, 4, 2);
    GIANT_CHECK(f.doing(later)["place"] == f.travel.trip(later)->place && f.travel.trip(later)->place != "smithy");
    // Its entry now asks for another speed: on its way to the same spot, but sent again at that speed.
    const std::string place = f.travel.trip(later)->place;
    GIANT_CHECK(f.run("remove_place", {{"name", "smithy"}}).ok);
    GIANT_CHECK(f.run("set_routine", {{"name", "day"}, {"entries", {{{"from", 6}, {"activity", "work"}, {"at", "tag"}, {"name", "work"}, {"speed", 4.0}}}}}).ok);
    f.frames(7.0, 0.1, 4, 2);
    GIANT_CHECK(f.travel.trip(later)->place == place && f.travel.trip(later)->speed == 4.0);
}

namespace {

// The fixture with people given entities near a camera, of a look whose file has clips (idle, raise):
// eating shows as an arm raised once, sleeping as idling slowly, working names a clip the file lacks.
struct Shown : Fixture {
    AnimationState animation{nullptr};
    PeopleState people;
    std::filesystem::path dir;
    DVec3 camera{5000.0, 0.0, 0.0};
    Shown() {
        people.travel = &travel;
        people.routines = &routines;
        people.animation = &animation;
        people.meshes = &meshes;
        people.settings.made_a_frame = 64;
        state.animation = &animation;
        state.people = &people;
        register_animation_commands(commands, state);
        // (A folder of this fixture's own: tests run side by side do not write each other's file.)
        dir = std::filesystem::temp_directory_path() /
              ("giant_routine_tests_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
               std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(dir);
        const std::string file = (dir / "person.glb").generic_string();
        const std::vector<u8> bytes = geometry::testing::skinned_person();
        std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        const ai::CommandResult r = run("create_entity", {{"shape", "mesh"}, {"mesh", file}});
        GIANT_CHECK(r.ok);
        GIANT_CHECK(run("set_person_look", {{"name", "villager"}, {"entity", r.value["id"]}, {"activities", shown()}}).ok);
        GIANT_CHECK(run("delete_entities", {{"ids", {r.value["id"]}}}).ok);
    }
    ~Shown() {
        std::error_code ignored;
        std::filesystem::remove_all(dir, ignored);
    }
    static Json shown() {
        return {{{"activity", "eat"}, {"clip", "raise"}, {"loop", false}},
                {{"activity", "sleep"}, {"clip", "idle"}, {"speed", 0.5}},
                {{"activity", "work"}, {"clip", "no such clip"}}};
    }
    void frames(double hours, double more = 0.0, int count = 4, u64 run_id = 1) {
        for (int i = 0; i < count; ++i) {
            seconds += more / count;
            travel.update(state.scene, run_id, seconds);
            routines.update(state.scene, travel, hours);
            people.update(state.scene, camera, nullptr);
            const world::SceneChanges changed = state.scene.take_changes();
            animation.update(state.scene, meshes, changed, more / count);
        }
    }
    bool by_command(world::PersonId id) const {
        const AnimationState::Animated* a = animation.find(world::person_entity_id(id));
        return a != nullptr && a->by_command;
    }
    // One gone to the inn out of sight, eating there, then seen.
    world::PersonId eater() {
        camera = {5000.0, 0.0, 0.0};
        frames(12.0, 0.0, 1);
        const world::PersonId id = person();
        frames(12.0);
        frames(12.2, 80.0);
        GIANT_CHECK(travel.trip(id) == nullptr && doing(id)["activity"] == "eat" && people.stats().embodied == 0);
        camera = {50.0, 0.0, 3.0};
        frames(12.3, 0.1);
        GIANT_CHECK(people.stats().embodied == 1 && people.stats().acting == 1 && by_command(id));
        return id;
    }
};

} // namespace

GIANT_TEST("routines: one standing where its routine has it plays its look's clip for what it does there, and is matched again when it goes") {
    Shown f;
    // Gone to the inn out of sight, it eats there; seen, its entity raises its arm (the look's clip for eating).
    const world::PersonId id = f.eater();
    const world::EntityId entity = world::person_entity_id(id);
    ai::CommandResult r = f.run("get_animation", {{"id", entity}});
    GIANT_CHECK(r.ok && r.value["by_command"] == true && r.value.dump().find("raise") != std::string::npos);
    GIANT_CHECK(f.run("set_people_settings").value["acting"] == 1);
    // Resting has no clip in the look: matched again (standing).
    f.frames(13.0, 0.1);
    GIANT_CHECK(f.people.stats().acting == 0 && !f.by_command(id) && f.doing(id)["activity"] == "rest");
    // Sent home at eight: on its way (it stands here: no walkers in this test), so nothing is played.
    f.frames(20.0, 0.1);
    GIANT_CHECK(f.travel.trip(id) != nullptr && f.people.stats().acting == 0 && !f.by_command(id));
    // Stopped and begun again in the evening by its home: asleep there, it idles; stopped, it is matched again.
    f.camera = {0.0, 0.0, -5.0};
    f.frames(21.0, 0.0, 4, 0);
    GIANT_CHECK(f.people.stats().acting == 0);
    f.frames(21.0, 0.1, 6, 2);
    GIANT_CHECK(f.doing(id)["activity"] == "sleep" && f.people.stats().embodied == 1 && f.people.stats().acting == 1 && f.by_command(id));
    const AnimationState::Animated* a = f.animation.find(entity);
    GIANT_CHECK(a != nullptr && a->player.speed() == 0.5f);
    f.frames(21.0, 0.0, 2, 0);
    GIANT_CHECK(f.people.stats().acting == 0 && !f.by_command(id));
}

GIANT_TEST("routines: a clip is played only where the routine sent one, only while routines run, and never one the file lacks") {
    Shown f;
    // Stopped half way to the inn (out of sight, so by the clock), then seen: it eats in name, but is not there.
    const world::PersonId walker = f.person();
    f.frames(12.0);
    f.frames(12.1, 15.0);
    GIANT_CHECK(f.travel.trip(walker) != nullptr && f.run("stop_person", {{"id", walker}}).ok);
    const DVec3 stopped_at = f.where(walker);
    GIANT_CHECK(std::hypot(stopped_at.x - 50.0, stopped_at.z - 3.0) > 10.0);
    f.camera = stopped_at;
    f.frames(12.2, 0.1);
    GIANT_CHECK(f.people.stats().embodied == 1 && f.doing(walker)["activity"] == "eat" && f.people.stats().acting == 0 && !f.by_command(walker));
    GIANT_CHECK(f.run("remove_people", {{"ids", {walker}}}).ok);
    // One eating at the inn: routines turned off, its clip ends; on again, it is begun again.
    const world::PersonId id = f.eater();
    GIANT_CHECK(f.run("set_routine_settings", {{"on", false}}).ok);
    f.frames(12.4, 0.1);
    GIANT_CHECK(f.people.stats().acting == 0 && !f.by_command(id));
    GIANT_CHECK(f.run("set_routine_settings", {{"on", true}}).ok);
    f.frames(12.4, 0.1);
    GIANT_CHECK(f.people.stats().acting == 1 && f.by_command(id));
    // Its animation set by hand: the count is true at once, and the clip is begun again after a wait.
    GIANT_CHECK(f.run("set_animation", {{"id", world::person_entity_id(id)}, {"clip", "idle"}}).ok);
    f.frames(12.4, 0.1, 2);
    GIANT_CHECK(f.people.stats().acting == 0 && !f.by_command(id));
    f.frames(12.4, 3.0, 6);
    GIANT_CHECK(f.people.stats().acting == 1 && f.by_command(id));
    // At work, whose clip the file does not have: nothing plays, however long.
    f.frames(12.4, 0.0, 2, 0);
    f.camera = {5000.0, 0.0, 0.0};
    f.frames(7.0, 0.0, 2, 2);
    f.frames(7.5, 200.0, 8, 2);
    f.camera = {100.0, 0.0, 110.0};
    f.frames(7.5, 0.1, 2, 2);
    GIANT_CHECK(f.travel.trip(id) == nullptr && f.doing(id)["activity"] == "work" && f.people.stats().embodied == 1);
    f.frames(7.6, 10.0, 20, 2);
    GIANT_CHECK(f.people.stats().acting == 0 && !f.by_command(id));
}

GIANT_TEST("routines: an entity made again does not keep its last one's clip; a look's activities change without the entities going") {
    Shown f;
    // Eating at the inn, seen from where its home is in view too: the world stops, it is back home
    // (its entity taken and made again the same frame), and plays nothing there.
    world::PersonId id = f.eater();
    f.camera = {25.0, 0.0, 0.0};
    f.frames(12.3, 0.1);
    GIANT_CHECK(f.people.stats().acting == 1);
    f.frames(12.3, 0.0, 3, 0);
    GIANT_CHECK(f.where(id).z == -5.0 && f.people.stats().embodied == 1 && f.people.stats().acting == 0 && !f.by_command(id));
    // Again; its entity deleted while it eats: the one made in its place begins the clip itself (not left with the old).
    f.camera = {5000.0, 0.0, 0.0};
    f.frames(12.0, 0.0, 2, 2);
    f.frames(12.2, 80.0, 8, 2);
    f.camera = {50.0, 0.0, 3.0};
    f.frames(12.3, 0.1, 4, 2);
    GIANT_CHECK(f.people.stats().acting == 1 && f.by_command(id));
    const u64 made = f.people.stats().made;
    GIANT_CHECK(f.run("delete_entities", {{"ids", {world::person_entity_id(id)}}}).ok);
    f.frames(13.0, 0.1, 4, 2); // (resting now: no clip)
    GIANT_CHECK(f.people.stats().made == made + 1 && f.people.stats().acting == 0 && !f.by_command(id));
    // Eating again (the hour set back); the look's list emptied: the entity stays, matched again; given back, the clip plays.
    f.frames(12.5, 0.1, 4, 2);
    GIANT_CHECK(f.people.stats().acting == 1 && f.by_command(id));
    GIANT_CHECK(f.run("set_person_look", {{"name", "villager"}, {"activities", Json::array()}}).ok);
    f.frames(12.5, 0.1, 4, 2);
    GIANT_CHECK(f.people.stats().made == made + 1 && f.people.stats().acting == 0 && !f.by_command(id));
    GIANT_CHECK(f.run("set_person_look", {{"name", "villager"}, {"activities", Shown::shown()}}).ok);
    f.frames(12.5, 0.1, 4, 2);
    GIANT_CHECK(f.people.stats().made == made + 1 && f.people.stats().acting == 1 && f.by_command(id));
    // Asleep at home, never having moved, with no room to animate it (so its clip waits); the world
    // stopped and begun again (its entity stays): the wait is not carried over to the new run's clock.
    f.frames(12.5, 0.0, 2, 0);
    // (Its entity let go first, the camera far off for a frame, so that the one made by its home
    // is not animated already: taken and made again within one frame, whether the animation keeps
    // its copy goes by when the file's read lands.)
    f.camera = {5000.0, 0.0, 0.0};
    f.frames(12.5, 0.0, 2, 0);
    GIANT_CHECK(f.people.stats().embodied == 0 && f.animation.find(world::person_entity_id(id)) == nullptr);
    f.camera = {0.0, 0.0, -5.0};
    f.animation.max_animated = 0;
    f.frames(21.0, 1000.0, 4, 3);
    GIANT_CHECK(f.people.stats().embodied == 1 && f.people.stats().acting == 0 && f.doing(id)["activity"] == "sleep");
    const u64 kept = f.people.stats().made;
    f.animation.max_animated = AnimationState::k_max_animated;
    f.frames(21.0, 0.0, 2, 0);
    f.seconds = 0.0; // (the next run's clock begins again)
    f.frames(21.0, 0.5, 4, 4);
    GIANT_CHECK(f.people.stats().made == kept && f.people.stats().acting == 1 && f.by_command(id));
}

GIANT_TEST("routines: what one does is what it last acted on; clips are begun a few a frame") {
    Shown f;
    // Twelve at work at the mill; noon comes with one sent a frame: those not sent yet still work (they do not eat at the mill).
    Json batch = Json::array();
    for (int i = 0; i < 12; ++i) batch.push_back({{"look", "villager"}, {"home", "cottage"}, {"routine", "day"}});
    const ai::CommandResult made = f.run("create_people", {{"people", batch}});
    GIANT_CHECK(made.ok);
    f.frames(7.0);
    f.frames(7.5, 200.0, 8);
    GIANT_CHECK(f.travel.stats().trips == 0);
    f.routines.settings.sent_a_frame = 1;
    f.frames(12.0, 0.0, 3);
    usize eating = 0, working = 0;
    for (const Json& id : made.value["ids"]) {
        const world::PersonId p = id.get<world::PersonId>();
        PeopleRoutines::Activity doing;
        GIANT_CHECK(f.routines.activity(f.state.scene, p, doing));
        const bool sent = f.travel.trip(p) != nullptr;
        GIANT_CHECK((doing.activity == "eat") == sent && (doing.activity == "work") == !sent);
        eating += sent ? 1 : 0;
        working += sent ? 0 : 1;
        GIANT_CHECK(f.doing(p)["activity"] == (sent ? "eat" : "work"));
    }
    GIANT_CHECK(eating == 3 && working == 9);
    // All at the inn, then seen at once: eight begin their clip a frame, the rest the next.
    f.routines.settings.sent_a_frame = 32;
    f.frames(12.1, 200.0, 8);
    GIANT_CHECK(f.travel.stats().trips == 0);
    f.camera = {50.0, 0.0, 3.0};
    f.frames(12.2, 0.0, 1);
    GIANT_CHECK(f.people.stats().embodied == 12 && f.people.stats().acting == 0); // (made this frame: they act from the next)
    // (At most eight begin a frame, and of those not animated yet, each making its copy, two: this
    // file cannot be matched, so only the few the animation took up meanwhile are.)
    f.frames(12.2, 0.0, 1);
    const u32 first = f.people.stats().acting;
    GIANT_CHECK(first >= 2 && first <= 8);
    f.frames(12.2, 0.0, 1);
    GIANT_CHECK(f.people.stats().acting >= first + 2 && f.people.stats().acting <= first + 8);
    f.frames(12.2, 0.0, 4);
    GIANT_CHECK(f.people.stats().acting == 12);
    // Routines off: every clip ends at once (ending is not put off).
    GIANT_CHECK(f.run("set_routine_settings", {{"on", false}}).ok);
    f.frames(12.2, 0.0, 1);
    GIANT_CHECK(f.people.stats().acting == 0);
    GIANT_CHECK(f.run("set_routine_settings", {{"on", true}}).ok);
    f.frames(12.2, 0.0, 6);
    GIANT_CHECK(f.people.stats().acting == 12);
}
