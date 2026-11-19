// People travelling (people_travel.h): sent along the world's roads by
// command, moved by the simulation's clock, walked by the entity that
// stands for one near the camera (people_state.h), and back where the
// scene has them when the simulation stops.
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"
#include "../src/people_state.h"
#include "../src/people_travel.h"
#include "../src/nav_state.h"
#include "../src/physics_state.h"
#include "../src/walker_state.h"

#include "giant_test.h"

#include "giant/world/people.h"
#include "giant/world/routes.h"

#include <algorithm>
#include <cmath>
#include <string>
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
    Fixture() {
        state.meshes = &meshes;
        state.travel = &travel;
        register_scene_commands(commands, state);
        register_people_commands(commands, state);
        register_route_commands(commands, state);
        world::Entity e;
        e.shape = world::Shape::mesh;
        e.mesh = "people/villager.glb";
        const world::EntityId model = state.scene.add(e);
        GIANT_CHECK(run("set_person_look", {{"name", "villager"}, {"entity", model}}).ok);
        GIANT_CHECK(run("delete_entities", {{"ids", {model}}}).ok);
        // A road east 100 m then north 100 m, and the mill 10 m past its end.
        GIANT_CHECK(run("create_route", {{"points", {at(0, 0, 0), at(100, 0, 0), at(100, 0, 100)}}, {"kind", "road"}}).ok);
        GIANT_CHECK(run("create_place", {{"name", "mill"}, {"position", at(100, 0, 110)}}).ok);
    }
    ai::CommandResult run(const char* name, const Json& args = Json::object()) {
        return commands.execute(name, args, {});
    }
    world::PersonId person(double x, double z) {
        const ai::CommandResult r = run("create_people", {{"people", {{{"look", "villager"}, {"position", at(x, 0, z)}}}}});
        GIANT_CHECK(r.ok);
        return r.ok ? r.value["ids"][0].get<world::PersonId>() : 0;
    }
    // Simulated time moved to `seconds` of run 1, over a few frames.
    void to(double seconds) {
        for (int i = 0; i < 4; ++i) travel.update(state.scene, 1, seconds);
    }
    DVec3 where(world::PersonId id) { return travel.where(*world::find_person(state.scene.people, id)).position; }
    bool near(world::PersonId id, double x, double y, double z) {
        const DVec3 p = where(id);
        return std::fabs(p.x - x) < 1e-6 && std::fabs(p.y - y) < 1e-6 && std::fabs(p.z - z) < 1e-6;
    }
};

} // namespace

GIANT_TEST("travel: a person sent to a place goes along the road by the clock, and stays there") {
    Fixture f;
    const world::PersonId id = f.person(0, -10);
    // Not simulating: nobody travels.
    f.travel.update(f.state.scene, 0, 0.0);
    GIANT_CHECK(!f.run("send_person", {{"id", id}, {"to", "mill"}}).ok && f.run("get_travel").value["running"] == false);
    f.travel.update(f.state.scene, 1, 0.0);
    GIANT_CHECK(!f.run("send_person", {{"id", 99}, {"to", "mill"}}).ok && !f.run("send_person", {{"id", id}, {"to", "inn"}}).ok);
    GIANT_CHECK(!f.run("send_person", {{"id", id}, {"to", 5}}).ok && !f.run("send_person", {{"id", id}, {"to", at(1e300, 0, 0)}}).ok);
    GIANT_CHECK(!f.run("send_person", {{"id", id}, {"to", at(9000, 0, 0)}}).ok); // no road near there
    GIANT_CHECK(f.run("get_travel").value["trips"] == 0);
    ai::CommandResult r = f.run("send_person", {{"id", id}, {"to", "mill"}, {"speed", 2.0}});
    GIANT_CHECK(r.ok && r.value["trip"]["phase"] == "searching" && r.value["trip"]["place"] == "mill");
    GIANT_CHECK(f.run("get_travel").value["searching"] == 1 && f.near(id, 0, 0, -10));
    f.to(0.0);
    r = f.run("get_travel", {{"id", id}});
    GIANT_CHECK(r.ok && r.value["trip"]["phase"] == "travelling" && r.value["searching"] == 0 && r.value["searches"] == 1);
    GIANT_CHECK(std::fabs(r.value["trip"]["length"].get<double>() - 220.0) < 1e-9 && r.value["trip"]["arrives_in"] == 110.0);
    // 2 m/s: to the road, along it east, round the corner north.
    f.to(2.5);
    GIANT_CHECK(f.near(id, 0, 0, -5));
    f.to(30.0);
    GIANT_CHECK(f.near(id, 50, 0, 0));
    const world::Person& person = *world::find_person(f.state.scene.people, id);
    GIANT_CHECK(std::fabs(f.travel.where(person).yaw - 90.0f) < 1e-3f); // facing east
    f.to(60.0);
    GIANT_CHECK(f.near(id, 100, 0, 10) && std::fabs(f.travel.where(person).yaw) < 1e-3f); // north
    r = f.run("get_people", {{"ids", {id}}});
    GIANT_CHECK(r.ok && r.value["people"][0]["at"] == at(100, 0, 10) && r.value["people"][0]["position"] == at(0, 0, -10));
    GIANT_CHECK(r.value["people"][0]["trip"]["gone"] == 120.0);
    // There: the trip is over, the person stays (the scene still has where it started).
    f.to(500.0);
    r = f.run("get_travel", {{"id", id}});
    GIANT_CHECK(r.ok && r.value["trip"].is_null() && r.value["at"] == at(100, 0, 110) && r.value["arrived"] == 1 && r.value["moved"] == 1);
    GIANT_CHECK(person.position == DVec3(0.0, 0.0, -10.0));
    // Sent on from there to somewhere the road is no help: straight, at once; stopped half way, it stays.
    r = f.run("send_person", {{"id", id}, {"to", at(100, 0, 90)}, {"speed", 1.0}});
    GIANT_CHECK(r.ok && r.value["trip"]["phase"] == "travelling" && r.value["trip"]["length"] == 20.0);
    f.to(510.0);
    GIANT_CHECK(f.near(id, 100, 0, 100) && f.run("stop_person", {{"id", id}}).ok && !f.run("stop_person", {{"id", id}}).ok);
    f.to(600.0);
    GIANT_CHECK(f.near(id, 100, 0, 100) && f.run("get_travel").value["trips"] == 0);
    // Sent again while on a trip, it goes on from where it has got to.
    GIANT_CHECK(f.run("send_person", {{"id", id}, {"to", at(0, 0, 0)}, {"speed", 10.0}}).ok);
    f.to(600.0);
    f.to(605.0); // 50 m: down the road south
    GIANT_CHECK(f.near(id, 100, 0, 50) && f.run("send_person", {{"id", id}, {"to", "mill"}, {"speed", 10.0}}).ok);
    f.to(605.0);
    f.to(606.0);
    GIANT_CHECK(f.near(id, 100, 0, 60));
    // The simulation stopped, or started again: everyone is back where the scene has them.
    f.travel.update(f.state.scene, 2, 0.0);
    GIANT_CHECK(f.near(id, 0, 0, -10) && f.run("get_travel").value["trips"] == 0 && f.run("get_travel").value["moved"] == 0);
}

