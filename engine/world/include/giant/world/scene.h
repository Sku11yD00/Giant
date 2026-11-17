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
#include "giant/world/effects.h"
#include "giant/world/material_maps.h"
#include "giant/world/terrain.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace giant::world {

// Stable for the life of the scene and saved with it. 0 is "none". No id
// is over k_max_any_entity_id, so ids survive JSON readers that use
// doubles. The scene's own entities (those it hands ids to, those files
// hold) have ids up to k_max_entity_id; the ids from k_person_entity_base
// on are those of the entities standing for people (Entity::person), each
// its person's own (person_entity_id).
using EntityId = u64;
using PersonId = u32; // a person's id (Person, below); 0: none
constexpr EntityId k_max_any_entity_id = (u64{1} << 53) - 1;
constexpr EntityId k_person_entity_base = u64{1} << 52;
constexpr EntityId k_max_entity_id = k_person_entity_base - 1;
constexpr EntityId person_entity_id(PersonId person) { return k_person_entity_base + person; }

// Built-in shapes are about one metre: a 1 m cube, a 1 m wide sphere, a 1 m
// square plane facing +Y. `mesh` draws the imported file in Entity::mesh.
// `kit` places every piece of the kit named in Entity::kit. `decal` stamps
// its material onto the surfaces inside its box (1 m, scaled), from above.
// `cloth` hangs a sheet of cloth (Entity::cloth) from its origin, which the
// wind blows about (giant/sim/cloth.h); entities only, not kit pieces or
// scatter items.
enum class Shape { cube, sphere, plane, mesh, kit, decal, cloth };

// Longest Entity::mesh path: k_max_mesh_path_bytes (material_maps.h).
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

// A hanging sheet of cloth (Shape::cloth): a flag, a banner, a curtain, a
// sail, washing on a line. It hangs in its entity's x-y plane from the
// entity's origin, `width` across (x) and `height` down (-y), its scale
// unused; what holds it is `pin`: its top edge (a banner, a curtain), its
// two top corners (washing on a line, an awning) or its left edge (a flag
// on a pole). Lighter cloth blows about more; stiffer cloth folds less.
enum class ClothPin { top, corners, left };
const char* to_string(ClothPin pin);
std::optional<ClothPin> cloth_pin_from_string(std::string_view name);

struct ClothSettings {
    float width = 1.0f;     // metres, 0.05 to 20
    float height = 1.5f;    // metres, 0.05 to 20
    ClothPin pin = ClothPin::top;
    float weight = 0.3f;    // kilograms per square metre, 0.02 (silk) to 5 (heavy canvas)
    float stiffness = 0.5f; // 0 (hangs in soft folds) to 1 (stiff)
    bool operator==(const ClothSettings&) const = default;
};

// A hanging thing that swings (SwingSettings::on): a lantern, a sign, a
// bell, a lamp on a chain. It hangs from a pivot `length` metres straight
// above its origin and swings about it as a pendulum: pushed by the
// weather's wind on its sides (its scale, as a box, for primitives and
// kits; its mesh's size for a mesh), knocked by shots and blasts, pulled
// back by gravity. `hinge` free swings any way (on a rope or a chain);
// `x` only about its own x axis (a sign on two hooks: it swings across its
// face). It goes at most `limit` degrees from hanging straight. View
// state as it moves: the scene keeps where it hangs at rest.
enum class SwingHinge { free, x };
const char* to_string(SwingHinge hinge);
std::optional<SwingHinge> swing_hinge_from_string(std::string_view name);

struct SwingSettings {
    bool on = false;
    float length = 0.5f;  // metres, 0.02 to 20: the pivot above the origin
    SwingHinge hinge = SwingHinge::free;
    float weight = 2.0f;  // kilograms, 0.05 to 1000
    float limit = 80.0f;  // degrees, 5 to 175
    bool operator==(const SwingSettings&) const = default;
};

