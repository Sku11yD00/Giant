// The People panel's list, the words on what a person does and why, and the
// camera following one (people_list.h).
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"
#include "../src/people_list.h"
#include "../src/people_routines.h"
#include "../src/people_senses.h"
#include "../src/people_travel.h"
#include "../src/physics_state.h"
#include "../src/play_state.h"

#include "giant_test.h"

#include "giant/world/people.h"

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
    PeopleRoutines routines;
    PeopleSenses senses;
    double seconds = 0.0;
    Fixture() {
        state.meshes = &meshes;
        state.travel = &travel;
        state.routines = &routines;
        state.senses = &senses;
        register_scene_commands(commands, state);
        register_people_commands(commands, state);
        register_route_commands(commands, state);
        for (const char* look : {"villager", "guard"}) {
            world::Entity e;
            e.shape = world::Shape::mesh;
            e.mesh = "people/villager.glb";
            const world::EntityId model = state.scene.add(e);
            GIANT_CHECK(run("set_person_look", {{"name", look}, {"entity", model}}).ok);
            GIANT_CHECK(run("delete_entities", {{"ids", {model}}}).ok);
        }
        GIANT_CHECK(run("create_route", {{"points", {at(0, 0, 0), at(100, 0, 0), at(100, 0, 100)}}, {"kind", "road"}}).ok);
        GIANT_CHECK(run("create_place", {{"name", "cottage"}, {"position", at(0, 0, -5)}, {"tags", {"home"}}}).ok);
        GIANT_CHECK(run("create_place", {{"name", "mill"}, {"position", at(100, 0, 110)}, {"radius", 4.0}, {"tags", {"work"}}}).ok);
        GIANT_CHECK(run("set_routine", {{"name", "day"},
                                        {"entries", {{{"from", 6.5}, {"activity", "work"}, {"at", "place"}, {"name", "mill"}, {"speed", 2.0}},
                                                     {{"from", 20}, {"activity", "sleep"}, {"at", "home"}}}},
                                        {"alarmed", {{{"over", 0.5}, {"activity", "hide"}, {"at", "home"}}}}}).ok);
        routines.settings.spread_minutes = 0.0f;
    }
    ai::CommandResult run(const char* name, const Json& args = Json::object()) {
        return commands.execute(name, args, {});
    }
    world::PersonId person(Json fields) {
        if (!fields.contains("look")) fields["look"] = "villager";
        const ai::CommandResult r = run("create_people", {{"people", Json::array({fields})}});
        GIANT_CHECK(r.ok);
        return r.ok ? r.value["ids"][0].get<world::PersonId>() : 0;
    }
    void frames(double hours, double more = 0.0, int count = 4) {
        for (int i = 0; i < count; ++i) {
            seconds += more / count;
            travel.update(state.scene, 1, seconds);
            senses.update(state.scene, travel);
            routines.update(state.scene, travel, hours, &senses);
        }
    }
    const world::Person& of(world::PersonId id) { return *world::find_person(state.scene.people, id); }
    std::string why(world::PersonId id) { return person_why(state, of(id)); }
    std::vector<world::PersonId> listed(const Json& args) {
        std::vector<world::PersonId> out;
        const ai::CommandResult r = run("get_people", args);
        GIANT_CHECK(r.ok);
        if (r.ok) {
            for (const Json& p : r.value["people"]) out.push_back(p["id"].get<world::PersonId>());
        }
        return out;
    }
};

bool has(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

} // namespace

GIANT_TEST("people list: the filter takes names, looks, homes, routines and ids, letters' case aside") {
    Fixture f;
    const world::PersonId anna = f.person({{"name", "Anna Miller"}, {"home", "cottage"}, {"routine", "day"}});
    const world::PersonId bo = f.person({{"name", "Bo"}, {"look", "guard"}, {"position", at(30, 0, 0)}});
    PeopleList::Filter filter;
    const auto match = [&](world::PersonId id) {
        const world::Person& p = f.of(id);
        return PeopleList::matches(f.state, p, static_cast<usize>(&p - f.state.scene.people.data()), filter);
    };
    GIANT_CHECK(match(anna) && match(bo)); // no filter: all
    filter.text = "MILL";
    GIANT_CHECK(match(anna) && !match(bo));
    filter.text = "guard";
    GIANT_CHECK(!match(anna) && match(bo));
    filter.text = "cott";
    GIANT_CHECK(match(anna) && !match(bo));
    filter.text = "day";
    GIANT_CHECK(match(anna) && !match(bo));
    filter.text = std::to_string(bo); // an id: the whole of it, not a part
    GIANT_CHECK(match(bo) && !match(anna));
    filter.text = "nobody such";
    GIANT_CHECK(!match(anna) && !match(bo));
    // The command's the same.
    GIANT_CHECK(f.listed({{"text", "mill"}}) == std::vector<world::PersonId>{anna});
    GIANT_CHECK(f.listed({{"text", "GUARD"}}) == std::vector<world::PersonId>{bo});
    GIANT_CHECK(f.listed({{"text", "nobody such"}}).empty());
}

