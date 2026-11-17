// People commands: the people who live in the world, their looks and the
// places they go to (world/people.h; living-world plan LW1). Scene data
// (saved with the scene and the world's world.json, undoable). People are
// many, so an undo step keeps what one command changed, never the lists.
#include "editor_commands.h"
#include "people_state.h"
#include "people_travel.h"

#include "giant/core/assert.h"
#include "giant/world/people.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
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
    return {{"name", l.name},       {"mesh", l.mesh},        {"height", l.height}, {"parts", std::move(parts)},
            {"hair", l.hair.on},    {"matched", l.matched},  {"people", people}};
}

// The travel's clock brought up to the physics' now (a command between frames: time stepped on
// by step_physics, or the simulation started or stopped, since the last frame counts).
void sync_travel(EditorState& state) {
    if (state.travel == nullptr || state.physics == nullptr) return;
    const PeopleTravel::Clock now = PeopleTravel::clock_of(*state.physics);
    state.travel->sync(state.scene, now.run, now.seconds);
}

Json trip_json(const PeopleTravel& travel, const PeopleTravel::Trip& t) {
    Json j = {{"phase", PeopleTravel::to_string(t.phase)}, {"to", point_json(t.to)}, {"speed", t.speed}, {"standing", t.held}};
    if (!t.place.empty()) j["place"] = t.place;
    if (t.phase == PeopleTravel::Phase::travelling) {
        const double gone = travel.gone(t), length = PeopleTravel::length(t);
        j["gone"] = gone;
        j["length"] = length;
        j["arrives_in"] = t.held ? Json(nullptr) : Json((length - gone) / t.speed); // (standing: not known)
    }
    if (t.phase == PeopleTravel::Phase::failed) j["why"] = t.reason;
    return j;
}

// `people`: who is given an entity now (null: nobody is), named as `entity`.
Json person_json(const Person& p, const PeopleState* people = nullptr, const world::Scene* scene = nullptr,
                 const PeopleTravel* travel = nullptr) {
    Json j = {{"id", p.id},     {"name", p.name}, {"look", p.look}, {"home", p.home},
              {"position", point_json(p.position)}, {"yaw", p.yaw},   {"seed", p.seed}};
    if (scene != nullptr) {
        const world::EntityId entity = people != nullptr ? people->entity_of(*scene, p.id) : 0;
        j["entity"] = entity != 0 ? Json(entity) : Json(nullptr);
        const PeopleTravel::Where at = travel != nullptr ? travel->where(p) : PeopleTravel::Where{p.position, p.yaw};
        j["at"] = point_json(at.position);
        const PeopleTravel::Trip* trip = travel != nullptr ? travel->trip(p.id) : nullptr;
        j["trip"] = trip != nullptr ? trip_json(*travel, *trip) : Json(nullptr);
    }
    return j;
}

Json people_state_json(const PeopleState& people) {
    const PeopleState::Settings& s = people.settings;
    const PeopleState::Stats& t = people.stats();
    return {{"on", s.on},
            {"embody_distance", s.embody_distance},
            {"release_distance", s.release_distance},
            {"most", s.most},
            {"made_a_frame", s.made_a_frame},
            {"scanned_a_frame", s.scanned_a_frame},
            {"embodied", t.embodied},
            {"waiting", t.waiting},
            {"made", t.made},
            {"released", t.released}};
}

// What one command changed. A place or a look: the one at `index` before
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
    }
    bool has_look(std::string_view name) const { return std::binary_search(m_looks.begin(), m_looks.end(), name); }
    const Place* place(std::string_view name) const {
        const auto it = std::lower_bound(m_places.begin(), m_places.end(), name,
                                         [](const auto& entry, std::string_view n) { return entry.first < n; });
        return it == m_places.end() || it->first != name ? nullptr : it->second;
    }

private:
    std::vector<std::string_view> m_looks;
    std::vector<std::pair<std::string_view, const Place*>> m_places;
};

