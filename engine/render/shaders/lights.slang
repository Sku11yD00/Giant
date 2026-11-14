// Local lights (src/lights.h), for shading: lamps binned on the CPU into
// the view's clusters, 16 x 9 screen tiles by 24 slices of depth spaced
// evenly in log depth. A pixel finds its cluster and walks the lights
// listed there; resolve.slang lights the surface with each.
#pragma once
#include "common.slang"

static const uint k_light_columns = 16;
static const uint k_light_rows = 9;
static const uint k_light_slices = 24;
static const uint k_light_bytes = 64; // gpu::Light

struct LocalLights {
    uint buffer;   // lights, clusters and entries (count 0: none)
    uint count;
    uint lights;   // byte offsets in it
    uint clusters; // u32 first entry, u32 count per cluster
    uint entries;  // u32 light index each
    float near_depth;
    float scale;   // slice = floor(log2(depth / near_depth) * scale)
    uint shadows;  // byte offset of the LightShadow records
    uint atlas;    // read descriptor of the shadow tiles (atlas_on 0: none)
    uint atlas_on;
};

struct LocalLight {
    float3 position; // camera-relative
    float range;
    float3 color;    // colour x intensity
    float source_radius;
    float3 direction;
    float spot_scale; // cone: saturate(dot(-l, direction) * spot_scale + spot_offset)^2
    float spot_offset;
    uint shadow;      // 0: none; else 1 + its LightShadow's index
};

LocalLight load_light(ByteAddressBuffer b, uint at) {
    const float4 a = asfloat(b.Load4(at));
    const float4 c = asfloat(b.Load4(at + 16));
    const float4 d = asfloat(b.Load4(at + 32));
    LocalLight l;
    l.position = a.xyz;
    l.range = a.w;
    l.color = c.xyz;
    l.source_radius = c.w;
    l.direction = d.xyz;
    l.spot_scale = d.w;
    const uint2 tail = b.Load2(at + 48);
    l.spot_offset = asfloat(tail.x);
    l.shadow = tail.y;
    return l;
}

// Shadows (src/local_shadows.h): a spot's one tile, a point light's six
// (+x, -x, +y, -y, +z, -z), 256 x 256 depths each (near / distance along
// the face's axis, larger nearer; 0 nothing).
static const uint k_light_shadow_size = 256;
static const uint k_light_shadow_bytes = 32; // gpu::LightShadow

// One depth tap: 1 if `receiver` is as near as what the tile holds there.
float light_shadow_tap(ByteAddressBuffer atlas, uint tile, int2 texel, float receiver) {
    const uint2 t = uint2(clamp(texel, int2(0, 0), int2(k_light_shadow_size - 1, k_light_shadow_size - 1)));
    const float stored = asfloat(atlas.Load((tile * k_light_shadow_size * k_light_shadow_size +
                                             t.y * k_light_shadow_size + t.x) * 4));
    return stored <= receiver * 1.0005 ? 1.0 : 0.0;
}

// Where a camera-relative point `d` from a shadowed light (d = world -
// position) falls in its tiles: the tile, the texel position (texel
// centres at whole numbers), and its depth there (near / along). False:
// not in a drawn face (or nearer than its near plane, or outside a spot's
// tile). `push`: moved along `push_n` by that many texels first (off the
// surface: no acne; or into it).
struct LightTexel {
    uint tile;
    float2 p;
    float receiver;
    float near_dist;
    float along; // the point's distance along the face's axis (before the push)
    float texel; // a texel's size there, metres
};

bool light_texel(LocalLights ls, ByteAddressBuffer b, LocalLight light, float3 d, float3 push_n, float push,
                 out LightTexel o) {
    o.tile = 0u;
    o.p = float2(0.0, 0.0);
    o.receiver = 0.0;
    o.along = 0.0;
    o.texel = 0.0;
    const uint at = ls.shadows + (light.shadow - 1u) * k_light_shadow_bytes;
    const float4 a = asfloat(b.Load4(at));
    const float3 spot_right = asfloat(b.Load3(at + 16));
    const float near_dist = a.x;
    o.near_dist = near_dist;
    const uint tile = asuint(a.y);
    const uint drawn = asuint(a.z);
    const float tan_half = a.w;
    uint face = 0;
    float3 forward = light.direction;
    float3 right = spot_right;
    if (light.spot_scale <= 0.0) {
        // A point light: the cube face its major axis points through.
        const float3 m = abs(d);
        if (m.x >= m.y && m.x >= m.z) {
            face = d.x >= 0.0 ? 0u : 1u;
            forward = float3(d.x >= 0.0 ? 1.0 : -1.0, 0.0, 0.0);
            right = float3(0.0, 0.0, 1.0);
        } else if (m.y >= m.z) {
            face = d.y >= 0.0 ? 2u : 3u;
            forward = float3(0.0, d.y >= 0.0 ? 1.0 : -1.0, 0.0);
            right = float3(1.0, 0.0, 0.0);
        } else {
            face = d.z >= 0.0 ? 4u : 5u;
            forward = float3(0.0, 0.0, d.z >= 0.0 ? 1.0 : -1.0);
            right = float3(1.0, 0.0, 0.0);
        }
    }
    if (((drawn >> face) & 1u) == 0u) {
        return false; // not drawn yet
    }
    float along = dot(d, forward);
    if (along <= near_dist) {
        return false;
    }
    const float texel = 2.0 * tan_half * along / float(k_light_shadow_size);
    o.along = along;
    o.texel = texel;
    d += push_n * (push * texel);
    along = dot(d, forward);
    if (along <= near_dist) {
        return false;
    }
    const float3 up = cross(right, forward);
    const float x = dot(d, right) / (along * tan_half);
    const float y = dot(d, up) / (along * tan_half);
    if (abs(x) > 1.0 || abs(y) > 1.0) {
        return false; // outside the tile (a spot's cone ends before)
    }
    o.tile = tile + face;
    o.p = float2(0.5 + 0.5 * x, 0.5 - 0.5 * y) * float(k_light_shadow_size) - 0.5;
    o.receiver = near_dist / along;
    return true;
}

