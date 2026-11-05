// Data shared by the renderer's shaders: frame constants, instances, the
// mesh table and per-mesh cluster data, and the LOD and culling tests.
// Layouts match engine/render/src/gpu_layout.h; keep the two in step.
#pragma once
#include "giant/bindless.slang"

static const float k_terminal_error = 3.0e38; // clusters that are never replaced

// --- Frame constants (k_frame_bytes, in transient memory) ---------------------

struct Frame {
    float4 view_proj[4];  // columns: camera-relative world to clip
    float4 view_rows[3];  // camera-relative world to view (rows; view looks down -Z)
    float4 prev_view_rows[3]; // the same for the frame the HZB was built from
    float4 planes[5];     // frustum, xyz inward normal, w offset
    float4 camera_delta;  // xyz: this camera minus last frame's camera; w: 1 if last frame's HZB is usable
    float4 ray_x;         // camera-relative ray through pixel ndc (x, y): ray_x * x + ray_y * y + ray_z
    float4 ray_y;
    float4 ray_z;
    float4 sun_direction;
    float4 sun_light;
    float4 sky_light;
    float4 ground_light;
    float exposure;
    uint debug_view;      // 0 lit, 1 clusters, 2 LOD levels, 3 triangles
    float threshold;      // LOD error, pixels
    float pixels_per_unit;
    float near_plane;
    float width;
    float height;
    float p00;            // projection x scale
    float p11;            // projection y scale
    uint hzb_width;       // mip 0
    uint hzb_height;
    uint hzb_mips;
    uint instance_count;
    uint occlusion;       // 1: test against the HZB
    uint pool;            // read descriptor of the geometry pool (streamed cluster data)
    uint frame_number;    // marks the groups a frame used (streaming)
};
static const uint k_frame_bytes = 432;

float4 load4(ByteAddressBuffer b, uint at) { return asfloat(b.Load4(at)); }

Frame load_frame(uint descriptor, uint offset) {
    ByteAddressBuffer b = read_buffer(descriptor);
    Frame f;
    uint at = offset;
    [unroll] for (uint i = 0; i < 4; ++i) { f.view_proj[i] = load4(b, at); at += 16; }
    [unroll] for (uint i = 0; i < 3; ++i) { f.view_rows[i] = load4(b, at); at += 16; }
    [unroll] for (uint i = 0; i < 3; ++i) { f.prev_view_rows[i] = load4(b, at); at += 16; }
    [unroll] for (uint i = 0; i < 5; ++i) { f.planes[i] = load4(b, at); at += 16; }
    f.camera_delta = load4(b, at); at += 16;
    f.ray_x = load4(b, at); at += 16;
    f.ray_y = load4(b, at); at += 16;
    f.ray_z = load4(b, at); at += 16;
    f.sun_direction = load4(b, at); at += 16;
    f.sun_light = load4(b, at); at += 16;
    f.sky_light = load4(b, at); at += 16;
    f.ground_light = load4(b, at); at += 16;
    const float4 p0 = load4(b, at); at += 16;
    const float4 p1 = load4(b, at); at += 16;
    const float4 p2 = load4(b, at); at += 16;
    const uint4 p3 = b.Load4(at);
    f.exposure = p0.x;
    f.debug_view = asuint(p0.y);
    f.threshold = p0.z;
    f.pixels_per_unit = p0.w;
    f.near_plane = p1.x;
    f.width = p1.y;
    f.height = p1.z;
    f.p00 = p1.w;
    f.p11 = p2.x;
    f.hzb_width = asuint(p2.y);
    f.hzb_height = asuint(p2.z);
    f.hzb_mips = asuint(p2.w);
    f.instance_count = p3.x;
    f.occlusion = p3.y;
    f.pool = p3.z;
    f.frame_number = p3.w;
    return f;
}

// --- Instances (k_instance_bytes each) -----------------------------------------

struct Instance {
    float4 model[3];  // rows of the 3x4 mesh-to-camera-relative matrix
    float4 normal[3]; // rows of the normal matrix (w unused)
    float4 base_roughness;
    float4 emissive_metallic;
    uint mesh;
    float max_scale; // largest absolute axis scale: bounds and errors grow by it
};
static const uint k_instance_bytes = 144;

