// Water's surfaces (water plan W2a, W2b, W3), drawn after shading over a
// copy of the light (rgba16_float, before exposure) that post.slang then
// exposes:
//
// - vs_copy / ps_copy: the light as it is, under everything; where the
//   camera's near plane is under the water it is in (FrameInput::underwater),
//   dimmed by the water between (Beer-Lambert) and hidden by the water's own
//   light (its colour lit by the sun and the sky, dimmer the deeper the
//   light has gone: single scattering), with caustics on what lies under
//   the surface. Pixel by pixel, so the line where the near plane meets the
//   surface splits the screen.
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
//   scene, the light from under it keeping the air it already had; the
//   sun's light gathered by the waves into caustics on the bed. Seen from
//   below: the world above through Snell's window (the sky along the bent
//   ray, or what the screen shows that way where that lies above the
//   water), beyond it the water mirrored (total internal reflection), then
//   the water between it and the camera as ps_copy's.
//
// Running water (W4a): its waves carried along its flow (flowing_normal),
// rougher and foaming the faster it runs and in rapids (white water on
// the crests); a sloping surface (a river falling) lit as it leans, the
// vertices' slope added to the waves'.
//
// The sea (W4b): surf where it shoals over a shelving bed, bands running
// in along the depth's contours and breaking into foam.
//
// Caustics: where the sun's ray to a point on the bed came through the
// still surface, the waves' curvature there focuses (or spreads) the light
// as the bent rays' spread on the bed has it (one over the determinant of
// how the bed point moves with the surface point), the pattern fading as
// the bed deepens; laid over all of what is seen there (the sun's share of
// it is not known: the sun's share of the light from above, as a surface
// facing up would take it), where the sun reaches. (Averaged over the
// bed the pattern lightens it a little, a few hundredths at a few metres.)
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
    uint under_on;
    uint under_look;
    float under_level;
    float flow_clock;
    float surf_clock;
    float surf;
};
ConstantBuffer<WaterPush> push : register(b0);

static const float k_water_f0 = 0.02;        // reflectance seen straight down (n 1.33)
static const float k_scatter_per_turbidity = 1.5; // metres^-1 of scattering at turbidity 1
static const float k_refraction_reach = 4.0; // metres through the water the bend is worked out over, at most
static const float k_foam_depth = 0.6;       // metres of water at which shore foam fades out entirely (it shows from about 0.5 m, on the highest crests)
static const float k_foam_albedo = 0.8;
static const float k_caustic_fade = 10.0;    // metres of depth over which caustics blur away (e-folding)
static const float k_caustic_ripples = 3.0;  // the waves' curvature taken this many times: they stand for many finer ripples, which focus more
static const float k_water_cone = 0.05;      // the voxel cone's width (tangent) for the mirror (trace_reflection widens it to a sixth of its distance)
static const float k_flow_period = 2.0;      // seconds running water carries its waves before they start again (water.h's k_water_flow_period)
static const float k_run_rough = 4.0;        // m/s at which running water is as rough as it gets of itself (rapids roughen it more)
static const float k_slope_most = 10.0;      // a vertex's slope: snorm16 of this (water.h's k_water_slope_most)
static const float k_surf_period = 6.0;      // seconds between waves running in (water.h's k_water_surf_period)
static const float k_surf_depth = 1.8;       // metres of water over the bed from which the surf breaks (1.2 times k_break_depth)
static const float k_surf_spacing = 0.8;     // metres of depth between one wave's crest and the next as they run in
static const float k_break_depth = 1.5;      // breaking: from 1.8 m of depth (a fifth at 1.5 m), all of it from 0.3 m
static const float k_surf_shelving = 0.01;   // metres the bed falls a metre at which the surf is whole (a level bed: none)
static const float k_surf_edge = 0.5;        // a bed falling more than this a metre (twice it: none) is an edge, not a shore

[shader("vertex")]
float4 vs_copy(uint vertex_id : SV_VertexID) : SV_Position {
    const float2 p = float2(vertex_id == 1 ? 3.0 : -1.0, vertex_id == 2 ? 3.0 : -1.0);
    return float4(p, 0.0, 1.0);
}

