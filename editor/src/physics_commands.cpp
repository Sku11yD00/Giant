// Physics commands: simulating the scene's bodies (a play button: view
// state, not undoable), walking it as the player (play mode) and asking the
// physics world what is where.
#include "editor_commands.h"
#include "physics_state.h"
#include "play_state.h"

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

Json vec(const DVec3& v) { return Json::array({v.x, v.y, v.z}); }
Json vec(const Vec3& v) { return Json::array({v.x, v.y, v.z}); }

Json player_json(const EditorState& state) {
    const PlayState& play = *state.play;
    Json out{{"playing", play.playing()}};
    const std::optional<sim::CharacterState> s = play.player();
    if (!s) return out;
    const world::EntityId ground = state.physics->bodies().entity_of(s->ground);
    out["position"] = vec(s->position);
    out["eye"] = vec(state.camera.position);
    out["velocity"] = vec(s->velocity);
    out["on_ground"] = s->on_ground;
    out["sliding"] = s->on_steep;
    // What it stands on: an entity's id, 0 for the ground, null for nothing.
    out["standing_on"] = s->ground == sim::k_no_body ? Json(nullptr) : Json(ground);
    out["yaw"] = play.yaw();
    out["pitch"] = play.pitch();
    out["input"] = {{"forward", play.input().forward}, {"right", play.input().right}, {"sprint", play.input().sprint}};
    out["waiting_for_ground"] = play.waiting_for_ground();
    return out;
}

CommandResult no_play() { return CommandResult::failure("play is not available"); }
CommandResult not_playing() { return CommandResult::failure("not playing (play on first)"); }

} // namespace