GIANT_TEST("travel: a trip finds its person again when others are removed or added before it") {
    Fixture f;
    const world::PersonId first = f.person(90, -10), second = f.person(60, 5), third = f.person(100, 20);
    f.travel.update(f.state.scene, 1, 0.0);
    GIANT_CHECK(f.run("send_person", {{"id", third}, {"to", at(0, 0, -20)}, {"speed", 5.0}}).ok &&
                f.run("send_person", {{"id", second}, {"to", at(0, 0, -20)}, {"speed", 5.0}}).ok);
    f.to(0.0);
    f.to(1.0); // (each trip looked at: where its person is in the list is kept)
    GIANT_CHECK(f.travel.stats().trips == 2 && f.travel.trip(third)->phase == PeopleTravel::Phase::travelling);
    // One before them goes: both are found one place down the list, and travel on.
    GIANT_CHECK(f.run("remove_people", {{"ids", {first}}}).ok);
    f.to(2.0);
    GIANT_CHECK(f.travel.stats().trips == 2 && f.travel.trip(second) != nullptr && f.travel.trip(third) != nullptr &&
                f.travel.trip(third)->phase == PeopleTravel::Phase::travelling);
    // The one whose place the other now has goes: only its trip ends.
    GIANT_CHECK(f.run("remove_people", {{"ids", {second}}}).ok);
    f.to(3.0);
    GIANT_CHECK(f.travel.stats().trips == 1 && f.travel.trip(second) == nullptr && f.travel.trip(third) != nullptr);
    // Put back (undo): the list is as it was, and the trip that is left is still its own person's.
    GIANT_CHECK(f.run("undo").ok && f.run("undo").ok);
    f.to(4.0);
    GIANT_CHECK(f.travel.stats().trips == 1 && f.travel.trip(third) != nullptr && f.travel.trip(first) == nullptr);
    GIANT_CHECK(f.run("remove_people", {{"ids", {third}}}).ok);
    f.to(5.0);
    GIANT_CHECK(f.travel.stats().trips == 0);
}

GIANT_TEST("travel: no way, a person gone, roads changed: each trip ends saying why, the rest go on") {
    Fixture f;
    // A second road nothing joins to the first.
    GIANT_CHECK(f.run("create_route", {{"points", {at(0, 0, 5000), at(100, 0, 5000)}}}).ok);
    const world::PersonId a = f.person(90, -10), b = f.person(60, 5), c = f.person(100, 20);
    f.travel.update(f.state.scene, 1, 0.0);
    // Roads that nowhere join are known at once: no search is spent on them.
    ai::CommandResult r = f.run("send_person", {{"id", a}, {"to", at(100, 0, 5010)}});
    GIANT_CHECK(!r.ok && r.error.find("no way") == 0 && f.travel.trip(a) == nullptr);
    GIANT_CHECK(f.run("send_person", {{"id", b}, {"to", "mill"}}).ok && f.run("send_person", {{"id", c}, {"to", at(0, 0, -20)}}).ok);
    // b is removed before its way is found; the roads change while c waits (its goal's node goes).
    GIANT_CHECK(f.run("remove_people", {{"ids", {b}}}).ok && f.run("remove_route_nodes", {{"ids", {1}}}).ok);
    f.to(0.0);
    f.to(1.0);
    r = f.run("get_travel", {{"id", c}});
    GIANT_CHECK(f.travel.trip(b) == nullptr && r.value["trips"] == 1 && r.value["failed"] == 1 && r.value["searching"] == 0);
    GIANT_CHECK(r.value["trip"]["phase"] == "failed" && r.value["trip"]["why"].get<std::string>().find("roads changed") != std::string::npos);
    // Sent again, c goes by the roads as they are now: the one node near both ends is no help, so straight.
    GIANT_CHECK(f.run("send_person", {{"id", c}, {"to", at(0, 0, -20)}, {"speed", 5.0}}).ok && f.near(c, 100, 0, 20));
    f.to(1.0);
    f.to(2.0);
    const DVec3 now = f.where(c);
    GIANT_CHECK(f.run("get_travel", {{"id", c}}).value["trip"]["phase"] == "travelling");
    GIANT_CHECK(std::fabs(std::hypot(now.x - 100.0, now.z - 20.0) - 5.0) < 1e-6 && now.x < 100.0 && now.z < 20.0);
    // A travelling person removed: its trip goes with it.
    GIANT_CHECK(f.run("remove_people", {{"ids", {c}}}).ok);
    f.to(3.0);
    GIANT_CHECK(f.travel.trip(c) == nullptr && f.run("get_travel").value["trips"] == 0);
    // One at rest where it travelled to, removed: it is no longer kept there.
    GIANT_CHECK(f.run("send_person", {{"id", a}, {"to", at(90, 0, -5)}}).ok);
    f.to(100.0);
    GIANT_CHECK(f.near(a, 90, 0, -5) && f.travel.stats().moved >= 1 && f.run("remove_people", {{"ids", {a}}}).ok);
    for (int i = 0; i < 64 && f.travel.stats().moved != 0; ++i) f.to(100.0);
    GIANT_CHECK(f.travel.stats().moved == 0);
    // Another scene: nobody travels in it.
    GIANT_CHECK(f.run("new_scene").ok);
    f.to(101.0);
    GIANT_CHECK(f.travel.stats().trips == 0 && f.travel.running());
}

GIANT_TEST("travel: one with an entity near the camera stands; it is made where the person has got to") {
    Fixture f;
    PeopleState people;
    people.travel = &f.travel;
    f.state.people = &people;
    world::Scene& scene = f.state.scene;
    const world::PersonId id = f.person(0, -10);
    GIANT_CHECK(f.run("set_people_settings", {{"embody_distance", 25.0}, {"release_distance", 35.0}}).ok);
    f.travel.update(scene, 1, 0.0);
    people.update(scene, {0.0, 0.0, 0.0}, nullptr);
    const world::EntityId entity = people.entity_of(scene, id);
    GIANT_CHECK(entity != 0 && scene.find(entity)->transform.position == DVec3(0.0, 0.0, -10.0));
    // Sent while it has an entity: its way is found, but it stands.
    ai::CommandResult r = f.run("send_person", {{"id", id}, {"to", "mill"}, {"speed", 2.0}});
    GIANT_CHECK(r.ok && r.value["trip"]["embodied"] == true);
    for (int i = 0; i < 4; ++i) {
        f.travel.update(scene, 1, 10.0 * i);
        people.update(scene, {0.0, 0.0, 0.0}, nullptr);
    }
    GIANT_CHECK(f.near(id, 0, 0, -10) && f.run("get_travel", {{"id", id}}).value["trip"]["phase"] == "travelling");
    GIANT_CHECK(f.run("get_travel", {{"id", id}}).value["trip"]["gone"] == 0.0);
    // The camera leaves: the entity goes, and the person sets off from then (30 s in), 2 m/s.
    people.update(scene, {0.0, 0.0, -500.0}, nullptr);
    GIANT_CHECK(people.entity_of(scene, id) == 0 && scene.size() == 0);
    f.travel.update(scene, 1, 60.0); // 60 m: 50 m along the road
    GIANT_CHECK(f.near(id, 50, 0, 0) && f.run("get_travel", {{"id", id}}).value["trip"]["embodied"] == false);
    // The camera is by the road ahead: the entity is made where the person is, facing along the road.
    people.update(scene, {60.0, 0.0, 0.0}, nullptr);
    const world::Entity* made = scene.find(people.entity_of(scene, id));
    GIANT_CHECK(made != nullptr && made->transform.position == DVec3(50.0, 0.0, 0.0));
    GIANT_CHECK(made != nullptr && std::fabs(made->transform.rotation.y - std::sin(degrees_to_radians(45.0f))) < 1e-4f);
    f.travel.update(scene, 1, 100.0);
    people.update(scene, {60.0, 0.0, 0.0}, nullptr);
    GIANT_CHECK(f.near(id, 50, 0, 0) && people.entity_of(scene, id) != 0 && people.stats().made == 2); // standing: the same entity
    // The simulation stops: the person is back where the scene has it, so that entity goes.
    f.travel.update(scene, 0, 0.0);
    people.update(scene, {60.0, 0.0, 0.0}, nullptr);
    GIANT_CHECK(f.near(id, 0, 0, -10) && people.entity_of(scene, id) == 0 && scene.size() == 0);
    people.update(scene, {0.0, 0.0, 0.0}, nullptr);
    GIANT_CHECK(people.entity_of(scene, id) != 0 && scene.find(people.entity_of(scene, id))->transform.position == DVec3(0.0, 0.0, -10.0));
}

