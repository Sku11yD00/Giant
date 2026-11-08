// Swing commands: how the swinging entities move in the wind
// (swing_state.h), and pushing them as a hand or a blast would; and the
// physics' blows reaching them and the cloth (connect_blows). View state,
// not undoable.
#include "cloth_state.h"
#include "editor_commands.h"
#include "physics_state.h"
#include "swing_state.h"

#include "giant/core/assert.h"

#include <string>

namespace giant::editor {

namespace {

using ai::CommandContext;
using ai::CommandResult;
using ai::Json;

void add(ai::CommandRegistry& registry, ai::CommandDesc desc) {
    const bool added = registry.add(std::move(desc));
    GIANT_ASSERT(added, "swing command could not be registered");
}

Json triple(double minimum, double maximum, const char* description) {
    return {{"type", "array"},
            {"items", {{"type", "number"}, {"minimum", minimum}, {"maximum", maximum}}},
            {"minItems", 3},
            {"maxItems", 3},
            {"description", description}};
}

Json vec(const Vec3& v) { return Json::array({v.x, v.y, v.z}); }
Json dvec(const DVec3& v) { return Json::array({v.x, v.y, v.z}); }

} // namespace

void connect_blows(EditorState& state) {
    if (state.physics == nullptr) return;
    state.physics->on_hits = [&state](std::span<const PhysicsState::ShotHit> hits) {
        if (state.swing == nullptr) return;
        for (const PhysicsState::ShotHit& h : hits) {
            if (h.entity != 0) state.swing->swings().knock(h.entity, h.hit.impulse);
        }
    };
    state.physics->on_impact = [&state](const DVec3& point, float radius, float impulse) {
        if (state.swing != nullptr) {
            state.swing->follow(state.scene, state.scene.untaken_changes()); // one made just now too
            state.swing->swings().blast(point, radius, impulse);
        }
        if (state.cloth != nullptr) {
            state.cloth->catch_up(state.scene, state.scene.untaken_changes());
            state.cloth->cloths().blast(point, radius, impulse);
        }
    };
}

void register_swing_commands(ai::CommandRegistry& registry, EditorState& state) {
    add(registry,
        {"get_swing",
         "The swinging entities (set_entity's `swing`) and how they move: swings in all, how many moved in "
         "the last step (the nearest within `distance` metres of the camera, at most `max_simulated`; "
         "farther ones hold still), how many rest asleep (no change of wind about them, nothing knocking "
         "them), the most fixed steps one took, those put back at rest after going wrong. With `id`, that "
         "one too: degrees from hanging straight (`angle`), whether asleep, its pivot, where it is drawn "
         "(`position`, `rotation` as [x, y, z, w]), its speed (`velocity`, m/s) and the box the wind "
         "pushes on (`size`, metres).",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"properties", {{"id", {{"type", "integer"}, {"minimum", 1}, {"maximum", 9007199254740991.0}}}}}},
         false, // follows the scene's latest edits first (view state)
         [&state](const Json& args, CommandContext&) {
             if (state.swing == nullptr) return CommandResult::failure("swinging is not available");
             state.swing->follow(state.scene, state.scene.untaken_changes()); // the latest edits first
             const sim::Swings& swings = state.swing->swings();
             const sim::SwingStats s = swings.stats();
             Json out = {{"swings", s.swings},
                         {"simulated", s.simulated},
                         {"asleep", s.asleep},
                         {"steps", s.steps},
                         {"resets", s.resets},
                         {"distance", swings.settings().distance},
                         {"max_simulated", swings.settings().max_simulated}};
             if (args.contains("id")) {
                 const world::EntityId id = args["id"].get<world::EntityId>();
                 const sim::Swings::Swing* w = swings.find(id);
                 if (w == nullptr) {
                     return CommandResult::failure("entity " + std::to_string(id) + " is not swinging (swing on, shown)");
                 }
                 const Quat& q = w->pose.rotation;
                 out["swing"] = {{"id", id},
                                 {"angle", w->angle()},
                                 {"asleep", w->asleep},
                                 {"pivot", dvec(w->pivot)},
                                 {"position", dvec(w->pose.position)},
                                 {"rotation", Json::array({q.x, q.y, q.z, q.w})},
                                 {"velocity", vec(w->velocity)},
                                 {"size", vec(w->size)}};
             }
             return CommandResult::success(std::move(out));
         }});
    add(registry,
        {"push_swing",
         "Pushes the swinging entities near a point: every one whose origin is within `radius` metres of "
         "`position` gains `velocity` (m/s, world: a hand, a gust), or takes `impulse` (N s) away from "
         "`position` (a blast: its speed changes by that over its weight, at most 20 m/s); less towards "
         "the edge. Give one of velocity or impulse. Sleeping ones wake. They swing on the next steps. "
         "(impact, while simulating, throws them and the cloth too; a shot knocks what it hits.) View "
         "state, not undoable. Returns `pushed` (how many).",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", Json::array({"position", "radius"})},
              {"properties",
               {{"position", triple(-1e8, 1e8, "World metres [x, y, z].")},
                {"radius", {{"type", "number"}, {"minimum", 0.001}, {"maximum", 1000}, {"description", "Metres."}}},
                {"velocity", triple(-1000, 1000, "The change of speed at the centre, m/s [x, y, z].")},
                {"impulse", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1e9},
                             {"description", "N s at the centre, outwards."}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.swing == nullptr) return CommandResult::failure("swinging is not available");
             if (args.contains("velocity") == args.contains("impulse")) {
                 return CommandResult::failure("give one of velocity or impulse");
             }
             state.swing->follow(state.scene, state.scene.untaken_changes()); // one made just now too
             const Json& p = args["position"];
             const DVec3 at{p[0].get<double>(), p[1].get<double>(), p[2].get<double>()};
             const float radius = args["radius"].get<float>();
             usize pushed = 0;
             if (args.contains("velocity")) {
                 const Json& v = args["velocity"];
                 pushed = state.swing->swings().push(at, radius, {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()});
             } else {
                 pushed = state.swing->swings().blast(at, radius, args["impulse"].get<float>());
             }
             return CommandResult::success(Json{{"pushed", pushed}});
         }});
}

} // namespace giant::editor
