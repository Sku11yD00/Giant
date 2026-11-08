// Physics commands: simulating the scene's bodies (a play button: view
// state, not undoable), walking it as the player (play mode) and asking the
// physics world what is where.
#include "editor_commands.h"
#include "physics_state.h"
#include "hit_targets.h"
#include "mesh_library.h"
#include "play_state.h"

#include "giant/core/assert.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
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

Json shot_hit_json(const EditorState& state, const PhysicsState::ShotHit& s) {
    const sim::BulletHit& h = s.hit;
    Json out{{"bullet", h.bullet},
             {"outcome", sim::to_string(h.outcome)},
             {"entity", s.entity},
             {"surface", world::to_string(h.surface)},
             {"point", vec(h.point)},
             {"normal", vec(h.normal)},
             {"speed_in", h.speed_in},
             {"speed_out", h.speed_out},
             {"energy", h.energy},
             {"thickness", h.thickness},
             {"distance", h.distance}};
    if (s.piece) out["piece"] = *s.piece;
    if (s.tree) out["tree"] = true;
    if (s.character) out["character"] = true;
    if (h.character) {
        // A skinned character's bone capsule: which bone, in which zone.
        const std::string zone = state.hits != nullptr ? state.hits->zone_name(s.entity, h.zone) : std::string{};
        out["zone"] = zone.empty() ? Json(nullptr) : Json(zone);
        out["bone"] = state.hits != nullptr ? state.hits->bone_name(s.entity, h.bone) : std::string{};
    }
    if (h.outcome != sim::HitOutcome::stopped) out["exit"] = vec(h.exit);
    if (s.loosened > 0) out["loosened"] = s.loosened;
    return out;
}

// Characters' hit shapes brought in line with the scene now (a command's
// answer: an entity just made or moved is met where it is).
void catch_up_hits(EditorState& state) {
    if (state.hits == nullptr || state.animation == nullptr || state.meshes == nullptr) return;
    state.animation->follow_now(state.scene, *state.meshes, state.scene.untaken_changes());
    state.hits->update(state.scene, *state.meshes, *state.animation);
}

Json zone_map_json(const sim::HitZoneMap& map) {
    Json out = Json::array();
    for (const sim::HitZoneRule& r : map.rules) out.push_back({{"zone", r.zone}, {"match", r.match}});
    return out;
}

Json surfaces_json(const sim::BallisticsSettings& settings) {
    Json out = Json::object();
    for (usize i = 0; i < world::k_surface_count; ++i) {
        const sim::SurfaceBallistics& b = settings.surfaces[i];
        out[world::to_string(static_cast<world::Surface>(i))] = {
            {"resistance", b.resistance}, {"ricochet_degrees", b.ricochet_degrees}, {"ricochet_keep", b.ricochet_keep}};
    }
    return out;
}