// A light an entity or kit piece carries (a lamp, a lantern, a street
// light): a point light at its origin plus `offset` (in its own space,
// scaled and turned with it; for cloth, whose scale is unused, in metres), or a spot shining down its own -Y. Its light
// at 1 m is colour x intensity, in the sun's units (the default sun is 3),
// fading to nothing at `range`. With `shadows` it casts them (the nearest few
// lamps in view); without, it lights through walls.
enum class LampKind : u8 { point, spot };
const char* to_string(LampKind kind);
std::optional<LampKind> lamp_kind_from_string(std::string_view name);
constexpr float k_max_lamp_intensity = 1e6f;

struct Lamp {
    bool on = false;
    LampKind kind = LampKind::point;
    Vec3 color{1.0f, 0.8f, 0.55f}; // linear, 0 to 1 (a warm lamp)
    float intensity = 3.0f;       // 0 to k_max_lamp_intensity
    float range = 10.0f;          // metres, 0.1 to 1000
    Vec3 offset{};                // metres, -100 to 100 each
    float inner_degrees = 30.0f;  // spot: full light inside this, 0 to 89
    float outer_degrees = 45.0f;  // spot: none outside this, 0.1 to 89
    float source_radius = 0.05f;  // metres, 0 to 10: the bulb's size
    bool shadows = false;         // casts shadows (the nearest few in view)
    float shadow_near = 0.1f;     // metres, 0.01 to 10: nothing nearer the light casts (its own shade)
    bool operator==(const Lamp&) const = default;
};

