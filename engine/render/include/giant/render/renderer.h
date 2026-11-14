// The renderer: turns plain per-frame data (a camera, lighting and a list of
// instances) into a lit image. It never sees the world or the editor, so it
// can be swapped or tested on its own (docs/architecture.md).
//
// Every mesh is cluster LOD (geometry/clusters.h). Each frame the GPU picks,
// per instance, the clusters whose detail is just under a pixel of error,
// inside the view and not hidden behind what is already drawn (two-pass
// occlusion culling against a depth pyramid), and draws them with mesh
// shaders (or a vertex-shader fallback) into a visibility buffer: per pixel,
// which triangle covers it. One full-screen pass then shades every pixel
// once: decals stamped on (binned into screen tiles on the CPU), then a sun
// with GGX specular and sky/ground ambient, tone mapped. Draw calls do not
// grow with instances, meshes or materials: every instance of every mesh
// goes through the same few passes.
//
// Sun shadows are virtual shadow maps (src/shadow_pages.h): a clipmap of
// pages seen from the sun, drawn from the same clusters with the same
// culling, only where shading asked for them, cached until what casts onto
// them changes, a budgeted number of pages a frame.
//
// The CPU can pick the clusters instead (RenderSettings::gpu_culling off):
// the same cut, for reference and tests, on any backend.
//
// Geometry streams: a fixed pool of GPU memory holds the cluster groups the
// view needs, loaded as culling asks for them and evicted when unused
// (src/streaming.h). Until finer detail arrives, coarser clusters stand in.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"
#include "giant/core/wind.h"
#include "giant/geometry/clusters.h"
#include "giant/rhi/rhi.h"
#include "giant/voxel/clipmap.h"

#include <array>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace giant::render {

namespace gpu {
struct MeshEntry; // src/gpu_layout.h
struct Material;
struct MaterialWind;
struct GpuOccluder;
struct VoxelLevel;
struct ShadowLevel;
struct FrameConstants;
struct CullPush;
}
class GeometryStreamer; // src/streaming.h
class VoxelScene;       // src/voxel_scene.h
class InstanceStore;    // src/instance_store.h
class TerrainPaintStore; // src/terrain_paint.h
class ShadowPages;      // src/shadow_pages.h
class VoxelLight;       // src/voxel_light.h
struct ShadowWindows;
struct DecalBins;       // src/decals.h
struct LightBins;       // src/lights.h
class LocalShadows;     // src/local_shadows.h
class VoxelLamps;       // src/voxel_lamps.h
class ParticleRing;     // src/particles.h

// A mesh loaded into the renderer: a built-in shape or one added with
// add_mesh.
using MeshId = u32;
inline constexpr MeshId k_no_mesh = ~0u;

enum class BuiltinMesh {
    cube,   // 1 m, centred
    sphere, // 1 m wide, centred
    plane,  // 1 m square at y = 0, facing +Y
};

// A texture loaded into the renderer with add_texture: a slot (low 20
// bits) and that slot's generation (high 12), so an id kept after
// remove_texture stops resolving instead of showing whatever is added there
// next.
using TextureId = u32;
inline constexpr TextureId k_no_texture = ~0u;

// Streamed textures (Renderer::stream_texture): a texture holds its levels
// from a first mip down, and finer levels come and go. A texture from one
// of its levels down is a texture of its own: this is its description.
rhi::TextureDesc texture_from_mip(const rhi::TextureDesc& desc, u32 first_mip);
// The coarsest level a texture may start at: its last, or for a
// block-compressed one the last whose sides are whole blocks (multiples
// of 4).
u32 coarsest_first_mip(const rhi::TextureDesc& desc);

// Where a streamed texture stands (Renderer::texture_residency).
struct TextureResidency {
    static constexpr u32 k_not_seen = ~0u;
    u32 first_mip = 0;  // the finest level on the GPU
    u32 target_mip = 0; // where it is going (first_mip when nothing is under way)
    // The finest level the drawn pixels needed lately (GPU feedback over
    // the last 32 to 64 frames, read back two frames late); k_not_seen if
    // nothing drew it then.
    u32 wanted_mip = k_not_seen;
    u64 bytes = 0; // on the GPU
};

// How a surface looks (metal/roughness PBR). Instances share them by index
// into FrameInput::materials, so a kit used a thousand times sends its few
// materials once.
struct Material {
    Vec3 base_color{0.8f, 0.8f, 0.8f}; // linear
    float roughness = 0.5f;
    float metallic = 0.0f;
    Vec3 emissive{}; // linear
    // How it moves in FrameInput::wind (giant/core/wind.h; 0 to 4 each):
    // bending from its base (trees, grass) and shaking at its tips (leaves).
    float sway = 0.0f;
    float flutter = 0.0f;
    // Textures over the mesh's uvs (giant/texture cooks them), each
    // k_no_texture for none; an unknown or removed id counts as none, and so
    // does a texture of the wrong colour space (logged once): base colour
    // must be sRGB, the others linear. Combined as glTF combines its maps,
    // in the cooker's channels: base colour multiplies base_color (its
    // alpha cuts out, with alpha_cutoff); the normal map (x in
    // R, y in G, green up, z rebuilt) bends the shading normal in the mesh's
    // tangent frame; roughness (R) and metallic (G) multiply theirs;
    // occlusion (R) dims the ambient, bounce and reflected light, not the
    // sun's. Sampled with wrapping. Only meshes drawn as meshes show them:
    // the voxel world (the far field, GI's bounce) takes base_color times
    // base_color_average below.
    TextureId base_color_texture = k_no_texture;
    TextureId normal_texture = k_no_texture;
    TextureId roughness_metal_texture = k_no_texture;
    TextureId occlusion_texture = k_no_texture;
    // Ground: shaded from FrameInput::terrain_layers instead (its own
    // values where none are given; its own textures never: the layers map
    // theirs by world position). The voxel world (GI's bounce, the far
    // field) still takes base_color: set it near the layers' average.
    bool terrain = false;
    // Leaves, grass, cloth: seen from both sides (no back-face culling; the
    // back's normal is turned to the viewer). Each kind of material
    // in a frame (one- or two-sided, solid or cut out) is a visibility pass.
    bool two_sided = false;
    // Thin surfaces (leaves): how much of the sun shines through to the
    // side away from it (0 to 1), tinted by the base colour.
    float translucency = 0.0f;
    // Cut-outs (leaves on cards, grass, fences): where the base colour
    // texture's alpha is under this (0 to 1), nothing is there, in the
    // image and in shadows. 0, or no base colour texture (yet): solid.
    // Drawn in a visibility pass of their own.
    float alpha_cutoff = 0.0f;
    // Leafy crowns (0 to 1): the shading normal bends this far towards the
    // direction out from the instance's bounding sphere's centre, so a
    // crown of cards is lit as one rounded mass (kept from facing away from
    // the viewer); the voxel world's facing bends the same way. Shadows
    // keep the surface's own normal. At 1 the normal map's detail is gone
    // (0.5 to 0.8 keeps some); the middle is the crown's at rest, so a
    // swaying crown's light shifts a little.
    float canopy = 0.0f;
    // For the voxel world, which cannot sample textures: the base colour
    // texture's average colour (linear; for a cut-out, of what its cutoff
    // leaves: texture::summarize_color), which multiplies base_color there,
    // and, for a cut-out, the share of its surface alpha_cutoff leaves (0
    // to 1), which thins its voxels to match (voxel::BrickBuilder). Both
    // count only with base_color_texture. The caller keeps them in step
    // with the texture: 1 while it has none (or it is not ready). A change
    // builds the voxels of its instances again.
    Vec3 base_color_average{1.0f, 1.0f, 1.0f};
    float coverage = 1.0f;
};

// One of the ground's layers (FrameInput::terrain_layers), for materials
// with `terrain`. Layers stack in order like paint: the first covers
// everywhere, each later one covers where both its slope and its height
// bands hold (soft over slope_blend degrees and height_blend metres at
// their edges). Textures as Material's (base colour sRGB, normal map,
// roughness and metallic: roughness multiplies roughness, metallic is not
// used), mapped from above by world x and z, every `tiling` metres, and
// from the sides too where the ground is steep (triplanar), so cliffs do
// not stretch. A slow variation in brightness over tens of metres hides
// the textures' repeats.
inline constexpr u32 k_max_terrain_layers = 8;
// Painted layers (Renderer::set_terrain_paint): bytes a ground sample, the
// weights (0 to 255) of layers 1 to 7, the last spare.
inline constexpr u32 k_terrain_paint_channels = 8;
struct TerrainLayer {
    Vec3 base_color{0.23f, 0.2f, 0.15f}; // linear
    float roughness = 0.92f;
    TextureId base_color_texture = k_no_texture;
    TextureId normal_texture = k_no_texture;
    TextureId roughness_metal_texture = k_no_texture;
    float tiling = 4.0f;    // metres (0.05 to 1000)
    float min_slope = 0.0f; // degrees from level (0 to 90)
    float max_slope = 90.0f;
    float slope_blend = 5.0f;   // degrees
    float min_height = -1.0e4f; // metres above y = 0
    float max_height = 1.0e4f;
    float height_blend = 10.0f; // metres
};

struct Instance {
    DVec3 position{};
    Quat rotation{};
    Vec3 scale{1.0f, 1.0f, 1.0f};
    MeshId mesh = 0;
    // Into FrameInput::materials; out of range draws the default Material.
    u32 material = 0;
    // Drawn as a mesh at every distance: the far field never stands in for
    // it, however small (terrain tiles: each is small, together they are
    // the ground and the skyline). It is still in the voxel world.
    bool always_mesh = false;
    // Moves often (a swinging lantern): kept out of the voxel world (as
    // skinned meshes are), so moving it each frame costs the voxels
    // nothing, and so drawn as a mesh at every distance (the far field
    // never stands in for it). It bounces no light.
    bool moves = false;
};

// The mask a decal stamps (shapes for now; image stamps arrive with textures).
enum class DecalShape : u32 {
    disc,      // round, soft-edged
    rectangle, // the whole box face
    ring,
    splat,     // an irregular blot (seeded): mud, blood, scorch
    cracks,    // cracked ground (seeded): impacts, giant footfalls
    // Wet (water plan W5f: what went in the water): every surface inside
    // the box, whichever way it faces, darkened where it is porous and
    // made glossier, as rain wets things (its colour, metal and emissive
    // left out); `softness` fades its top edge (the waterline), as a
    // fraction of the box's height. `opacity`: how wet (1 soaked).
    wet,
};

