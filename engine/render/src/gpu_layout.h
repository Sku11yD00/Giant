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
};
static_assert(sizeof(FrameConstants) == 432);

// 64 bytes, so 10,000 trees are 640 KB a frame. The normal matrix is
// worked out in the shader (the cofactors of the model's 3x3).
struct Instance {
    float model[12]; // rows of the 3x4 model matrix (camera-relative)
    u32 mesh;
    u32 material; // into the frame's material table (checked on the CPU)
    float max_scale;
    u32 pad;
};
static_assert(sizeof(Instance) == 64);

struct Material {
    float base_roughness[4];
    float emissive_metallic[4];
};
static_assert(sizeof(Material) == 32);

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

struct MeshEntry {
    u32 residency;  // u32 per group: its geometry's byte offset in the pool, or k_not_resident
    u32 group_base; // first of its groups in the global numbering (usage)
    u32 pad0;
    u32 clusters;
    u32 groups;
    u32 nodes;
    u32 levels; // 0: removed
    u32 pad1;
    float bounds[4];
};
inline constexpr u32 k_not_resident = 0xFFFF'FFFFu;
static_assert(sizeof(MeshEntry) == 48);

// A loaded group's geometry, in the pool: per cluster, its vertices (24
// bytes: position, normal) at vertex_offset, then its triangles (u32: three
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
    u32 pad;
    float bounds[4];
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
    u32 pad[2];
};
static_assert(sizeof(RasterPush) == 48);

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
    u32 pad;
};
static_assert(sizeof(ResolvePush) == 96);

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
    float origin[4]; // xyz: the window's minimum corner, camera-relative; w: brick size (m)
    i32 brick[4];    // xyz: the window's minimum brick (slots wrap on it); w: unused
};
static_assert(sizeof(VoxelLevel) == 32);

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
};
static_assert(sizeof(VoxelPush) == 48);
inline constexpr u32 k_no_brick = ~0u;

} // namespace giant::render::gpu
