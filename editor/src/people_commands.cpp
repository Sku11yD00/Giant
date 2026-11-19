// People commands: the people who live in the world, their looks, the
// places they go to, their routines and their behaviours (world/people.h;
// living-world plan LW1, LW3 and LW6). Scene data
// (saved with the scene and the world's world.json, undoable). People are
// many, so an undo step keeps what one command changed, never the lists.
#include "editor_commands.h"

#include "audio_state.h"
#include "people_figures.h"
#include "people_list.h"
#include "people_routines.h"
#include "people_senses.h"
#include "people_state.h"
#include "people_travel.h"
#include "physics_state.h"
#include "play_state.h"
#include "pose_bakes.h"

#include "giant/core/assert.h"
#include "giant/world/people.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace giant::editor {

namespace {

using ai::CommandContext;
using ai::CommandResult;
using ai::Json;
using world::Person;
using world::PersonId;
using world::PersonLook;
using world::Place;
using world::Behaviour;
using world::Routine;

constexpr usize k_most_a_call = 4096; // people made or removed by one command
constexpr usize k_most_listed = 1000; // people or places one get_ lists

void add(ai::CommandRegistry& registry, ai::CommandDesc desc) {
    const bool added = registry.add(std::move(desc));
    GIANT_ASSERT(added, "people command could not be registered");
}

Json name_schema(const char* description) {
    return {{"type", "string"}, {"minLength", 1}, {"maxLength", world::k_max_name_bytes}, {"description", description}};
}
Json point_schema(const char* description) {
    return {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3},
            {"description", description}};
}
Json yaw_schema(const char* description) {
    return {{"type", "number"}, {"minimum", -360}, {"maximum", 360}, {"description", description}};
}
Json object_schema(Json required, Json properties) {
    Json schema = {{"type", "object"}, {"additionalProperties", false}, {"properties", std::move(properties)}};
    if (!required.empty()) schema["required"] = std::move(required);
    return schema;
}

DVec3 point(const Json& j) { return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()}; }
Json point_json(const DVec3& p) { return Json::array({p.x, p.y, p.z}); }

Json place_json(const Place& p) {
    return {{"name", p.name}, {"position", point_json(p.position)}, {"radius", p.radius}, {"yaw", p.yaw}, {"tags", p.tags}};
}

Json look_json(const PersonLook& l, usize people) {
    Json parts = Json::array();
    for (const world::PartLook& p : l.parts) parts.push_back(p.name);
    Json activities = Json::array();
    for (const world::LookActivity& a : l.activities) {
        activities.push_back({{"activity", a.activity}, {"clip", a.clip}, {"speed", a.speed}, {"loop", a.loop}});
    }
    return {{"name", l.name},       {"mesh", l.mesh},        {"height", l.height}, {"parts", std::move(parts)},
            {"hair", l.hair.on},    {"matched", l.matched},  {"activities", std::move(activities)}, {"people", people}};
}

Json routine_json(const Routine& r, usize people) {
    Json entries = Json::array();
    for (const world::RoutineEntry& e : r.entries) {
        entries.push_back({{"from", e.from}, {"activity", e.activity}, {"at", world::routine_at_name(e.at)},
                           {"name", e.name}, {"speed", e.speed}});
    }
    Json alarmed = Json::array();
    for (const world::RoutineAlarm& a : r.alarmed) {
        alarmed.push_back({{"over", a.over}, {"activity", a.does.activity}, {"at", world::routine_at_name(a.does.at)},
                           {"name", a.does.name}, {"speed", a.does.speed}});
    }
    return {{"name", r.name}, {"entries", std::move(entries)}, {"alarmed", std::move(alarmed)}, {"people", people}};
}

// A behaviour as commands give and take it (what set_behaviour is given it takes back, but for `people`).
Json behaviour_json(const Behaviour& b, usize people) {
    Json states = Json::array();
    for (const world::BehaviourState& s : b.states) {
        Json state = {{"name", s.name}};
        if (s.own) {
            state["activity"] = s.does.activity;
            state["at"] = world::routine_at_name(s.does.at);
            if (!s.does.name.empty()) state["place"] = s.does.name;
            state["speed"] = s.does.speed;
        }
        Json transitions = Json::array();
        for (const world::BehaviourTransition& t : s.transitions) {
            Json when = Json::array();
            for (const world::BehaviourCondition& c : t.conditions) {
                Json condition = {{"is", world::behaviour_when_name(c.when)}};
                switch (c.when) {
                case world::BehaviourWhen::alarm_over:
                case world::BehaviourWhen::alarm_under:
                case world::BehaviourWhen::after: condition["value"] = c.value; break;
                case world::BehaviourWhen::hour:
                    condition["value"] = c.value;
                    condition["until"] = c.until;
                    break;
                case world::BehaviourWhen::sensed: condition["kind"] = c.kind; break;
                case world::BehaviourWhen::arrived:
                case world::BehaviourWhen::failed: break;
                }
                when.push_back(std::move(condition));
            }
            transitions.push_back({{"to", t.to}, {"when", std::move(when)}});
        }
        state["transitions"] = std::move(transitions);
        states.push_back(std::move(state));
    }
    return {{"name", b.name}, {"states", std::move(states)}, {"people", people}};
}

// The travel's clock brought up to the physics' now (a command between frames: time stepped on
// by step_physics, or the simulation started or stopped, since the last frame counts).
void sync_travel(EditorState& state) {
    if (state.travel == nullptr || state.physics == nullptr) return;
    const PeopleTravel::Clock now = PeopleTravel::clock_of(*state.physics);
    state.travel->sync(state.scene, now.run, now.seconds);
}

Json trip_json(const PeopleTravel& travel, const PeopleTravel::Trip& t) {
    Json j = {{"phase", PeopleTravel::to_string(t.phase)}, {"to", point_json(t.to)}, {"speed", t.speed}, {"embodied", t.held}};
    if (!t.place.empty()) j["place"] = t.place;
    if (t.phase == PeopleTravel::Phase::travelling) {
        const double gone = travel.gone(t), length = PeopleTravel::length(t);
        j["gone"] = gone;
        j["length"] = length;
        j["arrives_in"] = t.held ? Json(nullptr) : Json((length - gone) / t.speed); // (as fast as its entity walks)
    }
    if (t.phase == PeopleTravel::Phase::failed) j["why"] = t.reason;
    return j;
}

// `people`: who is given an entity now (null: nobody is), named as `entity`.
Json person_json(const Person& p, const PeopleState* people = nullptr, const world::Scene* scene = nullptr,
                 const PeopleTravel* travel = nullptr, const PeopleRoutines* routines = nullptr,
                 const PeopleSenses* senses = nullptr, const EditorState* state = nullptr) {
    Json j = {{"id", p.id},     {"name", p.name}, {"look", p.look}, {"home", p.home}, {"routine", p.routine}, {"behaviour", p.behaviour},
              {"position", point_json(p.position)}, {"yaw", p.yaw},   {"seed", p.seed}};
    if (scene != nullptr) {
        const world::EntityId entity = people != nullptr ? people->entity_of(*scene, p.id) : 0;
        j["entity"] = entity != 0 ? Json(entity) : Json(nullptr);
        const PeopleTravel::Where at = travel != nullptr ? travel->where(p) : PeopleTravel::Where{p.position, p.yaw};
        j["at"] = point_json(at.position);
        const PeopleTravel::Trip* trip = travel != nullptr ? travel->trip(p.id) : nullptr;
        j["trip"] = trip != nullptr ? trip_json(*travel, *trip) : Json(nullptr);
        PeopleRoutines::Doing doing;
        if (routines != nullptr && routines->doing(*scene, p.id, doing)) {
            j["doing"] = {{"activity", doing.activity}, {"place", doing.place}, {"failed", doing.failed}, {"own", doing.own}};
        } else {
            j["doing"] = nullptr;
        }
        PeopleRoutines::Behaving behaving;
        if (routines != nullptr && travel != nullptr && routines->behaving(*scene, *travel, p.id, behaving)) {
            j["state"] = {{"name", behaving.state}, {"index", behaving.index}, {"seconds", behaving.seconds}};
        } else {
            j["state"] = nullptr;
        }
        PeopleSenses::Heard heard;
        if (senses != nullptr && senses->heard(*scene, p.id, heard)) {
            j["alarm"] = heard.alarm;
            j["heard"] = {{"kind", PeopleSenses::to_string(heard.kind)},
                          {"from", point_json(heard.from)},
                          {"level", heard.level},
                          {"seconds", heard.seconds}};
        } else {
            j["alarm"] = 0.0;
            j["heard"] = nullptr;
        }
        if (state != nullptr) j["why"] = person_why(*state, p);
    }
    return j;
}

Json people_state_json(const PeopleState& people) {
    const PeopleState::Settings& s = people.settings;
    const PeopleState::Stats& t = people.stats();
    return {{"on", s.on},
            {"embody_distance", s.embody_distance},
            {"release_distance", s.release_distance},
            {"view_distance", s.view_distance},
            {"most", s.most},
            {"made_a_frame", s.made_a_frame},
            {"scanned_a_frame", s.scanned_a_frame},
            {"embodied", t.embodied},
            {"waiting", t.waiting},
            {"made", t.made},
            {"released", t.released},
            {"walking", t.walking},
            {"acting", t.acting},
            {"unseen", t.unseen},
            {"swapped", t.swapped},
            {"appeared_in_view", t.appeared_in_view},
            {"vanished_in_view", t.vanished_in_view}};
}

// What one command changed. A place, a look, a routine or a behaviour: the one at `index` before
// and after (none before: put in there; none after: taken out). People: the
// ones it took out and the ones it put in, by id (a changed person is both).
template <typename T>
struct Edit {
    bool any = false;
    usize index = 0;
    std::optional<T> before, after;
};
struct PeopleChange {
    Edit<Place> place;
    Edit<PersonLook> look;
    Edit<Routine> routine;
    Edit<Behaviour> behaviour;
    std::vector<Person> out, in; // ids rising
    // The scene's next id after it (0: as it was). Undoing never gives an
    // id back: a save made meanwhile may hold the person it was.
    PersonId next_after = 0;
};

// The scene's looks and places by name, made once for a command or an undo
// step: thousands of people are checked against tens of thousands of
// places. Sorted, not hashed: n log n whatever the names (a loaded file's
// may be made to collide).
class Names {
public:
    explicit Names(const world::Scene& scene) {
        m_looks.reserve(scene.person_looks.size());
        for (const PersonLook& l : scene.person_looks) m_looks.push_back(l.name);
        std::sort(m_looks.begin(), m_looks.end());
        m_places.reserve(scene.places.size());
        for (const Place& p : scene.places) m_places.emplace_back(p.name, &p);
        std::sort(m_places.begin(), m_places.end());
        m_routines.reserve(scene.routines.size());
        for (const Routine& r : scene.routines) m_routines.push_back(r.name);
        std::sort(m_routines.begin(), m_routines.end());
        m_behaviours.reserve(scene.behaviours.size());
        for (const Behaviour& b : scene.behaviours) m_behaviours.push_back(b.name);
        std::sort(m_behaviours.begin(), m_behaviours.end());
    }
    bool has_look(std::string_view name) const { return std::binary_search(m_looks.begin(), m_looks.end(), name); }
    bool has_routine(std::string_view name) const { return std::binary_search(m_routines.begin(), m_routines.end(), name); }
    bool has_behaviour(std::string_view name) const { return std::binary_search(m_behaviours.begin(), m_behaviours.end(), name); }
    const Place* place(std::string_view name) const {
        const auto it = std::lower_bound(m_places.begin(), m_places.end(), name,
                                         [](const auto& entry, std::string_view n) { return entry.first < n; });
        return it == m_places.end() || it->first != name ? nullptr : it->second;
    }

private:
    std::vector<std::string_view> m_looks, m_routines, m_behaviours;
    std::vector<std::pair<std::string_view, const Place*>> m_places;
};