// A box that stamps a material onto whatever surfaces lie inside it, seen
// from its local +Y (it projects downwards, along -Y; a wet one wets all
// of what is inside). Later decals in the list draw over earlier ones.
struct Decal {
    DVec3 position{};
    Quat rotation{};
    Vec3 size{1.0f, 1.0f, 1.0f}; // the box, metres: x and z the stamp, y its depth
    Vec3 base_color{0.1f, 0.1f, 0.1f};
    float opacity = 1.0f;
    float roughness = 0.8f;
    float metallic = 0.0f;
    Vec3 emissive{};
    DecalShape shape = DecalShape::disc;
    float softness = 0.1f; // edge fade, as a fraction of the stamp
    u32 seed = 0;          // varies splat and cracks
};

struct Lighting {
    Vec3 sun_direction{0.0f, 1.0f, 0.0f}; // unit, towards the sun
    Vec3 sun_light{3.0f, 3.0f, 3.0f};     // colour times intensity
    Vec3 sky_light{0.2f, 0.3f, 0.5f};     // ambient from above, times intensity
    Vec3 ground_light{0.1f, 0.1f, 0.1f};  // ambient from below, times intensity
    Vec3 background{0.33f, 0.52f, 0.85f}; // linear, where nothing is drawn
    float exposure = 1.0f;
};

// The sky over the scene: an atmosphere the sun shines through, a layer of
// clouds that shades the ground, and height fog (src/sky shaders). Off:
// Lighting::background, flat, and no fog. Lit views only.
struct Sky {
    bool on = false;
    Vec3 sun_top{3.0f, 3.0f, 3.0f}; // the sun above the air, colour times intensity (Lighting::sun_light is at the ground)
    float haze = 1.0f;              // 0 to 10: dust and moisture (paler sky)
    // Clouds: a layer from cloud_base to cloud_top, metres above y = 0, over
    // a round earth.
    float cloud_cover = 0.0f;    // 0 to 1
    float cloud_density = 0.5f;  // 0 to 1
    float cloud_base = 1500.0f;
    float cloud_top = 3000.0f;
    float cloud_tile = 32000.0f; // metres the clouds' pattern repeats (their size: about a tenth)
    DVec3 cloud_offset{};        // metres the wind has carried the clouds (x and z)
    // Height fog: per metre at y = 0, thinning by exp(-falloff * height).
    float fog_density = 0.0f;
    float fog_falloff = 1.0f / 200.0f;
    // What falls (0 to 1 each), blown by the wind (metres a second), and
    // the ground under it: wet (darker, glossier, puddles on the flat) and
    // snow on what faces up, except where something above shelters it.
    float rain = 0.0f;
    float snow = 0.0f;
    Vec3 wind{};
    float wetness = 0.0f;
    float snow_cover = 0.0f;
    double time = 0.0; // seconds: what falls moves with it
    // The night sky (world/clock.h): stars fixed to the sky's own frame,
    // `celestial`'s rows taking a world direction into it, so they turn
    // as the hours pass; `stars` their brightness (0: none; seen where the
    // sky is darker than they are, and through no cloud). `moon`: the disc
    // drawn at sun_direction is the moon, lit from `moon_sun` (towards the
    // sun) by its phase, with a faint earthshine on its dark side;
    // `moon_lit` the share of it lit, which Lighting's light (the moon's)
    // already carries, so its lit face is as bright at any phase.
    float stars = 0.0f;
    std::array<Vec3, 3> celestial{Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};
    bool moon = false;
    float moon_lit = 1.0f;
    Vec3 moon_sun{0.0f, -1.0f, 0.0f};
};

// A soft ball of dust in the air (a collapse's cloud): densest at its
// centre, thinning to nothing at `radius`, over whatever is behind it, lit
// by the sun and the sky.
struct DustPuff {
    DVec3 center{};
    float radius = 1.0f;
    float density = 0.1f;  // light taken out per metre at the centre
    Vec3 color{0.5f, 0.5f, 0.5f}; // how much light its dust gives back (0 to 1)
};
// Puffs one frame draws at most (the first of FrameInput::dust).
inline constexpr u32 k_max_dust_puffs = 32;

// A part of a character's body for the light round it (characters plan C2):
// a capsule (the segment a to b, `radius` round it) that shuts the sky's and
// the bounced light out of what is near it, and gives some back as its own
// colour (`albedo`, 0 to 1: light bounced off it). Worked out per pixel from
// the capsules themselves, so it follows the pose with no lag.
struct Occluder {
    DVec3 a{};
    DVec3 b{};
    float radius = 0.1f; // metres (at most 10)
    Vec3 albedo{0.5f, 0.5f, 0.5f};
};
// Occluders one frame uses at most (the nearest of those in view), and how
// far from the camera they count.
inline constexpr u32 k_max_occluders = 1024;
inline constexpr float k_occluder_distance = 150.0f;

// Particles (effects plan FX1): born in bursts, each moving by a closed
// form from where and how it was born (no simulation state: the same age
// is always the same place, so nothing flickers without history), drawn
// as soft quads. A style is how one looks and moves over its life.
enum class ParticleShape : u32 {
    puff,   // a soft round blot (smoke, dust, spray, fire)
    streak, // stretched along its motion (sparks, rain splashes, chips)
};

struct ParticleStyle {
    float life_min = 1.0f; // seconds, each particle between (at most k_max_particle_life)
    float life_max = 2.0f;
    float size_start = 0.2f; // metres across, at birth and at the end (linear between)
    float size_end = 0.6f;
    Vec3 color_start{0.5f, 0.5f, 0.5f}; // what it gives back of the light (0 to 1)
    Vec3 color_end{0.5f, 0.5f, 0.5f};
    float opacity_start = 1.0f; // 0 to 1: eased in over the first tenth of its life, then to the end's
    float opacity_end = 0.0f;
    Vec3 emissive{};         // light of its own (sparks, fire, embers), fading with its opacity
    float emissive_end = 1.0f; // the share of `emissive` left at the end
    float gravity = 1.0f;    // how much of the world's gravity pulls it (0: none)
    float buoyancy = 0.0f;   // metres a second squared up (smoke, fire)
    float drag = 0.0f;       // 1/s: how fast it takes the air's speed (0: none, ballistic)
    float wind = 1.0f;       // how much of the wind the air it takes the speed of moves with (with drag)
    float wobble = 0.0f;     // metres it wanders off its path (seeded, slow: smoke curling)
    ParticleShape shape = ParticleShape::puff;
    bool lit = true;         // lit by the sun, sky and lamps (false: emissive only)
    float soft = 0.3f;       // metres over which it fades into what it meets (no hard edge)
};

// Particles born together: `count` from around `position` (within
// `spread` metres), going `direction` (within `cone_degrees` of it; 180:
// any way) at speed_min to speed_max metres a second, plus `velocity` (what
// they came off). Those that fall to `ground` (a height) stop there. The
// same seed gives the same particles.
struct ParticleBurst {
    u32 style = 0; // into FrameInput::particle_styles
    u32 count = 1;
    DVec3 position{};
    float spread = 0.0f;
    Vec3 direction{0.0f, 1.0f, 0.0f};
    float cone_degrees = 30.0f;
    float speed_min = 1.0f;
    float speed_max = 2.0f;
    Vec3 velocity{};
    float size_scale = 1.0f;
    double ground = -1.0e30;
    u32 seed = 0;
};

inline constexpr u32 k_max_particle_styles = 256;
inline constexpr u32 k_max_particles = 1u << 18;      // alive at once (the oldest make way)
// Born a frame (past it: left out). Births cost the CPU about 0.05
// microseconds each (measured on the build machine).
inline constexpr u32 k_max_particles_born = 1u << 14;
// Of those, born a frame with a `ground` (each costs a search for where it
// lands as well, about 0.3 microseconds; past it: left out). The two caps
// together hold births to about 2 ms of CPU at worst.
inline constexpr u32 k_max_particles_grounded = 1u << 12;
inline constexpr float k_max_particle_life = 60.0f;   // seconds

// A lamp: a point light, or a spot light shining down a cone (lit views,
// not the far field). Its light at distance d is colour x intensity / d^2
// (the same relative units as Lighting::sun_light, so intensity 3 at 1 m
// lights like the default sun), easing smoothly to nothing at `range`.
// A source the size of `source_radius` keeps close surfaces from flaring
// and widens its highlights. With a shadow_id it casts shadows (the
// nearest few in view; the id must stay the same from frame to frame, as
// the shadows are kept by it and drawn again only when the light or what
// is near it changes); without, it lights through walls.
enum class LightKind : u8 { point, spot };
struct LocalLight {
    DVec3 position{};
    Vec3 color{1.0f, 1.0f, 1.0f}; // linear
    float intensity = 1.0f;
    float range = 10.0f;          // metres (0.01 to 1000)
    LightKind kind = LightKind::point;
    Vec3 direction{0.0f, -1.0f, 0.0f}; // spot: where it points (normalized here)
    float inner_angle = 0.4f;   // spot: full light inside this half angle, radians
    float outer_angle = 0.6f;   // spot: none outside this (at most 89 degrees)
    float source_radius = 0.05f; // metres
    u32 shadow_id = 0;          // nonzero: casts shadows (unique among the frame's lights)
    float shadow_near = 0.1f;   // metres: nothing nearer the light than this casts (its own lamp's shade)
};
// Lights one frame shades at most (the nearest of FrameInput::lights in
// view), and at most per cluster of the screen (the nearest again).
inline constexpr u32 k_max_lights = 4096;
inline constexpr u32 k_max_lights_per_cluster = 256;

struct View {
    DVec3 position{};
    Quat orientation{}; // looks down -Z, +Y up, before rotation
    float vertical_fov_degrees = 60.0f;
    float near_plane = 0.1f; // metres; there is no far plane (reverse-Z)
};

// How the exposed light is brought into the screen's 0 to 1 (lit views).
enum class ToneMapper : u32 {
    pbr_neutral, // Khronos PBR Neutral: base colours kept true until the highlights roll off
    agx,         // AgX: a filmic curve; bright colours go gently over to white
};

// What the image shows: the lit scene, or how the geometry is built.
enum class DebugView : u32 {
    lit,
    clusters,   // each cluster in its own colour
    lod_levels, // colour by level: 0 (original triangles) is red, then yellow, green, cyan...
    triangles,  // each triangle in its own colour
    voxels,     // the voxel copy of the scene (RenderSettings::voxel_world), with its light when gi is on
    lighting,   // the lit view in plain white: the light alone (sun, shadows, bounce and sky)
};

