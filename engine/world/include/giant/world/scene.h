// The editable world: entities with a transform, a shape and a material,
// plus the scene's lighting. Plain data; the renderer gets a copy of what
// it needs each frame (it never includes this module).
//
// Scenes save as JSON text (format "giant-scene"), so people, git diffs and
// the AI can read them. Units: metres, degrees in the editor, +Y up,
// right-handed; colours are linear.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/world/cell.h"
#include "giant/world/damage.h"
#include "giant/world/terrain.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace giant::world {

// Stable for the life of the scene and saved with it. 0 is "none". At most
// k_max_entity_id, so ids survive JSON readers that use doubles.
using EntityId = u64;
constexpr EntityId k_max_entity_id = (u64{1} << 53) - 1;

// Built-in shapes are about one metre: a 1 m cube, a 1 m wide sphere, a 1 m
// square plane facing +Y. `mesh` draws the imported file in Entity::mesh.
// `kit` places every piece of the kit named in Entity::kit. `decal` stamps
// its material onto the surfaces inside its box (1 m, scaled), from above.
enum class Shape { cube, sphere, plane, mesh, kit, decal };

// Longest Entity::mesh path, in bytes of UTF-8.
constexpr usize k_max_mesh_path_bytes = 1024;
// Longest entity, kit or piece name, in bytes of UTF-8.
constexpr usize k_max_name_bytes = 128;
constexpr usize k_max_kit_pieces = 4096;

const char* to_string(Shape shape);
std::optional<Shape> shape_from_string(std::string_view name);

// What a decal stamps (render::DecalShape, by name in scenes).
enum class DecalShape { disc, rectangle, ring, splat, cracks };
const char* to_string(DecalShape shape);
std::optional<DecalShape> decal_shape_from_string(std::string_view name);

struct DecalSettings {
    DecalShape shape = DecalShape::disc;
    float opacity = 1.0f;  // 0 to 1
    float softness = 0.1f; // edge fade, 0 (crisp) to 1, a fraction of the stamp
    u32 seed = 0;          // varies splat and cracks
    bool operator==(const DecalSettings&) const = default;
};

struct Transform {
    DVec3 position{};  // metres; doubles so a 20 km world keeps millimetres
    Quat rotation{};   // unit quaternion
    Vec3 scale{1.0f, 1.0f, 1.0f};
    bool operator==(const Transform&) const = default;
};

// What a thing is made of, as a bullet meets it (giant/sim/ballistics.h):
// how hard it is to go through, whether shots glance off it. `standard`
// is a middling solid (unset); the ground is dirt.
enum class Surface : u8 { standard, flesh, wood, glass, dirt, foliage, brick, concrete, metal };
constexpr usize k_surface_count = 9;
const char* to_string(Surface surface);
std::optional<Surface> surface_from_string(std::string_view name);

// A physically based material (metal/roughness, as artists know it).
struct Material {
    Vec3 base_color{0.8f, 0.8f, 0.8f}; // linear, 0 to 1
    float roughness = 0.5f;             // 0 mirror to 1 matte
    float metallic = 0.0f;              // 0 or 1 for real materials
    Vec3 emissive{};                    // linear, added light; may exceed 1
    Surface surface = Surface::standard; // what bullets meet (not drawn)
    bool operator==(const Material&) const = default;
};

// How an entity takes part in physics (giant/sim): fixed collides and
// never moves (the ground's rocks, a standing house: the default), dynamic
// falls and is pushed (a crate, debris), none does not collide (decor).
// Decals and meshes never collide yet.
enum class BodyKind { fixed, dynamic, none };
const char* to_string(BodyKind body);
std::optional<BodyKind> body_from_string(std::string_view name);

// Heaviest a dynamic body may be (kilograms): a city block's worth.
constexpr float k_max_body_mass = 1.0e9f;

// The animation a skinned mesh entity plays (its file's clip, by name),
// looping or once, at a speed (1 normal; negative plays back). No clip:
// it stands in its rest pose. The editor plays it (view state may play
// something else for a while: play_animation).
constexpr float k_max_animation_speed = 100.0f;
struct EntityAnimation {
    std::string clip;
    float speed = 1.0f; // -k_max_animation_speed to k_max_animation_speed
    bool loop = true;
    bool operator==(const EntityAnimation&) const = default;
};