Instance load_instance(ByteAddressBuffer b, uint at) {
    Instance i;
    [unroll] for (uint r = 0; r < 3; ++r) { i.model[r] = load4(b, at + r * 16); }
    [unroll] for (uint r = 0; r < 3; ++r) { i.normal[r] = load4(b, at + 48 + r * 16); }
    i.base_roughness = load4(b, at + 96);
    i.emissive_metallic = load4(b, at + 112);
    const uint2 tail = b.Load2(at + 128);
    i.mesh = tail.x;
    i.max_scale = asfloat(tail.y);
    return i;
}

float3 to_world(Instance i, float3 p) {
    const float4 p1 = float4(p, 1.0);
    return float3(dot(i.model[0], p1), dot(i.model[1], p1), dot(i.model[2], p1));
}

float3 to_world_normal(Instance i, float3 n) {
    return float3(dot(i.normal[0].xyz, n), dot(i.normal[1].xyz, n), dot(i.normal[2].xyz, n));
}

// --- Meshes ------------------------------------------------------------------------
//
// The mesh table (k_mesh_bytes per mesh id) holds each mesh's descriptors:
//   residency: u32 per group: byte offset of its geometry in the pool, or
//              k_not_resident while it is not loaded (streaming)
//   clusters:  k_cluster_bytes each
//   groups:    k_group_bytes each
//   nodes:     k_node_bytes each (the LOD tree; roots are nodes 0 to levels - 1)
// group_base numbers the mesh's groups across all meshes (requests, usage).
//
// A loaded group's geometry, in the pool: per cluster, its vertices (24
// bytes each: position, normal) at vertex_offset, and its triangles (u32
// each: three cluster-local indices, 8 bits apart) at triangle_offset.

static const uint k_not_resident = 0xFFFFFFFFu;

struct MeshEntry {
    uint residency;
    uint group_base;
    uint pad0;
    uint clusters;
    uint groups;
    uint nodes;
    uint levels; // 0: no mesh (removed); nothing is drawn
    uint pad1;
    float4 bounds; // whole mesh: centre, radius
};
static const uint k_mesh_bytes = 48;

MeshEntry load_mesh(ByteAddressBuffer b, uint at) {
    MeshEntry m;
    const uint4 a = b.Load4(at);
    const uint4 c = b.Load4(at + 16);
    m.residency = a.x;
    m.group_base = a.y;
    m.pad0 = a.z;
    m.clusters = a.w;
    m.groups = c.x;
    m.nodes = c.y;
    m.levels = c.z;
    m.pad1 = c.w;
    m.bounds = load4(b, at + 32);
    return m;
}

// Where a group's geometry is in the pool, or k_not_resident.
uint group_offset(MeshEntry m, uint group) {
    return read_buffer_nonuniform(m.residency).Load(group * 4);
}

struct Cluster {
    uint vertex_offset;   // bytes, from its group's geometry
    uint vertex_count;
    uint triangle_offset; // bytes, from its group's geometry
    uint triangle_count;
    uint group;
    uint refined;        // the group it was simplified from; ~0 for original triangles
    uint level;          // depth of its group: 0 holds the original triangles
    uint pad;
    float4 bounds;       // centre, radius
};
static const uint k_cluster_bytes = 48;
static const uint k_no_group = 0xFFFFFFFFu;

Cluster load_cluster(ByteAddressBuffer b, uint index) {
    const uint at = index * k_cluster_bytes;
    Cluster c;
    const uint4 a = b.Load4(at);
    const uint4 d = b.Load4(at + 16);
    c.vertex_offset = a.x;
    c.vertex_count = a.y;
    c.triangle_offset = a.z;
    c.triangle_count = a.w;
    c.group = d.x;
    c.refined = d.y;
    c.level = d.z;
    c.pad = d.w;
    c.bounds = load4(b, at + 32);
    return c;
}

