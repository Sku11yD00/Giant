// Water commands: lakes, rivers and the sea (world/water.h). Scene data
// (saved with the scene and the world's world.json, undoable).
#include "editor_commands.h"

#include "giant/core/assert.h"
#include "giant/world/water.h"

#include <algorithm>
#include <string>

namespace giant::editor {

namespace {

using ai::CommandContext;
using ai::CommandResult;
using ai::Json;
using world::Water;
using Waters = std::vector<Water>;

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

Json properties() {
    const double h = world::k_max_terrain_height;
    return {
        {"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
        {"kind", {{"enum", {"lake", "river", "sea"}},
                  {"description", "lake: a level inside its outline; river: a strip down its path; sea: a level "
                                  "everywhere (the shore is where the ground rises over it)."}}},
        {"level", number(-h, h, "Lake and sea: the surface's height, metres.")},
        {"outline", {{"type", "array"},
                     {"minItems", 3},
                     {"maxItems", world::k_max_water_points},
                     {"items", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 2}, {"maxItems", 2}}},
                     {"description", "Lake: its shore, [x, z] metres in order (the water is inside)."}}},
        {"path", {{"type", "array"},
                  {"minItems", 2},
                  {"maxItems", world::k_max_water_points},
                  {"items", {{"type", "object"},
                             {"additionalProperties", false},
                             {"required", {"x", "z", "width", "level"}},
                             {"properties", {{"x", {{"type", "number"}}},
                                             {"z", {{"type", "number"}}},
                                             {"width", number(0.5, 2000, "Metres across here.")},
                                             {"level", number(-h, h, "The surface's height here.")}}}}},
                  {"description", "River: down its middle, upstream first."}}},
        {"flow", number(0, 20, "River: metres a second downstream (default 1).")},
        {"color", triple(0, 1, "What it scatters back: its colour seen deep, linear RGB.")},
        {"absorption", triple(0, 10, "Light lost a metre through it, per channel (default clear water 0.45, 0.09, 0.065).")},
        {"turbidity", number(0, 1, "0 clear to 1 muddy (default 0.1).")},
        {"waves", number(0, 4, "0 still to 4: how high the wind raises its waves (default 1).")},
        {"visible", {{"type", "boolean"}, {"description", "Hidden: not there (not drawn, nothing floats)."}}},
    };
}

Json water_json(const Water& w) {
    Json j = {{"name", w.name},
              {"kind", std::string(world::water_kind_name(w.kind))},
              {"level", w.level},
              {"flow", w.flow},
              {"color", Json::array({w.color.x, w.color.y, w.color.z})},
              {"absorption", Json::array({w.absorption.x, w.absorption.y, w.absorption.z})},
              {"turbidity", w.turbidity},
              {"waves", w.waves},
              {"visible", w.visible}};
    Json outline = Json::array();
    for (const world::WaterPoint& p : w.outline) outline.push_back(Json::array({p.x, p.z}));
    j["outline"] = std::move(outline);
    Json path = Json::array();
    for (const world::RiverPoint& p : w.path) {
        path.push_back({{"x", p.x}, {"z", p.z}, {"width", p.width}, {"level", p.level}});
    }
    j["path"] = std::move(path);
    double x0, z0, x1, z1;
    if (world::water_bounds(w, x0, z0, x1, z1)) j["bounds"] = {{"from", {x0, z0}}, {"to", {x1, z1}}};
    return j;
}

Vec3 vec(const Json& v) { return {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()}; }

// Sets the fields given (outline and path replace the lists); a kind
// changed drops the other kind's points unless given.
std::string apply_water(const Json& args, Water& w) {
    if (args.contains("kind")) {
        const auto kind = world::water_kind_from_name(args["kind"].get<std::string>());
        if (!kind) return "kind must be lake, river or sea";
        if (*kind != w.kind) {
            if (*kind != world::WaterKind::lake) w.outline.clear();
            if (*kind != world::WaterKind::river) w.path.clear();
        }
        w.kind = *kind;
    }
    if (args.contains("name")) w.name = args["name"].get<std::string>();
    w.level = args.value("level", w.level);
    w.flow = args.value("flow", w.flow);
    if (args.contains("color")) w.color = vec(args["color"]);
    if (args.contains("absorption")) w.absorption = vec(args["absorption"]);
    w.turbidity = args.value("turbidity", w.turbidity);
    w.waves = args.value("waves", w.waves);
    w.visible = args.value("visible", w.visible);
    if (args.contains("outline")) {
        w.outline.clear();
        for (const Json& p : args["outline"]) w.outline.push_back({p[0].get<double>(), p[1].get<double>()});
    }
    if (args.contains("path")) {
        w.path.clear();
        for (const Json& p : args["path"]) {
            w.path.push_back({p["x"].get<double>(), p["z"].get<double>(), p["width"].get<float>(), p["level"].get<float>()});
        }
    }
    if (const auto bad = world::check_water(w)) return *bad;
    return {};
}

void record(EditorState& state, CommandContext& context, Waters before, Waters after) {
    context.record(scene_change(
        state, [&state, before = std::move(before)] { state.scene.water = before; },
        [&state, after = std::move(after)] { state.scene.water = after; }));
}

Water* find(Waters& waters, const std::string& name) {
    const auto it = std::find_if(waters.begin(), waters.end(), [&](const Water& w) { return w.name == name; });
    return it == waters.end() ? nullptr : &*it;
}

Json get_schema() {
    Json point = {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 2}, {"maxItems", 2}};
    Json points = {{"type", "array"}, {"maxItems", 4096}, {"items", point}};
    Json props = Json::object();
    props["points"] = points;
    return {{"type", "object"}, {"additionalProperties", false}, {"properties", props}};
}