// The same asked of the scene's lists as they are: for one person.
class Lists {
public:
    explicit Lists(const world::Scene& scene) : m_scene(scene) {}
    bool has_look(std::string_view name) const { return world::find_person_look(m_scene.person_looks, name) != nullptr; }
    bool has_routine(std::string_view name) const { return world::find_routine(m_scene.routines, name) != nullptr; }
    bool has_behaviour(std::string_view name) const { return world::find_behaviour(m_scene.behaviours, name) != nullptr; }
    const Place* place(std::string_view name) const { return world::find_place(m_scene.places, name); }

private:
    const world::Scene& m_scene;
};

// Does a place's, a look's, a routine's or a behaviour's edit, or undoes it. The one it changes is
// found by its name, the index only says where one is put back: commands
// run outside the history (scripts) may have changed the list since. What
// would leave the scene broken is left undone: putting in a name that is
// there or one more than the `most` a scene keeps, taking out one that is
// not or that `in_use` says people still have.
template <typename T, typename InUse>
void apply(std::vector<T>& list, const Edit<T>& edit, bool forward, usize most, InUse in_use) {
    if (!edit.any) return;
    const std::optional<T>& from = forward ? edit.before : edit.after;
    const std::optional<T>& to = forward ? edit.after : edit.before;
    const auto named = [&](const std::string& name) {
        return std::find_if(list.begin(), list.end(), [&](const T& t) { return t.name == name; });
    };
    if (!from) {
        if (named(to->name) != list.end() || list.size() >= most) return;
        list.insert(list.begin() + static_cast<std::ptrdiff_t>(std::min(edit.index, list.size())), *to);
        return;
    }
    const auto at = named(from->name);
    if (at == list.end()) return;
    if (!to) {
        if (!in_use(from->name)) list.erase(at);
    } else {
        *at = *to;
    }
}

// Takes `out` from the people and puts `in` among them (both and the people
// by rising id): once through the list, whatever their number. One put in
// whose look is gone since is left out, one whose home, routine or behaviour is gone has none.
void swap_people(world::Scene& scene, const std::vector<Person>& out, const std::vector<Person>& in) {
    if (out.empty() && in.empty()) return;
    std::vector<Person>& people = scene.people;
    std::optional<Names> names;
    if (!in.empty()) names.emplace(scene);
    std::vector<Person> next;
    next.reserve(people.size() + in.size());
    const auto put = [&](const Person& p) {
        if (!names->has_look(p.look)) return;
        next.push_back(p);
        if (!p.home.empty() && names->place(p.home) == nullptr) next.back().home.clear();
        if (!p.routine.empty() && !names->has_routine(p.routine)) next.back().routine.clear();
        if (!p.behaviour.empty() && !names->has_behaviour(p.behaviour)) next.back().behaviour.clear();
    };
    usize o = 0, i = 0;
    for (Person& p : people) {
        while (i < in.size() && in[i].id < p.id) put(in[i++]);
        while (o < out.size() && out[o].id < p.id) ++o;
        if (o < out.size() && out[o].id == p.id) continue;
        if (i < in.size() && in[i].id == p.id) continue; // (put back under an id still there: `in` wins, below)
        next.push_back(std::move(p));
    }
    while (i < in.size()) put(in[i++]);
    people = std::move(next);
}

void apply(world::Scene& scene, const PeopleChange& change, bool forward) {
    apply(scene.places, change.place, forward, world::k_max_places, [&](const std::string& name) {
        return std::any_of(scene.people.begin(), scene.people.end(), [&](const Person& p) { return p.home == name; });
    });
    apply(scene.person_looks, change.look, forward, world::k_max_person_looks, [&](const std::string& name) {
        return std::any_of(scene.people.begin(), scene.people.end(), [&](const Person& p) { return p.look == name; });
    });
    apply(scene.routines, change.routine, forward, world::k_max_routines, [&](const std::string& name) {
        return std::any_of(scene.people.begin(), scene.people.end(), [&](const Person& p) { return p.routine == name; });
    });
    apply(scene.behaviours, change.behaviour, forward, world::k_max_behaviours, [&](const std::string& name) {
        return std::any_of(scene.people.begin(), scene.people.end(), [&](const Person& p) { return p.behaviour == name; });
    });
    swap_people(scene, forward ? change.out : change.in, forward ? change.in : change.out);
    if (forward) scene.next_person_id = std::max(scene.next_person_id, change.next_after);
}

// Does the change and records it.
void commit(EditorState& state, CommandContext& context, PeopleChange change) {
    apply(state.scene, change, true);
    const auto kept = std::make_shared<const PeopleChange>(std::move(change));
    context.record(scene_change(
        state, [&state, kept] { apply(state.scene, *kept, false); }, [&state, kept] { apply(state.scene, *kept, true); }));
}

usize index_of_place(const std::vector<Place>& places, const std::string& name) {
    const Place* p = world::find_place(places, name);
    return p == nullptr ? places.size() : static_cast<usize>(p - places.data());
}
usize index_of_look(const std::vector<PersonLook>& looks, const std::string& name) {
    const PersonLook* l = world::find_person_look(looks, name);
    return l == nullptr ? looks.size() : static_cast<usize>(l - looks.data());
}

// Sets a place's fields given.
std::string apply_place(const Json& args, Place& p) {
    if (args.contains("position")) p.position = point(args["position"]);
    p.radius = args.value("radius", p.radius);
    p.yaw = args.value("yaw", p.yaw);
    if (args.contains("tags")) p.tags = args["tags"].get<std::vector<std::string>>();
    if (const auto bad = world::check_place(p)) return *bad;
    return {};
}

// Sets a person's fields given; a new one given no position stands at its
// home, facing its way.
template <typename Known>
std::string apply_person(const Json& args, const Known& names, Person& p, bool fresh) {
    if (args.contains("name")) p.name = args["name"].get<std::string>();
    if (args.contains("look")) p.look = args["look"].get<std::string>();
    if (args.contains("home")) p.home = args["home"].get<std::string>();
    if (args.contains("routine")) p.routine = args["routine"].get<std::string>();
    if (!p.routine.empty() && !names.has_routine(p.routine)) {
        return "no routine named \"" + p.routine + "\" (set_routine makes one)";
    }
    if (args.contains("behaviour")) p.behaviour = args["behaviour"].get<std::string>();
    if (!p.behaviour.empty() && !names.has_behaviour(p.behaviour)) {
        return "no behaviour named \"" + p.behaviour + "\" (set_behaviour makes one)";
    }
    if (!names.has_look(p.look)) return "no person look named \"" + p.look + "\" (set_person_look makes one)";
    const Place* home = p.home.empty() ? nullptr : names.place(p.home);
    if (!p.home.empty() && home == nullptr) return "no place named \"" + p.home + "\" (create_place makes one)";
    if (args.contains("position")) {
        p.position = point(args["position"]);
    } else if (fresh) {
        if (home == nullptr) return "a person needs a position or a home";
        p.position = home->position;
        if (!args.contains("yaw")) p.yaw = home->yaw;
    }
    p.yaw = args.value("yaw", p.yaw);
    if (args.contains("seed")) p.seed = args["seed"].get<u32>();
    if (const auto bad = world::check_person(p)) return *bad;
    return {};
}

// A seed for a person given none: its id, well mixed.
u32 seed_of(PersonId id) {
    u32 x = id * 0x9e3779b9u;
    x ^= x >> 16;
    x *= 0x85ebca6bu;
    x ^= x >> 13;
    return x;
}

bool near_enough(const DVec3& a, const DVec3& b, double within) {
    const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz <= within * within;
}

Json person_properties() {
    return {{"name", {{"type", "string"}, {"maxLength", world::k_max_name_bytes}, {"description", "Need not be unique."}}},
            {"look", name_schema("A person look's name (set_person_look).")},
            {"home", {{"type", "string"}, {"maxLength", world::k_max_name_bytes},
                      {"description", "A place's name (\"\" for none)."}}},
            {"routine", {{"type", "string"}, {"maxLength", world::k_max_name_bytes},
                         {"description", "A routine's name: what it does through the day (set_routine; \"\" for none)."}}},
            {"behaviour", {{"type", "string"}, {"maxLength", world::k_max_name_bytes},
                           {"description", "A behaviour's name: what it does as things happen to it (set_behaviour; \"\" for none)."}}},
            {"position", point_schema("Where it starts, metres (a new person given none starts at its home).")},
            {"yaw", yaw_schema("Degrees (a new person given no position faces its home's way; else 0).")},
            {"seed", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4294967295.0},
                      {"description", "What tells it from others of its look (default: from its id)."}}}};
}

Json list_properties(const char* what) {
    return {{"near", point_schema("Only those within `within` metres of here.")},
            {"within", {{"type", "number"}, {"minimum", 0}, {"maximum", 1e8}, {"description", "Metres (default 100)."}}},
            {"offset", {{"type", "integer"}, {"minimum", 0}, {"maximum", 1 << 20},
                        {"description", std::string("How many matching ") + what + " to skip (default 0)."}}},
            {"limit", {{"type", "integer"}, {"minimum", 0}, {"maximum", k_most_listed},
                       {"description", "The most listed (default 100; 0 only counts)."}}}};
}

} // namespace

