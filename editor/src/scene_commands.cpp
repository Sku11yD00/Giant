// Scene, lighting and camera commands: how people (through the panels) and
// the AI (over MCP) build and light a scene. Every scene change is undoable;
// the camera is view state and is not.
#include "editor_commands.h"
#include "mesh_library.h"

#include "giant/core/assert.h"
#include "giant/core/text.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace giant::editor {

namespace {

using ai::CommandContext;
using ai::CommandResult;
using ai::Json;
using world::Entity;
using world::EntityId;

constexpr usize k_max_batch = 10'000;

void add(ai::CommandRegistry& registry, ai::CommandDesc desc) {
    const bool added = registry.add(std::move(desc));
    GIANT_ASSERT(added, "scene command could not be registered");
}

Json vec3_json(const Vec3& v) { return Json::array({v.x, v.y, v.z}); }
Json dvec3_json(const DVec3& v) { return Json::array({v.x, v.y, v.z}); }
Vec3 vec3_from(const Json& j) { return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()}; }
DVec3 dvec3_from(const Json& j) {
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

// "assets/big rock.glb" -> "big rock"
std::string utf8_stem(const std::string& path) {
    const usize slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const usize dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name.resize(dot);
    return name.empty() ? std::string("mesh") : name;
}

Json entity_json(const Entity& e) {
    const EulerDegrees r = euler_from_quat(e.transform.rotation);
    Json j = {{"id", e.id},
            {"name", e.name},
            {"shape", world::to_string(e.shape)},
            {"position", dvec3_json(e.transform.position)},
            {"rotation", {{"pitch", r.pitch}, {"yaw", r.yaw}, {"roll", r.roll}}},
            {"scale", vec3_json(e.transform.scale)},
            {"base_color", vec3_json(e.material.base_color)},
            {"roughness", e.material.roughness},
            {"metallic", e.material.metallic},
            {"emissive", vec3_json(e.material.emissive)},
            {"visible", e.visible}};
    if (!e.mesh.empty()) {
        j["mesh"] = e.mesh;
    }
    return j;
}

Json mesh_json(const MeshLibrary::Mesh& m) {
    return {{"path", m.path},
            {"triangles", m.triangles},
            {"clusters", m.clusters},
            {"levels", m.levels},
            {"coarsest_triangles", m.coarsest_triangles},
            {"import_seconds", m.import_seconds},
            {"warnings", m.warnings}};
}

// JSON Schema helpers.
Json number(double minimum, double maximum, const char* description) {
    return {{"type", "number"}, {"minimum", minimum}, {"maximum", maximum},
            {"description", description}};
}
Json triple(double minimum, double maximum, const char* description) {
    return {{"type", "array"},
            {"items", {{"type", "number"}, {"minimum", minimum}, {"maximum", maximum}}},
            {"minItems", 3},
            {"maxItems", 3},
            {"description", description}};
}

// Every field an entity has; all optional (defaults on create, unchanged
// on set).
Json entity_properties() {
    return {
        {"name", {{"type", "string"}, {"maxLength", 128}}},
        {"shape",
         {{"enum", {"cube", "sphere", "plane", "mesh"}},
          {"description", "cube (1 m), sphere (1 m wide), plane (1 m square facing up), or "
                          "mesh (the file in `mesh`). Default cube."}}},
        {"mesh",
         {{"type", "string"},
          {"minLength", 1},
          {"maxLength", world::k_max_mesh_path_bytes},
          {"description", "A glTF or GLB file to draw (path from the editor's working folder); "
                          "imported on first use. Giving it sets shape to mesh unless shape is "
                          "given too."}}},
        {"position", triple(-1e8, 1e8, "Centre in metres [x, y, z]; +Y up. Default origin.")},
        {"rotation",
         {{"type", "object"},
          {"additionalProperties", false},
          {"properties",
           {{"pitch", number(-360, 360, "Degrees about X (tilt up).")},
            {"yaw", number(-360, 360, "Degrees about Y (turn left).")},
            {"roll", number(-360, 360, "Degrees about Z.")}}},
          {"description", "Degrees; applied yaw, then pitch, then roll. Default none."}}},
        {"scale", triple(0.0001, 1e6, "Size multiplier per axis [x, y, z]. Default [1, 1, 1].")},
        {"base_color", triple(0, 1, "Linear RGB albedo. Default light grey.")},
        {"roughness", number(0, 1, "0 mirror-smooth, 1 matte. Default 0.5.")},
        {"metallic", number(0, 1, "0 for non-metals, 1 for metals. Default 0.")},
        {"emissive", triple(0, 1000, "Linear RGB light it gives off. Default none.")},
        {"visible", {{"type", "boolean"}}},
    };
}

// Returns what is wrong (and may have changed `e` partly), or "".
std::string apply_fields(const Json& args, Entity& e, EditorState& state) {
    if (args.contains("name")) e.name = args["name"].get<std::string>();
    if (args.contains("shape")) {
        e.shape = world::shape_from_string(args["shape"].get<std::string>()).value_or(e.shape);
    }
    if (args.contains("mesh")) {
        e.mesh = MeshLibrary::normalize(args["mesh"].get<std::string>());
        if (!args.contains("shape")) e.shape = world::Shape::mesh;
    }
    if (args.contains("position")) e.transform.position = dvec3_from(args["position"]);
    if (args.contains("rotation")) {
        // Fields left out keep their current value.
        EulerDegrees r = euler_from_quat(e.transform.rotation);
        const Json& rot = args["rotation"];
        r.pitch = rot.value("pitch", r.pitch);
        r.yaw = rot.value("yaw", r.yaw);
        r.roll = rot.value("roll", r.roll);
        e.transform.rotation = normalize(quat_from_euler(r));
    }
    if (args.contains("scale")) e.transform.scale = vec3_from(args["scale"]);
    if (args.contains("base_color")) e.material.base_color = vec3_from(args["base_color"]);
    if (args.contains("roughness")) e.material.roughness = args["roughness"].get<float>();
    if (args.contains("metallic")) e.material.metallic = args["metallic"].get<float>();
    if (args.contains("emissive")) e.material.emissive = vec3_from(args["emissive"]);
    if (args.contains("visible")) e.visible = args["visible"].get<bool>();
    if (e.shape == world::Shape::mesh) {
        if (e.mesh.empty()) {
            return "shape mesh needs a file: give mesh, e.g. \"assets/rock.glb\"";
        }
        if (e.mesh.size() > world::k_max_mesh_path_bytes) {
            return "the mesh path is too long";
        }
        if (state.meshes == nullptr) {
            return "meshes cannot be imported in this editor";
        }
        const MeshLibrary::Loaded loaded = state.meshes->load(e.mesh);
        if (loaded.mesh == nullptr) {
            return loaded.error;
        }
    }
    return {};
}

// An entity from create arguments, or what is wrong with them.
std::string new_entity(const Json& args, EditorState& state, Entity& out) {
    out = Entity{};
    if (std::string error = apply_fields(args, out, state); !error.empty()) {
        return error;
    }
    if (out.name.empty()) {
        std::string base = world::to_string(out.shape);
        if (out.shape == world::Shape::mesh) {
            base = utf8_stem(out.mesh);
        }
        out.name = base + " " + std::to_string(state.scene.next_id());
    }
    return {};
}

Json lighting_json(const world::Lighting& l) {
    return {{"sun_elevation_degrees", l.sun_elevation_degrees},
            {"sun_azimuth_degrees", l.sun_azimuth_degrees},
            {"sun_color", vec3_json(l.sun_color)},
            {"sun_intensity", l.sun_intensity},
            {"sky_color", vec3_json(l.sky_color)},
            {"ground_color", vec3_json(l.ground_color)},
            {"ambient_intensity", l.ambient_intensity},
            {"exposure", l.exposure}};
}

const char* to_string(render::DebugView v) {
    switch (v) {
    case render::DebugView::lit: return "lit";
    case render::DebugView::clusters: return "clusters";
    case render::DebugView::lod_levels: return "lod_levels";
    case render::DebugView::triangles: return "triangles";
    }
    return "lit";
}

render::DebugView debug_view_from(const std::string& name) {
    for (const auto v : {render::DebugView::lit, render::DebugView::clusters,
                         render::DebugView::lod_levels, render::DebugView::triangles}) {
        if (name == to_string(v)) return v;
    }
    return render::DebugView::lit;
}

Json render_settings_json(const EditorState& state) {
    const render::RenderStats& s = state.scene_stats;
    return {{"lod_error_pixels", state.render_settings.lod_error_pixels},
            {"debug_view", to_string(state.render_settings.debug_view)},
            {"last_frame",
             {{"instances", s.instances}, {"draws", s.draws}, {"clusters", s.clusters},
              {"triangles", s.triangles}}}};
}

Json camera_json(const EditorCamera& c) {
    return {{"position", dvec3_json(c.position)},
            {"yaw", c.yaw_degrees},
            {"pitch", c.pitch_degrees},
            {"vertical_fov_degrees", c.vertical_fov_degrees},
            {"fly_speed", c.speed}};
}

const char* const k_ids_exhausted =
    "every entity id has been used (the scene's next id is at the limit, 2^53 - 1); no more "
    "entities can be added";

// Replaces the whole scene, undoably (new_scene, load_scene).
void swap_scene(EditorState& state, world::Scene next, CommandContext& context) {
    auto before = std::make_shared<world::Scene>(state.scene);
    auto after = std::make_shared<world::Scene>(std::move(next));
    const u64 before_generation = state.scene_generation;
    const u64 after_generation = state.next_scene_generation++;
    state.scene = *after;
    state.scene_generation = after_generation;
    state.selected = 0;
    // Not guarded: a swap replaces whatever scene is there, and restores the
    // generation its neighbours in the history were recorded against.
    context.record({[&state, before, before_generation] {
                        state.scene = *before;
                        state.scene_generation = before_generation;
                        state.selected = 0;
                    },
                    [&state, after, after_generation] {
                        state.scene = *after;
                        state.scene_generation = after_generation;
                        state.selected = 0;
                    }});
}

std::filesystem::path utf8_path(const std::string& path) {
    return std::filesystem::path(std::u8string(path.begin(), path.end()));
}

} // namespace