GIANT_TEST("travel: thousands sent at once have their ways found over frames and all arrive") {
    Fixture f;
    world::Scene& scene = f.state.scene;
    // A 40 x 40 grid of paths 50 m apart, well away from the fixture's road.
    world::Routes& routes = scene.routes;
    const u32 side = 40, base = routes.next_id;
    for (u32 z = 0; z < side; ++z) {
        for (u32 x = 0; x < side; ++x) routes.nodes.push_back({base + z * side + x, {10000.0 + x * 50.0, 0.0, z * 50.0}});
    }
    for (u32 z = 0; z < side; ++z) {
        for (u32 x = 0; x < side; ++x) {
            world::RouteLink l;
            l.a = base + z * side + x;
            if (x + 1 < side) {
                l.b = l.a + 1;
                routes.links.push_back(l);
            }
            if (z + 1 < side) {
                l.b = l.a + side;
                routes.links.push_back(l);
            }
        }
    }
    routes.next_id = base + side * side;
    GIANT_CHECK(!world::check_routes(routes));
    Json batch = Json::array();
    const int count = 600;
    for (int i = 0; i < count; ++i) batch.push_back({{"look", "villager"}, {"position", at(10000.0 + (i % 40) * 50.0, 0, (i / 40 % 40) * 50.0 + 3.0)}});
    const ai::CommandResult made = f.run("create_people", {{"people", batch}});
    GIANT_CHECK(made.ok);
    f.travel.update(scene, 1, 0.0);
    const DVec3 corner{10000.0 + 39 * 50.0, 0.0, 39 * 50.0 + 5.0};
    for (const Json& id : made.value["ids"]) {
        GIANT_CHECK(f.travel.send(scene, id.get<world::PersonId>(), corner, 5.0f, 500.0).empty());
    }
    // A frame's budget finds only some; the rest wait their turn.
    f.travel.update(scene, 1, 0.0);
    PeopleTravel::Stats s = f.travel.stats();
    GIANT_CHECK(s.trips == count && s.searching > 0 && s.searches >= 1 && s.searches <= 16);
    u32 frames = 1;
    for (; f.travel.stats().searching > 0 && frames < 100000; ++frames) f.travel.update(scene, 1, 0.0);
    s = f.travel.stats();
    GIANT_CHECK(s.searching == 0 && s.failed == 0 && frames > 100 && s.opened > 0);
    // The farthest has under 4 km to go at 5 m/s; arrivals are noticed a share a frame.
    f.travel.settings.looked_a_frame = 200;
    f.travel.update(scene, 1, 1000.0);
    GIANT_CHECK(f.travel.stats().arrived == 200);
    for (int i = 0; i < 4; ++i) f.travel.update(scene, 1, 1000.0);
    s = f.travel.stats();
    GIANT_CHECK(s.arrived == count && s.trips == 0 && s.moved == count);
    GIANT_CHECK(f.near(made.value["ids"][0].get<world::PersonId>(), corner.x, corner.y, corner.z));
}

GIANT_TEST("travel: a way of no length keeps the way one faced; one standing keeps its entity when sent") {
    Fixture f;
    PeopleState people;
    people.travel = &f.travel;
    f.state.people = &people;
    world::Scene& scene = f.state.scene;
    const ai::CommandResult made = f.run("create_people", {{"people", {{{"look", "villager"}, {"position", at(-10, 0, 0)}, {"yaw", 123.0}}}}});
    GIANT_CHECK(made.ok);
    const world::PersonId id = made.value["ids"][0].get<world::PersonId>();
    const world::Person& person = *world::find_person(scene.people, id);
    f.travel.update(scene, 1, 0.0);
    // Sent to where it is, and straight up: it faces as it did, on the way and there.
    GIANT_CHECK(f.run("send_person", {{"id", id}, {"to", at(-10, 0, 0)}}).ok && f.travel.where(person).yaw == 123.0f);
    f.to(1.0);
    GIANT_CHECK(f.travel.trip(id) == nullptr && f.near(id, -10, 0, 0) && f.travel.where(person).yaw == 123.0f);
    GIANT_CHECK(f.run("send_person", {{"id", id}, {"to", at(-10, 4, 0)}, {"speed", 1.0}}).ok);
    f.to(3.0);
    GIANT_CHECK(f.near(id, -10, 2, 0) && f.travel.where(person).yaw == 123.0f);
    f.travel.update(scene, 2, 0.0); // (another run: back at its start)
    // With an entity (facing 123 degrees), sent east along the road: the entity stays, the person stands.
    people.update(scene, {0.0, 0.0, 0.0}, nullptr);
    const world::EntityId entity = people.entity_of(scene, id);
    GIANT_CHECK(entity != 0 && f.run("send_person", {{"id", id}, {"to", "mill"}}).ok);
    for (int i = 1; i <= 6; ++i) {
        f.travel.update(scene, 2, 5.0 * i);
        people.update(scene, {0.0, 0.0, 0.0}, nullptr);
    }
    ai::CommandResult r = f.run("get_travel", {{"id", id}});
    GIANT_CHECK(r.value["trip"]["phase"] == "travelling" && r.value["trip"]["gone"] == 0.0 && r.value["trip"]["arrives_in"].is_null());
    GIANT_CHECK(people.entity_of(scene, id) == entity && people.stats().made == 1 && people.stats().released == 0);
    // Stopped while it stands, then let go by the camera: no trip, and it has not moved.
    GIANT_CHECK(f.run("stop_person", {{"id", id}}).ok);
    people.update(scene, {0.0, 0.0, -900.0}, nullptr);
    f.to(100.0);
    GIANT_CHECK(people.entity_of(scene, id) == 0 && f.travel.trip(id) == nullptr && f.near(id, -10, 0, 0));
}