void register_people_commands(ai::CommandRegistry& registry, EditorState& state) {
    // --- Places ---------------------------------------------------------------
    const Json place_fields = {
        {"name", name_schema("Unique among the places.")},
        {"position", point_schema("Its middle, on the ground, metres.")},
        {"radius", {{"type", "number"}, {"minimum", 0.25}, {"maximum", 1000}, {"description", "Metres: anywhere within is there (default 1)."}}},
        {"yaw", yaw_schema("Degrees: the way one stands there, like a bench's front (default 0).")},
        {"tags", {{"type", "array"},
                  {"maxItems", world::k_max_place_tags},
                  {"items", {{"type", "string"}, {"minLength", 1}, {"maxLength", world::k_max_tag_bytes}}},
                  {"description", "What it is for, each once, of a-z, 0-9 and _: home, work, rest, any words."}}}};
    add(registry,
        {"create_place",
         "Adds a place: a named spot people go to (a home, a workplace, a bench) with a `radius` and `tags` "
         "saying what it is for. People name one as their home. Saved with the scene or the world (not with "
         "cells: at most 65536). Undoable.",
         object_schema({"name", "position"}, place_fields),
         false,
         [&state](const Json& args, CommandContext& context) {
             std::vector<Place>& places = state.scene.places;
             if (places.size() >= world::k_max_places) {
                 return CommandResult::failure("a scene has at most " + std::to_string(world::k_max_places) + " places");
             }
             Place p;
             p.name = args["name"].get<std::string>();
             if (world::find_place(places, p.name) != nullptr) {
                 return CommandResult::failure("there is a place named \"" + p.name + "\" already: set_place changes it");
             }
             if (std::string bad = apply_place(args, p); !bad.empty()) return CommandResult::failure(bad);
             PeopleChange change;
             change.place = {true, places.size(), std::nullopt, p};
             commit(state, context, std::move(change));
             return CommandResult::success(place_json(p));
         }});
    add(registry,
        {"set_place",
         "Changes a place (by name): only the fields given (tags replace the list). Undoable.",
         object_schema({"name"}, place_fields),
         false,
         [&state](const Json& args, CommandContext& context) {
             std::vector<Place>& places = state.scene.places;
             const std::string name = args["name"].get<std::string>();
             const usize index = index_of_place(places, name);
             if (index == places.size()) return CommandResult::failure("no place named \"" + name + "\"");
             Place changed = places[index];
             if (std::string bad = apply_place(args, changed); !bad.empty()) return CommandResult::failure(bad);
             PeopleChange change;
             change.place = {true, index, places[index], changed};
             commit(state, context, std::move(change));
             return CommandResult::success(place_json(changed));
         }});
    add(registry,
        {"remove_place",
         "Removes a place (by name). Refused while it is anyone's home (set_person or remove_people first). Undoable.",
         object_schema({"name"}, {{"name", name_schema("The place.")}}),
         false,
         [&state](const Json& args, CommandContext& context) {
             std::vector<Place>& places = state.scene.places;
             const std::string name = args["name"].get<std::string>();
             const usize index = index_of_place(places, name);
             if (index == places.size()) return CommandResult::failure("no place named \"" + name + "\"");
             const auto lives = std::count_if(state.scene.people.begin(), state.scene.people.end(),
                                              [&](const Person& p) { return p.home == name; });
             if (lives != 0) {
                 return CommandResult::failure("\"" + name + "\" is the home of " + std::to_string(lives) +
                                               " people: give them another (set_person) or remove them first");
             }
             PeopleChange change;
             change.place = {true, index, places[index], std::nullopt};
             commit(state, context, std::move(change));
             return CommandResult::success({{"removed", name}});
         }});
    Json places_props = list_properties("places");
    places_props["tag"] = {{"type", "string"}, {"minLength", 1}, {"maxLength", world::k_max_tag_bytes},
                           {"description", "Only places with this tag."}};
    add(registry,
        {"get_places",
         "Lists places: all, or those with `tag` and/or within `within` metres of `near`, `limit` at a time "
         "from `offset` (in the order made). `count` is how many match, `total` how many there are.",
         object_schema(Json::array(), places_props),
         true,
         [&state](const Json& args, CommandContext&) {
             const std::string tag = args.value("tag", std::string());
             const bool by_place = args.contains("near");
             const DVec3 near = by_place ? point(args["near"]) : DVec3{};
             const double within = args.value("within", 100.0);
             const usize offset = args.value("offset", usize{0}), limit = args.value("limit", usize{100});
             usize count = 0;
             Json list = Json::array();
             for (const Place& p : state.scene.places) {
                 if (!tag.empty() && std::find(p.tags.begin(), p.tags.end(), tag) == p.tags.end()) continue;
                 if (by_place && !near_enough(p.position, near, within)) continue;
                 if (count >= offset && list.size() < limit) list.push_back(place_json(p));
                 ++count;
             }
             return CommandResult::success(
                 {{"count", count}, {"total", state.scene.places.size()}, {"places", std::move(list)}});
         }});

    // --- Looks ----------------------------------------------------------------
    add(registry,
        {"set_person_look",
         "Makes or changes a person look: how a kind of person looks, shared by many people. `entity` (an "
         "id) gives it that entity's look: its mesh file (it must draw one), material, parts' looks, hair "
         "and matched clips, and its scale as `height`; so dress a character with create_entity/set_entity, "
         "then keep it as a look (the entity is not needed after). `height` and `matched` set those alone. "
         "`activities` (replaces the list; `entity` leaves it be): what people of this look show while they "
         "do an activity of their routine where it has them be, each `{activity, clip, speed, loop}`: the "
         "file's `clip` plays (at `speed`, default 1; `loop` false: once, its last pose held) instead of "
         "the motion matching, which takes over when they walk. An activity with none shows as standing. "
         "A new look needs `entity`. Saved with the scene or the world (at most 1024). Undoable.",
         object_schema({"name"},
                       {{"name", name_schema("Unique among the person looks.")},
                        {"entity", {{"type", "integer"}, {"minimum", 1}, {"description", "The entity whose look it takes."}}},
                        {"height", {{"type", "number"}, {"minimum", 0.1}, {"maximum", 10},
                                    {"description", "The scale people of this look are drawn at, all axes."}}},
                        {"matched", {{"type", "array"},
                                     {"maxItems", world::k_max_matched_clips},
                                     {"items", {{"type", "string"}, {"minLength", 1}, {"maxLength", world::k_max_name_bytes}}},
                                     {"description", "The clips they are motion matched among ([\"*\"] or none: every clip)."}}},
                        {"activities",
                         {{"type", "array"},
                          {"maxItems", world::k_max_look_activities},
                          {"items", object_schema({"activity", "clip"},
                                                  {{"activity", {{"type", "string"}, {"minLength", 1}, {"maxLength", world::k_max_tag_bytes}}},
                                                   {"clip", {{"type", "string"}, {"minLength", 1}, {"maxLength", world::k_max_name_bytes}}},
                                                   {"speed", {{"type", "number"}, {"minimum", 0.01}, {"maximum", 100}}},
                                                   {"loop", {{"type", "boolean"}}}})},
                          {"description", "The clip shown for each activity of a routine (each activity once)."}}}}),
         false,
         [&state](const Json& args, CommandContext& context) {
             std::vector<PersonLook>& looks = state.scene.person_looks;
             const std::string name = args["name"].get<std::string>();
             const usize index = index_of_look(looks, name);
             const bool fresh = index == looks.size();
             if (fresh && looks.size() >= world::k_max_person_looks) {
                 return CommandResult::failure("a scene has at most " + std::to_string(world::k_max_person_looks) +
                                               " person looks");
             }
             if (fresh && !args.contains("entity")) {
                 return CommandResult::failure("no person look named \"" + name + "\": a new one needs `entity`");
             }
             PersonLook look = fresh ? PersonLook{} : looks[index];
             look.name = name;
             if (args.contains("entity")) {
                 const world::EntityId id = args["entity"].get<world::EntityId>();
                 const world::Entity* e = state.scene.find(id);
                 if (e == nullptr) return CommandResult::failure("no entity " + std::to_string(id));
                 if (e->shape != world::Shape::mesh || e->mesh.empty()) {
                     return CommandResult::failure("entity " + std::to_string(id) + " does not draw a mesh file");
                 }
                 const Vec3 scale = e->transform.scale;
                 look.mesh = e->mesh;
                 look.height = (scale.x + scale.y + scale.z) / 3.0f;
                 look.material = e->material;
                 look.parts = e->parts;
                 look.hair = e->hair;
                 look.matched = e->animation.matched;
             }
             look.height = args.value("height", look.height);
             if (args.contains("matched")) look.matched = args["matched"].get<std::vector<std::string>>();
             if (args.contains("activities")) {
                 look.activities.clear();
                 for (const Json& a : args["activities"]) {
                     world::LookActivity activity;
                     activity.activity = a["activity"].get<std::string>();
                     activity.clip = a["clip"].get<std::string>();
                     activity.speed = a.value("speed", 1.0f);
                     activity.loop = a.value("loop", true);
                     look.activities.push_back(std::move(activity));
                 }
             }
             if (const auto bad = world::check_person_look(look)) return CommandResult::failure(*bad);
             PeopleChange change;
             change.look = {true, index, fresh ? std::nullopt : std::optional<PersonLook>(looks[index]), look};
             commit(state, context, std::move(change));
             const auto people = std::count_if(state.scene.people.begin(), state.scene.people.end(),
                                               [&](const Person& p) { return p.look == name; });
             return CommandResult::success(look_json(look, static_cast<usize>(people)));
         }});
    add(registry,
        {"remove_person_look",
         "Removes a person look (by name). Refused while people have it (set_person or remove_people first). Undoable.",
         object_schema({"name"}, {{"name", name_schema("The look.")}}),
         false,
         [&state](const Json& args, CommandContext& context) {
             std::vector<PersonLook>& looks = state.scene.person_looks;
             const std::string name = args["name"].get<std::string>();
             const usize index = index_of_look(looks, name);
             if (index == looks.size()) return CommandResult::failure("no person look named \"" + name + "\"");
             const auto people = std::count_if(state.scene.people.begin(), state.scene.people.end(),
                                               [&](const Person& p) { return p.look == name; });
             if (people != 0) {
                 return CommandResult::failure(std::to_string(people) + " people have the look \"" + name +
                                               "\": give them another (set_person) or remove them first");
             }
             PeopleChange change;
             change.look = {true, index, looks[index], std::nullopt};
             commit(state, context, std::move(change));
             return CommandResult::success({{"removed", name}});
         }});
    add(registry,
        {"get_person_looks",
         "Lists the person looks: each one's mesh file, height, the parts it gives looks of their own, "
         "whether it wears hair, its matched clips, its `activities` (the clip shown for each), and how many `people` have it.",
         object_schema(Json::array(), Json::object()),
         true,
         [&state](const Json&, CommandContext&) {
             std::unordered_map<std::string_view, usize> people;
             for (const Person& p : state.scene.people) ++people[p.look];
             Json list = Json::array();
             for (const PersonLook& l : state.scene.person_looks) list.push_back(look_json(l, people[l.name]));
             return CommandResult::success({{"person_looks", std::move(list)}});
         }});

    // --- Routines -------------------------------------------------------------
    const Json entry_schema = object_schema(
        {"from", "activity"},
        {{"from", {{"type", "number"}, {"minimum", 0}, {"maximum", 24},
                   {"description", "The hour of the day it begins (0 to under 24): later than the entry's before it."}}},
         {"activity", {{"type", "string"}, {"minLength", 1}, {"maxLength", world::k_max_tag_bytes},
                       {"description", "What one does then, a word of a-z, 0-9 and _: sleep, work, eat, any word."}}},
         {"at", {{"type", "string"}, {"enum", {"stay", "home", "place", "tag"}},
                 {"description", "Where: `stay` where it is (default), its `home`, the `place` named by `name`, or a place "
                                 "with the `tag` in `name` (each person its own: the nearest to its home)."}}},
         {"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", world::k_max_name_bytes},
                   {"description", "With `at` place: a place's name (it must be there). With `at` tag: a place's tag."}}},
         {"speed", {{"type", "number"}, {"minimum", 0.1}, {"maximum", 20}, {"description", "Metres a second going there (default 1.4)."}}}});
    Json alarm_schema = entry_schema;
    alarm_schema["required"] = Json::array({"over", "activity"});
    alarm_schema["properties"].erase("from");
    alarm_schema["properties"]["over"] = {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1},
                                           {"description", "The alarm (get_people) at or over which it holds: more than the one's before it."}};
    add(registry,
        {"set_routine",
         "Makes or replaces a routine: what the people who have it (create_people / set_person `routine`) do "
         "through the day. `entries` (1 to 24, by rising `from`): each begins at its hour and holds until the "
         "next; the last holds over midnight until the first. Saved with the scene or the world (at most "
         "1024). `alarmed` (at most 8; left out: none): what one does instead while alarmed by what it "
         "heard or saw, each as an entry with `over` in place of `from`: the last whose `over` its alarm "
         "is at or over holds (so {over 0.2, activity watch} and {over 0.6, activity flee, at home, speed "
         "5} has one stop and watch, then run home), and one on it stays until its alarm is 0.05 under; "
         "calm again it does what the hour says. Undoable.",
         object_schema({"name", "entries"},
                       {{"name", name_schema("Unique among the routines.")},
                        {"entries", {{"type", "array"}, {"minItems", 1}, {"maxItems", world::k_max_routine_entries},
                                     {"items", entry_schema}}},
                        {"alarmed", {{"type", "array"}, {"maxItems", world::k_max_routine_alarms}, {"items", alarm_schema}}}}),
         false,
         [&state](const Json& args, CommandContext& context) {
             std::vector<Routine>& routines = state.scene.routines;
             Routine routine;
             routine.name = args["name"].get<std::string>();
             const Routine* had = world::find_routine(routines, routine.name);
             if (had == nullptr && routines.size() >= world::k_max_routines) {
                 return CommandResult::failure("a scene has at most " + std::to_string(world::k_max_routines) + " routines");
             }
             const Json& entries = args["entries"];
             const Json alarmed = args.value("alarmed", Json::array());
             for (usize i = 0; i < entries.size() + alarmed.size(); ++i) {
                 const bool alarm = i >= entries.size();
                 const Json& j = alarm ? alarmed[i - entries.size()] : entries[i];
                 const std::string which = (alarm ? "alarmed[" + std::to_string(i - entries.size()) : "entries[" + std::to_string(i)) + "]: ";
                 world::RoutineEntry e;
                 if (!alarm) e.from = j["from"].get<float>();
                 e.activity = j["activity"].get<std::string>();
                 const std::optional<world::RoutineAt> where = world::routine_at_named(j.value("at", std::string("stay")));
                 if (!where) return CommandResult::failure(which + "at must be stay, home, place or tag");
                 e.at = *where;
                 e.name = j.value("name", std::string());
                 e.speed = j.value("speed", e.speed);
                 if (e.at == world::RoutineAt::place && !e.name.empty() && world::find_place(state.scene.places, e.name) == nullptr) {
                     return CommandResult::failure(which + "no place named \"" + e.name + "\" (create_place makes one)");
                 }
                 if (alarm) {
                     routine.alarmed.push_back({j["over"].get<float>(), std::move(e)});
                     continue;
                 }
                 routine.entries.push_back(std::move(e));
             }
             if (const auto bad = world::check_routine(routine)) return CommandResult::failure(*bad);
             PeopleChange change;
             const usize index = had == nullptr ? routines.size() : static_cast<usize>(had - routines.data());
             change.routine = {true, index, had == nullptr ? std::nullopt : std::optional<Routine>(*had), routine};
             commit(state, context, std::move(change));
             const auto people = std::count_if(state.scene.people.begin(), state.scene.people.end(),
                                               [&](const Person& p) { return p.routine == routine.name; });
             return CommandResult::success(routine_json(routine, static_cast<usize>(people)));
         }});
    add(registry,
        {"remove_routine",
         "Removes a routine (by name). Refused while people have it (set_person or remove_people first). Undoable.",
         object_schema({"name"}, {{"name", name_schema("The routine.")}}),
         false,
         [&state](const Json& args, CommandContext& context) {
             std::vector<Routine>& routines = state.scene.routines;
             const std::string name = args["name"].get<std::string>();
             const Routine* had = world::find_routine(routines, name);
             if (had == nullptr) return CommandResult::failure("no routine named \"" + name + "\"");
             const auto people = std::count_if(state.scene.people.begin(), state.scene.people.end(),
                                               [&](const Person& p) { return p.routine == name; });
             if (people != 0) {
                 return CommandResult::failure(std::to_string(people) + " people have the routine \"" + name +
                                               "\": give them another (set_person) or remove them first");
             }
             PeopleChange change;
             change.routine = {true, static_cast<usize>(had - routines.data()), *had, std::nullopt};
             commit(state, context, std::move(change));
             return CommandResult::success({{"removed", name}});
         }});
    add(registry,
        {"get_routines",
         "Lists the routines: each one's entries and how many `people` have it. With `hour` (0 to 24), each "
         "also names `now`: the index of the entry that holds at that hour (24 is midnight, as 0).",
         object_schema(Json::array(), {{"hour", {{"type", "number"}, {"minimum", 0}, {"maximum", 24},
                                                 {"description", "An hour of the day to name each routine's entry at."}}}}),
         true,
         [&state](const Json& args, CommandContext&) {
             // How many have each: by sorted names (not hashed: a loaded file's may be made to collide).
             const std::vector<Routine>& routines = state.scene.routines;
             std::vector<std::pair<std::string_view, usize>> names;
             names.reserve(routines.size());
             for (usize i = 0; i < routines.size(); ++i) names.emplace_back(routines[i].name, i);
             std::sort(names.begin(), names.end());
             std::vector<usize> people(routines.size(), 0);
             for (const Person& p : state.scene.people) {
                 if (p.routine.empty()) continue;
                 const auto it = std::lower_bound(names.begin(), names.end(), std::string_view(p.routine),
                                                  [](const auto& entry, std::string_view n) { return entry.first < n; });
                 if (it != names.end() && it->first == p.routine) ++people[it->second];
             }
             Json list = Json::array();
             for (usize i = 0; i < routines.size(); ++i) {
                 const Routine& r = routines[i];
                 Json j = routine_json(r, people[i]);
                 if (args.contains("hour")) j["now"] = world::routine_entry_at(r, args["hour"].get<double>());
                 list.push_back(std::move(j));
             }
             return CommandResult::success({{"routines", std::move(list)}});
         }});

    // --- Behaviours -----------------------------------------------------------
    const Json condition_schema = object_schema(
        {"is"},
        {{"is", {{"type", "string"}, {"enum", {"alarm_over", "alarm_under", "sensed", "arrived", "failed", "after", "hour"}},
                 {"description", "`alarm_over` / `alarm_under`: its alarm (get_people) is at or over / under `value` (over 0, to 1). "
                                 "`sensed`: what alarmed it most of late is of `kind`. `arrived`: it is where the state sent it "
                                 "(or the state sends it nowhere). `failed`: it could not be sent there. `after`: it has been in "
                                 "the state `value` seconds of the simulation. `hour`: the time of day is from `value` to `until`."}}},
         {"value", {{"type", "number"}, {"minimum", 0}, {"maximum", 86400},
                    {"description", "With alarm_over, alarm_under (over 0, to 1), after (seconds, over 0) and hour (0 to under 24)."}}},
         {"until", {{"type", "number"}, {"minimum", 0}, {"maximum", 24},
                    {"description", "With hour: when it ends (0 to under 24; less than `value`: over midnight)."}}},
         {"kind", {{"type", "string"}, {"enum", {"shot", "impact", "blast", "splash", "voice", "other", "sight"}},
                   {"description", "With sensed: a noise's kind, or sight."}}}});
    const Json transition_schema = object_schema(
        {"to", "when"},
        {{"to", {{"type", "string"}, {"minLength", 1}, {"maxLength", world::k_max_tag_bytes}, {"description", "Another state of the behaviour."}}},
         {"when", {{"type", "array"}, {"minItems", 1}, {"maxItems", world::k_max_behaviour_conditions}, {"items", condition_schema},
                   {"description", "The conditions: all must hold."}}}});
    const Json state_schema = object_schema(
        {"name"},
        {{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", world::k_max_tag_bytes},
                   {"description", "Unique in the behaviour: a word of a-z, 0-9 and _."}}},
         {"activity", {{"type", "string"}, {"minLength", 1}, {"maxLength", world::k_max_tag_bytes},
                       {"description", "What one does in the state, a word of a-z, 0-9 and _ (flee, hide, watch, any word). "
                                       "Left out: its routine says what it does (the hour's entry, or an alarmed one)."}}},
         {"at", {{"type", "string"}, {"enum", {"stay", "home", "place", "tag"}},
                 {"description", "With `activity`, where: `stay` where it is (default), its `home`, the place named by `place`, "
                                 "or a place with the tag in `place` (each person its own: the nearest to its home)."}}},
         {"place", {{"type", "string"}, {"minLength", 1}, {"maxLength", world::k_max_name_bytes},
                    {"description", "With `at` place: a place's name (it must be there). With `at` tag: a place's tag."}}},
         {"speed", {{"type", "number"}, {"minimum", 0.1}, {"maximum", 20}, {"description", "Metres a second going there (default 1.4)."}}},
         {"transitions", {{"type", "array"}, {"maxItems", world::k_max_behaviour_transitions}, {"items", transition_schema},
                          {"description", "When it goes to another state: the first whose conditions all hold, in this order."}}}});
    add(registry,
        {"set_behaviour",
         "Makes or replaces a behaviour: what the people who have it (create_people / set_person `behaviour`) do "
         "as things happen to them, as a small state machine. `states` (1 to 16): one with the behaviour is "
         "in one of them, the first to begin with. A state has what one does while in it (`activity` with "
         "`at`, `place`, `speed`, as a routine's entry; with no activity its routine says) and "
         "`transitions` (at most 8): each names another state `to` and is taken `when` all its conditions "
         "(1 to 4) hold. So a state `day` with {to flee, when [{is sensed, kind shot}]}, a state `flee` "
         "(activity flee, at home, speed 5) with {to hide, when [{is arrived}]} and a state `hide` (activity "
         "hide) with {to day, when [{is after, value 120}, {is alarm_under, value 0.1}]} has one run home "
         "from a shot, hide there, and go back to its day two minutes on once calm. People act on theirs "
         "while simulating (get_people's `state` and `doing`; set_routine_settings). Saved with the scene "
         "or the world (at most 256). Undoable.",
         object_schema({"name", "states"},
                       {{"name", name_schema("Unique among the behaviours.")},
                        {"states", {{"type", "array"}, {"minItems", 1}, {"maxItems", world::k_max_behaviour_states},
                                    {"items", state_schema}}}}),
         false,
         [&state](const Json& args, CommandContext& context) {
             std::vector<Behaviour>& behaviours = state.scene.behaviours;
             Behaviour behaviour;
             behaviour.name = args["name"].get<std::string>();
             const Behaviour* had = world::find_behaviour(behaviours, behaviour.name);
             if (had == nullptr && behaviours.size() >= world::k_max_behaviours) {
                 return CommandResult::failure("a scene has at most " + std::to_string(world::k_max_behaviours) + " behaviours");
             }
             const Json& states = args["states"];
             for (usize i = 0; i < states.size(); ++i) {
                 const Json& j = states[i];
                 const std::string which = "states[" + std::to_string(i) + "]: ";
                 world::BehaviourState s;
                 s.name = j["name"].get<std::string>();
                 s.own = j.contains("activity");
                 if (!s.own && (j.contains("at") || j.contains("place") || j.contains("speed"))) {
                     return CommandResult::failure(which + "at, place and speed are given only with an activity");
                 }
                 if (s.own) {
                     s.does.activity = j["activity"].get<std::string>();
                     const std::optional<world::RoutineAt> where = world::routine_at_named(j.value("at", std::string("stay")));
                     if (!where) return CommandResult::failure(which + "at must be stay, home, place or tag");
                     s.does.at = *where;
                     s.does.name = j.value("place", std::string());
                     s.does.speed = j.value("speed", s.does.speed);
                     if (s.does.at == world::RoutineAt::place && !s.does.name.empty() &&
                         world::find_place(state.scene.places, s.does.name) == nullptr) {
                         return CommandResult::failure(which + "no place named \"" + s.does.name + "\" (create_place makes one)");
                     }
                 }
                 const Json transitions = j.value("transitions", Json::array());
                 for (usize t = 0; t < transitions.size(); ++t) {
                     world::BehaviourTransition transition;
                     transition.to = transitions[t]["to"].get<std::string>();
                     for (const Json& w : transitions[t]["when"]) {
                         world::BehaviourCondition c;
                         const std::optional<world::BehaviourWhen> when = world::behaviour_when_named(w["is"].get<std::string>());
                         if (!when) return CommandResult::failure(which + "is must be a condition");
                         c.when = *when;
                         // Each has what it needs and nothing else (a number left out is not taken as 0).
                         const bool valued = *when == world::BehaviourWhen::alarm_over || *when == world::BehaviourWhen::alarm_under ||
                                             *when == world::BehaviourWhen::after || *when == world::BehaviourWhen::hour;
                         if (w.contains("value") != valued || w.contains("until") != (*when == world::BehaviourWhen::hour) ||
                             w.contains("kind") != (*when == world::BehaviourWhen::sensed)) {
                             return CommandResult::failure(which + "to " + transition.to + ": " + world::behaviour_when_name(*when) +
                                                           (valued ? (*when == world::BehaviourWhen::hour ? " takes a value and an until, and no kind"
                                                                                                          : " takes a value, and no until or kind")
                                                                   : (*when == world::BehaviourWhen::sensed ? " takes a kind, and no value or until"
                                                                                                            : " takes no value, until or kind")));
                         }
                         c.value = w.value("value", 0.0f);
                         c.until = w.value("until", 0.0f);
                         c.kind = w.value("kind", std::string());
                         transition.conditions.push_back(std::move(c));
                     }
                     s.transitions.push_back(std::move(transition));
                 }
                 behaviour.states.push_back(std::move(s));
             }
             if (const auto bad = world::check_behaviour(behaviour)) return CommandResult::failure(*bad);
             PeopleChange change;
             const auto people = std::count_if(state.scene.people.begin(), state.scene.people.end(),
                                               [&](const Person& p) { return p.behaviour == behaviour.name; });
             const usize index = had == nullptr ? behaviours.size() : static_cast<usize>(had - behaviours.data());
             change.behaviour = {true, index, had != nullptr ? std::optional<Behaviour>(*had) : std::nullopt, behaviour};
             commit(state, context, std::move(change));
             return CommandResult::success(behaviour_json(behaviour, static_cast<usize>(people)));
         }});
    add(registry,
        {"remove_behaviour",
         "Removes a behaviour (by name). Refused while people have it (set_person or remove_people first). Undoable.",
         object_schema({"name"}, {{"name", name_schema("The behaviour.")}}),
         false,
         [&state](const Json& args, CommandContext& context) {
             std::vector<Behaviour>& behaviours = state.scene.behaviours;
             const std::string name = args["name"].get<std::string>();
             const Behaviour* had = world::find_behaviour(behaviours, name);
             if (had == nullptr) return CommandResult::failure("no behaviour named \"" + name + "\"");
             const auto people = std::count_if(state.scene.people.begin(), state.scene.people.end(),
                                               [&](const Person& p) { return p.behaviour == name; });
             if (people != 0) {
                 return CommandResult::failure(std::to_string(people) + " people have the behaviour \"" + name +
                                               "\": give them another (set_person) or remove them first");
             }
             PeopleChange change;
             change.behaviour = {true, static_cast<usize>(had - behaviours.data()), *had, std::nullopt};
             commit(state, context, std::move(change));
             return CommandResult::success({{"removed", name}});
         }});
    add(registry,
        {"get_behaviours",
         "Lists the behaviours: each one's states (what one does in each, its transitions and their "
         "conditions) and how many `people` have it.",
         object_schema(Json::array(), Json::object()),
         true,
         [&state](const Json&, CommandContext&) {
             // How many have each: by sorted names (not hashed: a loaded file's may be made to collide).
             const std::vector<Behaviour>& behaviours = state.scene.behaviours;
             std::vector<std::pair<std::string_view, usize>> names;
             names.reserve(behaviours.size());
             for (usize i = 0; i < behaviours.size(); ++i) names.emplace_back(behaviours[i].name, i);
             std::sort(names.begin(), names.end());
             std::vector<usize> people(behaviours.size(), 0);
             for (const Person& p : state.scene.people) {
                 if (p.behaviour.empty()) continue;
                 const auto it = std::lower_bound(names.begin(), names.end(), std::string_view(p.behaviour),
                                                  [](const auto& entry, std::string_view n) { return entry.first < n; });
                 if (it != names.end() && it->first == p.behaviour) ++people[it->second];
             }
             Json list = Json::array();
             for (usize i = 0; i < behaviours.size(); ++i) list.push_back(behaviour_json(behaviours[i], people[i]));
             return CommandResult::success({{"behaviours", std::move(list)}});
         }});
    add(registry,
        {"get_person_events",
         "What happened to people's behaviours while the world ran, oldest first: each a `serial`, the "
         "`person`, its `behaviour`, the state it left (`from`: \"\" when it began the behaviour) and the "
         "one it is in since (`to`), and the simulation's `seconds` then. Give `after` (a serial: only later "
         "ones; `after` of the last answer goes on from there) and `limit` (1 to 256, default 64). `latest` "
         "is the serial of the last that happened (more than `after`: ask again). The last 16384 are kept "
         "(`oldest`: the first serial still kept). C# scripts are told the same as `OnPersonEvent`.",
         object_schema(Json::array(), {{"after", {{"type", "integer"}, {"minimum", 0}}},
                                       {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 256}}}}),
         true,
         [&state](const Json& args, CommandContext&) {
             Json list = Json::array();
             u64 after = 0, latest = 0, oldest = 1;
             if (state.routines != nullptr) {
                 const PeopleRoutines& routines = *state.routines;
                 u64 told = args.value("after", u64{0});
                 const usize limit = args.value("limit", usize{64});
                 std::vector<PeopleRoutines::Change> changes;
                 changes.reserve(limit);
                 routines.changes_since(state.scene, told, limit, changes);
                 for (const PeopleRoutines::Change& c : changes) {
                     list.push_back({{"serial", c.serial}, {"person", c.person}, {"behaviour", c.behaviour},
                                     {"from", c.from}, {"to", c.to}, {"seconds", c.seconds}});
                 }
                 after = told;
                 latest = routines.changes();
                 oldest = routines.changes_from();
             }
             return CommandResult::success({{"events", std::move(list)}, {"after", after}, {"latest", latest}, {"oldest", oldest}});
         }});

    // --- People ---------------------------------------------------------------
    add(registry,
        {"create_people",
         "Adds people (1 to 4096 a call; a scene keeps at most 65536): each a `look` (a person look's name) "
         "and a `position` or a `home` (a place's name; given no position it starts there). They are world "
         "data, not entities: kept with the scene or the world whichever cells are loaded. Returns their "
         "`ids` in order (an id is never used again, not even after undo). One undo step.",
         object_schema({"people"},
                       {{"people", {{"type", "array"},
                                    {"minItems", 1},
                                    {"maxItems", k_most_a_call},
                                    {"items", object_schema({"look"}, person_properties())}}}}),
         false,
         [&state](const Json& args, CommandContext& context) {
             const world::Scene& scene = state.scene;
             const Json& list = args["people"];
             if (scene.people.size() + list.size() > world::k_max_people) {
                 return CommandResult::failure("a scene has at most " + std::to_string(world::k_max_people) +
                                               " people (" + std::to_string(scene.people.size()) + " now)");
             }
             if (u64{scene.next_person_id} + list.size() > world::k_mod_person_base) { // (from there on: mods' people)
                 return CommandResult::failure("this scene has run out of person ids");
             }
             PeopleChange change;
             change.in.reserve(list.size());
             Json ids = Json::array();
             PersonId next = scene.next_person_id;
             const Names names(scene);
             for (usize i = 0; i < list.size(); ++i) {
                 Person p;
                 p.id = next++;
                 p.seed = seed_of(p.id);
                 if (std::string bad = apply_person(list[i], names, p, true); !bad.empty()) {
                     return CommandResult::failure("people[" + std::to_string(i) + "]: " + bad);
                 }
                 ids.push_back(p.id);
                 change.in.push_back(std::move(p));
             }
             change.next_after = next;
             commit(state, context, std::move(change));
             return CommandResult::success({{"ids", std::move(ids)}, {"people", state.scene.people.size()}});
         }});
    Json set_props = person_properties();
    set_props["id"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 4294967295.0}};
    add(registry,
        {"set_person",
         "Changes a person (by id): only the fields given. Undoable.",
         object_schema({"id"}, set_props),
         false,
         [&state](const Json& args, CommandContext& context) {
             const PersonId id = args["id"].get<PersonId>();
             const Person* had = world::find_person(state.scene.people, id);
             if (had == nullptr) return CommandResult::failure("no person " + std::to_string(id));
             Person changed = *had;
             if (std::string bad = apply_person(args, Lists(state.scene), changed, false); !bad.empty()) {
                 return CommandResult::failure(bad);
             }
             PeopleChange change;
             change.out.push_back(*had);
             change.in.push_back(changed);
             commit(state, context, std::move(change));
             return CommandResult::success(person_json(changed));
         }});
    add(registry,
        {"remove_people",
         "Removes people by id (1 to 4096 a call, each once; refused whole if one is not there). One undo step.",
         object_schema({"ids"}, {{"ids", {{"type", "array"},
                                          {"minItems", 1},
                                          {"maxItems", k_most_a_call},
                                          {"items", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4294967295.0}}}}}}),
         false,
         [&state](const Json& args, CommandContext& context) {
             std::vector<PersonId> ids = args["ids"].get<std::vector<PersonId>>();
             std::sort(ids.begin(), ids.end());
             if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
                 return CommandResult::failure("ids names a person twice");
             }
             PeopleChange change;
             change.out.reserve(ids.size());
             for (const PersonId id : ids) {
                 const Person* p = world::find_person(state.scene.people, id);
                 if (p == nullptr) return CommandResult::failure("no person " + std::to_string(id));
                 change.out.push_back(*p);
             }
             commit(state, context, std::move(change));
             return CommandResult::success({{"removed", ids.size()}, {"people", state.scene.people.size()}});
         }});
    Json people_props = list_properties("people");
    people_props["ids"] = {{"type", "array"},
                           {"maxItems", k_most_listed},
                           {"items", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4294967295.0}}},
                           {"description", "These people alone (null in `people` for one not there); nothing else may be given."}};
    people_props["look"] = name_schema("Only people of this look.");
    people_props["home"] = name_schema("Only people whose home is this place.");
    people_props["routine"] = name_schema("Only people with this routine.");
    people_props["behaviour"] = name_schema("Only people with this behaviour.");
    people_props["text"] = {{"type", "string"}, {"minLength", 1}, {"maxLength", world::k_max_name_bytes},
                            {"description", "Only people with this in their name, look, home, routine or behaviour (letters' case aside), or whose id it is."}};
    people_props["alarmed"] = {{"type", "boolean"}, {"description", "Only people alarmed now (while the world runs)."}};
    people_props["travelling"] = {{"type", "boolean"}, {"description", "Only people on a trip now."}};
    add(registry,
        {"get_people",
         "Lists people as the scene keeps them (where each starts; where each is while the world runs is "
         "the simulation's): those named by `ids`, or all, or those of `look`, living at `home`, with `routine` or `behaviour`, "
         "with `text` in their name, look, home, routine or behaviour, `alarmed` or `travelling` now and/or "
         "within `within` metres of `near`, `limit` at a time from `offset` (by rising id). `count` is how "
         "many match, `total` how many there are. `entity`: the entity standing for one near the camera now "
         "(set_people_settings), else null. `at`: where it is now (its `position` until it travels: "
         "send_person) and `trip`: its trip (as get_travel gives it), else null. `doing`: what its routine has "
         "it do now while the world runs (`activity`, the `place` it was sent to or \"\", and `failed` when it "
         "could not be sent: no such place, no road near, no way found; `own`: it is its behaviour's state's own, not its routine's), "
         "else null. `state`: the state of its behaviour it is in while the world runs (`name`, its `index` among the "
         "behaviour's, `seconds` simulated since it entered it), else null. `alarm` (0 calm to 1) and "
         "`heard`: the noise that alarmed it most of late (`kind`, `from` where it was made, `level` at its "
         "ears, `seconds` since), or what it saw (`kind` \"sight\", `from` where the thing was, `seconds` "
         "since it last saw it), else null (make_noise, show_to_people, set_sense_settings). `why`: what it "
         "does and why, in words for a person to read (the wording may change). `near` goes by `position`.",
         object_schema(Json::array(), people_props),
         true,
         [&state](const Json& args, CommandContext&) {
             sync_senses(state);
             const std::vector<Person>& people = state.scene.people;
             Json list = Json::array();
             if (args.contains("ids")) {
                 if (args.size() != 1) return CommandResult::failure("`ids` is given alone");
                 usize found = 0;
                 for (const Json& id : args["ids"]) {
                     const Person* p = world::find_person(people, id.get<PersonId>());
                     list.push_back(p == nullptr ? Json(nullptr) : person_json(*p, state.people, &state.scene, state.travel, state.routines, state.senses, &state));
                     found += p != nullptr ? 1 : 0;
                 }
                 return CommandResult::success({{"count", found}, {"total", people.size()}, {"people", std::move(list)}});
             }
             const std::string look = args.value("look", std::string()), home = args.value("home", std::string());
             const std::string routine = args.value("routine", std::string());
             const std::string behaviour = args.value("behaviour", std::string());
             const bool by_place = args.contains("near");
             const DVec3 near = by_place ? point(args["near"]) : DVec3{};
             const double within = args.value("within", 100.0);
             const usize offset = args.value("offset", usize{0}), limit = args.value("limit", usize{100});
             PeopleList::Filter filter;
             filter.text = args.value("text", std::string());
             filter.alarmed = args.value("alarmed", false);
             filter.travelling = args.value("travelling", false);
             const bool filtered = !(filter == PeopleList::Filter{});
             usize count = 0;
             for (const Person& p : people) {
                 if (!look.empty() && p.look != look) continue;
                 if (!home.empty() && p.home != home) continue;
                 if (!routine.empty() && p.routine != routine) continue;
                 if (!behaviour.empty() && p.behaviour != behaviour) continue;
                 if (filtered && !PeopleList::matches(state, p, static_cast<usize>(&p - people.data()), filter)) continue;
                 if (by_place && !near_enough(p.position, near, within)) continue;
                 if (count >= offset && list.size() < limit) list.push_back(person_json(p, state.people, &state.scene, state.travel, state.routines, state.senses, &state));
                 ++count;
             }
             return CommandResult::success({{"count", count}, {"total", people.size()}, {"people", std::move(list)}});
         }});
    add(registry,
        {"follow_person",
         "The viewport camera follows a person (`id`; null or left out: nobody): kept `distance` metres "
         "behind it (1 to 200, default 6, or as last given) and never under the ground, looking the way the camera is turned, so "
         "set_camera's yaw and pitch look round it (a position given is not kept, and look_at aims from where the camera was). Following ends when the person is removed, another scene is taken up or play "
         "starts. A loaded game ends it too. Returns `following` (the id, or null; get_camera gives it as well) and `distance`. View state: not undone, not saved.",
         object_schema(Json::array(),
                       {{"id", {{"type", {"integer", "null"}}, {"minimum", 1}, {"maximum", 4294967295.0},
                                 {"description", "The person to follow; null or left out: nobody."}}},
                        {"distance", {{"type", "number"}, {"minimum", 1}, {"maximum", 200},
                                      {"description", "Metres behind it (default 6, or as last given)."}}}}),
         false,
         [&state](const Json& args, CommandContext&) {
             const bool nobody = !args.contains("id") || args["id"].is_null();
             if (!nobody) {
                 if (state.play != nullptr && state.play->playing()) return CommandResult::failure("playing: the camera is the player's eyes (play off first)");
                 if (world::find_person(state.scene.people, args["id"].get<PersonId>()) == nullptr) return CommandResult::failure("no such person");
             }
             state.follow.distance = args.value("distance", state.follow.distance);
             state.follow.person = nobody ? 0 : args["id"].get<PersonId>();
             state.follow.scene = state.scene.identity();
             sync_travel(state);
             follow_person_camera(state);
             return CommandResult::success({{"following", state.follow.person != 0 ? Json(state.follow.person) : Json(nullptr)},
                                            {"distance", state.follow.distance}});
         }});
    add(registry,
        {"set_people_settings",
         "How the people near the camera are given entities (real characters made from their looks, each "
         "an entity carrying its `person`): `on`; `embody_distance` (metres from the camera within which a "
         "person gets one, default 60) and `release_distance` (beyond which it is taken away, default 80, "
         "never under embody_distance); `view_distance` (default 200, at most 2000: one the camera looks "
         "towards, or is about to, gets its entity this far off, so nobody appears or goes close by "
         "in view; while simulating a near one in view waits until it is out of sight, but for once through the list after a load, "
         "a camera put elsewhere, an edit or the simulation stopped; an entity in view stays until view_distance and the gap "
         "between the other two distances is passed); `most` held at once (default 256, at most 1024); `made_a_frame` "
         "(default 2) and `scanned_a_frame` (people looked at a frame, default 1024). Fields left out stay; "
         "with none it only reports. Returns the settings with `embodied` (entities held now), `waiting` "
         "(wanting one but over `most`), `walking` (of them: walking their trips' ways, send_person), "
         "`acting` (of them: playing their look's clip for what their routine has them do), `unseen` (near, in view, "
         "waiting to be out of sight), `swapped` (the farthest given up for someone nearer while `most` were held), "
         "`appeared_in_view` and `vanished_in_view` (entities made and taken away close by in view while simulating: "
         "after a load, a camera put elsewhere or an edit; a flight that counts none saw nobody appear or go), and "
         "`made` and `released` since the scene was taken up. The "
         "entities are never saved and not undone; deleting one only has it made again. View state.",
         object_schema(Json::array(),
                       {{"on", {{"type", "boolean"}}},
                        {"embody_distance", {{"type", "number"}, {"minimum", 1}, {"maximum", 1000}}},
                        {"release_distance", {{"type", "number"}, {"minimum", 1}, {"maximum", 2000}}},
                        {"view_distance", {{"type", "number"}, {"minimum", 1}, {"maximum", 2000}}},
                        {"most", {{"type", "integer"}, {"minimum", 0}, {"maximum", PeopleState::k_max_embodied}}},
                        {"made_a_frame", {{"type", "integer"}, {"minimum", 1}, {"maximum", 64}}},
                        {"scanned_a_frame", {{"type", "integer"}, {"minimum", 1}, {"maximum", 65536}}}}),
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.people == nullptr) return CommandResult::failure("people are not given entities here");
             PeopleState::Settings s = state.people->settings;
             s.on = args.value("on", s.on);
             s.embody_distance = args.value("embody_distance", s.embody_distance);
             s.release_distance = args.value("release_distance", s.release_distance);
             s.view_distance = args.value("view_distance", s.view_distance);
             s.most = args.value("most", s.most);
             s.made_a_frame = args.value("made_a_frame", s.made_a_frame);
             s.scanned_a_frame = args.value("scanned_a_frame", s.scanned_a_frame);
             if (s.release_distance < s.embody_distance) {
                 if (args.contains("release_distance")) {
                     return CommandResult::failure("release_distance must be at least embody_distance");
                 }
                 s.release_distance = s.embody_distance;
             }
             state.people->settings = s;
             return CommandResult::success(people_state_json(*state.people));
         }});
    add(registry,
        {"set_figure_settings",
         "How the people farther from the camera than those with entities are drawn (crowds): as figures, "
         "each the instances of one baked pose of its look's character (bake_pose) put where the person "
         "is, with no entity, skeleton or physics, so a town seen from afar shows its people. A look's "
         "pose is asked for when someone of it is first within `distance`. `on` "
         "(default false: until a figure and its entity change places unseen, one may be seen to); "
         "`distance` (metres from the camera a person is drawn out to, default 400, at most 4000) and "
         "`keep` (metres farther one drawn stays, default 40); `most` at once (default 4096, at most "
         "16384); `made_a_frame` (default 64) and `scanned_a_frame` (people looked at a frame, default "
         "1024); `triangles` each look's pose is cut to (default 1500) and `clip`, the clip of the look's "
         "file whose start its people stand in (default \"idle\"; a file without it: its rest pose). "
         "Changing either of the last two has every look's pose asked for again, one a frame (one "
         "kept from before is taken, the rest are baked; nobody is a figure until its look's is there; "
         "the old ones stay until clear_baked_poses). Fields left out stay; with none it only reports. Returns the settings with "
         "`figures` (kept now: each drawn while its look's pose is there) and `slots` (kept for them), "
         "`waiting` (wanted but over `most`), `made` and `removed` since the scene was taken up, and "
         "`looks_ready`, `looks_baking` and `looks_without` (the scene's looks by their pose: baked, "
         "being baked, and none: nobody of the look has been within `distance`, or it is not to be had: "
         "the file not loaded or not skinned, the bake failed, or as many poses are kept as can be, "
         "256 with bake_pose's). The counts are the last frame's. A person given an entity is a figure "
         "no longer; one near with no entity (over set_people_settings' `most`, or waiting to be out of "
         "sight) is a figure too; figures stand on the ground (one travelling is put on along its way "
         "every so often). View state: not saved, not undone.",
         object_schema(Json::array(),
                       {{"on", {{"type", "boolean"}}},
                        {"distance", {{"type", "number"}, {"minimum", 1}, {"maximum", 4000}}},
                        {"keep", {{"type", "number"}, {"minimum", 0}, {"maximum", 1000}}},
                        {"most", {{"type", "integer"}, {"minimum", 0}, {"maximum", PeopleFigures::k_max_figures}}},
                        {"made_a_frame", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1024}}},
                        {"scanned_a_frame", {{"type", "integer"}, {"minimum", 1}, {"maximum", 65536}}},
                        {"triangles",
                         {{"type", "integer"}, {"minimum", PoseBakes::k_min_triangles}, {"maximum", PoseBakes::k_max_triangles}}},
                        {"clip", {{"type", "string"}, {"maxLength", world::k_max_name_bytes}}}}),
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.figures == nullptr) return CommandResult::failure("people are not drawn as figures here");
             PeopleFigures::Settings& s = state.figures->settings;
             s.on = args.value("on", s.on);
             s.distance = args.value("distance", s.distance);
             s.keep = args.value("keep", s.keep);
             s.most = args.value("most", s.most);
             s.made_a_frame = args.value("made_a_frame", s.made_a_frame);
             s.scanned_a_frame = args.value("scanned_a_frame", s.scanned_a_frame);
             s.triangles = args.value("triangles", s.triangles);
             s.clip = args.value("clip", s.clip);
             const PeopleFigures::Stats& t = state.figures->stats();
             return CommandResult::success({{"on", s.on},
                                            {"distance", s.distance},
                                            {"keep", s.keep},
                                            {"most", s.most},
                                            {"made_a_frame", s.made_a_frame},
                                            {"scanned_a_frame", s.scanned_a_frame},
                                            {"triangles", s.triangles},
                                            {"clip", s.clip},
                                            {"figures", t.figures},
                                            {"slots", t.slots},
                                            {"waiting", t.waiting},
                                            {"made", t.made},
                                            {"removed", t.removed},
                                            {"looks_ready", t.looks_ready},
                                            {"looks_baking", t.looks_baking},
                                            {"looks_without", t.looks_without}});
         }});
    add(registry,
        {"make_noise",
         "While simulating, makes a noise at `at` [x, y, z] that the people hear: `loudness` its level at "
         "1 m (as play_sound's: a rifle's report about 100, a shout about 3) and `kind` (shot, impact, "
         "blast, splash, voice, other; default other). It spreads at the speed of sound; each person it "
         "is loud enough for where the person is (the level falls with the distance) hears it once, and "
         "its `alarm` rises (get_people). Shots fired, bullets striking, blasts and splashes make their "
         "own. Nothing is played: play_sound is what the listener hears. View state: not undone, not saved.",
         object_schema({"at", "loudness"},
                       {{"at", point_schema("Where the noise is made, metres.")},
                        {"loudness", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", PeopleSenses::k_max_loudness}}},
                        {"kind", {{"type", "string"}, {"enum", {"shot", "impact", "blast", "splash", "voice", "other"}}}}}),
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.senses == nullptr || state.travel == nullptr) return CommandResult::failure("nobody hears anything here");
             sync_senses(state); // (its clock as it is now)
             PeopleSenses::Kind kind = PeopleSenses::Kind::other;
             if (!PeopleSenses::from_string(args.value("kind", std::string("other")), kind)) return CommandResult::failure("no such kind of noise");
             if (!state.travel->running()) return CommandResult::failure("not simulating (simulate on first): sounds spread by the simulation's clock");
             if (!state.senses->settings.on) return CommandResult::failure("the people's hearing is off (set_sense_settings)");
             const float loudness = args["loudness"].get<float>();
             const DVec3 where = point(args["at"]);
             if (!(std::fabs(where.x) <= 1e8 && std::fabs(where.y) <= 1e8 && std::fabs(where.z) <= 1e8)) return CommandResult::failure("`at` is out of the world (1e8 m)");
             double reach = 0.0;
             if (!state.senses->noise(where, loudness, kind, &reach)) {
                 return CommandResult::failure(loudness <= std::clamp(state.senses->settings.hear_loudness, 0.01f, 100.0f)
                                                   ? "too quiet for anyone to hear (set_sense_settings hear_loudness)"
                                                   : "not made: as many louder noises are spreading as are kept");
             }
             return CommandResult::success({{"kind", PeopleSenses::to_string(kind)},
                                            {"reach", reach},
                                            {"noises", state.senses->stats().noises}});
         }});
    add(registry,
        {"show_to_people",
         "Something the people may see, by `name`, at `at` (its middle or its eyes; again with the same "
         "name moves it; 16 at most; the player is seen while playing without being shown, and the name "
         "\"player\" is its own). `clearance` (metres, 0 to 50, default 0): how far short of `at` the look "
         "stops, about the half size of the thing's own body so that it does not hide itself (0 for a "
         "point in the open). While simulating, a person sees it within see_distance of its eyes, within "
         "see_angle about the level way it faces (or within arm's reach) and with nothing between (bodies "
         "and terrain; characters are seen through): its `alarm` rises as near as it is and is held while "
         "it is in sight (get_people: `heard` with kind \"sight\"; set_sense_settings). Forgotten when the "
         "simulation stops or starts again, with another scene, and when the senses are turned off. View "
         "state: not undone, not saved.",
         object_schema({"name", "at"},
                       {{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", PeopleSenses::k_max_name}}},
                        {"at", point_schema("Where it is, metres.")},
                        {"clearance", {{"type", "number"}, {"minimum", 0}, {"maximum", 50}}}}),
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.senses == nullptr || state.travel == nullptr) return CommandResult::failure("nobody sees anything here");
             sync_senses(state);
             if (!state.travel->running()) return CommandResult::failure("not simulating (simulate on first): what is shown is forgotten when the simulation stops");
             if (!state.senses->settings.on) return CommandResult::failure("the people's senses are off (set_sense_settings)");
             const DVec3 where = point(args["at"]);
             if (!(std::fabs(where.x) <= 1e8 && std::fabs(where.y) <= 1e8 && std::fabs(where.z) <= 1e8)) return CommandResult::failure("`at` is out of the world (1e8 m)");
             const std::string name = args["name"].get<std::string>();
             if (name == "player") return CommandResult::failure("\"player\" is the player's own name (seen while playing)");
             if (!state.senses->watch(name, where, static_cast<float>(args.value("clearance", 0.0)))) {
                 return CommandResult::failure("as many things are shown as can be (16): hide_from_people one first");
             }
             return CommandResult::success({{"shown", state.senses->watched().size()}});
         }});
    add(registry,
        {"hide_from_people",
         "What show_to_people showed by `name` is no longer there to be seen. View state.",
         object_schema({"name"}, {{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", PeopleSenses::k_max_name}}}}),
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.senses == nullptr) return CommandResult::failure("nobody sees anything here");
             sync_senses(state);
             if (!state.senses->unwatch(args["name"].get<std::string>())) return CommandResult::failure("nothing of that name is shown");
             return CommandResult::success({{"shown", state.senses->watched().size()}});
         }});
    add(registry,
        {"set_sense_settings",
         "How the people hear and see while simulating: `on`; `hear_loudness` (the level at one's ears below "
         "which nobody notices, 0.01 to 100, default 0.2: a rifle's report is heard to 500 m); "
         "`alarm_loudness` (the level that alarms one fully, default 2, never under hear_loudness: quieter "
         "alarms in proportion, and each noise takes its share of what calm is left); `calm_seconds` (a "
         "full alarm eases to nothing over this, 1 to 3600, default 30); `looked_a_frame` (people looked "
         "at a frame while a noise spreads, 1 to 16384, default 512: 5,000 people are gone through in ten frames, each "
         "hearing each noise once however long that takes, up to two times through late (a third of a "
         "second for 5,000 people at 60 fps, four seconds for 65,536: raise it for fuller worlds); fewer "
         "than set when it is over 4096 and many noises "
         "spread at once: 262,144 tests of a person against a noise a frame at most). Sight: `sight` (on); "
         "`see_distance` (metres from its eyes, 1 to 300, default 60); `see_angle` (degrees across, about "
         "the way it faces, 10 to 360, default 120); `seen_a_frame` (people looked at a frame while anything "
         "is shown, 1 to 16384, default 1024) and `rays_a_frame` (rays cast a frame for what stands "
         "between, 1 to 128, default 32). Fields left out stay; with none it "
         "only reports. Returns the settings with `noises` (spreading now), `made`, `dropped` (more at "
         "once than the 64 kept) and `heard` (times someone heard one), `watched` (what is shown: `name`, "
         "`at`, `clearance`; the player first while playing), `seen` (times someone looked and saw one) "
         "and `rays` since the simulation started. Off, the noises spreading and what is shown are forgotten. "
         "View state.",
         object_schema(Json::array(),
                       {{"on", {{"type", "boolean"}}},
                        {"hear_loudness", {{"type", "number"}, {"minimum", 0.01}, {"maximum", 100}}},
                        {"alarm_loudness", {{"type", "number"}, {"minimum", 0.01}, {"maximum", 1000}}},
                        {"calm_seconds", {{"type", "number"}, {"minimum", 1}, {"maximum", 3600}}},
                        {"looked_a_frame", {{"type", "integer"}, {"minimum", 1}, {"maximum", PeopleSenses::k_max_looked}}},
                        {"sight", {{"type", "boolean"}}},
                        {"see_distance", {{"type", "number"}, {"minimum", 1}, {"maximum", PeopleSenses::k_max_see}}},
                        {"see_angle", {{"type", "number"}, {"minimum", 10}, {"maximum", 360}}},
                        {"seen_a_frame", {{"type", "integer"}, {"minimum", 1}, {"maximum", PeopleSenses::k_max_looked}}},
                        {"rays_a_frame", {{"type", "integer"}, {"minimum", 1}, {"maximum", PeopleSenses::k_max_rays}}}}),
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.senses == nullptr) return CommandResult::failure("nobody hears anything here");
             PeopleSenses::Settings s = state.senses->settings;
             s.on = args.value("on", s.on);
             s.hear_loudness = static_cast<float>(args.value("hear_loudness", static_cast<double>(s.hear_loudness)));
             s.alarm_loudness = static_cast<float>(args.value("alarm_loudness", static_cast<double>(s.alarm_loudness)));
             s.calm_seconds = static_cast<float>(args.value("calm_seconds", static_cast<double>(s.calm_seconds)));
             s.looked_a_frame = args.value("looked_a_frame", s.looked_a_frame);
             s.sight = args.value("sight", s.sight);
             s.see_distance = static_cast<float>(args.value("see_distance", static_cast<double>(s.see_distance)));
             s.see_angle = static_cast<float>(args.value("see_angle", static_cast<double>(s.see_angle)));
             s.seen_a_frame = args.value("seen_a_frame", s.seen_a_frame);
             s.rays_a_frame = args.value("rays_a_frame", s.rays_a_frame);
             if (s.alarm_loudness < s.hear_loudness) {
                 if (args.contains("alarm_loudness")) return CommandResult::failure("alarm_loudness must be at least hear_loudness");
                 s.alarm_loudness = s.hear_loudness;
             }
             state.senses->settings = s;
             if (!s.on) state.senses->silence();
             const PeopleSenses::Stats& t = state.senses->stats();
             Json watched = Json::array();
             if (s.on && state.senses->player) watched.push_back({{"name", "player"}, {"at", point_json(*state.senses->player)}, {"clearance", 0.0}});
             for (const PeopleSenses::Watched& w : state.senses->watched()) {
                 watched.push_back({{"name", w.name}, {"at", point_json(w.at)}, {"clearance", w.clearance}});
             }
             return CommandResult::success({{"on", s.on},
                                            {"hear_loudness", s.hear_loudness},
                                            {"alarm_loudness", s.alarm_loudness},
                                            {"calm_seconds", s.calm_seconds},
                                            {"looked_a_frame", s.looked_a_frame},
                                            {"noises", t.noises},
                                            {"made", t.made},
                                            {"dropped", t.dropped},
                                            {"heard", t.heard},
                                            {"sight", s.sight},
                                            {"see_distance", s.see_distance},
                                            {"see_angle", s.see_angle},
                                            {"seen_a_frame", s.seen_a_frame},
                                            {"rays_a_frame", s.rays_a_frame},
                                            {"watched", std::move(watched)},
                                            {"seen", t.seen},
                                            {"rays", t.rays}});
         }});
    add(registry,
        {"set_routine_settings",
         "How people follow their routines while simulating (set_routine; the time of day is the clock's: "
         "set_time, moved on by play): `on`; `spread_minutes` (0 to 120, default 20: each person keeps its "
         "hours late by its own share of this, the same every day, so a village does not set off as one); "
         "`looked_a_frame` (people looked at a frame, 1 to 8192, default 512) and `sent_a_frame` (of them, "
         "sent or stopped, 1 to 128, default 32); `within` (metres a person and its goal may be from a "
         "road, 0 to 1000, default 500). "
         "An entry sends one to its home, the place named, or the place with the tag nearest its home, to "
         "a spot of its own within the place's radius (send_person's way of travelling); `stay` stops it "
         "where it is. One with a behaviour (set_behaviour) is in one of its states, "
         "the first on each run: when looked at, the first of the state's transitions whose conditions all hold is "
         "taken, and a state with its own activity is acted on in place of what the routine says. "
         "No arguments: reads them, with `following` (people with a routine or a behaviour), `behaving` (of them, "
         "with a behaviour) and `sent`, `stayed`, `failed` (entries and states acted on) and `changed` (states gone "
         "to) since the simulation started (one that failed is tried "
         "again when the places or the roads change). A person sent by hand (send_person) is left to it until its "
         "routine next says something else. View state.",
         object_schema(Json::array(),
                       {{"on", {{"type", "boolean"}}},
                        {"spread_minutes", {{"type", "number"}, {"minimum", 0}, {"maximum", 120}}},
                        {"looked_a_frame", {{"type", "integer"}, {"minimum", 1}, {"maximum", PeopleRoutines::k_most_looked}}},
                        {"sent_a_frame", {{"type", "integer"}, {"minimum", 1}, {"maximum", PeopleRoutines::k_most_sent}}},
                        {"within", {{"type", "number"}, {"minimum", 0}, {"maximum", PeopleRoutines::k_most_within}}}}),
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.routines == nullptr) return CommandResult::failure("nobody follows a routine here");
             PeopleRoutines::Settings& s = state.routines->settings;
             s.on = args.value("on", s.on);
             s.spread_minutes = args.value("spread_minutes", s.spread_minutes);
             s.looked_a_frame = args.value("looked_a_frame", s.looked_a_frame);
             s.sent_a_frame = args.value("sent_a_frame", s.sent_a_frame);
             s.within = args.value("within", s.within);
             const PeopleRoutines::Stats t = state.routines->stats();
             return CommandResult::success({{"on", s.on},
                                            {"spread_minutes", s.spread_minutes},
                                            {"looked_a_frame", s.looked_a_frame},
                                            {"sent_a_frame", s.sent_a_frame},
                                            {"within", s.within},
                                            {"following", t.following},
                                            {"sent", t.sent},
                                            {"stayed", t.stayed},
                                            {"behaving", t.behaving},
                                            {"changed", t.changed},
                                            {"failed", t.failed}});
         }});
    add(registry,
        {"send_person",
         "While simulating, sends a person (`id`) travelling to `to`: a place's name (anywhere within it "
         "is there: its middle) or a point [x, y, z]. It goes along the world's roads and paths "
         "(create_route) at `speed` (m/s, default 1.4, a walk): to the nearest route node within `within` "
         "metres (default 500, at most 4096; one already on the roads, going along them or stopped part way, "
         "joins at the nearest within 4096), the cheapest way along the links (as get_route finds), and "
         "from the last node to the goal; straight there when the roads are no help. The way is looked "
         "for over the next frames (`phase` searching, then travelling, or failed with `why`); then it "
         "moves by the simulation's clock whether anyone sees it or not (get_people gives `at`). One with "
         "an entity near the camera (`embodied`) walks its way over the ground as a walker does (20 m/s "
         "at most), the trip going on as far as it has walked. Sent again, it goes on from where it has got to. Stopping the simulation puts everyone "
         "back where the scene has them. View state: not undone, not saved.",
         object_schema({"id", "to"},
                       {{"id", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4294967295.0}}},
                        {"to", {{"description", "A place's name or a point [x, y, z]."}}},
                        {"speed", {{"type", "number"}, {"minimum", 0.1}, {"maximum", 100}}},
                        {"within", {{"type", "number"}, {"minimum", 0}, {"maximum", PeopleTravel::k_join_within}}}}),
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.travel == nullptr) return CommandResult::failure("nobody travels here");
             sync_travel(state);
             const PersonId id = args["id"].get<PersonId>();
             const Json& to = args["to"];
             DVec3 goal{};
             std::string place;
             if (to.is_string()) {
                 place = to.get<std::string>();
                 const Place* found = world::find_place(state.scene.places, place);
                 if (found == nullptr) return CommandResult::failure("no place \"" + place + "\"");
                 goal = found->position;
             } else if (to.is_array() && to.size() == 3 && to[0].is_number() && to[1].is_number() && to[2].is_number()) {
                 goal = point(to);
             } else {
                 return CommandResult::failure("`to` must be a place's name or a point [x, y, z]");
             }
             const std::string problem = state.travel->send(state.scene, id, goal, args.value("speed", 1.4f),
                                                            args.value("within", 500.0), std::move(place));
             if (!problem.empty()) return CommandResult::failure(problem);
             return CommandResult::success({{"id", id}, {"trip", trip_json(*state.travel, *state.travel->trip(id))}});
         }});
    add(registry,
        {"stop_person",
         "Ends a person's trip (`id`): it stays where it has got to. View state.",
         object_schema({"id"}, {{"id", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4294967295.0}}}}),
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.travel == nullptr) return CommandResult::failure("nobody travels here");
             sync_travel(state);
             const PersonId id = args["id"].get<PersonId>();
             const Person* person = world::find_person(state.scene.people, id);
             if (person == nullptr) return CommandResult::failure("no person " + std::to_string(id));
             if (!state.travel->stop(id)) return CommandResult::failure("person " + std::to_string(id) + " has no trip");
             return CommandResult::success({{"id", id}, {"at", point_json(state.travel->where(*person).position)}});
         }});
    add(registry,
        {"get_travel",
         "The people's travelling: whether it is `running` (simulating), the `seconds` simulated, `trips` "
         "(searching, travelling and failed), `searching` (their way not found yet), `moved` (people sent "
         "since it started: where each is is the simulation's), and since the simulation started `arrived`, `failed`, "
         "`searches` and the route nodes they `opened`. With `id`: that person's `at` and `trip` (`phase` "
         "searching, travelling or failed with `why`; `to`, `place`, `speed`, `embodied`; travelling: "
         "`gone` and `length` in metres, `arrives_in` seconds, null while embodied), null when it has none.",
         object_schema(Json::array(), {{"id", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4294967295.0}}}}),
         true,
         [&state](const Json& args, CommandContext&) {
             if (state.travel == nullptr) return CommandResult::failure("nobody travels here");
             sync_travel(state);
             const PeopleTravel& travel = *state.travel;
             const PeopleTravel::Stats s = travel.stats();
             Json j = {{"running", travel.running()}, {"seconds", travel.seconds()}, {"trips", s.trips},
                       {"searching", s.searching},    {"moved", s.moved},            {"arrived", s.arrived},
                       {"failed", s.failed},          {"searches", s.searches},      {"opened", s.opened},
                       {"reused", s.reused}};
             if (args.contains("id")) {
                 const PersonId id = args["id"].get<PersonId>();
                 const Person* person = world::find_person(state.scene.people, id);
                 if (person == nullptr) return CommandResult::failure("no person " + std::to_string(id));
                 const PeopleTravel::Trip* trip = travel.trip(id);
                 j["id"] = id;
                 j["at"] = point_json(travel.where(*person).position);
                 j["trip"] = trip != nullptr ? trip_json(travel, *trip) : Json(nullptr);
             }
             return CommandResult::success(std::move(j));
         }});
}

