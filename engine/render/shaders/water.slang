// Water's surfaces (water plan W2a, W2b), drawn after shading over a copy
// of the light (rgba16_float, before exposure) that post.slang then exposes:
//
// - vs_copy / ps_copy: the light as it is, under everything.
// - vs_main / ps_main: the water's triangles (src/water.h packs them,
//   camera-relative, from FrameInput::water_*), each pixel in front of
//   what shading drew there (the scene depth, reverse-Z, tested here: no
//   depth target) shaded as water: a flat surface whose normal is a sum of
//   travelling waves (src/water.cpp's water_waves, faded out where a wave is
//   shorter than a few pixels); what lies under it seen through, bent by
//   the waves (Snell; the unbent pixel where the bent one shows something
//   above the water), its light lost a metre per channel (Beer-Lambert) and
//   replaced, as the water deepens, by the water's own colour lit by the
//   sun and the sky; mirrored by Fresnel's law (n 1.33): what the screen
//   shows along the mirror direction, else a cone through the lit voxel
//   world, else the sky; the sun's (or the moon's) glint, shadowed, and the
//   lamps' highlights; foam where it is shallow; the air over it all (the
//   clear air, fog, clouds, what falls, dust) as shading puts over the
//   scene, the light from under it keeping the air it already had.
//   Seen from below (the camera under the surface) it is left out (W3).
//
// Layout: WaterPush, WaterVertex, WaterLook, WaterWave in src/gpu_layout.h;
// shading's extras as resolve.slang's (shading_extras.slang).
#include "common.slang"
#include "screen_trace.slang"
#include "shading_extras.slang"
#include "voxel_gi.slang"

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
    uint extras;
    uint extras_offset;
    uint extras_on;
    uint pad0;
    uint pad1;
};
ConstantBuffer<WaterPush> push : register(b0);

static const float k_water_f0 = 0.02;        // reflectance seen straight down (n 1.33)
static const float k_scatter_per_turbidity = 1.5; // metres^-1 of scattering at turbidity 1
static const float k_refraction_reach = 4.0; // metres through the water the bend is worked out over, at most
static const float k_foam_depth = 0.6;       // metres of water at which shore foam fades out entirely (it shows from about 0.5 m, on the highest crests)
static const float k_foam_albedo = 0.8;
static const float k_water_cone = 0.05;      // the voxel cone's width (tangent) for the mirror (trace_reflection widens it to a sixth of its distance)

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
// `crest`: 0 in the troughs to 1 on the crests (0.5 where the waves fade).
float3 wave_normal(float2 xz, float footprint, float scale, out float crest) {
    ByteAddressBuffer b = read_buffer(push.data);
    float2 slope = float2(0.0, 0.0);
    float height = 0.0;
    float total = 0.0;
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
        height += seen * sin(theta);
        total += 1.0;
    }
    crest = saturate(0.5 + 0.5 * height / max(total * 0.5, 1.0));
    return normalize(float3(-slope.x, 1.0, -slope.y));
}

// How far (along the view, scaled as the ray's: the depth's near / d) the
// scene lies at a pixel: 3e38 where nothing was drawn.
float scene_scale(Frame f, Texture2D<float> depth, int2 px) {
    const float d = depth.Load(int3(px, 0));
    return d > 0.0 ? f.near_plane / d : 3.0e38;
}

// A highlight's share (GGX, height-correlated Smith) of light from `l`
// seen from `to_eye` off a surface with normal `n`, `alpha` wide.
float water_highlight(float3 n, float3 to_eye, float3 l, float n_dot_v, float alpha) {
    const float3 h = safe_normalize(l + to_eye, n);
    const float n_dot_l = saturate(dot(n, l));
    const float n_dot_h = saturate(dot(n, h));
    const float a2 = alpha * alpha;
    const float dd = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    const float ggx = a2 / max(3.14159265 * dd * dd, 1e-7);
    const float vis = 0.5 / max(n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0 - a2) + a2) +
                                    n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0 - a2) + a2), 1e-5);
    return ggx * vis * n_dot_l;
}

