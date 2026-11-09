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

static const uint k_ssr_steps = 32;
static const uint k_ssr_refine = 6;
static const float k_ssr_distance = 200.0; // metres, at most

float2 ndc_to_uv(float2 ndc) { return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5); }

// How far a screen position is from leaving the screen, faded over a
// tenth of it (0 at the edge, 1 well inside).
float edge_fade(float2 uv) {
    const float2 e = min(uv, 1.0 - uv);
    return saturate(min(e.x, e.y) * 10.0);
}

// How thick what the depth buffer holds is taken to be (metres at a
// distance): a ray this far behind a surface still hits it.
float ssr_thickness(float z) { return 0.3 + 0.02 * z; }

// The pixel a view-space point falls in (clamped to the screen), and the
// distance the depth buffer holds there (3e38: nothing drawn).
float scene_distance(Frame f, Texture2D<float> depth, float3 q, out int2 px) {
    const float2 uv = ndc_to_uv(float2(f.p00 * q.x, f.p11 * q.y) / -q.z);
    px = clamp(int2(uv * float2(f.width, f.height)), int2(0, 0), int2(int(f.width) - 1, int(f.height) - 1));
    const float d = depth.Load(int3(px, 0));
    return d > 0.0 ? f.near_plane / d : 3.0e38;
}

// The ray from `p` (camera-relative, already off the surface) along `r`
// (unit) against the depth buffer. True on a hit: `hit_px` is the pixel it
// hits, `trust` how much to trust it (screen edges, rays back at the
// camera).
bool screen_ray(Frame f, float3 p, float3 r, out int2 hit_px, out float trust) {
    hit_px = int2(0, 0);
    trust = 0.0;
    Texture2D<float> depth = read_texture_float(push.depth);
    const float3 vo = float3(dot(f.view_rows[0].xyz, p), dot(f.view_rows[1].xyz, p), dot(f.view_rows[2].xyz, p));
    const float3 vr = float3(dot(f.view_rows[0].xyz, r), dot(f.view_rows[1].xyz, r), dot(f.view_rows[2].xyz, r));
    // Rays coming back at the camera meet what is behind it: faded out.
    const float towards = saturate((0.6 - vr.z) * 2.5);
    if (towards <= 0.0 || -vo.z <= f.near_plane * 1.5) {
        return false;
    }
    float len = k_ssr_distance;
    if (vr.z > 0.0) {
        len = min(len, (-vo.z - f.near_plane * 1.5) / vr.z); // stays in front of the camera
    }
    if (len <= 0.0) {
        return false;
    }
    float before = 0.0;
    [loop]
    for (uint i = 0; i < k_ssr_steps; ++i) {
        const float k = float(i + 1) / float(k_ssr_steps);
        const float t = len * k * k; // finer near the surface
        const float3 q = vo + vr * t;
        const float2 uv = ndc_to_uv(float2(f.p00 * q.x, f.p11 * q.y) / -q.z);
        if (any(uv < 0.0) || any(uv >= 1.0)) {
            return false; // left the screen
        }
        int2 px;
        if (-q.z > scene_distance(f, depth, q, px)) {
            // Behind what the screen shows: find where it went in.
            float lo = before;
            float hi = t;
            [loop]
            for (uint j = 0; j < k_ssr_refine; ++j) {
                const float mid = 0.5 * (lo + hi);
                const float3 m = vo + vr * mid;
                int2 mpx;
                if (-m.z > scene_distance(f, depth, m, mpx)) {
                    hi = mid;
                } else {
                    lo = mid;
                }
            }
            const float3 m = vo + vr * hi;
            int2 mpx;
            const float zs = scene_distance(f, depth, m, mpx);
            if (-m.z - zs < ssr_thickness(zs)) {
                const float2 muv = ndc_to_uv(float2(f.p00 * m.x, f.p11 * m.y) / -m.z);
                hit_px = mpx;
                trust = towards * edge_fade(muv);
                return true;
            }
            // Passed behind something thin: go on, from in front of it.
        } else {
            before = t;
        }
    }
    return false;
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
            if (screen_ray(f, world + n * (2.0 * pixel_size), r, hit_px, trust) && trust > 0.0) {
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