struct RenderSettings {
    // How much simplification may show: a cluster is used once its error
    // covers at most this many pixels. Lower is sharper and slower.
    float lod_error_pixels = 1.0f;
    DebugView debug_view = DebugView::lit;
    // Clusters are picked on the GPU (off: on the CPU, which also gives
    // exact, same-frame stats; the GPU's arrive a frame or two later).
    bool gpu_culling = true;
    // GPU culling skips what is hidden behind nearer geometry.
    bool occlusion_culling = true;
    // Draw with mesh shaders where the GPU has them (off: the vertex
    // shader fallback, as on GPUs without them).
    bool mesh_shaders = true;
    // Smooths jagged geometry edges from the finished image (SMAA 1x): no
    // history, so the image stays as sharp moving as standing still.
    bool antialiasing = true;
    // Widens shiny highlights where the surface curves faster than a pixel
    // can show, so they do not sparkle or crawl.
    bool specular_antialiasing = true;
    // Keeps the voxel copy of the scene around the camera up to date (the
    // shared voxel world: far geometry and lighting build on it). Off, it
    // stays as it is.
    bool voxel_world = true;
    // CPU time per frame for building voxel bricks (at least one is built
    // when any wait); the rest wait for the next frame.
    float voxel_ms_per_frame = 1.0f;
    // The far field: past far_field_distance (metres) the voxel world
    // stands in for small instances (bounding radius at most
    // far_field_max_radius metres), which are not drawn as meshes at all.
    // Larger ones (terrain, mountains, big buildings) stay meshes; the
    // voxels show in front of them. Lit view only, and only with
    // voxel_world on (a frozen voxel world would show stale far geometry).
    bool voxel_far_field = true;
    float far_field_distance = 3000.0f;
    float far_field_max_radius = 50.0f;
    // Sun shadows (lit view, GPU culling only). Pages are drawn when shading
    // first asks for them and again when what casts onto them changes; at
    // most shadow_pages_per_frame a frame (at least one when any wait),
    // the rest keep their old content (or fall back to coarser pages).
    bool shadows = true;
    u32 shadow_pages_per_frame = 64;
    // Global illumination: light bouncing off the scene and the sky's light
    // shut out by it (lit views, with voxel_world on), from the lit voxel
    // world. Up to gi_bricks_per_frame voxel bricks are lit again a frame
    // (at least one): new and rebuilt bricks first, then the rest in turn,
    // so light follows the sun and changes within about bricks / budget
    // frames. Off: the flat sky and ground colours.
    bool gi = true;
    u32 gi_bricks_per_frame = 256;
    // Reflections on glossy surfaces (lit views): a cone through the lit
    // voxel world along the mirror direction, as wide as the highlight
    // (needs gi), and, where what it meets is on screen, a sharp one from
    // this frame's shaded image (screen_space_reflections). Off: the sky
    // and ground colours, dimmed where global illumination finds them
    // hidden.
    bool reflections = true;
    bool screen_space_reflections = true;
    // Ambient occlusion (lit views): creases, corners and contact darken
    // the ambient light and reflections, from this frame's depth (GTAO at
    // half resolution, a spatial denoise, no history), out to
    // ambient_occlusion_radius metres (0.1 to 4); the ambient light also
    // comes from the open direction there (the bent normal).
    bool ambient_occlusion = true;
    float ambient_occlusion_radius = 1.0f;
    // Contact shadows (lit views): the small shadows the sun's shadow maps
    // are too coarse for (a cup on a table, grass blades on the ground,
    // the crease under a rock), from this frame's depth marched towards
    // the sun for contact_shadow_length metres (0.05 to 2), never further
    // than 48 pixels.
    bool contact_shadows = true;
    float contact_shadow_length = 0.3f;
    // Characters in the light (lit views, FrameInput::occluders): their
    // bodies shut the sky's and bounced light out round them (a person
    // standing on the ground darkens it round their feet) and give back
    // their own colour.
    bool character_occlusion = true;
    // Local lights (FrameInput::lights; lit views).
    bool local_lights = true;
    // Water (FrameInput::water_vertices; lit views).
    bool water = true;
    // Particles drawn (FrameInput::particle_bursts; the lit view). Off,
    // they are still born and age, unseen.
    bool particles = true;
    // Their shadows (lights with a shadow_id; needs `shadows` and GPU
    // culling): faces of shadow tiles drawn a frame at most (nearest first;
    // a face not drawn yet lets its light through).
    bool local_shadows = true;
    u32 local_shadow_faces_per_frame = 24;
    // The physical sky (with FrameInput::sky on): the air's light scattered
    // many times (a brighter, deeper sky, and the glow after sunset), the
    // clear air between the camera and what it sees (far hills fade blue
    // into the distance, out to aerial_distance metres), and the ambient
    // light taken from the sky itself all round (the clouds' deck as they
    // cover it, the sunlit ground below) instead of Lighting's flat
    // sky_light and ground_light. All every frame, without history.
    bool physical_sky = true;
    float aerial_distance = 32000.0f; // 1000 to 100000
    // Exposure (lit views): how bright the light is made before tone
    // mapping, Lighting::exposure times, with auto_exposure, the eye's own:
    // the scene's brightness is measured every frame (a histogram of the
    // light before exposure, read back once the GPU is done with it, a
    // frame or two later), its middle (the 10th to 90th percentiles, an
    // average in stops) brought to middle grey, raised or lowered by
    // exposure_compensation stops, kept within exposure_min to exposure_max
    // stops of 1, and eased there by at most exposure_speed_up (brighter)
    // or exposure_speed_down (darker) stops a second of
    // FrameInput::delta_seconds, as eyes adapt. One number, no image
    // history: nothing smears. The first measurement is taken at once.
    bool auto_exposure = true;
    float exposure_compensation = 0.0f; // stops, -10 to 10
    float exposure_min = -16.0f;        // stops, -20 to 20
    float exposure_max = 16.0f;
    float exposure_speed_up = 3.0f;     // stops a second, 0 (held) to 100
    float exposure_speed_down = 1.5f;
    // Bloom (lit views): light spread round bright things as a lens does,
    // from the light before exposure blurred over six halvings of the
    // screen, mixed in as bloom_strength (0 to 1) of every pixel's light
    // (so it only moves light about: none is added).
    bool bloom = true;
    float bloom_strength = 0.04f;
    ToneMapper tone_mapper = ToneMapper::pbr_neutral;
    // The clouds (with FrameInput::sky on): marched at half resolution,
    // cloud_steps steps a ray (8 to 256), and their shadows on the ground.
    bool clouds = true;
    u32 cloud_steps = 48;
    bool cloud_shadows = true;
    // Rain and snow (with FrameInput::sky on): falling (`precipitation`),
    // on surfaces (`wet_surfaces`: the weather's only; wet decals, what
    // went in the water, are decals), and kept off what is sheltered
    // (`weather_cover`: a map from above made from the voxel world).
    bool precipitation = true;
    bool wet_surfaces = true;
    bool weather_cover = true;
    // Swaying (materials with sway or flutter, in FrameInput::wind): out to
    // wind_distance metres from the camera, fading out over its last
    // third; further away they stand still (and the far field and the
    // voxel world, which never sway, match them). 0: nothing sways. Sun
    // shadow pages under swaying things that far are drawn again in turn,
    // at most wind_shadow_pages_per_frame a frame (besides
    // shadow_pages_per_frame), so their shadows sway too.
    float wind_distance = 150.0f;
    u32 wind_shadow_pages_per_frame = 32;
};

// Water (lit views; water plan W2): the surfaces of lakes, rivers and the
// sea as triangles the caller makes (FrameInput::water_vertices, three
// indices a triangle in water_indices), drawn after shading over what lies
// under them: seen through (bent by the waves, the light lost a metre per
// channel by `absorption`, fading into the water's own `color` lit by the
// sun and the sky as it deepens, `turbidity` thickening it), and mirroring
// the sky and the sun as Fresnel's law has it (more the flatter it is
// seen). The waves are a sum of travelling waves blown by
// FrameInput::wind, `waves` high (0 still to 4), their pattern moving with
// FrameInput::water_seconds; the surface itself stays flat (its normals
// wave), but for the sea's swell (FrameInput::swell, WaterVertex::swell),
// which raises and lowers it. Running water (WaterVertex::flow) carries its waves downstream,
// rougher and foaming the faster it runs and in rapids, and a surface that
// slopes (WaterVertex::slope_*) is lit as it leans. Where it shoals over a
// sloping bed, the waves run in to the shore in bands and break into foam
// (higher in a stronger wind). The sun's light through
// the waves gathers into moving caustics on
// what lies under them. Seen from below (FrameInput::underwater) the
// surface lets the world above through a window (Snell's) and mirrors the
// water beyond it.
struct WaterLook {
    Vec3 color{0.02f, 0.05f, 0.06f};       // seen deep (linear RGB, what it scatters back)
    Vec3 absorption{0.45f, 0.09f, 0.065f}; // light lost a metre, per channel (0 to 10)
    float turbidity = 0.1f;                // 0 clear to 1 muddy
    float waves = 1.0f;                    // 0 still to 4
};
struct WaterVertex {
    DVec3 position{}; // world, on the surface
    float flow_x = 0.0f; // the water's speed (m/s; rivers)
    float flow_z = 0.0f;
    u32 look = 0;     // index into FrameInput::water_looks (past it: the default look)
    // White water (rivers: rapids where the bed is steep): 0 calm to 1,
    // foaming and rough. (Running water roughens and foams with its speed too.)
    float rapids = 0.0f;
    // How the surface slopes here (its height's rise a metre along x and
    // z; -10 to 10): lit as it leans (a river falling), smoothly across
    // its triangles. 0 for level water.
    float slope_x = 0.0f;
    float slope_z = 0.0f;
    // Rides the swell: raised and lowered by FrameInput::swell, times its
    // look's `waves`, as it lies here, lit as it leans. Waves too short for
    // the triangles to follow fade out with the distance from the camera
    // (made for FrameInput::seas' mesh: about 3 % of the distance across).
    bool swell = false;
};
// A sea (FrameInput::seas): the renderer draws it everywhere, as rings
// round the camera out past the horizon (fine out to 2 km, each 1.03 times
// the last, 256 round, for the swell to ride; then coarse), far to near so
// a nearer crest hides what is behind it (the water pass has no depth),
// riding the swell. Seas are drawn after the water lower than them
// (FrameInput::water_indices_under_seas) and before the rest, which shows
// over them.
struct SeaSurface {
    double level = 0.0; // the still surface (world y)
    u32 look = 0;       // index into FrameInput::water_looks (past it: the default look)
};
inline constexpr u32 k_max_seas = 4;
// One of the sea's swell's waves (world::SwellWave, which physics floats
// bodies on): its height at (x, z) at FrameInput::water_seconds `t` is
//   amplitude * sin(k (dir_x x + dir_z z) - sqrt(9.81 k) t + phase).
struct SwellWave {
    float dir_x = 1.0f; // unit, level
    float dir_z = 0.0f;
    float k = 0.0f;         // radians a metre
    float amplitude = 0.0f; // metres (0: none)
    float phase = 0.0f;     // radians
};
inline constexpr u32 k_swell_waves = 8; // (world::k_swell_waves: a set and the one fading out)
// The water the camera is in (water plan W3), as the caller finds it at the
// camera (x, z): wherever the camera's near plane dips under `level`, the
// view there is under water: what is seen dimmed and hidden by the water's
// own colour (its look's absorption and turbidity, lit by the sun and the
// sky, dimmer the deeper), the surface seen from below, caustics on the
// bed. The line where the near plane meets the surface splits the screen.
struct Underwater {
    bool on = false;    // the camera is over (or in) a body of water
    double level = 0.0; // its surface there (world y; the sea's still level)
    WaterLook look;
    bool swell = false; // the sea: its surface rises and falls with the swell (as WaterVertex::swell)
};
// What one frame draws at most (the rest are left out: RenderStats::water_overflow).
inline constexpr u32 k_max_water_vertices = 1u << 18;
inline constexpr u32 k_max_water_triangles = 1u << 18;
inline constexpr u32 k_max_water_looks = 256;

