// Ambient occlusion from the depth buffer (photoreal part 4b): GTAO
// (Jimenez et al. 2016) at half resolution, one frame on its own (no
// history, so nothing smears), then a depth-aware spatial denoise.
//
// - cs_gtao: per half-resolution pixel, the view-space point and normal
//   (from depth, the flatter neighbour on each axis) at the full-size pixel
//   it stands for; two slices through it, rotated by a fixed 4x4 pattern
//   (16 angles over every 4x4 block), each marching k_ao_steps steps a side
//   out to the radius (metres, as seen on screen, capped at k_ao_max_pixels),
//   finds the horizons and integrates the cosine-weighted visibility
//   between them, and the bent normal (the average open direction).
// - cs_denoise: each pixel the average of the 4x4 block round it (which
//   holds all 16 angles), weighted by how near in depth each is.
//
// Output (rgba16_float): x the ambient visibility (1: open), y the view
// depth (metres; 0: nothing drawn), zw the bent normal in view space
// (octahedral, -1 to 1). Resolve upsamples it with depth weights.
//
// Layout: AoPush in src/gpu_layout.h.
#include "common.slang"

struct AoPush {
    uint frame;
    uint frame_offset;
    uint depth;  // read: the scene depth (reverse-Z, full size)
    uint source; // read: cs_denoise's input (cs_gtao's output)
    uint target; // write: rgba16_float, half size
    uint width;  // of the target
    uint height;
    uint pad0;
    float radius; // metres
    float pad1;
    float pad2;
    float pad3;
};
ConstantBuffer<AoPush> push : register(b0);

static const float k_ao_pi = 3.14159265;
static const uint k_ao_slices = 2;
static const uint k_ao_steps = 6;
static const float k_ao_max_pixels = 96.0; // full-size pixels: the march's longest reach on screen

// The view-space point of a full-size pixel (z negative ahead), and
// whether anything is drawn there.
// Off the screen counts as nothing drawn.
float3 point_at(Frame f, float d, float2 pos) {
    const float z = f.near_plane / d;
    const float2 uv = (pos + 0.5) / float2(f.width, f.height);
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    return float3(ndc.x * z / f.p00, ndc.y * z / f.p11, -z);
}

float3 view_point(Frame f, Texture2D<float> depth, int2 px, out bool drawn) {
    drawn = false;
    if (any(px < int2(0, 0)) || px.x >= int(f.width) || px.y >= int(f.height)) {
        return float3(0.0, 0.0, -1.0e6);
    }
    const float d = depth.Load(int3(px, 0));
    drawn = d > 0.0;
    return drawn ? point_at(f, d, float2(px)) : float3(0.0, 0.0, -1.0e6);
}

// The view-space point at a fractional pixel position: reverse-Z depth is
// linear across the screen on a plane, so it is interpolated between the
// four pixels round it (where all four are drawn; else the nearest), and
// samples lie on the slice exactly (rounding them would lift the horizon
// of open ground half the time).
float3 view_point_between(Frame f, Texture2D<float> depth, float2 pos, out bool drawn) {
    drawn = false;
    if (any(pos < float2(0.0, 0.0)) || pos.x > f.width - 1.0 || pos.y > f.height - 1.0) {
        return float3(0.0, 0.0, -1.0e6);
    }
    const int2 base = min(int2(floor(pos)), int2(int(f.width) - 2, int(f.height) - 2));
    const float2 t = saturate(pos - float2(base));
    const float d00 = depth.Load(int3(base, 0));
    const float d10 = depth.Load(int3(base + int2(1, 0), 0));
    const float d01 = depth.Load(int3(base + int2(0, 1), 0));
    const float d11 = depth.Load(int3(base + int2(1, 1), 0));
    float d;
    if (d00 > 0.0 && d10 > 0.0 && d01 > 0.0 && d11 > 0.0) {
        d = lerp(lerp(d00, d10, t.x), lerp(d01, d11, t.x), t.y);
    } else {
        const int2 nearest = base + int2(round(t));
        d = depth.Load(int3(nearest, 0));
    }
    drawn = d > 0.0;
    return drawn ? point_at(f, d, pos) : float3(0.0, 0.0, -1.0e6);
}

float2 encode_octahedral(float3 n) {
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-6);
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    }
    return n.xy;
}

