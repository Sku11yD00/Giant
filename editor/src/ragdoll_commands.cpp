// Ragdoll commands: skinned characters gone limp while the physics
// simulates (ragdoll_state.h). View state, not undoable.
#include "animation_state.h"
#include "editor_commands.h"
#include "hit_targets.h"
#include "mesh_library.h"
#include "physics_state.h"
#include "ragdoll_state.h"

#include "giant/core/assert.h"

#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace giant::editor {

namespace {

using ai::CommandContext;
using ai::CommandResult;
using ai::Json;

void add(ai::CommandRegistry& registry, ai::CommandDesc desc) {
    const bool added = registry.add(std::move(desc));
    GIANT_ASSERT(added, "ragdoll command could not be registered");
}

Json id_schema() { return {{"type", "integer"}, {"minimum", 1}, {"maximum", 9007199254740991.0}}; }
Json dvec(const DVec3& v) { return Json::array({v.x, v.y, v.z}); }

Json ragdoll_json(const EditorState& state, const RagdollState::Ragdoll& r, bool bones) {
    const sim::PhysicsWorld& world = state.physics->world();
    const std::span<const sim::BodyId> bodies = world.ragdoll_bodies(r.id);
    float mass = 0.0f;
    for (const RagdollState::Bone& b : r.bones) mass += b.mass;
    Json out = {{"id", r.entity},
                {"bones", r.bones.size()},
                {"joints", r.bone_of.size()},
                {"mass", mass},
                {"asleep", r.asleep}};
    if (const auto p = state.ragdolls->poses().find(r.entity); p != state.ragdolls->poses().end()) {
        out["position"] = dvec(p->second.position);
    }
    if (!bodies.empty()) {
        if (const auto s = world.state(bodies.front())) {
            out["root"] = dvec(s->position);
            out["velocity"] = Json::array({s->linear_velocity.x, s->linear_velocity.y, s->linear_velocity.z});
        }
    }
    if (!bones) return out;
    const std::vector<std::string>& names = r.skinned->skeleton.names;
    const auto name = [&names](u32 joint) { return joint < names.size() ? names[joint] : std::string(); };
    Json list = Json::array();
    for (usize i = 0; i < r.bones.size(); ++i) {
        const RagdollState::Bone& b = r.bones[i];
        Json bone = {{"joint", name(b.joint)},
                     {"parent", b.parent >= 0 ? Json(name(r.bones[static_cast<usize>(b.parent)].joint)) : Json()},
                     {"kind", RagdollState::to_string(b.kind)},
                     {"hinge", b.hinge},
                     {"mass", b.mass}};
        if (i < bodies.size()) {
            if (const auto s = world.state(bodies[i])) bone["position"] = dvec(s->position);
        }
        list.push_back(std::move(bone));
    }
    out["bone_list"] = std::move(list);
    return out;
}

// Its hit capsules gone (or back) now, not a frame later: a bullet flying this frame meets only its bones (or only them).
void follow_hits(EditorState& state) {
    if (state.hits == nullptr || state.animation == nullptr) return;
    // (The animation first: one let go is posed by its clips again before its capsules are made.)
    state.animation->follow_now(state.scene, *state.meshes, state.scene.untaken_changes());
    state.hits->update(state.scene, *state.meshes, *state.animation);
}

std::string unavailable(const EditorState& state) {
    if (state.ragdolls == nullptr || state.physics == nullptr || state.meshes == nullptr) return "ragdolls are not available";
    return {};
}

} // namespace

void connect_ragdolls(EditorState& state) {
    if (state.physics == nullptr || state.ragdolls == nullptr || state.meshes == nullptr) return;
    // Round each fixed step run by hand (step_physics), after what was connected before (the walkers).
    state.physics->before_fixed_step = [&state, before = std::move(state.physics->before_fixed_step)](double seconds) {
        if (before) before(seconds);
        state.ragdolls->before_step(state.scene, *state.meshes);
    };
    state.physics->after_fixed_step = [&state, after = std::move(state.physics->after_fixed_step)] {
        if (after) after();
        state.ragdolls->after_step();
    };
}