// A run of FrameInput::instances that changes together (a world cell).
// Instances one frame draws at most (instance culling is one dispatch of
// 65535 groups of 64); the ones past it are not drawn.
inline constexpr u32 k_max_instances = 65535u * 64u;
inline constexpr u32 k_max_skin_joints = 1024; // anim::k_max_joints
inline constexpr float k_max_morph_weight = 1000.0f; // either way (set_pose)

struct CellRange {
    u32 first = 0;
    u32 count = 0;
    // A cell whose version and place are as last frame's holds the same
    // instances as then, by value (their material indices: a material
    // row's values may change under an unchanged cell, as its textures
    // arrive; the voxel world compares the rows itself).
    u64 version = 0;
    // A cell changed from version prev_version (same place and count) only
    // in instances [first + changed_first, + changed_count): the rest are
    // as then, so whoever saw prev_version need look at those alone. 0: no
    // such promise.
    u64 prev_version = 0;
    u32 changed_first = 0;
    u32 changed_count = 0;
};

struct FrameInput {
    View view;
    Lighting lighting;
    Sky sky;
    RenderSettings settings;
    std::span<const Instance> instances;
    // Optional. When given, the cells cover `instances` end to end, in
    // order, and the renderer looks for changes only in the cells that
    // changed; without, it compares every instance with last frame's.
    std::span<const CellRange> cells;
    std::span<const Material> materials;
    // The ground's layers, for materials with `terrain` (the first
    // k_max_terrain_layers).
    std::span<const TerrainLayer> terrain_layers;
    std::span<const Decal> decals;
    // Dust in the air, nearest first (lit views; past k_max_dust_puffs left out).
    std::span<const DustPuff> dust;
    // Characters' bodies in the light (lit views; see Occluder).
    std::span<const Occluder> occluders;
    // Lamps (lit views; see LocalLight).
    std::span<const LocalLight> lights;
    // Particles born this frame (ParticleBurst), and the styles they and
    // those alive draw with (the same index the same style from frame to
    // frame; past k_max_particle_styles: left out). Their clock is
    // particle_seconds (or delta_seconds when that is negative; 0: still).
    std::span<const ParticleBurst> particle_bursts;
    std::span<const ParticleStyle> particle_styles;
    // What swaying materials move in (WindClock::state).
    WindState wind;
    // Seconds since the last frame, for auto exposure's easing (0: held;
    // at most 1 counts).
    float delta_seconds = 1.0f / 60.0f;
    // The particles' clock: seconds since the last frame (the game's, so a
    // paused game's stand still; negative: delta_seconds).
    float particle_seconds = -1.0f;
    // Water's surfaces (see WaterLook), and the clock its waves move by
    // (seconds; the same seconds draw the same waves).
    std::span<const WaterVertex> water_vertices;
    std::span<const u32> water_indices;
    std::span<const WaterLook> water_looks;
    double water_seconds = 0.0;
    // The seas (past k_max_seas: left out, RenderStats::water_overflow).
    std::span<const SeaSurface> seas;
    // How many of water_indices (from the first; whole triangles) are
    // drawn before the seas: the water lower than them (the rest after).
    u32 water_indices_under_seas = 0;
    // Changes whenever water_vertices, water_indices or water_looks do
    // (the voxel world then looks at them again); 0: unknown, they are
    // compared every frame (a hash of them all).
    u64 water_version = 0;
    // The sea's swell (WaterVertex::swell): none when every amplitude is 0.
    std::array<SwellWave, k_swell_waves> swell{};
    // The water the camera is in (lit views, with RenderSettings::water).
    Underwater underwater;
};

struct RenderStats {
    u32 instances = 0; // with at least one cluster drawn (GPU culling: passing instance culling)
    u32 draws = 0;     // draw calls this frame (raster passes and shading)
    u32 post_draws = 0; // full-screen passes after shading (screen reflections when on, anti-aliasing): fixed, not per object
    u32 clusters = 0;
    u32 triangles = 0;
    // GPU culling: the counts above come from the GPU, from a frame or two
    // ago (they are read back once the GPU is done with that frame).
    bool from_gpu = false;
    u32 occluded_instances = 0; // hidden in the first pass (GPU, occlusion on)
    u32 occluded_clusters = 0;
    bool overflow = false; // a list was full: some geometry was dropped
    u32 materials = 0;     // in the frame's material table
    u32 decals = 0;        // in view this frame (on the CPU: never late)
    bool decal_overflow = false; // too many decals on screen: some were not drawn
    u32 cells = 0;         // in FrameInput::cells (0 without them)
    u32 cells_changed = 0; // of them, looked at for changes this frame (all of them when the layout changed)
    // Streaming (this frame, on the CPU: never late).
    u64 pool_bytes = 0;      // GPU memory for cluster geometry
    u64 pool_used_bytes = 0;
    u32 resident_groups = 0; // cluster groups loaded, of every mesh
    u32 total_groups = 0;
    u32 streamed_in = 0;     // groups loaded this frame
    u32 evicted = 0;         // groups dropped this frame to make room
    u32 waiting = 0;         // groups asked for but not loaded yet
    // Material textures (this frame, on the CPU).
    u32 textures = 0;              // live
    u64 texture_bytes = 0;         // on the GPU, their levels resident
    u64 texture_stream_bytes = 0;  // finer levels uploaded this frame
    u32 textures_changing = 0;     // waiting for or in a level change
    bool texture_feedback = false; // drawn pixels asked what detail they need this frame
    // The voxel world (this frame, on the CPU).
    u32 voxel_bricks = 0;         // in the GPU pool
    u32 voxel_capacity = 0;       // the pool's size, bricks
    u32 voxel_pending = 0;        // waiting to be built
    u32 voxel_built = 0;          // built this frame
    u32 voxel_missing = 0;        // bricks that did not fit in the pool (built as room frees up)
    bool voxel_overflow = false;  // voxel_missing > 0: the pool is full
    float voxel_cpu_ms = 0.0f;    // voxel work this frame
    u32 voxel_rebuilds = 0;       // whole windows rebuilt (too many changes at once, a new stand-in radius), ever
    // The far field (this frame, on the CPU).
    bool far_field = false; // on this frame
    u32 far_culled = 0;     // instances left to the voxel world
    // Sun shadows (this frame, on the CPU; requests are a few frames old).
    bool shadows = false;     // on this frame
    u32 shadow_pages = 0;     // pages with room in the pool
    u32 shadow_capacity = 0;  // the pool, pages
    u32 shadow_drawn = 0;     // pages drawn this frame
    u32 shadow_swayed = 0;    // of them, drawn again for what sways on them (RenderSettings::wind_shadow_pages_per_frame)
    u32 shadow_waiting = 0;   // pages asked for or changed, not drawn yet
    u32 shadow_behind = 0;    // of them, never drawn or changed over 30 frames ago (ShadowStats::behind)
    u32 shadow_missing = 0;   // pages asked for that found no room
    u32 shadow_draws = 0;     // raster passes for them (one per level with pages to draw, two with cut-out materials)
    // Global illumination (this frame, on the CPU).
    bool gi = false;      // on this frame
    u32 gi_lit = 0;       // voxel bricks lit this frame
    u32 gi_cleared = 0;   // bricks new in the pool, cleared to wait for their light
    u32 gi_waiting = 0;   // new or rebuilt bricks not lit yet
    // Reflections (this frame).
    bool reflections = false;        // voxel cones on
    bool screen_reflections = false; // screen-space rays on
    bool ambient_occlusion = false;  // GTAO on
    bool contact_shadows = false;    // the sun's contact shadows marched
    u32 occluders = 0;               // characters' capsules shading the light round them
    // The sky (this frame).
    bool sky = false;           // the atmosphere drawn
    bool physical_sky = false;  // with light scattered many times, the air over the view and the sky's ambient light
    // Exposure and bloom (this frame).
    bool auto_exposure = false;   // the scene's brightness measured this frame
    float exposure = 0.0f;        // the light's multiplier before tone mapping (Lighting::exposure included)
    float scene_stops = 0.0f;     // the brightness auto exposure last read (log2 of the light, before exposure)
    bool exposure_measured = false; // a measurement has been read back (scene_stops holds it)
    bool bloom = false;
    bool clouds = false;        // clouds marched
    bool cloud_shadows = false; // and their shadows cast
    bool falling = false;       // rain or snow drawn falling
    bool wet_surfaces = false;  // wet or snowy surfaces
    bool weather_cover = false; // the cover map made (sheltered places stay dry)
    u32 dust_puffs = 0;         // dust puffs drawn
    u32 particles = 0;          // alive (the ring's span from the oldest alive to the newest)
    u32 particles_born = 0;     // born this frame
    u32 particles_dropped = 0;  // left out this frame: past the caps a frame, bad numbers or styles, too far
    u32 particles_cut = 0;      // alive ones gone early this frame: made way (past k_max_particles), or the camera went over 4 km from where they were kept
    bool particles_drawn = false; // their passes ran this frame (the lit view, the setting on, some alive)
    u32 lights = 0;             // local lights shaded this frame (in view, near enough)
    bool light_overflow = false; // too many: the furthest were left out of some clusters or of the frame
    u32 light_lamps = 0;         // lamps the lights stand for (far lamps merged: each counted)
    u32 lights_merged = 0;       // of the lights, far lamps merged into one (src/lights.h)
    u32 local_shadows = 0;        // lights shaded with shadows this frame
    u32 local_shadow_drawn = 0;   // shadow tile faces drawn this frame
    u32 local_shadow_culls = 0;   // GPU cullings they took (one per light: its faces share it)
    u32 local_shadow_waiting = 0; // faces of shadowed lights not drawn yet, or to draw again
    u32 local_shadow_tiles = 0;   // tiles in use (of RendererDesc::local_shadow_tiles)
    u32 gi_lamps = 0;             // lamps lighting the voxel bricks lit this frame (global illumination)
    u32 water_triangles = 0;      // water triangles drawn (in view or not: the GPU clips them)
    bool water_overflow = false;  // past k_max_water_vertices or _triangles: some left out
    u32 water_dropped = 0;        // triangles left out for bad indices or numbers
    bool underwater = false;      // some of the view was under water (FrameInput::underwater)
    u32 skinned_meshes = 0;     // skinned meshes added (live)
    u32 posed = 0;              // of them, posed on the GPU this frame (a new pose, or just loaded)
    u32 posed_clusters = 0;     // clusters the pose pass went over (shown depths only; hidden groups among them skipped)
};