// How much of a light reaches `world` (camera-relative) on a surface
// facing `surface_n`: 1 lit, 0 shadowed (bilinear between four taps).
// Moved off the surface by about a texel and a half (no acne).
float light_shadow(LocalLights ls, ByteAddressBuffer b, LocalLight light, float3 world, float3 surface_n) {
    LightTexel t;
    if (!light_texel(ls, b, light, world - light.position, surface_n, 1.5, t)) {
        return 1.0; // not drawn yet, or outside it: lit
    }
    const int2 i = int2(floor(t.p));
    const float2 w = t.p - floor(t.p);
    ByteAddressBuffer atlas = read_buffer(ls.atlas);
    const float s00 = light_shadow_tap(atlas, t.tile, i, t.receiver);
    const float s10 = light_shadow_tap(atlas, t.tile, i + int2(1, 0), t.receiver);
    const float s01 = light_shadow_tap(atlas, t.tile, i + int2(0, 1), t.receiver);
    const float s11 = light_shadow_tap(atlas, t.tile, i + int2(1, 1), t.receiver);
    return lerp(lerp(s00, s10, w.x), lerp(s01, s11, w.x), w.y);
}

// How far light from a shadowed light goes through whatever is between it
// and `world` (camera-relative, on a surface facing `surface_n`), in
// metres: the distance along the ray from the nearest surface the tile
// holds to the point (bilinear between four taps), looked up a texel
// and a half into the surface (so an edge's taps land on it, not past
// it), with the sideways part of that push put back (shadow_pages.slang's
// sun_thickness_on says why). -1: not known (not drawn, nothing held,
// outside, or what the tile holds there lies well beyond the point: lit
// straight on).
float light_thickness(LocalLights ls, ByteAddressBuffer b, LocalLight light, float3 world, float3 surface_n) {
    LightTexel t;
    if (!light_texel(ls, b, light, world - light.position, -surface_n, 1.5, t)) {
        return -1.0;
    }
    const float along = t.along;
    // Along the face's axis to along the ray.
    const float to_ray = length(world - light.position) / along;
    const float c = saturate(dot(surface_n, normalize(world - light.position)));
    const float slant = min(1.5 * t.texel * (1.0 - c * c) / max(c, 0.2), 3.0 * t.texel);
    const int2 i = int2(floor(t.p));
    const float2 w = t.p - floor(t.p);
    ByteAddressBuffer atlas = read_buffer(ls.atlas);
    float through[4];
    [unroll] for (uint k = 0; k < 4; ++k) {
        const int2 q = clamp(i + int2(k & 1u, k >> 1u), int2(0, 0),
                             int2(k_light_shadow_size - 1, k_light_shadow_size - 1));
        const float stored = asfloat(atlas.Load((t.tile * k_light_shadow_size * k_light_shadow_size +
                                                 uint(q.y) * k_light_shadow_size + uint(q.x)) * 4));
        const float raw = (along - t.near_dist / max(stored, 1e-20)) * to_ray + slant;
        if (stored <= 0.0 || raw < -2.0 * t.texel) {
            return -1.0;
        }
        through[k] = max(raw, 0.0);
    }
    return lerp(lerp(through[0], through[1], w.x), lerp(through[2], through[3], w.x), w.y);
}

// The pixel's cluster: its first entry and how many (count 0: none, or
// past the last slice).
uint2 light_cluster(LocalLights ls, ByteAddressBuffer b, float2 position, float width, float height, float depth) {
    float slice = 0.0;
    if (depth > ls.near_depth) {
        slice = floor(log2(depth / ls.near_depth) * ls.scale);
    }
    if (slice >= float(k_light_slices)) {
        return uint2(0, 0);
    }
    const uint x = min(uint(position.x * float(k_light_columns) / width), k_light_columns - 1);
    const uint y = min(uint(position.y * float(k_light_rows) / height), k_light_rows - 1);
    const uint cluster = (uint(slice) * k_light_rows + y) * k_light_columns + x;
    return b.Load2(ls.clusters + cluster * 8);
}

// How much of a light reaches `world` from direction `l` (unnormalized
// `to_light`): the windowed inverse square, softened by the source's size,
// and the spot's cone.
float light_falloff(LocalLight light, float3 to_light, float3 l) {
    const float d2 = dot(to_light, to_light);
    const float r2 = light.range * light.range;
    const float q = d2 / r2;
    const float window = saturate(1.0 - q * q);
    float falloff = window * window / (d2 + light.source_radius * light.source_radius);
    const float cone = saturate(dot(-l, light.direction) * light.spot_scale + light.spot_offset);
    return falloff * cone * cone;
}
