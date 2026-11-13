// Particles (effects plan FX1b): drawn from the ring src/particles.h keeps,
// after water and before post, over the light before exposure.
//
// - cs_prepare: each alive particle once. Where it is at its age (the same
//   closed form as particle_position in src/particles.cpp, plus its style's
//   seeded wobble), how big, how opaque, its colour; lit at its centre by
//   the sun (shadowed, the clouds' shadow), the sky (or the voxel world's
//   bounce light where it has some), the lamps listed in its cluster
//   (shadowed); its own light added; the air between it and the camera
//   over it (the clear air, fog, clouds, what falls, dust) as shading puts
//   over a surface there. Dead, cut off by the frustum, or of a bad style:
//   size 0, not drawn.
// - vs_main / ps_main: a quad for each (a puff facing the camera; a streak
//   along its motion as seen), at half size, into weighted blended
//   transparency's sums (McGuire and Bavoil 2013: no sorting, so nothing
//   pops as the order changes). Each pixel tests the scene's depth (no
//   depth target) and fades over `soft` metres into what is behind it, and
//   near the camera.
// - vs_full / ps_composite: the light copied into a target of its own with
//   the sums brought up to full size over it, each full pixel taking the
//   half-size pixels whose scene depth matches its own (so nothing bleeds
//   over an edge in front). Over the flat background (shown as it is, not
//   exposed: alpha 0) the particles are exposed and tone mapped here, as
//   resolve.slang does dust there.
//
// Nothing here keeps anything from frame to frame: the same age is the
// same place, look and light (no temporal noise, AGENTS.md rule 2).
//
// Layout: ParticlePush, Particle, ParticleStyle, ParticleDrawn in
// src/gpu_layout.h.
#include "common.slang"
#include "shading_extras.slang"
#include "voxel_gi.slang"
#include "tone_map.slang"

struct ParticlePush {
    uint frame;
    uint frame_offset;
    uint ring;
    uint drawn;
    uint styles;
    uint styles_offset;
    uint style_count;
    uint first;
    uint count;
    uint now;
    uint extras;
    uint extras_offset;
    float3 anchor;
    uint extras_on;
    uint depth;
    uint accum;
    uint reveal;
    uint half_width;
    uint half_height;
    uint light;
    uint pad1;
    uint pad2;
};
ConstantBuffer<ParticlePush> push : register(b0);

static const uint k_max_particles = 262144u;  // src/particles.h's ring (renderer.h's k_max_particles)
static const uint k_particle_bytes = 64u;     // gpu::Particle
static const uint k_style_bytes = 80u;        // gpu::ParticleStyle
static const uint k_drawn_bytes = 64u;        // gpu::ParticleDrawn
static const uint k_style_streak = 1u;
static const uint k_style_lit = 2u;
static const uint k_style_usable = 4u;
static const float k_fade_in = 0.1;           // the share of its life it fades in over
static const float k_near_fade = 0.5;         // metres from the camera over which it fades out

struct Particle {
    float3 position; // at birth, relative to the anchor
    uint born;
    float3 velocity;
    float life;
    float3 air;
    float stop;
    float lift;
    float drag;
    float size;
    uint seed_style;
};

Particle load_particle(ByteAddressBuffer b, uint at) {
    const float4 a = asfloat(b.Load4(at));
    const float4 c = asfloat(b.Load4(at + 16u));
    const float4 d = asfloat(b.Load4(at + 32u));
    const uint4 e = b.Load4(at + 48u);
    Particle p;
    p.position = a.xyz;
    p.born = asuint(a.w);
    p.velocity = c.xyz;
    p.life = c.w;
    p.air = d.xyz;
    p.stop = d.w;
    p.lift = asfloat(e.x);
    p.drag = asfloat(e.y);
    p.size = asfloat(e.z);
    p.seed_style = e.w;
    return p;
}

struct Style {
    float size_start;
    float size_end;
    float opacity_start;
    float opacity_end;
    float3 color_start;
    float emissive_end;
    float3 color_end;
    float soft;
    float3 emissive;
    uint flags;
    float wobble;
};

