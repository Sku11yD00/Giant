// A ray against this frame's depth buffer, for sharp reflections from
// the screen (reflect.slang on glossy surfaces, water.slang on water).
#pragma once
#include "common.slang"

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
bool screen_ray(Frame f, Texture2D<float> depth, float3 p, float3 r, out int2 hit_px, out float trust) {
    hit_px = int2(0, 0);
    trust = 0.0;
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

