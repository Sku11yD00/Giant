// Scene, lighting and camera commands: how people (through the panels) and
// the AI (over MCP) build and light a scene. Every scene change is undoable;
// the camera is view state and is not.
#include "editor_commands.h"

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

Json entity_json(const Entity& e) {
    const EulerDegrees r = euler_from_quat(e.transform.rotation);
    return {{"id", e.id},
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
         {{"enum", {"cube", "sphere", "plane"}},
          {"description", "cube (1 m), sphere (1 m wide) or plane (1 m square facing up). "
                          "Default cube."}}},
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

void apply_fields(const Json& args, Entity& e) {
    if (args.contains("name")) e.name = args["name"].get<std::string>();
    if (args.contains("shape")) {
        e.shape = world::shape_from_string(args["shape"].get<std::string>()).value_or(e.shape);
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
}

Entity new_entity(const Json& args, const world::Scene& scene) {
    Entity e;
    apply_fields(args, e);
    if (e.name.empty()) {
        e.name = std::string(world::to_string(e.shape)) + " " + std::to_string(scene.next_id());
    }
    return e;
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

Json camera_json(const EditorCamera& c) {
    return {{"position", dvec3_json(c.position)},
            {"yaw", c.yaw_degrees},
            {"pitch", c.pitch_degrees},
            {"vertical_fov_degrees", c.vertical_fov_degrees},
            {"fly_speed", c.speed}};
}

// Replaces the whole scene, undoably (new_scene, load_scene).
void swap_scene(EditorState& state, world::Scene next, CommandContext& context) {
    auto before = std::make_shared<world::Scene>(state.scene);
    auto after = std::make_shared<world::Scene>(std::move(next));
    state.scene = *after;
    state.selected = 0;
    context.record({[&state, before] {
                        state.scene = *before;
                        state.selected = 0;
                    },
                    [&state, after] {
                        state.scene = *after;
                        state.selected = 0;
                    }});
}

std::filesystem::path utf8_path(const std::string& path) {
    return std::filesystem::path(std::u8string(path.begin(), path.end()));
}

} // namespace

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
             const Entity e = new_entity(args, state.scene);
             const EntityId id = state.scene.add(e);
             Entity stored = *state.scene.find(id);
             // Undo and redo tolerate a scene that changed outside the history
             // (a script's load_scene): they skip ids that are gone or taken.
             context.record({[&state, id] {
                                 state.scene.remove(id);
                                 if (state.selected == id) state.selected = 0;
                             },
                             [&state, stored] {
                                 if (state.scene.find(stored.id) == nullptr) state.scene.add(stored);
                             }});
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
             for (const Json& item : args["entities"]) {
                 const EntityId id = state.scene.add(new_entity(item, state.scene));
                 created->push_back(*state.scene.find(id));
                 ids.push_back(id);
             }
             auto created_ids = std::make_shared<std::vector<EntityId>>();
             for (const Entity& e : *created) {
                 created_ids->push_back(e.id);
             }
             context.record({[&state, created_ids] {
                                 state.scene.remove_many(*created_ids);
                                 state.selected = state.scene.find(state.selected) ? state.selected : 0;
                             },
                             [&state, created] {
                                 for (const Entity& e : *created) {
                                     if (state.scene.find(e.id) == nullptr) state.scene.add(e);
                                 }
                             }});
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
             apply_fields(args, *e);
             const Entity after = *e;
             const auto set = [&state](const Entity& value) {
                 if (Entity* target = state.scene.find(value.id)) {
                     *target = value;
                 }
             };
             context.record({[set, before] { set(before); }, [set, after] { set(after); }});
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
             context.record({[&state, removed] {
                                 std::vector<world::Scene::Removed> back;
                                 for (const auto& r : *removed) {
                                     if (state.scene.find(r.entity.id) == nullptr) back.push_back(r);
                                 }
                                 state.scene.restore_many(std::move(back));
                             },
                             [&state, ids] {
                                 state.scene.remove_many(*ids);
                                 if (state.scene.find(state.selected) == nullptr) state.selected = 0;
                             }});
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
             context.record({[&state, before] { state.scene.lighting = before; },
                             [&state, after] { state.scene.lighting = after; }});
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
             const usize count = loaded.size();
             swap_scene(state, std::move(loaded), context);
             return CommandResult::success({{"entities", count}});
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