Style load_style(ByteAddressBuffer b, uint at) {
    const float4 a = asfloat(b.Load4(at));
    const float4 c = asfloat(b.Load4(at + 16u));
    const float4 d = asfloat(b.Load4(at + 32u));
    const float4 e = asfloat(b.Load4(at + 48u));
    Style s;
    s.size_start = a.x;
    s.size_end = a.y;
    s.opacity_start = a.z;
    s.opacity_end = a.w;
    s.color_start = c.xyz;
    s.emissive_end = c.w;
    s.color_end = d.xyz;
    s.soft = d.w;
    s.emissive = e.xyz;
    s.flags = asuint(e.w);
    s.wobble = asfloat(b.Load(at + 64u));
    return s;
}

// E1 = (1 - e^-kt) / k, E2 = (t - E1) / k and e^-kt, by series under kt
// 0.5 (src/particles.cpp's fall: the same terms).
void fall(float k, float t, out float e1, out float e2, out float decay) {
    const float x = k * t;
    if (x < 0.5) {
        e1 = t * (1.0 - x * (1.0 / 2.0 - x * (1.0 / 6.0 - x * (1.0 / 24.0 - x * (1.0 / 120.0 -
                 x * (1.0 / 720.0 - x * (1.0 / 5040.0 - x * (1.0 / 40320.0))))))));
        e2 = t * t * (1.0 / 2.0 - x * (1.0 / 6.0 - x * (1.0 / 24.0 - x * (1.0 / 120.0 - x * (1.0 / 720.0 -
                     x * (1.0 / 5040.0 - x * (1.0 / 40320.0 - x * (1.0 / 362880.0))))))));
        decay = 1.0 - k * e1;
    } else {
        decay = exp(-x);
        e1 = (1.0 - decay) / k;
        e2 = (t - e1) / k;
    }
}

uint mix(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float unit(uint h) { return float(h >> 8) * (1.0 / 16777216.0); }

// A slow seeded wander, zero at birth (smoke curling): three sines a
// particle's own, half as much up and down.
float3 wobble(uint seed, float age, float amount) {
    if (amount <= 0.0) {
        return float3(0.0, 0.0, 0.0);
    }
    const uint h0 = mix(seed ^ 0x51ed270bu);
    const uint h1 = mix(h0);
    const uint h2 = mix(h1);
    const float3 rate = 6.2831853 * (0.15 + 0.35 * float3(unit(h0), unit(h1), unit(h2)));
    const float3 phase = 6.2831853 * float3(unit(mix(h0 + 1u)), unit(mix(h1 + 1u)), unit(mix(h2 + 1u)));
    const float3 w = sin(rate * age + phase) - sin(phase);
    return amount * float3(w.x, 0.5 * w.y, w.z);
}

struct ParticleDrawn {
    float3 center;
    float size;
    float3 light;
    float opacity;
    float3 axis;
    float soft;
    uint seed;
    float view_z;
    uint streak; // 1: drawn as a streak (a capsule), even when not moving
};

ParticleDrawn none() {
    ParticleDrawn d;
    d.center = float3(0.0, 0.0, 0.0);
    d.size = 0.0;
    d.light = float3(0.0, 0.0, 0.0);
    d.opacity = 0.0;
    d.axis = float3(0.0, 0.0, 0.0);
    d.soft = 0.0;
    d.seed = 0u;
    d.view_z = 0.0;
    d.streak = 0u;
    return d;
}

void store_drawn(RWByteAddressBuffer b, uint at, ParticleDrawn d) {
    b.Store4(at, asuint(float4(d.center, d.size)));
    b.Store4(at + 16u, asuint(float4(d.light, d.opacity)));
    b.Store4(at + 32u, asuint(float4(d.axis, d.soft)));
    b.Store4(at + 48u, uint4(d.seed, asuint(d.view_z), d.streak, 0u));
}

ParticleDrawn load_drawn(ByteAddressBuffer b, uint at) {
    const float4 a = asfloat(b.Load4(at));
    const float4 c = asfloat(b.Load4(at + 16u));
    const float4 e = asfloat(b.Load4(at + 32u));
    const uint3 g = b.Load3(at + 48u);
    ParticleDrawn d;
    d.center = a.xyz;
    d.size = a.w;
    d.light = c.xyz;
    d.opacity = c.w;
    d.axis = e.xyz;
    d.soft = e.w;
    d.seed = g.x;
    d.view_z = asfloat(g.y);
    d.streak = g.z;
    return d;
}

float4 to_clip(Frame f, float3 p) {
    return f.view_proj[0] * p.x + f.view_proj[1] * p.y + f.view_proj[2] * p.z + f.view_proj[3];
}

// The lamps' light on a particle at `p` (camera-relative), seen from the
// camera along `v`: as a soft ball, lit a little on its far side too.
float3 lamps_on(Extras e, Frame f, float2 position, float3 p, float view_z, float3 v) {
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
        float falloff = light_falloff(lamp, to_light, l);
        if (falloff <= 0.0) {
            continue;
        }
        if (lamp.shadow != 0u && e.local.atlas_on != 0u) {
            falloff *= light_shadow(e.local, b, lamp, p, l);
        }
        const float wrap = 0.25 + 0.75 * saturate(0.5 - 0.5 * dot(v, l));
        sum += lamp.color * (falloff * wrap / 3.14159265);
    }
    return sum;
}