struct Entity {
    EntityId id = 0;
    std::string name;
    Transform transform;
    Shape shape = Shape::cube;
    // A glTF/GLB file (UTF-8 path, relative to the editor's working folder,
    // which is the project folder; not to the scene file), drawn
    // when shape is `mesh`. Kept when the shape changes, so it can change back.
    std::string mesh;
    // The kit drawn when shape is `kit` (kept when the shape changes).
    std::string kit;
    Material material; // unused by kits (their pieces have their own)
    DecalSettings decal; // used when shape is `decal`
    bool visible = true;
    BodyKind body = BodyKind::fixed;
    float mass = 0.0f; // kilograms, when dynamic; 0: from its size (as water)
    EntityAnimation animation; // when shape is `mesh` and the file is skinned (kept otherwise)
    bool operator==(const Entity&) const = default;
};

// One part of a kit, placed relative to the kit entity's transform.
struct KitPiece {
    std::string name;
    Shape shape = Shape::cube; // not `kit`: kits do not nest
    std::string mesh;
    Transform transform;
    Material material;
    DecalSettings decal;
    bool operator==(const KitPiece&) const = default;
};

// A reusable assembly of pieces (a modular wall, a house, a tree with its
// rocks): placed any number of times by kit entities, which all share its
// meshes and materials. Changing a kit changes every placement.
constexpr float k_default_kit_strength = 1500.0f; // a 100 kg rock at 15 m/s
constexpr float k_max_kit_strength = 1e9f;

struct Kit {
    std::string name; // unique in the scene
    std::vector<KitPiece> pieces;
    // Breakable (a building): while simulating, a piece struck hard enough
    // comes loose and falls, and so does whatever it held up (pieces that
    // touch are joined; those at the kit's bottom stand on the ground).
    bool breakable = false;
    // The blow a piece takes before it comes loose, newton seconds: about
    // the mass times the speed of what strikes it (0: k_default_kit_strength).
    float strength = 0.0f;
    bool operator==(const Kit&) const = default;
};

// Scatter layers (rules for strewing things over the ground; see
// giant/world/scatter.h for how they place).

// A scatter item that is a tree: while simulating, its trunk stands as a
// body things bump into, and struck hard enough (or by impact) it snaps at
// its foot and topples, falling as a body of its own onto what is there
// (other trees it knocks down in turn; buildings it is heavy enough to break).
// N s: a tree of its own size falling on it does not fell it (no chain through
// a whole forest); one much bigger, a building's wall, a giant's foot do.
constexpr float k_default_tree_strength = 8000.0f;
struct ScatterTree {
    float trunk_radius = 0.25f; // metres, at scale 1
    float height = 10.0f;       // metres, at scale 1: the trunk, foot to top (what falls)
    // The blow it takes before it snaps (newton seconds, at scale 1;
    // 0: k_default_tree_strength), grown with the trunk's cross-section
    // (the scale squared).
    float strength = 0.0f;
    bool operator==(const ScatterTree&) const = default;
};

// One kind of thing a layer places.
struct ScatterItem {
    Shape shape = Shape::cube; // not decal
    std::string mesh;          // shape mesh: a glTF file, as Entity::mesh
    std::string kit;           // shape kit: one of the scene's kits
    Material material;         // built-in shapes and meshes
    float weight = 1.0f;       // how often it is picked, against the layer's others (> 0)
    float scale_min = 1.0f;    // uniform scale, picked evenly between
    float scale_max = 1.0f;
    float align = 0.0f; // 0: upright; 1: along the ground's normal (rocks)
    float sink = 0.0f;  // metres into the ground, at scale 1 (roots on a slope)
    std::optional<ScatterTree> tree; // none: it never falls (rocks, grass)
    bool operator==(const ScatterItem&) const = default;
};