// What the voxel world holds at a point, on one clipmap level
// (Renderer::probe_voxels): "outside" its window, "building" (not built
// yet: the next coarser level stands in), "empty" or "solid".
struct VoxelProbeLevel {
    u32 level = 0;
    double voxel_size = 0.0; // metres
    const char* state = "outside";
};

struct RendererDesc {
    // GPU memory for streamed cluster geometry. Every mesh's coarsest level
    // must fit; more keeps more detail around.
    u64 geometry_pool_bytes = 256ull << 20;
    // At most this much is loaded per frame (the rest waits a frame).
    u64 stream_bytes_per_frame = 32ull << 20;
    // Finer texture levels uploaded per frame (Renderer::stream_texture;
    // at least a row of blocks while any wait), and textures made per frame
    // for level changes (each a new GPU resource, into which the levels it
    // keeps are copied on the GPU: copies not counted in the bytes).
    u64 texture_stream_bytes_per_frame = 8ull << 20;
    u32 texture_changes_per_frame = 8;
    // Painted terrain tiles uploaded per frame (Renderer::set_terrain_paint;
    // at least one while any wait).
    u64 terrain_paint_bytes_per_frame = 8ull << 20;
    // The voxel world: its levels, and GPU memory for its bricks (2176
    // bytes each; the default is 68 MiB).
    voxel::ClipmapDesc voxel_clipmap{};
    u32 voxel_bricks = 32768;
    // Sun shadow pages of 128 x 128 depths (64 KiB each; the default is
    // 64 MiB), made on the first frame with shadows.
    u32 shadow_pages = 1024;
    // Local lights' shadow tiles of 256 x 256 depths (256 KiB each; the
    // default is 48 MiB): a spot light takes one, a point light six. Made
    // on the first frame with a shadowed light.
    u32 local_shadow_tiles = 192;
};

// Where render()'s CPU time goes (Renderer::last_cpu_ms), for finding what
// makes a slow frame.
enum class CpuSection : u8 {
    cells,       // which cells changed
    streaming,   // geometry and texture streaming
    voxels,      // the voxel world's updates (voxel_ms_per_frame)
    gi_plan,     // which bricks to light
    instances,   // which instances to draw
    shadow_plan, // shadow pages to draw
    store,       // the GPU instance store
    tables,      // frame constants, materials, decals, the mesh table
    light,       // voxel levels and lighting the voxel world
    culling,     // culling and raster
    shadows,     // drawing shadow pages
    sky,         // sky, cloud shadows, clouds
    shading,     // shading, reflections, anti-aliasing
    particles,   // particles born and sent to the GPU
    count
};
inline constexpr std::array<const char*, static_cast<usize>(CpuSection::count)> k_cpu_section_names = {
    "cells", "streaming", "voxels", "gi_plan", "instances", "shadow_plan", "store",
    "tables", "light",     "culling", "shadows", "sky",       "shading", "particles"};
using CpuSectionMs = std::array<float, static_cast<usize>(CpuSection::count)>;

class Renderer {
    struct ConstructKey {
        explicit ConstructKey() = default;
    };

public:
    // Null (and logs why) if a pipeline cannot be created.
    static std::unique_ptr<Renderer> create(rhi::Device& device, const RendererDesc& desc = {});
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    MeshId builtin(BuiltinMesh mesh) const;

    // Uploads a mesh; k_no_mesh if it is empty or the GPU is out of memory.
    // A removed mesh's id is handed out again, so callers must stop drawing
    // an id once they remove it (an instance left with it would draw
    // whatever mesh comes next). Safe mid-frame.
    MeshId add_mesh(geometry::ClusterMesh mesh);
    // A skinned mesh (geometry::build_skinned_clusters, its original
    // triangles alone or with its levels of detail), with `joints` joints.
    // Each is its own copy, one per character: set_pose poses it on the GPU
    // (its vertices, and each cluster's culling sphere refit), and every
    // instance of it shows that pose. It shows the bind pose until posed.
    // With levels, it is drawn with LOD like any mesh: the LOD spheres stay
    // at bind, widened by how far the pose moves any vertex (from the
    // joints; errors stay the bind pose's), and only depths a view draws are
    // posed (the streamer's floor, aimed by the camera: finer ones show a
    // frame or a few after the camera, or shadows already drawing them,
    // first need them, coarser clusters standing in meanwhile). It is never left to the far field
    // and the voxel world does not hold it (no bounce light from it); it
    // casts shadows, its pages drawn again when the pose changes. k_no_mesh
    // when empty, not one weight per vertex, weights not summing to 255, a
    // weighted joint out of range, no joints or more than
    // k_max_skin_joints, or its clusters not depth by depth.
    // Its morph targets (mesh.morphs, at most geometry::k_max_morph_targets)
    // are added to its bind vertices and normals before skinning, by their
    // weights (set_pose; until then 0: the base shape, as a file's rest-pose
    // mesh is; callers apply MorphTarget::weight); refused when a delta
    // names a vertex out of range, twice, moves it past 1 km, or is not
    // finite.
    MeshId add_skinned_mesh(geometry::SkinnedClusterMesh mesh, u32 joints);
    // The pose for a skinned mesh's next frames: one matrix per joint, from
    // bind space to the mesh's space (anim::skin_matrices), a sphere around
    // the posed mesh in its space (anim::pose_bounds: without its morphs,
    // which widen it here by the most they move a vertex), and one weight
    // per morph target (empty: the weights it has; at most
    // k_max_morph_weight either way). False (and nothing changes) for a
    // mesh not skinned, the wrong counts, numbers that are not finite, a
    // weight past that, or a negative radius.
    bool set_pose(MeshId mesh, std::span<const Mat4> skin, const geometry::Sphere& bounds,
                  std::span<const float> morph_weights = {});
    // A skinned mesh's morph targets (0 for any other id).
    u32 morph_targets(MeshId mesh) const;
    // Another copy of a skinned mesh, with its own pose (bind until posed),
    // sharing the original's geometry: its CPU clusters, streamed layout,
    // LOD tree and skin source (only its pose, cluster bounds, residency
    // and place in the geometry pool are its own). A crowd of one character
    // costs its geometry once. The original may be removed before its
    // copies. k_no_mesh when `of` is not a skinned mesh.
    MeshId add_skinned_copy(MeshId of);
    bool skinned(MeshId mesh) const;
    // Built-in meshes stay. Unknown or removed ids are ignored.
    void remove_mesh(MeshId mesh);
    // Uploads a sampled texture for materials (texture::Cooked's desc and
    // contents: any format rhi samples, mips and all); k_no_texture (logged)
    // if rhi refuses it or it is an array. Call it, like remove_texture, from
    // the thread that calls render(), between frames or inside one (but not
    // while render() runs).
    TextureId add_texture(const rhi::TextureDesc& desc, std::span<const u8> contents);
    // A streamed texture: `desc` is the whole texture, `contents` its levels
    // from `first_mip` down (up to coarsest_first_mip), as rhi lays them out
    // (texture_from_mip's contents).
    TextureId add_texture(const rhi::TextureDesc& desc, std::span<const u8> contents, u32 first_mip);
    // Brings a texture's finer levels: `levels` holds those from
    // `first_mip` to the one above its first resident level, packed as in
    // the whole texture's contents. render() makes a texture of them that
    // takes the resident levels over on the GPU and uploads the new ones a
    // row of blocks at a time under texture_stream_bytes_per_frame, oldest
    // change first; the texture draws what it had until it is whole, then
    // the new one. False (logged, nothing changes) for an unknown id, a
    // level not finer, or the wrong size. A change under way is replaced.
    bool stream_texture(TextureId texture, u32 first_mip, std::vector<u8> levels);
    // Drops a texture's levels finer than `first_mip` (no upload: the rest
    // are copied on the GPU at the next render()); a finer change under way
    // is given up (the first resident level itself: only that). False for
    // an unknown id, a level finer than its first resident one, or past
    // coarsest_first_mip.
    bool trim_texture(TextureId texture, u32 first_mip);
    std::optional<TextureResidency> texture_residency(TextureId texture) const;
    // Unknown or removed ids are ignored. Frames in flight finish with it;
    // its id then never resolves again (until its slot's generations wrap,
    // after 4094 reuses).
    void remove_texture(TextureId texture);

