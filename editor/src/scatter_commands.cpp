// Scatter commands: layers of things strewn over the ground by rules
// (world/scatter.h). The layers are scene data (saved, undoable); their
// placements follow on a worker thread (ScatterPlacer), cell by cell, so
// get_scatter says when they are all placed.
#include "editor_commands.h"
#include "material_maps.h"
#include "mesh_library.h"
#include "scatter_placer.h"

#include "giant/core/assert.h"
#include "giant/world/scatter.h"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>

namespace giant::editor {

namespace {

using ai::CommandContext;
using ai::CommandResult;
using ai::Json;
using world::ScatterItem;
using world::ScatterLayer;

Json number(double minimum, double maximum, const char* description) {
    return {{"type", "number"}, {"minimum", minimum}, {"maximum", maximum}, {"description", description}};
}
Json pair(double minimum, double maximum, const char* description) {
    return {{"type", "array"},
            {"items", {{"type", "number"}, {"minimum", minimum}, {"maximum", maximum}}},
            {"minItems", 2},
            {"maxItems", 2},
            {"description", description}};
}
Json triple(double minimum, double maximum, const char* description) {
    return {{"type", "array"},
            {"items", {{"type", "number"}, {"minimum", minimum}, {"maximum", maximum}}},
            {"minItems", 3},
            {"maxItems", 3},
            {"description", description}};
}

Json item_schema() {
    return {{"type", "object"},
            {"additionalProperties", false},
            {"properties",
             {{"shape", {{"enum", {"cube", "sphere", "plane", "mesh", "kit"}},
                         {"description", "Default cube; giving mesh or kit sets it."}}},
              {"mesh", {{"type", "string"},
                        {"minLength", 1},
                        {"maxLength", world::k_max_mesh_path_bytes},
                        {"description", "A glTF or GLB file (imported on first use). The item takes the file's look (import_mesh: material) under the look fields given."}}},
              {"kit", {{"type", "string"},
                       {"minLength", 1},
                       {"maxLength", world::k_max_name_bytes},
                       {"description", "A kit of the scene (a tree with its rocks)."}}},
              {"base_color", triple(0, 1, "Linear RGB. Default light grey.")},
              {"roughness", number(0, 1, "Default 0.5.")},
              {"metallic", number(0, 1, "Default 0.")},
              {"emissive", triple(0, 1000, "Default none.")},
              {"surface",
               {{"enum", {"standard", "flesh", "wood", "glass", "dirt", "foliage", "brick", "concrete", "metal"}},
                {"description", "What bullets meet (set_entity's surface). Default standard; trees are wood."}}},
              {"sway", number(0, 4, "How it bends in the wind (set_entity's sway): 1 a tree, 2 to 3 grass. "
                                    "Default 0.")},
              {"flutter", number(0, 4, "How its tips shake in the wind (set_entity's flutter). Default 0.")},
              {"two_sided", {{"type", "boolean"}, {"description", "Drawn from both sides (set_entity's two_sided)."}}},
              {"translucency", number(0, 1, "Sun shining through it (set_entity's translucency). Default 0.")},
              {"alpha_cutoff", number(0, 1, "Cut away where the base colour map's alpha is under it "
                                            "(set_entity's alpha_cutoff). Default 0.")},
              {"maps", maps_schema()},
              {"weight", number(1e-6, 1e6, "How often it is picked, against the layer's other items. Default 1.")},
              {"scale", pair(0.0001, 10000, "[min, max] uniform scale, picked evenly. Default [1, 1].")},
              {"align", number(0, 1, "0 upright (trees), 1 along the ground's slope (rocks). Default 0.")},
              {"sink", number(-1000, 1000, "Metres into the ground at scale 1. Default 0.")},
              {"tree", {{"type", {"object", "null"}},
                        {"additionalProperties", false},
                        {"properties",
                         {{"trunk_radius", number(0.01, 100, "Metres at scale 1. Default 0.25.")},
                          {"height", number(0.1, 1000, "Metres at scale 1, foot to top: what falls. Default 10.")},
                          {"strength", number(0, 1e9, "The blow (N s, at scale 1; grows with scale squared) that "
                                                      "snaps it. 0 (default): 8000, more than a tree its own size falling on it.")}}},
                        {"description", "A tree: while simulating its trunk stands as a body, and struck hard "
                                        "enough (or by impact) it snaps and topples onto what is there. "
                                        "Null or left out: it never falls."}}}}}};
}

// Every field a layer has (all optional on set; name and items needed on add).
Json layer_properties() {
    return {
        {"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", world::k_max_name_bytes}}},
        {"items", {{"type", "array"}, {"items", item_schema()}, {"minItems", 1},
                   {"maxItems", world::k_max_scatter_items},
                   {"description", "What it places, picked by weight (replaces them all on set)."}}},
        {"spacing", number(world::k_min_scatter_spacing, world::k_max_scatter_spacing,
                           "Metres between points, about (a jittered grid). Default 4.")},
        {"density", number(0, 1, "The share of points kept. Default 1.")},
        {"jitter", number(0, 1, "0 an even grid, 1 anywhere in its square; no two points closer than "
                                "spacing x (1 - jitter). Default 0.9.")},
        {"clump_size", number(0, 100000, "Metres across the clumps of a noise that thins the points: 0 (none, the default) or 1 to 100000.")},
        {"clumping", number(0, 1, "How far the clumps thin: 1 keeps only their high parts. Default 0.")},
        {"height_min", number(-10000, 10000, "The lowest ground (metres) it goes on.")},
        {"height_max", number(-10000, 10000, "The highest ground it goes on.")},
        {"slope_min", number(0, 90, "The flattest ground (degrees) it goes on. Default 0.")},
        {"slope_max", number(0, 90, "The steepest ground it goes on. Default 90.")},
        {"area", {{"type", {"object", "null"}},
                  {"additionalProperties", false},
                  {"required", {"from", "to"}},
                  {"properties",
                   {{"from", pair(-1e8, 1e8, "[x, z] metres: a corner.")},
                    {"to", pair(-1e8, 1e8, "[x, z] metres: the opposite corner.")}}},
                  {"description", "Only within this rectangle (null: wherever there is ground)."}}},
        {"seed", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4294967295.0},
                  {"description", "Another seed, other points. Default 1."}}},
        {"visible", {{"type", "boolean"}}},
    };
}