// Hair an entity wears (when on): strands grown from the top of its shape
// (cube and sphere entities) or from a character's skin round one of its
// joints (mesh entities with a skinned file: `joint`), simulated near the
// camera and drawn by the renderer's hair passes. The shape of the hair is geometry::GroomParams',
// its look render::HairLook's. Kept while off.
constexpr i32 k_max_hair_strands = 200000;
constexpr i32 k_max_hair_guides = 1024;
constexpr usize k_max_hair_joint_bytes = 128;
// The axes a groom file's points are in (the file does not say): the
// engine's own, right-handed with +Z up (Houdini or Maya set so), or
// left-handed with +Z up (a groom made for Unreal).
enum class HairAxes : u8 { y_up, z_up, z_up_left };
const char* to_string(HairAxes axes);
std::optional<HairAxes> hair_axes_from_string(std::string_view name);
struct Hair {
    bool on = false;
    i32 strands = 20000;            // 1 to k_max_hair_strands
    i32 guides = 256;               // 1 to k_max_hair_guides (and at most `strands`): the strands that are simulated
    float top = 0.5f;               // 0.01 to 1: the share of the shape's height, from its top, that grows hair
    std::string joint;              // a character's joint it grows round and rides (empty: the first named like a head)
    bool from_file = false;         // a mesh entity's: its file's own hair curves (line primitives) instead of grown ones
    // A groom file (Alembic, .abc) whose curves are worn instead of grown
    // ones (and before `from_file`): modelled round the head they are for,
    // so in a character's model space, where they ride `joint`; else in
    // the entity's. Up to k_max_mesh_path_bytes; empty: none.
    std::string file;
    float file_scale = 1.0f;        // 0.0001 to 10000: the file's units in metres (0.01: centimetres)
    HairAxes file_axes = HairAxes::y_up;
    float length = 0.25f;           // metres, 0.001 to 10
    float length_variation = 0.2f;  // 0 to 1: strands are shorter by up to this share
    float droop = 0.8f;             // 0 to 1: stands on end to hangs
    float curl_radius = 0.0f;       // metres, 0 to 1
    float curl_turns = 0.0f;        // 0 to 100, along a full-length strand
    float frizz = 0.0f;             // metres, 0 to 1: a strand's wander by its tip
    float clump = 0.0f;             // 0 to 1: tips drawn in to locks
    i32 seed = 1;                   // 1 to 1000000
    float melanin = 0.5f;           // 0 to 1: white through blond and brown to black
    float redness = 0.2f;           // 0 to 1: ginger
    Vec3 tint{1.0f, 1.0f, 1.0f};    // linear, 0 to 1 (dye)
    float roughness = 0.3f;         // 0.05 to 1
    float width = 0.00008f;         // metres across a strand at its root, 0.00001 to 0.01
    float tip_width = 0.3f;         // 0 to 1: its width at the tip, as a share
    float variation = 0.15f;        // 0 to 1: how much strands differ in shade
    bool moves = true;              // simulated (near the camera); off, it keeps its shape
    float stiffness = 8.0f;         // Hz, 0 to 60: how firmly the roots hold the style
    float damping = 2.0f;           // 1/s, 0 to 60
    bool operator==(const Hair&) const = default;
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

// Skin (characters): light under it spreads out before it leaves (red
// furthest) over `scatter` metres at its widest (0.0005 to 0.1), `strength`
// (0 to 1) of its diffuse light; its highlight is two lobes, at the
// roughness times lobe_roughness[0] and [1] (0.1 to 4), the second
// `lobe_mix` (0 to 1) of it; `transmission` (0 to 1) of the light from the
// sun or a shadow-casting lamp passes through its thin parts (ears,
// fingers); `detail_normal` (an image file, as MaterialMaps' paths; empty:
// none) is a fine normal map (pores, wrinkles) over the uvs times
// `detail_tiling` (1 to 256), on top of the material's own.
struct SkinLook {
    float scatter = 0.012f;
    float strength = 1.0f;
    float lobe_roughness[2] = {0.75f, 1.3f};
    float lobe_mix = 0.15f;
    float transmission = 1.0f;
    std::string detail_normal;
    float detail_tiling = 16.0f;
    bool detail_green_down = false; // the detail map is DirectX-authored
    bool operator==(const SkinLook&) const = default;
};

// An eye (characters): a mesh with uvs whose iris is centred at uv
// (0.5, 0.5), `iris_radius` wide in uv (0.02 to 0.5), under a clear cornea:
// the iris lies `iris_depth` (0 to 0.45 of its radius) below it and shifts
// as the eye turns. `painted`: the material's base colour map holds the
// iris and pupil; else they are drawn, `iris_color` with a black pupil
// `pupil_radius` (0 to 0.95 of the iris) wide over the material's base
// colour as the white. A dark ring `limbal` wide (0 to 0.5 of the iris
// radius), `limbal_darkness` deep (0 to 1), rims the iris. The eye is wet:
// its highlight at `cornea_roughness` (0.045 to 1) off a cornea bulging by
// `bulge` (0 to 1); `shade` (0 to 1) darkens the white toward the lids.
struct EyeLook {
    float iris_radius = 0.15f;
    float pupil_radius = 0.3f;
    float iris_depth = 0.35f;
    float limbal = 0.15f;
    float limbal_darkness = 0.7f;
    Vec3 iris_color{0.25f, 0.4f, 0.55f};
    bool painted = false;
    float cornea_roughness = 0.06f;
    float bulge = 0.3f;
    float shade = 0.5f;
    bool operator==(const EyeLook&) const = default;
};

// A physically based material (metal/roughness, as artists know it).
struct Material {
    Vec3 base_color{0.8f, 0.8f, 0.8f}; // linear, 0 to 1
    float roughness = 0.5f;             // 0 mirror to 1 matte
    float metallic = 0.0f;              // 0 or 1 for real materials
    Vec3 emissive{};                    // linear, added light; may exceed 1
    Surface surface = Surface::standard; // what bullets meet (not drawn)
    // How it moves in the wind (world/wind.h), each 0 to 4: `sway` bends it
    // from its base (1: a tree, 2 to 3: bushes and grass), `flutter` shakes
    // what stands out from its middle (leaves, needles). 0: still.
    float sway = 0.0f;
    float flutter = 0.0f;
    // Leaves, grass, cloth: drawn from both sides (the back lit as the
    // side facing the viewer), and how much sun shines through (0 to 1,
    // tinted by the base colour) to the side away from it.
    bool two_sided = false;
    float translucency = 0.0f;
    // Cut-outs (leaves on cards, grass blades, fences): where the base
    // colour map's alpha is under this (0 to 1), nothing is there. 0: solid.
    float alpha_cutoff = 0.0f;
    // Leafy crowns: how far (0 to 1) the shading normal bends outward from
    // the middle of the thing, so a crown of leaves is lit as one soft,
    // rounded mass instead of a scatter of flat cards. 0: as modelled.
    float canopy = 0.0f;
    MaterialMaps maps; // entities, kit pieces and scatter items (kept, not drawn, on decals and cloth)
    std::optional<SkinLook> skin; // skin (none: not skin); not on the ground
    std::optional<EyeLook> eye;   // an eye (none: not one; never with skin); not on the ground
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
//
// `matched`: instead of one clip, the editor picks among these clips of
// the file frame by frame, by how the entity moves (motion matching: a
// walker walks, runs, turns and stands with whichever fits); "*" alone is
// every clip of the file. `clip` then plays only while the file cannot be
// matched (its hips and feet not found, none of the clips there).
constexpr float k_max_animation_speed = 100.0f;
constexpr usize k_max_matched_clips = 256;
struct EntityAnimation {
    std::string clip;
    float speed = 1.0f; // -k_max_animation_speed to k_max_animation_speed
    bool loop = true;
    std::vector<std::string> matched; // clip names, each once (at most k_max_name_bytes each, k_max_matched_clips in all)
    bool operator==(const EntityAnimation&) const = default;
};

// A look of its own for one part of a mesh drawn in parts (a file using
// several materials: each material's triangles are a part, named as the
// material is; the editor's MeshLibrary makes them). The part with the
// most triangles takes the entity's own material; any other takes the
// file's look for it unless the entity names it here. Its `material` is
// that part's whole look (colour, maps, skin or an eye, sides, cut-out),
// but for `sway` and `flutter`, which every part takes from the entity so
// that the mesh moves as one, and `surface`, which is the entity's.
// Names no part of the file has (or its first part's) do nothing.
constexpr usize k_max_entity_parts = 16;
struct PartLook {
    std::string name; // the part's name (the material's in the file, "" for triangles with none); each once an entity
    Material material;
    bool operator==(const PartLook&) const = default;
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
    std::vector<PartLook> parts; // when shape is `mesh` (kept otherwise): at most k_max_entity_parts
    DecalSettings decal; // used when shape is `decal`
    ClothSettings cloth; // used when shape is `cloth` (kept when the shape changes)
    SwingSettings swing; // when on: cube, sphere, plane, mesh and kit entities swing (kept otherwise)
    Lamp lamp;           // when on: it gives light, any shape (hidden entities give none)
    EntityEffect effect; // when on: it runs the effect where it stands (effects.h), any shape
    Hair hair;           // when on: cube and sphere entities wear hair (kept otherwise)
    bool visible = true;
    BodyKind body = BodyKind::fixed;
    float mass = 0.0f; // kilograms, when dynamic; 0: from its size (as water)
    EntityAnimation animation; // when shape is `mesh` and the file is skinned (kept otherwise)
    // Not 0: it stands for this person (people.h) while the person is near:
    // made and taken away by the editor as the camera moves, so never saved
    // (not in a scene's text, not in a cell's file): the person is. Its id
    // is person_entity_id(person), and the scene keeps such entities after
    // all others in its list (so the others' places never depend on them).
    PersonId person = 0;
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
    Lamp lamp; // when on: each placement gives its light
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

// The time of day (clock.h works out the sky from it). On, it places the
// sun (and at night the moon) instead of Lighting's angles; the editor
// runs it while playing, `speed` game seconds to each real one.
struct Clock {
    bool on = false;
    double hours = 12.0;        // local solar time, 0 to 24
    i32 day = 172;              // of the year, 1 to 365 (172: midsummer in the north)
    float latitude = 45.0f;     // degrees, -90 (south pole) to 90
    float north_degrees = 0.0f; // the compass heading the world's -Z points to, 0 to 360
    float speed = 60.0f;        // game seconds a real second while playing (0 to 86400; 60: an hour a minute)
    bool operator==(const Clock&) const = default;
};

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

// A body of water (water.h for the queries): a lake (a level inside its
// shore), a river (a strip down its path, its level and width per point)
// or the sea (a level everywhere: the shore is where the ground rises over
// it). Bodies never move the ground: carve their beds with the terrain
// brushes.
enum class WaterKind : u8 { lake, river, sea };

struct RiverPoint {
    double x = 0.0; // metres
    double z = 0.0;
    float width = 10.0f; // metres across
    float level = 0.0f;  // the surface's height here (metres)
    bool operator==(const RiverPoint&) const = default;
};

struct WaterPoint {
    double x = 0.0;
    double z = 0.0;
    bool operator==(const WaterPoint&) const = default;
};

// --- People (living-world plan LW1; people.h checks them) --------------------
//
// The people who live in the world: thousands of small records kept by the
// scene or the world, not by cells (they cross cells). The editor makes the
// few near the camera real characters (an entity apiece) and keeps the rest
// as records. Here: who they are and where they start; where each is while
// the world runs is the simulation's, kept by game saves.

// A spot people go to: a home, a workplace, a bench. `tags` say what it is
// for ("home", "work", "rest": any words; routines ask for places by tag).
constexpr usize k_max_places = 65536;
constexpr usize k_max_place_tags = 8;
constexpr usize k_max_tag_bytes = 32;
struct Place {
    std::string name; // unique in the scene, 1 to k_max_name_bytes
    DVec3 position;   // metres; its middle, on the ground
    float radius = 1.0f; // metres (0.25 to 1000): anywhere within is there
    float yaw = 0.0f;    // degrees: the way one stands there (a bench's front)
    std::vector<std::string> tags; // each once, 1 to k_max_tag_bytes of lower-case letters, digits and _
    bool operator==(const Place&) const = default;
};

// How a kind of person looks: what the entity drawn for one is given. Many
// people share one, so a crowd costs a few meshes.
constexpr usize k_max_person_looks = 1024;
struct PersonLook {
    std::string name; // unique in the scene, 1 to k_max_name_bytes
    std::string mesh; // a skinned glTF/GLB file (as Entity::mesh)
    float height = 1.0f; // the entity's scale, all axes (0.1 to 10)
    Material material;
    std::vector<PartLook> parts; // as Entity::parts
    Hair hair;
    std::vector<std::string> matched; // the clips it is motion matched among (as EntityAnimation::matched; empty: {"*"})
    bool operator==(const PersonLook&) const = default;
};

// One person. `id` is its own for good (never used again in the scene).
constexpr usize k_max_people = 65536;

// A routine: what the people who have it do through the day (living-world
// plan LW3). Each entry begins at an hour and holds until the next (the
// last holds over midnight until the first), and says what one does then
// and where: where it is, at its home, at a named place, or at a place
// with a tag (each person its own: the nearest to its home).
constexpr usize k_max_routines = 1024;
constexpr usize k_max_routine_entries = 24;
enum class RoutineAt : u8 { stay, home, place, tag };
struct RoutineEntry {
    float from = 0.0f;    // the hour it begins (0 to under 24), rising through the list
    std::string activity; // what one does then: a word as a place's tag is ("sleep", "work")
    RoutineAt at = RoutineAt::stay;
    std::string name;     // place: a Place's name; tag: a place's tag; else empty
    float speed = 1.4f;   // metres a second going there (0.1 to 20)
    bool operator==(const RoutineEntry&) const = default;
};
struct Routine {
    std::string name; // unique in the scene, 1 to k_max_name_bytes
    std::vector<RoutineEntry> entries; // 1 to k_max_routine_entries
    bool operator==(const Routine&) const = default;
};
struct Person {
    PersonId id = 0;
    std::string name;    // at most k_max_name_bytes; need not be unique
    std::string look;    // a PersonLook's name
    std::string home;    // a Place's name ("" for none)
    std::string routine; // a Routine's name ("" for none: it stays where it is)
    DVec3 position;      // where it starts (metres)
    float yaw = 0.0f;    // degrees
    u32 seed = 0;        // what tells it apart from others of its look (tint, pace)
    bool operator==(const Person&) const = default;
};

// --- Routes (living-world plan LW2; routes.h checks them) --------------------
//
// The world's roads and paths as a coarse graph: nodes (points on the
// ground) and links between them (walked both ways). People far from the
// camera travel along it by the clock; near, they walk the ground itself
// from node to node. Kept by the scene or the world, not by cells.
using RouteNodeId = u32; // 0: none
constexpr usize k_max_route_nodes = 65536;
constexpr usize k_max_route_links = 262144;
struct RouteNode {
    RouteNodeId id = 0; // its own for good (never used again in the scene)
    DVec3 position;     // metres, on the ground
    bool operator==(const RouteNode&) const = default;
};
enum class RouteKind : u8 { path, road }; // a road is quicker going (nav/route.h: RouteSettings)
struct RouteLink {
    RouteNodeId a = 0, b = 0; // a < b, both nodes there; a pair once
    float width = 2.0f;       // metres (0.5 to 100)
    RouteKind kind = RouteKind::path;
    bool operator==(const RouteLink&) const = default;
};
struct Routes {
    std::vector<RouteNode> nodes; // by rising id
    std::vector<RouteLink> links; // by rising (a, b)
    RouteNodeId next_id = 1;      // one past the last id any node here ever had
    bool operator==(const Routes&) const = default;
};

constexpr usize k_max_waters = 256;
constexpr usize k_max_water_points = 4096;

struct Water {
    std::string name; // unique in the scene
    WaterKind kind = WaterKind::lake;
    float level = 0.0f;               // lake and sea: the surface's height (metres)
    std::vector<WaterPoint> outline;  // lake: its shore in order (3 or more points; the water is inside)
    std::vector<RiverPoint> path;     // river: down its middle, upstream first (2 or more points)
    float flow = 1.0f;                // river: metres a second downstream (0 to 20)
    Vec3 color{0.02f, 0.06f, 0.07f};  // what it scatters back (its colour seen deep), 0 to 1
    Vec3 absorption{0.45f, 0.09f, 0.065f}; // light lost a metre through it, per channel (clear water: red first)
    float turbidity = 0.1f;           // 0 clear to 1 muddy: how much light it scatters back
    float waves = 1.0f;               // 0 still to 4: how high the wind raises its waves
    bool visible = true;              // hidden: not there (not drawn, nothing floats)
    bool operator==(const Water&) const = default;
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