// The lamps' highlights on the water (the lamps listed in the pixel's
// cluster, as resolve.slang's local_lighting), widened by each lamp's size,
// times Fresnel; shadowed as the surface would be.
float3 lamp_highlights(Extras e, Frame f, float2 position, float3 p, float view_z, float3 n, float3 to_eye,
                       float n_dot_v, float alpha) {
    float3 sum = float3(0.0, 0.0, 0.0);
    if (e.local.count == 0u) {
        return sum;
    }
    ByteAddressBuffer b = read_buffer(e.local.buffer);
    const uint2 listed = light_cluster(e.local, b, position, f.width, f.height, view_z);
    [loop] for (uint i = 0; i < listed.y; ++i) {
        const uint index = b.Load(e.local.entries + (listed.x + i) * 4u);
        const LocalLight lamp = load_light(b, e.local.lights + index * k_light_bytes);
        const float3 to_light = lamp.position - p;
        const float d2 = dot(to_light, to_light);
        if (d2 >= lamp.range * lamp.range || d2 < 1e-8) {
            continue;
        }
        const float inv_d = rsqrt(d2);
        const float3 l = to_light * inv_d;
        float falloff = light_falloff(lamp, to_light, l);
        if (falloff <= 0.0 || dot(n, l) <= 0.0) {
            continue;
        }
        if (lamp.shadow != 0u && e.local.atlas_on != 0u) {
            falloff *= light_shadow(e.local, b, lamp, p, float3(0.0, 1.0, 0.0));
        }
        const float wide = saturate(alpha + lamp.source_radius * inv_d * 0.5);
        const float v_dot_h = saturate(dot(to_eye, safe_normalize(l + to_eye, n)));
        const float fresnel = k_water_f0 + (1.0 - k_water_f0) * pow(1.0 - v_dot_h, 5.0);
        sum += lamp.color * (water_highlight(n, to_eye, l, n_dot_v, wide) * fresnel * falloff);
    }
    return sum;
}

