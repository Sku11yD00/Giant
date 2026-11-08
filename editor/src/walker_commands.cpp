// Walker commands: entities sent walking along the navigation's paths while
// the physics simulates (walker_state.h). View state, not undoable.
#include "editor_commands.h"
#include "nav_state.h"
#include "physics_state.h"
#include "walker_state.h"

#include "giant/core/assert.h"

#include <string>

namespace giant::editor {

namespace {

using ai::CommandContext;
using ai::CommandResult;
using ai::Json;

void add(ai::CommandRegistry& registry, ai::CommandDesc desc) {
    const bool added = registry.add(std::move(desc));
    GIANT_ASSERT(added, "walker command could not be registered");
}

Json point_schema(const char* description) {
    return {{"type", "array"},
            {"items", {{"type", "number"}, {"minimum", -1e8}, {"maximum", 1e8}}},
            {"minItems", 3},
            {"maxItems", 3},
            {"description", description}};
}

Json id_schema() { return {{"type", "integer"}, {"minimum", 1}, {"maximum", 9007199254740991.0}}; }
Json dvec(const DVec3& v) { return Json::array({v.x, v.y, v.z}); }

Json walker_json(world::EntityId id, const WalkerState::Walker& w) {
    Json path = Json::array();
    for (const DVec3& p : w.follow.path()) path.push_back(dvec(p));
    Json out = {{"id", id},
                {"phase", WalkerState::to_string(w.phase)},
                {"feet", dvec(w.feet)},
                {"target", dvec(w.target)},
                {"speed", w.settings.speed},
                {"velocity", Json::array({w.velocity.x, w.velocity.y, w.velocity.z})},
                {"path", std::move(path)},
                {"corner", w.follow.corner()},
                {"remaining", w.follow.remaining(w.feet)},
                {"searches", w.searches}};
    if (w.phase == WalkerState::Phase::failed) out["reason"] = w.reason;
    return out;
}

} // namespace

void connect_walkers(EditorState& state) {
    if (state.physics == nullptr || state.walkers == nullptr) return;
    state.physics->before_fixed_step = [&state](double seconds) { state.walkers->before_step(state.scene, seconds); };
    state.physics->after_fixed_step = [&state] { state.walkers->after_step(); };
}

void register_walker_commands(ai::CommandRegistry& registry, EditorState& state) {
    add(registry,
        {"walk_to",
         "Sends an entity walking to `to` (world metres) at `speed` m/s (default 1.4, a walk; 3 a jog; 6 a "
         "run) while simulating (simulate on first): it becomes a character that walks the way find_path "
         "finds (built now when it takes at most 64 tiles, else over the next frames), turning to face "
         "where it goes, walking up steps and slopes, stepping round other walkers, and stopping at the end. "
         "The entity must have body \"none\" (it walks as a character, not as its own body); a cube's or "
         "sphere's feet are its bottom, a mesh's or kit's its origin. Drawn where it walks; the scene keeps "
         "where it was put. Sending a walker again sends it on from where it is. View state, not undoable. "
         "Returns get_walker's report for it.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", Json::array({"id", "to"})},
              {"properties",
               {{"id", id_schema()},
                {"to", point_schema("World metres [x, y, z].")},
                {"speed", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 20}, {"description", "m/s."}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.walkers == nullptr || state.nav == nullptr || state.physics == nullptr) {
                 return CommandResult::failure("walking is not available");
             }
             const world::EntityId id = args["id"].get<world::EntityId>();
             const Json& t = args["to"];
             const DVec3 to{t[0].get<double>(), t[1].get<double>(), t[2].get<double>()};
             const float speed = args.contains("speed") ? args["speed"].get<float>() : 1.4f;
             state.nav->follow(state.scene, state.scene.untaken_changes());
             // The way now, if it is near enough to find at once.
             std::vector<DVec3> path;
             const std::optional<DVec3> feet = state.walkers->feet_of(state.scene, id);
             if (feet && state.physics->simulating()) {
                 const std::unique_ptr<nav::PathSearch> s = state.nav->find_path_now(state.scene, *feet, to, 64);
                 if (s->state() == nav::SearchState::found) path = s->path();
                 if (s->state() == nav::SearchState::failed) {
                     return CommandResult::failure(std::string("no way there: ") + s->reason());
                 }
             }
             const std::string error = state.walkers->walk_to(state.scene, id, to, speed, path.empty() ? nullptr : &path);
             if (!error.empty()) return CommandResult::failure(error);
             return CommandResult::success(walker_json(id, *state.walkers->find(id)));
         }});
    add(registry,
        {"stop_walker",
         "Stops a walker and lets it go: drawn where the scene has it again. View state.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", Json::array({"id"})},
              {"properties", {{"id", id_schema()}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.walkers == nullptr) return CommandResult::failure("walking is not available");
             const world::EntityId id = args["id"].get<world::EntityId>();
             if (!state.walkers->stop(id)) return CommandResult::failure("entity " + std::to_string(id) + " is not walking");
             return CommandResult::success(Json{{"stopped", id}});
         }});
    add(registry,
        {"get_walker",
         "The walkers: how many, and how many are searching for the way, walking, arrived or failed. With "
         "`id`, that one: `phase`, `feet` (where it stands), `target`, `speed`, `velocity`, its `path` "
         "(corners), the `corner` it walks to, metres `remaining`, `searches` (1, more when it got stuck "
         "and looked again) and, failed, the `reason`.",
         Json{{"type", "object"}, {"additionalProperties", false}, {"properties", {{"id", id_schema()}}}},
         true,
         [&state](const Json& args, CommandContext&) {
             if (state.walkers == nullptr) return CommandResult::failure("walking is not available");
             Json out = {{"walkers", state.walkers->size()}};
             usize by[4] = {};
             for (const auto& [id, w] : state.walkers->walkers()) ++by[static_cast<usize>(w.phase)];
             out["searching"] = by[0];
             out["walking"] = by[1];
             out["arrived"] = by[2];
             out["failed"] = by[3];
             if (args.contains("id")) {
                 const world::EntityId id = args["id"].get<world::EntityId>();
                 const WalkerState::Walker* w = state.walkers->find(id);
                 if (w == nullptr) return CommandResult::failure("entity " + std::to_string(id) + " is not walking");
                 out["walker"] = walker_json(id, *w);
             }
             return CommandResult::success(std::move(out));
         }});
}

} // namespace giant::editor