GIANT_TEST("travel: stopped, sent again or the roads changed while its way is looked for, the right way is found") {
    Fixture f;
    world::Scene& scene = f.state.scene;
    // A 60 x 60 grid of paths 50 m apart, away from the fixture's road; one slice of search a frame.
    world::Routes& routes = scene.routes;
    const u32 side = 60, base = routes.next_id;
    for (u32 z = 0; z < side; ++z) {
        for (u32 x = 0; x < side; ++x) routes.nodes.push_back({base + z * side + x, {10000.0 + x * 50.0, 0.0, z * 50.0}});
    }
    for (u32 z = 0; z < side; ++z) {
        for (u32 x = 0; x < side; ++x) {
            world::RouteLink l;
            l.a = base + z * side + x;
            if (x + 1 < side) {
                l.b = l.a + 1;
                routes.links.push_back(l);
            }
            if (z + 1 < side) {
                l.b = l.a + side;
                routes.links.push_back(l);
            }
        }
    }
    routes.next_id = base + side * side;
    GIANT_CHECK(!world::check_routes(routes));
    f.travel.settings.search_work_a_frame = 256;
    const world::PersonId a = f.person(10000, 3), b = f.person(10050, 3);
    const DVec3 far{10000.0 + 59 * 50.0, 0.0, 59 * 50.0 + 5.0}, side_end{10000.0 + 59 * 50.0, 0.0, 5.0};
    const auto frames = [&](int n) {
        for (int i = 0; i < n; ++i) f.travel.update(scene, 1, 0.0);
    };
    f.travel.update(scene, 1, 0.0);
    GIANT_CHECK(f.travel.send(scene, a, far, 5.0f, 500.0).empty() && f.travel.send(scene, b, far, 5.0f, 500.0).empty());
    frames(3); // a's search is under way
    GIANT_CHECK(f.travel.stats().searches == 1 && f.travel.stats().searching == 2);
    // a is sent elsewhere meanwhile: the search under way is nobody's, b's comes next, then a's new one.
    GIANT_CHECK(f.travel.send(scene, a, side_end, 5.0f, 500.0).empty() && f.travel.stats().searching == 2);
    frames(2);
    // The roads change while b's is looked for (a node far off goes): it begins again in the new graph.
    GIANT_CHECK(f.run("remove_route_nodes", {{"ids", {base + 30 * side + 30}}}).ok);
    for (int i = 0; i < 20000 && f.travel.stats().searching > 0; ++i) frames(1);
    const PeopleTravel::Trip* ta = f.travel.trip(a);
    const PeopleTravel::Trip* tb = f.travel.trip(b);
    GIANT_CHECK(ta != nullptr && ta->phase == PeopleTravel::Phase::travelling && ta->to == side_end);
    GIANT_CHECK(ta != nullptr && std::fabs(PeopleTravel::length(*ta) - (3.0 + 59 * 50.0 + 5.0)) < 1e-6); // along the first row
    GIANT_CHECK(tb != nullptr && tb->phase == PeopleTravel::Phase::travelling && tb->to == far);
    // (117 links whichever way round; a few metres less when it cuts the first corner.)
    GIANT_CHECK(tb != nullptr && PeopleTravel::length(*tb) > 5850.0 && PeopleTravel::length(*tb) < 5858.0 + 1e-6);
    GIANT_CHECK(f.travel.stats().searches >= 4 && f.travel.stats().failed == 0);
    // Stopped while its way is looked for: no trip, and the next search is not taken for it.
    GIANT_CHECK(f.travel.send(scene, a, far, 5.0f, 500.0).empty());
    frames(3);
    GIANT_CHECK(f.travel.stop(a) && f.travel.stats().searching == 0 && f.travel.send(scene, b, side_end, 5.0f, 500.0).empty());
    for (int i = 0; i < 20000 && f.travel.stats().searching > 0; ++i) frames(1);
    GIANT_CHECK(f.travel.trip(a) == nullptr && f.travel.trip(b)->to == side_end && f.travel.stats().trips == 1);
}

GIANT_TEST("travel: more waiting for their ways than the list is trimmed at all set off in turn") {
    Fixture f;
    world::Scene& scene = f.state.scene;
    Json batch = Json::array();
    const int count = 1200;
    for (int i = 0; i < count; ++i) batch.push_back({{"look", "villager"}, {"position", at(i * 0.01, 0, -10)}});
    const ai::CommandResult made = f.run("create_people", {{"people", batch}});
    GIANT_CHECK(made.ok);
    f.travel.update(scene, 1, 0.0);
    const world::Place& mill = scene.places[0];
    for (const Json& id : made.value["ids"]) {
        GIANT_CHECK(f.travel.send(scene, id.get<world::PersonId>(), mill.position, 2.0f, 500.0).empty());
    }
    // Some are sent twice (named twice in the list): each still has one trip.
    for (int i = 0; i < 50; ++i) {
        GIANT_CHECK(f.travel.send(scene, made.value["ids"][static_cast<usize>(i)].get<world::PersonId>(), mill.position, 2.0f, 500.0).empty());
    }
    GIANT_CHECK(f.travel.stats().trips == count && f.travel.stats().searching == count);
    for (int i = 0; i < 2000 && f.travel.stats().searching > 0; ++i) f.travel.update(scene, 1, 0.0);
    PeopleTravel::Stats s = f.travel.stats();
    GIANT_CHECK(s.searching == 0 && s.failed == 0 && s.trips == count);
    GIANT_CHECK(s.searches == 1 && s.reused == count - 1); // (all join and leave the road at the same nodes: one way found)
    for (int i = 0; i < 4; ++i) f.travel.update(scene, 1, 1000.0);
    s = f.travel.stats();
    GIANT_CHECK(s.arrived == count && s.trips == 0 && s.moved == count);
}

GIANT_TEST("travel: a command goes by the physics' clock as it is now, not as the last frame had it") {
    Fixture f;
    PhysicsState physics;
    f.state.physics = &physics;
    register_physics_commands(f.commands, f.state);
    const world::PersonId id = f.person(0, -10);
    // Simulating since the last frame: a person can be sent at once.
    GIANT_CHECK(!f.run("send_person", {{"id", id}, {"to", at(0, 0, -30)}}).ok);
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok && f.run("step_physics", {{"seconds", 2.0}}).ok);
    ai::CommandResult r = f.run("send_person", {{"id", id}, {"to", at(0, 0, -30)}, {"speed", 2.0}});
    GIANT_CHECK(r.ok && r.value["trip"]["phase"] == "travelling" && r.value["trip"]["gone"] == 0.0); // from now, not from 2 s ago
    r = f.run("get_travel");
    GIANT_CHECK(r.value["running"] == true && std::fabs(r.value["seconds"].get<double>() - 2.0) < 0.02);
    // Time stepped on by command moves it, with no frame between; stopped, it is where it has got to.
    GIANT_CHECK(f.run("step_physics", {{"seconds", 3.0}}).ok);
    r = f.run("get_people", {{"ids", {id}}});
    GIANT_CHECK(std::fabs(r.value["people"][0]["at"][2].get<double>() + 16.0) < 0.05);
    GIANT_CHECK(f.run("step_physics", {{"seconds", 1.0}}).ok && f.run("stop_person", {{"id", id}}).ok);
    r = f.run("get_travel", {{"id", id}});
    GIANT_CHECK(std::fabs(r.value["at"][2].get<double>() + 18.0) < 0.05 && r.value["trip"].is_null());
    // The frame's clock is the same one.
    const PeopleTravel::Clock now = PeopleTravel::clock_of(physics);
    f.travel.update(f.state.scene, now.run, now.seconds);
    GIANT_CHECK(now.run != 0 && std::fabs(f.where(id).z + 18.0) < 0.05 && f.travel.seconds() == now.seconds);
    // The simulation stopped: by the next command everyone is back.
    GIANT_CHECK(f.run("simulate", {{"on", false}}).ok);
    r = f.run("get_people", {{"ids", {id}}});
    GIANT_CHECK(r.value["people"][0]["at"] == at(0, 0, -10) && f.run("get_travel").value["running"] == false);
}