ai::Change scene_change(EditorState& state, std::function<void()> undo,
                        std::function<void()> redo) {
    const u64 generation = state.scene_generation;
    return {[&state, generation, undo = std::move(undo)] {
                if (state.scene_generation == generation) undo();
            },
            [&state, generation, redo = std::move(redo)] {
                if (state.scene_generation == generation) redo();
            }};
}

void register_scene_commands(ai::CommandRegistry& registry, EditorState& state) {
    const Json properties = entity_properties();

    add(registry,
        {"create_entity",
         "Adds one entity (a shape with a transform and material) to the scene and returns it "
         "with its id. Every field is optional. Undoable. For many entities use "
         "create_entities.",
         Json{{"type", "object"}, {"additionalProperties", false}, {"properties", properties}},
         false,
         [&state](const Json& args, CommandContext& context) {
             Entity e;
             if (std::string error = new_entity(args, state, e); !error.empty()) {
                 return CommandResult::failure(error);
             }
             const EntityId id = state.scene.add(e);
             if (id == 0) {
                 return CommandResult::failure(k_ids_exhausted);
             }
             Entity stored = *state.scene.find(id);
             context.record(scene_change(
                 state,
                 [&state, id] {
                     state.scene.remove(id);
                     if (state.selected == id) state.selected = 0;
                 },
                 [&state, stored] {
                     if (state.scene.find(stored.id) == nullptr) state.scene.add(stored);
                 }));
             return CommandResult::success(entity_json(stored));
         }});

    add(registry,
        {"create_entities",
         "Adds many entities in one step (one undo), e.g. a whole street or forest. Each item "
         "takes the same fields as create_entity. Returns their ids in order.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"entities"}},
              {"properties",
               {{"entities",
                 {{"type", "array"},
                  {"minItems", 1},
                  {"maxItems", k_max_batch},
                  {"items",
                   {{"type", "object"}, {"additionalProperties", false},
                    {"properties", properties}}}}}}}},
         false,
         [&state](const Json& args, CommandContext& context) {
             auto created = std::make_shared<std::vector<Entity>>();
             Json ids = Json::array();
             const auto undo_added = [&state, &created] {
                 std::vector<EntityId> added;
                 for (const Entity& e : *created) added.push_back(e.id);
                 state.scene.remove_many(added); // all or nothing
             };
             usize item_index = 0;
             for (const Json& item : args["entities"]) {
                 Entity e;
                 if (std::string error = new_entity(item, state, e); !error.empty()) {
                     undo_added();
                     return CommandResult::failure("entities[" + std::to_string(item_index) +
                                                   "]: " + error);
                 }
                 ++item_index;
                 const EntityId id = state.scene.add(std::move(e));
                 if (id == 0) {
                     undo_added();
                     return CommandResult::failure(k_ids_exhausted);
                 }
                 created->push_back(*state.scene.find(id));
                 ids.push_back(id);
             }
             auto created_ids = std::make_shared<std::vector<EntityId>>();
             for (const Entity& e : *created) {
                 created_ids->push_back(e.id);
             }
             context.record(scene_change(
                 state,
                 [&state, created_ids] {
                     state.scene.remove_many(*created_ids);
                     state.selected = state.scene.find(state.selected) ? state.selected : 0;
                 },
                 [&state, created] {
                     for (const Entity& e : *created) {
                         if (state.scene.find(e.id) == nullptr) state.scene.add(e);
                     }
                 }));
             return CommandResult::success({{"ids", std::move(ids)}});
         }});

    Json set_properties = properties;
    set_properties["id"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 9007199254740991.0}};
    add(registry,
        {"set_entity",
         "Changes fields of one entity; fields left out stay as they are (for rotation, each "
         "of pitch, yaw and roll). Returns the entity. Undoable.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"id"}},
              {"properties", set_properties}},
         false,
         [&state](const Json& args, CommandContext& context) {
             const EntityId id = args["id"].get<EntityId>();
             Entity* e = state.scene.find(id);
             if (e == nullptr) {
                 return CommandResult::failure("no entity with id " + std::to_string(id) +
                                               "; list_entities shows them");
             }
             const Entity before = *e;
             Entity after = before; // a refused change leaves the entity alone
             if (std::string error = apply_fields(args, after, state); !error.empty()) {
                 return CommandResult::failure(error);
             }
             *e = after;
             const auto set = [&state](const Entity& value) {
                 if (Entity* target = state.scene.find(value.id)) {
                     *target = value;
                 }
             };
             context.record(
                 scene_change(state, [set, before] { set(before); }, [set, after] { set(after); }));
             return CommandResult::success(entity_json(after));
         }});

    add(registry,
        {"delete_entities", "Removes entities by id, in one undo step.",
         Json::parse(R"({"type": "object", "additionalProperties": false, "required": ["ids"],
             "properties": {"ids": {"type": "array", "minItems": 1, "maxItems": 10000,
                 "items": {"type": "integer", "minimum": 1, "maximum": 9007199254740991}}}})"),
         false,
         [&state](const Json& args, CommandContext& context) {
             // All or nothing: every id must exist (listing one twice is fine).
             auto ids = std::make_shared<std::vector<EntityId>>();
             for (const Json& id_json : args["ids"]) {
                 const EntityId id = id_json.get<EntityId>();
                 if (state.scene.find(id) == nullptr) {
                     return CommandResult::failure("no entity with id " + std::to_string(id));
                 }
                 ids->push_back(id);
             }
             auto removed = std::make_shared<std::vector<world::Scene::Removed>>(
                 state.scene.remove_many(*ids)); // one pass, even for a whole forest
             if (state.scene.find(state.selected) == nullptr) {
                 state.selected = 0;
             }
             context.record(scene_change(
                 state,
                 [&state, removed] {
                     std::vector<world::Scene::Removed> back;
                     for (const auto& r : *removed) {
                         if (state.scene.find(r.entity.id) == nullptr) back.push_back(r);
                     }
                     state.scene.restore_many(std::move(back));
                 },
                 [&state, ids] {
                     state.scene.remove_many(*ids);
                     if (state.scene.find(state.selected) == nullptr) state.selected = 0;
                 }));
             return CommandResult::success({{"deleted", removed->size()}});
         }});

    add(registry,
        {"get_entity", "Returns every field of one entity.",
         Json::parse(R"({"type": "object", "additionalProperties": false, "required": ["id"],
             "properties": {"id": {"type": "integer", "minimum": 1, "maximum": 9007199254740991}}})"),
         true,
         [&state](const Json& args, CommandContext&) {
             const EntityId id = args["id"].get<EntityId>();
             const Entity* e = state.scene.find(id);
             if (e == nullptr) {
                 return CommandResult::failure("no entity with id " + std::to_string(id));
             }
             return CommandResult::success(entity_json(*e));
         }});

    add(registry,
        {"list_entities",
         "Lists the scene's entities in outliner order: id, name, shape and position (all "
         "fields with full: true).",
         Json::parse(R"({"type": "object", "additionalProperties": false, "properties": {
             "full": {"type": "boolean"},
             "offset": {"type": "integer", "minimum": 0, "maximum": 100000000},
             "limit": {"type": "integer", "minimum": 1, "maximum": 10000,
                       "description": "At most this many (default 500)."}}})"),
         true,
         [&state](const Json& args, CommandContext&) {
             const bool full = args.value("full", false);
             const usize offset = args.value("offset", usize{0});
             const usize limit = args.value("limit", usize{500});
             const auto& all = state.scene.entities();
             Json list = Json::array();
             for (usize i = offset; i < all.size() && list.size() < limit; ++i) {
                 const Entity& e = all[i];
                 list.push_back(full ? entity_json(e)
                                     : Json{{"id", e.id},
                                            {"name", e.name},
                                            {"shape", world::to_string(e.shape)},
                                            {"position", dvec3_json(e.transform.position)}});
             }
             return CommandResult::success({{"count", all.size()}, {"entities", std::move(list)}});
         }});

    add(registry,
        {"get_lighting", "Returns the scene's sun, sky and ground light and exposure.",
         Json{{"type", "object"}, {"additionalProperties", false}}, true,
         [&state](const Json&, CommandContext&) {
             return CommandResult::success(lighting_json(state.scene.lighting));
         }});

    add(registry,
        {"set_lighting",
         "Changes the scene lighting; fields left out stay as they are. The sun is placed by "
         "elevation (0 horizon, 90 overhead, below 0 is night) and compass azimuth (0 north = "
         "-Z, 90 east = +X). sky_color is also the background. Undoable.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"properties",
               {{"sun_elevation_degrees", number(-90, 90, "Height of the sun.")},
                {"sun_azimuth_degrees", number(-360, 360, "Compass direction of the sun.")},
                {"sun_color", triple(0, 1, "Linear RGB.")},
                {"sun_intensity", number(0, 100, "Multiplies sun_color; about 3 is daylight.")},
                {"sky_color", triple(0, 1, "Linear RGB: the background and light from above.")},
                {"ground_color", triple(0, 1, "Linear RGB light bounced from below.")},
                {"ambient_intensity", number(0, 100, "Multiplies sky and ground light.")},
                {"exposure", number(0.01, 100, "Camera brightness before tone mapping.")}}}},
         false,
         [&state](const Json& args, CommandContext& context) {
             const world::Lighting before = state.scene.lighting;
             world::Lighting& l = state.scene.lighting;
             l.sun_elevation_degrees = args.value("sun_elevation_degrees", l.sun_elevation_degrees);
             l.sun_azimuth_degrees = args.value("sun_azimuth_degrees", l.sun_azimuth_degrees);
             if (args.contains("sun_color")) l.sun_color = vec3_from(args["sun_color"]);
             l.sun_intensity = args.value("sun_intensity", l.sun_intensity);
             if (args.contains("sky_color")) l.sky_color = vec3_from(args["sky_color"]);
             if (args.contains("ground_color")) l.ground_color = vec3_from(args["ground_color"]);
             l.ambient_intensity = args.value("ambient_intensity", l.ambient_intensity);
             l.exposure = args.value("exposure", l.exposure);
             const world::Lighting after = l;
             context.record(
                 scene_change(state, [&state, before] { state.scene.lighting = before; },
                              [&state, after] { state.scene.lighting = after; }));
             return CommandResult::success(lighting_json(after));
         }});

    add(registry,
        {"new_scene",
         "Replaces the scene with an empty one with default lighting. Undoable (undo brings "
         "the old scene back).",
         Json{{"type", "object"}, {"additionalProperties", false}}, false,
         [&state](const Json&, CommandContext& context) {
             world::Scene empty;
             // Ids keep counting up, so undo history never confuses two entities.
             empty.reserve_ids_below(state.scene.next_id());
             swap_scene(state, std::move(empty), context);
             return CommandResult::success({{"entities", 0}});
         }});

    add(registry,
        {"save_scene",
         "Saves the scene (entities and lighting) as a JSON text file, by convention "
         "<name>.scene.json. Relative paths are from the editor's working folder. Overwrites.",
         Json::parse(R"({"type": "object", "additionalProperties": false, "required": ["path"],
             "properties": {"path": {"type": "string", "minLength": 1, "maxLength": 1024}}})"),
         false,
         [&state](const Json& args, CommandContext&) {
             const std::filesystem::path path = utf8_path(args["path"].get<std::string>());
             const std::string text = world::save_scene(state.scene);
             std::ofstream file(path, std::ios::binary);
             file.write(text.data(), static_cast<std::streamsize>(text.size()));
             file.close();
             if (!file) {
                 return CommandResult::failure("could not write " + path_to_utf8(path));
             }
             std::error_code error;
             return CommandResult::success(
                 {{"saved_to", path_to_utf8(std::filesystem::absolute(path, error))},
                  {"entities", state.scene.size()}});
         }});

    add(registry,
        {"load_scene",
         "Replaces the scene with one saved by save_scene. Undoable. Entity ids are the ones "
         "saved in the file, so ids from before the load may now name other entities. Fails, "
         "changing nothing, if the file is missing or not a valid scene (the error says where).",
         Json::parse(R"({"type": "object", "additionalProperties": false, "required": ["path"],
             "properties": {"path": {"type": "string", "minLength": 1, "maxLength": 1024}}})"),
         false,
         [&state](const Json& args, CommandContext& context) {
             const std::filesystem::path path = utf8_path(args["path"].get<std::string>());
             std::ifstream file(path, std::ios::binary);
             if (!file) {
                 return CommandResult::failure("could not open " + path_to_utf8(path));
             }
             std::stringstream text;
             text << file.rdbuf();
             world::Scene loaded;
             if (const auto error = world::load_scene(text.str(), loaded)) {
                 return CommandResult::failure(path_to_utf8(path) + ": " + error->message);
             }
             // Meshes it uses are imported now; one that fails leaves its
             // entities invisible until it is fixed, but the scene loads.
             Json mesh_errors = Json::array();
             if (state.meshes != nullptr) {
                 for (const Entity& e : loaded.entities()) {
                     if (e.shape == world::Shape::mesh && state.meshes->find(e.mesh) == nullptr) {
                         const MeshLibrary::Loaded m = state.meshes->load(e.mesh);
                         if (m.mesh == nullptr && mesh_errors.size() < 100) {
                             bool listed = false;
                             for (const Json& x : mesh_errors) listed = listed || x["path"] == e.mesh;
                             if (!listed) mesh_errors.push_back({{"path", e.mesh}, {"error", m.error}});
                         }
                     }
                 }
             }
             const usize count = loaded.size();
             swap_scene(state, std::move(loaded), context);
             Json result = {{"entities", count}};
             if (!mesh_errors.empty()) result["mesh_errors"] = std::move(mesh_errors);
             return CommandResult::success(std::move(result));
         }});

    add(registry,
        {"import_mesh",
         "Imports a glTF (.gltf with its buffers) or .glb file and builds its cluster LOD, so "
         "entities can draw it (create_entity with mesh). Returns its triangle and cluster "
         "counts. Already-imported files return at once unless reload is true (to pick up "
         "changes). Large files take a few seconds. Materials and textures are not imported "
         "yet. Not undoable: it loads an asset and changes no entity.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"path"}},
              {"properties",
               {{"path", {{"type", "string"}, {"minLength", 1}, {"maxLength", world::k_max_mesh_path_bytes}}},
                {"reload", {{"type", "boolean"}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.meshes == nullptr) {
                 return CommandResult::failure("meshes cannot be imported in this editor");
             }
             const std::string path = args["path"].get<std::string>();
             const MeshLibrary::Loaded m = args.value("reload", false) ? state.meshes->reload(path)
                                                                        : state.meshes->load(path);
             if (m.mesh == nullptr) {
                 return CommandResult::failure(m.error);
             }
             return CommandResult::success(mesh_json(*m.mesh));
         }});

    add(registry,
        {"list_meshes", "Lists the imported meshes with their triangle and cluster counts.",
         Json{{"type", "object"}, {"additionalProperties", false}}, true,
         [&state](const Json&, CommandContext&) {
             Json list = Json::array();
             if (state.meshes != nullptr) {
                 for (const auto& m : state.meshes->meshes()) list.push_back(mesh_json(*m));
             }
             return CommandResult::success({{"meshes", std::move(list)}});
         }});

    add(registry,
        {"get_render_settings",
         "Returns the LOD error (pixels) and debug view, plus last frame's counts of instances, "
         "draws, clusters and triangles.",
         Json{{"type", "object"}, {"additionalProperties", false}}, true,
         [&state](const Json&, CommandContext&) {
             return CommandResult::success(render_settings_json(state));
         }});

    add(registry,
        {"set_render_settings",
         "Changes how the viewport draws. lod_error_pixels: how much simplification may show "
         "(1 = under a pixel; lower is sharper and slower). debug_view: lit (normal), clusters "
         "(each cluster coloured), lod_levels (red = full detail, then yellow, green, cyan, "
         "blue...) or triangles. View state: not undoable.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"properties",
               {{"lod_error_pixels", number(0.1, 64, "Pixels of error allowed. Default 1.")},
                {"debug_view", {{"enum", {"lit", "clusters", "lod_levels", "triangles"}}}}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             render::RenderSettings& r = state.render_settings;
             r.lod_error_pixels = args.value("lod_error_pixels", r.lod_error_pixels);
             if (args.contains("debug_view")) {
                 r.debug_view = debug_view_from(args["debug_view"].get<std::string>());
             }
             return CommandResult::success(render_settings_json(state));
         }});

    add(registry,
        {"get_camera",
         "Returns the viewport camera: position, yaw (0 looks north along -Z, 90 west), pitch "
         "(up positive) and field of view.",
         Json{{"type", "object"}, {"additionalProperties", false}}, true,
         [&state](const Json&, CommandContext&) {
             return CommandResult::success(camera_json(state.camera));
         }});

    add(registry,
        {"set_camera",
         "Moves the viewport camera. Give look_at to aim it at a point (easiest), or yaw and "
         "pitch. Fields left out stay as they are. View state, like scrolling: not undoable.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"properties",
               {{"position", triple(-1e8, 1e8, "Metres [x, y, z].")},
                {"look_at", triple(-1e8, 1e8, "Aim at this point [x, y, z], after moving.")},
                {"yaw", number(-360, 360, "Degrees; 0 looks along -Z, 90 along -X.")},
                {"pitch", number(-89, 89, "Degrees; up is positive.")},
                {"vertical_fov_degrees", number(10, 120, "Field of view.")},
                {"fly_speed", number(0.1, 10000, "Metres per second when flying.")}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             // Worked out on a copy: a failure leaves the camera untouched.
             EditorCamera c = state.camera;
             if (args.contains("position")) c.position = dvec3_from(args["position"]);
             c.yaw_degrees = args.value("yaw", c.yaw_degrees);
             c.pitch_degrees = args.value("pitch", c.pitch_degrees);
             c.vertical_fov_degrees = args.value("vertical_fov_degrees", c.vertical_fov_degrees);
             c.speed = args.value("fly_speed", c.speed);
             if (args.contains("look_at")) {
                 const DVec3 d = dvec3_from(args["look_at"]) - c.position;
                 const double flat = std::sqrt(d.x * d.x + d.z * d.z);
                 if (flat < 1e-9 && std::fabs(d.y) < 1e-9) {
                     return CommandResult::failure("look_at is the camera position");
                 }
                 c.yaw_degrees = radians_to_degrees(static_cast<float>(std::atan2(-d.x, -d.z)));
                 c.pitch_degrees = std::clamp(
                     radians_to_degrees(static_cast<float>(std::atan2(d.y, flat))), -89.0f, 89.0f);
             }
             state.camera = c;
             return CommandResult::success(camera_json(c));
         }});
}

} // namespace giant::editor