[numthreads(64, 1, 1)]
void cs_prepare(uint3 id : SV_DispatchThreadID) {
    const uint i = id.x;
    if (i >= push.count) {
        return;
    }
    RWByteAddressBuffer out_buffer = write_buffer(push.drawn);
    const uint out_at = i * k_drawn_bytes;
    const Particle p = load_particle(read_buffer(push.ring), ((push.first + i) % k_max_particles) * k_particle_bytes);
    const uint style_index = p.seed_style & 0xffu;
    const float age = float(push.now - p.born) * 1e-6; // (wrapping: live ones are under a minute old)
    if (style_index >= push.style_count || !(age < p.life) || !(p.life > 0.0)) {
        store_drawn(out_buffer, out_at, none());
        return;
    }
    const Style s = load_style(read_buffer(push.styles), push.styles_offset + style_index * k_style_bytes);
    if ((s.flags & k_style_usable) == 0u) {
        store_drawn(out_buffer, out_at, none());
        return;
    }
    const Frame f = load_frame(push.frame, push.frame_offset);
    const uint seed = p.seed_style >> 8;
    // Where it is: the closed form, held at its stop, and its wobble.
    const float t = clamp(age, 0.0, max(p.stop, 0.0));
    float e1;
    float e2;
    float decay;
    fall(p.drag, t, e1, e2, decay);
    float3 center = push.anchor + p.position + p.air * t + (p.velocity - p.air) * e1 + float3(0.0, p.lift, 0.0) * e2 +
                    wobble(seed, age, s.wobble);
    const float3 velocity = age < p.stop ? p.air + (p.velocity - p.air) * decay + float3(0.0, p.lift, 0.0) * e1
                                         : float3(0.0, 0.0, 0.0);
    const float a = age / p.life;
    const float size = 0.5 * lerp(s.size_start, s.size_end, a) * p.size;
    const float opacity = lerp(s.opacity_start, s.opacity_end, a) * saturate(a / k_fade_in);
    // In view (a ball as big as it, and its streak), else not drawn.
    const float3 axis = (s.flags & k_style_streak) != 0u ? velocity * (0.5 / 60.0) : float3(0.0, 0.0, 0.0);
    const float reach = size + length(axis);
    const float view_z = -dot(f.view_rows[2].xyz, center);
    bool seen = size > 0.0 && opacity > 0.0 && view_z + reach > f.near_plane;
    [unroll] for (uint k = 0; k < 5u && seen; ++k) {
        seen = dot(f.planes[k].xyz, center) + f.planes[k].w >= -reach;
    }
    if (!seen) {
        store_drawn(out_buffer, out_at, none());
        return;
    }
    const float distance = length(center);
    const float3 v = center / max(distance, 1e-6); // from the camera
    const float4 clip = to_clip(f, center);
    const float2 ndc = clip.w > 1e-6 ? clip.xy / clip.w : float2(0.0, 0.0);
    const float2 position = float2(saturate(ndc.x * 0.5 + 0.5) * f.width, saturate(0.5 - ndc.y * 0.5) * f.height);
    const uint2 pixel = uint2(min(position, float2(f.width, f.height) - 1.0));
    const float3 color = lerp(s.color_start, s.color_end, a);
    float3 light = s.emissive * lerp(1.0, s.emissive_end, a);
    const bool have = push.extras_on != 0u;
    // (Without, the frame's descriptor: read, never used.)
    const Extras e = load_extras(have ? push.extras : push.frame, have ? push.extras_offset : 0u);
    const bool sky_on = have && e.sky_on != 0u;
    const Sky sky = load_sky(have ? e.sky_params : push.frame, have ? e.sky_params_offset : 0u);
    if ((s.flags & k_style_lit) != 0u) {
        // The sun: shadowed, through the clouds; a soft ball lit a little
        // on its far side too.
        const float3 l = f.sun_direction.xyz;
        const float pixel_size = distance * 2.0 / max(f.p11 * f.height, 1e-6);
        float sun = have ? sun_visibility(e.shadows, center, l, l, max(pixel_size, size)) : 1.0;
        if (sky_on) {
            sun *= cloud_shadow(sky, center);
        }
        const float wrap = 0.25 + 0.75 * saturate(0.5 - 0.5 * dot(v, l));
        float3 lit = f.sun_light.rgb * (sun * wrap / 3.14159265);
        // From all round: the sky and ground (as dust's), or the voxel
        // world's bounce light where it has some.
        float3 below;
        float3 above;
        ambient_light2(f, float3(0.0, -1.0, 0.0), float3(0.0, 1.0, 0.0), below, above);
        float3 round_light = lerp(below, above, 0.6);
        if (have && e.gi_on != 0u) {
            VoxelWorld w;
            w.pool = e.voxel_pool;
            w.table = e.voxel_table;
            w.levels = e.voxel_levels;
            w.levels_offset = e.voxel_levels_offset;
            w.level_count = e.voxel_level_count;
            w.bricks_per_axis = e.voxel_bricks_per_axis;
            w.stand_ins = 1u;
            w.lamps = e.gi_lamps;
            w.lamps_on = e.gi_lamps_on;
            const float4 bounce = voxel_bounce(w, write_buffer(e.gi_light), center, -v);
            round_light = lerp(round_light, bounce.rgb, bounce.w);
        }
        lit += round_light;
        if (have) {
            lit += lamps_on(e, f, position, center, view_z, v);
        }
        light += color * lit;
    }
    // The air between it and the camera, as over a surface there.
    if (sky_on) {
        float through;
        light = under_sky(sky, light, v, distance, pixel, through);
    }
    if (have) {
        float through = 1.0;
        light = apply_dust(e, f, light, v, distance, through);
    }
    ParticleDrawn d;
    d.center = center;
    d.size = size;
    d.light = light;
    d.opacity = saturate(opacity);
    d.axis = axis;
    d.soft = s.soft;
    d.seed = seed;
    d.view_z = view_z;
    d.streak = (s.flags & k_style_streak) != 0u ? 1u : 0u;
    store_drawn(out_buffer, out_at, d);
}