GIANT_TEST("people list: only the alarmed, only those travelling") {
    Fixture f;
    const world::PersonId near_by = f.person({{"name", "near"}, {"position", at(5, 0, 0)}});
    const world::PersonId far_off = f.person({{"name", "far"}, {"position", at(400, 0, 0)}});
    f.frames(3.0);
    GIANT_CHECK(f.listed({{"alarmed", true}}).empty() && f.listed({{"travelling", true}}).empty());
    GIANT_CHECK(f.run("make_noise", {{"at", at(0, 0, 0)}, {"loudness", 20.0}}).ok);
    f.frames(3.0, 0.5);
    GIANT_CHECK(f.listed({{"alarmed", true}}) == std::vector<world::PersonId>{near_by});
    GIANT_CHECK(f.listed({{"alarmed", true}, {"text", "far"}}).empty()); // both must hold
    GIANT_CHECK(f.run("send_person", {{"id", far_off}, {"to", at(100, 0, 50)}}).ok);
    f.frames(3.0, 0.1);
    GIANT_CHECK(f.listed({{"travelling", true}}) == std::vector<world::PersonId>{far_off});
    GIANT_CHECK(f.listed({{"alarmed", false}, {"travelling", false}}).size() == 2); // false: not asked for
}

GIANT_TEST("people list: thousands are gone through a share a frame; the first that match are shown, and it keeps up") {
    Fixture f;
    Json people = Json::array();
    const usize count = PeopleList::k_looked_a_frame * 2 + 500;
    for (usize i = 0; i < count; ++i) {
        people.push_back({{"look", i % 2 == 0 ? "villager" : "guard"}, {"position", at(static_cast<double>(i), 0, 500)}});
        if (people.size() == 4096 || i + 1 == count) { // (a command makes at most 4096)
            GIANT_CHECK(f.run("create_people", {{"people", people}}).ok);
            people = Json::array();
        }
    }
    GIANT_CHECK(f.state.scene.people.size() == count);
    PeopleList list;
    list.filter.text = "guard";
    list.update(f.state);
    GIANT_CHECK(!list.whole() && list.shown().empty()); // a share only
    list.update(f.state);
    GIANT_CHECK(!list.whole());
    list.update(f.state);
    GIANT_CHECK(list.whole() && list.matching() == count / 2 && list.shown().size() == PeopleList::k_most_shown);
    for (usize i = 1; i < list.shown().size(); ++i) GIANT_CHECK(list.shown()[i - 1] < list.shown()[i]); // ids rising
    for (const world::PersonId id : list.shown()) GIANT_CHECK(f.of(id).look == "guard");
    // What is shown stays while the next time through is under way.
    const world::PersonId* const one = list.shown().data();
    const world::PersonId first = list.shown()[0];
    list.update(f.state);
    GIANT_CHECK(list.whole() && list.shown().size() == PeopleList::k_most_shown && list.shown()[0] == first && list.shown().data() == one);
    // People removed part way through neither hide nor double anyone: the two lowest go (a villager and
    // the first guard, both looked at already), and those after the place it has got to are all still looked at.
    const world::PersonId lowest = f.state.scene.people[0].id;
    GIANT_CHECK(f.state.scene.people[1].id == first && f.run("remove_people", {{"ids", {lowest, first}}}).ok);
    list.update(f.state);
    list.update(f.state);
    GIANT_CHECK(list.whole() && list.matching() == count / 2 && list.shown()[0] == first); // (this time through began before)
    const world::PersonId* const other = list.shown().data();
    GIANT_CHECK(other != one);
    for (usize i = 1; i < list.shown().size(); ++i) GIANT_CHECK(list.shown()[i - 1] < list.shown()[i]);
    // The next time through has it right; put back (undo) part way through, right again a time later.
    for (int i = 0; i < 3; ++i) list.update(f.state);
    GIANT_CHECK(list.shown()[0] != first && list.matching() == count / 2 - 1);
    list.update(f.state);
    GIANT_CHECK(f.run("undo").ok && f.state.scene.people[1].id == first);
    for (int i = 0; i < 2; ++i) list.update(f.state);
    GIANT_CHECK(list.whole() && list.matching() == count / 2 - 1);
    for (usize i = 1; i < list.shown().size(); ++i) GIANT_CHECK(list.shown()[i - 1] < list.shown()[i]);
    for (int i = 0; i < 3; ++i) list.update(f.state);
    GIANT_CHECK(list.shown()[0] == first && list.matching() == count / 2);
    // The same two lists all along: nothing is allocated for a time through.
    GIANT_CHECK(list.shown().data() == one || list.shown().data() == other);
    // Begun again (the panel was not open for a while): empty until a whole time through.
    list.restart();
    GIANT_CHECK(!list.whole() && list.shown().empty() && list.matching() == 0);
    for (int i = 0; i < 3; ++i) list.update(f.state);
    GIANT_CHECK(list.whole() && list.matching() == count / 2 && (list.shown().data() == one || list.shown().data() == other));
    // Another filter begins again at once.
    list.filter.text = "villager";
    list.update(f.state);
    GIANT_CHECK(!list.whole() && list.shown().empty());
    for (int i = 0; i < 3; ++i) list.update(f.state);
    GIANT_CHECK(list.whole() && list.matching() == count - count / 2);
    // A few people: whole in one frame, all shown.
    list.filter.text = std::to_string(list.shown()[3]);
    for (int i = 0; i < 3; ++i) list.update(f.state);
    GIANT_CHECK(list.whole() && list.matching() == 1 && list.shown().size() == 1);
    // Nobody at all.
    GIANT_CHECK(f.run("new_scene").ok);
    list.update(f.state);
    GIANT_CHECK(list.whole() && list.shown().empty() && list.matching() == 0);
}