// A cluster's geometry, from its group's place in the pool (`base`).
float3 cluster_position(ByteAddressBuffer pool, uint base, Cluster c, uint local_vertex) {
    return asfloat(pool.Load3(base + c.vertex_offset + local_vertex * 24));
}
float3 cluster_normal(ByteAddressBuffer pool, uint base, Cluster c, uint local_vertex) {
    return asfloat(pool.Load3(base + c.vertex_offset + local_vertex * 24 + 12));
}

struct Group {
    float4 bounds; // of its simplified replacement: centre, radius
    float error;   // of that replacement, metres
    uint first_cluster;
    uint cluster_count;
    uint depth;
};
static const uint k_group_bytes = 32;

Group load_group(ByteAddressBuffer b, uint index) {
    const uint at = index * k_group_bytes;
    Group g;
    g.bounds = load4(b, at);
    const uint4 t = b.Load4(at + 16);
    g.error = asfloat(t.x);
    g.first_cluster = t.y;
    g.cluster_count = t.z;
    g.depth = t.w;
    return g;
}

struct Node {
    float4 bounds; // covers the worst error of every group below
    float error;
    uint group;    // leaf: its group; internal: k_no_group
    uint child_offset;
    uint child_count;
};
static const uint k_node_bytes = 32;

Node load_node(ByteAddressBuffer b, uint index) {
    const uint at = index * k_node_bytes;
    Node n;
    n.bounds = load4(b, at);
    const uint4 t = b.Load4(at + 16);
    n.error = asfloat(t.x);
    n.group = t.y;
    n.child_offset = t.z;
    n.child_count = t.w;
    return n;
}

uint3 unpack_triangle(uint packed) {
    return uint3(packed & 0xFFu, (packed >> 8) & 0xFFu, (packed >> 16) & 0xFFu);
}

uint3 cluster_triangle(ByteAddressBuffer pool, uint base, Cluster c, uint triangle) {
    return unpack_triangle(pool.Load(base + c.triangle_offset + triangle * 4));
}

// --- LOD and culling (as geometry::projected_error and select_clusters) -------------

// Pixels the error of `bounds` (mesh space) covers for instance `i`.
float projected_error(Frame f, Instance i, float4 bounds, float error) {
    if (error >= k_terminal_error) {
        return 1.0e30; // never replaced: always "too coarse to swap for something coarser"
    }
    const float3 c = to_world(i, bounds.xyz);
    const float d = length(c) - bounds.w * i.max_scale;
    return error * i.max_scale / max(d, f.near_plane) * f.pixels_per_unit;
}

bool sphere_in_frustum(Frame f, float3 c, float r) {
    [unroll] for (uint p = 0; p < 5; ++p) {
        if (dot(f.planes[p].xyz, c) + f.planes[p].w < -r) {
            return false;
        }
    }
    return true;
}

// --- Hierarchical depth (HZB) ----------------------------------------------------------
//
// A buffer of floats: mip 0 is the largest power-of-two size not above the
// viewport, each texel the farthest (reverse-Z: smallest) depth over its
// footprint; each further mip halves it, down to 1x1.

uint hzb_mip_offset(uint width, uint height, uint mip) {
    uint offset = 0;
    for (uint m = 0; m < mip; ++m) {
        offset += max(width >> m, 1u) * max(height >> m, 1u);
    }
    return offset;
}

// 2D Polyhedral Bounds of a Clipped, Perspective-Projected 3D Sphere
// (Mara and McGuire, 2013), as in zeux's niagara: the sphere's screen
// rectangle in uv (0..1, y down). `c` is in view space with +z forward.
// False when the sphere reaches the near plane (then it is not culled).
bool project_sphere(float3 c, float r, float near_plane, float p00, float p11, out float4 aabb) {
    aabb = float4(0.0, 0.0, 1.0, 1.0);
    if (c.z < r + near_plane) {
        return false;
    }
    const float3 cr = c * r;
    const float czr2 = c.z * c.z - r * r;
    const float vx = sqrt(c.x * c.x + czr2);
    const float minx = (vx * c.x - cr.z) / (vx * c.z + cr.x);
    const float maxx = (vx * c.x + cr.z) / (vx * c.z - cr.x);
    const float vy = sqrt(c.y * c.y + czr2);
    const float miny = (vy * c.y - cr.z) / (vy * c.z + cr.y);
    const float maxy = (vy * c.y + cr.z) / (vy * c.z - cr.y);
    aabb = float4(minx * p00, miny * p11, maxx * p00, maxy * p11);
    aabb = aabb.xwzy * float4(0.5, -0.5, 0.5, -0.5) + float4(0.5, 0.5, 0.5, 0.5);
    return true;
}