struct ParticleVaryings {
    float4 position : SV_Position;
    float2 uv : UV;          // across the quad: -1 to 1 (a streak: x past 1 along its length)
    nointerpolation float body : BODY;   // a streak: its length's half over its size (0: not moving as seen)
    nointerpolation uint streak : STREAK; // 1: a streak (no lumps)
    nointerpolation float4 light : LIGHT; // rgb: its light, a: opacity
    nointerpolation float soft : SOFT;
    nointerpolation uint seed : SEED;
    float view_z : VIEW_Z;
};

ParticleVaryings vs_main(uint vertex_id : SV_VertexID) {
    ParticleVaryings o;
    const uint index = vertex_id / 6u;
    const uint corner_index = vertex_id % 6u;
    // Two triangles: (0, 1, 2), (2, 1, 3) of the corners (-1,-1), (1,-1), (-1,1), (1,1).
    const uint c = corner_index < 3u ? corner_index : (corner_index == 3u ? 2u : (corner_index == 4u ? 1u : 3u));
    const float2 corner = float2((c & 1u) != 0u ? 1.0 : -1.0, (c & 2u) != 0u ? 1.0 : -1.0);
    const ParticleDrawn d = load_drawn(read_buffer(push.drawn), index * k_drawn_bytes);
    o.light = float4(d.light, d.opacity);
    o.soft = d.soft;
    o.seed = d.seed;
    o.body = 0.0;
    o.streak = d.streak;
    o.uv = corner;
    if (!(d.size > 0.0)) {
        o.position = float4(0.0, 0.0, 0.0, 0.0); // (all corners at one point: nothing drawn)
        o.view_z = 0.0;
        return o;
    }
    const Frame f = load_frame(push.frame, push.frame_offset);
    const float3 to_eye = -safe_normalize(d.center, -f.view_rows[2].xyz);
    float3 along = f.view_rows[0].xyz; // a puff: the camera's right and up
    float3 across = f.view_rows[1].xyz;
    float length_half = d.size;
    // A streak: along its motion as seen (across the view), as long as it went.
    const float3 seen_axis = d.axis - to_eye * dot(d.axis, to_eye);
    const float seen_length = length(seen_axis);
    if (seen_length > 1e-4) {
        along = seen_axis / seen_length;
        across = normalize(cross(to_eye, along));
        length_half = d.size + seen_length;
        o.body = seen_length / d.size;
        o.uv = float2(corner.x * (1.0 + o.body), corner.y);
    }
    const float3 world = d.center + along * (corner.x * length_half) + across * (corner.y * d.size);
    o.position = to_clip(f, world);
    o.view_z = -dot(f.view_rows[2].xyz, world);
    return o;
}

