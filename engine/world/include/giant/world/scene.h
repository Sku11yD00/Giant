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

// A physically based material (metal/roughness, as artists know it).
struct Material {
    Vec3 base_color{0.8f, 0.8f, 0.8f}; // linear, 0 to 1
    float roughness = 0.5f;             // 0 mirror to 1 matte
    float metallic = 0.0f;              // 0 or 1 for real materials
    Vec3 emissive{};                    // linear, added light; may exceed 1
    bool operator==(const Material&) const = default;
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
};

// A reusable assembly of pieces (a modular wall, a house, a tree with its
// rocks): placed any number of times by kit entities, which all share its
// meshes and materials. Changing a kit changes every placement.
struct Kit {
    std::string name; // unique in the scene
    std::vector<KitPiece> pieces;
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
};

// A unit vector pointing towards the sun.
Vec3 sun_direction(const Lighting& lighting);

// The world is cut into square cells on the ground (x and z), k_cell_size
// metres a side, cell (0, 0) spanning [0, k_cell_size) on both. An entity
// belongs to the cell its position is in, whatever its size. What draws,
// streams and is worked on together goes by cell, so a change to one
// entity costs its cell's work, not the world's.
constexpr double k_cell_size = 64.0;

struct CellCoord {
    i32 x = 0;
    i32 z = 0;
    bool operator==(const CellCoord&) const = default;
};
struct CellCoordHash {
    usize operator()(const CellCoord& c) const;
};
// The cell a point is in (clamped to the range i32 holds).
CellCoord cell_of(const DVec3& position);

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
    void clear(); // entities only; lighting and kits are kept

    // Hands over what changed since the last call (kits, lighting and the
    // meshes entities name are not tracked: compare those). A scene
    // made, copied or assigned starts as "all changed".
    SceneChanges take_changes();
    // Tells apart scenes (a copy or an assignment is a new scene), so a
    // follower of take_changes knows when to start over.
    u64 identity() const { return m_identity.value; }

    Lighting lighting;
    // By name, in creation order. Names are unique (the commands and
    // load_scene keep them so).
    std::vector<Kit> kits;
    const Kit* find_kit(std::string_view name) const;
    Kit* find_kit(std::string_view name);

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

struct WorldIndex {
    EntityId next_id = 1; // one past the last id any cell holds
    Lighting lighting;
    std::vector<Kit> kits;
    std::vector<std::string> meshes; // imported when the world opens
    std::vector<CellCoord> cells;    // cells that have a file
};

// "<x>_<z>.cell.json", in the world's cells folder.
std::string cell_file_name(CellCoord cell);

// JSON text, stable order (meshes sorted, cells by z then x), trailing newline.
std::string save_world_index(const WorldIndex& world);
// Replaces `out` only on success; strict, like load_scene.
std::optional<LoadError> load_world_index(std::string_view text, WorldIndex& out);

// One cell's entities, sorted by id, so an unchanged cell saves the same text.
std::string save_cell(CellCoord cell, std::span<const Entity> entities);
// Replaces `out` only on success. Every entity must be in `cell` (by
// position) and ids unique in the file. Kits are not checked (the world's
// index holds them): an entity naming a missing kit just draws nothing.
std::optional<LoadError> load_cell(std::string_view text, CellCoord cell, std::vector<Entity>& out);

} // namespace giant::world