void register_physics_commands(ai::CommandRegistry& registry, EditorState& state) {
    add(registry,
        {"simulate",
         "Starts or stops simulating the scene's physics, like a play button. While on, dynamic entities "
         "(body \"dynamic\") fall, collide and come to rest on everything else that collides (fixed "
         "entities, kits, the ground). They are drawn where they move to, but the scene keeps where they "
         "were put (get_entity gives both: `simulated` is where it is now), so undo and saving are not "
         "touched; edits made meanwhile are followed (a moved entity is put there and falls from there). "
         "Stopping draws every one where the scene has it again (and stops play). View state, not undoable.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"on"}},
              {"properties", {{"on", {{"type", "boolean"}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.physics == nullptr) return no_physics();
             const bool on = args["on"].get<bool>();
             // Off while playing: play stops too (it needs the simulation).
             if (!on && state.play != nullptr && state.play->playing()) state.play->stop(state.camera);
             state.physics->set_simulating(on);
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
         "Casts a ray through what collides (entities with a body, kits, the ground, the player) and returns "
         "the first hit: the entity (0 for the ground; `player` true for the player), the point, the surface "
         "normal and the distance; hit false "
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
             Json out{{"hit", true},
                      {"entity", state.physics->bodies().entity_of(hit->body)},
                      {"point", vec(hit->point)},
                      {"normal", vec(hit->normal)},
                      {"distance", hit->distance}};
             if (state.play != nullptr && state.play->playing() && hit->body == state.play->player_body()) {
                 out["player"] = true;
             }
             return CommandResult::success(out);
         }});

    add(registry,
        {"play",
         "Starts or stops play mode: walking the scene as the player, a 1.8 m capsule that walks up steps "
         "(0.4 m), stops at walls, slides off slopes over 50 degrees, jumps (about 1.3 m) and pushes dynamic "
         "entities. Starting puts it on what is under the camera, looking where the camera looks, turns "
         "simulating on and puts the camera at its eyes; in the viewport the mouse looks, W A S D walk, "
         "Shift runs, Space jumps, Esc stops. Stopping takes it away, puts the camera back and simulating "
         "back as it was. A new scene stops it. View state, not undoable. Returns get_player's report.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"on"}},
              {"properties", {{"on", {{"type", "boolean"}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.physics == nullptr || state.play == nullptr) return no_play();
             if (args["on"].get<bool>()) {
                 if (!state.play->start(state.scene, state.camera, state.scene_generation)) {
                     return CommandResult::failure("the player could not be put there");
                 }
             } else {
                 state.play->stop(state.camera);
             }
             return CommandResult::success(player_json(state));
         }});

    add(registry,
        {"set_player_input",
         "While playing, tells the player what to do, as the keys would (held until changed; a key pressed "
         "takes over again): `forward` and `right` from -1 to 1 along where it looks (W/S and D/A), "
         "`sprint` (Shift: 8 m/s, else 4), `jump` true jumps once (as soon as it is on the ground), `yaw` and "
         "`pitch` where it looks (degrees, as the camera's: yaw 0 looks down -z, 90 down -x). Leave out what "
         "stays. Time moves with the frames, or with step_player. Returns get_player's report.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"properties",
               {{"forward", {{"type", "number"}, {"minimum", -1}, {"maximum", 1}}},
                {"right", {{"type", "number"}, {"minimum", -1}, {"maximum", 1}}},
                {"sprint", {{"type", "boolean"}}},
                {"jump", {{"type", "boolean"}}},
                {"yaw", {{"type", "number"}, {"minimum", -720}, {"maximum", 720}}},
                {"pitch", {{"type", "number"}, {"minimum", -89}, {"maximum", 89}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.physics == nullptr || state.play == nullptr) return no_play();
             if (!state.play->playing()) return not_playing();
             PlayerInput in = state.play->input();
             in.forward = args.value("forward", in.forward);
             in.right = args.value("right", in.right);
             in.sprint = args.value("sprint", in.sprint);
             state.play->set_input(in);
             state.play->look(args.value("yaw", state.play->yaw()), args.value("pitch", state.play->pitch()));
             if (args.value("jump", false)) state.play->jump();
             state.play->after_step(state.camera, 0.0); // the camera looks the new way at once
             return CommandResult::success(player_json(state));
         }});

    add(registry,
        {"step_player",
         "While playing, moves time on by `seconds` now, in fixed 1/60 s steps (at least one), with the "
         "player doing what it was told: for checking where it gets to. Returns get_player's report.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"seconds"}},
              {"properties",
               {{"seconds",
                 {{"type", "number"}, {"minimum", 0}, {"maximum", 60}, {"description", "Up to a minute."}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.physics == nullptr || state.play == nullptr) return no_play();
             if (!state.play->playing()) return not_playing();
             const double seconds = args["seconds"].get<double>();
             const u32 steps = std::max(1u, static_cast<u32>(std::lround(seconds / sim::PhysicsWorld::k_step_seconds)));
             state.physics->flush(state.scene);
             u32 stepped = 0;
             for (; stepped < steps; ++stepped) {
                 if (!state.play->before_step(state.scene, state.camera, state.scene_generation,
                                              sim::PhysicsWorld::k_step_seconds)) {
                     break; // play stopped (a new scene)
                 }
                 state.physics->run_steps(1);
                 state.play->after_step(state.camera, sim::PhysicsWorld::k_step_seconds);
             }
             Json out = player_json(state);
             out["stepped"] = stepped;
             return CommandResult::success(out);
         }});

    add(registry,
        {"get_player",
         "Reports play mode: whether playing and, while it is, the player's feet (`position`), `eye` (the "
         "camera), `velocity`, `on_ground`, `sliding` (on a slope too steep), `standing_on` (an entity's id, "
         "0 the ground, null nothing), `yaw`, `pitch`, its `input`, and `waiting_for_ground` (held still "
         "while the ground under it is still being made).",
         Json{{"type", "object"}, {"additionalProperties", false}, {"properties", Json::object()}},
         true,
         [&state](const Json&, CommandContext&) {
             if (state.physics == nullptr || state.play == nullptr) return no_play();
             return CommandResult::success(player_json(state));
         }});
}

} // namespace giant::editor