Json ballistics_json(const EditorState& state, usize hits) {
    const PhysicsState& p = *state.physics;
    const sim::Ballistics& b = p.ballistics();
    const sim::BallisticsStats s = b.stats();
    Json flying = Json::array();
    for (const sim::BulletState& bullet : b.bullets()) {
        if (flying.size() >= 64) break;
        flying.push_back({{"bullet", bullet.id},
                          {"position", vec(bullet.position)},
                          {"velocity", vec(bullet.velocity)},
                          {"distance", bullet.distance},
                          {"seconds", bullet.seconds}});
    }
    Json recent = Json::array();
    const auto& kept = p.recent_hits();
    for (usize i = kept.size() - std::min(hits, kept.size()); i < kept.size(); ++i) recent.push_back(shot_hit_json(state, kept[i]));
    return {{"simulating", p.simulating()},
            {"flying", s.flying},
            {"fired", s.fired},
            {"refused", s.refused},
            {"hits", s.hits},
            {"rays", s.rays},
            {"last_step_ms", s.last_step_ms},
            {"bullets", std::move(flying)},
            {"recent_hits", std::move(recent)},
            {"drag", b.settings().drag},
            {"blow_per_joule", p.blow_per_joule},
            {"surfaces", surfaces_json(b.settings())}};
}

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
                      {"distance", hit->distance},
                      {"surface", world::to_string(state.physics->surface_of(*hit))}};
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
        {"fire",
         "Fires a bullet from `origin` along `direction` at `speed` m/s (default 880, a rifle; a pistol is "
         "about 360) weighing `mass` kg (default 0.008). It falls and slows as it flies (drag), and what it "
         "meets is made of a surface (set_entity's `surface`: wood, metal, concrete...; the ground is dirt, "
         "trees wood, characters flesh): it goes through what its energy takes it through (each surface "
         "takes so many joules a metre: get_ballistics; `penetration` 2 goes through twice as much, armour "
         "piercing), glances off hard surfaces struck at a shallow angle, or stops (the terrain always stops it). What it meets is pushed "
         "(dynamic bodies), and a breakable kit's piece shot enough (the energy left in it, times "
         "blow_per_joule, adding up to the kit's strength) is shot out with what it held up. While playing, "
         "the player is not hit by its own shots. Needs simulating: the bullet flies with the steps (its "
         "hits show in get_ballistics); `resolve` flies it to its end now and returns its `hits` in order "
         "(each: outcome stopped, passed or ricochet; entity, piece, surface, point, speeds in and out, the "
         "energy left there, thickness gone through, distance flown). Not simulating, `resolve` only "
         "reports (nothing is pushed or broken).",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"origin", "direction"}},
              {"properties",
               {{"origin", triple(1e8, "World metres.")},
                {"direction", triple(1e6, "Need not be unit length; not zero.")},
                {"speed", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 5000}, {"description", "m/s. Default 880."}}},
                {"mass", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 100}, {"description", "Kilograms. Default 0.008."}}},
                {"penetration",
                 {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 100}, {"description", "Default 1."}}},
                {"resolve", {{"type", "boolean"}, {"description", "Fly it to its end now. Default false."}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.physics == nullptr) return no_physics();
             const bool resolve = args.value("resolve", false);
             if (!resolve && !state.physics->simulating()) {
                 return CommandResult::failure("not simulating (simulate on first, or resolve the shot)");
             }
             const Json& o = args["origin"];
             const Json& d = args["direction"];
             const double dx = d[0].get<double>();
             const double dy = d[1].get<double>();
             const double dz = d[2].get<double>();
             const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
             if (!(length > 1e-9)) return CommandResult::failure("direction is zero");
             const double speed = args.value("speed", 880.0);
             const float mass = args.value("mass", 0.008f);
             const float penetration = args.value("penetration", 1.0f);
             if (!(static_cast<float>(speed) >= 1e-3f) || !(mass >= 1e-9f) || !(penetration >= 1e-6f)) {
                 return CommandResult::failure("speed, mass and penetration must be above 0");
             }
             sim::Shot shot;
             shot.origin = {o[0].get<double>(), o[1].get<double>(), o[2].get<double>()};
             shot.velocity = {static_cast<float>(dx / length * speed), static_cast<float>(dy / length * speed),
                              static_cast<float>(dz / length * speed)};
             shot.mass = mass;
             shot.penetration = penetration;
             if (state.play != nullptr && state.play->playing()) shot.ignore = state.play->player_body();
             if (resolve) {
                 // The world exactly as it is now (bodies still waiting made at once).
                 state.physics->flush(state.scene);
                 catch_up_hits(state);
                 Json hits = Json::array();
                 for (const PhysicsState::ShotHit& h : state.physics->resolve(shot)) hits.push_back(shot_hit_json(state, h));
                 return CommandResult::success(Json{{"hits", std::move(hits)}, {"simulating", state.physics->simulating()}});
             }
             const sim::BulletId id = state.physics->fire(shot);
             if (id == sim::k_no_bullet) return CommandResult::failure("too many bullets flying");
             return CommandResult::success(Json{{"bullet", id}});
         }});

    add(registry,
        {"get_ballistics",
         "Reports the bullets: how many are flying (up to 64 listed: position, velocity, distance flown, "
         "seconds), fired and refused in all, `hits` (the count, in all), the latest `recent_hits` (`hits` "
         "argument: how many, default 16, at most 256; as fire's), the "
         "drag, blow_per_joule and each surface's `resistance` (joules a metre), `ricochet_degrees` (glances "
         "off under this angle to the surface; 0 never) and `ricochet_keep` (the share of speed kept).",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"properties",
               {{"hits", {{"type", "integer"}, {"minimum", 0}, {"maximum", 256}, {"description", "Default 16."}}}}}},
         true,
         [&state](const Json& args, CommandContext&) {
             if (state.physics == nullptr) return no_physics();
             return CommandResult::success(ballistics_json(state, args.value("hits", 16u)));
         }});

    add(registry,
        {"set_ballistics",
         "Tunes the bullets (view state, as the simulation: not saved or undoable; the editor starts with "
         "the defaults): `drag` (1/m, slowing of drag times speed squared; default 0.0007), `blow_per_joule` "
         "(N s of wear on a kit piece each joule left in it; default 0.1: a 3000 J rifle round stopped in a piece wears it by 300 N s, a fifth of the default strength) and `surfaces`, by name, each with "
         "any of resistance (joules a metre, > 0), ricochet_degrees (0 to 90) and ricochet_keep (0 to 1). "
         "Returns get_ballistics (with no recent hits).",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"properties",
               {{"drag", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
                {"blow_per_joule", {{"type", "number"}, {"minimum", 0}, {"maximum", 1e6}}},
                {"surfaces",
                 {{"type", "object"},
                  {"additionalProperties",
                   {{"type", "object"},
                    {"additionalProperties", false},
                    {"properties",
                     {{"resistance", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1e9}}},
                      {"ricochet_degrees", {{"type", "number"}, {"minimum", 0}, {"maximum", 90}}},
                      {"ricochet_keep", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}}}}}}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.physics == nullptr) return no_physics();
             sim::BallisticsSettings settings = state.physics->ballistics().settings();
             if (const auto it = args.find("surfaces"); it != args.end()) {
                 for (const auto& [name, values] : it->items()) {
                     const std::optional<world::Surface> surface = world::surface_from_string(name);
                     if (!surface) return CommandResult::failure("surfaces." + name + ": no such surface");
                     if (!values.is_object()) return CommandResult::failure("surfaces." + name + ": must be an object");
                     sim::SurfaceBallistics& b = settings.surfaces[static_cast<usize>(*surface)];
                     for (const auto& [key, value] : values.items()) {
                         const std::string at = "surfaces." + name + "." + key;
                         float* field = key == "resistance"         ? &b.resistance
                                        : key == "ricochet_degrees" ? &b.ricochet_degrees
                                        : key == "ricochet_keep"    ? &b.ricochet_keep
                                                                    : nullptr;
                         if (field == nullptr) return CommandResult::failure(at + ": is not a known argument");
                         if (!value.is_number()) return CommandResult::failure(at + ": must be a number");
                         const double v = value.get<double>();
                         const double hi = key == "resistance" ? 1e9 : key == "ricochet_degrees" ? 90.0 : 1.0;
                         if (!(v >= 0.0 && v <= hi) || (key == "resistance" && !(v > 0.0))) {
                             return CommandResult::failure(at + ": out of range");
                         }
                         *field = static_cast<float>(v);
                     }
                 }
             }
             settings.drag = args.value("drag", settings.drag);
             state.physics->ballistics().settings() = settings;
             state.physics->blow_per_joule = args.value("blow_per_joule", state.physics->blow_per_joule);
             return CommandResult::success(ballistics_json(state, 0));
         }});

    add(registry,
        {"get_hit_zones",
         "What bullets meet on a skinned character (an entity drawing a skinned glTF file): a capsule "
         "round each bone, fitted to the vertices the bone moves most when the file is imported, posed as "
         "the entity is drawn (its animation, or the rest pose) and named by zone from its file's zone map "
         "(the first rule with a word the bone's name holds, ignoring case; default rules: head, neck, arm, "
         "leg, torso for the usual rigs). Returns `zones` (the map), `default` (whether it is the default) "
         "and `capsules` (bone, zone or null, a, b, radius in world metres). A shot (fire) reports the "
         "`zone` and `bone` it met; capsules are flesh.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"id"}},
              {"properties", {{"id", {{"type", "integer"}, {"minimum", 1}}}}}},
        false, // brings the animation in line first, as get_animation
         [&state](const Json& args, CommandContext&) {
             if (state.hits == nullptr) return CommandResult::failure("hit shapes are not available");
             const world::EntityId id = args["id"].get<world::EntityId>();
             const world::Entity* e = state.scene.find(id);
             if (e == nullptr) return CommandResult::failure("no entity " + std::to_string(id));
             catch_up_hits(state);
             const sim::HitCharacter* c = state.hits->shapes().find(id);
             if (c == nullptr) {
                 return CommandResult::failure("entity " + std::to_string(id) +
                                               " has no hit shapes (not a visible skinned mesh, or not loaded yet)");
             }
             Json capsules = Json::array();
             for (const sim::HitCapsule& h : c->capsules) {
                 const std::string zone = state.hits->zone_name(id, h.zone);
                 capsules.push_back({{"bone", state.hits->bone_name(id, h.bone)},
                                     {"zone", zone.empty() ? Json(nullptr) : Json(zone)},
                                     {"a", vec(h.a)},
                                     {"b", vec(h.b)},
                                     {"radius", h.radius}});
             }
             const sim::HitZoneMap& map = state.hits->zones(e->mesh);
             return CommandResult::success(Json{{"id", id},
                                                {"zones", zone_map_json(map)},
                                                {"default", map == sim::default_hit_zones()},
                                                {"capsules", std::move(capsules)}});
         }});

    add(registry,
        {"set_hit_zones",
         "Names the hit zones of a skinned file's characters (every entity drawing `mesh`): `zones` is a "
         "list of rules in order, each a `zone` name and the words (`match`) a bone's name may hold, "
         "ignoring case; the first rule that matches names the bone, and a bone no rule matches is in no "
         "zone. `reset` true goes back to the default rules. View state: not saved or undoable (the "
         "editor starts with the defaults). Returns the map.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"mesh"}},
              {"properties",
               {{"mesh", {{"type", "string"}, {"minLength", 1}, {"maxLength", world::k_max_mesh_path_bytes}}},
                {"reset", {{"type", "boolean"}}},
                {"zones",
                 {{"type", "array"},
                  {"maxItems", 256},
                  {"items",
                   {{"type", "object"},
                    {"additionalProperties", false},
                    {"required", {"zone", "match"}},
                    {"properties",
                     {{"zone", {{"type", "string"}, {"minLength", 1}, {"maxLength", world::k_max_name_bytes}}},
                      {"match",
                       {{"type", "array"},
                        {"minItems", 1},
                        {"maxItems", 64},
                        {"items", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}}}}}}}}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.hits == nullptr) return CommandResult::failure("hit shapes are not available");
             const std::string mesh = MeshLibrary::normalize(args["mesh"].get<std::string>());
             if (args.value("reset", false)) {
                 if (args.contains("zones")) return CommandResult::failure("give zones or reset, not both");
                 state.hits->reset_zones(mesh);
             } else {
                 if (!args.contains("zones")) return CommandResult::failure("zones (or reset) is needed");
                 sim::HitZoneMap map;
                 for (const Json& r : args["zones"]) {
                     sim::HitZoneRule rule;
                     rule.zone = r["zone"].get<std::string>();
                     for (const Json& w : r["match"]) rule.match.push_back(w.get<std::string>());
                     map.rules.push_back(std::move(rule));
                 }
                 state.hits->set_zones(mesh, std::move(map));
             }
             return CommandResult::success(Json{{"mesh", mesh}, {"zones", zone_map_json(state.hits->zones(mesh))}});
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
