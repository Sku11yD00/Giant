// Effects commands: particle looks and effects (world/effects.h). Scene data
// (saved with the scene and the world's world.json, undoable).
#include "editor_commands.h"
#include "world_streaming.h"

#include "giant/core/assert.h"
#include "giant/world/effects.h"

#include <algorithm>
#include <string>

namespace giant::editor {

namespace {

using ai::CommandContext;
using ai::CommandResult;
using ai::Json;
using world::Effect;
using world::Emitter;
using world::ParticleLook;
using Looks = std::vector<ParticleLook>;
using Effects = std::vector<Effect>;

Json number(double minimum, double maximum, const char* description) {
    return {{"type", "number"}, {"minimum", minimum}, {"maximum", maximum}, {"description", description}};
}
Json triple(double minimum, double maximum, const char* description) {
    return {{"type", "array"},
            {"items", {{"type", "number"}, {"minimum", minimum}, {"maximum", maximum}}},
            {"minItems", 3},
            {"maxItems", 3},
            {"description", description}};
}
Json name_schema() {
    return {{"type", "string"}, {"minLength", 1}, {"maxLength", world::k_max_effect_name_bytes}};
}
Json vec3_json(const Vec3& v) { return Json::array({v.x, v.y, v.z}); }
Vec3 vec(const Json& v) { return {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()}; }

constexpr const char* k_built_ins = "smoke, fire, sparks, dust_kick, spray, mist, embers, debris_chips";

Json look_properties() {
    const double pull = world::k_max_look_pull, size = world::k_max_look_size;
    return {
        {"name", name_schema()},
        {"life_min", number(0, world::k_max_look_life, "Seconds each lives, at least (over 0; default 1).")},
        {"life_max", number(0, world::k_max_look_life, "Seconds each lives, at most (default 2).")},
        {"size_start", number(0, size, "Metres across at birth (default 0.2).")},
        {"size_end", number(0, size, "Metres across at the end, linear between (default 0.6).")},
        {"color_start", triple(0, 1, "What it gives back of the light at birth, linear RGB (default grey 0.5).")},
        {"color_end", triple(0, 1, "The same at the end (default grey 0.5).")},
        {"opacity_start", number(0, 1, "At birth, eased in over its first tenth (default 1).")},
        {"opacity_end", number(0, 1, "At the end (default 0: fades out).")},
        {"emissive", triple(0, world::k_max_look_light, "Light of its own (fire, sparks, embers), linear RGB; "
                                                        "fades with its opacity (default none).")},
        {"emissive_end", number(0, 1, "The share of emissive left at the end (default 1).")},
        {"gravity", number(-pull, pull, "The share of gravity pulling it down (default 1; 0 none).")},
        {"buoyancy", number(-world::k_max_look_buoyancy, world::k_max_look_buoyancy, "Metres a second squared up (smoke ~0.6, fire ~3; default 0).")},
        {"drag", number(0, world::k_max_look_drag, "1/s: how fast it takes the speed of the air (default 0: "
                                                   "ballistic, the wind does not move it).")},
        {"wind", number(-pull, pull, "The share of the wind that air moves with (with drag; default 1).")},
        {"wobble", number(0, size, "Metres it wanders off its path, slowly (smoke curling; default 0).")},
        {"form", {{"enum", {"puff", "streak"}},
                  {"description", "puff: a soft round blot (smoke, dust, spray, fire; the default); streak: "
                                  "stretched along its motion (sparks, chips)."}}},
        {"lit", {{"type", "boolean"}, {"description", "Lit by sun, sky and lamps (default true; false: its "
                                                      "emissive only, for fire and sparks)."}}},
        {"soft", number(0, size, "Metres over which it fades into what it meets (default 0.3).")},
    };
}

Json look_json(const ParticleLook& l) {
    return {{"name", l.name},
            {"life_min", l.life_min},
            {"life_max", l.life_max},
            {"size_start", l.size_start},
            {"size_end", l.size_end},
            {"color_start", vec3_json(l.color_start)},
            {"color_end", vec3_json(l.color_end)},
            {"opacity_start", l.opacity_start},
            {"opacity_end", l.opacity_end},
            {"emissive", vec3_json(l.emissive)},
            {"emissive_end", l.emissive_end},
            {"gravity", l.gravity},
            {"buoyancy", l.buoyancy},
            {"drag", l.drag},
            {"wind", l.wind},
            {"wobble", l.wobble},
            {"form", world::to_string(l.form)},
            {"lit", l.lit},
            {"soft", l.soft}};
}

void apply_look(const Json& a, ParticleLook& l) {
    if (a.contains("name")) l.name = a["name"].get<std::string>();
    l.life_min = a.value("life_min", l.life_min);
    l.life_max = a.value("life_max", l.life_max);
    l.size_start = a.value("size_start", l.size_start);
    l.size_end = a.value("size_end", l.size_end);
    if (a.contains("color_start")) l.color_start = vec(a["color_start"]);
    if (a.contains("color_end")) l.color_end = vec(a["color_end"]);
    l.opacity_start = a.value("opacity_start", l.opacity_start);
    l.opacity_end = a.value("opacity_end", l.opacity_end);
    if (a.contains("emissive")) l.emissive = vec(a["emissive"]);
    l.emissive_end = a.value("emissive_end", l.emissive_end);
    l.gravity = a.value("gravity", l.gravity);
    l.buoyancy = a.value("buoyancy", l.buoyancy);
    l.drag = a.value("drag", l.drag);
    l.wind = a.value("wind", l.wind);
    l.wobble = a.value("wobble", l.wobble);
    if (a.contains("form")) l.form = world::particle_form_from_string(a["form"].get<std::string>()).value_or(l.form);
    l.lit = a.value("lit", l.lit);
    l.soft = a.value("soft", l.soft);
}

Json emitter_schema() {
    const double reach = world::k_max_emitter_reach, speed = world::k_max_emitter_speed;
    return {{"type", "object"},
            {"additionalProperties", false},
            {"required", {"look"}},
            {"properties",
             {{"look", {{"type", "string"},
                        {"minLength", 1},
                        {"maxLength", world::k_max_effect_name_bytes},
                        {"description", "The particle look, by name: the scene's (set_particle_look), else a "
                                        "built-in one of the same names as the built-in effects."}}},
              {"rate", number(0, world::k_max_emitter_rate, "Particles a second while it runs (default 10).")},
              {"burst", {{"type", "integer"},
                         {"minimum", 0},
                         {"maximum", world::k_max_emitter_burst},
                         {"description", "Particles at once when it is spawned (default 0)."}}},
              {"shape", {{"enum", {"point", "sphere", "box", "disc"}},
                         {"description", "Where they start: at the point (the default), inside a sphere "
                                         "(size[0] its radius), inside a box (size its half extents) or on a flat "
                                         "disc across the effect's own x and z (size[0] its radius)."}}},
              {"size", triple(0, reach, "Metres (see shape; default [0, 0, 0]).")},
              {"offset", triple(-reach, reach, "Metres from the effect's origin, in its own space (default 0).")},
              {"direction", triple(-1e6, 1e6, "Which way they go, in the effect's own space; not zero (default "
                                              "up [0, 1, 0]).")},
              {"cone_degrees", number(0, 180, "Within this of direction (180: any way; default 30).")},
              {"speed_min", number(0, speed, "Metres a second, at least (default 1).")},
              {"speed_max", number(0, speed, "Metres a second, at most (default 2).")},
              {"size_scale", number(0, 1000, "Times the look's sizes, over 0 (default 1).")},
              {"ground", {{"type", "boolean"},
                          {"description", "They stop where they fall to the ground (default true)."}}}}},
            {"description", "One stream of particles of one look."}};
}

Json emitter_json(const Emitter& e) {
    return {{"look", e.look},
            {"rate", e.rate},
            {"burst", e.burst},
            {"shape", world::to_string(e.shape)},
            {"size", vec3_json(e.size)},
            {"offset", vec3_json(e.offset)},
            {"direction", vec3_json(e.direction)},
            {"cone_degrees", e.cone_degrees},
            {"speed_min", e.speed_min},
            {"speed_max", e.speed_max},
            {"size_scale", e.size_scale},
            {"ground", e.ground}};
}

Emitter emitter_from(const Json& a) {
    Emitter e;
    e.look = a["look"].get<std::string>();
    e.rate = a.value("rate", e.rate);
    e.burst = a.value("burst", e.burst);
    if (a.contains("shape")) e.shape = world::emitter_shape_from_string(a["shape"].get<std::string>()).value_or(e.shape);
    if (a.contains("size")) e.size = vec(a["size"]);
    if (a.contains("offset")) e.offset = vec(a["offset"]);
    if (a.contains("direction")) e.direction = vec(a["direction"]);
    e.cone_degrees = a.value("cone_degrees", e.cone_degrees);
    e.speed_min = a.value("speed_min", e.speed_min);
    e.speed_max = a.value("speed_max", e.speed_max);
    e.size_scale = a.value("size_scale", e.size_scale);
    e.ground = a.value("ground", e.ground);
    return e;
}

Json effect_properties() {
    return {{"name", name_schema()},
            {"emitters", {{"type", "array"},
                          {"minItems", 1},
                          {"maxItems", world::k_max_emitters},
                          {"items", emitter_schema()},
                          {"description", "Its streams of particles (set_effect: replaces the list)."}}},
            {"duration", number(0, world::k_max_effect_seconds,
                                "Spawned: seconds it runs its rates after its bursts (default 0: bursts only). "
                                "On an entity it runs while the entity's effect is on.")}};
}

// The effect's looks found nowhere (the scene's own, else built-in).
Json missing_looks(const Effect& f, const Looks& looks) {
    Json missing = Json::array();
    for (const Emitter& e : f.emitters) {
        if (world::find_particle_look(looks, e.look) == nullptr &&
            std::find(missing.begin(), missing.end(), Json(e.look)) == missing.end()) {
            missing.push_back(e.look);
        }
    }
    return missing;
}

Json effect_json(const Effect& f, const Looks& looks) {
    Json emitters = Json::array();
    for (const Emitter& e : f.emitters) emitters.push_back(emitter_json(e));
    Json j = {{"name", f.name}, {"duration", f.duration}, {"emitters", std::move(emitters)}};
    if (Json missing = missing_looks(f, looks); !missing.empty()) j["missing_looks"] = std::move(missing);
    return j;
}

// Sets the fields given (emitters replace the list).
std::string apply_effect(const Json& a, Effect& f) {
    if (a.contains("name")) f.name = a["name"].get<std::string>();
    f.duration = a.value("duration", f.duration);
    if (a.contains("emitters")) {
        f.emitters.clear();
        for (const Json& e : a["emitters"]) f.emitters.push_back(emitter_from(e));
    }
    if (const auto bad = world::check_effect(f)) return *bad;
    return {};
}

template <typename List>
auto find(List& list, const std::string& name) -> decltype(&*list.begin()) {
    const auto it = std::find_if(list.begin(), list.end(), [&](const auto& t) { return t.name == name; });
    return it == list.end() ? nullptr : &*it;
}

bool built_in_look(std::string_view name) {
    const auto looks = world::built_in_particle_looks();
    return std::any_of(looks.begin(), looks.end(), [&](const ParticleLook& l) { return l.name == name; });
}
bool built_in_effect(std::string_view name) {
    const auto effects = world::built_in_effects();
    return std::any_of(effects.begin(), effects.end(), [&](const Effect& f) { return f.name == name; });
}

// Records a change to the looks and the effects (both, so one undo puts back a rename's whole change).
void record(EditorState& state, CommandContext& context, Looks looks_before, Effects effects_before) {
    Looks looks_after = state.scene.particle_looks;
    Effects effects_after = state.scene.effects;
    context.record(scene_change(
        state,
        [&state, l = std::move(looks_before), f = std::move(effects_before)] {
            state.scene.particle_looks = l;
            state.scene.effects = f;
        },
        [&state, l = std::move(looks_after), f = std::move(effects_after)] {
            state.scene.particle_looks = l;
            state.scene.effects = f;
        }));
}

// The entities naming the effect (by id), on or off.
Json runners(const EditorState& state, const std::string& effect) {
    Json ids = Json::array();
    for (const world::Entity& e : state.scene.entities()) {
        if (e.effect.effect == effect) ids.push_back(e.id);
    }
    return ids;
}

void add(ai::CommandRegistry& registry, ai::CommandDesc desc) {
    const bool added = registry.add(std::move(desc));
    GIANT_ASSERT(added, "effects command could not be registered");
}

} // namespace

void register_effects_commands(ai::CommandRegistry& registry, EditorState& state) {
    Json look_props = look_properties();
    look_props["new_name"] = name_schema();
    add(registry,
        {"set_particle_look",
         std::string("Makes or changes a particle look (by name): how particles of it look and move (life, size, "
                     "colour, opacity, emissive glow; gravity, buoyancy, drag and the wind; puff or streak). Only "
                     "the fields given change; a new look starts from the built-in look of its name if there is "
                     "one (") + k_built_ins + "), else from the defaults, and stands in for the built-in one. "
                     "new_name renames it (effects naming the old name are renamed with it; a look not yet the scene's is "
                     "made under the new name, effects untouched). Saved with the "
                     "scene or the world. Undoable.",
         Json{{"type", "object"}, {"additionalProperties", false}, {"required", {"name"}}, {"properties", look_props}},
         false,
         [&state](const Json& args, CommandContext& context) {
             Looks& looks = state.scene.particle_looks;
             const std::string name = args["name"].get<std::string>();
             ParticleLook* existing = find(looks, name);
             if (existing == nullptr && looks.size() >= world::k_max_particle_looks) {
                 return CommandResult::failure("a scene has at most " + std::to_string(world::k_max_particle_looks) +
                                               " particle looks of its own");
             }
             ParticleLook changed;
             if (existing != nullptr) {
                 changed = *existing;
             } else if (const ParticleLook* base = world::find_particle_look({}, name)) {
                 changed = *base;
             } else {
                 changed.name = name;
             }
             Json fields = args;
             fields.erase("name");
             std::string renamed = name;
             if (args.contains("new_name")) {
                 renamed = args["new_name"].get<std::string>();
                 if (renamed != name && find(looks, renamed) != nullptr) {
                     return CommandResult::failure("there is a particle look named \"" + renamed + "\" already");
                 }
                 fields["name"] = renamed;
                 fields.erase("new_name");
             }
             apply_look(fields, changed);
             if (const auto bad = world::check_particle_look(changed)) return CommandResult::failure(*bad);
             Looks looks_before = looks;
             Effects effects_before = state.scene.effects;
             if (existing != nullptr) {
                 *existing = changed;
             } else {
                 looks.push_back(changed);
             }
             if (renamed != name && existing != nullptr) {
                 for (Effect& f : state.scene.effects) {
                     for (Emitter& e : f.emitters) {
                         if (e.look == name) e.look = renamed;
                     }
                 }
             }
             record(state, context, std::move(looks_before), std::move(effects_before));
             Json out = look_json(changed);
             out["created"] = existing == nullptr;
             return CommandResult::success(std::move(out));
         }});

    add(registry,
        {"remove_particle_look",
         "Removes one of the scene's particle looks (by name). Effects naming it then use the built-in look of "
         "that name if there is one; one that would be left with no look at all is refused (change those "
         "effects first). Undoable.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"name"}},
              {"properties", {{"name", name_schema()}}}},
         false,
         [&state](const Json& args, CommandContext& context) {
             Looks& looks = state.scene.particle_looks;
             const std::string name = args["name"].get<std::string>();
             if (find(looks, name) == nullptr) {
                 return CommandResult::failure("the scene has no particle look named \"" + name + "\"" +
                                               (built_in_look(name) ? " (built-in looks cannot be removed)" : ""));
             }
             if (!built_in_look(name)) {
                 for (const Effect& f : state.scene.effects) {
                     for (const Emitter& e : f.emitters) {
                         if (e.look == name) {
                             return CommandResult::failure("effect \"" + f.name + "\" uses particle look \"" + name +
                                                           "\": change it first (set_effect)");
                         }
                     }
                 }
             }
             Looks looks_before = looks;
             std::erase_if(looks, [&](const ParticleLook& l) { return l.name == name; });
             record(state, context, std::move(looks_before), state.scene.effects);
             return CommandResult::success({{"removed", name}});
         }});