// A soft, lumpy blot (a puff's own, by its seed; still from frame to frame).
float lumps(float2 uv, uint seed) {
    const float angle = 6.2831853 * unit(mix(seed));
    const float2 r = float2(cos(angle) * uv.x - sin(angle) * uv.y, sin(angle) * uv.x + cos(angle) * uv.y);
    const float2 shift = float2(unit(mix(seed + 7u)), unit(mix(seed + 13u))) * 17.0;
    const float2 q = r * 1.7 + shift;
    const float2 cell = floor(q);
    const float2 t = q - cell;
    const float2 s = t * t * (3.0 - 2.0 * t);
    const uint cx = uint(int(cell.x) + 4096);
    const uint cy = uint(int(cell.y) + 4096);
    const float n00 = unit(mix(cx * 73856093u ^ cy * 19349663u ^ seed));
    const float n10 = unit(mix((cx + 1u) * 73856093u ^ cy * 19349663u ^ seed));
    const float n01 = unit(mix(cx * 73856093u ^ (cy + 1u) * 19349663u ^ seed));
    const float n11 = unit(mix((cx + 1u) * 73856093u ^ (cy + 1u) * 19349663u ^ seed));
    return lerp(lerp(n00, n10, s.x), lerp(n01, n11, s.x), s.y);
}

struct Sums {
    float4 accum : SV_Target0;  // light x opacity x weight, opacity x weight
    float4 reveal : SV_Target1; // r: opacity (the blend keeps what shows through)
};

Sums ps_main(ParticleVaryings i) {
    const Frame f = load_frame(push.frame, push.frame_offset);
    // The scene's depth at the full-size pixel standing for this one
    // (ps_composite takes the same).
    const int2 full = int2(i.position.xy) * 2;
    const float d = read_texture_float(push.depth).Load(int3(full, 0));
    const float scene_z = d > 0.0 ? f.near_plane / d : 3.0e38;
    // Shape: round (a streak: a capsule along its length).
    const float along = max(abs(i.uv.x) - i.body, 0.0);
    const float r2 = along * along + i.uv.y * i.uv.y;
    float shape = saturate(1.0 - r2);
    shape *= shape;
    if (i.streak == 0u) {
        shape *= lerp(0.45, 1.0, lumps(i.uv, i.seed));
    }
    const float behind = scene_z - i.view_z;
    const float soft = i.soft > 0.0 ? saturate(behind / i.soft) : (behind > 0.0 ? 1.0 : 0.0);
    const float near = saturate((i.view_z - f.near_plane) / k_near_fade);
    const float alpha = saturate(i.light.a * shape * soft * near);
    Sums o;
    if (alpha < 1.0 / 512.0) {
        discard;
    }
    // McGuire and Bavoil's weight by depth (their equation 9): nearer
    // counts for more where layers overlap.
    const float z = max(i.view_z, 1e-3);
    const float zw = z / 5.0;
    const float zf = z / 200.0;
    const float w = clamp(10.0 / (1e-5 + zw * zw + zf * zf * zf * zf * zf * zf), 1e-2, 3e3);
    // (Kept finite: the sums are 32-bit, the light as lit_output keeps it.)
    o.accum = float4(min(i.light.rgb, float3(60000.0, 60000.0, 60000.0)) * alpha, alpha) * w;
    o.reveal = float4(alpha, alpha, alpha, alpha);
    return o;
}

