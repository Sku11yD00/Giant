// Terrain layers (photoreal part 5a): the ground's materials (renderer.h's
// TerrainLayer), stacked like paint by slope and height, mapped by world
// position: from above, and from the sides too where it is steep
// (triplanar, weights |n|^4, so flat ground takes one sample a texture),
// with a slow variation in brightness against the textures' repeats.
// Painted weights (part 5c-2, TerrainPaintStore) cover as bands do, the
// larger counting; a textured layer's edge is sharpened by its texture's
// brightness as a height (stones show through sand before the sand ends).
//
// Its own full-size compute pass (not in resolve's pixel shader: code
// there slows every pixel of the frame on WARP, terrain or not). Per pixel
// whose instance has a terrain material: the triangle from the visibility
// buffer (as resolve.slang's surface_at), the layers' colour and roughness
// into `surface` (rgba16_float: rgb, a roughness) and the shading normal
// into `normal` (rgba16_float: xyz, w 1). Other pixels are left as they
// are: resolve reads these only where the material is terrain.
//
// Layout: TerrainPush in src/gpu_layout.h. Fixed loops, unrolled (WARP),
// but the paint table's probe (short: the table is at most half full).
#include "common.slang"

struct TerrainPush {
    uint frame;
    uint frame_offset;
    uint vis;            // read: the visibility buffer (rg32_uint)
    uint visible;        // read: the visible list
    uint visible_offset;
    uint instances;
    uint instance_offset;
    uint meshes;
    uint mesh_offset;
    uint materials;      // the frame's material table, the default last
    uint material_offset;
    uint material_count;
    uint layers;         // TerrainLayer each, layer_count of them (1 to k_max_terrain_layers)
    uint layers_offset;
    uint layer_count;
    uint surface;        // write: rgba16_float, full size
    uint normal;         // write: rgba16_float, full size
    uint width;
    uint height;
    float camera_y;      // the camera's height (metres above y = 0)
    float2 camera_xz;    // the camera's x and z modulo k_macro_period
    // Painted layers (TerrainPaintStore): a table of 16-byte entries (tile
    // x, z, its block's byte offset in the pool or ~0 empty, 0), paint_mask
    // + 1 of them, open addressing by paint_hash; paint_pool k_no_texture: none.
    uint paint_table;
    uint paint_table_offset;
    uint paint_mask;
    uint paint_pool;
    uint paint_samples;
    float paint_spacing; // metres between samples
    int2 paint_tile;     // the tile the camera is over
    float2 paint_origin; // the camera from that tile's corner, metres
};
ConstantBuffer<TerrainPush> push : register(b0);

static const uint k_material_bytes = 64; // gpu::Material; flags at +48
static const uint k_material_terrain = 1u;
static const uint k_no_texture = 0xFFFFFFFFu; // rhi::k_no_descriptor
static const uint k_max_terrain_layers = 8;
static const uint k_terrain_layer_bytes = 80; // gpu::TerrainLayer
static const float k_macro_period = 4096.0;   // metres: camera_xz's (renderer.cpp)
static const float k_terrain_min_weight = 0.004;

struct TerrainLayer {
    float3 color;
    float rough;
    uint4 textures; // base colour, normal, roughness and metallic
    float tiling;
    float min_slope;
    float max_slope;
    float slope_blend;
    float min_height;
    float max_height;
    float height_blend;
    float3 uv_offset;
};

TerrainLayer load_terrain_layer(uint i) {
    ByteAddressBuffer b = read_buffer(push.layers);
    const uint at = push.layers_offset + i * k_terrain_layer_bytes;
    TerrainLayer l;
    const float4 cr = asfloat(b.Load4(at));
    l.color = cr.rgb;
    l.rough = cr.a;
    l.textures = b.Load4(at + 16);
    const float4 a = asfloat(b.Load4(at + 32));
    l.tiling = max(a.x, 0.05);
    l.min_slope = a.y;
    l.max_slope = a.z;
    l.slope_blend = a.w;
    const float4 h = asfloat(b.Load4(at + 48));
    l.min_height = h.x;
    l.max_height = h.y;
    l.height_blend = h.z;
    l.uv_offset = asfloat(b.Load3(at + 64));
    return l;
}

static const uint k_paint_empty = 0xFFFFFFFFu;
static const uint k_paint_bytes = 8; // a sample: layers 1 to 7, a spare
// How sharp a textured layer's edge is (a part of its coverage's ramp):
// its texture's bright parts (high, as stones over sand) cover first.
static const float k_height_blend = 0.3;

