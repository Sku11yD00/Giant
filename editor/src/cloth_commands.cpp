// Cloth commands: how the cloth entities move in the wind (cloth_state.h),
// and pushing them as a blast or a shot would. View state, not undoable.
#include "cloth_state.h"
#include "editor_commands.h"

#include "giant/core/assert.h"

#include <cmath>
#include <string>

namespace giant::editor {

namespace {

using ai::CommandContext;
using ai::CommandResult;
using ai::Json;

void add(ai::CommandRegistry& registry, ai::CommandDesc desc) {
    const bool added = registry.add(std::move(desc));
    GIANT_ASSERT(added, "cloth command could not be registered");
}

Json triple(double minimum, double maximum, const char* description) {
    return {{"type", "array"},
            {"items", {{"type", "number"}, {"minimum", minimum}, {"maximum", maximum}}},
            {"minItems", 3},
            {"maxItems", 3},
            {"description", description}};
}

} // namespace

void register_cloth_commands(ai::CommandRegistry& registry, EditorState& state) {
    add(registry,
        {"get_cloth",
         "The cloth entities (shape cloth) and how they move: sheets in all, how many moved in the last "
         "step (the nearest within `distance` metres of the camera, at most `max_simulated`, and at most "
         "`max_points` points between them; farther ones hold still), how many rest asleep (no wind, "
         "nothing pushing), fixed steps taken, sheets put back at rest after going wrong, meshes drawn and "
         "waiting, poses handed to the renderer. With `id`, that sheet too: its grid, whether it is asleep, "
         "and where its points are (`points`, in its own space: x across, y down from its origin, z out of "
         "its face; row after row from the top).",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"properties", {{"id", {{"type", "integer"}, {"minimum", 1}, {"maximum", 9007199254740991.0}}}}}},
         false, // follows the scene's latest edits first (view state)
         [&state](const Json& args, CommandContext&) {
             if (state.cloth == nullptr) return CommandResult::failure("cloth is not available");
             state.cloth->catch_up(state.scene, state.scene.untaken_changes()); // the latest edits first
             const sim::Cloths& cloths = state.cloth->cloths();
             const sim::ClothStats s = cloths.stats();
             const sim::ClothSettingsSim& settings = cloths.settings();
             Json out = {{"sheets", s.sheets},
                         {"simulated", s.simulated},
                         {"asleep", s.asleep},
                         {"steps", s.steps},
                         {"resets", s.resets},
                         {"drawn", state.cloth->meshes().size()},
                         {"waiting", state.cloth->waiting()},
                         {"poses", state.cloth->poses()},
                         {"distance", settings.distance},
                         {"max_simulated", settings.max_simulated},
                         {"max_points", settings.max_points}};
             if (args.contains("id")) {
                 const world::EntityId id = args["id"].get<world::EntityId>();
                 const sim::Cloths::Sheet* sheet = cloths.find(id);
                 if (sheet == nullptr) {
                     return CommandResult::failure("entity " + std::to_string(id) + " is no cloth shown");
                 }
                 Json points = Json::array();
                 for (const Vec3& p : sheet->points()) points.push_back({p.x, p.y, p.z});
                 out["sheet"] = {{"id", id},
                                 {"columns", sheet->columns()},
                                 {"rows", sheet->rows()},
                                 {"asleep", sheet->asleep()},
                                 {"drawn", state.cloth->meshes().count(id) != 0},
                                 {"points", std::move(points)}};
             }
             return CommandResult::success(std::move(out));
         }});
    add(registry,
        {"push_cloth",
         "Pushes the cloth near a point as a blast or a passing shot would: every point of every sheet "
         "within `radius` metres of `position` gains `velocity` (m/s, world), less towards the edge; "
         "sleeping sheets wake. It moves on the next steps (the wind takes over after). View state, not "
         "undoable.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", Json::array({"position", "radius", "velocity"})},
              {"properties",
               {{"position", triple(-1e8, 1e8, "World metres [x, y, z].")},
                {"radius", {{"type", "number"}, {"minimum", 0.001}, {"maximum", 1000}, {"description", "Metres."}}},
                {"velocity", triple(-1000, 1000, "The change of speed at the centre, m/s [x, y, z].")}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.cloth == nullptr) return CommandResult::failure("cloth is not available");
             state.cloth->catch_up(state.scene, state.scene.untaken_changes()); // a sheet made just now too
             const Json& p = args["position"];
             const Json& v = args["velocity"];
             state.cloth->cloths().push({p[0].get<double>(), p[1].get<double>(), p[2].get<double>()},
                                        args["radius"].get<float>(),
                                        {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()});
             return CommandResult::success(Json{{"pushed", true}});
         }});
}

} // namespace giant::editor
