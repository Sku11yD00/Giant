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

// How much of a light reaches `world` (camera-relative) on a surface
// facing `surface_n`: 1 lit, 0 shadowed (bilinear between four taps).
float light_shadow(LocalLights ls, ByteAddressBuffer b, LocalLight light, float3 world, float3 surface_n) {
    const uint at = ls.shadows + (light.shadow - 1u) * k_light_shadow_bytes;
    const float4 a = asfloat(b.Load4(at));
    const float3 spot_right = asfloat(b.Load3(at + 16));
    const float near_dist = a.x;
    const uint tile = asuint(a.y);
    const uint drawn = asuint(a.z);
    const float tan_half = a.w;
    float3 d = world - light.position;
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
        return 1.0; // not drawn yet: lit
    }
    float along = dot(d, forward);
    if (along <= near_dist) {
        return 1.0;
    }
    // Moved off the surface by about a texel and a half there (no acne).
    const float texel = 2.0 * tan_half * along / float(k_light_shadow_size);
    d += surface_n * (1.5 * texel);
    along = dot(d, forward);
    if (along <= near_dist) {
        return 1.0;
    }
    const float3 up = cross(right, forward);
    const float x = dot(d, right) / (along * tan_half);
    const float y = dot(d, up) / (along * tan_half);
    if (abs(x) > 1.0 || abs(y) > 1.0) {
        return 1.0; // outside the tile (a spot's cone ends before)
    }
    const float2 p = float2(0.5 + 0.5 * x, 0.5 - 0.5 * y) * float(k_light_shadow_size) - 0.5;
    const int2 i = int2(floor(p));
    const float2 w = p - floor(p);
    const float receiver = near_dist / along;
    ByteAddressBuffer atlas = read_buffer(ls.atlas);
    const uint t = tile + face;
    const float s00 = light_shadow_tap(atlas, t, i, receiver);
    const float s10 = light_shadow_tap(atlas, t, i + int2(1, 0), receiver);
    const float s01 = light_shadow_tap(atlas, t, i + int2(0, 1), receiver);
    const float s11 = light_shadow_tap(atlas, t, i + int2(1, 1), receiver);
    return lerp(lerp(s00, s10, w.x), lerp(s01, s11, w.x), w.y);
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
