// The sun's shadows of hair (characters plan C6d): deep opacity maps
// (Yuksel and Keyser), a small tile a drawn groom. A tile spans the ball
// round its groom as the sun sees it; it holds the depth nearest the sun
// at each texel, and how much hair lies within three depths behind that,
// and in all. A point's shade is read between those: none at the front (a
// strand never shades itself), deepening smoothly inwards. hair.slang
// draws the tiles and shades hair by its own groom's; shading
// (shading_extras.slang's hair_shade) shades the world by the nearest
// grooms'.
#pragma once
#include "common.slang"

static const uint k_hair_tile = 128u;          // texels a side (src/hair.h's k_hair_shadow_tile)
static const uint k_hair_tiles_across = 16u;
// The depths behind the front, as shares of the tile's depth (the ball
// across): for a head of hair 0.5 m across, 1, 3 and 9 cm.
static const float3 k_hair_layers = float3(0.02, 0.06, 0.18);
static const float k_hair_layers_end = 0.5;    // by here, all of it
// A point this far behind a texel's front still counts as at it: the front
// is the nearest in the whole texel, and a sheet of hair the sun strikes
// at a slant is that much deeper from one texel to the next.
static const float k_hair_slack = 1.0 / 128.0;

void sun_axes(float3 l, out float3 u, out float3 v) {
    const float3 up = abs(l.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    u = normalize(cross(up, l));
    v = cross(l, u);
}

// Where `p` (camera-relative) lies in the tile of a groom whose hair is
// within `ball` (its middle and radius): texels across and down it, and
// its depth (0 to 1 within the ball, more nearer the sun; past them
// outside it).
float3 in_hair_tile(float4 ball, float3 l, float3 p) {
    float3 u;
    float3 v;
    sun_axes(l, u, v);
    const float3 q = (p - ball.xyz) / ball.w;
    return float3((0.5 + 0.5 * dot(q, u)) * float(k_hair_tile), (0.5 - 0.5 * dot(q, v)) * float(k_hair_tile),
                  0.5 + 0.5 * dot(q, l));
}

// How much hair lies between `at` (in_hair_tile) and the sun: strands
// thick, as if each covered its texel whole. `front` and `sums` are the
// tiles' two textures.
float hair_before(uint front_texture, uint sums_texture, uint tile, float3 at) {
    Texture2D<float> front = read_texture_float(front_texture);
    Texture2D<float4> sums = read_texture(sums_texture);
    const int2 origin = int2(int(tile % k_hair_tiles_across), int(tile / k_hair_tiles_across)) * int(k_hair_tile);
    const float2 t = clamp(at.xy - 0.5, float2(0.0, 0.0), float2(float(k_hair_tile) - 1.0, float(k_hair_tile) - 1.0));
    const int2 base = int2(floor(t));
    const float2 share = t - floor(t);
    const float depth = min(at.z, 1.0);
    float total = 0.0;
    [unroll] for (uint j = 0u; j < 4u; ++j) {
        const int2 texel = min(base + int2(int(j & 1u), int(j >> 1)), int2(int(k_hair_tile) - 1, int(k_hair_tile) - 1));
        const float z0 = front.Load(int3(origin + texel, 0));
        const float4 o = sums.Load(int3(origin + texel, 0));
        const float behind = max(z0 - depth - k_hair_slack, 0.0);
        float before;
        if (behind < k_hair_layers.x) {
            before = o.x * (behind / k_hair_layers.x);
        } else if (behind < k_hair_layers.y) {
            before = lerp(o.x, o.y, (behind - k_hair_layers.x) / (k_hair_layers.y - k_hair_layers.x));
        } else if (behind < k_hair_layers.z) {
            before = lerp(o.y, o.z, (behind - k_hair_layers.y) / (k_hair_layers.z - k_hair_layers.y));
        } else {
            // (All of it by the end of the ball at the latest: a point
            // behind the ball is behind all its hair.)
            const float end = min(k_hair_layers_end, max(z0 - k_hair_slack, k_hair_layers.z + 1e-4));
            before = lerp(o.z, o.w, saturate((behind - k_hair_layers.z) / (end - k_hair_layers.z)));
        }
        const float weight = ((j & 1u) != 0u ? share.x : 1.0 - share.x) * ((j >> 1) != 0u ? share.y : 1.0 - share.y);
        total += max(before, 0.0) * weight;
    }
    return total;
}