    // Painted terrain layers: the ground is tiles `tile_size` metres square
    // from the origin on x and z, `samples` a side (tile_size / samples
    // apart, sample (0, 0) at the tile's corner; between a tile's last and
    // the next tile's first, both blend). A change of grid drops all paint.
    void set_terrain_paint_grid(double tile_size, u32 samples);
    // A tile's painted weights: samples^2 * k_terrain_paint_channels bytes,
    // sample (i, j) layer L at (j * samples + i) * 8 + L - 1; empty removes
    // it. Each painted layer covers the ones before it where its weight is
    // above its bands' coverage (the larger counts). Uploaded in render()
    // under terrain_paint_bytes_per_frame (a tile draws its old weights, or
    // none, until then). False (nothing changes) for the wrong size or
    // before a grid is set. From the thread that calls render().
    bool set_terrain_paint(i32 tile_x, i32 tile_z, std::vector<u8> weights);
    void clear_terrain_paint();
    usize terrain_paint_tiles() const;   // painted, uploaded or not
    usize terrain_paint_waiting() const; // not uploaded yet
    // The whole texture's description (all its levels, resident or not);
    // null for an unknown or removed id; valid until it is removed.
    const rhi::TextureDesc* texture(TextureId texture) const;
    // Its coarsest level is loaded, so instances of it draw (a mesh added
    // this frame may take a frame or more under a busy streaming budget).
    bool mesh_ready(MeshId mesh) const;
    // Null for an unknown or removed id.
    const geometry::ClusterMesh* mesh(MeshId mesh) const;

    // Records the frame into `target`: an rgba8_srgb render target (the
    // swap chain's back buffer or an offscreen image). Clears it to the
    // background first. Instances with an unknown mesh are skipped.
    void render(rhi::CommandList& cmd, rhi::TextureHandle target, const FrameInput& frame);

    const RenderStats& last_stats() const { return m_stats; }
    // The particles' records on the GPU (src/particles.h; tests read it back).
    rhi::BufferHandle particle_ring() const { return m_particle_ring; }
    // The last render()'s CPU milliseconds by section.
    const CpuSectionMs& last_cpu_ms() const { return m_cpu_ms; }
    // The voxel world at a world position, finest level first.
    std::vector<VoxelProbeLevel> probe_voxels(const DVec3& position) const;
    // The sun's shadow pages (src/shadow_pages.h; tests and tools).
    ShadowPages& shadow_pages() { return *m_shadows; }
    // GPU culling: reads the newest GPU counts now. Only after
    // Device::wait_idle (tests, screenshots), outside a frame.
    void read_gpu_stats();

    // The matrix that takes camera-relative positions to clip space.
    static Mat4 view_projection(const View& view, float aspect);

    // Use create(); public only for make_unique.
    Renderer(ConstructKey, rhi::Device& device, const RendererDesc& desc);

private:
    // What a skinned mesh's pose pass and LOD need of its geometry: the same
    // for every copy (shared).
    struct SkinShape {
        // Per joint, 4 floats: the bind-space sphere around the vertices it
        // moves (radius < 0: none). How far a pose moves any vertex from
        // bind follows from them (set_pose): LOD spheres, at bind, widen by
        // that, so the cut stays whole and is never coarser where things
        // went (errors stay the bind pose's: skin_lod.h pose_reach).
        std::vector<float> joint_spheres;
        // Per depth, plus the end: the first cluster of that depth (clusters
        // come depth by depth), so the pass poses only shown depths; and the
        // runs of clusters of terminal groups (first, end, finest depth in
        // it), posed too where finer than the floor.
        std::vector<u32> depth_first;
        std::vector<std::array<u32, 3>> terminal_runs;
        // Per depth: around its non-terminal groups (skin_lod.h
        // depth_bounds), for the camera's aim.
        std::vector<geometry::LodBounds> depth_lod;
        // Per morph target: the most it moves a bind vertex at weight 1.
        std::vector<float> morph_max;
    };
    // A skinned mesh's pose and its source on the GPU.
    struct Skin {
        rhi::BufferHandle source;     // gpu::SkinVertex each, cluster after cluster
        u32 joints = 0;
        std::vector<float> matrices;  // 12 per joint (3 rows of the 3x4): the pose to draw
        std::vector<float> morph_weights; // per morph target: the pose to draw
        rhi::BufferHandle morph_ranges;  // per source vertex: gpu::MorphRange (only where a target moves something)
        rhi::BufferHandle morph_entries; // gpu::MorphEntry, vertex by vertex
        geometry::Sphere bounds;      // around the pose, in the mesh's space
        u64 version = 1;              // of the pose (set_pose)
        u64 posed = 0;                // the version in the pool; 0: none yet
        geometry::Sphere posed_bounds; // bounds of the pose in the pool (culling and shadows use these)
        float reach = 0.0f;        // of the pose to draw
        float posed_reach = 0.0f;  // of the pose in the pool
        u32 posed_shown = 0;       // the streamer's shown_version the pool's pose covers
        std::shared_ptr<const SkinShape> shape; // the file's, shared by copies
        // Where its instances were when their shadows were last drawn
        // (world box), and with which pose: a change dirties both places.
        bool shadow_known = false;
        DVec3 shadow_min{};
        DVec3 shadow_max{};
        u64 shadow_version = 0;
        u32 shadow_shown = 0;  // and with which groups shown (finer ones loaded or shown redraw it)
    };
    // Buffers copies of a skinned mesh share with it (add_skinned_copy):
    // destroyed with the last mesh holding them.
    struct SharedBuffers {
        rhi::Device* device = nullptr;
        rhi::BufferHandle groups;
        rhi::BufferHandle nodes;
        rhi::BufferHandle source; // skinned: the skin source
        rhi::BufferHandle morph_ranges;  // skinned with morphs
        rhi::BufferHandle morph_entries;
        SharedBuffers() = default;
        SharedBuffers(const SharedBuffers&) = delete;
        SharedBuffers& operator=(const SharedBuffers&) = delete;
        ~SharedBuffers();
    };
    struct Mesh {
        // The CPU copy (CPU culling, stats, editor); copies share it.
        std::shared_ptr<const geometry::ClusterMesh> data;
        std::unique_ptr<Skin> skin; // skinned meshes only
        std::shared_ptr<SharedBuffers> shared; // owns groups, nodes and the skin source
        // Skinned: its gpu::Clusters as laid out, before any pose (a copy's
        // own clusters, refit to its pose, start from these).
        std::shared_ptr<const std::vector<u8>> bind_clusters;
        rhi::BufferHandle clusters;   // gpu::Cluster (streamed: GeometryStreamer); a skinned copy's own
        rhi::BufferHandle groups;     // gpu::Group (shared)
        rhi::BufferHandle nodes;      // gpu::Node (shared)
        u32 tree_levels = 0;          // deepest node, plus one
        float top = 0.0f;             // the highest vertex's y (wind: what sways bends from 0 to this)
        bool live = false;
    };
    // Buffers the GPU culling passes work in, made on first use.
    struct CullBuffers {
        rhi::BufferHandle counters;
        rhi::BufferHandle args;
        rhi::BufferHandle queues[2];
        rhi::BufferHandle visible;
        rhi::BufferHandle occluded_clusters;
        rhi::BufferHandle occluded_instances;
        u32 instance_capacity = 0;
        // Counters copied back each frame, tagged with the device frame.
        rhi::BufferHandle readback[3];
        u64 readback_frame[3] = {~u64{0}, ~u64{0}, ~u64{0}};
    };
    struct FrameData; // this frame's uploads and descriptors (renderer.cpp)

    bool init();
    bool ensure_targets(u32 width, u32 height);
    bool ensure_cull_buffers(CullBuffers& c, u32 instance_count);
    void destroy(Mesh& mesh);
    MeshId add(geometry::ClusterMesh data, const std::vector<geometry::SkinWeights>* weights,
               const std::vector<geometry::MorphTarget>* morphs, u32 joints);
    void update_particles(rhi::CommandList& cmd, const FrameInput& frame); // births and the ring's upload
    void particle_stats(); // into m_stats (again after anything resets it)
    // A skinned mesh's pose bounds and reach from its matrices and morph
    // weights, `bounds` around the pose without morphs.
    static void fit_pose(Skin& skin, const geometry::Sphere& bounds);
    void pose_skins(rhi::CommandList& cmd);           // after streaming, before anything reads the geometry
    void track_skin_shadows(const FrameInput& frame); // after the shadows' own tracking
    void aim_skin_floors(const FrameInput& frame, u32 width, u32 height); // before the streamer's update: the camera's aims
    void draw_voxels(rhi::CommandList& cmd, rhi::TextureHandle target, const FrameData& data,
                     const Vec3& background); // the voxel debug view, in place of shading
    void select_on_cpu(const FrameInput& frame, const Mat4& view_proj, u32 height, u32 frame_number);
    void cull_on_gpu(rhi::CommandList& cmd, const FrameData& data);
    // GPU culling's steps, into `c` (the view's lists, or the shadow pages').
    gpu::CullPush cull_push(const CullBuffers& c, const FrameData& data, const rhi::TransientMemory& frame,
                            bool streaming_requests);
    // Reset, the instances, the trees and the raster arguments of pass 0.
    void cull_first_pass(rhi::CommandList& cmd, CullBuffers& c, const FrameData& data, const gpu::CullPush& base);
    void cull_barriers(rhi::CommandList& cmd, const CullBuffers& c, bool with_args);
    void cull_prepare(rhi::CommandList& cmd, const CullBuffers& c, const gpu::CullPush& base, u32 pass, u32 mode,
                      u32 slot_in, u32 capacity, u32 arg_offset);
    void cull_walk(rhi::CommandList& cmd, const CullBuffers& c, const gpu::CullPush& base, const FrameData& data,
                   u32 pass);
    bool ensure_shadow_buffers(u32 instance_count);
    bool ensure_local_atlas();
    void draw_local_shadows(rhi::CommandList& cmd, FrameData& data, const FrameInput& frame,
                            const gpu::FrameConstants& base);
    // Sun shadows: which pages this frame, then drawing them.
    void plan_shadows(const FrameInput& frame, bool on);
    void sway_shadows(const FrameInput& frame);
    // Uploads the page table and levels into `data` (shading reads them).
    void draw_shadows(rhi::CommandList& cmd, FrameData& data, const FrameInput& frame,
                      const gpu::FrameConstants& base);
    void raster(rhi::CommandList& cmd, const FrameData& data, u32 pass, bool gpu);
    void build_hzb(rhi::CommandList& cmd);
    void smaa(rhi::CommandList& cmd, rhi::TextureHandle target, const Vec3& background); // after shading, into target
    void read_stats_slot(u32 slot);
    void copy_streaming_stats(); // and the voxel world's
    void finish_stats(const FrameData& data);
    bool ensure_voxel_light();
    // Global illumination's voxel lighting. False: the light pool may hold
    // stale words this frame (out of memory), so nothing may read it.
    bool light_voxels(rhi::CommandList& cmd, const FrameData& data, const FrameInput& frame);
    // (Until a frame first has lamps the words are all zero, as made: not
    // written or read.)
    bool lamps_in_voxels() const { return m_gi_on && m_voxel_lamps.valid() && m_voxel_lamps_seen; }
    // The sky's passes before shading (sky view, cloud shadows, clouds):
    // fills `params` (the SkyParams shading reads). False: no sky this frame.
    bool draw_sky(rhi::CommandList& cmd, const FrameData& data, const FrameInput& frame,
                  rhi::TransientMemory& params);
    bool ensure_sky_targets();
    bool ensure_post_targets(); // bloom's levels, sized to the frame
    // Water over `light` (water.h, shaders/water.slang): the light to expose next.
    rhi::TextureHandle draw_water(rhi::CommandList& cmd, const FrameData& data, const FrameInput& frame,
                                  rhi::TextureHandle light);
    // Particles over `light` (src/particles.h, shaders/particles.slang): the light to expose next.
    rhi::TextureHandle draw_particles(rhi::CommandList& cmd, const FrameData& data, const FrameInput& frame, rhi::TextureHandle light);
    // Auto exposure from the newest histogram the GPU is done with, eased;
    // the multiplier this frame's light gets before tone mapping.
    float update_exposure(const FrameInput& frame, bool lit_view);
    // The light before exposure (`light`: rgba16_float, a: 1 lit) measured,
    // bloomed, exposed and tone mapped into `shaded`.
    void post(rhi::CommandList& cmd, const FrameData& data, const FrameInput& frame, rhi::TextureHandle light,
              rhi::TextureHandle shaded, bool lit_view);

