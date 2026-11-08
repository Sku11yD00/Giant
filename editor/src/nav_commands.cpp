// Navigation commands: where people can walk (nav_state.h) and the way
// from one place to another over it. View state, not undoable.
#include "editor_commands.h"
#include "nav_state.h"

#include "giant/core/assert.h"

#include <string>

namespace giant::editor {

namespace {

using ai::CommandContext;
using ai::CommandResult;
using ai::Json;

void add(ai::CommandRegistry& registry, ai::CommandDesc desc) {
    const bool added = registry.add(std::move(desc));
    GIANT_ASSERT(added, "nav command could not be registered");
}

Json point_schema(const char* description) {
    return {{"type", "array"},
            {"items", {{"type", "number"}, {"minimum", -1e8}, {"maximum", 1e8}}},
            {"minItems", 3},
            {"maxItems", 3},
            {"description", description}};
}

DVec3 point_of(const Json& p) { return {p[0].get<double>(), p[1].get<double>(), p[2].get<double>()}; }
Json dvec(const DVec3& v) { return Json::array({v.x, v.y, v.z}); }

} // namespace

void register_nav_commands(ai::CommandRegistry& registry, EditorState& state) {
    add(registry,
        {"find_path",
         "The way a person walks from `from` to `to` (world metres; heights are ignored: each end stands "
         "on the walkable ground within 2 m of it). Walkable ground is where the slope is at most 40 "
         "degrees, with 1.8 m head room, kept 0.35 m from what stands in the way (fixed cubes, spheres, "
         "planes, meshes, kit pieces, scatter tree trunks); steps up to 0.4 m are walked up. The tiles it "
         "needs (64 m cells) are built now, at most `max_tiles` (default 64). Returns `state` (found, "
         "failed with a `reason`, or missing: it needed more tiles than it may build, `missing` the cell), "
         "`path` (corners [x, y, z] where one stands, start to end), `length` (metres along it), `nodes` "
         "(samples looked at) and `tiles_built`.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", Json::array({"from", "to"})},
              {"properties",
               {{"from", point_schema("World metres [x, y, z].")},
                {"to", point_schema("World metres [x, y, z].")},
                {"max_tiles", {{"type", "integer"}, {"minimum", 0}, {"maximum", 512},
                               {"description", "Tiles it may build now (default 64)."}}}}}},
         false, // follows the scene's latest edits first (view state)
         [&state](const Json& args, CommandContext&) {
             if (state.nav == nullptr) return CommandResult::failure("navigation is not available");
             state.nav->follow(state.scene, state.scene.untaken_changes());
             const u32 max_tiles = args.contains("max_tiles") ? args["max_tiles"].get<u32>() : 64u;
             u32 built = 0;
             const std::unique_ptr<nav::PathSearch> search =
                 state.nav->find_path_now(state.scene, point_of(args["from"]), point_of(args["to"]), max_tiles, &built);
             Json out = {{"state", nav::to_string(search->state())},
                         {"nodes", search->nodes()},
                         {"tiles_built", built}};
             if (search->state() == nav::SearchState::found) {
                 Json path = Json::array();
                 for (const DVec3& p : search->path()) path.push_back(dvec(p));
                 out["path"] = std::move(path);
                 out["length"] = search->length();
             } else if (search->state() == nav::SearchState::failed) {
                 out["reason"] = search->reason();
             } else if (search->state() == nav::SearchState::missing) {
                 out["missing"] = Json::array({search->missing_tile().x, search->missing_tile().z});
             }
             return CommandResult::success(std::move(out));
         }});
    add(registry,
        {"get_nav",
         "The walkable map: tiles built (64 m cells, 128 x 128 samples 0.5 m apart), being built, stale "
         "(waiting to be built again after what they were built from changed), built ever, searches "
         "running, and the settings (max_slope_degrees, step_height, clearance, agent_radius). With "
         "`point` (world metres [x, y, z]): its tile built now if it is not (or is stale), and whether "
         "one can stand there (`walkable`), the height one stands at (`height`, null: no ground) and how "
         "many of the tile's samples are walkable.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"properties", {{"point", point_schema("World metres [x, y, z].")}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.nav == nullptr) return CommandResult::failure("navigation is not available");
             NavState& nav = *state.nav;
             nav.follow(state.scene, state.scene.untaken_changes());
             const nav::NavSettings& s = nav.settings();
             Json out = {{"tiles", nav.map().size()},
                         {"building", nav.building()},
                         {"stale", nav.stale()},
                         {"tiles_built", nav.tiles_built()},
                         {"searches", nav.searches()},
                         {"settings",
                          {{"max_slope_degrees", s.max_slope_degrees},
                           {"step_height", s.step_height},
                           {"clearance", s.clearance},
                           {"agent_radius", s.agent_radius}}}};
             if (args.contains("point")) {
                 const DVec3 p = point_of(args["point"]);
                 const world::CellCoord cell = world::cell_of(p);
                 const nav::NavTile* tile = nav.tile_now(state.scene, cell);
                 const nav::NavMap::Sample at = nav.map().at(p.x, p.z);
                 out["point"] = {{"cell", Json::array({cell.x, cell.z})},
                                 {"walkable", at.walkable},
                                 {"height", std::isfinite(at.height) ? Json(at.height) : Json(nullptr)},
                                 {"tile_walkable", tile != nullptr ? tile->walkable_count : 0}};
             }
             return CommandResult::success(std::move(out));
         }});
}

} // namespace giant::editor