void sync_senses(EditorState& state) {
    sync_travel(state);
    if (state.senses != nullptr && state.travel != nullptr) state.senses->sync(state.scene, *state.travel);
}

void connect_senses(EditorState& state) {
    if (state.physics == nullptr || state.senses == nullptr || state.senses->connected) return;
    state.senses->connected = true;
    // What stands between a person's eyes and a thing shown: the bodies (characters seen through: the
    // person's own, the player's) and the terrain past what the physics holds.
    state.senses->blocked = [&state](const DVec3& from, const DVec3& to) {
        const DVec3 way = to - from;
        const double reach = giant::length(way);
        if (!(reach > 1e-3)) return false;
        const DVec3 unit = way / reach;
        const Vec3 dir{static_cast<float>(unit.x), static_cast<float>(unit.y), static_cast<float>(unit.z)};
        if (state.physics != nullptr) {
            const sim::PhysicsWorld& w = state.physics->world();
            sim::PhysicsWorld::RayFilter filter;
            filter.accept = [&w](sim::BodyId id, u64) { return !w.is_character_body(id); };
            if (w.raycast(from, dir, reach, filter)) return true;
        }
        return march_terrain(state.scene.terrain, from, dir, reach).has_value();
    };
    std::function<void(std::span<const PhysicsState::ShotHit>)> hits_before = state.physics->on_hits;
    state.physics->on_hits = [&state, hits_before](std::span<const PhysicsState::ShotHit> hits) {
        if (hits_before) hits_before(hits);
        if (state.senses == nullptr || hits.empty()) return;
        sync_senses(state); // (made by the clock as the step has it, a run's first frame too)
        for (const PhysicsState::ShotHit& h : hits) {
            state.senses->noise(h.hit.point, AudioState::impact_loudness(h.hit.energy), PeopleSenses::Kind::impact);
        }
    };
    std::function<void(const DVec3&, float, float)> impact_before = state.physics->on_impact;
    state.physics->on_impact = [&state, impact_before](const DVec3& at, float radius, float impulse) {
        if (impact_before) impact_before(at, radius, impulse);
        if (state.senses == nullptr) return;
        sync_senses(state);
        state.senses->noise(at, PeopleSenses::blast_loudness(impulse), PeopleSenses::Kind::blast);
    };
    std::function<void(std::span<const sim::WaterEntry>)> splashes_before = state.physics->on_splashes;
    state.physics->on_splashes = [&state, splashes_before](std::span<const sim::WaterEntry> entries) {
        if (splashes_before) splashes_before(entries);
        if (state.senses == nullptr || entries.empty()) return;
        sync_senses(state);
        for (const sim::WaterEntry& e : entries) {
            state.senses->noise(e.point, AudioState::splash_loudness(e), PeopleSenses::Kind::splash);
        }
    };
}

} // namespace giant::editor