// Where a layer places, on the ground (x, z metres; from <= to).
struct ScatterArea {
    double from_x = 0.0;
    double from_z = 0.0;
    double to_x = 0.0;
    double to_z = 0.0;
    bool operator==(const ScatterArea&) const = default;
};

constexpr float k_min_scatter_spacing = 0.25f; // metres
constexpr float k_max_scatter_spacing = 1000.0f;
constexpr usize k_max_scatter_items = 64;
constexpr usize k_max_scatter_layers = 64;

struct ScatterLayer {
    std::string name; // unique in the scene
    std::vector<ScatterItem> items;
    float spacing = 4.0f; // metres between points, about
    float density = 1.0f; // the share of points kept (0 to 1)
    float jitter = 0.9f;  // 0: an even grid; 1: anywhere in its square
    // Clumps: a noise this many metres across (0: none) thins the points,
    // `clumping` (0 to 1) of the way to keeping only its high parts.
    float clump_size = 0.0f;
    float clumping = 0.0f;
    // The ground it may go on: height (metres) and slope (degrees from flat).
    float height_min = -k_max_terrain_height;
    float height_max = k_max_terrain_height;
    float slope_min = 0.0f;
    float slope_max = 90.0f;
    std::optional<ScatterArea> area; // none: wherever there is ground
    u32 seed = 1;
    bool visible = true;
    bool operator==(const ScatterLayer&) const = default;
};

// One sun plus sky and ground ambient light (a hemisphere): enough for a
// readable lit scene until the voxel GI arrives.
struct Lighting {
    // Where the sun is: elevation above the horizon and compass heading
    // (0 = north = -Z, 90 = east = +X).
    float sun_elevation_degrees = 50.0f;
    float sun_azimuth_degrees = 135.0f;
    Vec3 sun_color{1.0f, 0.95f, 0.88f};
    float sun_intensity = 3.0f; // multiplies sun_color
    Vec3 sky_color{0.15f, 0.34f, 0.71f};    // the background, and light from above
    Vec3 ground_color{0.22f, 0.2f, 0.18f};  // light bounced from below
    float ambient_intensity = 0.6f;         // multiplies sky and ground light
    float exposure = 1.0f;                  // camera brightness, before tone mapping
    bool operator==(const Lighting&) const = default;
};

// A unit vector pointing towards the sun.
Vec3 sun_direction(const Lighting& lighting);

// The weather (weather.h for presets, blending and the light it gives):
// clouds, wind, fog and haze. A scene with none keeps the flat sky colours
// of its Lighting; with one, the sky is an atmosphere the sun shines
// through, with clouds that drift on the wind and shade the ground.
struct Weather {
    float cloud_cover = 0.35f;      // 0 clear to 1 overcast: how much of the sky the clouds fill
    float cloud_density = 0.5f;     // 0 to 1: how thick they are (light and dark)
    float cloud_base = 1500.0f;     // metres above 0, 100 to 10000
    float cloud_thickness = 1500.0f; // metres, 100 to 10000
    float cloud_scale = 1.0f;       // 0.25 to 4: the clouds' size (1: about 3 km across)
    float wind_direction_degrees = 90.0f; // where it blows to, compass (0 = north = -Z, 90 = east = +X)
    float wind_speed = 10.0f;       // metres a second, 0 to 200 (moves the clouds)
    float fog_density = 0.0f;       // 0 to 1: at 1 the ground fog hides things about 150 m away
    float fog_height = 200.0f;      // metres, 1 to 10000: the fog thins by half each this much higher
    float haze = 1.0f;              // 0 to 10: dust and moisture in the air (paler sky, redder sun)
    float precipitation = 0.0f;     // 0 to 1: how hard it rains or snows
    float temperature = 10.0f;      // degrees Celsius, -50 to 50: snow below about 0, rain above 2
    // The ground as the scene starts (what has fallen before): 0 to 1. The
    // editor then wets, dries, covers and melts it as the weather goes on.
    float wetness = 0.0f;
    float snow_cover = 0.0f;
    bool operator==(const Weather&) const = default;
};