    add(registry,
        {"create_effect",
         std::string("Adds an effect: a named set of emitters, each a stream of particles of one look (rate a "
                     "second while it runs, a burst at once when spawned; from a point, sphere, box or disc; "
                     "within a cone of a direction at a speed). An entity runs one where it stands while its "
                     "effect is on (set_entity effect: chimney smoke, a torch's fire). A name of a built-in "
                     "effect (") + k_built_ins + ") stands in for it. Saved with the scene or the world. Undoable.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"name", "emitters"}},
              {"properties", effect_properties()}},
         false,
         [&state](const Json& args, CommandContext& context) {
             Effects& effects = state.scene.effects;
             if (effects.size() >= world::k_max_effects) {
                 return CommandResult::failure("a scene has at most " + std::to_string(world::k_max_effects) +
                                               " effects");
             }
             const std::string name = args["name"].get<std::string>();
             if (find(effects, name) != nullptr) {
                 return CommandResult::failure("there is an effect named \"" + name + "\" already: set_effect changes it");
             }
             Effect f;
             if (std::string bad = apply_effect(args, f); !bad.empty()) return CommandResult::failure(bad);
             Effects before = effects;
             effects.push_back(f);
             record(state, context, state.scene.particle_looks, std::move(before));
             return CommandResult::success(effect_json(f, state.scene.particle_looks));
         }});

    Json set_props = effect_properties();
    set_props["new_name"] = name_schema();
    add(registry,
        {"set_effect",
         "Changes one of the scene's effects (by name): only the fields given (emitters replace the list; "
         "new_name renames it, and the entities running it with it: in an open world, only once every cell "
         "is loaded). Undoable.",
         Json{{"type", "object"}, {"additionalProperties", false}, {"required", {"name"}}, {"properties", set_props}},
         false,
         [&state](const Json& args, CommandContext& context) {
             Effects& effects = state.scene.effects;
             const std::string name = args["name"].get<std::string>();
             Effect* f = find(effects, name);
             if (f == nullptr) {
                 return CommandResult::failure("the scene has no effect named \"" + name + "\"" +
                                               (built_in_effect(name) ? " (create_effect makes one standing in for "
                                                                        "the built-in one)"
                                                                      : ""));
             }
             Effect changed = *f;
             Json fields = args;
             fields.erase("name");
             std::string renamed = name;
             if (args.contains("new_name")) {
                 renamed = args["new_name"].get<std::string>();
                 if (renamed != name && find(effects, renamed) != nullptr) {
                     return CommandResult::failure("there is an effect named \"" + renamed + "\" already");
                 }
                 fields["name"] = renamed;
                 fields.erase("new_name");
             }
             if (std::string bad = apply_effect(fields, changed); !bad.empty()) return CommandResult::failure(bad);
             if (renamed == name) {
                 Effects before = effects;
                 *f = changed;
                 record(state, context, state.scene.particle_looks, std::move(before));
                 return CommandResult::success(effect_json(changed, state.scene.particle_looks));
             }
             // Renamed: the entities running it follow, in the same undo (all of them: every cell loaded).
             if (state.world != nullptr && state.world->active(state.scene_generation) && !state.world->all_loaded()) {
                 return CommandResult::failure("cells of the open world not loaded now may run \"" + name +
                                               "\": load them all (set_world_streaming load_radius) first");
             }
             std::vector<world::EntityId> ids;
             for (const world::Entity& e : state.scene.entities()) {
                 if (e.effect.effect == name) ids.push_back(e.id);
             }
             Effects before = effects;
             *f = changed;
             Effects after = effects;
             const auto rename = [&state, ids](const std::string& to) {
                 for (const world::EntityId id : ids) {
                     if (world::Entity* e = state.scene.modify(id)) e->effect.effect = to;
                 }
             };
             // An undo or redo with some of them streamed out since changes nothing (the effect and the
             // entities naming it must not fall out of step).
             const auto all_here = [&state, ids] {
                 return std::all_of(ids.begin(), ids.end(), [&](world::EntityId id) { return state.scene.find(id) != nullptr; });
             };
             rename(renamed);
             context.record(scene_change(
                 state,
                 [&state, before = std::move(before), rename, all_here, name] {
                     if (!all_here()) return;
                     state.scene.effects = before;
                     rename(name);
                 },
                 [&state, after = std::move(after), rename, all_here, renamed] {
                     if (!all_here()) return;
                     state.scene.effects = after;
                     rename(renamed);
                 }));
             Json out = effect_json(changed, state.scene.particle_looks);
             out["entities_renamed"] = ids.size();
             return CommandResult::success(std::move(out));
         }});

    add(registry,
        {"remove_effect",
         "Removes one of the scene's effects (by name). Entities running it then run the built-in effect of "
         "that name if there is one, else nothing (their effect is kept). Undoable.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"name"}},
              {"properties", {{"name", name_schema()}}}},
         false,
         [&state](const Json& args, CommandContext& context) {
             Effects& effects = state.scene.effects;
             const std::string name = args["name"].get<std::string>();
             if (find(effects, name) == nullptr) {
                 return CommandResult::failure("the scene has no effect named \"" + name + "\"" +
                                               (built_in_effect(name) ? " (built-in effects cannot be removed)" : ""));
             }
             Effects before = effects;
             std::erase_if(effects, [&](const Effect& f) { return f.name == name; });
             record(state, context, state.scene.particle_looks, std::move(before));
             return CommandResult::success({{"removed", name}, {"entities", runners(state, name)}});
         }});

    add(registry,
        {"get_effects",
         std::string("Lists the scene's particle looks and effects (every field; an effect naming a look found "
                     "nowhere lists it in missing_looks), the built-in ones' names (") + k_built_ins +
             "; with built_in true, every field of them too), and the entities naming each effect (on or off).",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"properties", {{"built_in", {{"type", "boolean"}, {"description", "Every field of the built-in "
                                                                                  "looks and effects too."}}}}}},
         true,
         [&state](const Json& args, CommandContext&) {
             const Looks& looks = state.scene.particle_looks;
             Json look_list = Json::array();
             for (const ParticleLook& l : looks) look_list.push_back(look_json(l));
             Json effect_list = Json::array();
             for (const Effect& f : state.scene.effects) {
                 Json j = effect_json(f, looks);
                 j["entities"] = runners(state, f.name);
                 effect_list.push_back(std::move(j));
             }
             const bool full = args.value("built_in", false);
             Json built_looks = Json::array();
             for (const ParticleLook& l : world::built_in_particle_looks()) {
                 const bool shadowed = find(looks, l.name) != nullptr;
                 Json j = full ? look_json(l) : Json{{"name", l.name}};
                 if (shadowed) j["stood_in_for"] = true;
                 built_looks.push_back(std::move(j));
             }
             Json built_effects = Json::array();
             for (const Effect& f : world::built_in_effects()) {
                 const bool shadowed = find(state.scene.effects, f.name) != nullptr;
                 Json j = full ? effect_json(f, looks) : Json{{"name", f.name}};
                 if (shadowed) {
                     j["stood_in_for"] = true;
                 } else {
                     j["entities"] = runners(state, f.name);
                 }
                 built_effects.push_back(std::move(j));
             }
             return CommandResult::success({{"particle_looks", std::move(look_list)},
                                            {"effects", std::move(effect_list)},
                                            {"built_in", {{"particle_looks", std::move(built_looks)},
                                                          {"effects", std::move(built_effects)}}}});
         }});
}

} // namespace giant::editor
