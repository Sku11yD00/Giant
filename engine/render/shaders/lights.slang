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
};

struct LocalLight {
    float3 position; // camera-relative
    float range;
    float3 color;    // colour x intensity
    float source_radius;
    float3 direction;
    float spot_scale; // cone: saturate(dot(-l, direction) * spot_scale + spot_offset)^2
    float spot_offset;
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
    l.spot_offset = asfloat(b.Load(at + 48));
    return l;
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
