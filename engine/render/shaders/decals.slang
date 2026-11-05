// Decal stamps, applied while shading (resolve.slang): each pixel tests the
// decals binned into its 16-pixel tile (src/decals.h), in order, and blends
// their material over the surface's. Masks are shapes worked out here, with
// edges anti-aliased to the pixel's footprint, so they stay sharp and calm
// at any distance.
#include "common.slang"

struct Decal {
    float4 rows[3]; // camera-relative position -> unit box ([-0.5, 0.5]^3)
    float4 color_opacity;
    float4 emissive_roughness;
    float metallic;
    float softness;
    uint shape;
    uint seed;
};
static const uint k_decal_bytes = 96;
static const uint k_decal_tile = 16;

Decal load_decal(ByteAddressBuffer b, uint at) {
    Decal d;
    [unroll] for (uint r = 0; r < 3; ++r) { d.rows[r] = load4(b, at + r * 16); }
    d.color_opacity = load4(b, at + 48);
    d.emissive_roughness = load4(b, at + 64);
    const uint4 tail = b.Load4(at + 80);
    d.metallic = asfloat(tail.x);
    d.softness = asfloat(tail.y);
    d.shape = tail.z;
    d.seed = tail.w;
    return d;
}

uint hash_uint(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float hash01(uint x) { return float(hash_uint(x) >> 8) * (1.0 / 16777216.0); }

float2 cell_point(int2 cell, uint seed) {
    const uint h = hash_uint(uint(cell.x) * 73856093u ^ uint(cell.y) * 19349663u ^ seed * 83492791u);
    return float2(cell) + float2(hash01(h), hash01(h ^ 0x9e3779b9u)) * 0.8 + 0.1;
}

// Positive inside, in units of the stamp's half width (q spans [-1, 1]).
float decal_distance(uint shape, float2 q, uint seed) {
    const float r = length(q);
    if (shape == 1) { // rectangle
        return 1.0 - max(abs(q.x), abs(q.y));
    }
    if (shape == 2) { // ring
        return 0.18 - abs(r - 0.72);
    }
    if (shape == 3) { // splat: a disc with a seeded, wobbly rim
        const float a = atan2(q.y, q.x);
        const float p = hash01(seed) * 6.2831853;
        const float rim = 0.68 + 0.12 * sin(3.0 * a + p) + 0.07 * sin(5.0 * a + 2.0 * p) +
                          0.04 * sin(11.0 * a + 3.0 * p);
        return rim - r;
    }
    if (shape == 4) { // cracks: Voronoi cell borders, thinning towards the rim
        const float2 g = q * 3.0;
        const int2 base = int2(floor(g));
        float f1 = 1e9;
        float f2 = 1e9;
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                const float2 p = cell_point(base + int2(x, y), seed);
                const float d = dot(g - p, g - p);
                if (d < f1) {
                    f2 = f1;
                    f1 = d;
                } else if (d < f2) {
                    f2 = d;
                }
            }
        }
        // Distance to the border with the second-nearest point, in g units.
        const float edge = (sqrt(f2) - sqrt(f1)) * 0.5;
        const float width = 0.09 * saturate(1.0 - r);
        return min((width - edge) / 3.0, 1.0 - r);
    }
    return 1.0 - r; // disc
}

// Blends every decal over `pixel` into the surface's material.
void apply_decals(uint buffer, uint decal_offset, uint tile_offset, uint entry_offset, uint columns,
                  uint rows, uint2 pixel, float3 world, float3 n, float pixel_size, inout float3 base,
                  inout float roughness, inout float metallic, inout float3 emissive) {
    if (columns == 0) {
        return;
    }
    const uint2 tile = min(pixel / k_decal_tile, uint2(columns - 1, rows - 1));
    ByteAddressBuffer b = read_buffer(buffer);
    const uint2 list = b.Load2(tile_offset + (tile.y * columns + tile.x) * 8);
    for (uint i = 0; i < list.y; ++i) {
        const uint index = b.Load(entry_offset + (list.x + i) * 4);
        const Decal d = load_decal(b, decal_offset + index * k_decal_bytes);
        const float4 p1 = float4(world, 1.0);
        const float3 p = float3(dot(d.rows[0], p1), dot(d.rows[1], p1), dot(d.rows[2], p1));
        if (any(abs(p) > 0.5)) {
            continue;
        }
        // Surfaces facing away from the stamp's direction take none of it.
        const float3 up = safe_normalize(d.rows[1].xyz, float3(0.0, 1.0, 0.0));
        const float facing = saturate((dot(n, up) - 0.1) * 2.5);
        const float depth = saturate((0.5 - abs(p.y)) * 8.0);
        // One pixel, in q units (q = 2 * p.xz).
        const float aa = max(pixel_size * 2.0 * max(length(d.rows[0].xyz), length(d.rows[2].xyz)), 1e-5);
        // Cracks are thin (their distance peaks near 0.03): soften them in
        // proportion, so the default softness still stamps them fully.
        const float softness = d.shape == 4 ? d.softness * 0.1 : d.softness;
        const float w = max(softness, aa);
        const float mask = saturate(decal_distance(d.shape, p.xz * 2.0, d.seed) / w + 0.5 * aa / w);
        const float a = d.color_opacity.a * mask * facing * depth;
        base = lerp(base, d.color_opacity.rgb, a);
        roughness = lerp(roughness, d.emissive_roughness.a, a);
        metallic = lerp(metallic, d.metallic, a);
        emissive = lerp(emissive, d.emissive_roughness.rgb, a);
    }
}