float4 vs_full(uint vertex_id : SV_VertexID) : SV_Position {
    const float2 p = float2(vertex_id == 1 ? 3.0 : -1.0, vertex_id == 2 ? 3.0 : -1.0);
    return float4(p, 0.0, 1.0);
}

// One half-size pixel's part in a full one: by how near its scene depth
// lies to the full pixel's.
void take(Texture2D<float4> accum, Texture2D<float4> reveal, Texture2D<float> depth, Frame f, int2 at, float weight,
          float own_z, inout float4 sum_accum, inout float sum_reveal, inout float sum_weight) {
    const int2 half_size = int2(int(push.half_width), int(push.half_height));
    const int2 t = clamp(at, int2(0, 0), half_size - 1);
    const float d = depth.Load(int3(t * 2, 0));
    const float z = d > 0.0 ? f.near_plane / d : 3.0e38;
    const float near = abs(z - own_z) / max(min(z, own_z), 1e-3);
    const float w = weight / (1e-3 + near);
    sum_accum += accum.Load(int3(t, 0)) * w;
    sum_reveal += reveal.Load(int3(t, 0)).r * w;
    sum_weight += w;
}

float4 ps_composite(float4 position : SV_Position) : SV_Target0 {
    const int2 pixel = int2(position.xy);
    const float4 under = read_texture(push.light).Load(int3(pixel, 0));
    const Frame f = load_frame(push.frame, push.frame_offset);
    Texture2D<float4> accum = read_texture(push.accum);
    Texture2D<float4> reveal = read_texture(push.reveal);
    Texture2D<float> depth = read_texture_float(push.depth);
    const float d = depth.Load(int3(pixel, 0));
    const float own_z = d > 0.0 ? f.near_plane / d : 3.0e38;
    // The four half-size pixels round this one, bilinear, then by depth.
    const float2 at = (position.xy * 0.5) - 0.5;
    const int2 base = int2(floor(at));
    const float2 fr = at - float2(base);
    float4 sum_accum = float4(0.0, 0.0, 0.0, 0.0);
    float sum_reveal = 0.0;
    float sum_weight = 0.0;
    take(accum, reveal, depth, f, base, (1.0 - fr.x) * (1.0 - fr.y), own_z, sum_accum, sum_reveal, sum_weight);
    take(accum, reveal, depth, f, base + int2(1, 0), fr.x * (1.0 - fr.y), own_z, sum_accum, sum_reveal, sum_weight);
    take(accum, reveal, depth, f, base + int2(0, 1), (1.0 - fr.x) * fr.y, own_z, sum_accum, sum_reveal, sum_weight);
    take(accum, reveal, depth, f, base + int2(1, 1), fr.x * fr.y, own_z, sum_accum, sum_reveal, sum_weight);
    if (!(sum_weight > 0.0)) {
        return under;
    }
    const float4 a = sum_accum / sum_weight;
    const float shows = saturate(sum_reveal / sum_weight); // what shows through: 1 where none
    if (shows > 0.9995 || !(a.a > 0.0)) {
        return under;
    }
    // The weighted mean of their light, as opaque as all of them together.
    float3 light = min(a.rgb / a.a, float3(60000.0, 60000.0, 60000.0));
    if (under.a <= 0.0) {
        // Over what is shown as it is: exposed and tone mapped here.
        const float3 exposed = light * f.exposure;
        light = f.tone_mapper == 1u ? tone_map_agx(exposed) : tone_map(exposed);
    }
    return float4(lerp(under.rgb, light, 1.0 - shows), under.a);
}