Vec3 vec3_from(const Json& j) { return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()}; }

// Sets the fields `args` gives; returns what is wrong, or "".
std::string apply_layer(const Json& args, ScatterLayer& l, EditorState& state) {
    if (args.contains("name")) l.name = args["name"].get<std::string>();
    if (args.contains("items")) {
        l.items.clear();
        for (const Json& j : args["items"]) {
            ScatterItem i;
            if (j.contains("mesh")) {
                i.mesh = MeshLibrary::normalize(j["mesh"].get<std::string>());
                i.shape = world::Shape::mesh;
            }
            if (j.contains("kit")) {
                i.kit = j["kit"].get<std::string>();
                i.shape = world::Shape::kit;
            }
            if (j.contains("shape")) i.shape = world::shape_from_string(j["shape"].get<std::string>()).value_or(i.shape);
            if (j.contains("base_color")) i.material.base_color = vec3_from(j["base_color"]);
            i.material.roughness = j.value("roughness", i.material.roughness);
            i.material.metallic = j.value("metallic", i.material.metallic);
            if (j.contains("emissive")) i.material.emissive = vec3_from(j["emissive"]);
            if (j.contains("surface")) {
                i.material.surface =
                    world::surface_from_string(j["surface"].get<std::string>()).value_or(i.material.surface);
            }
            i.material.sway = j.value("sway", i.material.sway);
            i.material.flutter = j.value("flutter", i.material.flutter);
            i.material.two_sided = j.value("two_sided", i.material.two_sided);
            i.material.translucency = j.value("translucency", i.material.translucency);
            i.material.alpha_cutoff = j.value("alpha_cutoff", i.material.alpha_cutoff);
            if (j.contains("maps")) {
                if (std::string bad = apply_maps(j["maps"], i.material.maps); !bad.empty()) return bad;
            }
            i.weight = j.value("weight", i.weight);
            if (j.contains("scale")) {
                i.scale_min = j["scale"][0].get<float>();
                i.scale_max = j["scale"][1].get<float>();
            }
            i.align = j.value("align", i.align);
            i.sink = j.value("sink", i.sink);
            if (j.contains("tree") && !j["tree"].is_null()) {
                const Json& t = j["tree"];
                world::ScatterTree tree;
                tree.trunk_radius = t.value("trunk_radius", tree.trunk_radius);
                tree.height = t.value("height", tree.height);
                tree.strength = t.value("strength", tree.strength);
                i.tree = tree;
            }
            if (i.shape == world::Shape::kit && state.scene.find_kit(i.kit) == nullptr) {
                return "no kit named \"" + i.kit + "\"; list_kits shows them, create_kit makes one";
            }
            if (i.shape == world::Shape::mesh) {
                if (i.mesh.empty()) return "a mesh item needs its file: give mesh, e.g. \"assets/tree.glb\"";
                if (state.meshes == nullptr) return "meshes cannot be imported in this editor";
                const MeshLibrary::Loaded loaded = state.meshes->load(i.mesh);
                if (loaded.mesh == nullptr) return loaded.error;
                take_file_look(*loaded.mesh, i.material, j);
            }
            l.items.push_back(std::move(i));
        }
    }
    l.spacing = args.value("spacing", l.spacing);
    l.density = args.value("density", l.density);
    l.jitter = args.value("jitter", l.jitter);
    l.clump_size = args.value("clump_size", l.clump_size);
    l.clumping = args.value("clumping", l.clumping);
    l.height_min = args.value("height_min", l.height_min);
    l.height_max = args.value("height_max", l.height_max);
    l.slope_min = args.value("slope_min", l.slope_min);
    l.slope_max = args.value("slope_max", l.slope_max);
    if (args.contains("area")) {
        const Json& a = args["area"];
        if (a.is_null()) {
            l.area.reset();
        } else {
            const double x0 = a["from"][0].get<double>(), z0 = a["from"][1].get<double>();
            const double x1 = a["to"][0].get<double>(), z1 = a["to"][1].get<double>();
            l.area = world::ScatterArea{std::min(x0, x1), std::min(z0, z1), std::max(x0, x1), std::max(z0, z1)};
        }
    }
    l.seed = args.value("seed", l.seed);
    l.visible = args.value("visible", l.visible);
    if (const auto bad = world::check_scatter_layer(l)) return *bad;
    return {};
}

