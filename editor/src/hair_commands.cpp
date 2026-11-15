// Hair commands: what the entities' hair is doing (hair_state.h). Hair is
// set on an entity with create_entity / set_entity (`hair`). View state.
#include "editor_commands.h"
#include "hair_state.h"

#include "giant/core/assert.h"

#include <string>

namespace giant::editor {

namespace {

using ai::CommandContext;
using ai::CommandResult;
using ai::Json;

} // namespace

void register_hair_commands(ai::CommandRegistry& registry, EditorState& state) {
    const bool added = registry.add(
        {"get_hair",
         "The entities' hair (set with create_entity / set_entity `hair`) and what it is doing: entities "
         "wearing it, grooms grown, `reading` (waiting for a groom file, `hair.file`, to be read on a worker: "
         "seconds for a large one; at most 8 different files are held at once and hair past them waits) and "
         "still growing (a groom grows on a worker after it is put on or "
         "changed: a few frames, about a second for 200000 strands; changed hair shows its old groom until "
         "the new one is grown and sent to the GPU, and hair changed every frame keeps the old one until the "
         "changes stop), how many moved in the last update (the nearest within 30 m of the camera with "
         "`moves` on, up to 16 of them and 4096 guides in all; the rest hang as grown), how many the "
         "renderer is given, strands in all, entities left out (more than 256 wear hair, or more than "
         "2000000 strands in all: lowest ids first), simulations put back at rest after going wrong. With "
         "`id`, that entity's too: whether it is grown, reading, growing, ready (the renderer has all of its groom, so "
         "it is drawn from the next frame on), simulated and left out, its `problem` (why a mesh entity grows "
         "none: its file is not loaded or has no skeleton, no such joint, or a groom file that cannot be read, which is "
         "not tried again until the setting changes; empty otherwise), a character's `joint` and the "
         "`capsules` of its body the hair is pushed aside by, where it is drawn (`position`, `rotation` x y z "
         "w: its entity's, as the physics, a walker or a swing moves it, or its joint's as animated), its "
         "strands and guides, its longest strand (metres), its bounds, and with `points` true where its "
         "guides' points are (16 a guide, root first, in the entity's or joint's space without its scale: as "
         "simulated, or as grown). `wait` true waits for "
         "every groom being grown first and shows it at once; the renderer still needs a few frames to draw "
         "it (`ready`).",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"properties",
               {{"id", {{"type", "integer"}, {"minimum", 1}, {"maximum", 9007199254740991.0}}},
                {"points", {{"type", "boolean"}}},
                {"wait", {{"type", "boolean"}}}}}},
         false, // follows the scene's latest edits first (view state)
         [&state](const Json& args, CommandContext&) {
             if (state.hair == nullptr) return CommandResult::failure("hair is not available");
             state.hair->follow(state.scene, state.scene.untaken_changes()); // the latest edits first
             if (args.value("wait", false)) state.hair->finish();
             const HairStats s = state.hair->stats();
             Json out = {{"wearing", s.wearing},     {"grown", s.grown},   {"growing", s.growing},   {"reading", s.reading},
                         {"simulated", s.simulated}, {"drawn", s.drawn},   {"strands", s.strands},
                         {"left_out", s.left_out},   {"resets", s.resets}};
             if (args.contains("id")) {
                 const world::EntityId id = args["id"].get<world::EntityId>();
                 HairState::View v;
                 if (!state.hair->view(id, v)) {
                     return CommandResult::failure("entity " + std::to_string(id) +
                                                   " wears no hair (hair.on, a visible cube, sphere or character)");
                 }
                 Json one = {{"id", id},
                             {"grown", v.grown},
                             {"growing", v.growing},
                             {"reading", v.reading},
                             {"ready", v.ready},
                             {"left_out", v.left_out},
                             {"problem", v.problem},
                             {"joint", v.joint},
                             {"position", {v.position.x, v.position.y, v.position.z}},
                             {"rotation", {v.rotation.x, v.rotation.y, v.rotation.z, v.rotation.w}},
                             {"capsules", v.capsules},
                             {"simulated", v.simulated},
                             {"strands", v.strands},
                             {"guides", v.guides},
                             {"longest", v.longest},
                             {"bounds_min", {v.bounds_min.x, v.bounds_min.y, v.bounds_min.z}},
                             {"bounds_max", {v.bounds_max.x, v.bounds_max.y, v.bounds_max.z}}};
                 if (args.value("points", false)) {
                     Json points = Json::array();
                     for (const Vec3& p : v.guide_points) points.push_back({p.x, p.y, p.z});
                     one["points"] = std::move(points);
                 }
                 out["hair"] = std::move(one);
             }
             return CommandResult::success(std::move(out));
         }});
    GIANT_ASSERT(added, "hair command could not be registered");
}

} // namespace giant::editor
