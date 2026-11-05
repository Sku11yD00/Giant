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
enum class Shape { cube, sphere, plane, mesh };

// Longest Entity::mesh path, in bytes of UTF-8.
constexpr usize k_max_mesh_path_bytes = 1024;

const char* to_string(Shape shape);
std::optional<Shape> shape_from_string(std::string_view name);

struct Transform {
    DVec3 position{};  // metres; doubles so a 20 km world keeps millimetres
    Quat rotation{};   // unit quaternion
    Vec3 scale{1.0f, 1.0f, 1.0f};
};

// A physically based material (metal/roughness, as artists know it).
struct Material {
    Vec3 base_color{0.8f, 0.8f, 0.8f}; // linear, 0 to 1
    float roughness = 0.5f;             // 0 mirror to 1 matte
    float metallic = 0.0f;              // 0 or 1 for real materials
    Vec3 emissive{};                    // linear, added light; may exceed 1
};

struct Entity {
    EntityId id = 0;
    std::string name;
    Transform transform;
    Shape shape = Shape::cube;
    // A glTF/GLB file (UTF-8 path, relative to the project folder), drawn
    // when shape is `mesh`. Kept when the shape changes, so it can change back.
    std::string mesh;
    Material material;
    bool visible = true;
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

class Scene {
public:
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

    Entity* find(EntityId id);
    const Entity* find(EntityId id) const;
    // In creation order (the outliner's order).
    const std::vector<Entity>& entities() const { return m_entities; }
    usize size() const { return m_entities.size(); }
    void clear(); // entities only; lighting is kept

    Lighting lighting;

    // The id the next new entity gets.
    EntityId next_id() const { return m_next_id; }
    // New ids start at `next` or later (ids of deleted entities stay unused).
    void reserve_ids_below(EntityId next) { m_next_id = next > m_next_id ? next : m_next_id; }

private:
    void reindex(usize from);

    std::vector<Entity> m_entities;
    std::unordered_map<EntityId, usize> m_index; // id -> place in m_entities
    EntityId m_next_id = 1;
};

// JSON text, stable key order, with a trailing newline.
std::string save_scene(const Scene& scene);

struct LoadError {
    std::string message; // what is wrong and where, for people and the AI
};
// Replaces `out` only on success. Unknown keys are errors (typos must not
// silently drop data); missing optional keys take their defaults.
std::optional<LoadError> load_scene(std::string_view text, Scene& out);

} // namespace giant::world