Json layer_json(const ScatterLayer& l) {
    Json items = Json::array();
    for (const ScatterItem& i : l.items) {
        Json j = {{"shape", world::to_string(i.shape)}};
        if (i.shape == world::Shape::mesh) j["mesh"] = i.mesh;
        if (i.shape == world::Shape::kit) j["kit"] = i.kit;
        if (i.shape != world::Shape::kit) {
            j["base_color"] = {i.material.base_color.x, i.material.base_color.y, i.material.base_color.z};
            j["roughness"] = i.material.roughness;
            j["metallic"] = i.material.metallic;
            j["emissive"] = {i.material.emissive.x, i.material.emissive.y, i.material.emissive.z};
        }
        if (i.material.surface != world::Surface::standard) j["surface"] = world::to_string(i.material.surface);
        if (i.material.sway != 0.0f) j["sway"] = i.material.sway;
        if (i.material.flutter != 0.0f) j["flutter"] = i.material.flutter;
        if (i.material.two_sided) j["two_sided"] = true;
        if (i.material.translucency != 0.0f) j["translucency"] = i.material.translucency;
        if (i.material.alpha_cutoff != 0.0f) j["alpha_cutoff"] = i.material.alpha_cutoff;
        if (!i.material.maps.empty() || i.material.maps.normal_green_down) j["maps"] = maps_json(i.material.maps);
        j["weight"] = i.weight;
        j["scale"] = {i.scale_min, i.scale_max};
        j["align"] = i.align;
        j["sink"] = i.sink;
        j["tree"] = i.tree ? Json{{"trunk_radius", i.tree->trunk_radius},
                                  {"height", i.tree->height},
                                  {"strength", i.tree->strength}}
                           : Json(nullptr);
        items.push_back(std::move(j));
    }
    Json out = {{"name", l.name},
                {"items", std::move(items)},
                {"spacing", l.spacing},
                {"density", l.density},
                {"jitter", l.jitter},
                {"clump_size", l.clump_size},
                {"clumping", l.clumping},
                {"height_min", l.height_min},
                {"height_max", l.height_max},
                {"slope_min", l.slope_min},
                {"slope_max", l.slope_max},
                {"seed", l.seed},
                {"visible", l.visible}};
    out["area"] = l.area ? Json{{"from", {l.area->from_x, l.area->from_z}}, {"to", {l.area->to_x, l.area->to_z}}}
                         : Json(nullptr);
    return out;
}

using Layers = std::vector<ScatterLayer>;

// Records `before` -> `after` (the whole list: layers are few).
void record(EditorState& state, CommandContext& context, Layers before, Layers after) {
    context.record(scene_change(
        state, [&state, before = std::move(before)] { state.scene.scatter = before; },
        [&state, after = std::move(after)] { state.scene.scatter = after; }));
}

ScatterLayer* find(Layers& layers, const std::string& name) {
    const auto it = std::find_if(layers.begin(), layers.end(), [&](const ScatterLayer& l) { return l.name == name; });
    return it == layers.end() ? nullptr : &*it;
}

void add(ai::CommandRegistry& registry, ai::CommandDesc desc) {
    const bool added = registry.add(std::move(desc));
    GIANT_ASSERT(added, "scatter command could not be registered");
}

} // namespace

