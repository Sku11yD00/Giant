// People acting on their behaviours (people_routines.h; living-world plan
// LW6 part 3): each in a state, going to another when a transition's
// conditions hold, doing what the state says in place of its routine.
#include "../src/editor_commands.h"
#include "../src/mesh_library.h"
#include "../src/people_list.h"
#include "../src/people_routines.h"
#include "../src/people_senses.h"
#include "../src/people_travel.h"

#include "giant_test.h"

#include "giant/world/people.h"

#include <cmath>
#include <string>

using namespace giant;
using namespace giant::editor;
using ai::Json;

namespace {

Json at(double x, double y, double z) { return Json::array({x, y, z}); }
Json when(std::initializer_list<Json> conditions) { return Json(Json::array_t(conditions)); }
Json to(const char* state, Json conditions) { return {{"to", state}, {"when", std::move(conditions)}}; }

struct Fixture {
    ai::CommandRegistry commands;
    MeshLibrary meshes{nullptr};
    EditorState state;
    PeopleTravel travel;
    PeopleRoutines routines;
    PeopleSenses senses;
    double seconds = 0.0;
    double hours = 7.0;
    u64 run_id = 1;
    Fixture() {
        state.meshes = &meshes;
        state.travel = &travel;
        state.routines = &routines;
        state.senses = &senses;
        senses.settings.hear_loudness = 0.2f;
        senses.settings.alarm_loudness = 2.0f;
        senses.settings.calm_seconds = 30.0f;
        register_scene_commands(commands, state);
        register_people_commands(commands, state);
        register_route_commands(commands, state);
        world::Entity e;
        e.shape = world::Shape::mesh;
        e.mesh = "people/villager.glb";
        const world::EntityId model = state.scene.add(e);
        GIANT_CHECK(run("set_person_look", {{"name", "villager"}, {"entity", model}}).ok);
        GIANT_CHECK(run("delete_entities", {{"ids", {model}}}).ok);
        // A road east 100 m then north 100 m; a cottage by its start, an inn half way, a mill past its end.
        GIANT_CHECK(run("create_route", {{"points", {at(0, 0, 0), at(100, 0, 0), at(100, 0, 100)}}, {"kind", "road"}}).ok);
        GIANT_CHECK(run("create_place", {{"name", "cottage"}, {"position", at(0, 0, -5)}, {"tags", {"home"}}}).ok);
        GIANT_CHECK(run("create_place", {{"name", "inn"}, {"position", at(50, 0, 3)}, {"radius", 2.0}, {"tags", {"rest"}}}).ok);
        GIANT_CHECK(run("create_place", {{"name", "mill"}, {"position", at(100, 0, 110)}, {"radius", 4.0}, {"tags", {"work"}}}).ok);
        GIANT_CHECK(run("set_routine", {{"name", "day"}, {"entries", {{{"from", 0}, {"activity", "work"}, {"at", "tag"}, {"name", "work"}, {"speed", 2.0}}}}}).ok);
        routines.settings.spread_minutes = 0.0f;
    }
    ai::CommandResult run(const char* name, const Json& args = Json::object()) { return commands.execute(name, args, {}); }
    world::PersonId person(const char* behaviour, const char* routine = "", const char* home = "cottage") {
        const ai::CommandResult r = run("create_people", {{"people", {{{"look", "villager"}, {"home", home}, {"routine", routine}, {"behaviour", behaviour}}}}});
        GIANT_CHECK(r.ok);
        return r.ok ? r.value["ids"][0].get<world::PersonId>() : 0;
    }
    // `more` seconds simulated over `count` frames.
    void frames(double more = 0.0, int count = 4) {
        for (int i = 0; i < count; ++i) {
            seconds += more / count;
            travel.update(state.scene, run_id, seconds);
            senses.update(state.scene, travel);
            routines.update(state.scene, travel, hours, &senses);
        }
    }
    const world::Person& of(world::PersonId id) { return *world::find_person(state.scene.people, id); }
    DVec3 where(world::PersonId id) { return travel.where(of(id)).position; }
    double from(world::PersonId id, double x, double z) { return std::hypot(where(id).x - x, where(id).z - z); }
    Json told(world::PersonId id) { return run("get_people", {{"ids", {id}}}).value["people"][0]; }
    std::string in(world::PersonId id) {
        const Json state_now = told(id)["state"];
        return state_now.is_null() ? std::string() : state_now["name"].get<std::string>();
    }
    std::string trip_to(world::PersonId id) {
        const PeopleTravel::Trip* trip = travel.trip(id);
        return trip == nullptr ? std::string() : trip->place;
    }
    void shot_by(world::PersonId id, float loudness = 60.0f) { // ten metres from where it is now
        const DVec3 here = where(id);
        GIANT_CHECK(senses.noise({here.x, here.y, here.z + 10.0}, loudness, PeopleSenses::Kind::shot));
    }
};

// One runs home from a shot, hides there, and goes back to its day a while on once calm.
Json wary_states() {
    return Json::array({{{"name", "day"}, {"transitions", Json::array({to("flee", when({{{"is", "sensed"}, {"kind", "shot"}}, {{"is", "alarm_over"}, {"value", 0.5}}}))})}},
                        {{"name", "flee"}, {"activity", "flee"}, {"at", "home"}, {"speed", 5.0},
                         {"transitions", Json::array({to("hide", when({{{"is", "arrived"}}})), to("day", when({{{"is", "failed"}}}))})}},
                        {{"name", "hide"}, {"activity", "hide"},
                         {"transitions", Json::array({to("day", when({{{"is", "after"}, {"value", 20}}, {{"is", "alarm_under"}, {"value", 0.1}}}))})}}});
}

bool has(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

} // namespace

GIANT_TEST("behaviours: one runs home from a shot, hides there, and goes back to its day once calm") {
    Fixture f;
    GIANT_CHECK(f.run("set_behaviour", {{"name", "wary"}, {"states", wary_states()}}).ok);
    const world::PersonId id = f.person("wary", "day"), plain = f.person("", "day");
    // Not simulating: nobody is in any state.
    f.run_id = 0;
    f.frames();
    GIANT_CHECK(f.in(id).empty() && f.told(id)["doing"].is_null() && f.routines.stats().behaving == 0);
    GIANT_CHECK(has(person_why(f.state, f.of(id)), "has not begun"));
    f.run_id = 1;
    // Its day: the state leaves it to the routine, which sends it to work.
    f.frames(10.0);
    Json told = f.told(id);
    GIANT_CHECK(f.in(id) == "day" && told["state"]["index"] == 0 && told["doing"]["activity"] == "work" && told["doing"]["own"] == false);
    GIANT_CHECK(f.trip_to(id) == "mill" && f.trip_to(plain) == "mill");
    GIANT_CHECK(f.routines.stats().following == 2 && f.routines.stats().behaving == 1 && f.routines.stats().changed == 0);
    GIANT_CHECK(std::fabs(told["state"]["seconds"].get<double>() - 10.0) < 3.0);
    // A noise that is no shot, however loud: it goes on (its routine says nothing of alarm).
    const DVec3 here = f.where(id);
    GIANT_CHECK(f.senses.noise({here.x, here.y, here.z + 10.0}, 60.0f, PeopleSenses::Kind::impact));
    f.frames(1.0);
    GIANT_CHECK(f.in(id) == "day" && f.trip_to(id) == "mill" && f.senses.alarm(f.state.scene, id) > 0.5f);
    // A shot far off, heard but under half alarmed: both conditions must hold.
    f.frames(40.0, 40);
    GIANT_CHECK(f.senses.alarm(f.state.scene, id) == 0.0f);
    f.shot_by(id, 6.0f); // (0.6 at its ears: 0.3)
    f.frames(1.0);
    GIANT_CHECK(f.in(id) == "day" && f.senses.alarm(f.state.scene, id) > 0.2f);
    // A shot close by: it flees home at a run; the other goes on to work.
    f.shot_by(id);
    f.frames(1.0);
    told = f.told(id);
    GIANT_CHECK(f.in(id) == "flee" && told["doing"]["activity"] == "flee" && told["doing"]["own"] == true && told["doing"]["place"] == "cottage");
    GIANT_CHECK(f.trip_to(id) == "cottage" && f.travel.trip(id)->speed == 5.0 && f.trip_to(plain) == "mill");
    GIANT_CHECK(f.routines.stats().changed == 1);
    std::string why = person_why(f.state, f.of(id));
    GIANT_CHECK(has(why, "behaviour \"wary\"") && has(why, "state \"flee\"") && has(why, "The state has it flee") && !has(why, "Its routine"));
    // Home: it hides where it is (the state after, a look on).
    f.frames(26.0, 104); // (a hundred metres or so, at 5 m/s)
    GIANT_CHECK(f.from(id, 0, -5) < 6.0 && f.travel.trip(id) == nullptr);
    told = f.told(id);
    GIANT_CHECK(f.in(id) == "hide" && told["doing"]["activity"] == "hide" && told["doing"]["own"] == true && f.routines.stats().changed == 2);
    // Calm (30 s) and 20 s in the state: back to its day, off to work again.
    f.frames(8.0, 16);
    GIANT_CHECK(f.in(id) == "hide"); // (calm by now, but not 20 s in the state)
    f.frames(40.0, 80);
    told = f.told(id);
    GIANT_CHECK(f.in(id) == "day" && told["doing"]["activity"] == "work" && told["doing"]["own"] == false && f.trip_to(id) == "mill");
    GIANT_CHECK(f.routines.stats().changed == 3 && has(person_why(f.state, f.of(id)), "Its routine \"day\" last had it work"));
    // The simulation stopped: nobody is in any state; another run begins in the first.
    f.shot_by(id);
    f.frames(1.0);
    GIANT_CHECK(f.in(id) == "flee");
    f.run_id = 0;
    f.frames();
    GIANT_CHECK(f.in(id).empty());
    f.run_id = 2;
    f.frames(1.0);
    GIANT_CHECK(f.in(id) == "day" && f.routines.stats().changed == 0);
    // Routines off: behaviours too.
    GIANT_CHECK(f.run("set_routine_settings", {{"on", false}}).ok);
    f.shot_by(id);
    f.frames(1.0);
    GIANT_CHECK(f.in(id).empty());
    const Json settings = f.run("set_routine_settings", {{"on", true}}).value;
    GIANT_CHECK(settings.contains("behaving") && settings.contains("changed"));
}

GIANT_TEST("behaviours: one with a behaviour and no routine is followed; the hour, arriving and time move it on") {
    Fixture f;
    // Idle until eight; to the inn; arrived, a rest of 30 s; then home; and idle again after ten.
    GIANT_CHECK(f.run("set_behaviour",
                      {{"name", "visitor"},
                       {"states", Json::array({{{"name", "idle"}, {"transitions", Json::array({to("out", when({{{"is", "hour"}, {"value", 8}, {"until", 10}}}))})}},
                                               {{"name", "out"}, {"activity", "visit"}, {"at", "place"}, {"place", "inn"}, {"speed", 2.0},
                                                {"transitions", Json::array({to("rest", when({{{"is", "arrived"}}}))})}},
                                               {{"name", "rest"}, {"activity", "sit"},
                                                {"transitions", Json::array({to("back", when({{{"is", "after"}, {"value", 30}}}))})}},
                                               {{"name", "back"}, {"activity", "walk"}, {"at", "home"},
                                                {"transitions", Json::array({to("idle", when({{{"is", "arrived"}}, {{"is", "hour"}, {"value", 10}, {"until", 8}}}))})}}})}})
                    .ok);
    const world::PersonId id = f.person("visitor");
    f.frames(5.0);
    GIANT_CHECK(f.in(id) == "idle" && f.told(id)["doing"].is_null() && f.travel.trip(id) == nullptr);
    GIANT_CHECK(f.routines.stats().following == 1 && f.routines.stats().behaving == 1);
    // (An hour's range that goes over midnight holds on both sides of it.)
    f.hours = 23.0;
    f.frames(1.0);
    GIANT_CHECK(f.in(id) == "idle");
    f.hours = 8.5;
    f.frames(1.0);
    GIANT_CHECK(f.in(id) == "out" && f.trip_to(id) == "inn" && f.told(id)["doing"]["activity"] == "visit");
    // On its way it has not arrived; there, it rests (after counts from when it entered the state).
    f.frames(10.0, 20);
    GIANT_CHECK(f.in(id) == "out" && f.travel.trip(id) != nullptr);
    f.frames(30.0, 60);
    GIANT_CHECK(f.in(id) == "rest" && f.from(id, 50, 3) < 2.5 && f.told(id)["doing"]["activity"] == "sit");
    const double rested = f.told(id)["state"]["seconds"].get<double>();
    GIANT_CHECK(rested > 0.0 && rested < 30.0);
    f.frames(29.0 - rested, 8);
    GIANT_CHECK(f.in(id) == "rest");
    f.frames(2.0, 4);
    GIANT_CHECK(f.in(id) == "back" && f.trip_to(id) == "cottage");
    // Home before ten: it has arrived, but the hour is not yet; at ten it idles, and stands.
    f.frames(60.0, 120);
    GIANT_CHECK(f.in(id) == "back" && f.travel.trip(id) == nullptr && f.from(id, 0, -5) < 6.0);
    f.hours = 10.25;
    f.frames(1.0);
    GIANT_CHECK(f.in(id) == "idle" && f.told(id)["doing"].is_null() && f.routines.stats().changed == 4);
    // A chain of states whose conditions all hold takes a look each.
    GIANT_CHECK(f.run("set_behaviour",
                      {{"name", "chain"},
                       {"states", Json::array({{{"name", "a"}, {"transitions", Json::array({to("b", when({{{"is", "arrived"}}}))})}},
                                               {{"name", "b"}, {"transitions", Json::array({to("c", when({{{"is", "arrived"}}}))})}},
                                               {{"name", "c"}}})}})
                    .ok);
    const world::PersonId chained = f.person("chain");
    f.frames(0.1, 1); // looked at: in the first, not yet acted on (nothing has arrived)
    GIANT_CHECK(f.in(chained) == "a");
    f.frames(0.1, 1);
    GIANT_CHECK(f.in(chained) == "b");
    f.frames(0.1, 1);
    GIANT_CHECK(f.in(chained) == "c");
    f.frames(5.0);
    GIANT_CHECK(f.in(chained) == "c"); // a state one never leaves
}

GIANT_TEST("behaviours: a state that cannot be done fails; a behaviour replaced, taken away or removed is followed") {
    Fixture f;
    // To a place that is not there (a file's: a command refuses one): failed, and on to another state.
    world::Behaviour lost;
    lost.name = "lost";
    lost.states.resize(3);
    lost.states[0].name = "go";
    lost.states[0].own = true;
    lost.states[0].does = {0.0f, "seek", world::RoutineAt::place, "nowhere", 1.4f};
    lost.states[0].transitions = {{"give_up", {{world::BehaviourWhen::failed, 0.0f, 0.0f, ""}}}, {"there", {{world::BehaviourWhen::arrived, 0.0f, 0.0f, ""}}}};
    lost.states[1].name = "give_up";
    lost.states[1].own = true;
    lost.states[1].does = {0.0f, "shrug", world::RoutineAt::stay, "", 1.4f};
    lost.states[2].name = "there";
    GIANT_CHECK(!world::check_behaviour(lost));
    f.state.scene.behaviours.push_back(lost);
    const world::PersonId id = f.person("lost", "day");
    f.frames(0.5, 1);
    // (Its routine had no say: the state's own came first, and nothing was sent.)
    GIANT_CHECK(f.in(id) == "go" && f.told(id)["doing"]["failed"] == true && f.travel.trip(id) == nullptr);
    GIANT_CHECK(has(person_why(f.state, f.of(id)), "could not be sent"));
    f.frames(0.5, 1);
    GIANT_CHECK(f.in(id) == "give_up");
    f.frames(0.5, 1);
    GIANT_CHECK(f.told(id)["doing"]["activity"] == "shrug" && f.told(id)["doing"]["failed"] == false);
    // A state's own that cannot be done stops what the last had it do: on its way to work, then told to seek nowhere.
    GIANT_CHECK(f.run("set_behaviour", {{"name", "turn"},
                                        {"states", Json::array({{{"name", "day"}, {"transitions", Json::array({to("flee", when({{{"is", "sensed"}, {"kind", "shot"}}}))})}},
                                                                {{"name", "flee"}, {"activity", "flee"}, {"at", "home"}, {"speed", 5.0}}})}})
                    .ok);
    ai::CommandResult made = f.run("create_people", {{"people", {{{"look", "villager"}, {"routine", "day"}, {"behaviour", "turn"}, {"position", at(5, 0, 0)}}}}});
    GIANT_CHECK(made.ok);
    const world::PersonId homeless = made.value["ids"][0].get<world::PersonId>();
    f.frames(10.0);
    GIANT_CHECK(f.in(homeless) == "day" && f.trip_to(homeless) == "mill");
    f.shot_by(homeless);
    f.frames(1.0);
    GIANT_CHECK(f.in(homeless) == "flee" && f.told(homeless)["doing"]["failed"] == true && f.travel.trip(homeless) == nullptr); // no home to flee to
    // Replaced by one with fewer states while in a later one: it begins again in the first.
    GIANT_CHECK(f.run("set_behaviour", {{"name", "wary2"}, {"states", wary_states()}}).ok);
    const world::PersonId runner = f.person("wary2", "day");
    f.frames(5.0);
    f.shot_by(runner);
    f.frames(10.0, 40);
    GIANT_CHECK(f.in(runner) == "hide");
    GIANT_CHECK(f.run("set_behaviour", {{"name", "wary2"}, {"states", Json::array({{{"name", "only"}}})}}).ok);
    f.frames(1.0);
    GIANT_CHECK(f.in(runner) == "only" && f.told(runner)["doing"]["activity"] == "work" && f.trip_to(runner) == "mill");
    // Its behaviour taken away while a state's own trip is under way: with a routine, the routine says; with none, it stops and stands.
    GIANT_CHECK(f.run("undo").ok); // (the three states again)
    const world::PersonId other = f.person("wary2");
    GIANT_CHECK(f.run("set_person", {{"id", other}, {"position", at(90, 0, 0)}}).ok);
    f.frames(40.0, 40); // (calm again)
    f.shot_by(runner);
    f.shot_by(other);
    f.frames(2.0);
    GIANT_CHECK(f.in(runner) == "flee" && f.in(other) == "flee" && f.trip_to(other) == "cottage" && f.trip_to(runner) == "cottage");
    GIANT_CHECK(f.run("set_person", {{"id", other}, {"behaviour", ""}}).ok && f.run("set_person", {{"id", runner}, {"behaviour", ""}}).ok);
    f.frames(1.0);
    GIANT_CHECK(f.in(other).empty() && f.travel.trip(other) == nullptr && f.told(other)["doing"].is_null());
    GIANT_CHECK(f.in(runner).empty() && f.trip_to(runner) == "mill" && f.told(runner)["doing"]["own"] == false);
    GIANT_CHECK(f.routines.stats().following >= 1);
    // A trip begun by hand is not a behaviour's to stop: one with nothing to say leaves it be.
    GIANT_CHECK(f.run("send_person", {{"id", other}, {"to", "inn"}}).ok);
    f.frames(2.0);
    GIANT_CHECK(f.trip_to(other) == "inn");
    // What a state's own had one do is not a game save's to keep yet; what a routine had it do is.
    PeopleRoutines::Kept kept;
    f.routines.ready(f.state.scene, f.travel);
    const auto index = [&](world::PersonId person) { return static_cast<usize>(&f.of(person) - f.state.scene.people.data()); };
    GIANT_CHECK(f.routines.kept(f.state.scene, f.travel, index(runner), kept));
    GIANT_CHECK(!f.routines.kept(f.state.scene, f.travel, index(id), kept)); // (shrugging: its state's own)
}

GIANT_TEST("behaviours: thousands are followed a share a frame, each in its own state") {
    Fixture f;
    GIANT_CHECK(f.run("set_behaviour", {{"name", "wary"}, {"states", wary_states()}}).ok);
    Json people = Json::array();
    const usize count = 3000;
    for (usize i = 0; i < count; ++i) {
        people.push_back({{"look", "villager"}, {"home", "cottage"}, {"behaviour", "wary"}, {"position", at(static_cast<double>(i % 100), 0, 1.0)}});
    }
    GIANT_CHECK(f.run("create_people", {{"people", people}}).ok);
    f.routines.settings.looked_a_frame = 512;
    f.frames(1.0, 12); // (twice through)
    GIANT_CHECK(f.routines.stats().following == count && f.routines.stats().behaving == count && f.routines.stats().changed == 0);
    // A shot in their midst: those who hear it loud flee, those looked at a frame at most, and all in the end.
    GIANT_CHECK(f.senses.noise({50.0, 0.0, 1.0}, 2000.0f, PeopleSenses::Kind::shot));
    f.frames(1.0, 4);
    const u64 changed = f.routines.stats().changed;
    GIANT_CHECK(changed > 0 && changed <= 512 * 4);
    f.frames(20.0, 200);
    usize fleeing_or_hidden = 0;
    for (const world::Person& p : f.state.scene.people) {
        PeopleRoutines::Behaving b;
        if (f.routines.behaving(f.state.scene, f.travel, p.id, b) && (b.state == "flee" || b.state == "hide")) ++fleeing_or_hidden;
    }
    GIANT_CHECK(fleeing_or_hidden == count);
}

GIANT_TEST("behaviours: an hour's range over midnight holds on both sides; another behaviour removed does not disturb a state") {
    Fixture f;
    GIANT_CHECK(f.run("set_behaviour", {{"name", "aside"}, {"states", Json::array({{{"name", "only"}}})}}).ok);
    GIANT_CHECK(f.run("set_behaviour",
                      {{"name", "sleeper"},
                       {"states", Json::array({{{"name", "up"}, {"transitions", Json::array({to("abed", when({{{"is", "hour"}, {"value", 22}, {"until", 6}}}))})}},
                                               {{"name", "abed"}, {"activity", "sleep"}, {"at", "home"},
                                                {"transitions", Json::array({to("up", when({{{"is", "hour"}, {"value", 6}, {"until", 22}}}))})}}})}})
                    .ok);
    const world::PersonId id = f.person("sleeper");
    f.hours = 12.0;
    f.frames(1.0);
    GIANT_CHECK(f.in(id) == "up");
    f.hours = 21.9;
    f.frames(1.0);
    GIANT_CHECK(f.in(id) == "up");
    f.hours = 23.0; // (before midnight)
    f.frames(1.0);
    GIANT_CHECK(f.in(id) == "abed");
    f.hours = 12.0;
    f.frames(1.0);
    GIANT_CHECK(f.in(id) == "up");
    f.hours = 3.0; // (after it)
    f.frames(1.0);
    GIANT_CHECK(f.in(id) == "abed");
    f.hours = 5.9;
    f.frames(1.0);
    GIANT_CHECK(f.in(id) == "abed");
    f.hours = 6.0; // (until: not the hour itself)
    f.frames(1.0);
    GIANT_CHECK(f.in(id) == "up");
    // In a later state for a while; a behaviour before its own in the list removed: the state and its seconds stay.
    f.hours = 23.0;
    f.frames(1.0);
    f.frames(20.0, 8);
    GIANT_CHECK(f.in(id) == "abed");
    const double before = f.told(id)["state"]["seconds"].get<double>();
    const u64 changed = f.routines.stats().changed;
    GIANT_CHECK(before > 15.0);
    GIANT_CHECK(f.run("remove_behaviour", {{"name", "aside"}}).ok);
    f.frames(1.0);
    GIANT_CHECK(f.in(id) == "abed" && f.told(id)["state"]["seconds"].get<double>() > before && f.routines.stats().changed == changed);
    GIANT_CHECK(f.told(id)["doing"]["activity"] == "sleep");
    // Its own given another name's (another behaviour): the first state.
    GIANT_CHECK(f.run("set_behaviour", {{"name", "other"}, {"states", Json::array({{{"name", "first"}}, {{"name", "second"}}})}}).ok);
    GIANT_CHECK(f.run("set_person", {{"id", id}, {"behaviour", "other"}}).ok);
    f.frames(1.0);
    GIANT_CHECK(f.in(id) == "first" && f.told(id)["state"]["seconds"].get<double>() < 2.0);
    // Given another behaviour, and its old one removed, between two looks: found where the old one was, it is another still.
    const world::PersonId moved = f.person("sleeper");
    f.frames(1.0);
    f.frames(5.0);
    GIANT_CHECK(f.in(moved) == "abed");
    GIANT_CHECK(f.run("set_person", {{"id", moved}, {"behaviour", "other"}}).ok && f.run("remove_behaviour", {{"name", "sleeper"}}).ok);
    f.frames(1.0);
    GIANT_CHECK(f.in(moved) == "first" && f.told(moved)["state"]["seconds"].get<double>() < 2.0 && f.told(moved)["doing"].is_null());
    GIANT_CHECK(f.travel.trip(moved) == nullptr);
}

GIANT_TEST("behaviours: one that failed has neither arrived nor failed until acted on again; a state's own is acted on before it is left") {
    Fixture f;
    // To a place not there yet, and on only when arrived.
    world::Behaviour seeker;
    seeker.name = "seeker";
    seeker.states.resize(2);
    seeker.states[0].name = "go";
    seeker.states[0].own = true;
    seeker.states[0].does = {0.0f, "seek", world::RoutineAt::place, "well", 2.0f};
    seeker.states[0].transitions = {{"there", {{world::BehaviourWhen::arrived, 0.0f, 0.0f, ""}}}};
    seeker.states[1].name = "there";
    GIANT_CHECK(!world::check_behaviour(seeker));
    f.state.scene.behaviours.push_back(seeker);
    // And one whose routine sends it to such a place: its state (the routine's to say) on only when arrived.
    world::Routine astray;
    astray.name = "astray";
    astray.entries.push_back({0.0f, "seek", world::RoutineAt::place, "well", 2.0f});
    f.state.scene.routines.push_back(astray);
    GIANT_CHECK(f.run("set_behaviour", {{"name", "waits"},
                                        {"states", Json::array({{{"name", "go"}, {"transitions", Json::array({to("there", when({{{"is", "arrived"}}}))})}}, {{"name", "there"}}})}})
                    .ok);
    const world::PersonId id = f.person("seeker"), led = f.person("waits", "astray");
    f.frames(2.0, 8);
    GIANT_CHECK(f.in(id) == "go" && f.told(id)["doing"]["failed"] == true);
    GIANT_CHECK(f.in(led) == "go" && f.told(led)["doing"]["failed"] == true);
    // The routine found at another place in the list (one put before it): what it did was the old one's, and it has not arrived.
    world::Routine first;
    first.name = "aaa";
    first.entries.push_back({0.0f, "idle", world::RoutineAt::stay, "", 1.4f});
    f.state.scene.routines.insert(f.state.scene.routines.begin(), first);
    f.frames(0.25, 1);
    GIANT_CHECK(f.in(led) == "go");
    f.frames(2.0, 8);
    GIANT_CHECK(f.in(led) == "go" && f.told(led)["doing"]["failed"] == true);
    // The place made, far off: both are sent, and neither has arrived.
    GIANT_CHECK(f.run("create_place", {{"name", "well"}, {"position", at(100, 0, 60)}, {"radius", 2.0}}).ok);
    f.frames(0.25, 1);
    GIANT_CHECK(f.in(id) == "go" && f.in(led) == "go");
    f.frames(2.0, 8);
    GIANT_CHECK(f.in(id) == "go" && f.trip_to(id) == "well" && f.told(id)["doing"]["failed"] == false);
    GIANT_CHECK(f.in(led) == "go" && f.trip_to(led) == "well");
    f.frames(100.0, 200);
    GIANT_CHECK(f.in(id) == "there" && f.in(led) == "there" && f.from(id, 100, 60) < 3.0);

    // Many entering a state at once, one sent a frame: each is sent before it goes on (none is passed by).
    GIANT_CHECK(f.run("set_behaviour",
                      {{"name", "queue"},
                       {"states", Json::array({{{"name", "set_off"}, {"activity", "visit"}, {"at", "place"}, {"place", "inn"}, {"speed", 2.0},
                                                {"transitions", Json::array({to("gone", when({{{"is", "after"}, {"value", 0.01}}}))})}},
                                               {{"name", "gone"}}})}})
                    .ok);
    GIANT_CHECK(f.run("set_routine_settings", {{"sent_a_frame", 1}}).ok);
    const u64 sent = f.routines.stats().sent;
    world::PersonId queued[6];
    for (world::PersonId& q : queued) q = f.person("queue");
    f.frames(0.5, 3);
    usize waiting = 0;
    for (const world::PersonId q : queued) waiting += f.in(q) == "set_off" ? 1u : 0u;
    GIANT_CHECK(waiting >= 3 && f.routines.stats().sent <= sent + 3); // (one sent a frame: the rest wait in the state)
    f.frames(4.0, 40);
    for (const world::PersonId q : queued) GIANT_CHECK(f.in(q) == "gone");
    GIANT_CHECK(f.routines.stats().sent == sent + 6);
    GIANT_CHECK(f.run("set_routine_settings", {{"sent_a_frame", 64}}).ok);

    // From a state's own to one that leaves it to a routine that cannot be done: the state's trip does not go on.
    world::Routine lost;
    lost.name = "lost";
    lost.entries.push_back({0.0f, "seek", world::RoutineAt::place, "void", 2.0f});
    f.state.scene.routines.push_back(lost);
    GIANT_CHECK(f.run("set_behaviour",
                      {{"name", "handover"},
                       {"states", Json::array({{{"name", "run"}, {"activity", "run"}, {"at", "home"}, {"speed", 2.0},
                                                {"transitions", Json::array({to("led", when({{{"is", "after"}, {"value", 3}}}))})}},
                                               {{"name", "led"}}})}})
                    .ok);
    const world::PersonId handed = f.person("handover", "lost");
    GIANT_CHECK(f.run("set_person", {{"id", handed}, {"position", at(90, 0, 0)}}).ok);
    f.frames(2.0, 4);
    GIANT_CHECK(f.in(handed) == "run" && f.trip_to(handed) == "cottage");
    f.frames(3.0, 6);
    GIANT_CHECK(f.in(handed) == "led" && f.travel.trip(handed) == nullptr && f.told(handed)["doing"]["failed"] == true);
}

GIANT_TEST("behaviours: a state entered is acted on though it says the same; a trip by hand is left; a save keeps a routine's") {
    Fixture f;
    // To the inn; there, and 5 s on, to the inn again (a state that says what the last did).
    GIANT_CHECK(f.run("set_behaviour",
                      {{"name", "twice"},
                       {"states", Json::array({{{"name", "one"}, {"activity", "visit"}, {"at", "place"}, {"place", "inn"}, {"speed", 2.0},
                                                {"transitions", Json::array({to("two", when({{{"is", "arrived"}}, {{"is", "after"}, {"value", 40}}}))})}},
                                               {{"name", "two"}, {"activity", "visit"}, {"at", "place"}, {"place", "inn"}, {"speed", 2.0},
                                                {"transitions", Json::array({to("rest", when({{{"is", "arrived"}}}))})}},
                                               {{"name", "rest"}}})}})
                    .ok);
    const world::PersonId id = f.person("twice");
    f.frames(35.0, 70);
    GIANT_CHECK(f.in(id) == "one" && f.travel.trip(id) == nullptr && f.from(id, 50, 3) < 2.5);
    // Sent off by hand meanwhile: the state entered sends it to the inn again.
    GIANT_CHECK(f.run("send_person", {{"id", id}, {"to", "mill"}}).ok);
    f.frames(2.0, 4);
    GIANT_CHECK(f.in(id) == "one" && f.trip_to(id) == "mill");
    f.frames(5.0, 10);
    GIANT_CHECK(f.in(id) == "two" && f.trip_to(id) == "inn");
    f.frames(30.0, 60);
    GIANT_CHECK(f.in(id) == "rest" && f.from(id, 50, 3) < 2.5);
    // In a state that says nothing, with no routine: a trip begun by hand is not its behaviour's to stop.
    GIANT_CHECK(f.run("send_person", {{"id", id}, {"to", "mill"}}).ok);
    f.frames(4.0, 8);
    GIANT_CHECK(f.in(id) == "rest" && f.trip_to(id) == "mill" && f.told(id)["doing"].is_null());
    // One with a behaviour whose state leaves it to its routine: a save keeps what the routine had it do, and
    // put back it is not sent again, and is in its state still.
    GIANT_CHECK(f.run("set_behaviour", {{"name", "wary"}, {"states", wary_states()}}).ok);
    const world::PersonId worker = f.person("wary", "day");
    f.frames(5.0, 10);
    GIANT_CHECK(f.in(worker) == "day" && f.trip_to(worker) == "mill");
    PeopleRoutines::Kept kept;
    f.routines.ready(f.state.scene, f.travel);
    const usize index = static_cast<usize>(&f.of(worker) - f.state.scene.people.data());
    GIANT_CHECK(f.routines.kept(f.state.scene, f.travel, index, kept) && kept.own_trip && kept.place == "mill");
    const u64 sent = f.routines.stats().sent;
    GIANT_CHECK(f.routines.restore(f.state.scene, f.travel, worker, kept));
    f.frames(2.0, 4);
    GIANT_CHECK(f.in(worker) == "day" && f.trip_to(worker) == "mill" && f.routines.stats().sent == sent);
    GIANT_CHECK(f.told(worker)["doing"]["activity"] == "work" && f.told(worker)["doing"]["own"] == false);
    // A routine with no entries (a file's), and one that is not there: as ever (the first followed, the
    // other not; neither does anything). Given a behaviour, the one whose routine is not there is followed,
    // and has not failed.
    world::Routine empty;
    empty.name = "empty";
    f.state.scene.routines.push_back(empty);
    const u32 following = f.routines.stats().following;
    const world::PersonId idle = f.person("", "empty"), astray = f.person("", "empty");
    for (world::Person& p : f.state.scene.people) {
        if (p.id == astray) p.routine = "nowhere";
    }
    f.frames(2.0, 4);
    GIANT_CHECK(f.routines.stats().following == following + 1);
    GIANT_CHECK(f.told(idle)["doing"].is_null() && f.told(astray)["doing"].is_null() && f.travel.trip(idle) == nullptr && f.travel.trip(astray) == nullptr);
    GIANT_CHECK(f.run("set_behaviour", {{"name", "calm"},
                                        {"states", Json::array({{{"name", "wait"}, {"transitions", Json::array({to("bad", when({{{"is", "failed"}}}))})}}, {{"name", "bad"}}})}})
                    .ok);
    for (world::Person& p : f.state.scene.people) {
        if (p.id == astray) p.behaviour = "calm";
    }
    f.frames(2.0, 4);
    GIANT_CHECK(f.routines.stats().following == following + 2 && f.in(astray) == "wait" && f.told(astray)["doing"].is_null());
}

GIANT_TEST("behaviours: what happens is kept for scripts and get_person_events, in order, the last few thousand") {
    Fixture f;
    GIANT_CHECK(f.run("set_behaviour", {{"name", "wary"}, {"states", wary_states()}}).ok);
    Json got = f.run("get_person_events").value;
    GIANT_CHECK(got["events"].empty() && got["latest"] == 0 && got["oldest"] == 1);
    const world::PersonId id = f.person("wary", "day"), plain = f.person("", "day");
    f.frames(2.0);
    // It began its behaviour; the one with none did nothing to tell.
    got = f.run("get_person_events").value;
    GIANT_CHECK(got["events"].size() == 1 && got["latest"] == 1 && got["oldest"] == 1);
    GIANT_CHECK(got["events"][0] == Json({{"serial", 1}, {"person", id}, {"behaviour", "wary"}, {"from", ""}, {"to", "day"},
                                          {"seconds", got["events"][0]["seconds"]}}));
    GIANT_CHECK(got["events"][0]["seconds"].get<double>() <= 2.0 && plain != id);
    // A shot: day to flee; home: flee to hide. Asked after the first: the two, in order.
    f.shot_by(id);
    f.frames(10.0, 40);
    GIANT_CHECK(f.in(id) == "hide");
    got = f.run("get_person_events", {{"after", 1}}).value;
    GIANT_CHECK(got["events"].size() == 2 && got["latest"] == 3);
    GIANT_CHECK(got["events"][0]["serial"] == 2 && got["events"][0]["from"] == "day" && got["events"][0]["to"] == "flee");
    GIANT_CHECK(got["events"][1]["serial"] == 3 && got["events"][1]["from"] == "flee" && got["events"][1]["to"] == "hide");
    GIANT_CHECK(got["events"][0]["seconds"].get<double>() < got["events"][1]["seconds"].get<double>());
    // A limit: the oldest first, and `latest` goes on from there; after the last, none.
    got = f.run("get_person_events", {{"limit", 2}}).value;
    GIANT_CHECK(got["events"].size() == 2 && got["latest"] == 2 && got["events"][1]["to"] == "flee");
    got = f.run("get_person_events", {{"after", 3}}).value;
    GIANT_CHECK(got["events"].empty() && got["latest"] == 3);
    got = f.run("get_person_events", {{"after", 1000}}).value; // (more than happened)
    GIANT_CHECK(got["events"].empty() && got["latest"] == 3);
    GIANT_CHECK(!f.run("get_person_events", {{"limit", 0}}).ok && !f.run("get_person_events", {{"limit", 257}}).ok);
    GIANT_CHECK(!f.run("get_person_events", {{"after", -1}}).ok);
    // As the scripts are told: a share at a time, moving on.
    std::vector<PeopleRoutines::Change> changes;
    u64 told = 0;
    f.routines.changes_since(f.state.scene, told, 2, changes);
    GIANT_CHECK(changes.size() == 2 && told == 2 && changes[0].from.empty() && changes[1].to == "flee" && changes[1].person == id);
    f.routines.changes_since(f.state.scene, told, 2, changes);
    GIANT_CHECK(changes.size() == 3 && told == 3 && changes[2].to == "hide");
    f.routines.changes_since(f.state.scene, told, 2, changes);
    GIANT_CHECK(changes.size() == 3 && told == 3);
    // Another run: everyone begins again, and is told so (the serials go on).
    f.run_id = 2;
    f.frames(1.0);
    got = f.run("get_person_events", {{"after", 3}}).value;
    GIANT_CHECK(got["events"].size() == 1 && got["events"][0]["serial"] == 4 && got["events"][0]["from"] == "" && got["events"][0]["to"] == "day");
    // Its behaviour changed since (a state fewer): what happened in the old one is not told with the new one's names.
    f.shot_by(id);
    f.frames(0.25, 1); // (a look: it is by its home, and would be there the next)
    GIANT_CHECK(f.in(id) == "flee" && f.routines.changes() == 5);
    GIANT_CHECK(f.run("set_behaviour", {{"name", "wary"}, {"states", Json::array({{{"name", "only"}}})}}).ok);
    got = f.run("get_person_events", {{"after", 4}}).value;
    GIANT_CHECK(got["events"].empty() && got["latest"] == 5); // (to the state at 1: there is none now)
    f.frames(1.0);
    got = f.run("get_person_events", {{"after", 5}}).value;
    GIANT_CHECK(got["events"].size() == 1 && got["events"][0]["from"] == "" && got["events"][0]["to"] == "only");
    // The behaviour removed: nothing of it is told.
    GIANT_CHECK(f.run("set_person", {{"id", id}, {"behaviour", ""}}).ok && f.run("remove_behaviour", {{"name", "wary"}}).ok);
    f.frames(1.0);
    got = f.run("get_person_events").value;
    GIANT_CHECK(got["events"].empty() && got["latest"] == 6 && f.routines.changes() == 6);

    // More than are kept: the oldest are gone, and one asking from the start goes on from the oldest kept.
    GIANT_CHECK(f.run("set_behaviour", {{"name", "wary"}, {"states", wary_states()}}).ok);
    Json people = Json::array();
    const usize count = 3000;
    for (usize i = 0; i < count; ++i) {
        people.push_back({{"look", "villager"}, {"home", "cottage"}, {"behaviour", "wary"}, {"position", at(static_cast<double>(i % 100), 0, 1.0)}});
    }
    GIANT_CHECK(f.run("create_people", {{"people", people}}).ok);
    f.routines.settings.looked_a_frame = 1024;
    f.frames(1.0, 4);
    GIANT_CHECK(f.routines.changes() == 6 + count && f.routines.changes_from() == 1);
    GIANT_CHECK(f.senses.noise({50.0, 0.0, 1.0}, 2000.0f, PeopleSenses::Kind::shot));
    f.frames(10.0, 60); // (those sent a frame are few: the rest wait their turn)
    const u64 made = f.routines.changes();
    GIANT_CHECK(made > 6 + count + 1500 && f.routines.changes_from() == made - PeopleRoutines::k_changes_kept + 1);
    got = f.run("get_person_events", {{"limit", 256}}).value;
    GIANT_CHECK(got["oldest"] == f.routines.changes_from() && got["events"].size() == 256);
    GIANT_CHECK(got["events"][0]["serial"] == f.routines.changes_from() && got["latest"] == f.routines.changes_from() + 255);
    u64 last = 0;
    usize all = 0;
    for (u64 after = 0;;) {
        got = f.run("get_person_events", {{"after", after}, {"limit", 256}}).value;
        if (got["events"].empty()) break;
        for (const Json& e : got["events"]) {
            GIANT_CHECK(e["serial"].get<u64>() > last);
            last = e["serial"].get<u64>();
            ++all;
        }
        after = got["latest"].get<u64>();
    }
    GIANT_CHECK(all == PeopleRoutines::k_changes_kept && last == made);
    // Another scene: what happened in the last is not told in this one.
    f.state.scene = world::Scene();
    f.frames(0.1, 1);
    got = f.run("get_person_events").value;
    GIANT_CHECK(got["events"].empty() && got["oldest"] == made + 1 && got["latest"] == made);
}