// Camera-relative world to clip (Frame::view_proj: columns).
float4 to_clip(Frame f, float3 p) {
    return f.view_proj[0] * p.x + f.view_proj[1] * p.y + f.view_proj[2] * p.z + f.view_proj[3];
}

struct WaterVaryings {
    float4 position : SV_Position;
    float3 world : WORLD;   // camera-relative
    float2 flow : FLOW;     // m/s
    float rapids : RAPIDS;  // 0 to 1
    float2 slope : SLOPE;   // the surface's rise a metre along x, z
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
    o.flow = asfloat(b.Load2(at + 16u));
    o.rapids = asfloat(b.Load(at + 24u));
    const uint slope = b.Load(at + 28u);
    o.slope = float2(float(int(slope << 16) >> 16), float(int(slope) >> 16)) * (k_slope_most / 32767.0);
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

// The camera-relative ray through a pixel's centre, a metre deep along the view.
float3 pixel_ray(Frame f, float2 position) {
    const float2 ndc = position / float2(f.width, f.height) * float2(2.0, -2.0) + float2(-1.0, 1.0);
    return f.ray_x.xyz * ndc.x + f.ray_y.xyz * ndc.y + f.ray_z.xyz;
}

float3 water_extinction(Look look) {
    return max(look.absorption + look.turbidity * k_scatter_per_turbidity, float3(1e-3, 1e-3, 1e-3));
}

// What the water scatters at its surface: its colour lit by the sky and
// the sun (`sun`: how much of the sun reaches).
float3 water_source(Frame f, Look look, float sun) {
    const float3 sky_up = ambient_light(f, float3(0.0, 1.0, 0.0));
    return look.color * (sky_up + f.sun_light.rgb * (saturate(f.sun_direction.y) * sun * 0.31830989));
}

// The water's own light along a view `length` metres through it, from
// `start_depth` metres under the surface heading `vy` up (the view's
// direction's y): single scattering of `source`, the light dimmed by the
// water above each point (extinction `sigma`) and by the water back to
// the start. (The view stays under the surface: start_depth - vy * length >= 0.)
float3 water_glow(float3 source, float3 sigma, float start_depth, float length, float vy) {
    const float a = max(1.0 - vy, 1e-3);
    return source * exp(-sigma * max(start_depth, 0.0)) * (1.0 - exp(-sigma * (max(length, 0.0) * a))) / a;
}

// How much the waves gather the sun's light (1: as still water lets it
// through, up to 5) on a bed `depth` metres under the surface at
// camera-relative `xz`, a pixel there `footprint` metres wide; `l`: toward
// the sun. (See the top.)
float caustic(float2 xz, float depth, float footprint, float scale, float3 l) {
    if (depth <= 0.0 || scale <= 0.0 || l.y <= 0.0) {
        return 1.0;
    }
    // The sun's ray, bent by the still surface, came through it here.
    const float3 t = refract(-l, float3(0.0, 1.0, 0.0), 1.0 / 1.33);
    const float2 at = xz + t.xz * (depth / min(t.y, -1e-3));
    ByteAddressBuffer b = read_buffer(push.data);
    // The waves' curvature there (each wave's slope changing across it).
    float3 m = float3(0.0, 0.0, 0.0); // xx, xz, zz
    [loop]
    for (uint i = 0; i < push.wave_count; ++i) {
        const uint w_at = push.waves_offset + i * 32u;
        const float4 w = asfloat(b.Load4(w_at));
        const float phase = asfloat(b.Load(w_at + 16u));
        const float wavelength = 6.2831853 / w.z;
        const float seen = saturate(wavelength / max(footprint * 4.0, 1e-6) - 1.0);
        const float bend = w.w * scale * seen * w.z * sin(w.z * dot(w.xy, at) + phase);
        m += float3(w.x * w.x, w.x * w.y, w.y * w.y) * bend;
    }
    // A ray through a surface leaning by slope s turns about (1 - 1 / 1.33) s.
    const float c = depth * (1.0 - 1.0 / 1.33) * k_caustic_ripples;
    // (m is minus the height's curvature: a crest, a lens, gathers.)
    const float det = (1.0 - c * m.x) * (1.0 - c * m.z) - c * c * m.y * m.y;
    const float gathered = min(1.0 / max(abs(det), 0.2), 5.0);
    return lerp(1.0, gathered, exp(-depth / k_caustic_fade));
}

// How much of what a lit bed shows the caustics gather: the sun's share of
// the light from above (`sun`: how much of it reaches), the sky's the rest.
float caustic_weight(Frame f, float sun) {
    const float3 luma = float3(0.2126, 0.7152, 0.0722);
    const float from_sun = dot(f.sun_light.rgb, luma) * saturate(f.sun_direction.y) * sun * 0.31830989;
    const float from_sky = dot(ambient_light(f, float3(0.0, 1.0, 0.0)), luma);
    return from_sun / max(from_sun + from_sky, 1e-6);
}

// The waves' normal on running water: carried `flow` (m/s) downstream
// from two starts half a period apart, each faded out as it starts again
// (a flow map's two phases; the second shifted so the two differ), the
// blend's slopes and crests scaled back to one sum's spread (two unlike
// patterns halfway averaged would be calmer); the blend comes in as the
// water starts to run. Still water's as it is.
float3 flowing_normal(float2 xz, float2 flow, float footprint, float scale, out float crest) {
    const float speed = length(flow);
    if (speed < 1e-3) {
        return wave_normal(xz, footprint, scale, crest);
    }
    const float t0 = saturate(push.flow_clock / k_flow_period);
    const float t1 = frac(t0 + 0.5);
    const float w = abs(2.0 * t0 - 1.0); // the second's share: all of it as the first starts again
    // (Slower than 0.1 m/s the two phases come together into still water's
    // waves, never seen as either starts again.)
    const float running = saturate(speed / 0.1);
    const float2 xz0 = xz - flow * ((t0 - 0.5) * k_flow_period);
    const float2 xz1 = xz - flow * ((t1 - 0.5) * k_flow_period) + float2(3.7, -2.3) * running;
    ByteAddressBuffer b = read_buffer(push.data);
    float2 slope0 = float2(0.0, 0.0);
    float2 slope1 = float2(0.0, 0.0);
    float height0 = 0.0;
    float height1 = 0.0;
    float total = 0.0;
    [loop]
    for (uint i = 0; i < push.wave_count; ++i) {
        const uint at = push.waves_offset + i * 32u;
        const float4 wave = asfloat(b.Load4(at));
        const float phase = asfloat(b.Load(at + 16u));
        const float wavelength = 6.2831853 / wave.z;
        const float seen = saturate(wavelength / max(footprint * 4.0, 1e-6) - 1.0);
        const float theta0 = wave.z * dot(wave.xy, xz0) + phase;
        const float theta1 = wave.z * dot(wave.xy, xz1) + phase;
        const float a = wave.w * scale * seen;
        slope0 += wave.xy * (a * cos(theta0));
        slope1 += wave.xy * (a * cos(theta1));
        height0 += seen * sin(theta0);
        height1 += seen * sin(theta1);
        total += 1.0;
    }
    const float spread = lerp(1.0, rsqrt(max((1.0 - w) * (1.0 - w) + w * w, 0.5)), running);
    const float2 slope = (slope0 * (1.0 - w) + slope1 * w) * spread;
    const float height = (height0 * (1.0 - w) + height1 * w) * spread;
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

// The water between the camera and a point `distance` metres out along
// `v` (from the camera), over `color` there: where this pixel's near plane
// is under the camera's water (FrameInput::underwater), from the near
// plane on (the view stays under the surface that far: the caller's).
float3 under_camera_water(Frame f, float3 color, float3 v, float distance, float3 ray) {
    if (push.under_on == 0u || ray.y * f.near_plane >= push.under_level) {
        return color;
    }
    const Look look = load_look(push.under_look);
    const float3 sigma = water_extinction(look);
    const float start = f.near_plane * length(ray);
    const float length_through = max(distance - start, 0.0);
    return color * exp(-sigma * length_through) +
           water_glow(water_source(f, look, 1.0), sigma, push.under_level - v.y * start, length_through, v.y);
}

// The surface seen from below (`v` heading up, `n` the waves' normal, up).
float3 shade_below(Frame f, bool screen_on, bool sky_on, Sky sky, Look look, float3 n, float3 p, float distance, float3 v,
                   float footprint, Texture2D<float> depth, Texture2D<float4> light) {
    const float3 down = -n;
    // Out into the air (Snell, 1.33 to 1), unless it is all reflected.
    const float3 t = refract(v, down, 1.33);
    float fresnel = 1.0;
    float3 above = float3(0.0, 0.0, 0.0);
    if (dot(t, t) > 1e-8) {
        const float cos_t = saturate(dot(t, n));
        fresnel = k_water_f0 + (1.0 - k_water_f0) * pow(1.0 - cos_t, 5.0);
        const float3 t_up = normalize(float3(t.x, max(t.y, 0.01), t.z));
        above = sky_on ? sample_sky(sky, t_up) : ambient_light(f, t_up);
        // What the screen shows that way (as if far), where that lies above the water.
        const float4 far_clip = to_clip(f, p + t_up * 1.0e4);
        if (far_clip.w > 1e-4) {
            const float2 uv = far_clip.xy / far_clip.w * float2(0.5, -0.5) + 0.5;
            if (all(uv >= 0.0) && all(uv < 1.0)) {
                const int2 px = int2(uv * float2(f.width, f.height));
                const float d = depth.Load(int3(px, 0));
                const float3 q = pixel_ray(f, float2(px) + 0.5) * (d > 0.0 ? f.near_plane / d : 1.0e6);
                if (q.y > p.y) {
                    above = light.Load(int3(px, 0)).rgb;
                }
            }
        }
    }
    // Beyond the window the water under it mirrored: its own light, seen
    // down from the surface into water as deep as it goes.
    const float3 r = reflect(v, down);
    const float3 source = water_source(f, look, 1.0);
    const float3 sigma = water_extinction(look);
    float3 mirrored = water_glow(source, sigma, 0.0, 1.0e4, min(r.y, 0.0));
    // What the screen shows along the mirror (a pool's floor), through the
    // water back to the surface.
    int2 hit_px = int2(0, 0);
    float trust = 0.0;
    if (screen_on && fresnel > 0.0 &&
        screen_ray(f, depth, p - float3(0.0, 2.0 * footprint * max(v.y, 0.05), 0.0), r, hit_px, trust) && trust > 0.0) {
        const float d = depth.Load(int3(hit_px, 0));
        if (d > 0.0) {
            const float3 q = pixel_ray(f, float2(hit_px) + 0.5) * (f.near_plane / d);
            const float along = length(q - p);
            const float3 seen = light.Load(int3(hit_px, 0)).rgb * exp(-sigma * along) +
                                water_glow(source, sigma, 0.0, along, min(r.y, 0.0));
            mirrored = lerp(mirrored, seen, trust);
        }
    }
    const float3 color = above * (1.0 - fresnel) + mirrored * fresnel;
    // (`p` is on the view: its ray is p over its view-space depth.)
    const float own_view_z = -dot(f.view_rows[2].xyz, p);
    const float3 shown = under_camera_water(f, color, v, distance, p / max(own_view_z, 1e-6));
    return min(max(shown, float3(0.0, 0.0, 0.0)), float3(60000.0, 60000.0, 60000.0));
}

[shader("pixel")]
float4 ps_main(WaterVaryings i) : SV_Target {
    const Frame f = load_frame(push.frame, push.frame_offset);
    const int2 pixel = int2(i.position.xy);
    Texture2D<float> depth = read_texture_float(push.depth);
    Texture2D<float4> light = read_texture(push.light);
    const float scene_d = depth.Load(int3(pixel, 0));
    // How the bed's depth (straight down) changes from pixel to pixel, for
    // the surf (worked out before any pixel leaves: neighbours' differences).
    float bed_change;
    {
        const float here = length(i.world);
        const float view_z = max(-dot(f.view_rows[2].xyz, i.world), 1e-6);
        const float scene_z = scene_d > 0.0 ? min(f.near_plane / scene_d, 1.0e6) : 1.0e6;
        const float bed_here = min(max(scene_z - view_z, 0.0) * (here / view_z) * max(-i.world.y / max(here, 1e-6), 0.0), 1.0e4);
        bed_change = length(float2(ddx(bed_here), ddy(bed_here)));
    }
    // Behind what shading drew here: not seen.
    if (scene_d > i.position.z) {
        discard;
    }
    const float3 p = i.world;
    const float distance = length(p);
    const float3 v = p / max(distance, 1e-6); // from the camera
    const Look look = load_look(i.look);
    // Shading's extras: shadows, the voxel light, the sky, the lamps.
    const bool have = push.extras_on != 0u;
    // (Without, the frame's descriptor: read, never used.)
    const Extras e = load_extras(have ? push.extras : push.frame, have ? push.extras_offset : 0u);
    const bool sky_on = have && e.sky_on != 0u;
    const Sky sky = load_sky(have ? e.sky_params : push.frame, have ? e.sky_params_offset : 0u);
    // A pixel's width on the surface there (metres).
    const float footprint = distance * 2.0 / max(f.p11 * f.height, 1e-6) / max(abs(v.y), 0.05);
    // Running water: rougher the faster it runs, and in rapids.
    const float speed = length(i.flow);
    const float rapids = saturate(i.rapids);
    const float rough = saturate(0.5 * saturate(speed / k_run_rough) + rapids);
    const float scale = look.waves * (1.0 + 1.5 * rough) + rough;
    float crest;
    float3 n = flowing_normal(p.xz, i.flow, footprint, scale, crest);
    // Leaning as the surface does (its slope and the waves', added).
    if (any(i.slope != float2(0.0, 0.0))) {
        n = normalize(n / max(n.y, 1e-3) - float3(i.slope.x, 0.0, i.slope.y));
    }
    if (v.y > 0.0) {
        const bool screen_on = have && (e.reflect_on & 2u) != 0u;
        return float4(shade_below(f, screen_on, sky_on, sky, look, n, p, distance, v, footprint, depth, light), 1.0);
    }
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
    // (Against the surface as it lies: a sloping river's own lean is no wave.)
    const float3 t_still = refract(v, normalize(float3(-i.slope.x, 1.0, -i.slope.y)), 1.0 / 1.33);
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
    // The sun's light gathered by the waves on the bed seen (as lit as the surface over it).
    float gather = 1.0;
    if (under_z < 1.0e37 && scale > 0.0 && sun > 0.0) {
        const float3 bed_point = pixel_ray(f, float2(bent_px) + 0.5) * under_z;
        const float bed_under = p.y - bed_point.y;
        const float bed_distance = length(bed_point);
        // (A pixel's width on a level bed there.)
        const float bed_footprint = bed_distance * 2.0 / max(f.p11 * f.height, 1e-6) /
                                    max(abs(bed_point.y) / max(bed_distance, 1e-6), 0.05);
        gather = lerp(1.0, caustic(bed_point.xz, bed_under, bed_footprint, scale, l), caustic_weight(f, sun));
    }
    // The water's own colour, lit by the sun and the sky (as a rough
    // surface would be lit under the surface), where its depth hides what is under it.
    const float3 sky_up = ambient_light(f, float3(0.0, 1.0, 0.0));
    // (The same light as from below: the water_glow along the view bent into it.)
    const float3 deep = water_glow(water_source(f, look, sun), extinction, 0.0, through_water, t_still.y);
    // Foam where the bed comes near (how deep straight down), broken up by
    // the waves' crests.
    const float bed_depth = straight_full * max(-v.y, 0.0);
    const float shore = saturate(1.0 - bed_depth / k_foam_depth);
    const float shore_foam = look.waves > 0.0 ? saturate(shore * 1.6 - 1.0 + (crest - 0.5) * 1.5) * saturate(look.waves) : 0.0;
    // White water: in rapids, and where it runs fast, on the crests carried down.
    // (None on still or slow water: its crests stay clear.)
    const float white = rapids > 0.0 || speed > 2.0
                            ? saturate(rapids * 1.7 + saturate((speed - 2.0) / 6.0) * 0.6 - 0.7 + (crest - 0.5) * 1.5) *
                                  saturate(rapids * 20.0 + (speed - 2.0))
                            : 0.0;
    // Surf: where it shoals, waves run in to the shore in bands (their
    // crests along the depth's contours, coming in as the depth they are
    // at falls), breaking into foam as it gets shallow, broken up by the crests.
    float surf_foam = 0.0;
    // (Where the bed shelves: over a level one, a pool's, nothing runs in.)
    // (Far steeper is an edge: a body's side, a rock's, not a beach.)
    const float bed_slope = bed_change / max(footprint, 1e-6);
    const float shelving = saturate(bed_slope / k_surf_shelving) * saturate(2.0 - bed_slope / k_surf_edge);
    if (look.waves > 0.0 && bed_depth < k_surf_depth && shelving > 0.0) {
        const float theta = 6.2831853 * (bed_depth / k_surf_spacing + push.surf_clock / k_surf_period);
        const float band = pow(saturate(0.5 + 0.5 * sin(theta)), 6.0);
        const float breaking = saturate(1.2 - bed_depth / k_break_depth);
        surf_foam = saturate(band * breaking * push.surf * saturate(look.waves) * (0.6 + crest)) * shelving;
    }
    const float foam = max(max(shore_foam, white), surf_foam);
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
    const float alpha = 0.02 + 0.03 * saturate(look.waves) + 0.08 * rough; // the glint's spread
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
    const float3 own = lerp(deep * (1.0 - fresnel) + mirrored * fresnel + glint + lamps, foam_light, foam);
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
    color += max(seen - air, float3(0.0, 0.0, 0.0)) * transmitted * ((1.0 - fresnel) * (1.0 - foam) * gather);
    // The screen's reflection over the mirror's (it holds its own air, as reflect.slang's).
    if (trust > 0.0) {
        color += (light.Load(int3(hit_px, 0)).rgb - mirrored) * (fresnel * (1.0 - foam) * through * trust);
    }
    color = min(max(color, float3(0.0, 0.0, 0.0)), float3(60000.0, 60000.0, 60000.0));
    return float4(color, 1.0);
}

[shader("pixel")]
float4 ps_copy(float4 position : SV_Position) : SV_Target {
    const int2 pixel = int2(position.xy);
    float4 c = read_texture(push.light).Load(int3(pixel, 0));
    if (push.under_on == 0u) {
        return c;
    }
    const Frame f = load_frame(push.frame, push.frame_offset);
    const float3 ray = pixel_ray(f, position.xy);
    if (ray.y * f.near_plane >= push.under_level) {
        return c; // this pixel's near plane is above the surface
    }
    const float ray_length = length(ray);
    const float3 v = ray / ray_length;
    const float d = read_texture_float(push.depth).Load(int3(pixel, 0));
    float distance = d > 0.0 ? f.near_plane / d * ray_length : 1.0e6;
    // Not past where the view comes up through the surface (that the
    // surface's own pass draws over).
    if (v.y > 1e-5 && push.under_level > 0.0) {
        distance = min(distance, push.under_level / v.y);
    }
    const Look look = load_look(push.under_look);
    // Caustics on what is seen, where it is under the surface and the sun reaches it.
    const float3 l = f.sun_direction.xyz;
    if (d > 0.0 && look.waves > 0.0 && l.y > 0.0) {
        const float3 p = ray * (f.near_plane / d);
        const float bed_under = push.under_level - p.y;
        if (bed_under > 0.0) {
            const float footprint = length(p) * 2.0 / max(f.p11 * f.height, 1e-6) / max(abs(v.y), 0.05);
            float sun = 1.0;
            if (push.extras_on != 0u) {
                const Extras e = load_extras(push.extras, push.extras_offset);
                sun = sun_visibility(e.shadows, p, float3(0.0, 1.0, 0.0), l, footprint);
                if (e.sky_on != 0u) {
                    sun *= cloud_shadow(load_sky(e.sky_params, e.sky_params_offset), p);
                }
            }
            if (sun > 0.0) {
                c.rgb *= lerp(1.0, caustic(p.xz, bed_under, footprint, look.waves, l), caustic_weight(f, sun));
            }
        }
    }
    c.rgb = min(under_camera_water(f, c.rgb, v, distance, ray), float3(60000.0, 60000.0, 60000.0));
    c.a = 1.0; // (the sky behind is water now: exposed as everything drawn is)
    return c;
}