GIANT_TEST("people list: why a person does what it does, in words, and in get_people") {
    Fixture f;
    const world::PersonId idle = f.person({{"name", "idle"}, {"position", at(40, 0, -300)}});
    const world::PersonId worker = f.person({{"name", "worker"}, {"home", "cottage"}, {"routine", "day"}});
    GIANT_CHECK(has(f.why(idle), "no routine") && has(f.why(idle), "stands where it is"));
    GIANT_CHECK(has(f.why(worker), "\"day\"") && has(f.why(worker), "not had it do anything yet"));
    f.frames(7.0);
    std::string why = f.why(worker);
    GIANT_CHECK(has(why, "last had it work (from 06:30)") && has(why, "\"mill\"") && !has(why, "Alarmed"));
    GIANT_CHECK(has(why, "Looking for its way") || has(why, "On its way"));
    f.frames(7.0, 20.0);
    why = f.why(worker);
    GIANT_CHECK(has(why, "On its way to \"mill\"") && has(why, " m."));
    // The command says the same.
    const Json told = f.run("get_people", {{"ids", {worker}}}).value["people"][0];
    GIANT_CHECK(told["why"].get<std::string>() == why);
    // Alarmed by a noise: its routine's alarmed entry, and what alarmed it.
    const DVec3 here = f.travel.where(f.of(worker)).position;
    GIANT_CHECK(f.run("make_noise", {{"at", at(here.x, here.y, here.z + 3.0)}, {"loudness", 6.0}}).ok);
    f.frames(7.0, 0.5);
    why = f.why(worker);
    GIANT_CHECK(has(why, "Alarmed") && has(why, "by a noise") && has(why, "last had it hide (for an alarm over 0.50)"));
    // One with no routine sees something before it (it faces +z): alarmed by what it saw.
    GIANT_CHECK(!has(f.why(idle), "Alarmed"));
    GIANT_CHECK(f.run("show_to_people", {{"name", "thing"}, {"at", at(40, 1.6, -292)}}).ok);
    f.frames(7.0, 0.5);
    why = f.why(idle);
    GIANT_CHECK(has(why, "Alarmed") && has(why, "by what it saw") && !has(why, "noise") && has(why, "no routine"));
}