// What changed in a scene since its last take_changes().
struct SceneChanges {
    // Entities added, removed or handed out by Scene::modify, oldest first
    // (an id may be listed more than once, and may be gone by now).
    std::vector<EntityId> ids;
    // Everything may have changed: the list was cleared, or it grew too
    // long to be worth keeping.
    bool all = false;
};

class Scene {
public:
    Scene() = default;
    // A copy or a moved-to scene is a new scene: all changed, new identity.
    Scene(const Scene& other);
    Scene(Scene&& other) noexcept;
    Scene& operator=(const Scene& other);
    Scene& operator=(Scene&& other) noexcept;
    ~Scene() = default;

    // Adds `entity` and returns its id (0, adding nothing, once all 2^53 - 1
    // ids have been handed out). Id 0 gets a new id; a non-zero id
    // (restoring a deleted entity, loading) must be unused. `at` is its
    // place in the entity list (default: the end).
    EntityId add(Entity entity, std::optional<usize> at = std::nullopt);
    // Returns the removed entity and where it was, for undo.
    struct Removed {
        Entity entity;
        usize index = 0;
    };
    std::optional<Removed> remove(EntityId id);
    // Many at once, in one pass (a forest felled in one command). Ids that
    // are not in the scene are skipped. The result is in list order.
    std::vector<Removed> remove_many(std::span<const EntityId> ids);
    // Puts back what remove_many returned, each at its old place.
    void restore_many(std::vector<Removed> removed);

    const Entity* find(EntityId id) const;
    // The entity, to change: marks it changed (see take_changes), so call
    // it only to change it.
    Entity* modify(EntityId id);
    // In creation order (the outliner's order).
    const std::vector<Entity>& entities() const { return m_entities; }
    usize size() const { return m_entities.size(); }
    void clear(); // entities only; lighting, kits, terrain and scatter are kept

    // Hands over what changed since the last call (kits, lighting and the
    // meshes entities name are not tracked: compare those). A scene
    // made, copied or assigned starts as "all changed".
    SceneChanges take_changes();
    // What the next take_changes() would hand over, left there (for a
    // follower that must catch up between frames: a command's answer).
    const SceneChanges& untaken_changes() const { return m_changes; }
    // Tells apart scenes (a copy or an assignment is a new scene), so a
    // follower of take_changes knows when to start over.
    u64 identity() const { return m_identity.value; }

    Lighting lighting;
    // By name, in creation order. Names are unique (the commands and
    // load_scene keep them so).
    std::vector<Kit> kits;
    const Kit* find_kit(std::string_view name) const;
    Kit* find_kit(std::string_view name);
    // The ground (terrain.h): it tracks its own changes (Terrain::take_changes).
    Terrain terrain;
    // What is strewn over the ground (scatter.h), by name, in order (not
    // tracked: compare).
    std::vector<ScatterLayer> scatter;
    // None: the flat sky of `lighting` (not tracked: compare).
    std::optional<Weather> weather;

    // Damage kept (damage.h), by cell: copied, saved and loaded with the
    // scene, not undone by itself (commands record it). Setting a cell's
    // marks the kit placements its records name, before and after, as
    // changed (take_changes), so what follows them sees the change.
    const Damage& damage() const { return m_damage; }
    const CellDamage* damage_in(CellCoord cell) const;
    // Replaces the cell's damage (empty: none), sorted (sort_damage).
    // `hold_empty`: an empty one is held as an entry of its own, saying
    // "none here" (an undo for a cell streamed out: what the cell's file
    // holds then gives way to it when the cell comes back, as an entity
    // put back by an undo does).
    void set_damage(CellCoord cell, CellDamage damage, bool hold_empty = false);
    // The kept record of a kit placement (null: none); with `placed`, the
    // one made where it is placed now (two cells' files may name it: one
    // kept before it moved, stale).
    const KeptKit* kept_kit(EntityId id, const Transform* placed = nullptr) const;
    // Counts every set_damage (a follower looks over the damage only when it moved).
    u64 damage_edits() const { return m_damage_edits; }

