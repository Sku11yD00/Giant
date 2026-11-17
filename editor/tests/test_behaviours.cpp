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
    // A shot in their midst: those who hear it loud flee, at most `sent_a_frame` set off a frame, and all in the end.
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