float3 decode_octahedral(float2 q) {
    float3 n = float3(q, 1.0 - abs(q.x) - abs(q.y));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    }
    return safe_normalize(n, float3(0.0, 0.0, 1.0));
}

// The flatter of two differences (so edges do not bend the normal).
float3 flatter(float3 here, float3 a, float3 b, bool a_drawn, bool b_drawn) {
    const float3 da = here - a;
    const float3 db = b - here;
    if (!a_drawn && !b_drawn) return float3(0.0, 0.0, 0.0); // (the normal falls back to the view)
    if (!a_drawn) return db;
    if (!b_drawn) return da;
    return abs(da.z) < abs(db.z) ? da : db;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_gtao(uint3 id : SV_DispatchThreadID) {
    if (id.x >= push.width || id.y >= push.height) {
        return;
    }
    const Frame f = load_frame(push.frame, push.frame_offset);
    Texture2D<float> depth = read_texture_float(push.depth);
    RWTexture2D<float4> target = write_texture(push.target);
    const int2 px = int2(id.xy) * 2;
    bool drawn;
    const float3 p = view_point(f, depth, px, drawn);
    if (!drawn) {
        target[id.xy] = float4(1.0, 0.0, 0.0, 0.0);
        return;
    }
    bool l_drawn, r_drawn, u_drawn, d_drawn;
    const float3 l = view_point(f, depth, px - int2(1, 0), l_drawn);
    const float3 r = view_point(f, depth, px + int2(1, 0), r_drawn);
    const float3 u = view_point(f, depth, px - int2(0, 1), u_drawn);
    const float3 d = view_point(f, depth, px + int2(0, 1), d_drawn);
    const float3 dx = flatter(p, l, r, l_drawn, r_drawn);
    const float3 dy = flatter(p, u, d, u_drawn, d_drawn);
    const float3 v = safe_normalize(-p, float3(0.0, 0.0, 1.0));
    float3 n = safe_normalize(cross(dy, dx), v);
    if (dot(n, v) < 0.0) n = -n;

    // The radius on screen, in full-size pixels.
    const float radius = max(push.radius, 0.01);
    const float pixels = min(radius * f.p00 * 0.5 * f.width / max(-p.z, 1e-3), k_ao_max_pixels);
    if (pixels < 1.0) {
        target[id.xy] = float4(1.0, -p.z, encode_octahedral(n));
        return;
    }
    // The fixed 4x4 pattern: 16 rotations, and a step offset beside them.
    const uint cell = (id.x & 3u) + 4u * (id.y & 3u);
    const float rotation = float((cell * 5u) & 15u) / 16.0; // spread neighbours apart
    const float jitter = (float((cell * 7u + 3u) & 15u) + 0.5) / 16.0;
    float visibility = 0.0;
    float3 bent = float3(0.0, 0.0, 0.0);
    [loop]
    for (uint slice = 0; slice < k_ao_slices; ++slice) {
        const float phi = (float(slice) + rotation) / float(k_ao_slices) * k_ao_pi;
        const float2 screen_dir = float2(cos(phi), sin(phi)); // pixels, y down
        const float3 dir = float3(screen_dir.x, -screen_dir.y, 0.0);
        const float3 ortho = safe_normalize(dir - dot(dir, v) * v, dir);
        const float3 axis = safe_normalize(cross(dir, v), float3(0.0, 1.0, 0.0));
        const float3 projected = n - axis * dot(n, axis);
        const float projected_len = length(projected);
        if (projected_len < 1e-4) continue;
        const float cos_n = clamp(dot(projected, v) / projected_len, -1.0, 1.0);
        const float angle_n = (dot(ortho, projected) >= 0.0 ? 1.0 : -1.0) * acos(cos_n);
        float horizons[2];
        [unroll]
        for (uint side = 0; side < 2; ++side) {
            const float s = side == 0 ? -1.0 : 1.0;
            const float lowest = cos(angle_n + s * k_ao_pi * 0.5);
            float best = lowest;
            [loop]
            for (uint step = 0; step < k_ao_steps; ++step) {
                const float t = (float(step) + jitter) / float(k_ao_steps);
                const float2 offset = s * screen_dir * max(t * t * pixels, 1.0);
                bool q_drawn;
                const float3 q = view_point_between(f, depth, float2(px) + offset, q_drawn);
                if (!q_drawn) continue; // off the screen or the sky
                const float3 to = q - p;
                const float len2 = dot(to, to);
                if (len2 < 1e-8) continue;
                const float cos_h = dot(to, v) * rsqrt(len2);
                // Fading out towards the radius, so far things do not occlude.
                const float falloff = saturate(1.0 - len2 / (radius * radius));
                best = max(best, lerp(lowest, cos_h, falloff));
            }
            horizons[side] = s * acos(clamp(best, -1.0, 1.0));
        }
        const float h0 = angle_n + max(horizons[0] - angle_n, -k_ao_pi * 0.5);
        const float h1 = angle_n + min(horizons[1] - angle_n, k_ao_pi * 0.5);
        const float sin_n = sin(angle_n);
        const float arc = 0.25 * (-cos(2.0 * h0 - angle_n) + cos_n + 2.0 * h0 * sin_n) +
                          0.25 * (-cos(2.0 * h1 - angle_n) + cos_n + 2.0 * h1 * sin_n);
        visibility += projected_len * arc;
        const float middle = 0.5 * (h0 + h1);
        bent += (v * cos(middle) + ortho * sin(middle)) * projected_len;
    }
    // Kept unclamped (two slices' estimate scatters either side of the
    // truth): the denoise averages first, then clamps.
    visibility = clamp(visibility / float(k_ao_slices), 0.0, 2.0);
    const float3 bent_n = safe_normalize(bent, n);
    target[id.xy] = float4(visibility, -p.z, encode_octahedral(bent_n));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_denoise(uint3 id : SV_DispatchThreadID) {
    if (id.x >= push.width || id.y >= push.height) {
        return;
    }
    Texture2D<float4> source = read_texture(push.source);
    const float4 here = source.Load(int3(int2(id.xy), 0));
    if (here.y <= 0.0) {
        write_texture(push.target)[id.xy] = here;
        return;
    }
    // How far depth may differ and still be the same surface: a little,
    // plus the slope seen at this texel (grazing ground changes a lot
    // from one texel to the next), per texel of distance.
    const int2 last = int2(int(push.width) - 1, int(push.height) - 1);
    const float zl = source.Load(int3(clamp(int2(id.xy) - int2(1, 0), int2(0, 0), last), 0)).y;
    const float zr = source.Load(int3(clamp(int2(id.xy) + int2(1, 0), int2(0, 0), last), 0)).y;
    const float zu = source.Load(int3(clamp(int2(id.xy) - int2(0, 1), int2(0, 0), last), 0)).y;
    const float zd = source.Load(int3(clamp(int2(id.xy) + int2(0, 1), int2(0, 0), last), 0)).y;
    const float gx = min(zl > 0.0 ? abs(zl - here.y) : 1e9, zr > 0.0 ? abs(zr - here.y) : 1e9);
    const float gy = min(zu > 0.0 ? abs(zu - here.y) : 1e9, zd > 0.0 ? abs(zd - here.y) : 1e9);
    const float slope_x = gx < 1e8 ? gx : 0.0;
    const float slope_y = gy < 1e8 ? gy : 0.0;
    const float base_tolerance = 0.02 * here.y + 0.05;
    float total = 0.0;
    float ao = 0.0;
    float3 bent = float3(0.0, 0.0, 0.0);
    [loop]
    for (int y = -2; y < 2; ++y) {
        [loop]
        for (int x = -2; x < 2; ++x) {
            const int2 at = clamp(int2(id.xy) + int2(x, y), int2(0, 0), int2(int(push.width) - 1, int(push.height) - 1));
            const float4 s = source.Load(int3(at, 0));
            if (s.y <= 0.0) continue;
            const float tolerance = base_tolerance + 1.5 * (abs(float(x)) * slope_x + abs(float(y)) * slope_y);
            const float w = saturate(1.0 - abs(s.y - here.y) / tolerance);
            total += w;
            ao += s.x * w;
            bent += decode_octahedral(s.zw) * w;
        }
    }
    const float4 o = total > 0.0
                         ? float4(saturate(ao / total), here.y,
                                  encode_octahedral(safe_normalize(bent, decode_octahedral(here.zw))))
                         : float4(saturate(here.x), here.yzw);
    write_texture(push.target)[id.xy] = o;
}