GIANT_TEST("travel: roads cut after one was sent cost its trip no search, and trips ended cost the rest no slices") {
    Fixture f;
    world::Scene& scene = f.state.scene;
    const world::PersonId a = f.person(0, -10);
    f.travel.settings.search_work_a_frame = 256; // a slice a frame
    f.travel.update(scene, 1, 0.0);
    // Many sent and stopped again before their turn, then a.
    Json batch = Json::array();
    for (int i = 0; i < 200; ++i) batch.push_back({{"look", "villager"}, {"position", at(2 + i * 0.01, 0, -10)}});
    const ai::CommandResult made = f.run("create_people", {{"people", batch}});
    GIANT_CHECK(made.ok);
    for (const Json& id : made.value["ids"]) {
        GIANT_CHECK(f.travel.send(scene, id.get<world::PersonId>(), {100.0, 0.0, 110.0}, 2.0f, 500.0).empty());
        GIANT_CHECK(f.travel.stop(id.get<world::PersonId>()));
    }
    GIANT_CHECK(f.travel.send(scene, a, {100.0, 0.0, 110.0}, 2.0f, 500.0).empty());
    // The road's last link is cut: a's goal is in a part nothing joins to where it is.
    GIANT_CHECK(f.run("unlink_route_nodes", {{"a", 2}, {"b", 3}}).ok);
    f.travel.update(scene, 1, 0.0); // the stopped ones are passed over and a's turn comes in the one frame: failed, no search
    const PeopleTravel::Trip* ta = f.travel.trip(a);
    GIANT_CHECK(ta != nullptr && ta->phase == PeopleTravel::Phase::failed && std::string(ta->reason).find("no way") == 0);
    GIANT_CHECK(f.travel.stats().searches == 0 && f.travel.stats().failed == 1 && f.travel.stats().searching == 0);
}

namespace {

// The people's world with physics, walkers and the people near the camera, stepped as frames are.
struct Walking {
    Fixture f;
    PhysicsState physics;
    NavState nav{nullptr, nullptr, nullptr};
    WalkerState walkers{physics, nav};
    PeopleState people;
    world::PersonId id = 0;
    DVec3 camera{6.0, 0.0, -494.0};
    Walking() {
        f.state.physics = &physics;
        f.state.nav = &nav;
        f.state.walkers = &walkers;
        f.state.people = &people;
        people.travel = &f.travel;
        people.walkers = &walkers;
        register_physics_commands(f.commands, f.state);
        connect_walkers(f.state);
        // A floor, a path east 12 m then north 12 m on it, and someone 2 m before its start.
        GIANT_CHECK(f.run("create_entity", {{"shape", "plane"}, {"position", at(6, 0, -494)}, {"scale", {80.0, 1.0, 80.0}}}).ok);
        GIANT_CHECK(f.run("create_route", {{"points", {at(0, 0, -500), at(12, 0, -500), at(12, 0, -488)}}}).ok);
        id = f.person(-2, -500);
        GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
        frame(0.0);
    }
    // A frame: the travel's clock, the people near the camera, then the physics stepped on.
    void frame(double seconds) {
        const PeopleTravel::Clock now = PeopleTravel::clock_of(physics);
        f.travel.update(f.state.scene, now.run, now.seconds);
        people.update(f.state.scene, camera, nullptr);
        if (seconds > 0.0) GIANT_CHECK(f.run("step_physics", {{"seconds", seconds}}).ok);
    }
    world::EntityId entity() { return people.entity_of(f.state.scene, id); }
    double travel_seconds() { return f.travel.seconds(); }
    DVec3 feet() { return walkers.feet_of(f.state.scene, world::person_entity_id(id)).value_or(DVec3{}); }
};

} // namespace

GIANT_TEST("travel: one with an entity walks its trip's way over the ground, round the corner, and stands at the end") {
    Walking w;
    Fixture& f = w.f;
    GIANT_CHECK(w.entity() != 0 && w.walkers.size() == 0);
    GIANT_CHECK(f.run("send_person", {{"id", w.id}, {"to", at(12, 0, -486)}, {"speed", 3.0}}).ok);
    for (int i = 0; i < 8 && w.walkers.size() == 0; ++i) w.frame(0.05);
    GIANT_CHECK(w.walkers.size() == 1 && w.people.stats().walking == 1);
    // It walks: 3 m/s east along the path, the trip going on as far as it has walked (not by the clock).
    for (int i = 0; i < 8; ++i) w.frame(0.25);
    DVec3 feet = w.feet();
    ai::CommandResult r = f.run("get_travel", {{"id", w.id}});
    const double gone = r.value["trip"]["gone"].get<double>();
    GIANT_CHECK(feet.x > 1.0 && feet.x < 6.0 && std::fabs(feet.z + 500.0) < 0.5 && std::fabs(feet.y) < 0.2);
    GIANT_CHECK(std::fabs(gone - (feet.x + 2.0)) < 0.8 && r.value["trip"]["embodied"] == true);
    GIANT_CHECK(std::fabs(r.value["at"][0].get<double>() - feet.x) < 0.8); // the person is where its entity walks
    // Round the corner and to the end: there it stands, the scene's entity where it walked to, no walker.
    for (int i = 0; i < 80 && f.travel.trip(w.id) != nullptr; ++i) w.frame(0.25);
    r = f.run("get_travel", {{"id", w.id}});
    GIANT_CHECK(r.value["trip"].is_null() && r.value["arrived"] == 1);
    GIANT_CHECK(std::fabs(r.value["at"][0].get<double>() - 12.0) < 0.6 && std::fabs(r.value["at"][2].get<double>() + 486.0) < 0.6);
    const world::Entity* e = f.state.scene.find(w.entity());
    GIANT_CHECK(e != nullptr && std::fabs(e->transform.position.x - 12.0) < 0.6 && std::fabs(e->transform.position.z + 486.0) < 0.6);
    GIANT_CHECK(w.walkers.size() == 0 && w.people.stats().walking == 0 && w.people.stats().made == 1);
    for (int i = 0; i < 4; ++i) w.frame(0.25);
    GIANT_CHECK(w.entity() != 0 && w.people.stats().made == 1 && w.people.stats().released == 0); // it stays
}

GIANT_TEST("travel: one given its entity part way along its trip begins at its pace, not from a standstill") {
    Walking w;
    Fixture& f = w.f;
    // From rest by the camera: its walker begins standing.
    GIANT_CHECK(f.run("send_person", {{"id", w.id}, {"to", at(12, 0, -486)}, {"speed", 3.0}}).ok);
    for (int i = 0; i < 8 && w.walkers.size() == 0; ++i) w.frame(0.0);
    const WalkerState::Walker* walker = w.walkers.find(world::person_entity_id(w.id));
    GIANT_CHECK(walker != nullptr && walker->velocity.x == 0.0f && walker->velocity.z == 0.0f);
    const DVec3 rest = w.feet();
    w.frame(1.0 / 60.0);
    w.frame(0.0);
    GIANT_CHECK(w.feet().x - rest.x < 0.02); // (a step on: it has barely begun to move)
    // The camera away: it goes on by the clock, 2 s at 3 m/s. The camera back: its entity is made
    // where it has got to and its walker begins moving along the way at its pace.
    w.camera = {6.0, 0.0, 400.0};
    w.frame(0.0);
    GIANT_CHECK(w.entity() == 0 && w.walkers.size() == 0);
    w.frame(2.0);
    w.camera = {6.0, 0.0, -494.0};
    for (int i = 0; i < 8 && w.walkers.size() == 0; ++i) w.frame(0.0);
    walker = w.walkers.find(world::person_entity_id(w.id));
    GIANT_CHECK(walker != nullptr && std::fabs(walker->velocity.x - 3.0f) < 1e-3f && std::fabs(walker->velocity.z) < 1e-3f);
    const DVec3 from = w.feet();
    GIANT_CHECK(from.x > 2.0 && from.x < 8.0);
    w.frame(1.0 / 60.0);
    w.frame(0.0);
    GIANT_CHECK(w.feet().x - from.x > 0.04); // (a step on: 5 cm, its pace)
}

