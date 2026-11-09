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
    uint skip_offset;     // byte offset, in the instance buffer, of the bits of instances not drawn this frame
    uint sky_ambient;     // read descriptor: the sky's light from all round (9 by 1, sky.slang's cs_sky_ambient)
    uint sky_flags;       // bit 0: ambient light from sky_ambient (else sky_light and ground_light)
    uint tone_mapper;     // RenderSettings::tone_mapper: 0 PBR Neutral, 1 AgX
    // Wind (wind_offset below).
    float4 wind;          // velocity xyz (m/s), w: gustiness
    float4 wind_clock;    // the gusts' drift (x, z metres), the sway and flutter clocks (turns)
    float4 wind_camera;   // the camera's place modulo 64 m, w: how far out things sway (0: none)
    uint wind_table;      // read descriptor of per-material (sway, flutter), the default last
    uint wind_table_offset;
    uint wind_table_count;
};
static const uint k_frame_bytes = 512;

float4 load4(ByteAddressBuffer b, uint at) { return asfloat(b.Load4(at)); }

// normalize() that returns `fallback` instead of NaN for a zero vector
// (the camera on a surface, or looking straight into the sun).
float3 safe_normalize(float3 v, float3 fallback) {
    const float len2 = dot(v, v);
    return len2 > 1e-12 ? v * rsqrt(len2) : fallback;
}


// Real spherical harmonics, bands 0 to 2 (`j` 0 to 8; a constant `j`
// folds to its one term).
float sh_basis(float3 d, uint j) {
    if (j == 0u) return 0.282095;
    if (j == 1u) return 0.488603 * d.y;
    if (j == 2u) return 0.488603 * d.z;
    if (j == 3u) return 0.488603 * d.x;
    if (j == 4u) return 1.092548 * d.x * d.y;
    if (j == 5u) return 1.092548 * d.y * d.z;
    if (j == 6u) return 0.315392 * (3.0 * d.z * d.z - 1.0);
    if (j == 7u) return 1.092548 * d.x * d.z;
    return 0.546274 * (d.x * d.x - d.y * d.y);
}

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
    const uint4 p3 = b.Load4(at); at += 16;
    const uint4 p4 = b.Load4(at); at += 16;
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
    f.skip_offset = p4.x;
    f.sky_ambient = p4.y;
    f.sky_flags = p4.z;
    f.tone_mapper = p4.w;
    f.wind = load4(b, at); at += 16;
    f.wind_clock = load4(b, at); at += 16;
    f.wind_camera = load4(b, at); at += 16;
    const uint4 w = b.Load4(at);
    f.wind_table = w.x;
    f.wind_table_offset = w.y;
    f.wind_table_count = w.z;
    return f;
}

// The physical sky's light from all round (sky.slang's cs_sky_ambient, 9
// texels of spherical harmonics) for a surface facing `n`, over pi.
float3 sh_irradiance(uint descriptor, float3 n) {
    Texture2D<float4> t = read_texture(descriptor);
    float3 sum = float3(0.0, 0.0, 0.0);
    [unroll] for (uint j = 0; j < 9u; ++j) {
        sum += t.Load(int3(int(j), 0, 0)).rgb * sh_basis(n, j);
    }
    return max(sum, float3(0.0, 0.0, 0.0));
}

// The ambient light a surface facing `n` gets, over pi (its diffuse light
// is its albedo times this): the sky's own from all round (the physical
// sky), or the flat sky and ground colours' hemisphere.
float3 ambient_light(Frame f, float3 n) {
    if ((f.sky_flags & 1u) == 0u) {
        return lerp(f.ground_light.rgb, f.sky_light.rgb, n.y * 0.5 + 0.5);
    }
    return sh_irradiance(f.sky_ambient, n);
}

// The same for two directions (shading's normal and reflection), the
// table read once.
void ambient_light2(Frame f, float3 n, float3 r, out float3 along_n, out float3 along_r) {
    if ((f.sky_flags & 1u) == 0u) {
        along_n = lerp(f.ground_light.rgb, f.sky_light.rgb, n.y * 0.5 + 0.5);
        along_r = lerp(f.ground_light.rgb, f.sky_light.rgb, r.y * 0.5 + 0.5);
        return;
    }
    Texture2D<float4> t = read_texture(f.sky_ambient);
    float3 a = float3(0.0, 0.0, 0.0);
    float3 b = float3(0.0, 0.0, 0.0);
    [unroll] for (uint j = 0; j < 9u; ++j) {
        const float3 c = t.Load(int3(int(j), 0, 0)).rgb;
        a += c * sh_basis(n, j);
        b += c * sh_basis(r, j);
    }
    along_n = max(a, float3(0.0, 0.0, 0.0));
    along_r = max(b, float3(0.0, 0.0, 0.0));
}

