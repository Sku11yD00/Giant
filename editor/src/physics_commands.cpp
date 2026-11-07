// Physics commands: simulating the scene's bodies (a play button: view
// state, not undoable), walking it as the player (play mode) and asking the
// physics world what is where.
#include "editor_commands.h"
#include "physics_state.h"
#include "play_state.h"

#include "giant/core/assert.h"

#include <cmath>
#include <memory>
#include <unordered_set>

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

// The scene's kept damage, and what of it is applied now.
Json kept_json(const world::Scene& scene, const sim::ScenePhysicsStats& s, const sim::ForestStats& f) {
    usize kits = 0;
    usize pieces = 0;
    usize trees = 0;
    for (const auto& [c, d] : scene.damage()) {
        kits += d.kits.size();
        for (const world::KeptKit& k : d.kits) pieces += k.pieces.size();
        trees += d.trees.size();
    }
    usize cells = 0;
    for (const auto& [c, d] : scene.damage()) cells += d.empty() ? 0u : 1u;
    return {{"cells", cells}, {"kits", kits}, {"pieces", pieces}, {"trees", trees},
            {"pieces_down", s.kept_pieces}, {"trees_down", f.kept}};
}

Json physics_json(const EditorState& state) {
    const PhysicsState& p = *state.physics;
    const sim::PhysicsStats w = p.world().stats();
    const sim::ScenePhysicsStats s = p.bodies().stats();
    const sim::ForestStats f = p.forest().stats();
    const sim::DustStats d = p.dust().stats();
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
            {"moved", s.posed},
            {"broken_kits", s.broken_kits},
            {"loose_pieces", s.loose_pieces},
            {"rubble", s.rubble},
            {"trees", {{"trunks", f.trunks},
                       {"waiting", f.waiting},
                       {"falling", f.falling},
                       {"fallen", f.fallen},
                       {"laid", f.laid},
                       {"refused", f.refused},
                       {"shrugged", f.shrugged}}},
            {"ground", {{"dents", p.ground().dents()}, {"tiles", p.ground().tiles()}}},
            {"kept", kept_json(state.scene, s, f)},
            {"dust", {{"puffs", d.puffs}, {"emitted", d.emitted}, {"merged", d.merged}, {"dropped", d.dropped}}}};
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
    out["climbing"] = s->climbing;
    out["mantling"] = s->mantling;
    // The wall climbed: an entity's id, 0 for something else (the ground, a tree), null for none.
    out["wall"] = s->climbing && s->wall != sim::k_no_body ? Json(state.physics->bodies().entity_of(s->wall))
                                                            : Json(nullptr);
    const std::optional<world::EntityId> carrier = play.carried_by();
    out["carried_by"] = carrier ? Json(*carrier) : (s->carrier != sim::k_no_body ? Json(0) : Json(nullptr));
    out["input"] = {{"forward", play.input().forward},
                    {"right", play.input().right},
                    {"sprint", play.input().sprint},
                    {"climb", play.input().climb}};
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
             if (!on) state.physics->restore_ground(state.scene); // now: what follows in this batch sees it
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
         "the first hit: the entity (0 for the ground; `player` true for the player; `tree` {cell, point, fallen} "
         "for a scatter tree's trunk), the point, the surface normal and the distance; hit false "
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
             if (const auto tree = state.physics->forest().tree_of(hit->body)) {
                 out["tree"] = {{"cell", {tree->cell.x, tree->cell.z}}, {"point", tree->point}, {"fallen", tree->fallen}};
             }
             return CommandResult::success(out);
         }});

    add(registry,
        {"impact",
         "While simulating, a blow at `point`: the pieces of breakable kits (set_kit_breakable) within "
         "`radius` metres take `impulse` (N s, about mass times speed; less toward the edge), and those it "
         "is enough for come loose, pushed away, with whatever they held up. A footstep, an explosion, a "
         "wrecking ball. Scatter trees (items with `tree`) whose trunk is within reach and takes at least its "
         "strength snap and topple away from the point. Within 600 m of the camera loose pieces fall as "
         "bodies (4096 at most); farther, or past that, they come down at once as rubble lying flat, and "
         "trees with no trunk body (beyond about 400 m) are laid down where they fall. What comes down throws "
         "up dust. Stopping the simulation puts every piece back, stands every tree up and clears the dust. Returns `loosened` (pieces), `felled` (trees) and get_physics.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"point", "radius", "impulse"}},
              {"properties",
               {{"point", triple(1e8, "World metres.")},
                {"radius", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1000}, {"description", "Metres."}}},
                {"impulse", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1e9}, {"description", "N s."}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.physics == nullptr) return no_physics();
             if (!state.physics->simulating()) return CommandResult::failure("not simulating (simulate on first)");
             const Json& p = args["point"];
             state.physics->flush(state.scene);
             const PhysicsState::Impact hit =
                 state.physics->impact({p[0].get<double>(), p[1].get<double>(), p[2].get<double>()},
                                       args["radius"].get<float>(), args["impulse"].get<float>());
             Json out = physics_json(state);
             out["loosened"] = hit.loosened;
             out["felled"] = hit.felled;
             return CommandResult::success(out);
         }});

    add(registry,
        {"keep_damage",
         "Makes what simulating broke part of the world, undoably: every broken building's pieces stay down "
         "where they are drawn (falling ones stop where they are), every felled tree stays down where it "
         "lies, and every dent becomes the ground itself. Kept damage is saved in each cell's file (and in "
         "scene files), streams with its cell, and shows whether simulating or not; stopping the "
         "simulation leaves it. A building moved, resized or given another kit afterwards stands whole "
         "again, and a tree placed anew (another model, size or place) stands. clear_damage takes it "
         "away. Returns `cells` (changed), `tiles` (dents kept), the totals kept (`kits`, `pieces`, "
         "`trees`) and get_physics.",
         Json{{"type", "object"}, {"additionalProperties", false}, {"properties", Json::object()}},
         false,
         [&state](const Json&, CommandContext& context) {
             if (state.physics == nullptr) return no_physics();
             auto kept = std::make_shared<PhysicsState::KeptDamage>(state.physics->keep_damage(state.scene));
             if (!kept->before.empty() || !kept->after.empty() || !kept->ground.empty()) {
                 const auto put = [&state, kept](bool after) {
                     std::unordered_set<world::CellCoord, world::CellCoordHash> cells;
                     for (const auto& [c, d] : kept->before) cells.insert(c);
                     for (const auto& [c, d] : kept->after) cells.insert(c);
                     const world::Damage& to = after ? kept->after : kept->before;
                     for (const world::CellCoord c : cells) {
                         const auto it = to.find(c);
                         // (Held when empty: the cell may have streamed out since, its file holding the other side.)
                         state.scene.set_damage(c, it != to.end() ? it->second : world::CellDamage{}, true);
                     }
                     for (const world::GroundDamage::Kept& g : kept->ground) {
                         state.scene.terrain.restore(g.cell, after ? g.after : g.before,
                                                     after ? g.after_version : g.before_version);
                     }
                 };
                 context.record(scene_change(state, [put] { put(false); }, [put] { put(true); }));
             }
             std::unordered_set<world::CellCoord, world::CellCoordHash> cells;
             for (const auto& [c, d] : kept->before) cells.insert(c);
             for (const auto& [c, d] : kept->after) cells.insert(c);
             Json out = physics_json(state);
             out["cells"] = cells.size();
             out["tiles"] = kept->ground.size();
             out["kits"] = kept->kits;
             out["pieces"] = kept->pieces;
             out["trees"] = kept->trees;
             return CommandResult::success(out);
         }});

    add(registry,
        {"clear_damage",
         "Takes away the kept damage (keep_damage) of every loaded cell, undoably: broken buildings stand "
         "whole and felled trees stand again (kept craters are the ground now: undo keep_damage, or "
         "sculpt, to take them away). What simulating breaks since is not touched. Returns `cells` "
         "(cleared) and get_physics.",
         Json{{"type", "object"}, {"additionalProperties", false}, {"properties", Json::object()}},
         false,
         [&state](const Json&, CommandContext& context) {
             if (state.physics == nullptr) return no_physics();
             world::Damage had = state.scene.damage();
             std::erase_if(had, [](const auto& d) { return d.second.empty(); });
             auto before = std::make_shared<const world::Damage>(std::move(had));
             for (const auto& [c, d] : *before) state.scene.set_damage(c, {}, true);
             if (!before->empty()) {
                 context.record(scene_change(
                     state,
                     [&state, before] {
                         for (const auto& [c, d] : *before) state.scene.set_damage(c, d, true);
                     },
                     [&state, before] {
                         for (const auto& [c, d] : *before) state.scene.set_damage(c, {}, true);
                     }));
             }
             Json out = physics_json(state);
             out["cells"] = before->size();
             return CommandResult::success(out);
         }});

    add(registry,
        {"dent_ground",
         "While simulating, presses a dent into the terrain at `point` (x and z; y is not used): a crater, "
         "a bowl `radius` metres across its middle and `depth` metres deep at the centre with a rim "
         "thrown up round it (`rim`: its height as a share of depth, out to 1.5 radius; default 0.25), or, "
         "with `stretch` over 1, a footprint `stretch` times as long as wide along `yaw` (degrees from "
         "+x toward +z); radius times stretch at most 200. Terrain edits while dented are made to the ground "
         "as authored (a tile edited loses its dents). The ground follows everywhere (drawing, bodies, scatter); a round crater throws "
         "up dust. View state: stopping the simulation puts the ground back exactly, and saving saves it "
         "as it was. Only tiles that exist are dented. Returns `tiles` (changed) and get_physics.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"point", "radius", "depth"}},
              {"properties",
               {{"point", triple(1e8, "World metres.")},
                {"radius", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 200}, {"description", "Metres."}}},
                {"depth", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 100}, {"description", "Metres."}}},
                {"rim", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
                {"stretch", {{"type", "number"}, {"minimum", 1}, {"maximum", 8}}},
                {"yaw", {{"type", "number"}, {"minimum", -360}, {"maximum", 360}, {"description", "Degrees."}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.physics == nullptr) return no_physics();
             if (!state.physics->simulating()) return CommandResult::failure("not simulating (simulate on first)");
             const Json& p = args["point"];
             world::Dent dent;
             dent.radius = args["radius"].get<double>();
             dent.depth = args["depth"].get<float>();
             dent.rim = args.value("rim", 0.25f);
             dent.stretch = args.value("stretch", 1.0f);
             dent.yaw = degrees_to_radians(args.value("yaw", 0.0f));
             if (dent.radius * static_cast<double>(dent.stretch) > 200.0) {
                 return CommandResult::failure("a dent reaches 200 m at most: radius times stretch");
             }
             const usize tiles =
                 state.physics->dent(state.scene, dent, {p[0].get<double>(), p[1].get<double>(), p[2].get<double>()});
             Json out = physics_json(state);
             out["tiles"] = tiles;
             return CommandResult::success(out);
         }});

    add(registry,
        {"play",
         "Starts or stops play mode: walking the scene as the player, a 1.8 m capsule that walks up steps "
         "(0.4 m), stops at walls, slides off slopes over 50 degrees, jumps (about 1.3 m), pushes dynamic "
         "entities and climbs walls it walks into (W up, S down, A/D along, onto the top; Space pushes off). "
         "Starting puts it on what is under the camera, looking where the camera looks, turns "
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
         "`sprint` (Shift: 8 m/s, else 4), `climb` (default true: walked into a wall too steep to walk it "
         "climbs, forward up, back down, right/left along it, at 2 m/s, and pulls itself up onto the top; "
         "false lets go), `jump` true jumps once (as soon as it is on the ground; on a wall it pushes off), "
         "`yaw` and "
         "`pitch` where it looks (degrees, as the camera's: yaw 0 looks down -z, 90 down -x). Leave out what "
         "stays. Time moves with the frames, or with step_player. Returns get_player's report.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"properties",
               {{"forward", {{"type", "number"}, {"minimum", -1}, {"maximum", 1}}},
                {"right", {{"type", "number"}, {"minimum", -1}, {"maximum", 1}}},
                {"sprint", {{"type", "boolean"}}},
                {"climb", {{"type", "boolean"}}},
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
             in.climb = args.value("climb", in.climb);
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
        {"carry_player",
         "While playing, `entity` (one with a body: a crate, a platform; later a giant's hand) carries the "
         "player: held where it is against it, it goes wherever the entity goes (simulated, or moved), its "
         "input left aside, until `release` true lets it go (it falls on as it was carried) or the body "
         "goes (the entity removed, or changed so its body is made again). View state, not undoable. "
         "Returns get_player's report.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"properties",
               {{"entity", {{"type", "integer"}, {"minimum", 1}}}, {"release", {{"type", "boolean"}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.physics == nullptr || state.play == nullptr) return no_play();
             if (!state.play->playing()) return not_playing();
             const bool release = args.value("release", false);
             if (release == args.contains("entity")) {
                 return CommandResult::failure("give `entity` to carry the player, or `release` true");
             }
             if (release) {
                 state.play->release();
             } else {
                 state.physics->flush(state.scene); // its body in line with the scene
                 const world::EntityId entity = args["entity"].get<world::EntityId>();
                 if (state.play->placing()) {
                     return CommandResult::failure("the player is still being put down (step until "
                                                   "waiting_for_ground is false)");
                 }
                 if (!state.play->carry(entity)) {
                     return CommandResult::failure("entity " + std::to_string(entity) + " has no body to carry the player");
                 }
             }
             return CommandResult::success(player_json(state));
         }});

    add(registry,
        {"get_player",
         "Reports play mode: whether playing and, while it is, the player's feet (`position`), `eye` (the "
         "camera), `velocity`, `on_ground`, `sliding` (on a slope too steep), `standing_on` (an entity's id, "
         "0 the ground, null nothing), `climbing` (on a wall: `wall` its entity, 0 something else), "
         "`mantling` (pulling itself up onto a ledge), `carried_by` (carry_player: the entity, 0 a body that "
         "is no entity's (or one just removed), null none), "
         "`yaw`, `pitch`, its `input`, and `waiting_for_ground` (held still while the ground under it is "
         "still being made).",
         Json{{"type", "object"}, {"additionalProperties", false}, {"properties", Json::object()}},
         true,
         [&state](const Json&, CommandContext&) {
             if (state.physics == nullptr || state.play == nullptr) return no_play();
             return CommandResult::success(player_json(state));
         }});
}

} // namespace giant::editor