GIANT_TEST("travel: one walking is stopped where it stands; left by the camera it goes on by the clock from there") {
    Walking w;
    Fixture& f = w.f;
    GIANT_CHECK(f.run("send_person", {{"id", w.id}, {"to", at(12, 0, -486)}, {"speed", 2.0}}).ok);
    for (int i = 0; i < 12; ++i) w.frame(0.25);
    const DVec3 walked = w.feet();
    GIANT_CHECK(w.walkers.size() == 1 && walked.x > 1.0 && walked.x < 6.0);
    // Stopped: the walker is let go and the entity stands where it walked to.
    GIANT_CHECK(f.run("stop_person", {{"id", w.id}}).ok);
    w.frame(0.25);
    const world::Entity* e = f.state.scene.find(w.entity());
    GIANT_CHECK(w.walkers.size() == 0 && e != nullptr && std::fabs(e->transform.position.x - walked.x) < 0.3);
    GIANT_CHECK(std::fabs(f.where(w.id).x - walked.x) < 0.8 && w.people.stats().made == 1);
    // Sent on, it walks again; the camera leaves: no entity, no walker, and the trip goes on by the clock.
    GIANT_CHECK(f.run("send_person", {{"id", w.id}, {"to", at(12, 0, -486)}, {"speed", 2.0}}).ok);
    for (int i = 0; i < 8; ++i) w.frame(0.25);
    const double before = f.run("get_travel", {{"id", w.id}}).value["trip"]["gone"].get<double>();
    GIANT_CHECK(w.walkers.size() == 1 && before > 1.0);
    w.camera = {6.0, 0.0, 400.0};
    w.frame(0.0);
    GIANT_CHECK(w.entity() == 0 && w.walkers.size() == 0 && f.run("get_travel", {{"id", w.id}}).value["trip"]["embodied"] == false);
    w.frame(1.0);
    w.frame(0.0);
    const double after = f.run("get_travel", {{"id", w.id}}).value["trip"]["gone"].get<double>();
    GIANT_CHECK(std::fabs(after - before - 2.0) < 0.1);
    // The simulation stops: everyone is back where the scene has them, entities too.
    w.camera = {6.0, 0.0, -494.0};
    GIANT_CHECK(f.run("simulate", {{"on", false}}).ok);
    for (int i = 0; i < 3; ++i) w.frame(0.0);
    e = f.state.scene.find(w.entity());
    GIANT_CHECK(e != nullptr && e->transform.position == DVec3(-2.0, 0.0, -500.0) && w.walkers.size() == 0);
}

GIANT_TEST("travel: a long way is walked without slowing where the next stretch is given; sent elsewhere it turns at once") {
    Walking w;
    Fixture& f = w.f;
    // A straight path 120 m east from the first's start (joined to it), and the camera following.
    GIANT_CHECK(f.run("create_entity", {{"shape", "plane"}, {"position", at(60, 0, -520)}, {"scale", {200.0, 1.0, 20.0}}}).ok);
    GIANT_CHECK(f.run("create_route", {{"points", {at(0, 0, -500), at(0, 0, -520), at(120, 0, -520)}}}).ok);
    w.frame(0.0);
    GIANT_CHECK(f.run("send_person", {{"id", w.id}, {"to", at(120, 0, -520)}, {"speed", 6.0}}).ok);
    double slowest = 100.0, last_x = 0.0;
    int renewals = 0;
    double path_end = 0.0;
    for (int i = 0; i < 320 && f.travel.trip(w.id) != nullptr; ++i) {
        w.camera = w.feet();
        w.frame(0.1);
        const WalkerState::Walker* walker = w.walkers.find(world::person_entity_id(w.id));
        const DVec3 feet = w.feet();
        if (walker != nullptr && feet.x > 10.0 && feet.x < 110.0) { // on the long stretch, clear of its ends
            slowest = std::min(slowest, static_cast<double>(std::hypot(walker->velocity.x, walker->velocity.z)));
            const double end = walker->follow.path().empty() ? 0.0 : walker->follow.path().back().x;
            if (end > path_end + 1.0) ++renewals;
            path_end = std::max(path_end, end);
        }
        last_x = feet.x;
    }
    GIANT_CHECK(f.travel.trip(w.id) == nullptr && std::fabs(last_x - 120.0) < 0.6 && f.travel.stats().arrived == 1);
    GIANT_CHECK(renewals >= 2 && slowest > 5.0); // given its way on at least twice, never slowed for it
    GIANT_CHECK(w.people.stats().made == 1);
    // Sent back west, and two seconds on sent east again past where it was going: it turns at once.
    GIANT_CHECK(f.run("send_person", {{"id", w.id}, {"to", at(60, 0, -520)}, {"speed", 6.0}}).ok);
    for (int i = 0; i < 20; ++i) {
        w.camera = w.feet();
        w.frame(0.1);
    }
    const double west = w.feet().x;
    GIANT_CHECK(west < 112.0 && west > 100.0 && w.walkers.size() == 1);
    GIANT_CHECK(f.run("send_person", {{"id", w.id}, {"to", at(119, 0, -520)}, {"speed", 6.0}}).ok);
    for (int i = 0; i < 12; ++i) {
        w.camera = w.feet();
        w.frame(0.1);
        GIANT_CHECK(f.travel.trip(w.id) == nullptr || w.feet().x > west - 1.5); // never on west along the old path
    }
    for (int i = 0; i < 60 && f.travel.trip(w.id) != nullptr; ++i) w.frame(0.1);
    GIANT_CHECK(f.travel.trip(w.id) == nullptr && std::fabs(w.feet().x - 119.0) < 0.6 && f.travel.stats().arrived == 2);
}

GIANT_TEST("travel: a way that turns back on itself by a short link is walked through, not stuck at the turn") {
    Walking w;
    Fixture& f = w.f;
    // West 20 m, a 0.4 m link north, then back east 20 m: the turn's corners are nearer than a walker turns at.
    GIANT_CHECK(f.run("create_route", {{"points", {at(-4, 0, -480), at(-24, 0, -480), at(-24, 0, -479.6), at(-4, 0, -479.6)}}, {"join", 0}}).ok);
    const world::PersonId id = f.person(-4, -480); // on the path's start: the way round is the way
    w.camera = {-14.0, 0.0, -480.0};
    w.frame(0.0);
    w.frame(0.0);
    GIANT_CHECK(w.people.entity_of(f.state.scene, id) != 0);
    GIANT_CHECK(f.run("send_person", {{"id", id}, {"to", at(-4, 0, -479.6)}, {"speed", 4.0}, {"within", 5.0}}).ok);
    for (int i = 0; i < 6; ++i) w.frame(0.05);
    const PeopleTravel::Trip* trip = f.travel.trip(id);
    GIANT_CHECK(trip != nullptr && trip->points.size() >= 4 && PeopleTravel::length(*trip) > 40.0); // by the way round, not across
    double walked = 0.0;
    DVec3 last = w.walkers.feet_of(f.state.scene, world::person_entity_id(id)).value_or(DVec3{});
    for (int i = 0; i < 300 && f.travel.trip(id) != nullptr; ++i) {
        w.frame(0.1);
        const DVec3 feet = w.walkers.feet_of(f.state.scene, world::person_entity_id(id)).value_or(last);
        walked += std::hypot(feet.x - last.x, feet.z - last.z);
        last = feet;
    }
    GIANT_CHECK(f.travel.trip(id) == nullptr && f.travel.stats().arrived == 1 && std::fabs(last.x + 4.0) < 0.6);
    GIANT_CHECK(walked < 48.0); // once round (about 42 m), not to and fro
}