void register_scatter_commands(ai::CommandRegistry& registry, EditorState& state) {
    Json add_props = layer_properties();
    add(registry,
        {"add_scatter_layer",
         "Adds a scatter layer: things strewn over the ground by rules (forests, rocks, bushes, grass), "
         "like Unreal's PCG. Points about `spacing` apart (a jittered grid) are kept by density, clumps, "
         "`area`, and the ground's height and slope there; each gets an item (a built-in shape, a mesh or "
         "a kit, picked by weight), a random turn about up, a scale in its range, and stands upright or "
         "along the slope. Placements are worked out from the rules and the terrain (not saved: the same "
         "every time), cell by cell on a worker thread, again wherever the ground or the layer changes; "
         "get_scatter says when all are placed. Only ground with terrain tiles is scattered. Undoable.",
         Json{{"type", "object"}, {"additionalProperties", false}, {"required", {"name", "items"}},
              {"properties", add_props}},
         false,
         [&state](const Json& args, CommandContext& context) {
             Layers& layers = state.scene.scatter;
             if (layers.size() >= world::k_max_scatter_layers) {
                 return CommandResult::failure("a scene has at most " + std::to_string(world::k_max_scatter_layers) +
                                               " scatter layers");
             }
             const std::string name = args["name"].get<std::string>();
             if (find(layers, name) != nullptr) {
                 return CommandResult::failure("there is a scatter layer named \"" + name +
                                               "\" already: set_scatter_layer changes it");
             }
             ScatterLayer layer;
             if (std::string bad = apply_layer(args, layer, state); !bad.empty()) return CommandResult::failure(bad);
             Layers before = layers;
             layers.push_back(layer);
             record(state, context, std::move(before), layers);
             return CommandResult::success(layer_json(layer));
         }});

    Json set_props = layer_properties();
    set_props["new_name"] = {{"type", "string"}, {"minLength", 1}, {"maxLength", world::k_max_name_bytes}};
    add(registry,
        {"set_scatter_layer",
         "Changes a scatter layer (by name): only the fields given (items replaces the list; area null "
         "scatters wherever there is ground; new_name renames it). Its placements follow. Undoable.",
         Json{{"type", "object"}, {"additionalProperties", false}, {"required", {"name"}}, {"properties", set_props}},
         false,
         [&state](const Json& args, CommandContext& context) {
             Layers& layers = state.scene.scatter;
             const std::string name = args["name"].get<std::string>();
             ScatterLayer* layer = find(layers, name);
             if (layer == nullptr) return CommandResult::failure("no scatter layer named \"" + name + "\"");
             ScatterLayer changed = *layer;
             Json fields = args;
             fields.erase("name");
             if (args.contains("new_name")) {
                 const std::string renamed = args["new_name"].get<std::string>();
                 if (renamed != name && find(layers, renamed) != nullptr) {
                     return CommandResult::failure("there is a scatter layer named \"" + renamed + "\" already");
                 }
                 fields["name"] = renamed;
                 fields.erase("new_name");
             }
             if (std::string bad = apply_layer(fields, changed, state); !bad.empty()) {
                 return CommandResult::failure(bad);
             }
             Layers before = layers;
             *layer = changed;
             record(state, context, std::move(before), layers);
             return CommandResult::success(layer_json(changed));
         }});

    add(registry,
        {"remove_scatter_layer",
         "Removes a scatter layer (by name) and everything it placed. Undoable.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"name"}},
              {"properties", {{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", world::k_max_name_bytes}}}}}},
         false,
         [&state](const Json& args, CommandContext& context) {
             Layers& layers = state.scene.scatter;
             const std::string name = args["name"].get<std::string>();
             if (find(layers, name) == nullptr) return CommandResult::failure("no scatter layer named \"" + name + "\"");
             Layers before = layers;
             std::erase_if(layers, [&](const ScatterLayer& l) { return l.name == name; });
             record(state, context, std::move(before), layers);
             return CommandResult::success({{"removed", name}});
         }});

    add(registry,
        {"get_scatter",
         "Lists the scatter layers (every field) and what they placed: cells, points by layer, and "
         "`building` (cells still being placed: 0 when all are drawn as the rules say).",
         Json{{"type", "object"}, {"additionalProperties", false}},
         true,
         [&state](const Json&, CommandContext&) {
             Json layers = Json::array();
             std::unordered_map<std::string, u64> counts;
             if (state.scatter != nullptr) {
                 for (const world::CellCoord c : state.scatter->cells()) {
                     const ScatterPlacer::Cell* cell = state.scatter->find(c);
                     for (const world::ScatterPoint& p : cell->points) ++counts[(*cell->layers)[p.layer].name];
                 }
             }
             for (const ScatterLayer& l : state.scene.scatter) {
                 Json j = layer_json(l);
                 j["points"] = counts[l.name];
                 layers.push_back(std::move(j));
             }
             Json out = {{"layers", std::move(layers)}};
             if (state.scatter != nullptr) {
                 const ScatterStats s = state.scatter->stats();
                 out["cells"] = s.cells;
                 out["points"] = s.points;
                 out["instances"] = s.instances;
                 out["building"] = s.building;
                 out["cells_cut_short"] = s.cut_short;
             }
             return CommandResult::success(std::move(out));
         }});
}

} // namespace giant::editor