void add(ai::CommandRegistry& registry, ai::CommandDesc desc) {
    const bool added = registry.add(std::move(desc));
    GIANT_ASSERT(added, "water command could not be registered");
}

} // namespace

void register_water_commands(ai::CommandRegistry& registry, EditorState& state) {
    add(registry,
        {"create_water",
         "Adds a body of water: a lake (`level` inside its `outline`), a river (a strip down its `path`, "
         "its width and level given at each point, flowing `flow` m/s from the first point on) or the sea "
         "(`level` everywhere; the shore is where the ground rises over it). Water never moves the ground: "
         "carve its bed with sculpt_terrain. `color`, `absorption`, `turbidity` and `waves` set how it "
         "looks. Saved with the scene or the world. Undoable.",
         Json{{"type", "object"}, {"additionalProperties", false}, {"required", {"name", "kind"}},
              {"properties", properties()}},
         false,
         [&state](const Json& args, CommandContext& context) {
             Waters& waters = state.scene.water;
             if (waters.size() >= world::k_max_waters) {
                 return CommandResult::failure("a scene has at most " + std::to_string(world::k_max_waters) +
                                               " bodies of water");
             }
             const std::string name = args["name"].get<std::string>();
             if (find(waters, name) != nullptr) {
                 return CommandResult::failure("there is water named \"" + name + "\" already: set_water changes it");
             }
             Water w;
             if (std::string bad = apply_water(args, w); !bad.empty()) return CommandResult::failure(bad);
             Waters before = waters;
             waters.push_back(w);
             record(state, context, std::move(before), waters);
             return CommandResult::success(water_json(w));
         }});

    Json set_props = properties();
    set_props["new_name"] = {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}};
    add(registry,
        {"set_water",
         "Changes a body of water (by name): only the fields given (outline and path replace the lists; a "
         "new kind drops the old kind's points; new_name renames it). Undoable.",
         Json{{"type", "object"}, {"additionalProperties", false}, {"required", {"name"}}, {"properties", set_props}},
         false,
         [&state](const Json& args, CommandContext& context) {
             Waters& waters = state.scene.water;
             const std::string name = args["name"].get<std::string>();
             Water* w = find(waters, name);
             if (w == nullptr) return CommandResult::failure("no water named \"" + name + "\"");
             Water changed = *w;
             Json fields = args;
             fields.erase("name");
             if (args.contains("new_name")) {
                 const std::string renamed = args["new_name"].get<std::string>();
                 if (renamed != name && find(waters, renamed) != nullptr) {
                     return CommandResult::failure("there is water named \"" + renamed + "\" already");
                 }
                 fields["name"] = renamed;
                 fields.erase("new_name");
             }
             if (std::string bad = apply_water(fields, changed); !bad.empty()) return CommandResult::failure(bad);
             Waters before = waters;
             *w = changed;
             record(state, context, std::move(before), waters);
             return CommandResult::success(water_json(changed));
         }});

    add(registry,
        {"remove_water",
         "Removes a body of water (by name). Undoable.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"name"}},
              {"properties", {{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}}}}},
         false,
         [&state](const Json& args, CommandContext& context) {
             Waters& waters = state.scene.water;
             const std::string name = args["name"].get<std::string>();
             if (find(waters, name) == nullptr) return CommandResult::failure("no water named \"" + name + "\"");
             Waters before = waters;
             std::erase_if(waters, [&](const Water& w) { return w.name == name; });
             record(state, context, std::move(before), waters);
             return CommandResult::success({{"removed", name}});
         }});

    add(registry,
        {"get_water",
         "Lists the bodies of water (every field, and the ground each covers as `bounds`), and with "
         "`points` ([x, z] metres, at most 4096) the water at each: the body's `name`, the surface's "
         "`level`, `depth` over the ground (null where there is no ground tile; water only where the "
         "ground is under the level), and a river's `flow` [x, z] m/s and `width`; null where there is none.",
         get_schema(),
         true,
         [&state](const Json& args, CommandContext&) {
             Json list = Json::array();
             for (const Water& w : state.scene.water) list.push_back(water_json(w));
             Json out = {{"water", std::move(list)}};
             if (args.contains("points")) {
                 Json at = Json::array();
                 for (const Json& p : args["points"]) {
                     const double x = p[0].get<double>(), z = p[1].get<double>();
                     const auto here = world::water_at(state.scene.water, x, z);
                     const std::optional<double> ground = state.scene.terrain.height_at(x, z);
                     if (!here || (ground && *ground >= here->level)) {
                         at.push_back(nullptr);
                         continue;
                     }
                     Json j = {{"name", here->water->name}, {"level", here->level}};
                     j["depth"] = ground ? Json(here->level - *ground) : Json(nullptr);
                     if (here->water->kind == world::WaterKind::river) {
                         j["flow"] = Json::array({here->flow_x, here->flow_z});
                         j["width"] = here->width;
                     }
                     at.push_back(std::move(j));
                 }
                 out["at"] = std::move(at);
             }
             return CommandResult::success(std::move(out));
         }});
}

} // namespace giant::editor