    // Adds `entity` and returns its id (0, adding nothing, once all 2^52 - 1
    // ids have been handed out). Id 0 gets a new id; a non-zero id
    // (restoring a deleted entity, loading) must be unused. `at` is its
    // place in the entity list (default: the end). One standing for a
    // person (Entity::person) takes that person's entity id and goes after
    // all others whatever `at` (0, adding nothing, when it is there already);
    // the order among those is not kept (one of the scene's own added at the
    // end takes the first one's place, which goes last: nothing else moves).
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
    // How many are the scene's own: those not standing for people, which
    // come first in the list (and are what is saved).
    usize authored() const;
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
    // The time of day the scene starts at (not tracked: compare).
    Clock clock;
    // Lakes, rivers and the sea (water.h), by name, in order (not tracked: compare).
    std::vector<Water> water;
    // The people who live here (people.h), their looks and the places they
    // go to, in order (not tracked: compare). `next_person_id`: one past
    // the last id any person here ever had.
    std::vector<Place> places;
    std::vector<PersonLook> person_looks;
    std::vector<Person> people;
    PersonId next_person_id = 1;
    // What people do through the day (people.h), by name, in order (not tracked: compare).
    std::vector<Routine> routines;
    // The roads and paths people travel by (routes.h; not tracked: compare).
    Routes routes;
    // Particle looks and effects (effects.h), by name, in order (not tracked: compare).
    std::vector<ParticleLook> particle_looks;
    std::vector<Effect> effects;

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
    // The record of a kit placement shown (null: none): the game's, else the
    // kept one (of a cell the game holds nothing of); with `placed`, the
    // one made where it is placed now (two cells' files may name it: one
    // kept before it moved, stale).
    const KeptKit* kept_kit(EntityId id, const Transform* placed = nullptr) const;
    // Counts every set_damage and set_game_damage (a follower looks over the damage only when it moved).
    u64 damage_edits() const { return m_damage_edits; }

