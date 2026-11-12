// Water's surfaces (water plan W2a), drawn after shading over a copy of the
// light (rgba16_float, before exposure) that post.slang then exposes:
//
// - vs_copy / ps_copy: the light as it is, under everything.
// - vs_main / ps_main: the water's triangles (src/water.h packs them,
//   camera-relative, from FrameInput::water_*), each pixel in front of
//   what shading drew there (the scene depth, reverse-Z, tested here: no
//   depth target) shaded as water: a flat surface whose normal is a sum of
//   travelling waves (src/water.cpp's water_waves, faded out where a wave is
//   shorter than a few pixels); what lies under it seen through, bent by
//   the waves (the unbent pixel where the bent one shows something above
//   the water), its light lost a metre per channel (Beer-Lambert) and
//   replaced, as the water deepens, by the water's own colour lit by the
//   sun and the sky; the sky and the sun mirrored by Fresnel's law (n 1.33).
//   Seen from below (the camera under the surface) it is left out (W3).
//
// Layout: WaterPush, WaterVertex, WaterLook, WaterWave in src/gpu_layout.h.
#include "common.slang"

struct WaterPush {
    uint frame;
    uint frame_offset;
    uint data;
    uint vertices_offset;
    uint indices_offset;
    uint looks_offset;
    uint waves_offset;
    uint look_count;
    uint light;
    uint depth;
    uint wave_count;
    uint pad;
};
ConstantBuffer<WaterPush> push : register(b0);

static const float k_water_f0 = 0.02;        // reflectance seen straight down (n 1.33)
static const float k_scatter_per_turbidity = 1.5; // metres^-1 of scattering at turbidity 1
static const float k_refraction_reach = 4.0; // metres through the water the bend is worked out over, at most

[shader("vertex")]
float4 vs_copy(uint vertex_id : SV_VertexID) : SV_Position {
    const float2 p = float2(vertex_id == 1 ? 3.0 : -1.0, vertex_id == 2 ? 3.0 : -1.0);
    return float4(p, 0.0, 1.0);
}

[shader("pixel")]
float4 ps_copy(float4 position : SV_Position) : SV_Target {
    return read_texture(push.light).Load(int3(int2(position.xy), 0));
}

// Camera-relative world to clip (Frame::view_proj: columns).
float4 to_clip(Frame f, float3 p) {
    return f.view_proj[0] * p.x + f.view_proj[1] * p.y + f.view_proj[2] * p.z + f.view_proj[3];
}

struct WaterVaryings {
    float4 position : SV_Position;
    float3 world : WORLD;   // camera-relative
    nointerpolation uint look : LOOK;
};

[shader("vertex")]
WaterVaryings vs_main(uint vertex_id : SV_VertexID) {
    ByteAddressBuffer b = read_buffer(push.data);
    const uint index = b.Load(push.indices_offset + vertex_id * 4u);
    const uint at = push.vertices_offset + index * 32u;
    const float3 p = asfloat(b.Load3(at));
    const uint look = b.Load(at + 12u);
    const Frame f = load_frame(push.frame, push.frame_offset);
    WaterVaryings o;
    o.position = to_clip(f, p);
    o.world = p;
    o.look = look;
    return o;
}

struct Look {
    float3 color;
    float turbidity;
    float3 absorption;
    float waves;
};

Look load_look(uint index) {
    ByteAddressBuffer b = read_buffer(push.data);
    const uint at = push.looks_offset + min(index, push.look_count - 1u) * 32u;
    const float4 a = asfloat(b.Load4(at));
    const float4 c = asfloat(b.Load4(at + 16u));
    Look l;
    l.color = a.xyz;
    l.turbidity = a.w;
    l.absorption = c.xyz;
    l.waves = c.w;
    return l;
}

// The surface's normal at camera-relative `xz`: the waves' slopes summed,
// each faded out as it gets shorter than a few pixels `footprint` wide.
float3 wave_normal(float2 xz, float footprint, float scale) {
    ByteAddressBuffer b = read_buffer(push.data);
    float2 slope = float2(0.0, 0.0);
    [loop]
    for (uint i = 0; i < push.wave_count; ++i) {
        const uint at = push.waves_offset + i * 32u;
        const float4 w = asfloat(b.Load4(at));
        const float phase = asfloat(b.Load(at + 16u));
        const float2 dir = w.xy;
        const float k = w.z;
        const float wavelength = 6.2831853 / k;
        const float seen = saturate(wavelength / max(footprint * 4.0, 1e-6) - 1.0);
        const float theta = k * dot(dir, xz) + phase;
        slope += dir * (w.w * scale * seen * cos(theta));
    }
    return normalize(float3(-slope.x, 1.0, -slope.y));
}

// How far (along the view, scaled as the ray's: the depth's near / d) the
// scene lies at a pixel: 3e38 where nothing was drawn.
float scene_scale(Frame f, Texture2D<float> depth, int2 px) {
    const float d = depth.Load(int3(px, 0));
    return d > 0.0 ? f.near_plane / d : 3.0e38;
}