// The lamps' light on foam (Lambert, over pi).
float3 lamp_diffuse(Extras e, Frame f, float2 position, float3 p, float view_z, float3 n) {
    float3 sum = float3(0.0, 0.0, 0.0);
    if (e.local.count == 0u) {
        return sum;
    }
    ByteAddressBuffer b = read_buffer(e.local.buffer);
    const uint2 listed = light_cluster(e.local, b, position, f.width, f.height, view_z);
    [loop] for (uint i = 0; i < listed.y; ++i) {
        const uint index = b.Load(e.local.entries + (listed.x + i) * 4u);
        const LocalLight lamp = load_light(b, e.local.lights + index * k_light_bytes);
        const float3 to_light = lamp.position - p;
        const float d2 = dot(to_light, to_light);
        if (d2 >= lamp.range * lamp.range || d2 < 1e-8) {
            continue;
        }
        const float3 l = to_light * rsqrt(d2);
        const float falloff = light_falloff(lamp, to_light, l);
        sum += lamp.color * (falloff * saturate(dot(n, l)) * 0.31830989);
    }
    return sum;
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
    // Shading's extras: shadows, the voxel light, the sky, the lamps.
    const bool have = push.extras_on != 0u;
    // (Without, the frame's descriptor: read, never used.)
    const Extras e = load_extras(have ? push.extras : push.frame, have ? push.extras_offset : 0u);
    const bool sky_on = have && e.sky_on != 0u;
    const Sky sky = load_sky(have ? e.sky_params : push.frame, have ? e.sky_params_offset : 0u);
    // A pixel's width on the surface there (metres).
    const float footprint = distance * 2.0 / max(f.p11 * f.height, 1e-6) / max(-v.y, 0.05);
    float crest;
    const float3 n = wave_normal(p.xz, footprint, look.waves, crest);
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
    const float straight_full = straight_z >= 1.0e37 ? 1.0e6 : max(straight_z - own_view_z, 0.0) * (distance / max(own_view_z, 1e-6));
    const float straight_through = min(straight_full, k_refraction_reach);
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
        under_z = straight_z;
    }
    // The distance through the water to what is seen (the view's slant kept).
    const float through_water = under_z >= 1.0e37 ? 1.0e6 : max(under_z - own_view_z, 0.0) * (distance / max(own_view_z, 1e-6));
    // (At least a little: clear water over the sky still hides it, as the
    // sky behind is light post has not exposed.)
    const float3 extinction = max(look.absorption + look.turbidity * k_scatter_per_turbidity, float3(1e-3, 1e-3, 1e-3));
    const float3 transmitted = exp(-extinction * through_water);
    const float3 seen = light.Load(int3(bent_px, 0)).rgb;
    // The sun (or the moon) here: its shadows and the clouds'.
    const float3 l = f.sun_direction.xyz;
    float sun = have ? sun_visibility(e.shadows, p, float3(0.0, 1.0, 0.0), l, footprint) : 1.0;
    if (sky_on) {
        sun *= cloud_shadow(sky, p);
    }
    // The water's own colour, lit by the sun and the sky (as a rough
    // surface would be lit under the surface), where its depth hides what is under it.
    const float3 sky_up = ambient_light(f, float3(0.0, 1.0, 0.0));
    const float sun_up = saturate(l.y);
    const float3 deep = look.color * (sky_up + f.sun_light.rgb * (sun_up * sun * 0.31830989));
    // Foam where the bed comes near (how deep straight down), broken up by
    // the waves' crests.
    const float bed_depth = straight_full * max(-v.y, 0.0);
    const float shore = saturate(1.0 - bed_depth / k_foam_depth);
    const float foam = look.waves > 0.0 ? saturate(shore * 1.6 - 1.0 + (crest - 0.5) * 1.5) * saturate(look.waves) : 0.0;
    // The screen's ray and the voxel cone, unless foam hides the mirror
    // (water reflects at least a fiftieth even seen straight down: no cut-off by angle).
    const bool mirror_shows = foam < 0.95;
    // The mirror: the sky along it (its air alone; the sun's disc is the
    // glint), or the flat hemisphere without the sky; a cone through the
    // lit voxel world in front of that where the screen will not show it.
    const float3 r = reflect(v, n);
    const float3 r_up = normalize(float3(r.x, max(r.y, 0.01), r.z));
    float3 mirrored = sky_on ? sample_sky(sky, r_up) : ambient_light(f, r_up);
    int2 hit_px = int2(0, 0);
    float trust = 0.0;
    const bool on_screen = mirror_shows && have && (e.reflect_on & 2u) != 0u && screen_ray(f, depth, p + float3(0.0, 2.0 * footprint * max(-v.y, 0.05), 0.0), r_up, hit_px, trust)
                           && trust > 0.0 && light.Load(int3(hit_px, 0)).a > 0.0;
    if (!on_screen) {
        trust = 0.0;
    }
    if (mirror_shows && have && (e.reflect_on & 1u) != 0u && e.gi_on != 0u && trust < 0.99) {
        VoxelWorld w;
        w.pool = e.voxel_pool;
        w.table = e.voxel_table;
        w.levels = e.voxel_levels;
        w.levels_offset = e.voxel_levels_offset;
        w.level_count = e.voxel_level_count;
        w.bricks_per_axis = e.voxel_bricks_per_axis;
        w.stand_ins = 0u; // everything reflects
        w.lamps = e.gi_lamps;
        w.lamps_on = e.gi_lamps_on;
        const float4 cone = trace_reflection(w, write_buffer(e.gi_light), f, p, float3(0.0, 1.0, 0.0), r_up, k_water_cone);
        mirrored = cone.rgb + (1.0 - cone.a) * mirrored;
    }
    // The sun's glint off the waves (GGX), and the lamps'.
    const float alpha = 0.02 + 0.03 * saturate(look.waves); // the glint's spread
    const float3 glint = f.sun_light.rgb * (water_highlight(n, -v, l, n_dot_v, alpha) * fresnel * sun);
    const float3 lamps = have ? lamp_highlights(e, f, i.position.xy, p, own_view_z, n, -v, n_dot_v, alpha) : float3(0.0, 0.0, 0.0);
    float3 foam_light = float3(0.0, 0.0, 0.0);
    if (foam > 0.0) {
        foam_light = k_foam_albedo * (sky_up + f.sun_light.rgb * (saturate(dot(n, l)) * sun * 0.31830989));
        if (have) {
            foam_light += k_foam_albedo * lamp_diffuse(e, f, i.position.xy, p, own_view_z, n);
        }
    }
    // The water's own light (all but what comes from under it).
    const float3 own = lerp((deep * (1.0 - transmitted)) * (1.0 - fresnel) + mirrored * fresnel + glint + lamps, foam_light, foam);
    // The air in front of it, as shading puts it over the scene; `through`:
    // how much of the water's own light comes through it.
    // `air`: the air's own light, the same over nothing (it is linear in
    // what is behind it).
    float through = 1.0;
    float3 color = own;
    float3 air = float3(0.0, 0.0, 0.0);
    if (sky_on) {
        float unused;
        color = under_sky(sky, color, v, distance, uint2(pixel), through);
        air = under_sky(sky, air, v, distance, uint2(pixel), unused);
    }
    if (have) {
        float unused = 1.0;
        color = apply_dust(e, f, color, v, distance, through);
        air = apply_dust(e, f, air, v, distance, unused);
    }
    // What lies under it already holds the air in front of it (and the
    // little more to the bed): that air's light taken off, the rest dimmed
    // by the water.
    color += max(seen - air, float3(0.0, 0.0, 0.0)) * transmitted * ((1.0 - fresnel) * (1.0 - foam));
    // The screen's reflection over the mirror's (it holds its own air, as reflect.slang's).
    if (trust > 0.0) {
        color += (light.Load(int3(hit_px, 0)).rgb - mirrored) * (fresnel * (1.0 - foam) * through * trust);
    }
    color = min(max(color, float3(0.0, 0.0, 0.0)), float3(60000.0, 60000.0, 60000.0));
    return float4(color, 1.0);
}