    // The game's damage (a game save's, loaded), by cell: shown over the
    // kept damage (a cell the game holds is the game's, whole), never saved
    // with the scene or the world (save_world's files stay as authored),
    // copied and moved with the scene. Kit placements it names, and those
    // the cell's kept records name (hidden or shown again), are marked
    // changed. Empty: not held (the kept damage shows; a game can break
    // more than the world kept, never less).
    const Damage& game_damage() const { return m_game; }
    const CellDamage* game_damage_in(CellCoord cell) const;
    void set_game_damage(CellCoord cell, CellDamage damage);
    void clear_game_damage();
    // What a cell shows: the game's, else the kept (null: none).
    const CellDamage* shown_damage_in(CellCoord cell) const;

    // The id the next new entity gets.
    EntityId next_id() const { return m_next_id; }
    // New ids start at `next` or later (ids of deleted entities stay unused).
    void reserve_ids_below(EntityId next) { m_next_id = next > m_next_id ? next : m_next_id; }

private:
    void reindex(usize from);
    void changed(EntityId id);
    void set_layer(Damage& layer, std::unordered_multimap<EntityId, CellCoord>& index, CellCoord cell,
                   CellDamage damage, bool hold_empty);

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
    Damage m_game;
    std::unordered_multimap<EntityId, CellCoord> m_game_kits;
    u64 m_damage_edits = 0;
    SceneChanges m_changes{{}, true};
    Identity m_identity;
};

// JSON text, stable key order, with a trailing newline.
std::string save_scene(const Scene& scene);
// A cell's damage on its own (a game save's chunk), with the tile's heights
// when a game's ground is in it (`ground`: samples x samples, as Terrain's),
// and the weather.
std::string save_cell_damage(const CellDamage& damage, const Terrain::Heights* ground = nullptr);
std::string save_weather(const Weather& weather);

struct LoadError {
    std::string message; // what is wrong and where, for people and the AI
};
// Replaces `out` only on success. Unknown keys are errors (typos must not
// silently drop data); missing optional keys take their defaults.
std::optional<LoadError> load_scene(std::string_view text, Scene& out);
// Every kit placement and tree foot must be in `cell`. `ground` (null: a
// chunk with heights is refused) is set to the tile's heights, or none.
std::optional<LoadError> load_cell_damage(std::string_view text, CellCoord cell, CellDamage& out,
                                          std::optional<Terrain::Heights>* ground = nullptr);
std::optional<LoadError> load_weather(std::string_view text, Weather& out);

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
    Clock clock;
    std::vector<Water> water;
    std::vector<Place> places;
    std::vector<PersonLook> person_looks;
    std::vector<Person> people;
    PersonId next_person_id = 1;
    // What people do through the day (people.h), by name, in order (not tracked: compare).
    std::vector<Routine> routines;
    // The roads and paths people travel by (routes.h; not tracked: compare).
    Routes routes;
    std::vector<ParticleLook> particle_looks;
    std::vector<Effect> effects;
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
// Its kept damage too, if any ("damage": damage.h), and its tile's paint
// if painted ("paint": hex rows, with heights only).
std::string save_cell(CellCoord cell, std::span<const Entity> entities,
                      const Terrain::Heights* heights = nullptr, const CellDamage* damage = nullptr,
                      const Terrain::Paint* paint = nullptr);
// Replaces `out` and `heights` only on success (`heights` empty when the
// file holds no tile). Every entity must be in `cell` (by position) and
// ids unique in the file; a tile must be `terrain_samples` a side (the
// world's; 0: none may be there). Kits are not checked (the world's index holds them): an
// entity naming a missing kit just draws nothing.
// `damage` (when given) takes the cell's kept damage (empty: none; it is
// read and checked either way). `paint` (when given) takes the tile's
// paint (empty: none).
std::optional<LoadError> load_cell(std::string_view text, CellCoord cell, u32 terrain_samples,
                                   std::vector<Entity>& out, std::optional<Terrain::Heights>& heights,
                                   CellDamage* damage = nullptr, Terrain::Paint* paint = nullptr);
// The same for a cell that must hold no terrain (terrain_samples 0).
std::optional<LoadError> load_cell(std::string_view text, CellCoord cell, std::vector<Entity>& out);

} // namespace giant::world