[shader("pixel")]
float4 ps_main(WaterVaryings i) : SV_Target {
    const Frame f = load_frame(push.frame, push.frame_offset);
    const int2 pixel = int2(i.position.xy);
    Texture2D<float> depth = read_texture_float(push.depth);
    Texture2D<float4> light = read_texture(push.light);
    // Behind what shading drew here: not seen.
    const float scene_d = depth.Load(int3(pixel, 0));
    if (scene_d > i.position.z) {
        discard;
    }
    const float3 p = i.world;
    const float distance = length(p);
    const float3 v = p / max(distance, 1e-6); // from the camera
    if (v.y > 0.0) {
        discard; // seen from below: W3
    }
    const Look look = load_look(i.look);
    // A pixel's width on the surface there (metres).
    const float footprint = distance * 2.0 / max(f.p11 * f.height, 1e-6) / max(-v.y, 0.05);
    const float3 n = wave_normal(p.xz, footprint, look.waves);
    const float n_dot_v = saturate(dot(n, -v));
    // Fresnel (Schlick), n 1.33.
    const float fresnel = k_water_f0 + (1.0 - k_water_f0) * pow(1.0 - n_dot_v, 5.0);
    // What lies under it, through it: the water's thickness along the view.
    const float own_view_z = -dot(f.view_rows[2].xyz, p);
    // The bed seen straight through, then where the waves' refraction
    // moves it (Snell, n 1.33: the ray bent against the surface's lean,
    // compared with still water's, which the image already shows),
    // projected back to a pixel.
    const float straight_z = scene_scale(f, depth, pixel);
    const float straight_through = straight_z >= 1.0e37
        ? k_refraction_reach
        : min(max(straight_z - own_view_z, 0.0) * (distance / max(own_view_z, 1e-6)), k_refraction_reach);
    const float3 t_wavy = refract(v, n, 1.0 / 1.33);
    const float3 t_still = refract(v, float3(0.0, 1.0, 0.0), 1.0 / 1.33);
    const float3 bed = p + v * straight_through + (t_wavy - t_still) * straight_through;
    const float4 bed_clip = to_clip(f, bed);
    int2 bent_px = pixel;
    if (bed_clip.w > 1e-4) {
        const float2 bed_uv = bed_clip.xy / bed_clip.w * float2(0.5, -0.5) + 0.5;
        const float2 size = float2(f.width, f.height);
        bent_px = int2(clamp(bed_uv * size, float2(0.0, 0.0), size - 1.0));
    }
    // (Scales along the view: the scene's, and the surface's own, as a view-space z.)
    float under_z = scene_scale(f, depth, bent_px);
    if (under_z <= own_view_z) {
        bent_px = pixel; // the bent pixel shows something above the water: the straight one
        under_z = scene_scale(f, depth, pixel);
    }
    // The distance through the water to what is seen (the view's slant kept).
    const float through = under_z >= 1.0e37 ? 1.0e6 : max(under_z - own_view_z, 0.0) * (distance / max(own_view_z, 1e-6));
    // (At least a little: clear water over the sky still hides it, as the
    // sky behind is light post has not exposed.)
    const float3 extinction = max(look.absorption + look.turbidity * k_scatter_per_turbidity, float3(1e-3, 1e-3, 1e-3));
    const float3 transmitted = exp(-extinction * through);
    const float3 seen = light.Load(int3(bent_px, 0)).rgb;
    // The water's own colour, lit by the sun and the sky (as a rough
    // surface would be lit under the surface), where its depth hides what is under it.
    const float3 sky = ambient_light(f, float3(0.0, 1.0, 0.0));
    const float sun_up = saturate(f.sun_direction.y);
    const float3 deep = look.color * (sky + f.sun_light.rgb * (sun_up * 0.31830989));
    const float3 refracted = seen * transmitted + deep * (1.0 - transmitted);
    // The sky mirrored (blurred: the sky's light from round the mirror
    // direction), and the sun's glint off the waves.
    const float3 r = reflect(v, n);
    const float3 reflected_sky = ambient_light(f, float3(r.x, max(r.y, 0.0), r.z));
    const float3 l = f.sun_direction.xyz;
    const float3 h = safe_normalize(l - v, n);
    const float n_dot_l = saturate(dot(n, l));
    const float n_dot_h = saturate(dot(n, h));
    const float alpha = 0.02 + 0.03 * saturate(look.waves); // the glint's spread
    const float a2 = alpha * alpha;
    const float dd = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    const float ggx = a2 / max(3.14159265 * dd * dd, 1e-7);
    const float vis = 0.5 / max(n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0 - a2) + a2) + n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0 - a2) + a2), 1e-5);
    const float3 glint = f.sun_light.rgb * (ggx * vis * fresnel * n_dot_l);
    float3 color = refracted * (1.0 - fresnel) + reflected_sky * fresnel + glint;
    color = min(max(color, float3(0.0, 0.0, 0.0)), float3(60000.0, 60000.0, 60000.0));
    return float4(color, 1.0);
}
