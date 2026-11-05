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

Json decal_json(const world::DecalSettings& d) {
    return {{"shape", world::to_string(d.shape)}, {"opacity", d.opacity}, {"softness", d.softness},
            {"seed", d.seed}};
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
    if (!e.kit.empty()) {
        j["kit"] = e.kit;
    }
    const world::DecalSettings none;
    if (e.shape == world::Shape::decal || e.decal.shape != none.shape || e.decal.opacity != none.opacity ||
        e.decal.softness != none.softness || e.decal.seed != none.seed) {
        j["decal"] = decal_json(e.decal); // kept when the shape changes, as mesh and kit are
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
         {{"enum", {"cube", "sphere", "plane", "mesh", "kit", "decal"}},
          {"description", "cube (1 m), sphere (1 m wide), plane (1 m square facing up), mesh "
                          "(the file in `mesh`), kit (every piece of the kit named in `kit`; "
                          "create_kit makes kits), or decal (stamps its colour and roughness "
                          "onto the surfaces inside its 1 m box, scaled, from above; see "
                          "`decal`). Default cube."}}},
        {"kit",
         {{"type", "string"},
          {"minLength", 1},
          {"maxLength", world::k_max_name_bytes},
          {"description", "The kit to place (list_kits shows them). Giving it sets shape to "
                          "kit unless shape is given too. A kit entity's own material is "
                          "unused: its pieces have theirs."}}},
        {"decal",
         {{"type", "object"},
          {"additionalProperties", false},
          {"properties",
           {{"shape",
             {{"enum", {"disc", "rectangle", "ring", "splat", "cracks"}},
              {"description", "The stamp's mask. splat (mud, blood, scorch) and cracks "
                              "(impacts, footfalls) vary with seed. Default disc."}}},
            {"opacity", number(0, 1, "How much it covers the surface. Default 1.")},
            {"softness", number(0, 1, "Edge fade, as a fraction of the stamp. Default 0.1.")},
            {"seed", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4294967295.0},
                      {"description", "Varies splat and cracks. Default 0."}}}}},
          {"description", "Decal settings (shape decal). The box's scale is its size: x and z "
                          "the stamp, y how deep it reaches. Rotate it to stamp walls. Uses "
                          "base_color, roughness, metallic and emissive."}}},
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
    if (args.contains("kit")) {
        e.kit = args["kit"].get<std::string>();
        if (!args.contains("shape")) e.shape = world::Shape::kit;
    }
    if (args.contains("decal")) {
        const Json& d = args["decal"];
        if (d.contains("shape")) {
            e.decal.shape = world::decal_shape_from_string(d["shape"].get<std::string>()).value_or(e.decal.shape);
        }
        e.decal.opacity = d.value("opacity", e.decal.opacity);
        e.decal.softness = d.value("softness", e.decal.softness);
        e.decal.seed = d.value("seed", e.decal.seed);
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
    if (e.shape == world::Shape::kit && (args.contains("shape") || args.contains("kit"))) {
        if (e.kit.empty()) {
            return "shape kit needs a kit: give kit, e.g. \"house\" (list_kits shows them)";
        }
        if (state.scene.find_kit(e.kit) == nullptr) {
            return "no kit named \"" + e.kit + "\"; list_kits shows them, create_kit makes one";
        }
    }
    // Only a change of shape or file needs the file: an entity whose file
    // failed to load can still be moved, renamed or hidden.
    if (e.shape == world::Shape::mesh && (args.contains("shape") || args.contains("mesh"))) {
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
        } else if (out.shape == world::Shape::kit) {
            base = out.kit;
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
    case render::DebugView::voxels: return "voxels";
    }
    return "lit";
}

render::DebugView debug_view_from(const std::string& name) {
    for (const auto v : {render::DebugView::lit, render::DebugView::clusters,
                         render::DebugView::lod_levels, render::DebugView::triangles,
                         render::DebugView::voxels}) {
        if (name == to_string(v)) return v;
    }
    return render::DebugView::lit;
}

