// Physics commands: simulating the scene's bodies (a play button: view
// state, not undoable) and asking the physics world what is where.
#include "editor_commands.h"
#include "physics_state.h"

#include "giant/core/assert.h"

#include <cmath>

namespace giant::editor {

namespace {

using ai::CommandContext;
using ai::CommandResult;
using ai::Json;

void add(ai::CommandRegistry& registry, ai::CommandDesc desc) {
    const bool added = registry.add(std::move(desc));
    GIANT_ASSERT(added, "physics command could not be registered");
}

Json triple(double limit, const char* description) {
    return {{"type", "array"},
            {"items", {{"type", "number"}, {"minimum", -limit}, {"maximum", limit}}},
            {"minItems", 3},
            {"maxItems", 3},
            {"description", description}};
}

Json physics_json(const EditorState& state) {
    const PhysicsState& p = *state.physics;
    const sim::PhysicsStats w = p.world().stats();
    const sim::ScenePhysicsStats s = p.bodies().stats();
    return {{"simulating", p.simulating()},
            {"bodies", w.bodies},
            {"active", w.active},
            {"steps", w.steps},
            {"dropped_steps", w.dropped},
            {"last_step_ms", w.last_step_ms},
            {"entity_bodies", s.entity_bodies},
            {"dynamic_bodies", s.dynamic_bodies},
            {"ground_tiles", s.tile_bodies},
            {"not_colliding", s.not_colliding},
            {"refused", s.refused},
            {"waiting", s.waiting},
            {"ground_waiting", s.tiles_waiting},
            {"moved", s.posed}};
}

CommandResult no_physics() { return CommandResult::failure("physics is not available"); }

} // namespace

void register_physics_commands(ai::CommandRegistry& registry, EditorState& state) {
    add(registry,
        {"simulate",
         "Starts or stops simulating the scene's physics, like a play button. While on, dynamic entities "
         "(body \"dynamic\") fall, collide and come to rest on everything else that collides (fixed "
         "entities, kits, the ground). They are drawn where they move to, but the scene keeps where they "
         "were put (get_entity gives both: `simulated` is where it is now), so undo and saving are not "
         "touched; edits made meanwhile are followed (a moved entity is put there and falls from there). "
         "Stopping draws every one where the scene has it again. View state, not undoable.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"on"}},
              {"properties", {{"on", {{"type", "boolean"}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.physics == nullptr) return no_physics();
             state.physics->set_simulating(args["on"].get<bool>());
             return CommandResult::success(physics_json(state));
         }});

    add(registry,
        {"step_physics",
         "While simulating, moves time on by `seconds` now, in fixed 1/60 s steps (rounded to the nearest "
         "step, at least one), whatever the frame rate: for checking where things come to rest. Returns "
         "get_physics's report.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"seconds"}},
              {"properties",
               {{"seconds",
                 {{"type", "number"}, {"minimum", 0}, {"maximum", 60}, {"description", "Up to a minute."}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.physics == nullptr) return no_physics();
             if (!state.physics->simulating()) return CommandResult::failure("not simulating (simulate on first)");
             const double seconds = args["seconds"].get<double>();
             const u32 steps = std::max(1u, static_cast<u32>(std::lround(seconds / sim::PhysicsWorld::k_step_seconds)));
             state.physics->flush(state.scene);
             state.physics->run_steps(steps);
             Json out = physics_json(state);
             out["stepped"] = steps;
             return CommandResult::success(out);
         }});

    add(registry,
        {"get_physics",
         "Reports the physics: whether simulating, how many bodies (entities, dynamic ones, ground tiles; "
         "entities that do not collide, such as meshes and decals for now, or the world refused), what is "
         "still `waiting` to be made (the bodies follow the scene a share each frame; ground tiles' shapes "
         "are made in the background), how many entities simulating has `moved`, steps run and the last "
         "step's CPU milliseconds.",
         Json{{"type", "object"}, {"additionalProperties", false}, {"properties", Json::object()}},
         true,
         [&state](const Json&, CommandContext&) {
             if (state.physics == nullptr) return no_physics();
             return CommandResult::success(physics_json(state));
         }});

    add(registry,
        {"raycast",
         "Casts a ray through what collides (entities with a body, kits, the ground) and returns the first "
         "hit: the entity (0 for the ground), the point, the surface normal and the distance; hit false "
         "when nothing is within max_distance. Finishes any bodies still waiting first (in a big world "
         "just opened, that can take a moment).",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"origin", "direction"}},
              {"properties",
               {{"origin", triple(1e8, "World metres.")},
                {"direction", triple(1e6, "Need not be unit length; not zero.")},
                {"max_distance",
                 {{"type", "number"}, {"minimum", 0}, {"maximum", 1e6}, {"description", "Metres. Default 10000."}}}}}},
         true,
         [&state](const Json& args, CommandContext&) {
             if (state.physics == nullptr) return no_physics();
             const Json& o = args["origin"];
             const Json& d = args["direction"];
             const DVec3 origin{o[0].get<double>(), o[1].get<double>(), o[2].get<double>()};
             const double dx = d[0].get<double>();
             const double dy = d[1].get<double>();
             const double dz = d[2].get<double>();
             const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
             if (!(length > 1e-9)) return CommandResult::failure("direction is zero");
             const Vec3 direction{static_cast<float>(dx / length), static_cast<float>(dy / length),
                                  static_cast<float>(dz / length)};
             state.physics->flush(state.scene);
             const auto hit = state.physics->world().raycast(origin, direction, args.value("max_distance", 10000.0));
             if (!hit) return CommandResult::success(Json{{"hit", false}});
             return CommandResult::success(Json{{"hit", true},
                                                {"entity", state.physics->bodies().entity_of(hit->body)},
                                                {"point", Json::array({hit->point.x, hit->point.y, hit->point.z})},
                                                {"normal", Json::array({hit->normal.x, hit->normal.y, hit->normal.z})},
                                                {"distance", hit->distance}});
         }});
}

} // namespace giant::editor
