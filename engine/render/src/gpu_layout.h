// GPU data layouts shared with the shaders (shaders/common.slang and
// shaders/cull.slang). Change both together; the static_asserts pin sizes.
#pragma once

#include "giant/core/types.h"

namespace giant::render::gpu {

// Frame constants: Frame in common.slang.
struct FrameConstants {
    float view_proj[16];      // column-major
    float view_rows[12];      // 3 rows of float4
    float prev_view_rows[12];
    float planes[20];         // 5 planes
    float camera_delta[4];    // xyz, w: last frame's HZB usable
    float ray_x[4];
    float ray_y[4];
    float ray_z[4];
    float sun_direction[4];
    float sun_light[4];
    float sky_light[4];
    float ground_light[4];
    float exposure;
    u32 debug_view;
    float threshold;
    float pixels_per_unit;
    float near_plane;
    float width;
    float height;
    float p00;
    float p11;
    u32 hzb_width;
    u32 hzb_height;
    u32 hzb_mips;
    u32 instance_count;
    u32 occlusion;
    u32 pool;         // read descriptor of the geometry pool
    u32 frame_number; // marks the groups a frame used
    u32 skip_offset;  // instance buffer byte offset of the not-drawn bits (InstanceStore)
    u32 sky_ambient;  // read descriptor: the sky's light from all round (9 spherical harmonics)
    u32 sky_flags;    // bit 0: ambient light from sky_ambient (else sky_light and ground_light)
    u32 tone_mapper;  // RenderSettings::tone_mapper: 0 PBR Neutral, 1 AgX
    // Wind (giant/core/wind.h; common.slang's wind_offset).
    float wind[4];        // velocity xyz (m/s), w: gustiness
    float wind_clock[4];  // the gusts' drift (x, z metres), the sway and flutter clocks
    float wind_camera[4]; // the camera's place modulo 64 m (xyz), w: RenderSettings::wind_distance (0: none sway)
    u32 wind_table;       // read descriptor of per-material (sway, flutter), the default last
    u32 wind_table_offset;
    u32 wind_table_count; // materials before the default
    // The highest sea's level, camera-relative (k_no_sea: none): voxel
    // cones meet it where they cross it going down (voxel_gi.slang).
    float sea_level;
};
static_assert(sizeof(FrameConstants) == 512);
inline constexpr float k_no_sea = -1.0e30f;

// 64 bytes, kept on the GPU from frame to frame (InstanceStore). The
// normal matrix is worked out in the shader (the cofactors of the model's
// 3x3).
struct Instance {
    float model[12]; // rows of the 3x4 model matrix, translation from the cell anchor
    u32 mesh;
    u32 material; // into the frame's material table (checked on the CPU); top bit: k_always_mesh_bit
    float max_scale;
    u32 anchor; // byte offset in the same buffer of the anchor's camera-relative xyz
};
static_assert(sizeof(Instance) == 64);
// In Instance::material: render::Instance::always_mesh.
inline constexpr u32 k_always_mesh_bit = 0x8000'0000u;

struct Material {
    float base_roughness[4];
    float emissive_metallic[4];
    // Read descriptors of its textures (rhi::k_no_descriptor: none): base
    // colour, normal, roughness and metallic, occlusion.
    u32 textures[4];
    u32 flags; // k_material_terrain: shaded from the frame's terrain layers; k_material_two_sided; k_material_cut_out
    float translucency; // 0 to 1
    float alpha_cutoff; // k_material_cut_out: base colour alpha under it is cut away
    float canopy;       // 0 to 1: shading normal bent out from the instance's bounding sphere
};
static_assert(sizeof(Material) == 64);
inline constexpr u32 k_material_bytes = 64; // resolve.slang's k_material_bytes
inline constexpr u32 k_material_terrain = 1;
inline constexpr u32 k_material_two_sided = 2;
inline constexpr u32 k_material_cut_out = 4;
// A material's kind, as the visibility raster draws it in its own pass
// (RasterPush::kinds): opaque one-sided 0, opaque two-sided 1, cut-out
// one-sided 2, cut-out two-sided 3.
inline constexpr u32 k_material_kinds = 4;
inline constexpr u32 material_kind(u32 flags) {
    return ((flags & k_material_two_sided) != 0u ? 1u : 0u) + ((flags & k_material_cut_out) != 0u ? 2u : 0u);
}

// A terrain layer (terrain.slang's TerrainLayer): FrameInput::terrain_layers.
struct TerrainLayer {
    float color_rough[4];
    u32 textures[4]; // read descriptors: base colour, normal, roughness and metallic, (unused)
    float tiling;    // metres a texture repeat covers
    float min_slope; // degrees
    float max_slope;
    float slope_blend;
    float min_height; // metres above y = 0
    float max_height;
    float height_blend;
    float pad;
    float uv_offset[4]; // frac(camera / tiling) on x, y, z: world uvs continue as the camera moves
};
static_assert(sizeof(TerrainLayer) == 80);

// A material's wind (FrameConstants::wind_table), one per material.
struct MaterialWind {
    float sway;
    float flutter;
};
static_assert(sizeof(MaterialWind) == 8);

// A decal: camera-relative positions to its unit box ([-0.5, 0.5] on each
// axis; it stamps from +Y).
struct Decal {
    float rows[12];
    float color_opacity[4];
    float emissive_roughness[4];
    float metallic;
    float softness;
    u32 shape;
    u32 seed;
};
static_assert(sizeof(Decal) == 96);

// A local light (src/lights.h), camera-relative. Spot: the cone's light is
// saturate(dot(-l, direction) * spot_scale + spot_offset)^2 (a point
// light: scale 0, offset 1).
struct Light {
    float position[3];
    float range;
    float color[3]; // colour x intensity
    float source_radius;
    float direction[3];
    float spot_scale;
    float spot_offset;
    u32 shadow; // 0: none; else 1 + its LightShadow's index
    u32 pad[2];
};
static_assert(sizeof(Light) == 64);

// A local light's shadow tiles (src/local_shadows.h): the first tile (a
// spot has one, a point light six: +x, -x, +y, -y, +z, -z), which are
// drawn (bit per face), the depth's near distance, the face's half width
// (tan) and a spot's right axis.
struct LightShadow {
    float near;
    u32 tile;
    u32 drawn;
    float tan_half;
    float right[3];
    u32 pad;
};
static_assert(sizeof(LightShadow) == 32);

struct MeshEntry {
    u32 residency;  // u32 per group: its geometry's byte offset in the pool, or k_not_resident
    u32 group_base; // first of its groups in the global numbering (usage)
    float top;      // its highest vertex's y (swaying things bend from y = 0 to this)
    u32 clusters;
    u32 groups;
    u32 nodes;
    u32 levels; // 0: removed
    u32 pad1;
    float bounds[4];
};
inline constexpr u32 k_not_resident = 0xFFFF'FFFFu;
static_assert(sizeof(MeshEntry) == 48);

// A loaded group's geometry, in the pool: per cluster, its vertices
// (geometry::PackedVertex, 28 bytes: position, octahedral normal and
// tangent, uv) at vertex_offset, then its triangles (u32: three
// local indices, 8 bits apart) at triangle_offset; offsets are bytes from
// the group's start.
struct Cluster {
    u32 vertex_offset;
    u32 vertex_count;
    u32 triangle_offset;
    u32 triangle_count;
    u32 group;
    u32 refined; // k_no_group for original triangles
    u32 level;
    u32 skin_first; // skinned meshes: its first vertex in the skin source (skin.slang); else 0
    float bounds[4]; // skinned meshes: refit to the pose by skin.slang
};
static_assert(sizeof(Cluster) == 48);

struct Group {
    float bounds[4];
    float error;
    u32 first_cluster;
    u32 cluster_count;
    u32 depth;
};
static_assert(sizeof(Group) == 32);

struct Node {
    float bounds[4];
    float error;
    u32 group; // k_no_group for internal nodes
    u32 child_offset;
    u32 child_count;
};
static_assert(sizeof(Node) == 32);

inline constexpr u32 k_no_group = 0xFFFF'FFFFu;

// (instance, cluster) or (instance, node).
struct Entry {
    u32 instance;
    u32 index;
};
static_assert(sizeof(Entry) == 8);

// Counter slots (u32 each) in the counters buffer.
inline constexpr u32 k_slot_zero = 0;
inline constexpr u32 k_slot_visible = 1;
inline constexpr u32 k_slot_visible_pass0 = 2;
inline constexpr u32 k_slot_occluded_instances = 3;
inline constexpr u32 k_slot_occluded_clusters = 4;
inline constexpr u32 k_slot_overflow = 5;
inline constexpr u32 k_slot_triangles = 6;
inline constexpr u32 k_slot_instances = 7;
// Items at tree level k of pass p: slot 8 + 25 p + k. A pass walks at most
// k_max_tree_levels levels; the 25th slot takes the (empty) level below.
inline constexpr u32 k_slot_levels = 8;
inline constexpr u32 k_max_tree_levels = 24;
inline constexpr u32 k_level_slots_per_pass = k_max_tree_levels + 1;
inline constexpr u32 k_slot_range = 60; // base, count of the current raster pass
inline constexpr u32 k_slot_requests = 62; // streaming requests this frame
inline constexpr u32 k_counter_slots = 64;
inline constexpr u32 k_counter_bytes = k_counter_slots * 4;
static_assert(k_slot_levels + 2 * k_level_slots_per_pass <= k_slot_range);

inline constexpr u32 level_slot(u32 pass, u32 level) {
    return k_slot_levels + pass * k_level_slots_per_pass + level;
}

inline constexpr u32 k_overflow_queue = 1;
inline constexpr u32 k_overflow_visible = 2;
inline constexpr u32 k_overflow_occluded_clusters = 4;
inline constexpr u32 k_overflow_occluded_instances = 8;

// Indirect argument offsets (bytes) in the args buffer.
inline constexpr u32 k_args_traverse = 0;
inline constexpr u32 k_args_instances = 16;
inline constexpr u32 k_args_retest = 32;
inline constexpr u32 k_args_raster = 48; // mesh dispatch (12 bytes), then a draw at +16
inline constexpr u32 k_args_bytes = 96;

struct CullPush {
    u32 frame;
    u32 frame_offset;
    u32 instances;
    u32 instance_offset;
    u32 meshes;
    u32 mesh_offset;
    u32 counters;
    u32 queue_in;
    u32 queue_out;
    u32 visible;
    u32 occluded_instances;
    u32 occluded_clusters;
    u32 hzb;
    u32 args;
    u32 pass;
    u32 slot_in;
    u32 slot_out;
    u32 queue_capacity;
    u32 visible_capacity;
    u32 occluded_capacity;
    u32 instance_capacity;
    u32 mode;
    u32 arg_offset;
    u32 requests;
    u32 request_capacity;
    u32 usage;
    u32 pad[2];
};
static_assert(sizeof(CullPush) == 112);

// skin.slang's SkinPush: poses one skinned mesh.
struct SkinPush {
    u32 source;    // read descriptor: SkinVertex each
    u32 clusters;  // write descriptor: the mesh's clusters
    u32 residency; // read descriptor: the mesh's residency table
    u32 pool;      // write descriptor: the geometry pool
    u32 matrices;  // read descriptor (transient): 3 rows of float4 per joint
    u32 matrix_offset;
    u32 cluster_count;
    u32 joint_count;
};
static_assert(sizeof(SkinPush) == 32);

// A skinned mesh's vertex at bind, in the skin source.
// Normal and tangent packed as in geometry::PackedVertex.
struct SkinVertex {
    float position[3];
    u32 normal;
    u32 tangent;
    u16 joints[4];
    u8 weights[4]; // 255ths, summing to 255
};
static_assert(sizeof(SkinVertex) == 32);
inline constexpr u32 k_skin_dispatch_width = 65535; // cluster groups along x (skin.slang)

struct HzbPush {
    u32 depth;
    u32 hzb;
    u32 width;
    u32 height;
    u32 hzb_width;
    u32 hzb_height;
    u32 mip;
    u32 pad;
};
static_assert(sizeof(HzbPush) == 32);

struct RasterPush {
    u32 frame;
    u32 frame_offset;
    u32 instances;
    u32 instance_offset;
    u32 meshes;
    u32 mesh_offset;
    u32 visible;
    u32 visible_offset;
    u32 range;
    u32 range_offset;
    // Shadow pages only (raster.slang's ps_shadow): the atlas (write
    // descriptor; u32 depths, page after page of 128 x 128), the level's
    // page table (its first entry) and its window's minimum page.
    u32 shadow_atlas;
    u32 shadow_table;
    u32 shadow_table_offset;
    i32 shadow_page_x;
    i32 shadow_page_y;
    // Which clusters draw, by their material's kind (bit k for
    // material_kind k, by the frame's material table, below); 0: all.
    u32 kinds;
    u32 materials;
    u32 material_offset;
    u32 material_count;
    // Local light shadow tiles (ps_shadow): their side in texels (0: sun
    // pages); the tile's first word is shadow_table_offset.
    u32 local_size;
};
static_assert(sizeof(RasterPush) == 80);

// shadow_clear.slang's ClearPush: zeroes shadow pages (a list of physical
// pages) or a run of words.
struct ShadowClearPush {
    u32 target; // write descriptor
    u32 list;   // read descriptor of the physical pages (u32 each)
    u32 list_offset;
    u32 count;  // pages, or words
};
static_assert(sizeof(ShadowClearPush) == 16);

struct ResolvePush {
    float background[4];
    u32 frame;
    u32 frame_offset;
    u32 instances;
    u32 instance_offset;
    u32 meshes;
    u32 mesh_offset;
    u32 visible;
    u32 visible_offset;
    u32 vis;
    u32 materials;
    u32 material_offset;
    u32 material_count;
    u32 decals; // decals, tiles and entries share one upload
    u32 decal_offset;
    u32 tile_offset;
    u32 entry_offset;
    u32 tile_columns; // 0: no decals
    u32 tile_rows;
    u32 specular_aa;
    u32 extras; // read descriptor of the ResolveExtras (push constants hold 128 bytes)
    u32 extras_offset;
};
static_assert(sizeof(ResolvePush) == 100);

// The rest of what shading needs, in the frame's transient memory.
struct ResolveExtras {
    // The voxel world (voxel_march.slang), for the far field (beyond
    // far_start it stands in for the meshes left out there) and global
    // illumination.
    u32 voxel_pool;
    u32 voxel_table;
    u32 voxel_levels; // VoxelLevel per level
    u32 voxel_levels_offset;
    u32 voxel_level_count;
    u32 voxel_bricks_per_axis;
    float far_start;  // metres from the camera
    float far_radius; // instances this small are stand-ins: one still drawn ignores its own voxels
    // Sun shadows (shadow_pages.slang). shadow_level_count 0: off, all lit.
    u32 shadow_atlas;  // read descriptor: u32 depths, page after page
    u32 shadow_table;  // read descriptor: u32 entry per page slot, level after level
    u32 shadow_table_offset;
    u32 shadow_levels; // ShadowLevel per level
    u32 shadow_levels_offset;
    u32 shadow_level_count;
    u32 shadow_requests; // write descriptor: a bit per page slot, level after level
    float shadow_texel;  // level 0's texel, metres
    float shadow_x[4];   // the light's axes (camera-relative dot products); x.w: 1 / depth range
    float shadow_y[4];
    float shadow_z[4];   // z.w: the depth offset (depths from the CPU's anchor, ShadowPages::depth_offset)
    // What uses the voxel fields at the top (either sets them).
    u32 far_on;   // 1: the far field marches
    u32 gi_on;    // 1: bounce light from the lit voxels (voxel_gi.slang)
    u32 gi_light; // write descriptor of the voxel light pool (u32 per voxel, brick after brick)
    // Reflections. Bit 0: voxel cones (needs gi_on); bit 1: glossy pixels
    // are marked for the screen reflections pass.
    u32 reflect_on;
    // The sky (sky_common.slang): its SkyParams, or sky_on 0 (the flat background).
    u32 sky_params;
    u32 sky_params_offset;
    u32 sky_on;
    // Dust in the air: dust_count GpuDustPuff at dust_offset (0: none).
    u32 dust_puffs;
    u32 dust_offset;
    u32 dust_count;
    // Texture streaming: a write descriptor of u32 per material (the
    // largest ceil(-log2 footprint) + 1 a sampled pixel needed), for the
    // first feedback_materials (0: off).
    u32 texture_feedback;
    u32 feedback_materials;
    // The split-sum BRDF table (src/brdf.h): dfg_size^2 float2.
    u32 dfg;
    u32 dfg_size;
    // Ambient occlusion (ao.slang): its half-size rgba16_float, or ao_on 0.
    u32 ao;
    u32 ao_on;
    // Contact shadows (ao.slang's cs_contact): the sun let through, full
    // size r32_float (contact_on 0: off).
    u32 contact;
    u32 contact_on;
    // Local lights (src/lights.h): light_count gpu::Light, their clusters
    // and entries share one upload (light_count 0: none).
    u32 lights;
    u32 light_count;
    // Terrain layers (terrain.slang): the ground's colour and roughness
    // and its shading normal, full size rgba16_float (terrain_on 0: off).
    u32 terrain_surface;
    u32 terrain_normal;
    u32 terrain_on;
    u32 pad_terrain;
    // The sun shadows' previous view (ShadowPages::has_previous), standing
    // in where the new one's pages are not drawn yet: its axes, prev_x.w 1
    // while there is one (0: none), prev_z.w its depth offset. Its table
    // and levels follow the current view's (level_count * 256 entries and
    // level_count levels on).
    float shadow_prev_x[4];
    float shadow_prev_y[4];
    float shadow_prev_z[4];
    // Local lights, continued: where in the upload, and the depth slices
    // (slice = floor(log2(depth / light_near) * light_scale), 0 nearer).
    u32 light_offset;
    u32 light_cluster_offset; // u32 first entry, u32 count per cluster
    u32 light_entry_offset;   // u32 light index each
    float light_near;
    float light_scale;
    // Their shadows (src/local_shadows.h): LightShadow records at
    // light_shadow_offset in the lights' upload, and the tiles' depths
    // (read descriptor; local_atlas_on 0: none this frame).
    u32 light_shadow_offset;
    u32 local_atlas;
    u32 local_atlas_on;
    // Lamps in the voxel world (src/voxel_lamps.h): the lamp word per voxel
    // beside the light pool, for the far field (gi_lamps_on 0: none).
    u32 gi_lamps;
    u32 gi_lamps_on;
    u32 pad_lamps[2];
};
static_assert(sizeof(ResolveExtras) == 304);

// resolve.slang's dust puff: centre camera-relative.
struct GpuDustPuff {
    float center[3];
    float radius;
    float color[3];
    float density;
};
static_assert(sizeof(GpuDustPuff) == 32);

// sky_common.slang's Sky: what the sky's passes and shading read.
struct SkyParams {
    float sun_direction[3];
    float cloud_cover;
    float sun_light[3];
    float cloud_density;
    float sky_light[3];
    float cloud_base;
    float ground_light[3];
    float cloud_top;
    float cam_x;   // the camera's place in the cloud pattern (within a tile)
    float cam_alt; // its height above y = 0
    float cam_z;
    float cloud_tile;
    float fog_density;
    float fog_falloff;
    float haze;
    float cloud_steps;
    float shadow_origin[2]; // camera-relative x, z of the cloud shadow map's corner
    float shadow_size;
    u32 flags; // bit 0: clouds; bit 1: cloud shadows
    float half_size[2];
    float full_size[2];
    u32 sky_view; // read descriptors
    u32 cloud_map;
    u32 cloud_shadow;
    u32 clouds_a;
    u32 clouds_b;
    u32 depth;
    u32 multi_scatter; // read descriptors: light scattered many times, the air over the view
    u32 aerial;
    float sun_top[3];
    float aerial_far; // metres the air over the view's last slice ends
    // What falls and the ground under it (N3 part 8).
    float rain;       // 0 to 1
    float snow;       // 0 to 1
    float wetness;    // the ground, 0 to 1
    float snow_cover; // 0 to 1
    float wind[2];    // x, z metres a second
    float time;       // seconds (wrapped): what falls moves with it
    u32 cover;        // read descriptor: the cover map (camera-relative height of what is on top, r32)
    float cover_origin[2]; // camera-relative x, z of the cover map's corner
    float cover_size;      // metres it covers
    u32 weather_flags;     // bit 0: falling; bit 1: wet and snowy surfaces; bit 2: the cover map
    float place[2];        // the camera's x, z wrapped at k_weather_wrap metres (patterns on the ground)
    float pixel_angle;     // radians a pixel spans (vertically)
    u32 atmosphere;        // bit 0: light scattered many times; bit 1: the air over the view; bit 2: sky_ambient
    // The voxel world, for the cover map (voxel_march.slang's VoxelWorld).
    u32 voxel_pool;
    u32 voxel_table;
    u32 voxel_levels;
    u32 voxel_levels_offset;
    u32 voxel_level_count;
    u32 voxel_bricks_per_axis;
    u32 aerial_through; // read descriptor: what comes through the air over the view, by colour
    u32 sky_ambient;    // read descriptor: the sky's light from all round (atmosphere bit 2)
    // The night sky: rows taking a world direction into the stars' frame,
    // their brightness, whether the disc is the moon (1) and from where
    // the sun lights it.
    float celestial_x[3];
    float stars;
    float celestial_y[3];
    float moon;
    float celestial_z[3];
    float moon_lit;
    float moon_sun[3];
    float pad1;
};
static_assert(sizeof(SkyParams) == 336);

// sky.slang's SkyPush (all its passes).
struct SkyPush {
    u32 params;
    u32 params_offset;
    u32 frame;
    u32 frame_offset;
    u32 target;   // write descriptor
    u32 target_b; // the clouds' second target
    u32 pad[2];
};
static_assert(sizeof(SkyPush) == 32);

// reflect.slang's ReflectPush: screen reflections, then exposure and tone
// mapping, from shading's targets.
struct ReflectPush {
    u32 frame;
    u32 frame_offset;
    u32 radiance; // read descriptors of shading's targets
    u32 gloss;
    u32 fallback;
    u32 depth;
    u32 on;       // 1: trace screen reflections
    u32 pad;
};
static_assert(sizeof(ReflectPush) == 32);

// post.slang's PostPush (all its passes): the histogram, bloom, exposure
// and tone mapping.
struct PostPush {
    u32 frame;
    u32 frame_offset;
    u32 source; // read descriptor: the light, or the bloom level before
    u32 add;    // read descriptor: the level down at this size (up), the bloom's top (the image)
    u32 target; // write descriptor: the level made, or the histogram's bins
    u32 width;  // of what is made (the histogram: the image)
    u32 height;
    u32 flags;  // bit 0: the first level down; bit 1: AgX; bit 2: bloom
    float strength;
    float levels;
    u32 pad[2];
};
static_assert(sizeof(PostPush) == 48);

// ao.slang's AoPush (all its passes; cs_contact: radius is the contact length, target full size).
struct AoPush {
    u32 frame;
    u32 frame_offset;
    u32 depth;
    u32 source;
    u32 target;
    u32 width;
    u32 height;
    u32 pad0;
    float radius;
    float pad1[3];
};
static_assert(sizeof(AoPush) == 48);

// terrain.slang's TerrainPush.
struct TerrainPush {
    u32 frame;
    u32 frame_offset;
    u32 vis;
    u32 visible;
    u32 visible_offset;
    u32 instances;
    u32 instance_offset;
    u32 meshes;
    u32 mesh_offset;
    u32 materials;
    u32 material_offset;
    u32 material_count;
    u32 layers; // TerrainLayer each
    u32 layers_offset;
    u32 layer_count;
    u32 surface; // write descriptors
    u32 normal;
    u32 width;
    u32 height;
    float camera_y;
    float camera_xz[2]; // modulo 4096 m (terrain.slang's k_macro_period)
    // Painted layers (TerrainPaintStore): the table (a 16-byte header of
    // the painted tiles' bounds, then 16-byte entries, paint_mask + 1 of
    // them) and pool; none: paint_pool k_no_descriptor.
    u32 paint_table;
    u32 paint_table_offset;
    u32 paint_mask;
    u32 paint_pool;
    u32 paint_samples;
    float paint_spacing; // metres between samples
    i32 paint_tile[2];   // the tile the camera is over
    float paint_origin[2]; // the camera from that tile's corner, metres
};
static_assert(sizeof(TerrainPush) == 128);

// smaa.slang's SmaaPush (all three passes).
struct SmaaPush {
    float metrics[4]; // 1 / width, 1 / height, width, height
    u32 color;
    u32 edges;
    u32 weights;
    u32 area;
    u32 search;
    u32 pad[3];
};
static_assert(sizeof(SmaaPush) == 48);

// voxels.slang: one clipmap level, in the frame's levels upload.
struct VoxelLevel {
    float origin[4];   // xyz: the window's minimum corner, camera-relative; w: brick size (m)
    i32 brick[4];      // xyz: the window's minimum brick (slots wrap on it); w: 1 if the far field need look (stand-ins)
    // Bounds around every brick of the level (camera-relative; may be loose,
    // never tight): rays that miss them skip the level. Empty: min > max.
    float filled_min[4];
    float filled_max[4];
};
static_assert(sizeof(VoxelLevel) == 64);

// A sun shadow level (shadow_pages.h): its window of 16 x 16 pages in
// light space. Light-space x and y are the shadow basis's axes.
struct ShadowLevel {
    float origin[2];  // the window's minimum corner, light space, relative to the camera
    float page_size;  // metres
    float texel_size;
    i32 page[2];      // the window's minimum page (slots wrap on it)
    i32 pad[2];
};
static_assert(sizeof(ShadowLevel) == 32);

// voxels.slang's VoxelPush (the voxel debug view).
struct VoxelPush {
    u32 frame;
    u32 frame_offset;
    u32 pool;  // read descriptor of the brick pool (voxel::Brick each)
    u32 table; // read descriptor of the slot tables (u32 brick per slot, level after level)
    u32 levels; // VoxelLevel per level
    u32 levels_offset;
    u32 level_count;
    u32 bricks_per_axis;
    float background[4];
    u32 light;    // write descriptor of the voxel light pool
    u32 light_on; // 1: show the voxels with their light (bounce and the sun bit)
    u32 lamps;    // write descriptor of the lamp words (lamps_on 0: none)
    u32 lamps_on;
};
static_assert(sizeof(VoxelPush) == 64);

// voxel_light.slang: lighting voxel bricks (src/voxel_light.h).
struct LightJob {
    u32 index;    // the brick's pool index
    u32 level;
    u32 lamp_first; // its lamps (src/voxel_lamps.h): u32 entries in LightPush's list
    u32 lamp_count;
    i32 brick[4]; // xyz: its brick coordinate on the level
};
static_assert(sizeof(LightJob) == 32);

struct LightPush {
    u32 frame;
    u32 frame_offset;
    u32 jobs;     // read descriptor: LightJob each (cs_light), or u32 pool indices (cs_clear)
    u32 jobs_offset;
    u32 count;
    u32 pool;     // the voxel world (VoxelWorld in voxel_march.slang)
    u32 table;
    u32 levels;
    u32 levels_offset;
    u32 level_count;
    u32 bricks_per_axis;
    u32 light;    // write descriptor of the light pool
    // Lamps (src/voxel_lamps.h): the lamp word per voxel beside the light
    // pool (write descriptor; lamps_on 0: none), and this frame's lamps
    // (gpu::Light each) and the jobs' entries (u32 light index each).
    u32 lamps;
    u32 lamps_on;
    u32 lamp_lights;
    u32 lamp_lights_offset;
    u32 lamp_entries_offset;
    u32 pad[3];
};
static_assert(sizeof(LightPush) == 80);
inline constexpr u32 k_no_brick = ~0u;  // a slot with nothing in it
inline constexpr u32 k_unbuilt = ~1u;   // a slot not built yet: use the next coarser level

// Water (shaders/water.slang; src/water.h packs them).
struct WaterVertex {
    float position[3]; // camera-relative
    u32 look;          // its look; the top bit: rides the swell (k_water_swell_bit)
    float flow[2];
    float rapids; // 0 to 1
    u32 slope;    // the surface's slope x, z: snorm16 each (low, high) of k_water_slope_most
};
static_assert(sizeof(WaterVertex) == 32);
struct WaterLook {
    float color[3];
    float turbidity;
    float absorption[3];
    float waves;
};
static_assert(sizeof(WaterLook) == 32);
// One travelling wave: height steepness / k * sin(k * dot(dir, xz) + phase),
// xz camera-relative (the camera's own place folded into phase).
struct WaterWave {
    float dir_x;
    float dir_z;
    float k;         // radians a metre (2 pi / wavelength)
    float steepness; // amplitude times k at waves 1
    float phase;     // radians, 0 to 2 pi
    float pad[3];
};
static_assert(sizeof(WaterWave) == 32);
// The sea's swell's waves (camera-relative phase): height a sin(k dir.p + phase).
struct SwellWave {
    float dir_x;
    float dir_z;
    float k;         // radians a metre
    float amplitude; // metres
    float phase;     // radians at the camera, 0 to 2 pi
    float pad[3];
};
static_assert(sizeof(SwellWave) == 32);
struct WaterPush {
    u32 frame;
    u32 frame_offset;
    u32 data;           // the frame's water: vertices, indices, looks, waves (one upload)
    u32 vertices_offset; // bytes into `data`
    u32 indices_offset;
    u32 looks_offset;
    u32 waves_offset;
    u32 look_count;
    u32 light;          // the light before exposure to draw over (read)
    u32 depth;          // the scene depth (reverse-Z)
    u32 wave_count;
    u32 extras;         // shading's ResolveExtras (shadows, voxel light, sky, dust, lamps)
    u32 extras_offset;
    u32 extras_on;      // 0: shading had none (lit by the flat sky, unfogged)
    u32 under_on;       // 1: the near plane dips under the water the camera is in (FrameInput::underwater)
    u32 under_look;     // that water's look (an index into the looks)
    float under_level;  // its surface, camera-relative (metres)
    float flow_clock;   // running water's clock: seconds into its cycle (0 to k_water_flow_period)
    float surf_clock;   // the surf's clock: seconds into a wave's run in (0 to k_water_surf_period)
    float surf;         // how high the surf runs (0 to 1, from the wind)
    u32 swell_offset;   // bytes into `data`: the swell's waves
    u32 swell_count;    // 0: none
    u32 under_swell;    // 1: the camera's water rides the swell
    u32 sea_on;         // 1: this draw is a sea's rings (made from the vertex id), not the vertices
    u32 sea_radii_offset; // bytes into `data`: the rings' radii (floats, the first 0)
    u32 sea_rings;      // how many radii
    float sea_level;    // camera-relative
    u32 sea_look;       // its look (an index into the looks)
};
static_assert(sizeof(WaterPush) == 112);

} // namespace giant::render::gpu
