// People travelling (people_travel.h): sent along the world's roads by
// command, moved by the simulation's clock, standing while an entity
// stands for them, and back where the scene has them when it stops.
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"
#include "../src/people_state.h"
#include "../src/people_travel.h"
#include "../src/physics_state.h"

#include "giant_test.h"

#include "giant/world/people.h"
#include "giant/world/routes.h"

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
    GIANT_CHECK(r.ok && r.value["trip"]["standing"] == true);
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
    GIANT_CHECK(f.near(id, 50, 0, 0) && f.run("get_travel", {{"id", id}}).value["trip"]["standing"] == false);
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
    GIANT_CHECK(s.searching == 0 && s.failed == 0 && s.trips == count && s.searches == count);
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