Json render_settings_json(const EditorState& state) {
    const render::RenderStats& s = state.scene_stats;
    return {{"lod_error_pixels", state.render_settings.lod_error_pixels},
            {"debug_view", to_string(state.render_settings.debug_view)},
            {"gpu_culling", state.render_settings.gpu_culling},
            {"occlusion_culling", state.render_settings.occlusion_culling},
            {"mesh_shaders", state.render_settings.mesh_shaders},
            {"antialiasing", state.render_settings.antialiasing},
            {"specular_antialiasing", state.render_settings.specular_antialiasing},
            {"voxel_world", state.render_settings.voxel_world},
            {"voxel_ms_per_frame", state.render_settings.voxel_ms_per_frame},
            {"voxel_far_field", state.render_settings.voxel_far_field},
            {"far_field_distance", state.render_settings.far_field_distance},
            {"far_field_max_radius", state.render_settings.far_field_max_radius},
            {"last_frame",
             {{"instances", s.instances}, {"draws", s.draws}, {"post_draws", s.post_draws},
              {"clusters", s.clusters},
              {"triangles", s.triangles}, {"from_gpu", s.from_gpu},
              {"occluded_instances", s.occluded_instances},
              {"occluded_clusters", s.occluded_clusters}, {"overflow", s.overflow},
              {"materials", s.materials}, {"decals", s.decals}, {"decal_overflow", s.decal_overflow},
              {"streaming",
               {{"pool_bytes", s.pool_bytes}, {"pool_used_bytes", s.pool_used_bytes},
                {"resident_groups", s.resident_groups}, {"total_groups", s.total_groups},
                {"streamed_in", s.streamed_in}, {"evicted", s.evicted}, {"waiting", s.waiting}}},
              {"voxels",
               {{"bricks", s.voxel_bricks}, {"capacity", s.voxel_capacity}, {"pending", s.voxel_pending},
                {"built", s.voxel_built}, {"missing", s.voxel_missing}, {"overflow", s.voxel_overflow},
                {"cpu_ms", s.voxel_cpu_ms}}},
              {"far_field", {{"on", s.far_field}, {"culled", s.far_culled}}}}}};
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


world::KitPiece piece_from(const Entity& e) {
    world::KitPiece p;
    p.name = e.name;
    p.shape = e.shape;
    p.mesh = e.shape == world::Shape::mesh ? e.mesh : std::string();
    p.transform = e.transform;
    p.material = e.material;
    p.decal = e.decal;
    return p;
}

Json piece_json(const world::KitPiece& p) {
    Entity e;
    e.name = p.name;
    e.shape = p.shape;
    e.mesh = p.mesh;
    e.transform = p.transform;
    e.material = p.material;
    e.decal = p.decal;
    Json j = entity_json(e);
    j.erase("id");
    j.erase("visible");
    return j;
}

usize placements(const world::Scene& scene, const std::string& kit) {
    usize n = 0;
    for (const Entity& e : scene.entities()) {
        if (e.shape == world::Shape::kit && e.kit == kit) ++n;
    }
    return n;
}

void register_kit_commands(ai::CommandRegistry& registry, EditorState& state, const Json& entity_props) {
    Json piece = entity_props;
    piece.erase("kit");
    piece.erase("visible");
    piece["shape"]["enum"] = {"cube", "sphere", "plane", "mesh", "decal"};
    piece["shape"]["description"] = "As for entities; kits do not nest.";
    piece["position"]["description"] = "Relative to the kit's origin, metres.";

    add(registry,
        {"create_kit",
         "Makes a kit: a reusable group of pieces (a modular wall, a house, a tree with its "
         "rocks) that kit entities place any number of times, sharing its meshes and materials "
         "(create_entities with {\"kit\": name} places it). Give the pieces, or take them "
         "from_entities (their positions become relative to origin, by default the first "
         "entity's). replace_entities swaps those entities for one placement of the new kit. "
         "replace: true changes an existing kit, and so every placement. Undoable.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"name"}},
              {"properties",
               {{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", world::k_max_name_bytes}}},
                {"pieces",
                 {{"type", "array"},
                  {"minItems", 1},
                  {"maxItems", world::k_max_kit_pieces},
                  {"items", {{"type", "object"}, {"additionalProperties", false}, {"properties", piece}}}}},
                {"from_entities",
                 {{"type", "array"},
                  {"minItems", 1},
                  {"maxItems", world::k_max_kit_pieces},
                  {"items", {{"type", "integer"}, {"minimum", 1}, {"maximum", 9007199254740991.0}}}}},
                {"origin", triple(-1e8, 1e8, "With from_entities: the kit's origin in the world.")},
                {"replace_entities",
                 {{"type", "boolean"},
                  {"description", "With from_entities: delete them and place the kit where they were."}}},
                {"replace", {{"type", "boolean"}, {"description", "Overwrite a kit of this name."}}}}}},
         false,
         [&state](const Json& args, CommandContext& context) {
             const std::string name = args["name"].get<std::string>();
             const bool replace = args.value("replace", false);
             if (state.scene.find_kit(name) != nullptr && !replace) {
                 return CommandResult::failure("a kit named \"" + name +
                                               "\" exists; give replace: true to change it");
             }
             if (args.contains("pieces") == args.contains("from_entities")) {
                 return CommandResult::failure("give either pieces or from_entities");
             }
             if (!args.contains("from_entities") &&
                 (args.contains("origin") || args.contains("replace_entities"))) {
                 return CommandResult::failure("origin and replace_entities go with from_entities");
             }
             world::Kit kit;
             kit.name = name;
             std::vector<EntityId> sources;
             DVec3 origin{};
             if (args.contains("pieces")) {
                 usize index = 0;
                 for (const Json& item : args["pieces"]) {
                     Entity e;
                     e.name = "";
                     if (std::string error = apply_fields(item, e, state); !error.empty()) {
                         return CommandResult::failure("pieces[" + std::to_string(index) + "]: " + error);
                     }
                     kit.pieces.push_back(piece_from(e));
                     ++index;
                 }
             } else {
                 for (const Json& id_json : args["from_entities"]) {
                     const EntityId id = id_json.get<EntityId>();
                     const Entity* e = state.scene.find(id);
                     if (e == nullptr) {
                         return CommandResult::failure("no entity with id " + std::to_string(id));
                     }
                     if (e->shape == world::Shape::kit) {
                         return CommandResult::failure("entity " + std::to_string(id) +
                                                       " is a kit placement; kits do not nest");
                     }
                     if (std::find(sources.begin(), sources.end(), id) == sources.end()) {
                         sources.push_back(id);
                     }
                 }
                 origin = args.contains("origin") ? dvec3_from(args["origin"])
                                                  : state.scene.find(sources[0])->transform.position;
                 for (const EntityId id : sources) {
                     world::KitPiece p = piece_from(*state.scene.find(id));
                     p.transform.position = p.transform.position - origin;
                     kit.pieces.push_back(std::move(p));
                 }
             }
             auto before = std::make_shared<std::vector<world::Kit>>(state.scene.kits);
             if (world::Kit* existing = state.scene.find_kit(name)) {
                 *existing = kit;
             } else {
                 state.scene.kits.push_back(kit);
             }
             auto after = std::make_shared<std::vector<world::Kit>>(state.scene.kits);
             // Swap the source entities for one placement.
             auto removed = std::make_shared<std::vector<world::Scene::Removed>>();
             auto placed = std::make_shared<Entity>();
             if (args.value("replace_entities", false)) {
                 placed->name = name + " " + std::to_string(state.scene.next_id());
                 placed->shape = world::Shape::kit;
                 placed->kit = name;
                 placed->transform.position = origin;
                 const EntityId id = state.scene.add(*placed);
                 if (id == 0) {
                     state.scene.kits = *before;
                     return CommandResult::failure(k_ids_exhausted);
                 }
                 *placed = *state.scene.find(id);
                 *removed = state.scene.remove_many(sources);
                 if (state.scene.find(state.selected) == nullptr) state.selected = 0;
             }
             context.record(scene_change(
                 state,
                 [&state, before, removed, placed] {
                     if (placed->id != 0) {
                         state.scene.remove(placed->id);
                         std::vector<world::Scene::Removed> back;
                         for (const auto& r : *removed) {
                             if (state.scene.find(r.entity.id) == nullptr) back.push_back(r);
                         }
                         state.scene.restore_many(std::move(back));
                         if (state.scene.find(state.selected) == nullptr) state.selected = 0;
                     }
                     state.scene.kits = *before;
                 },
                 [&state, after, removed, placed] {
                     state.scene.kits = *after;
                     if (placed->id != 0) {
                         std::vector<EntityId> ids;
                         for (const auto& r : *removed) ids.push_back(r.entity.id);
                         state.scene.remove_many(ids);
                         if (state.scene.find(placed->id) == nullptr) state.scene.add(*placed);
                         if (state.scene.find(state.selected) == nullptr) state.selected = 0;
                     }
                 }));
             Json result = {{"name", name}, {"pieces", kit.pieces.size()},
                            {"placements", placements(state.scene, name)}};
             if (placed->id != 0) result["placed"] = placed->id;
             return CommandResult::success(std::move(result));
         }});

    add(registry,
        {"delete_kit",
         "Removes a kit. Refused while entities place it (delete those first). Undoable.",
         Json::parse(R"({"type": "object", "additionalProperties": false, "required": ["name"],
             "properties": {"name": {"type": "string", "minLength": 1, "maxLength": 128}}})"),
         false,
         [&state](const Json& args, CommandContext& context) {
             const std::string name = args["name"].get<std::string>();
             if (state.scene.find_kit(name) == nullptr) {
                 return CommandResult::failure("no kit named \"" + name + "\"");
             }
             if (const usize n = placements(state.scene, name); n > 0) {
                 return CommandResult::failure(std::to_string(n) + " entities place \"" + name +
                                               "\"; delete them first");
             }
             auto before = std::make_shared<std::vector<world::Kit>>(state.scene.kits);
             std::erase_if(state.scene.kits, [&](const world::Kit& k) { return k.name == name; });
             auto after = std::make_shared<std::vector<world::Kit>>(state.scene.kits);
             context.record(scene_change(
                 state, [&state, before] { state.scene.kits = *before; },
                 [&state, after] { state.scene.kits = *after; }));
             return CommandResult::success({{"deleted", name}});
         }});

    add(registry,
        {"list_kits", "Lists the scene's kits: name, piece count and how many entities place each.",
         Json{{"type", "object"}, {"additionalProperties", false}}, true,
         [&state](const Json&, CommandContext&) {
             Json list = Json::array();
             for (const world::Kit& k : state.scene.kits) {
                 list.push_back({{"name", k.name}, {"pieces", k.pieces.size()},
                                 {"placements", placements(state.scene, k.name)}});
             }
             return CommandResult::success({{"kits", std::move(list)}});
         }});

    add(registry,
        {"get_kit", "Returns a kit's pieces, every field (positions relative to the kit's origin).",
         Json::parse(R"({"type": "object", "additionalProperties": false, "required": ["name"],
             "properties": {"name": {"type": "string", "minLength": 1, "maxLength": 128}}})"),
         true,
         [&state](const Json& args, CommandContext&) {
             const std::string name = args["name"].get<std::string>();
             const world::Kit* k = state.scene.find_kit(name);
             if (k == nullptr) {
                 return CommandResult::failure("no kit named \"" + name + "\"; list_kits shows them");
             }
             Json pieces = Json::array();
             for (const world::KitPiece& p : k->pieces) pieces.push_back(piece_json(p));
             return CommandResult::success({{"name", k->name}, {"pieces", std::move(pieces)},
                                            {"placements", placements(state.scene, k->name)}});
         }});
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
             // One spelling per file, as entities made in the editor have.
             std::vector<EntityId> mesh_ids;
             for (const Entity& e : loaded.entities()) {
                 if (!e.mesh.empty()) mesh_ids.push_back(e.id);
             }
             for (const EntityId id : mesh_ids) {
                 Entity* e = loaded.find(id);
                 e->mesh = MeshLibrary::normalize(e->mesh);
             }
             for (world::Kit& kit : loaded.kits) {
                 for (world::KitPiece& p : kit.pieces) {
                     if (!p.mesh.empty()) p.mesh = MeshLibrary::normalize(p.mesh);
                 }
             }
             if (state.meshes != nullptr) {
                 const auto import = [&](const std::string& mesh_path) {
                     if (state.meshes->find(mesh_path) != nullptr) return;
                     const MeshLibrary::Loaded m = state.meshes->load(mesh_path);
                     if (m.mesh == nullptr && mesh_errors.size() < 100) {
                         bool listed = false;
                         for (const Json& x : mesh_errors) listed = listed || x["path"] == mesh_path;
                         if (!listed) mesh_errors.push_back({{"path", mesh_path}, {"error", m.error}});
                     }
                 };
                 for (const Entity& e : loaded.entities()) {
                     if (e.shape == world::Shape::mesh) import(e.mesh);
                 }
                 for (const world::Kit& kit : loaded.kits) {
                     for (const world::KitPiece& p : kit.pieces) {
                         if (p.shape == world::Shape::mesh) import(p.mesh);
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
         "Returns the LOD error (pixels), debug view and culling switches, plus last frame's "
         "counts of instances, draws, clusters and triangles. With gpu_culling the counts come "
         "from the GPU a frame or two late (from_gpu true); occluded_* count what the first "
         "occlusion pass hid; overflow means a GPU list was full and geometry was dropped. "
         "streaming: GPU memory for cluster geometry (pool) and how many cluster groups are "
         "loaded; detail loads over a few frames as the camera moves (waiting > 0 meanwhile). "
         "voxels: the voxel copy of the scene around the camera (bricks of 8x8x8 voxels in the "
         "GPU pool, of capacity; pending counts work still to do, a little CPU time each frame; "
         "missing bricks did not fit in the pool and overflow means it is full).",
         Json{{"type", "object"}, {"additionalProperties", false}}, true,
         [&state](const Json&, CommandContext&) {
             return CommandResult::success(render_settings_json(state));
         }});

    add(registry,
        {"set_render_settings",
         "Changes how the viewport draws. lod_error_pixels: how much simplification may show "
         "(1 = under a pixel; lower is sharper and slower). debug_view: lit (normal), clusters "
         "(each cluster coloured), lod_levels (red = full detail, then yellow, green, cyan, "
         "blue...), triangles, or voxels (the voxel copy of the scene: 0.25 m voxels near the "
         "camera, doubling with each level out to about 30 km). gpu_culling: pick clusters on the GPU (default) or the CPU. "
         "occlusion_culling: skip what is hidden behind nearer geometry (GPU culling only). "
         "mesh_shaders: draw with mesh shaders when the GPU has them, else vertex shaders. "
         "antialiasing: smooth jagged edges (SMAA, no blur in motion; lit view only). "
         "specular_antialiasing: keep fine shiny detail from sparkling. "
         "voxel_world: keep the voxel copy of the scene up to date (default on). "
         "voxel_ms_per_frame: CPU time each frame may spend building it (default 1). "
         "voxel_far_field: past far_field_distance metres (default 3000) the voxel world stands in "
         "for instances whose bounding radius is at most far_field_max_radius metres (default 50); "
         "they are not drawn as meshes (lit view, voxel_world on; last_frame.far_field.culled counts them). "
         "View state: not undoable.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"properties",
               {{"lod_error_pixels", number(0.1, 64, "Pixels of error allowed. Default 1.")},
                {"debug_view", {{"enum", {"lit", "clusters", "lod_levels", "triangles", "voxels"}}}},
                {"gpu_culling", {{"type", "boolean"}}},
                {"occlusion_culling", {{"type", "boolean"}}},
                {"mesh_shaders", {{"type", "boolean"}}},
                {"antialiasing", {{"type", "boolean"}}},
                {"specular_antialiasing", {{"type", "boolean"}}},
                {"voxel_world", {{"type", "boolean"}}},
                {"voxel_ms_per_frame", number(0, 100, "Milliseconds. Default 1.")},
                {"voxel_far_field", {{"type", "boolean"}}},
                {"far_field_distance", number(0, 1e6, "Metres from the camera. Default 3000.")},
                {"far_field_max_radius", number(0, 1e6, "Metres. Default 50.")}}}},
         false,
         [&state](const Json& args, CommandContext&) {
             render::RenderSettings& r = state.render_settings;
             r.lod_error_pixels = args.value("lod_error_pixels", r.lod_error_pixels);
             if (args.contains("debug_view")) {
                 r.debug_view = debug_view_from(args["debug_view"].get<std::string>());
             }
             r.gpu_culling = args.value("gpu_culling", r.gpu_culling);
             r.occlusion_culling = args.value("occlusion_culling", r.occlusion_culling);
             r.mesh_shaders = args.value("mesh_shaders", r.mesh_shaders);
             r.antialiasing = args.value("antialiasing", r.antialiasing);
             r.specular_antialiasing = args.value("specular_antialiasing", r.specular_antialiasing);
             r.voxel_world = args.value("voxel_world", r.voxel_world);
             r.voxel_ms_per_frame = args.value("voxel_ms_per_frame", r.voxel_ms_per_frame);
             r.voxel_far_field = args.value("voxel_far_field", r.voxel_far_field);
             r.far_field_distance = args.value("far_field_distance", r.far_field_distance);
             r.far_field_max_radius = args.value("far_field_max_radius", r.far_field_max_radius);
             return CommandResult::success(render_settings_json(state));
         }});

    add(registry,
        {"probe_voxels",
         "What the voxel world holds at a world position, per clipmap level (finest first): "
         "outside (beyond the level's window), building (not built yet; the next coarser level "
         "stands in), empty or solid. Wait for last_frame.voxels.pending to reach 0 (see "
         "get_render_settings) for settled answers.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"required", {"position"}},
              {"properties", {{"position", triple(-1e8, 1e8, "Metres [x, y, z].")}}}},
         true,
         [&state](const Json& args, CommandContext&) {
             if (!state.probe_voxels) {
                 return CommandResult::failure("no voxel world in this editor");
             }
             Json levels = Json::array();
             for (const render::VoxelProbeLevel& l : state.probe_voxels(dvec3_from(args["position"]))) {
                 levels.push_back({{"level", l.level}, {"voxel_size", l.voxel_size}, {"state", l.state}});
             }
             return CommandResult::success({{"levels", std::move(levels)}});
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

    register_kit_commands(registry, state, properties);
}

} // namespace giant::editor