void register_ragdoll_commands(ai::CommandRegistry& registry, EditorState& state) {
    add(registry,
        {"ragdoll_entity",
         "Makes a skinned character go limp while simulating: a ragdoll made of its bones as it is posed and "
         "stands now (a body for each bone worth one, from the capsules fitted round its joints at import: "
         "fingers and toes ride the hand and foot; joined where its joints are, elbows and knees folding one "
         "way, the rest leaning about the rest pose as far as a bone of that name does), which falls, is "
         "pushed about and comes to rest as bodies do, and poses the character from then on instead of its "
         "clips. `id` (required): a visible mesh entity whose file has a skeleton (the character faces +z); "
         "`velocity` [x, y, z] m/s every bone starts with (default: a walker's own, else at rest). A walker "
         "goes limp where it walks and walks no more. Bullets meet its bones (as flesh) and push them. It is "
         "drawn where its hips have gone; the scene keeps where it was put. Until `stop_ragdoll`, the "
         "simulation stopping, or the entity hidden, removed or given another file. At most 256. View state.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", Json::array({"id"})},
              {"properties",
               {{"id", id_schema()},
                {"velocity",
                 {{"type", "array"},
                  {"items", {{"type", "number"}, {"minimum", -1000.0}, {"maximum", 1000.0}}},
                  {"minItems", 3},
                  {"maxItems", 3}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (const std::string error = unavailable(state); !error.empty()) return CommandResult::failure(error);
             const world::EntityId id = args["id"].get<world::EntityId>();
             std::optional<Vec3> velocity;
             if (args.contains("velocity")) {
                 const Json& v = args["velocity"];
                 velocity = Vec3{v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
             }
             const std::string error = state.ragdolls->start(state.scene, *state.meshes, id, velocity);
             if (!error.empty()) return CommandResult::failure(error);
             follow_hits(state);
             return CommandResult::success(ragdoll_json(state, *state.ragdolls->find(id), true));
         }});
    add(registry,
        {"stop_ragdoll",
         "Lets a ragdoll go: the character is drawn where the scene has it again, playing its own clips. View state.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", Json::array({"id"})},
              {"properties", {{"id", id_schema()}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (const std::string error = unavailable(state); !error.empty()) return CommandResult::failure(error);
             const world::EntityId id = args["id"].get<world::EntityId>();
             if (!state.ragdolls->stop(state.scene, *state.meshes, id)) {
                 return CommandResult::failure("entity " + std::to_string(id) + " is not a ragdoll");
             }
             follow_hits(state);
             return CommandResult::success(Json{{"stopped", id}});
         }});
    add(registry,
        {"get_ragdoll",
         "The ragdolls: how many, how many are `asleep` (come to rest) and their `bodies` in all. With `id`, "
         "that one: `bones` (bodies), `joints` (of its skeleton: the rest ride a bone), `mass` kg, `asleep`, "
         "`position` (where the entity is drawn), `root` and `velocity` (its first bone's, the hips), and with "
         "`bones` true its `bone_list`: each bone's `joint`, `parent` (null for the first), `kind` (what its "
         "name makes it: other, spine, neck, head, collar, upper_arm, forearm, hand, thigh, shin, foot), "
         "`hinge` (it folds one way), `mass` and `position`.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"properties", {{"id", id_schema()}, {"bones", {{"type", "boolean"}}}}}},
         true,
         [&state](const Json& args, CommandContext&) {
             if (const std::string error = unavailable(state); !error.empty()) return CommandResult::failure(error);
             usize asleep = 0, bodies = 0;
             // (Another scene's, or another run's, not yet let go: none of this one's.)
             const bool current = state.ragdolls->current(state.scene);
             if (!current) {
                 if (args.contains("id")) {
                     return CommandResult::failure("entity " + std::to_string(args["id"].get<world::EntityId>()) + " is not a ragdoll");
                 }
                 return CommandResult::success(Json{{"ragdolls", 0}, {"asleep", 0}, {"bodies", 0}});
             }
             for (const auto& [id, r] : state.ragdolls->ragdolls()) {
                 asleep += r.asleep ? 1u : 0u;
                 bodies += r.bones.size();
             }
             Json out = {{"ragdolls", state.ragdolls->size()}, {"asleep", asleep}, {"bodies", bodies}};
             if (args.contains("id")) {
                 const world::EntityId id = args["id"].get<world::EntityId>();
                 const RagdollState::Ragdoll* r = state.ragdolls->find(id);
                 if (r == nullptr) return CommandResult::failure("entity " + std::to_string(id) + " is not a ragdoll");
                 out["ragdoll"] = ragdoll_json(state, *r, args.value("bones", false));
             }
             return CommandResult::success(std::move(out));
         }});
}

} // namespace giant::editor