// --- Instances (k_instance_bytes each) -----------------------------------------

// Kept from frame to frame, by world cell: the translation is from the
// instance's cell anchor, whose camera-relative position (rewritten each
// frame) sits in the same buffer at byte offset `anchor`.
struct Instance {
    float4 model[3]; // rows of the 3x4 mesh-to-camera-relative matrix (once loaded)
    uint mesh;
    uint material;   // into the frame's material table
    float max_scale; // largest absolute axis scale: bounds and errors grow by it
    bool always_mesh; // never left to the far field (terrain tiles)
};
static const uint k_instance_bytes = 64;

Instance load_instance(ByteAddressBuffer b, uint at) {
    Instance i;
    [unroll] for (uint r = 0; r < 3; ++r) { i.model[r] = load4(b, at + r * 16); }
    const uint4 tail = b.Load4(at + 48);
    i.mesh = tail.x;
    i.material = tail.y & 0x7FFFFFFFu; // the top bit is always_mesh (gpu::k_always_mesh_bit)
    i.always_mesh = (tail.y & 0x80000000u) != 0u;
    i.max_scale = asfloat(tail.z);
    const float3 anchor = asfloat(b.Load3(tail.w));
    i.model[0].w += anchor.x;
    i.model[1].w += anchor.y;
    i.model[2].w += anchor.z;
    return i;
}

// False for instances not drawn this frame (their mesh is not ready, or
// the voxel world stands in for them): a bit each, after the instances.
bool instance_drawn(Frame f, ByteAddressBuffer b, uint index) {
    return (b.Load(f.skip_offset + (index >> 5) * 4) & (1u << (index & 31u))) == 0u;
}

float3 to_world(Instance i, float3 p) {
    const float4 p1 = float4(p, 1.0);
    return float3(dot(i.model[0], p1), dot(i.model[1], p1), dot(i.model[2], p1));
}