    rhi::Device& m_device;
    RendererDesc m_desc;
    std::unique_ptr<GeometryStreamer> m_streamer;
    std::unique_ptr<DecalBins> m_decal_bins; // this frame's, reused
    std::unique_ptr<LightBins> m_light_bins; // this frame's, reused
    std::unique_ptr<LocalShadows> m_local_shadows;
    rhi::BufferHandle m_local_atlas; // u32 depths, tile after tile of 256 x 256 (made when first wanted)
    bool m_local_atlas_failed = false;
    bool m_local_planned = false; // this frame
    bool m_local_were_on = false;
    std::vector<std::pair<DVec3, DVec3>> m_local_changed; // this frame's changed caster boxes
    std::unique_ptr<VoxelScene> m_voxels;
    std::unique_ptr<InstanceStore> m_store; // the instances, on the GPU by cell
    std::unique_ptr<TerrainPaintStore> m_paint; // painted terrain tiles
    std::unique_ptr<ParticleRing> m_particles;  // the particles alive (their births and spans)
    rhi::BufferHandle m_particle_ring;          // their records (k_max_particles, made at init)
    u64 m_meshes_version = 0; // changes on every add_mesh and remove_mesh
    std::vector<CellRange> m_seen_cells; // last frame's FrameInput::cells
    std::vector<CellRange> m_changed_cells;
    u32 m_cells_count = 0;
    u32 m_cells_changed = 0;
    u32 m_request_count = 0; // GPU streaming requests in the slot read last
    u64 m_requests_taken = ~u64{0}; // the frame whose requests were taken last
    // Pipelines.
    // By material kind (gpu::material_kind): two-sided ones cull no faces,
    // cut-out ones drop pixels by alpha. Mesh ones invalid without mesh shaders.
    std::array<rhi::PipelineHandle, 4> m_raster_mesh;
    std::array<rhi::PipelineHandle, 4> m_raster_vertex;
    rhi::PipelineHandle m_resolve;
    rhi::PipelineHandle m_skin; // skin.slang
    u32 m_posed = 0;            // skinned meshes posed this frame
    u32 m_posed_clusters = 0;   // clusters the pose pass went over this frame
    std::vector<MeshId> m_skinned; // the skinned meshes live
    std::vector<float> m_skin_upload; // scratch: a mesh's matrices, then its morph weights
    struct SkinBox {
        DVec3 min{};
        DVec3 max{};
        bool any = false;
    };
    std::vector<SkinBox> m_skin_boxes; // scratch, by MeshId: around a skinned mesh's instances drawn
    std::vector<u32> m_skin_aims;      // scratch, by MeshId: the finest depth the camera may draw (~0: none in view)
    rhi::PipelineHandle m_cull_reset;
    rhi::PipelineHandle m_cull_prepare;
    rhi::PipelineHandle m_cull_instances;
    rhi::PipelineHandle m_cull_traverse;
    rhi::PipelineHandle m_cull_retest;
    rhi::PipelineHandle m_hzb_first;
    rhi::PipelineHandle m_hzb_down;
    rhi::PipelineHandle m_smaa_edges;
    rhi::PipelineHandle m_smaa_weights;
    rhi::PipelineHandle m_smaa_blend;
    rhi::PipelineHandle m_voxel_view;
    rhi::PipelineHandle m_reflect; // screen reflections, into m_scene
    rhi::PipelineHandle m_water_copy; // water (shaders/water.slang): the light under it, then
    rhi::PipelineHandle m_water;      // the surfaces, into m_water_light
    rhi::PipelineHandle m_particle_prepare;   // particles (shaders/particles.slang): each lit once,
    rhi::PipelineHandle m_particle_draw;      // their quads into the half-size sums,
    rhi::PipelineHandle m_particle_composite; // and the sums over the light
    rhi::BufferHandle m_particle_drawn;       // gpu::ParticleDrawn, k_max_particles (made at init)
    rhi::TextureHandle m_particle_accum;      // half size: weighted transparency's sums
    rhi::TextureHandle m_particle_reveal;
    rhi::TextureHandle m_particle_light;      // the frame's size: the light with them over it
    bool m_particles_ready = false;           // those three made for this size
    rhi::PipelineHandle m_resolve_direct; // shading without screen reflections: m_radiance alone
    // After shading (post.slang): auto exposure's histogram, bloom, exposure and tone mapping.
    rhi::PipelineHandle m_histogram_clear;
    rhi::PipelineHandle m_histogram;
    rhi::PipelineHandle m_bloom_down;
    rhi::PipelineHandle m_ao_gtao;
    rhi::PipelineHandle m_ao_denoise;
    rhi::TextureHandle m_ao_raw; // half size: cs_gtao's, then cs_denoise's into m_ao
    rhi::TextureHandle m_ao;
    u32 m_ao_width = 0;
    u32 m_ao_height = 0;
    bool m_ao_ready = false;     // its textures made for this size
    rhi::PipelineHandle m_contact_pass; // contact shadows (ao.slang's cs_contact)
    rhi::TextureHandle m_contact;       // the sun let through, full size
    bool m_contact_ready = false;
    rhi::PipelineHandle m_occlusion_pass; // characters in the light (occluders.slang)
    rhi::TextureHandle m_occlusion;       // its bounce and ambient let through, full size (made on first use)
    bool m_occlusion_logged = false;
    u32 m_occlusion_idle = 0; // frames without the pass (the texture freed after k_occlusion_idle_frames)
    std::vector<gpu::GpuOccluder> m_gpu_occluders;          // this frame's (scratch)
    std::vector<std::pair<float, u32>> m_occluder_order;    // (scratch)
    rhi::PipelineHandle m_terrain_pass;   // terrain layers (terrain.slang's cs_terrain)
    rhi::TextureHandle m_terrain_surface; // their colour and roughness, full size
    rhi::TextureHandle m_terrain_normal;  // and shading normal
    bool m_terrain_ready = false;
    rhi::PipelineHandle m_bloom_up;
    rhi::PipelineHandle m_post;
    rhi::TextureHandle m_smaa_area;   // SMAA's precomputed tables
    rhi::TextureHandle m_smaa_search;
    // Targets, sized to the frame.
    rhi::TextureHandle m_depth;
    rhi::TextureHandle m_vis; // visibility buffer (rg32_uint)
    rhi::BufferHandle m_hzb;  // depth pyramid
    rhi::TextureHandle m_color;   // shaded image before anti-aliasing (rgba8_srgb)
    rhi::TextureHandle m_edges;   // SMAA edges (rgba8_unorm)
    rhi::TextureHandle m_weights; // SMAA blend weights (rgba8_unorm)
    // Shading's output, before exposure (resolve.slang), which the screen
    // reflections pass then finishes (reflect.slang): radiance (a: 1 to
    // tone map, 0 as it is), the glossy pixels' reflection weight and
    // normal, and the reflected light shading used.
    rhi::TextureHandle m_radiance; // rgba16_float
    rhi::TextureHandle m_gloss;    // rg32_uint
    rhi::TextureHandle m_fallback; // rgba16_float
    rhi::TextureHandle m_scene;    // rgba16_float: the light with screen reflections, before exposure
    rhi::TextureHandle m_water_light; // rgba16_float: and with water (made when first drawn)
    rhi::TransientMemory m_shading_extras; // this frame's gpu::ResolveExtras (data null: none yet), which the water reads too
    // Bloom: the light halved k_bloom_levels times (down), and back up
    // (up[i]: down[i] plus up[i + 1] spread; the last level is down's own).
    static constexpr u32 k_bloom_levels = 6;
    rhi::TextureHandle m_bloom_downs[k_bloom_levels];
    rhi::TextureHandle m_bloom_ups[k_bloom_levels - 1];
    u32 m_bloom_width[k_bloom_levels] = {};
    u32 m_bloom_height[k_bloom_levels] = {};
    // Auto exposure: the histogram, read back once the GPU is done with it.
    rhi::BufferHandle m_histogram_bins; // src/exposure.h's bins
    rhi::BufferHandle m_histogram_readback[3];
    u64 m_histogram_frame[3] = {~u64{0}, ~u64{0}, ~u64{0}};
    u64 m_histogram_taken = ~u64{0};
    bool m_exposure_known = false; // m_exposure_stops is eased from a measurement
    float m_exposure_stops = 0.0f; // auto exposure's (log2 of its multiplier)
    float m_exposure_target = 0.0f;
    float m_scene_stops = 0.0f;    // the last measurement
    bool m_scene_measured = false; // m_scene_stops holds one
    bool m_auto_on = false;        // auto exposure on this frame
    bool m_bloom_ready = false;    // bloom's levels made for this size
    float m_frame_exposure = 1.0f; // this frame's multiplier (for the stats)
    bool m_measure_exposure = false; // this frame
    bool m_bloom_on = false;         // this frame
    bool m_post_logged = false;      // bloom's textures could not be made (logged once)
    u32 m_width = 0;
    u32 m_height = 0;
    u32 m_hzb_width = 0;
    u32 m_hzb_height = 0;
    u32 m_hzb_mips = 0;
    // What last frame's HZB was built from, for the first occlusion pass.
    bool m_hzb_valid = false;
    DVec3 m_hzb_camera{};
    float m_hzb_view_rows[12] = {};
    float m_hzb_p00 = 0.0f;
    float m_hzb_p11 = 0.0f;
    float m_hzb_near = 0.0f;
    CullBuffers m_cull;
    // Sun shadows.
    std::unique_ptr<ShadowPages> m_shadows;
    CullBuffers m_shadow_cull; // its own lists: pages are drawn after the view's raster
    rhi::BufferHandle m_shadow_atlas;    // u32 depths, page after page
    rhi::BufferHandle m_shadow_requests; // a bit per page slot, written by shading
    rhi::BufferHandle m_shadow_readback[3];
    u64 m_shadow_readback_frame[3] = {~u64{0}, ~u64{0}, ~u64{0}};
    std::unique_ptr<ShadowWindows[]> m_shadow_readback_windows; // the windows each slot's requests were made in
    u64 m_shadow_requests_taken = ~u64{0};
    bool m_shadows_on = false; // this frame
    bool m_shadows_were_on = false;
    bool m_shadow_buffers_logged = false;
    u32 m_shadow_draws = 0; // this frame
    rhi::PipelineHandle m_shadow_mesh; // invalid without mesh shaders
    rhi::PipelineHandle m_shadow_vertex;
    rhi::PipelineHandle m_shadow_mesh_cut; // cut-out materials' shadows
    rhi::PipelineHandle m_shadow_vertex_cut;
    u64 m_shadow_cut_outs = 0xcbf29ce484222325ull; // the frame's cut-out materials, hashed (a change redraws all pages)
    rhi::PipelineHandle m_shadow_clear_pages;
    rhi::PipelineHandle m_shadow_clear_words;
    std::vector<u8> m_casts; // per frame instance: drawn as a mesh, so it casts
    std::vector<u32> m_shadow_requests_cpu;
    std::vector<u32> m_shadow_table;
    std::vector<gpu::ShadowLevel> m_shadow_levels;
    // Global illumination.
    std::unique_ptr<VoxelLight> m_light_plan;
    rhi::BufferHandle m_voxel_light; // u32 per voxel, beside the brick pool (made on first use)
    bool m_voxel_light_failed = false; // could not be made: not tried again until gi is turned off and on
    // Lamps in it (src/voxel_lamps.h): a lamp word per voxel (made with the
    // first lamp under global illumination), each frame's lists.
    rhi::BufferHandle m_voxel_lamps;
    bool m_voxel_lamps_failed = false;
    bool m_voxel_lamps_seen = false; // some frame has had lamps under global illumination
    std::unique_ptr<VoxelLamps> m_voxel_lamp_lists;
    u32 m_gi_lamps = 0; // lamps listed this frame
    bool m_gi_on = false; // this frame
    rhi::PipelineHandle m_light_clear;
    rhi::PipelineHandle m_light_voxels;
    // The sky.
    rhi::PipelineHandle m_sky_view_pass;
    rhi::PipelineHandle m_cloud_map_pass;
    rhi::PipelineHandle m_cloud_shadow_pass;
    rhi::PipelineHandle m_cover_pass;
    rhi::PipelineHandle m_clouds_pass;
    rhi::PipelineHandle m_sky_multi_pass;
    rhi::PipelineHandle m_sky_aerial_pass;
    rhi::PipelineHandle m_sky_ambient_pass;
    rhi::TextureHandle m_sky_view;     // rgba16_float, the sky's light by direction
    rhi::TextureHandle m_sky_multi;    // rgba16_float, light scattered many times (32 square)
    rhi::TextureHandle m_sky_aerial;   // rgba16_float, the air over the view's light (32 slices of 32 square, side by side)
    rhi::TextureHandle m_sky_aerial_through; // rgba16_float, and what comes through it, by colour
    rhi::TextureHandle m_sky_ambient;  // rgba16_float, 9 by 1: the sky's light from all round (spherical harmonics)
    rhi::TextureHandle m_cloud_map;    // rgba8_unorm, the clouds' pattern (made once)
    rhi::TextureHandle m_cloud_shadow; // rgba16_float, sunlight through the clouds over the ground
    rhi::TextureHandle m_cover;        // r32_float, camera-relative height of what is on top (rain and snow)
    rhi::TextureHandle m_clouds_a;     // rgba16_float, half resolution: the clouds' light and transmittance
    rhi::TextureHandle m_clouds_b;     // rgba16_float, half resolution: their distance and the scene's
    bool m_cloud_map_made = false;
    bool m_sky_ambient_made = false; // filled by a frame before (global illumination reads it before the sky's passes)
    float m_sky_multi_haze = -1.0f;  // the haze the multiple-scattering table was made for (-1: none; it depends on nothing else)
    bool m_sky_failed = false; // its textures could not be made: tried again when the frame's targets are remade
    bool m_overflow_logged = false;
    bool m_cull_buffers_logged = false;
    bool m_instances_logged = false;
    bool m_cpu_clusters_logged = false;
    bool m_materials_logged = false;
    bool m_decals_logged = false;
    bool m_lights_logged = false;
    u32 m_local_drawn = 0; // shadow tile faces drawn this frame
    u32 m_local_culls = 0; // GPU cullings for them (one per light)