GIANT_TEST("people list: the camera follows a person, and lets go when it is gone") {
    Fixture f;
    const world::PersonId id = f.person({{"name", "walker"}, {"position", at(10, 0, 0)}});
    GIANT_CHECK(!follow_person_camera(f.state)); // nobody followed
    GIANT_CHECK(!f.run("follow_person", {{"id", id + 100}}).ok);
    GIANT_CHECK(!f.run("follow_person", {{"id", id}, {"distance", 0.5}}).ok);
    GIANT_CHECK(f.state.follow.person == 0);
    GIANT_CHECK(f.run("set_camera", {{"yaw", 0.0}, {"pitch", 0.0}}).ok);
    ai::CommandResult r = f.run("follow_person", {{"id", id}});
    GIANT_CHECK(r.ok && r.value["following"] == id && r.value["distance"] == 6.0);
    // Looking along -z: the camera is 6 m to +z of it, 1.5 m up.
    const auto camera = [&] { return f.state.camera.position; };
    GIANT_CHECK(std::fabs(camera().x - 10.0) < 1e-6 && std::fabs(camera().z - 6.0) < 1e-6 && std::fabs(camera().y - 1.5) < 1e-6);
    // Turned to look along -x and down 30 degrees: behind it along +x, and above.
    GIANT_CHECK(f.run("set_camera", {{"yaw", 90.0}, {"pitch", -30.0}}).ok);
    GIANT_CHECK(follow_person_camera(f.state));
    GIANT_CHECK(std::fabs(camera().x - (10.0 + 6.0 * std::cos(std::acos(-1.0) / 6.0))) < 1e-4 && std::fabs(camera().z) < 1e-4);
    GIANT_CHECK(std::fabs(camera().y - (1.5 + 3.0)) < 1e-4);
    // Looking up from behind it: the camera stays over the ground.
    GIANT_CHECK(f.run("set_camera", {{"yaw", 0.0}, {"pitch", 45.0}}).ok);
    GIANT_CHECK(follow_person_camera(f.state) && std::fabs(camera().y - 0.3) < 1e-6 && camera().z > 4.0);
    GIANT_CHECK(f.run("set_camera", {{"yaw", 90.0}, {"pitch", -30.0}}).ok);
    // It walks: the camera goes with it.
    f.frames(3.0);
    GIANT_CHECK(f.run("send_person", {{"id", id}, {"to", at(100, 0, 60)}}).ok);
    f.frames(3.0, 20.0, 16);
    const DVec3 there = f.travel.where(f.of(id)).position;
    GIANT_CHECK(std::hypot(there.x - 10.0, there.z) > 5.0);
    GIANT_CHECK(follow_person_camera(f.state));
    GIANT_CHECK(std::fabs(camera().z - there.z) < 1e-4 && std::fabs(camera().x - there.x - 6.0 * std::cos(std::acos(-1.0) / 6.0)) < 1e-4);
    // The distance alone is changed, and kept.
    r = f.run("follow_person", {{"id", id}, {"distance", 20.0}});
    GIANT_CHECK(r.ok && r.value["distance"] == 20.0 && f.state.follow.distance == 20.0f);
    r = f.run("follow_person", {{"id", id}});
    GIANT_CHECK(r.ok && r.value["distance"] == 20.0);
    // Nobody: the camera stays where it is.
    const DVec3 left = camera();
    r = f.run("follow_person", {{"id", nullptr}});
    GIANT_CHECK(r.ok && r.value["following"].is_null() && !follow_person_camera(f.state) && camera() == left);
    r = f.run("follow_person");
    GIANT_CHECK(r.ok && r.value["following"].is_null());
    // The one followed is removed: following ends, the camera stays.
    GIANT_CHECK(f.run("follow_person", {{"id", id}}).ok && f.state.follow.person == id);
    const DVec3 by_it = camera();
    GIANT_CHECK(f.run("remove_people", {{"ids", {id}}}).ok);
    GIANT_CHECK(!follow_person_camera(f.state) && f.state.follow.person == 0 && camera() == by_it);
    // Another scene's person of the same id is not followed.
    const world::PersonId again = f.person({{"name", "again"}, {"position", at(0, 0, 0)}});
    GIANT_CHECK(f.run("follow_person", {{"id", again}}).ok);
    GIANT_CHECK(f.run("new_scene").ok);
    GIANT_CHECK(!follow_person_camera(f.state) && f.state.follow.person == 0);
}

GIANT_TEST("people list: play ends following, and it does not come back when play stops") {
    Fixture f;
    PhysicsState physics;
    PlayState play{physics};
    f.state.physics = &physics;
    f.state.play = &play;
    register_physics_commands(f.commands, f.state);
    const world::PersonId id = f.person({{"name", "walker"}, {"position", at(10, 0, 0)}});
    GIANT_CHECK(f.run("follow_person", {{"id", id}}).ok && follow_person_camera(f.state));
    GIANT_CHECK(f.run("play", {{"on", true}}).ok);
    GIANT_CHECK(!f.run("follow_person", {{"id", id}}).ok); // the camera is the player's eyes
    const DVec3 eyes = f.state.camera.position;
    GIANT_CHECK(!follow_person_camera(f.state) && f.state.follow.person == 0 && f.state.camera.position == eyes);
    GIANT_CHECK(f.run("follow_person", {{"id", nullptr}}).ok); // nobody: allowed all the same
    GIANT_CHECK(f.run("play", {{"on", false}}).ok);
    const DVec3 back = f.state.camera.position;
    GIANT_CHECK(!follow_person_camera(f.state) && f.state.camera.position == back);
}