// True when a sphere (camera-relative centre `c`, radius `r`) is certainly
// hidden behind what the HZB holds. `previous`: test against the HZB of
// the last frame (from where that camera stood and looked).
bool occluded(Frame f, ByteAddressBuffer hzb, float3 c, float r, bool previous) {
    if (f.occlusion == 0 || f.hzb_mips == 0 || (previous && f.camera_delta.w == 0.0)) {
        return false; // off, or no HZB from a last frame to test against yet
    }
    float3 v;
    if (previous) {
        const float3 p = c + f.camera_delta.xyz;
        v = float3(dot(f.prev_view_rows[0].xyz, p), dot(f.prev_view_rows[1].xyz, p),
                   -dot(f.prev_view_rows[2].xyz, p));
    } else {
        v = float3(dot(f.view_rows[0].xyz, c), dot(f.view_rows[1].xyz, c), -dot(f.view_rows[2].xyz, c));
    }
    float4 aabb;
    if (!project_sphere(v, r, f.near_plane, f.p00, f.p11, aabb)) {
        return false;
    }
    aabb = saturate(aabb);
    const float w = (aabb.z - aabb.x) * float(f.hzb_width);
    const float h = (aabb.w - aabb.y) * float(f.hzb_height);
    uint mip = uint(clamp(ceil(log2(max(max(w, h), 1.0))), 0.0, float(f.hzb_mips - 1)));
    // At most 2x2 texels at the chosen mip; a step or two coarser if
    // rounding leaves the rectangle across three. Not occluded if it still
    // does not fit (never guess).
    uint2 lo = uint2(0, 0);
    uint2 hi = uint2(0, 0);
    uint2 size = uint2(1, 1);
    bool fits = false;
    for (uint attempt = 0; attempt < 3 && !fits; ++attempt) {
        if (attempt > 0) {
            mip = min(mip + 1, f.hzb_mips - 1);
        }
        size = uint2(max(f.hzb_width >> mip, 1u), max(f.hzb_height >> mip, 1u));
        lo = min(uint2(aabb.xy * float2(size)), size - 1);
        hi = min(uint2(aabb.zw * float2(size)), size - 1);
        fits = hi.x - lo.x <= 1 && hi.y - lo.y <= 1;
    }
    if (!fits) {
        return false;
    }
    const uint base = hzb_mip_offset(f.hzb_width, f.hzb_height, mip);
    float farthest = 1.0;
    for (uint y = lo.y; y <= hi.y; ++y) {
        for (uint x = lo.x; x <= hi.x; ++x) {
            farthest = min(farthest, asfloat(hzb.Load((base + y * size.x + x) * 4)));
        }
    }
    // Reverse-Z with no far plane: depth = near / distance. The sphere's
    // nearest point is hidden when it is farther than everything there.
    const float nearest = f.near_plane / (v.z - r);
    return nearest < farthest;
}

// --- Debug colours -------------------------------------------------------------------

// A well-mixed hash (PCG).
uint hash(uint v) {
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float3 hash_color(uint v) {
    const uint h = hash(v);
    return float3(h & 255u, (h >> 8) & 255u, (h >> 16) & 255u) / 255.0 * 0.8 + 0.2;
}

float3 level_color(uint level) {
    static const float3 k_levels[8] = {
        float3(1.0, 0.15, 0.1), float3(1.0, 0.85, 0.1), float3(0.3, 0.9, 0.2), float3(0.1, 0.85, 0.9),
        float3(0.2, 0.35, 1.0), float3(0.75, 0.3, 1.0), float3(1.0, 0.4, 0.75), float3(0.9, 0.9, 0.9),
    };
    return k_levels[min(level, 7u)];
}