    std::vector<Mesh> m_meshes;
    std::vector<u32> m_free_meshes; // removed slots, reused by add_mesh
    struct TextureSlot {
        rhi::TextureHandle handle; // its levels from first_mip down; invalid when removed
        u32 descriptor = rhi::k_no_descriptor;
        bool srgb = false;
        u32 generation = 1; // TextureId's high bits; never 0 or 4095
        rhi::TextureDesc desc; // the whole texture
        u32 first_mip = 0;
        u64 bytes = 0; // of handle
        // A change of levels (render() makes it): `target` the first mip it
        // goes to (first_mip: none). Finer: `next` takes the levels from
        // target down, `levels` the new ones (target to first_mip - 1),
        // `written` bytes of them uploaded so far.
        u32 target = 0;
        rhi::TextureHandle next;
        std::vector<u8> levels;
        u64 written = 0;
        // GPU feedback: the finest level wanted in this window of frames
        // and the last one (k_unseen: none).
        u8 wanted_now = k_unseen;
        u8 wanted_before = k_unseen;
        bool queued = false; // in m_texture_changes
    };
    static constexpr u8 k_unseen = 0xFF;
    // The live slot an id names, or null.
    const TextureSlot* texture_slot(TextureId id) const;
    TextureSlot* texture_slot(TextureId id);
    void release_texture_change(TextureSlot& t);
    void queue_texture_change(u32 slot);
    // render(): level changes, under the per-frame budgets.
    void change_textures();
    // Feedback: what the GPU asked two frames ago, and this frame's buffer.
    void read_texture_feedback();
    void note_wanted(TextureId id, u32 q);
    std::vector<TextureSlot> m_textures; // by TextureId's slot
    std::vector<u32> m_free_textures;    // removed slots, reused by add_texture
    std::deque<u32> m_texture_changes;   // slots with a change waiting or under way, oldest first
    bool m_texture_kind_logged = false;
    u64 m_texture_bytes = 0;             // resident, all slots
    rhi::BufferHandle m_feedback;        // u32 per material: ceil(-log2 footprint) + 1, largest
    rhi::BufferHandle m_feedback_zero;   // zeros, copied over it each frame
    rhi::BufferHandle m_dfg;             // the split-sum BRDF table (src/brdf.h), made at init
    rhi::BufferHandle m_feedback_readback[3];
    u64 m_feedback_frame[3] = {~u64{0}, ~u64{0}, ~u64{0}};
    u64 m_feedback_taken = ~u64{0};
    u64 m_feedback_window = 0; // frames_completed() / window length when the window last turned
    // Per read-back slot: which textures each textured material used.
    struct FeedbackMaterial {
        u32 material = 0;
        std::array<TextureId, 4> textures{};
    };
    std::vector<FeedbackMaterial> m_feedback_materials[3];
    std::vector<FeedbackMaterial> m_feedback_now; // this frame's (moved into its slot)
    u32 m_feedback_count = 0;                     // this frame's materials asked (0: none)
    u64 m_texture_streamed = 0;                   // this frame's level uploads, bytes
    bool m_feedback_on = false;                   // this frame
    std::vector<MeshId> m_builtin; // by BuiltinMesh
    // Per-frame scratch, kept to avoid allocating every frame.
    std::vector<u32> m_valid;    // instances (into frame.instances) with a live mesh, drawn as meshes
    bool m_far_field = false;    // this frame
    std::vector<gpu::VoxelLevel> m_voxel_levels; // this frame's, reused
    u32 m_far_culled = 0;        // this frame: instances left to the voxel world
    std::vector<u32> m_selected; // CPU culling: clusters of one instance
    std::vector<u32> m_entries;  // CPU culling: (instance, cluster) pairs, flattened
    std::vector<u32> m_wanted;   // CPU culling: groups of one instance not loaded
    std::vector<u32> m_touched;  // CPU culling: loaded groups it used
    std::vector<gpu::MeshEntry> m_table; // the mesh table
    std::vector<gpu::Material> m_gpu_materials;
    std::vector<gpu::MaterialWind> m_gpu_wind; // per material, the default last
    std::vector<u8> m_material_sways;          // per material (the default last): 1 if it sways
    struct SwayBox {
        u64 square = 0; // the 32 m square of its centre
        DVec3 min{};
        DVec3 max{};
        float reach = 0.0f;
    };
    std::vector<SwayBox> m_sway_boxes; // this frame's swaying instances near the camera (scratch)
    std::vector<bool> m_used;            // meshes drawn this frame
    RenderStats m_stats;
    CpuSectionMs m_cpu_ms{};
    RenderStats m_gpu_stats; // newest read back
};

} // namespace giant::render