// TerrainPaintStore's terrain_paint_hash.
uint paint_hash(int x, int z) {
    uint h = (uint(x) * 0x9E3779B1u) ^ (uint(z) * 0x85EBCA77u);
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}

// The byte offset of tile (x, z)'s block in the pool, or k_paint_empty.
uint paint_block(int x, int z) {
    ByteAddressBuffer table = read_buffer(push.paint_table);
    uint at = paint_hash(x, z) & push.paint_mask;
    // At most half full: a probe or two.
    for (uint k = 0; k <= push.paint_mask; ++k) {
        const uint4 e = table.Load4(push.paint_table_offset + at * 16);
        if (e.z == k_paint_empty) {
            return k_paint_empty;
        }
        if (int(e.x) == x && int(e.y) == z) {
            return e.z;
        }
        at = (at + 1) & push.paint_mask;
    }
    return k_paint_empty;
}

int floor_div(int v, int n) {
    return v >= 0 ? v / n : -((-v + n - 1) / n);
}

// The painted weights at sample `l` (counted from the camera's tile's
// corner): layers 1 to 4 in `lo`, 5 to 7 in `hi` (w spare).
void paint_sample(int2 l, out float4 lo, out float4 hi) {
    const int n = int(push.paint_samples);
    const int2 t = int2(floor_div(l.x, n), floor_div(l.y, n));
    const uint block = paint_block(push.paint_tile.x + t.x, push.paint_tile.y + t.y);
    if (block == k_paint_empty) {
        lo = float4(0.0, 0.0, 0.0, 0.0);
        hi = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    const int2 i = l - t * n;
    const uint2 v = read_buffer(push.paint_pool).Load2(block + uint(i.y * n + i.x) * k_paint_bytes);
    lo = float4(float(v.x & 255u), float((v.x >> 8) & 255u), float((v.x >> 16) & 255u), float(v.x >> 24)) / 255.0;
    hi = float4(float(v.y & 255u), float((v.y >> 8) & 255u), float((v.y >> 16) & 255u), float(v.y >> 24)) / 255.0;
}

// The painted weights at a point (camera-relative x and z), bilinear
// between samples (and across tiles: a tile's last and the next's first).
void paint_at(float2 xz, out float4 lo, out float4 hi) {
    lo = float4(0.0, 0.0, 0.0, 0.0);
    hi = float4(0.0, 0.0, 0.0, 0.0);
    if (push.paint_pool == k_no_texture) {
        return;
    }
    const float2 s = (push.paint_origin + xz) / push.paint_spacing;
    const float2 base = floor(s);
    const float2 f = s - base;
    const int2 b = int2(base);
    [unroll]
    for (uint k = 0; k < 4; ++k) {
        const int2 d = int2(int(k & 1u), int(k >> 1));
        const float w = (d.x == 1 ? f.x : 1.0 - f.x) * (d.y == 1 ? f.y : 1.0 - f.y);
        if (w > 0.0) {
            float4 a;
            float4 c;
            paint_sample(b + d, a, c);
            lo += w * a;
            hi += w * c;
        }
    }
}

// 1 inside [lo, hi], fading to 0 over `soft` centred on each edge.
float terrain_band(float x, float lo, float hi, float soft) {
    const float s = max(soft, 1e-3);
    return saturate((x - lo) / s + 0.5) * saturate((hi - x) / s + 0.5);
}

// Value noise (0 to 1) over x and z, `cells` cells a period (it wraps
// with k_macro_period, as camera_xz does).
float terrain_noise(float2 p, float cells) {
    const float2 q = p * (cells / k_macro_period);
    const float2 i = floor(q);
    const float2 t = q - i;
    const float2 u = t * t * (3.0 - 2.0 * t);
    const uint n = uint(cells);
    const uint2 c0 = uint2(fmod(i + cells * 64.0, cells));
    const uint2 c1 = (c0 + 1u) % n;
    const float a = float(hash(c0.x + c0.y * 7919u) & 0xFFFFu) / 65535.0;
    const float b = float(hash(c1.x + c0.y * 7919u) & 0xFFFFu) / 65535.0;
    const float c = float(hash(c0.x + c1.y * 7919u) & 0xFFFFu) / 65535.0;
    const float d = float(hash(c1.x + c1.y * 7919u) & 0xFFFFu) / 65535.0;
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float4 terrain_sample(uint descriptor, float2 uv, float2 gx, float2 gy) {
    return read_texture_nonuniform(descriptor).SampleGrad(get_sampler(k_sampler_anisotropic_wrap), uv, gx, gy);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_terrain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= push.width || id.y >= push.height) {
        return;
    }
    const uint2 vis = read_texture_uint2(push.vis).Load(int3(int2(id.xy), 0));
    if (vis.x == 0u) {
        return; // nothing drawn
    }
    const uint2 entry = read_buffer(push.visible).Load2(push.visible_offset + (vis.x - 1) * 8);
    const Instance inst = load_instance(read_buffer(push.instances), push.instance_offset + entry.x * k_instance_bytes);
    const uint material_at = push.material_offset + min(inst.material, push.material_count) * k_material_bytes;
    if ((read_buffer(push.materials).Load(material_at + 48) & k_material_terrain) == 0u) {
        return; // not the ground
    }
    const Frame f = load_frame(push.frame, push.frame_offset);
    // The triangle (resolve.slang's surface_at; the ground does not sway).
    const MeshEntry mesh = load_mesh(read_buffer(push.meshes), push.mesh_offset + inst.mesh * k_mesh_bytes);
    const Cluster cluster = load_cluster(read_buffer_nonuniform(mesh.clusters), entry.y);
    ByteAddressBuffer pool = read_buffer(f.pool);
    const uint geometry = group_offset(mesh, cluster.group);
    const uint3 corners = cluster_triangle(pool, geometry, cluster, vis.y);
    const float3 a = to_world(inst, cluster_position(pool, geometry, cluster, corners.x));
    const float3 b3 = to_world(inst, cluster_position(pool, geometry, cluster, corners.y));
    const float3 c3 = to_world(inst, cluster_position(pool, geometry, cluster, corners.z));
    const float2 position = float2(id.xy) + 0.5;
    const float2 ndc = float2((position.x / f.width) * 2.0 - 1.0, 1.0 - (position.y / f.height) * 2.0);
    const float3 dir = f.ray_x.xyz * ndc.x + f.ray_y.xyz * ndc.y + f.ray_z.xyz;
    const float3 w3 = on_triangle(plane_barycentrics(dir, a, b3, c3).xyz);
    const float3 object_normal = cluster_normal(pool, geometry, cluster, corners.x) * w3.x +
                                 cluster_normal(pool, geometry, cluster, corners.y) * w3.y +
                                 cluster_normal(pool, geometry, cluster, corners.z) * w3.z;
    const float3 geo = safe_normalize(to_world_normal(inst, object_normal), float3(0.0, 1.0, 0.0));
    const float3 world = a * w3.x + b3 * w3.y + c3 * w3.z; // camera-relative
    const float pixel_size = length(world) * 2.0 / max(f.p11 * f.height, 1e-6);
    const float3 dir_dx = dir + f.ray_x.xyz * (2.0 / f.width);
    const float3 dir_dy = dir - f.ray_y.xyz * (2.0 / f.height);

    const uint count = clamp(push.layer_count, 1u, k_max_terrain_layers);
    const float slope = degrees(acos(saturate(abs(geo.y))));
    const float height = world.y + push.camera_y;
    // The footprint: the neighbours' rays met with the surface's plane.
    const float plane = dot(world, geo);
    const float ddx_n = dot(dir_dx, geo);
    const float ddy_n = dot(dir_dy, geo);
    float3 dpdx = abs(ddx_n) > 1e-6 ? dir_dx * (plane / ddx_n) - world : float3(pixel_size, 0.0, 0.0);
    float3 dpdy = abs(ddy_n) > 1e-6 ? dir_dy * (plane / ddy_n) - world : float3(0.0, 0.0, pixel_size);
    // (Grazing: no longer than a few hundred pixels' worth.)
    const float longest = pixel_size * 256.0;
    if (dot(dpdx, dpdx) > longest * longest) dpdx = normalize(dpdx) * longest;
    if (dot(dpdy, dpdy) > longest * longest) dpdy = normalize(dpdy) * longest;
    // Painted weights here (layers 1 to 7).
    float4 paint_lo;
    float4 paint_hi;
    paint_at(world.xz, paint_lo, paint_hi);
    // The stack: each layer covers what is under it where its bands hold,
    // or as far as it is painted (the larger), its edge sharpened by its
    // texture's height.
    float w[k_max_terrain_layers];
    [unroll]
    for (uint i = 0; i < k_max_terrain_layers; ++i) {
        w[i] = 0.0;
        if (i < count) {
            const TerrainLayer l = load_terrain_layer(i);
            float c = 1.0;
            if (i > 0u) {
                const float painted = i <= 4u ? paint_lo[i - 1u] : paint_hi[i - 5u];
                c = max(terrain_band(slope, l.min_slope, l.max_slope, l.slope_blend) *
                            terrain_band(height, l.min_height, l.max_height, l.height_blend),
                        painted);
                if (c > 0.0 && c < 1.0 && l.textures.x != k_no_texture) {
                    // Height: the texture's brightness against its average
                    // (its coarsest level), from above.
                    const float inv = 1.0 / l.tiling;
                    const float2 uv = world.xz * inv + l.uv_offset.xz;
                    const float3 luma = float3(0.2126, 0.7152, 0.0722);
                    const float here = dot(terrain_sample(l.textures.x, uv, dpdx.xz * inv, dpdy.xz * inv).rgb, luma);
                    const float mean = dot(read_texture_nonuniform(l.textures.x)
                                               .SampleLevel(get_sampler(k_sampler_anisotropic_wrap), uv, 16.0)
                                               .rgb,
                                           luma);
                    const float h = saturate(0.5 + 0.5 * (here / max(mean, 1e-3) - 1.0));
                    const float threshold = k_height_blend * 0.5 + (1.0 - k_height_blend) * (1.0 - h);
                    c = saturate((c - threshold) / k_height_blend + 0.5);
                }
            }
            [unroll]
            for (uint j = 0; j < i; ++j) w[j] *= 1.0 - c;
            w[i] = c;
        }
    }
    // Triplanar weights: |n|^4, small ones dropped (flat ground: one plane).
    float3 b = geo * geo;
    b *= b;
    b /= max(b.x + b.y + b.z, 1e-6);
    b *= step(0.02, b);
    b /= max(b.x + b.y + b.z, 1e-6);
    float3 color = float3(0.0, 0.0, 0.0);
    float roughness = 0.0;
    float3 bump = float3(0.0, 0.0, 0.0);
    [unroll]
    for (uint i = 0; i < k_max_terrain_layers; ++i) {
        if (w[i] > k_terrain_min_weight) {
            const TerrainLayer l = load_terrain_layer(i);
            const float inv = 1.0 / l.tiling;
            const float3 p = world * inv + l.uv_offset;
            const float3 gx = dpdx * inv;
            const float3 gy = dpdy * inv;
            float3 c = float3(0.0, 0.0, 0.0);
            float r = 0.0;
            float3 nb = float3(0.0, 0.0, 0.0);
            [unroll]
            for (uint k = 0; k < 3; ++k) {
                const float bw = k == 0u ? b.y : (k == 1u ? b.x : b.z);
                if (bw > 0.0) {
                    // Above: u along +x, v along +z. The sides: v down the slope.
                    const float2 uv = k == 0u ? p.xz : (k == 1u ? float2(p.z, -p.y) : float2(p.x, -p.y));
                    const float2 ux = k == 0u ? gx.xz : (k == 1u ? float2(gx.z, -gx.y) : float2(gx.x, -gx.y));
                    const float2 uy = k == 0u ? gy.xz : (k == 1u ? float2(gy.z, -gy.y) : float2(gy.x, -gy.y));
                    float3 ck = l.color;
                    if (l.textures.x != k_no_texture) ck *= terrain_sample(l.textures.x, uv, ux, uy).rgb;
                    float rk = l.rough;
                    if (l.textures.z != k_no_texture) rk *= terrain_sample(l.textures.z, uv, ux, uy).r;
                    if (l.textures.y != k_no_texture) {
                        // Green up: +y is up the texture (-v). Turned into the world along the plane's axes.
                        const float2 t = terrain_sample(l.textures.y, uv, ux, uy).rg * 2.0 - 1.0;
                        nb += bw * (k == 0u ? float3(t.x, 0.0, -t.y) : (k == 1u ? float3(0.0, t.y, t.x) : float3(t.x, t.y, 0.0)));
                    }
                    c += bw * ck;
                    r += bw * rk;
                }
            }
            color += w[i] * c;
            roughness += w[i] * r;
            bump += w[i] * nb;
        }
    }
    // Brightness drifting slowly over tens and hundreds of metres.
    const float2 xz = world.xz + push.camera_xz;
    const float macro = terrain_noise(xz, 128.0) * 0.6 + terrain_noise(xz, 32.0) * 0.4; // 32 m and 128 m cells
    write_texture(push.surface)[id.xy] = float4(color * lerp(0.82, 1.18, macro), saturate(roughness));
    write_texture(push.normal)[id.xy] = float4(safe_normalize(geo + bump, geo), 1.0);
}
