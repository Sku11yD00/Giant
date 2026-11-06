// Scene, lighting and camera commands: how people (through the panels) and
// the AI (over MCP) build and light a scene. Every scene change is undoable;
// the camera is view state and is not.
#include "editor_commands.h"
#include "mesh_library.h"
#include "far_ground.h"
#include "world_streaming.h"

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
    case render::DebugView::lighting: return "lighting";
    }
    return "lit";
}

render::DebugView debug_view_from(const std::string& name) {
    for (const auto v : {render::DebugView::lit, render::DebugView::clusters,
                         render::DebugView::lod_levels, render::DebugView::triangles,
                         render::DebugView::voxels, render::DebugView::lighting}) {
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
            {"shadows", state.render_settings.shadows},
            {"shadow_pages_per_frame", state.render_settings.shadow_pages_per_frame},
            {"gi", state.render_settings.gi},
            {"gi_bricks_per_frame", state.render_settings.gi_bricks_per_frame},
            {"reflections", state.render_settings.reflections},
            {"screen_space_reflections", state.render_settings.screen_space_reflections},
            {"clouds", state.render_settings.clouds},
            {"cloud_steps", state.render_settings.cloud_steps},
            {"cloud_shadows", state.render_settings.cloud_shadows},
            {"last_frame",
             {{"instances", s.instances}, {"draws", s.draws}, {"post_draws", s.post_draws},
              {"clusters", s.clusters},
              {"triangles", s.triangles}, {"from_gpu", s.from_gpu},
              {"occluded_instances", s.occluded_instances},
              {"occluded_clusters", s.occluded_clusters}, {"overflow", s.overflow},
              {"materials", s.materials}, {"decals", s.decals}, {"decal_overflow", s.decal_overflow},
              {"cells", {{"count", s.cells}, {"changed", s.cells_changed}}},
              {"streaming",
               {{"pool_bytes", s.pool_bytes}, {"pool_used_bytes", s.pool_used_bytes},
                {"resident_groups", s.resident_groups}, {"total_groups", s.total_groups},
                {"streamed_in", s.streamed_in}, {"evicted", s.evicted}, {"waiting", s.waiting}}},
              {"voxels",
               {{"bricks", s.voxel_bricks}, {"capacity", s.voxel_capacity}, {"pending", s.voxel_pending},
                {"built", s.voxel_built}, {"missing", s.voxel_missing}, {"overflow", s.voxel_overflow},
                {"cpu_ms", s.voxel_cpu_ms}}},
              {"far_field", {{"on", s.far_field}, {"culled", s.far_culled}}},
              {"shadows",
               {{"on", s.shadows}, {"pages", s.shadow_pages}, {"capacity", s.shadow_capacity},
                {"drawn", s.shadow_drawn}, {"waiting", s.shadow_waiting}, {"missing", s.shadow_missing},
                {"draws", s.shadow_draws}}},
              {"gi",
               {{"on", s.gi}, {"lit", s.gi_lit}, {"cleared", s.gi_cleared}, {"waiting", s.gi_waiting}}},
              {"reflections", {{"voxel", s.reflections}, {"screen", s.screen_reflections}}},
              {"sky", {{"on", s.sky}, {"clouds", s.clouds}, {"cloud_shadows", s.cloud_shadows}}}}}};
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

// Replaces the whole scene, undoably (new_scene, load_scene, open_world).
// Each side keeps its scene with what a streamed world knew of it
// (`after_memo`: the next scene's, when it is a world's). Undo and redo
// keep the side they leave as it is then (with streaming's loads and
// unloads, which are not in the history, and its bookkeeping now), so the
// scene and the bookkeeping put back always match each other and the files.
void swap_scene(EditorState& state, world::Scene next, CommandContext& context,
                std::shared_ptr<const WorldStreaming::Memo> after_memo = nullptr) {
    struct Side {
        world::Scene scene;
        u64 generation = 0;
        std::shared_ptr<const WorldStreaming::Memo> memo;
    };
    WorldStreaming* world = state.world;
    // Only the scene it put there is kept: one a script put there since
    // (outside the history) is not this side's.
    const auto leave = [&state, world](Side& side) {
        if (state.scene_generation != side.generation) return;
        side.memo = world != nullptr && world->active(state.scene_generation) ? world->memo(state.scene) : nullptr;
        side.scene = state.scene;
        side.generation = state.scene_generation;
    };
    const auto enter = [&state, world](const Side& side) {
        state.scene = side.scene;
        state.scene_generation = side.generation;
        state.selected = 0;
        if (side.memo != nullptr) {
            world->restore(*side.memo, state.scene);
        }
    };
    auto before = std::make_shared<Side>();
    before->generation = state.scene_generation;
    leave(*before);
    auto after = std::make_shared<Side>(Side{std::move(next), state.next_scene_generation++, std::move(after_memo)});
    enter(*after);
    // Not guarded: a swap replaces whatever scene is there, and restores the
    // generation its neighbours in the history were recorded against.
    context.record({[leave, enter, before, after] {
                        leave(*after);
                        enter(*before);
                    },
                    [leave, enter, before, after] {
                        leave(*before);
                        enter(*after);
                    }});
}

// One spelling per file, as entities made in the editor have.
void normalize_meshes(world::Scene& scene) {
    std::vector<EntityId> mesh_ids;
    for (const Entity& e : scene.entities()) {
        if (!e.mesh.empty()) mesh_ids.push_back(e.id);
    }
    for (const EntityId id : mesh_ids) {
        Entity* e = scene.modify(id);
        e->mesh = MeshLibrary::normalize(e->mesh);
    }
    for (world::Kit& kit : scene.kits) {
        for (world::KitPiece& p : kit.pieces) {
            if (!p.mesh.empty()) p.mesh = MeshLibrary::normalize(p.mesh);
        }
    }
    for (world::ScatterLayer& layer : scene.scatter) {
        for (world::ScatterItem& i : layer.items) {
            if (!i.mesh.empty()) i.mesh = MeshLibrary::normalize(i.mesh);
        }
    }
}

// The mesh files the scene's entities and kits draw (normalized, once each).
std::vector<std::string> meshes_used(const world::Scene& scene) {
    std::vector<std::string> out;
    for (const Entity& e : scene.entities()) {
        if (e.shape == world::Shape::mesh) out.push_back(MeshLibrary::normalize(e.mesh));
    }
    for (const world::Kit& kit : scene.kits) {
        for (const world::KitPiece& p : kit.pieces) {
            if (p.shape == world::Shape::mesh) out.push_back(MeshLibrary::normalize(p.mesh));
        }
    }
    for (const world::ScatterLayer& layer : scene.scatter) {
        for (const world::ScatterItem& i : layer.items) {
            if (i.shape == world::Shape::mesh) out.push_back(MeshLibrary::normalize(i.mesh));
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// Imports each file not imported yet; returns the ones that failed (at most 100).
Json import_meshes(EditorState& state, const std::vector<std::string>& paths) {
    Json errors = Json::array();
    if (state.meshes == nullptr) {
        return errors;
    }
    for (const std::string& path : paths) {
        if (state.meshes->find(path) != nullptr) continue;
        const MeshLibrary::Loaded m = state.meshes->load(path);
        if (m.mesh == nullptr && errors.size() < 100) {
            errors.push_back({{"path", path}, {"error", m.error}});
        }
    }
    return errors;
}

Json streaming_json(const WorldStreaming& w, const FarGround* far) {
    const StreamingSettings& k = w.settings;
    Json out = {{"load_radius", k.load_radius},           {"unload_margin", k.unload_margin},
                {"entities_per_frame", k.entities_per_frame}, {"reads_in_flight", k.reads_in_flight},
                {"unload_every", k.unload_every},
                {"unloads_per_frame", k.unloads_per_frame}};
    if (far != nullptr) {
        out["far_radius_1"] = far->settings.radius[0];
        out["far_radius_2"] = far->settings.radius[1];
        // As used: level 1 at least load_radius + 5800 m, level 2 at least level 1.
        out["far_radius_used"] = {far->radius(1, w), far->radius(2, w)};
    }
    return out;
}

Json far_ground_json(const FarGround& far) {
    const FarGroundStats s = far.stats();
    Json levels = Json::array();
    for (usize l = 0; l < 2; ++l) {
        levels.push_back({{"files", s.files[l]},
                          {"loaded", s.loaded[l]},
                          {"meshes", s.meshes[l]},
                          {"building", s.building[l]},
                          {"drawn", s.drawn[l]}});
    }
    return {{"levels", std::move(levels)}, {"hidden", s.hidden}, {"reading", s.reading}, {"failed", s.failed}};
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
         "Removes a kit. Refused while entities or scatter layers place it (delete or change those first). Undoable.",
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
             for (const world::ScatterLayer& l : state.scene.scatter) {
                 for (const world::ScatterItem& i : l.items) {
                     if (i.shape == world::Shape::kit && i.kit == name) {
                         return CommandResult::failure("scatter layer \"" + l.name + "\" places \"" + name +
                                                       "\"; change or remove it first");
                     }
                 }
             }
             if (state.world != nullptr && state.world->active(state.scene_generation) &&
                 !state.world->all_loaded()) {
                 return CommandResult::failure("cells of the open world not loaded now may place \"" + name +
                                               "\": load them all (set_world_streaming load_radius) first");
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
             const Entity* found = state.scene.find(id);
             if (found == nullptr) {
                 return CommandResult::failure("no entity with id " + std::to_string(id) +
                                               "; list_entities shows them");
             }
             const Entity before = *found;
             Entity after = before; // a refused change leaves the entity alone
             if (std::string error = apply_fields(args, after, state); !error.empty()) {
                 return CommandResult::failure(error);
             }
             *state.scene.modify(id) = after;
             const auto set = [&state](const Entity& value) {
                 if (Entity* target = state.scene.modify(value.id)) {
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
             normalize_meshes(loaded);
             // Meshes it uses are imported now; one that fails leaves its
             // entities invisible until it is fixed, but the scene loads.
             Json mesh_errors = import_meshes(state, meshes_used(loaded));
             const usize count = loaded.size();
             swap_scene(state, std::move(loaded), context);
             Json result = {{"entities", count}};
             if (!mesh_errors.empty()) result["mesh_errors"] = std::move(mesh_errors);
             return CommandResult::success(std::move(result));
         }});

    const Json folder_schema = Json::parse(R"({"type": "object", "additionalProperties": false,
        "required": ["folder"],
        "properties": {"folder": {"type": "string", "minLength": 1, "maxLength": 1024}}})");
    add(registry,
        {"open_world",
         "Opens a world saved by save_world (a folder with world.json and a file per 64 m cell). "
         "The scene becomes the world: cells near the camera load in the background and far "
         "ones unload (get_world shows progress; set_world_streaming sets the radius). Undoable "
         "(undo brings the old scene back, unstreamed). Refused while the open world has unsaved "
         "edits, unless discard_unsaved is true. Imports the meshes the world lists.",
         Json::parse(R"({"type": "object", "additionalProperties": false, "required": ["folder"],
             "properties": {"folder": {"type": "string", "minLength": 1, "maxLength": 1024},
                            "discard_unsaved": {"type": "boolean"}}})"),
         false,
         [&state](const Json& args, CommandContext& context) {
             if (state.world == nullptr) {
                 return CommandResult::failure("worlds are not available in this editor");
             }
             WorldStreaming& w = *state.world;
             if (!args.value("discard_unsaved", false)) {
                 if (w.active(state.scene_generation)) {
                     if (const u32 n = w.unsaved_cells(state.scene); n > 0) {
                         return CommandResult::failure(
                             "the open world has " + std::to_string(n) +
                             " cells with unsaved edits: save_world first, or pass discard_unsaved: true");
                     }
                     if (w.unsaved_settings(state.scene)) {
                         return CommandResult::failure(
                             "the open world's lighting, kits, scatter or weather have unsaved changes: "
                             "save_world first, or pass discard_unsaved: true");
                     }
                 } else if (w.is_open()) {
                     return CommandResult::failure(
                         "the world in " + path_to_utf8(w.folder()) +
                         " is open, its scene replaced (undo brings it back): save it, close_world, or pass "
                         "discard_unsaved: true");
                 }
             }
             const std::filesystem::path folder = utf8_path(args["folder"].get<std::string>());
             world::Scene next;
             const u64 generation = state.next_scene_generation;
             if (const auto error = w.open(folder, next, generation)) {
                 return CommandResult::failure(*error);
             }
             // swap_scene takes the next generation: the one open() was given.
             auto memo = w.memo(next);
             swap_scene(state, std::move(next), context, std::move(memo));
             Json mesh_errors = import_meshes(state, state.world->index().meshes);
             std::error_code error;
             Json result = {{"folder", path_to_utf8(std::filesystem::absolute(folder, error))},
                            {"cells", state.world->index().cells.size()}};
             if (!mesh_errors.empty()) result["mesh_errors"] = std::move(mesh_errors);
             return CommandResult::success(std::move(result));
         }});

    add(registry,
        {"save_world",
         "Saves the open world: world.json and each 64 m cell whose entities or terrain tile changed (only "
         "those files are written; a cell left empty loses its file). With a folder and no "
         "world open, makes a new world there from the scene and opens it (the folder must "
         "not hold one). Cells with unsaved edits never unload, so nothing is lost by "
         "flying away; saving lets them go. Making a new world is refused while one is open "
         "(close_world first).",
         Json::parse(R"({"type": "object", "additionalProperties": false,
             "properties": {"folder": {"type": "string", "minLength": 1, "maxLength": 1024}}})"),
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.world == nullptr) {
                 return CommandResult::failure("worlds are not available in this editor");
             }
             WorldStreaming& w = *state.world;
             const bool active = w.active(state.scene_generation);
             WorldStreaming::Saved saved;
             if (args.contains("folder")) {
                 const std::filesystem::path folder = utf8_path(args["folder"].get<std::string>());
                 std::error_code a;
                 if (active && std::filesystem::equivalent(folder, w.folder(), a) && !a) {
                     if (state.far_ground != nullptr) state.far_ground->wait_reads(); // a file read stays open (Windows)
                     saved = w.save(state.scene, meshes_used(state.scene));
                 } else if (active) {
                     return CommandResult::failure(
                         "the open world is saved in " + path_to_utf8(w.folder()) +
                         "; saving it to another folder is not supported yet (copy the folder)");
                 } else {
                     saved = w.create(folder, state.scene, state.scene_generation, meshes_used(state.scene));
                 }
             } else if (active) {
                 if (state.far_ground != nullptr) state.far_ground->wait_reads(); // a file read stays open (Windows)
                 saved = w.save(state.scene, meshes_used(state.scene));
             } else {
                 return CommandResult::failure(
                     w.is_open() ? "the scene is no longer the open world's (new_scene or load_scene "
                                   "replaced it; undo brings it back); give a folder to make a new world"
                                 : "no world is open: give a folder to make one from the scene");
             }
             // Chunks written before a failure are on disk: the far ground takes them either way.
             if (state.far_ground != nullptr) state.far_ground->saved(saved);
             if (!saved.error.empty()) {
                 return CommandResult::failure(saved.error);
             }
             std::error_code error;
             return CommandResult::success({{"folder", path_to_utf8(std::filesystem::absolute(w.folder(), error))},
                                            {"cells_written", saved.cells_written},
                                            {"cells_removed", saved.cells_removed},
                                            {"cells", saved.cells},
                                            {"chunks_written", saved.chunks_written}});
         }});

    add(registry,
        {"close_world",
         "Stops streaming the open world. The scene stays as it is (the cells loaded now), as a "
         "plain scene; edits not saved with save_world are no longer saved to the world. Not "
         "undoable.",
         Json{{"type", "object"}, {"additionalProperties", false}}, false,
         [&state](const Json&, CommandContext&) {
             if (state.world == nullptr || !state.world->is_open()) {
                 return CommandResult::failure("no world is open");
             }
             state.world->close();
             return CommandResult::success({{"closed", true}});
         }});

    add(registry,
        {"get_world",
         "The open world's streaming: cells with a file, loaded, being read, parsed and waiting, "
         "failed (see the log), cells with unsaved edits, entities in the scene, cells whose file "
         "holds terrain (ground) and tiles loaded and unloaded, the far ground (coarse chunks of "
         "the terrain past the loaded cells: files, loaded, with a mesh, drawing, by level), and the "
         "streaming settings.",
         Json{{"type", "object"}, {"additionalProperties", false}}, true,
         [&state](const Json&, CommandContext&) {
             if (state.world == nullptr || !state.world->is_open()) {
                 return CommandResult::success({{"open", false}});
             }
             WorldStreaming& w = *state.world;
             const StreamingStats s = w.stats();
             return CommandResult::success(
                 {{"open", true},
                  {"active", w.active(state.scene_generation)},
                  {"folder", path_to_utf8(w.folder())},
                  {"cells", s.cells},
                  {"loaded", s.loaded},
                  {"reading", s.reading},
                  {"waiting", s.waiting},
                  {"failed", s.failed},
                  {"unsaved_cells", w.active(state.scene_generation) ? w.unsaved_cells(state.scene) : 0},
                  {"unsaved_settings", w.active(state.scene_generation) && w.unsaved_settings(state.scene)},
                  {"entities", state.scene.size()},
                  {"entities_loaded", s.entities_loaded},
                  {"entities_unloaded", s.entities_unloaded},
                  {"ids_changed", s.ids_changed},
                  {"ground", s.ground},
                  {"tiles_loaded", s.tiles_loaded},
                  {"tiles_unloaded", s.tiles_unloaded},
                  {"far_ground", state.far_ground != nullptr ? far_ground_json(*state.far_ground) : Json(nullptr)},
                  {"streaming", streaming_json(*state.world, state.far_ground)}});
         }});

    add(registry,
        {"set_world_streaming",
         "Sets how far around the camera world cells load (load_radius, metres on the ground), "
         "how much further they unload (unload_margin), how many entities join the scene per "
         "frame (entities_per_frame; more loads faster, fewer keeps frames even), how many cell "
         "files are read at once (reads_in_flight), how many frames pass between looks for "
         "cells to unload (unload_every), how many cells may unload in one frame (unloads_per_frame; "
         "the rest go in the next frames), and how far the far ground's level-1 (512 m) and level-2 "
         "(4096 m) chunks load (far_radius_1, far_radius_2; level 1 should reach past load_radius by "
         "about 5800 m). Not undoable: a view setting.",
         Json::parse(R"({"type": "object", "additionalProperties": false, "properties": {
             "load_radius": {"type": "number", "minimum": 64, "maximum": 8192},
             "unload_margin": {"type": "number", "minimum": 0, "maximum": 10000},
             "entities_per_frame": {"type": "integer", "minimum": 1, "maximum": 1000000},
             "reads_in_flight": {"type": "integer", "minimum": 1, "maximum": 256},
             "unload_every": {"type": "integer", "minimum": 1, "maximum": 10000},
             "unloads_per_frame": {"type": "integer", "minimum": 1, "maximum": 1000000},
             "far_radius_1": {"type": "number", "minimum": 0, "maximum": 65536},
             "far_radius_2": {"type": "number", "minimum": 0, "maximum": 131072}}})"),
         false,
         [&state](const Json& args, CommandContext&) {
             if (state.world == nullptr) {
                 return CommandResult::failure("worlds are not available in this editor");
             }
             StreamingSettings& k = state.world->settings;
             k.load_radius = args.value("load_radius", k.load_radius);
             k.unload_margin = args.value("unload_margin", k.unload_margin);
             k.entities_per_frame = args.value("entities_per_frame", k.entities_per_frame);
             k.reads_in_flight = args.value("reads_in_flight", k.reads_in_flight);
             k.unload_every = args.value("unload_every", k.unload_every);
             k.unloads_per_frame = args.value("unloads_per_frame", k.unloads_per_frame);
             if (state.far_ground != nullptr) {
                 FarGroundSettings& f = state.far_ground->settings;
                 f.radius[0] = args.value("far_radius_1", f.radius[0]);
                 f.radius[1] = args.value("far_radius_2", f.radius[1]);
             }
             return CommandResult::success(streaming_json(*state.world, state.far_ground));
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
         "shadows: sun shadows from virtual shadow pages (default on; lit view with GPU culling): pages "
         "are drawn when shading first needs them and again when what casts onto them changes, at most "
         "shadow_pages_per_frame a frame (default 64); last_frame.shadows has the pool and this frame's work. "
         "gi: global illumination (default on; lit views and the voxel view, voxel_world on): bounce and "
         "sky light from the lit voxel world; up to gi_bricks_per_frame voxel bricks (default 256) are lit "
         "again a frame, new and rebuilt ones first; last_frame.gi has this frame's work. "
         "reflections: glossy surfaces reflect along the mirror direction (default on; lit views): a cone "
         "through the lit voxel world as wide as the highlight (needs gi), and with screen_space_reflections "
         "(default on) a sharp reflection from the image itself where what it meets is on screen; "
         "last_frame.reflections says which are on. "
         "clouds: with a weather set (set_weather), its clouds marched at half resolution, cloud_steps "
         "steps a ray (default 48; fewer is faster and grainier), and cloud_shadows their shadows on the "
         "ground (both default on; last_frame.sky says what drew). "
         "debug_view lighting shows the lit view in plain white (the light alone). "
         "View state: not undoable.",
         Json{{"type", "object"},
              {"additionalProperties", false},
              {"properties",
               {{"lod_error_pixels", number(0.1, 64, "Pixels of error allowed. Default 1.")},
                {"debug_view", {{"enum", {"lit", "clusters", "lod_levels", "triangles", "voxels", "lighting"}}}},
                {"gpu_culling", {{"type", "boolean"}}},
                {"occlusion_culling", {{"type", "boolean"}}},
                {"mesh_shaders", {{"type", "boolean"}}},
                {"antialiasing", {{"type", "boolean"}}},
                {"specular_antialiasing", {{"type", "boolean"}}},
                {"voxel_world", {{"type", "boolean"}}},
                {"voxel_ms_per_frame", number(0, 100, "Milliseconds. Default 1.")},
                {"voxel_far_field", {{"type", "boolean"}}},
                {"far_field_distance", number(0, 1e6, "Metres from the camera. Default 3000.")},
                {"far_field_max_radius", number(0, 1e6, "Metres. Default 50.")},
                {"shadows", {{"type", "boolean"}}},
                {"shadow_pages_per_frame", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4096},
                                            {"description", "Pages drawn a frame at most. Default 64."}}},
                {"gi", {{"type", "boolean"}}},
                {"gi_bricks_per_frame", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4096},
                                         {"description", "Voxel bricks lit a frame at most. Default 256."}}},
                {"reflections", {{"type", "boolean"}}},
                {"screen_space_reflections", {{"type", "boolean"}}},
                {"clouds", {{"type", "boolean"}}},
                {"cloud_steps", {{"type", "integer"}, {"minimum", 8}, {"maximum", 256},
                                 {"description", "Steps a cloud ray. Default 48."}}},
                {"cloud_shadows", {{"type", "boolean"}}}}}},
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
             r.shadows = args.value("shadows", r.shadows);
             r.shadow_pages_per_frame = args.value("shadow_pages_per_frame", r.shadow_pages_per_frame);
             r.gi = args.value("gi", r.gi);
             r.gi_bricks_per_frame = args.value("gi_bricks_per_frame", r.gi_bricks_per_frame);
             r.reflections = args.value("reflections", r.reflections);
             r.screen_space_reflections = args.value("screen_space_reflections", r.screen_space_reflections);
             r.clouds = args.value("clouds", r.clouds);
             r.cloud_steps = args.value("cloud_steps", r.cloud_steps);
             r.cloud_shadows = args.value("cloud_shadows", r.cloud_shadows);
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
