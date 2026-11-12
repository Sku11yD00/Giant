// Finishes shading (resolve.slang): sharp reflections from the screen on
// glossy pixels, added to the light before exposure (rgba16_float, a as
// shading's), which post.slang then exposes and tone maps.
//
// Screen reflections: a ray along the mirror direction against this
// frame's depth buffer; where it hits, the light there in this frame's
// radiance takes the place of the reflected light shading used (a voxel
// cone's, or the hemisphere's), faded out at the screen's edges and for
// rays coming back at the camera. Nothing from earlier frames is used.
//
// Layout: ReflectPush in src/gpu_layout.h.
#include "common.slang"
#include "screen_trace.slang"

struct ReflectPush {
    uint frame;
    uint frame_offset;
    uint radiance; // rgba16_float, before exposure; a: 1 to tone map, 0 as it is
    uint gloss;    // rg32_uint: x the reflection's weight (10:10:10; 0: none), y the normal (octahedral)
    uint fallback; // rgba16_float: the reflected light shading used
    uint depth;    // the scene depth (reverse-Z)
    uint on;       // 1: trace screen reflections
    uint pad;
};
ConstantBuffer<ReflectPush> push : register(b0);

[shader("vertex")]
float4 vs_main(uint vertex_id : SV_VertexID) : SV_Position {
    const float2 p = float2(vertex_id == 1 ? 3.0 : -1.0, vertex_id == 2 ? 3.0 : -1.0);
    return float4(p, 0.0, 1.0);
}

float3 unpack_octahedral(uint packed) {
    const float2 q = float2(float(packed & 0xFFFFu), float(packed >> 16)) / 65535.0 * 2.0 - 1.0;
    float3 n = float3(q, 1.0 - abs(q.x) - abs(q.y));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    }
    return safe_normalize(n, float3(0.0, 1.0, 0.0));
}

[shader("pixel")]
float4 ps_main(float4 position : SV_Position) : SV_Target {
    const int2 pixel = int2(position.xy);
    Texture2D<float4> radiance = read_texture(push.radiance);
    const float4 shaded = radiance.Load(int3(pixel, 0));
    if (shaded.a <= 0.0) {
        return float4(shaded.rgb, 0.0); // the background and debug views, as they are
    }
    const Frame f = load_frame(push.frame, push.frame_offset);
    float3 color = shaded.rgb;
    if (push.on != 0) {
        const uint2 gloss = read_texture_uint2(push.gloss).Load(int3(pixel, 0));
        const float d = read_texture_float(push.depth).Load(int3(pixel, 0));
        if (gloss.x != 0u && d > 0.0) {
            const float3 weight =
                float3(float(gloss.x & 1023u), float((gloss.x >> 10) & 1023u), float((gloss.x >> 20) & 1023u)) / 1023.0;
            const float3 n = unpack_octahedral(gloss.y);
            // The point, from the depth: camera-relative.
            const float2 ndc = float2((position.x / f.width) * 2.0 - 1.0, 1.0 - (position.y / f.height) * 2.0);
            const float3 world = (f.ray_x.xyz * ndc.x + f.ray_y.xyz * ndc.y + f.ray_z.xyz) * (f.near_plane / d);
            const float3 v = safe_normalize(-world, n);
            const float3 r = reflect(-v, n);
            // Off the surface by a couple of pixels, so it does not hit itself.
            const float pixel_size = length(world) * 2.0 / max(f.p11 * f.height, 1e-6);
            int2 hit_px;
            float trust;
            if (screen_ray(f, read_texture_float(push.depth), world + n * (2.0 * pixel_size), r, hit_px, trust) && trust > 0.0) {
                const float4 seen = radiance.Load(int3(hit_px, 0));
                if (seen.a > 0.0) { // a lit surface (not the background)
                    const float3 used = read_texture(push.fallback).Load(int3(pixel, 0)).rgb;
                    color += weight * trust * (seen.rgb - used);
                }
            }
        }
    }
    return float4(min(max(color, float3(0.0, 0.0, 0.0)), float3(60000.0, 60000.0, 60000.0)), 1.0);
}