    // The id the next new entity gets.
    EntityId next_id() const { return m_next_id; }
    // New ids start at `next` or later (ids of deleted entities stay unused).
    void reserve_ids_below(EntityId next) { m_next_id = next > m_next_id ? next : m_next_id; }

private:
    void reindex(usize from);
    void changed(EntityId id);

    // A new value on construction, copy and assignment.
    struct Identity {
        Identity();
        Identity(const Identity&) : Identity() {}
        Identity& operator=(const Identity&);
        u64 value = 0;
    };

    std::vector<Entity> m_entities;
    std::unordered_map<EntityId, usize> m_index; // id -> place in m_entities
    EntityId m_next_id = 1;
    Damage m_damage;
    std::unordered_multimap<EntityId, CellCoord> m_kept_kits; // the cells whose damage holds each kit's records
    u64 m_damage_edits = 0;
    SceneChanges m_changes{{}, true};
    Identity m_identity;
};

// JSON text, stable key order, with a trailing newline.
std::string save_scene(const Scene& scene);

struct LoadError {
    std::string message; // what is wrong and where, for people and the AI
};
// Replaces `out` only on success. Unknown keys are errors (typos must not
// silently drop data); missing optional keys take their defaults.
std::optional<LoadError> load_scene(std::string_view text, Scene& out);

// --- Worlds streamed by cell ----------------------------------------------------
//
// A world is a folder: world.json (WorldIndex: lighting, kits, the meshes
// its cells use, which cells have a file) and cells/<x>_<z>.cell.json, one
// per cell with entities. Each is text like a scene; the editor loads
// cells around the camera and unloads far ones (editor/src/world_streaming.h).
// A cell's file also holds its terrain tile, if it has one.

struct WorldIndex {
    EntityId next_id = 1; // one past the last id any cell holds
    Lighting lighting;
    std::vector<Kit> kits;
    std::vector<ScatterLayer> scatter;
    std::optional<Weather> weather;
    std::vector<std::string> meshes; // imported when the world opens
    std::vector<CellCoord> cells;    // cells that have a file
    // The ground: one sample spacing and material for every tile, and the
    // cells whose file holds a tile (all listed in `cells`).
    u32 terrain_samples = k_default_terrain_samples;
    TerrainMaterial terrain_material;
    std::vector<CellCoord> terrain_cells;
    // The far ground still owed by a save cut short (world/far_ground.h):
    // cells whose level-1 chunk is not written over their tile yet, and
    // level-1 chunks whose level-2 chunk is not. Empty after a full save.
    std::vector<CellCoord> far_owed;
    std::vector<CellCoord> far_owed_2;
};

// "<x>_<z>.cell.json", in the world's cells folder.
std::string cell_file_name(CellCoord cell);

// JSON text, stable order (meshes sorted, cells by z then x), trailing newline.
std::string save_world_index(const WorldIndex& world);
// Replaces `out` only on success; strict, like load_scene.
std::optional<LoadError> load_world_index(std::string_view text, WorldIndex& out);

// One cell's entities, sorted by id, so an unchanged cell saves the same
// text, and its terrain tile if it has one ("heights", rows of text as in
// a scene; samples x samples of them).
// Its kept damage too, if any ("damage": damage.h).
std::string save_cell(CellCoord cell, std::span<const Entity> entities,
                      const Terrain::Heights* heights = nullptr, const CellDamage* damage = nullptr);
// Replaces `out` and `heights` only on success (`heights` empty when the
// file holds no tile). Every entity must be in `cell` (by position) and
// ids unique in the file; a tile must be `terrain_samples` a side (the
// world's; 0: none may be there). Kits are not checked (the world's index holds them): an
// entity naming a missing kit just draws nothing.
// `damage` (when given) takes the cell's kept damage (empty: none; it is
// read and checked either way).
std::optional<LoadError> load_cell(std::string_view text, CellCoord cell, u32 terrain_samples,
                                   std::vector<Entity>& out, std::optional<Terrain::Heights>& heights,
                                   CellDamage* damage = nullptr);
// The same for a cell that must hold no terrain (terrain_samples 0).
std::optional<LoadError> load_cell(std::string_view text, CellCoord cell, std::vector<Entity>& out);

} // namespace giant::world