// The same asked of the scene's lists as they are: for one person.
class Lists {
public:
    explicit Lists(const world::Scene& scene) : m_scene(scene) {}
    bool has_look(std::string_view name) const { return world::find_person_look(m_scene.person_looks, name) != nullptr; }
    const Place* place(std::string_view name) const { return world::find_place(m_scene.places, name); }

private:
    const world::Scene& m_scene;
};

// Does a place's or a look's edit, or undoes it. The one it changes is
// found by its name, the index only says where one is put back: commands
// run outside the history (scripts) may have changed the list since. What
// would leave the scene broken is left undone: putting in a name that is
// there, taking out one that is not or that `in_use` says people still have.
template <typename T, typename InUse>
void apply(std::vector<T>& list, const Edit<T>& edit, bool forward, InUse in_use) {
    if (!edit.any) return;
    const std::optional<T>& from = forward ? edit.before : edit.after;
    const std::optional<T>& to = forward ? edit.after : edit.before;
    const auto named = [&](const std::string& name) {
        return std::find_if(list.begin(), list.end(), [&](const T& t) { return t.name == name; });
    };
    if (!from) {
        if (named(to->name) != list.end()) return;
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
// whose look is gone since is left out, one whose home is gone has none.
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
    apply(scene.places, change.place, forward, [&](const std::string& name) {
        return std::any_of(scene.people.begin(), scene.people.end(), [&](const Person& p) { return p.home == name; });
    });
    apply(scene.person_looks, change.look, forward, [&](const std::string& name) {
        return std::any_of(scene.people.begin(), scene.people.end(), [&](const Person& p) { return p.look == name; });
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
         "A new look needs `entity`. Saved with the scene or the world (at most 1024). Undoable.",
         object_schema({"name"},
                       {{"name", name_schema("Unique among the person looks.")},
                        {"entity", {{"type", "integer"}, {"minimum", 1}, {"description", "The entity whose look it takes."}}},
                        {"height", {{"type", "number"}, {"minimum", 0.1}, {"maximum", 10},
                                    {"description", "The scale people of this look are drawn at, all axes."}}},
                        {"matched", {{"type", "array"},
                                     {"maxItems", world::k_max_matched_clips},
                                     {"items", {{"type", "string"}, {"minLength", 1}, {"maxLength", world::k_max_name_bytes}}},
                                     {"description", "The clips they are motion matched among ([\"*\"] or none: every clip)."}}}}),
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
         "whether it wears hair, its matched clips, and how many `people` have it.",
         object_schema(Json::array(), Json::object()),
         true,
         [&state](const Json&, CommandContext&) {
             std::unordered_map<std::string_view, usize> people;
             for (const Person& p : state.scene.people) ++people[p.look];
             Json list = Json::array();
             for (const PersonLook& l : state.scene.person_looks) list.push_back(look_json(l, people[l.name]));
             return CommandResult::success({{"person_looks", std::move(list)}});
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
             if (u64{scene.next_person_id} + list.size() > 0xffffffffu) {
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
    add(registry,
        {"get_people",
         "Lists people as the scene keeps them (where each starts; where each is while the world runs is "
         "the simulation's): those named by `ids`, or all, or those of `look`, living at `home` and/or "
         "within `within` metres of `near`, `limit` at a time from `offset` (by rising id). `count` is how "
         "many match, `total` how many there are. `entity`: the entity standing for one near the camera now "
         "(set_people_settings), else null. `at`: where it is now (its `position` until it travels: "
         "send_person) and `trip`: its trip (as get_travel gives it), else null. `near` goes by `position`.",
         object_schema(Json::array(), people_props),
         true,
         [&state](const Json& args, CommandContext&) {
             sync_travel(state);
             const std::vector<Person>& people = state.scene.people;
             Json list = Json::array();
             if (args.contains("ids")) {
                 if (args.size() != 1) return CommandResult::failure("`ids` is given alone");
                 usize found = 0;
                 for (const Json& id : args["ids"]) {
                     const Person* p = world::find_person(people, id.get<PersonId>());
                     list.push_back(p == nullptr ? Json(nullptr) : person_json(*p, state.people, &state.scene, state.travel));
                     found += p != nullptr ? 1 : 0;
                 }
                 return CommandResult::success({{"count", found}, {"total", people.size()}, {"people", std::move(list)}});
             }
             const std::string look = args.value("look", std::string()), home = args.value("home", std::string());
             const bool by_place = args.contains("near");
             const DVec3 near = by_place ? point(args["near"]) : DVec3{};
             const double within = args.value("within", 100.0);
             const usize offset = args.value("offset", usize{0}), limit = args.value("limit", usize{100});
             usize count = 0;
             for (const Person& p : people) {
                 if (!look.empty() && p.look != look) continue;
                 if (!home.empty() && p.home != home) continue;
                 if (by_place && !near_enough(p.position, near, within)) continue;
                 if (count >= offset && list.size() < limit) list.push_back(person_json(p, state.people, &state.scene, state.travel));
                 ++count;
             }
             return CommandResult::success({{"count", count}, {"total", people.size()}, {"people", std::move(list)}});
         }});
    add(registry,
        {"set_people_settings",
         "How the people near the camera are given entities (real characters made from their looks, each "
         "an entity carrying its `person`): `on`; `embody_distance` (metres from the camera within which a "
         "person gets one, default 60) and `release_distance` (beyond which it is taken away, default 80, "
         "never under embody_distance); `most` held at once (default 256, at most 1024); `made_a_frame` "
         "(default 2) and `scanned_a_frame` (people looked at a frame, default 1024). Fields left out stay; "
         "with none it only reports. Returns the settings with `embodied` (entities held now), `waiting` "
         "(near enough but over `most`), and `made` and `released` since the scene was taken up. The "
         "entities are never saved and not undone; deleting one only has it made again. View state.",
         object_schema(Json::array(),
                       {{"on", {{"type", "boolean"}}},
                        {"embody_distance", {{"type", "number"}, {"minimum", 1}, {"maximum", 1000}}},
                        {"release_distance", {{"type", "number"}, {"minimum", 1}, {"maximum", 2000}}},
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
        {"send_person",
         "While simulating, sends a person (`id`) travelling to `to`: a place's name (anywhere within it "
         "is there: its middle) or a point [x, y, z]. It goes along the world's roads and paths "
         "(create_route) at `speed` (m/s, default 1.4, a walk): to the nearest route node within `within` "
         "metres (default 500, at most 4096), the cheapest way along the links (as get_route finds), and "
         "from the last node to the goal; straight there when the roads are no help. The way is looked "
         "for over the next frames (`phase` searching, then travelling, or failed with `why`); then it "
         "moves by the simulation's clock whether anyone sees it or not (get_people gives `at`). One with "
         "an entity near the camera stands until the entity goes (`standing`: walking the ground is to "
         "come). Sent again, it goes on from where it has got to. Stopping the simulation puts everyone "
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
         "searching, travelling or failed with `why`; `to`, `place`, `speed`, `standing`; travelling: "
         "`gone` and `length` in metres, `arrives_in` seconds, null while standing), null when it has none.",
         object_schema(Json::array(), {{"id", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4294967295.0}}}}),
         true,
         [&state](const Json& args, CommandContext&) {
             if (state.travel == nullptr) return CommandResult::failure("nobody travels here");
             sync_travel(state);
             const PeopleTravel& travel = *state.travel;
             const PeopleTravel::Stats s = travel.stats();
             Json j = {{"running", travel.running()}, {"seconds", travel.seconds()}, {"trips", s.trips},
                       {"searching", s.searching},    {"moved", s.moved},            {"arrived", s.arrived},
                       {"failed", s.failed},          {"searches", s.searches},      {"opened", s.opened}};
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

} // namespace giant::editor