GIANT_TEST("travel: many sent at once all walk; a walker taken away is given again later; another run begins afresh") {
    Walking w;
    Fixture& f = w.f;
    Json batch = Json::array();
    for (int i = 0; i < 24; ++i) batch.push_back({{"look", "villager"}, {"position", at(-2, 0, -510.0 + i * 0.9)}});
    const ai::CommandResult made = f.run("create_people", {{"people", batch}});
    GIANT_CHECK(made.ok && f.run("set_people_settings", {{"made_a_frame", 32}}).ok);
    for (int i = 0; i < 3; ++i) w.frame(0.0);
    GIANT_CHECK(w.people.stats().embodied == 25);
    for (const Json& id : made.value["ids"]) {
        GIANT_CHECK(f.run("send_person", {{"id", id}, {"to", at(12, 0, -486)}, {"speed", 0.5}}).ok);
    }
    for (int i = 0; i < 6; ++i) w.frame(0.0); // the ways found (a few a frame), no time passing
    w.frame(0.0);
    GIANT_CHECK(w.people.stats().walking >= 16 && w.walkers.size() >= 16); // sixteen begun a frame
    w.frame(0.05);
    w.frame(0.05);
    GIANT_CHECK(w.people.stats().walking == 24 && w.walkers.size() == 24);
    // Every walker is let go by command: they stand two seconds, then walk on, sixteen begun a frame.
    const world::PersonId one = made.value["ids"][0].get<world::PersonId>();
    for (const Json& id : made.value["ids"]) GIANT_CHECK(w.walkers.stop(world::person_entity_id(id.get<world::PersonId>())));
    w.frame(0.1);
    w.frame(0.1);
    GIANT_CHECK(w.walkers.size() == 0 && w.people.stats().walking == 0);
    for (int i = 0; i < 30 && w.walkers.size() == 0; ++i) w.frame(0.1);
    GIANT_CHECK(w.walkers.size() == 16 && w.people.stats().walking == 16 && w.travel_seconds() > 1.9);
    w.frame(0.1);
    GIANT_CHECK(w.walkers.find(world::person_entity_id(one)) != nullptr && w.people.stats().walking == 24);
    // One hidden has its walker taken and is given none while it is; shown again it walks on.
    const world::EntityId hidden = world::person_entity_id(one);
    f.state.scene.modify(hidden)->visible = false;
    for (int i = 0; i < 40; ++i) w.frame(0.1);
    GIANT_CHECK(w.walkers.find(hidden) == nullptr && w.people.stats().walking == 23 && w.people.entity_of(f.state.scene, one) != 0);
    f.state.scene.modify(hidden)->visible = true;
    for (int i = 0; i < 25; ++i) w.frame(0.1);
    GIANT_CHECK(w.walkers.find(hidden) != nullptr && w.people.stats().walking == 24);
    // Someone new is sent and its walk begun, but no time passes: it has not moved.
    const world::PersonId still = f.person(-6, -500);
    for (int i = 0; i < 3; ++i) w.frame(0.0);
    GIANT_CHECK(f.run("send_person", {{"id", still}, {"to", at(12, 0, -486)}}).ok);
    for (int i = 0; i < 3; ++i) w.frame(0.0);
    GIANT_CHECK(w.walkers.find(world::person_entity_id(still)) != nullptr);
    const auto made_before = w.people.stats().made;
    // The simulation stopped and started: those who walked are made again where the scene has them;
    // the one who never moved keeps its entity, and walks at once when sent (nothing of the old run is kept).
    GIANT_CHECK(f.run("simulate", {{"on", false}}).ok);
    for (int i = 0; i < 20; ++i) w.frame(0.0);
    GIANT_CHECK(w.walkers.size() == 0 && w.people.stats().embodied == 26 && w.people.stats().walking == 0);
    GIANT_CHECK(w.people.stats().made == made_before + 24);
    GIANT_CHECK(f.run("simulate", {{"on", true}}).ok);
    w.frame(0.0);
    GIANT_CHECK(f.run("send_person", {{"id", still}, {"to", at(12, 0, -486)}}).ok);
    for (int i = 0; i < 4; ++i) w.frame(0.05);
    GIANT_CHECK(w.walkers.find(world::person_entity_id(still)) != nullptr && w.people.stats().walking == 1);
    GIANT_CHECK(w.people.stats().made == made_before + 24);
}


GIANT_TEST("travel: one standing in the road is walked round, not stopped behind; a crowded end is arrived at beside it") {
    Walking w;
    Fixture& f = w.f;
    // Someone standing in the middle of the first stretch, and someone on the very end (walkers, arrived where they stand).
    const auto stander = [&](double x, double z) {
        const ai::CommandResult r = f.run("create_entity", {{"shape", "cube"}, {"position", at(x, 0.9, z)}, {"scale", {0.5, 1.8, 0.5}}, {"body", "none"}});
        GIANT_CHECK(r.ok);
        const world::EntityId e = r.value["id"].get<world::EntityId>();
        const std::vector<DVec3> here{{x, 0.0, z}};
        GIANT_CHECK(w.walkers.walk_to(f.state.scene, e, here.back(), 1.4f, &here).empty());
        return e;
    };
    const world::EntityId in_road = stander(5.0, -500.0);
    const world::EntityId on_end = stander(12.0, -486.0);
    w.frame(0.1);
    GIANT_CHECK(w.walkers.find(in_road)->phase == WalkerState::Phase::arrived && w.walkers.find(on_end)->phase == WalkerState::Phase::arrived);
    GIANT_CHECK(f.run("send_person", {{"id", w.id}, {"to", at(12, 0, -486)}, {"speed", 3.0}}).ok);
    double least = 100.0;
    for (int i = 0; i < 400 && f.travel.trip(w.id) != nullptr; ++i) {
        w.frame(0.05);
        least = std::min(least, std::hypot(w.feet().x - 5.0, w.feet().z + 500.0));
    }
    // There (in well under the time the wait for one stopped short would add), beside the one on the end, having passed the other.
    GIANT_CHECK(f.travel.trip(w.id) == nullptr && f.travel.stats().arrived == 1);
    const DVec3 feet = w.feet();
    const double from_end = std::hypot(feet.x - 12.0, feet.z + 486.0);
    GIANT_CHECK(from_end < 2.0 && from_end > 0.4 && least > 0.4 && least < 2.5);
    GIANT_CHECK(std::fabs(f.where(w.id).x - feet.x) < 0.1 && std::fabs(f.where(w.id).z - feet.z) < 0.1);
}

