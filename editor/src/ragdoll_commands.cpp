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

// Its hit capsules back now, not a frame later (one let go: rare, so the look over every character is no matter).
void follow_hits(EditorState& state) {
    if (state.hits == nullptr || state.animation == nullptr) return;
    // (The animation first: one let go is posed by its clips again before its capsules are made.)
    state.animation->follow_now(state.scene, *state.meshes, state.scene.untaken_changes());
    state.hits->update(state.scene, *state.meshes, *state.animation);
}

Json settings_json(const RagdollState::Settings& s) {
    return {{"shot_drop", s.shot_drop}, {"blast_drop", s.blast_drop}, {"starts_per_step", s.starts_per_step}};
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
    // A shot that meets a ragdoll's bone names its joint (and its zone by that).
    state.physics->ragdoll_joint = [&state](world::EntityId id, u32 bone) { return state.ragdolls->joint_name(id, bone); };
    state.ragdolls->set_hit_targets(state.hits);
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
         "`velocity` [x, y, z] m/s every bone starts with (default: a walker's own, else at rest); over it each "
         "bone moves as its joint was moving in its animation. A walker "
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
             // (The physics' world as the scene is now: one made in a scene it has yet to notice would go with its clearing.)
             state.physics->flush(state.scene);
             const std::string error = state.ragdolls->start(state.scene, *state.meshes, id, velocity);
             if (!error.empty()) return CommandResult::failure(error);
             // (Its hit capsules went as it started: a bullet flying this frame meets only its bones.)
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
        {"set_ragdoll_settings",
         "How shots and blasts drop characters (those bullets meet by their capsules: visible skinned "
         "entities with a body; and walkers, met by their body). "
         "`shot_drop`: the push in N s (the momentum a bullet loses in it: about 3 for a pistol round that "
         "stops, 8 for a rifle's) a shot gives a standing character's capsule for it to go limp, pushed on "
         "the bone struck (default 3; 0: shots drop none). `blast_drop`: the same for a blast (`impact`), "
         "by its impulse where it reaches the character (default 150; 0: blasts drop none); a blast also "
         "throws ragdolls already lying in its reach. `starts_per_step`: how many of those struck down are "
         "made ragdolls each step, 1 to 64 (default 4: the rest wait their turn standing, oldest first). "
         "Fields left out stay. Returns the settings. View state.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"properties",
               {{"shot_drop", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 1e6}}},
                {"blast_drop", {{"type", "number"}, {"minimum", 0.0}, {"maximum", 1e6}}},
                {"starts_per_step", {{"type", "integer"}, {"minimum", 1}, {"maximum", 64}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (const std::string error = unavailable(state); !error.empty()) return CommandResult::failure(error);
             RagdollState::Settings s = state.ragdolls->settings();
             if (args.contains("shot_drop")) s.shot_drop = static_cast<float>(args["shot_drop"].get<double>());
             if (args.contains("blast_drop")) s.blast_drop = static_cast<float>(args["blast_drop"].get<double>());
             if (args.contains("starts_per_step")) s.starts_per_step = args["starts_per_step"].get<u32>();
             if (const std::string error = state.ragdolls->set_settings(s); !error.empty()) return CommandResult::failure(error);
             return CommandResult::success(settings_json(state.ragdolls->settings()));
         }});
    add(registry,
        {"get_ragdoll",
         "The ragdolls: how many, how many are `asleep` (come to rest) and their `bodies` in all; how many "
         "characters struck down are `waiting` to go limp (a few are made each step) and how many shots and "
         "blasts have `dropped` since the editor opened; the `settings` (set_ragdoll_settings). With `id`, "
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
                 Json none = {{"ragdolls", 0}, {"asleep", 0}, {"bodies", 0}, {"waiting", state.ragdolls->waiting()}};
                 none["dropped"] = state.ragdolls->dropped();
                 none["settings"] = settings_json(state.ragdolls->settings());
                 return CommandResult::success(std::move(none));
             }
             for (const auto& [id, r] : state.ragdolls->ragdolls()) {
                 asleep += r.asleep ? 1u : 0u;
                 bodies += r.bones.size();
             }
             Json out = {{"ragdolls", state.ragdolls->size()}, {"asleep", asleep}, {"bodies", bodies}};
             out["waiting"] = state.ragdolls->waiting();
             out["dropped"] = state.ragdolls->dropped();
             out["settings"] = settings_json(state.ragdolls->settings());
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