// The normal matrix is the inverse transpose of the model's 3x3; its
// cofactor matrix is that times the determinant, which normalising removes
// (the sign is kept, so mirrored instances keep outward normals). Returns a
// unit vector, or zero.
float3 to_world_normal(Instance i, float3 n) {
    const float3 c0 = float3(i.model[0].x, i.model[1].x, i.model[2].x);
    const float3 c1 = float3(i.model[0].y, i.model[1].y, i.model[2].y);
    const float3 c2 = float3(i.model[0].z, i.model[1].z, i.model[2].z);
    const float3 k0 = cross(c1, c2);
    const float3 k1 = cross(c2, c0);
    const float3 k2 = cross(c0, c1);
    const float det = dot(c0, k0);
    // Normalised here, relative to its own length: the cofactors grow with
    // the square of the scale, so a millimetre-scale instance must not fall
    // under an absolute threshold later.
    const float3 r = (k0 * n.x + k1 * n.y + k2 * n.z) * (det < 0.0 ? -1.0 : 1.0);
    const float len2 = dot(r, r);
    return len2 > 0.0 ? r * rsqrt(len2) : r;
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
// A loaded group's geometry, in the pool: per cluster, its vertices
// (k_vertex_bytes each: position as floats, normal and tangent as
// octahedral snorm16 pairs, the tangent's lowest y bit the bitangent sign,
// uv as floats: geometry::PackedVertex) at vertex_offset, and its triangles
// (u32 each: three cluster-local indices, 8 bits apart) at triangle_offset.

static const uint k_not_resident = 0xFFFFFFFFu;

struct MeshEntry {
    uint residency;
    uint group_base;
    float top; // its highest vertex's y (0 for meshes that never sway)
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
    m.top = asfloat(a.z);
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
    uint skin_first;     // skinned meshes: its first vertex in the skin source (skin.slang)
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
    c.skin_first = d.w;
    c.bounds = load4(b, at + 32);
    return c;
}

// Two snorm16 values in a uint (x low), each in [-1, 1].
float2 unpack_snorm2x16(uint u) {
    return max(float2(float(asint(u << 16) >> 16), float(asint(u) >> 16)) / 32767.0, -1.0);
}
uint pack_snorm2x16(float2 v) {
    const int2 q = int2(round(clamp(v, -1.0, 1.0) * 32767.0));
    return (uint(q.x) & 0xFFFFu) | ((uint(q.y) & 0xFFFFu) << 16);
}
// Octahedral coordinates of a unit vector, and back.
float2 octahedral_encode(float3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    float2 e = n.xy;
    if (n.z < 0.0) {
        e = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    }
    return e;
}
float3 octahedral_decode(float2 e) {
    float3 n = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    const float t = max(-n.z, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}
float3 unpack_normal(uint u) { return octahedral_decode(unpack_snorm2x16(u)); }
// xyz the tangent, w the bitangent sign.
float4 unpack_tangent(uint u) {
    return float4(octahedral_decode(unpack_snorm2x16(u)), (u & 0x10000u) != 0 ? -1.0 : 1.0);
}
uint pack_normal(float3 n) { return pack_snorm2x16(octahedral_encode(n)); }
uint pack_tangent(float3 t, float sign) {
    const uint u = pack_normal(t);
    return (u & ~0x10000u) | (sign < 0.0 ? 0x10000u : 0u);
}

static const uint k_vertex_bytes = 28;

// A cluster's geometry, from its group's place in the pool (`base`).
uint cluster_vertex_at(uint base, Cluster c, uint local_vertex) {
    return base + c.vertex_offset + local_vertex * k_vertex_bytes;
}
float3 cluster_position(ByteAddressBuffer pool, uint base, Cluster c, uint local_vertex) {
    return asfloat(pool.Load3(cluster_vertex_at(base, c, local_vertex)));
}
float3 cluster_normal(ByteAddressBuffer pool, uint base, Cluster c, uint local_vertex) {
    return unpack_normal(pool.Load(cluster_vertex_at(base, c, local_vertex) + 12));
}
float4 cluster_tangent(ByteAddressBuffer pool, uint base, Cluster c, uint local_vertex) {
    return unpack_tangent(pool.Load(cluster_vertex_at(base, c, local_vertex) + 16));
}
float2 cluster_uv(ByteAddressBuffer pool, uint base, Cluster c, uint local_vertex) {
    return asfloat(pool.Load2(cluster_vertex_at(base, c, local_vertex) + 20));
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

// --- Wind (giant/core/wind.h: the same numbers in the same order) ---------------

static const float k_tau = 6.28318530717958647692;
static const float k_gust_tile = 64.0;
static const float k_max_sway_lean = 0.3;

// What one instance does in the wind.
struct Swayer {
    float3 base;   // its origin, camera-relative
    float height;  // metres above it that bend (0: it stands still)
    float sway;
    float flutter;
    float fade;    // 1 near the camera, 0 at wind_distance and past it
};

float wind_wrap(float v) { return v - k_gust_tile * floor(v / k_gust_tile); }

// 0 (a lull) to 1 at a place (any multiple of 64 m off: the same).
float wind_gust(Frame f, float2 xz) {
    const float u = wind_wrap(xz.x - f.wind_clock.x);
    const float v = wind_wrap(xz.y - f.wind_clock.y);
    const float k = k_tau / k_gust_tile;
    return saturate(0.5 + 0.25 * sin(k * (2.0 * u + v)) + 0.15 * sin(k * (-3.0 * u + 5.0 * v) + 1.0) +
                    0.1 * sin(k * (7.0 * u + 4.0 * v) + 2.0));
}

float wind_lean(float sway, float speed) { return sway * (0.004 * speed + 0.00018 * speed * speed); }

float wind_shake(float flutter, float speed) {
    return min(flutter * (0.002 * speed + 0.00008 * speed * speed), 0.12 * flutter);
}

float wind_rock_rate(float height) {
    return clamp(round(16.0 * 2.0 / sqrt(max(height, 0.25))) / 16.0, 0.25, 4.0);
}

Swayer swayer_of(Frame f, Instance i, MeshEntry m) {
    Swayer w;
    w.base = float3(i.model[0].w, i.model[1].w, i.model[2].w);
    w.height = 0.0;
    w.sway = 0.0;
    w.flutter = 0.0;
    w.fade = 0.0;
    const float out_to = f.wind_camera.w;
    if (out_to <= 0.0 || !(m.top > 0.0)) {
        return w;
    }
    w.fade = saturate((out_to - length(w.base)) / (out_to / 3.0));
    if (w.fade <= 0.0) {
        return w;
    }
    const float2 values = asfloat(read_buffer(f.wind_table).Load2(
        f.wind_table_offset + min(i.material, f.wind_table_count) * 8));
    w.sway = values.x;
    w.flutter = values.y;
    if (w.sway > 0.0 || w.flutter > 0.0) {
        w.height = m.top * i.max_scale;
    }
    return w;
}

// How far a point of it (camera-relative, at rest) moves: world::sway_offset.
float3 wind_offset(Frame f, Swayer w, float3 rest) {
    const float speed0 = length(f.wind.xyz);
    if (!(w.height > 0.0) || speed0 <= 1e-4) {
        return float3(0.0, 0.0, 0.0);
    }
    const float3 dir = f.wind.xyz / speed0;
    const float3 side = float3(-dir.z, 0.0, dir.x);
    const float3 base = w.base + f.wind_camera.xyz; // the same modulo 64 m as the world's
    const float3 at = rest + f.wind_camera.xyz;
    const float gust = wind_gust(f, base.xz);
    const float speed = speed0 * (1.0 + f.wind.w * (gust - 0.5) * 1.2);
    const float3 rel = rest - w.base;
    const float h = saturate(rel.y / w.height);
    const float bend = h * h;

    const float phase = k_tau * (7.0 * base.x + 13.0 * base.z) / k_gust_tile;
    const float lean = wind_lean(w.sway, speed);
    const float rock = lean * (0.3 + 0.6 * f.wind.w);
    const float m = wind_rock_rate(w.height);
    const float t = f.wind_clock.z;
    const float swing = sin(k_tau * m * t + phase) + 0.3 * sin(k_tau * 2.0 * m * t + 2.0 * phase);
    float along = clamp(lean + rock * swing, -k_max_sway_lean, k_max_sway_lean);
    float across = rock * 0.35 * sin(k_tau * 1.5 * m * t + 3.0 * phase);
    const float bent = sqrt(along * along + across * across);
    const float hold = bent > k_max_sway_lean ? k_max_sway_lean / bent : 1.0;
    along *= hold;
    across *= hold;
    float3 offset = (dir * along + side * across) * (w.height * bend);
    const float up = max(rel.y, 0.0);
    offset.y = -min(dot(offset, offset) / (2.0 * max(up, 0.01)), 0.5 * up);

    if (w.flutter > 0.0) {
        const float shake = wind_shake(w.flutter, speed);
        const float spread = length(rel.xz);
        const float weight = h * saturate(spread / (0.25 * w.height + 0.01));
        const float k = k_tau / k_gust_tile;
        const float c = f.wind_clock.w;
        const float3 n = float3(sin(k_tau * 3.0 * c + k * (37.0 * at.x + 41.0 * at.y + 23.0 * at.z)),
                                0.5 * sin(k_tau * 4.0 * c + k * (-29.0 * at.x + 43.0 * at.y + 17.0 * at.z)),
                                sin(k_tau * 5.0 * c + k * (19.0 * at.x - 31.0 * at.y + 47.0 * at.z)));
        offset += n * (shake * weight);
    }
    return offset * w.fade;
}

// The most wind_offset can move any point of it: world::max_sway_offset
// (its bounds grow by this).
float wind_reach(Frame f, Swayer w) {
    const float speed0 = length(f.wind.xyz);
    if (!(w.height > 0.0) || speed0 <= 1e-4) {
        return 0.0;
    }
    const float speed = speed0 * (1.0 + 0.6 * f.wind.w);
    const float lean = wind_lean(w.sway, speed);
    const float rock = lean * (0.3 + 0.6 * f.wind.w);
    const float along = min(lean + 1.3 * rock, k_max_sway_lean);
    const float across = 0.35 * rock;
    return (w.height * min(sqrt(along * along + across * across), k_max_sway_lean) * 1.05 + 1.5 * wind_shake(w.flutter, speed)) * w.fade;
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