GIANT_TEST("travel: those far from the camera are passed over until they or it could have come near, and are never late") {
    Fixture f;
    PeopleState people;
    people.travel = &f.travel;
    f.state.people = &people;
    world::Scene& scene = f.state.scene;
    GIANT_CHECK(f.run("create_route", {{"points", {at(3000, 0, 0), at(100, 0, 0)}}, {"kind", "road"}}).ok);
    // (As many made a frame as there are: one near without its entity was not looked at in time.)
    GIANT_CHECK(f.run("set_people_settings", {{"embody_distance", 25.0}, {"release_distance", 35.0}, {"made_a_frame", 64}}).ok);
    const world::PersonId by = f.person(0, -10);
    std::vector<world::PersonId> away;
    for (int i = 0; i < 20; ++i) away.push_back(f.person(3000.0, 2.0 * i));
    DVec3 camera{0.0, 0.0, 0.0};
    double clock = 0.0;
    const auto frame = [&] {
        f.travel.update(scene, 1, clock);
        people.update(scene, camera, nullptr);
    };
    const auto off = [&](world::PersonId id) {
        const DVec3 p = f.where(id);
        return std::sqrt((p.x - camera.x) * (p.x - camera.x) + (p.y - camera.y) * (p.y - camera.y) + (p.z - camera.z) * (p.z - camera.z));
    };
    // Looked at once, the twenty far off are passed over from then on; the one near has its entity.
    frame();
    GIANT_CHECK(people.stats().passed_over == 0 && people.entity_of(scene, by) != 0);
    frame();
    GIANT_CHECK(people.stats().passed_over == 20 && people.stats().embodied == 1);
    GIANT_CHECK(f.run("set_people_settings", nlohmann::json::object()).value["passed_over"] == 20);
    // One of them sent to the camera as fast as anyone may go (faster than anyone was sent: everyone is
    // looked at afresh): whenever it is near, it has its entity (it is looked at again well before),
    // while the rest are still passed over.
    GIANT_CHECK(f.run("send_person", {{"id", away[0]}, {"to", at(0, 0, 0)}, {"speed", 100.0}}).ok);
    frame();
    GIANT_CHECK(people.stats().passed_over == 0);
    bool late = false, passed = false;
    for (int i = 0; i < 140; ++i) {
        clock += 0.25;
        frame();
        late = late || (off(away[0]) <= 25.0 && people.entity_of(scene, away[0]) == 0);
        passed = passed || people.stats().passed_over == 20;
        GIANT_CHECK(people.stats().passed_over >= 19 || off(away[0]) < 2000.0);
    }
    // (Given its entity as it came within reach, it stands there: nothing walks it here.)
    GIANT_CHECK(!late && passed && off(away[0]) <= 25.0 && people.entity_of(scene, away[0]) != 0);
    GIANT_CHECK(people.stats().passed_over == 19);
    // One moved by an edit, and one put somewhere (a loaded game's), are looked at at once.
    GIANT_CHECK(f.run("set_person", {{"id", away[1]}, {"position", at(5, 0, 5)}}).ok);
    frame();
    GIANT_CHECK(people.entity_of(scene, away[1]) != 0);
    frame();
    GIANT_CHECK(people.stats().passed_over == 18);
    GIANT_CHECK(f.travel.put(scene, away[2], {{3.0, 0.0, 3.0}, 0.0f}));
    frame();
    GIANT_CHECK(people.entity_of(scene, away[2]) != 0);
    frame();
    GIANT_CHECK(people.stats().passed_over == 17);
    // One taken from the middle of the list: those after it are others' now, and looked at afresh. So
    // when it is brought back, and when one more is made.
    GIANT_CHECK(f.run("remove_people", {{"ids", {away[10]}}}).ok);
    frame();
    GIANT_CHECK(people.stats().passed_over == 7);
    frame();
    GIANT_CHECK(people.stats().passed_over == 16);
    GIANT_CHECK(f.run("undo").ok);
    frame();
    frame();
    frame(); // (back in its place in the list: those after it are looked at afresh once more)
    GIANT_CHECK(people.stats().passed_over == 17);
    f.person(3000.0, 60.0);
    frame();
    frame();
    GIANT_CHECK(people.stats().passed_over == 18);
    // The camera flown to the rest, 10 m a frame, while one of them comes to meet it as fast (the two
    // close at once): whenever anyone is near, it has its entity.
    GIANT_CHECK(f.run("send_person", {{"id", away[4]}, {"to", at(100, 0, 0)}, {"speed", 100.0}}).ok);
    late = false;
    bool met = false;
    for (int i = 0; i < 300; ++i) {
        camera.x += 10.0;
        clock += 0.1;
        frame();
        for (usize k = 3; k < away.size(); ++k) late = late || (off(away[k]) <= 25.0 && people.entity_of(scene, away[k]) == 0);
        met = met || (camera.x > 1000.0 && camera.x < 2000.0 && people.entity_of(scene, away[4]) != 0);
        if (i % 20 == 0) GIANT_CHECK(people.stats().passed_over > 0 || camera.x > 2000.0);
    }
    GIANT_CHECK(!late && met && people.stats().embodied >= 9 && people.entity_of(scene, away[3]) != 0);
    // Another run, the camera where it was: everyone is where the scene has them, and looked at afresh.
    f.travel.update(scene, 2, 0.0);
    people.update(scene, camera, nullptr);
    GIANT_CHECK(people.stats().passed_over == 0);
    people.update(scene, camera, nullptr);
    GIANT_CHECK(people.entity_of(scene, away[0]) != 0 && people.entity_of(scene, away[4]) != 0);
    GIANT_CHECK(people.entity_of(scene, by) == 0 && people.stats().passed_over == 3); // (the two by where it began, the one 60 m aside)
}

GIANT_TEST("travel: one far off removed and brought back by an undo is looked at afresh; so is everyone when what is near changes") {
    Fixture f;
    PeopleState people;
    people.travel = &f.travel;
    f.state.people = &people;
    world::Scene& scene = f.state.scene;
    GIANT_CHECK(f.run("create_route", {{"points", {at(3000, 0, 0), at(100, 0, 0)}}, {"kind", "road"}}).ok);
    GIANT_CHECK(f.run("set_people_settings", {{"embody_distance", 25.0}, {"release_distance", 35.0}, {"scanned_a_frame", 4}}).ok);
    // One whose home is by the camera, on its way far off (2 km gone); forty who stand far off. Four are looked at a frame.
    const world::PersonId walker = f.person(2, 2);
    for (int i = 0; i < 40; ++i) f.person(3000.0, 2.0 * i);
    const DVec3 camera{0.0, 0.0, 0.0};
    f.travel.update(scene, 1, 0.0);
    GIANT_CHECK(f.run("send_person", {{"id", walker}, {"to", at(3000, 0, 0)}, {"speed", 50.0}}).ok);
    f.to(0.0); // (its way found: it sets off)
    double clock = 40.0;
    const auto frames = [&](int count) {
        for (int i = 0; i < count; ++i) {
            clock += 0.01;
            f.travel.update(scene, 1, clock);
            people.update(scene, camera, nullptr);
        }
    };
    frames(12); // (a pass of eleven frames, and the first four again: passed over)
    GIANT_CHECK(people.stats().passed_over == 4 && people.stats().embodied == 0 && f.where(walker).x > 1500.0);
    // Removed (its trip forgotten), then brought back: it stands at home, and has its entity when the list comes round.
    GIANT_CHECK(f.run("remove_people", {{"ids", {walker}}}).ok);
    frames(1);
    GIANT_CHECK(f.run("undo").ok && f.near(walker, 2, 0, 2));
    frames(14);
    GIANT_CHECK(people.entity_of(scene, walker) != 0);
    frames(11);
    GIANT_CHECK(people.stats().passed_over == 4);
    // The people turned off and on, or entities made at another distance: nobody is passed over until looked at again.
    GIANT_CHECK(f.run("set_people_settings", {{"on", false}}).ok);
    frames(1);
    GIANT_CHECK(people.stats().passed_over == 0 && people.stats().embodied == 0);
    GIANT_CHECK(f.run("set_people_settings", {{"on", true}}).ok);
    frames(1);
    GIANT_CHECK(people.stats().passed_over == 0);
    frames(22);
    GIANT_CHECK(people.stats().passed_over >= 3 && people.entity_of(scene, walker) != 0);
    GIANT_CHECK(f.run("set_people_settings", {{"embody_distance", 30.0}}).ok);
    frames(1);
    GIANT_CHECK(people.stats().passed_over == 0);
}
