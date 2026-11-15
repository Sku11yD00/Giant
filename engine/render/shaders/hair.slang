// Hair (characters plan C6c): grooms drawn as strands, after opaque shading
// and before water and particles, over the light before exposure.
//
// Strands are far thinner than a pixel, so hair is a matter of coverage,
// and the visibility buffer's one surface a pixel cannot hold it without
// history (AGENTS.md rule 2). It has passes of its own:
//
// - cs_prepare: each drawn strand once. Its 16 points posed (each moves as
//   its guides' points moved from rest, by its weights), put in the world,
//   and lit: the sun through three lobes along the strand (Marschner's R,
//   TT and TRT as Karis fits them, "Physically based hair shading in
//   Unreal") and a scattered term, shadowed at three places along it; the
//   sky (or the voxel world's bounce light) once; the air between it and
//   the camera over it. Nothing downstream lights anything.
// - vs_ribbon: a ribbon facing the camera along each strand, from the
//   vertex id alone. It is drawn half a pixel wider each side than the
//   strand is, and each pixel takes the share of itself the strand covers
//   (a box a pixel wide over a line of the strand's width), so strands a
//   tenth of a pixel wide are smooth lines a tenth as opaque, with no
//   samples to flicker between.
// - ps_front: the ribbons' nearest depth (a depth target of hair's own).
// - ps_main: the ribbons again into weighted sums. A pixel's weight falls
//   off sharply behind the nearest hair there, so the colour is the outer
//   layer's (averaged where strands share a pixel), while what shows
//   through is the product of every layer's, in any order.
// - ps_composite: the sums over the light. ps_depth: where hair covers
//   more than half a pixel its depth becomes the scene's, so water,
//   particles and what else comes after treat it as a surface.
//
// Nothing is kept from frame to frame.
//
// Layout: HairPush, HairGroom, HairStrand, HairPoint in src/gpu_layout.h.
#include "common.slang"
#include "shading_extras.slang"
#include "voxel_gi.slang"
#include "tone_map.slang"

struct HairPush {
    uint frame;
    uint frame_offset;
    uint grooms;
    uint grooms_offset;
    uint groom;
    uint drawn;
    uint extras;
    uint extras_offset;
    uint extras_on;
    uint depth;
    uint front;
    uint accum;
    uint reveal;
    uint light;
    uint pad0;
    uint pad1;
};
ConstantBuffer<HairPush> push : register(b0);

static const uint k_strand_points = 16u;
static const uint k_groom_bytes = 144u;   // gpu::HairGroom
static const uint k_strand_bytes = 16u;   // gpu::HairStrand
static const uint k_point_bytes = 8u;     // a packed rest point
static const uint k_drawn_bytes = 16u;    // gpu::HairPoint
static const float k_pi = 3.14159265;
// Behind the nearest hair a layer counts for 1/e as much each this many
// metres (or two pixels' width there, if that is more).
static const float k_layer = 0.003;
// How far behind the scene's surface hair still shows (its roots lie on it).
static const float k_scene_bias = 0.002;
static const float k_scene_bias_far = 0.001; // and this share of the distance (depth's own grain)

struct Groom {
    float4 to_world[3];
    float3 bounds_min;
    uint buffer;
    float3 bounds_size;
    uint points_offset;
    uint posed;
    uint posed_offset;
    uint posed_on;
    uint first;
    float3 color;
    float roughness;
    float half_width;
    float tip_width;
    float variation;
    uint count;
    uint stride;
};

Groom load_groom() {
    ByteAddressBuffer b = read_buffer(push.grooms);
    const uint at = push.grooms_offset + push.groom * k_groom_bytes;
    Groom g;
    g.to_world[0] = asfloat(b.Load4(at));
    g.to_world[1] = asfloat(b.Load4(at + 16u));
    g.to_world[2] = asfloat(b.Load4(at + 32u));
    const uint4 a = b.Load4(at + 48u);
    const uint4 c = b.Load4(at + 64u);
    const uint4 d = b.Load4(at + 80u);
    const uint4 e = b.Load4(at + 96u);
    const uint4 h = b.Load4(at + 112u);
    g.bounds_min = asfloat(a.xyz);
    g.buffer = a.w;
    g.bounds_size = asfloat(c.xyz);
    g.points_offset = c.w;
    g.posed = d.x;
    g.posed_offset = d.y;
    g.posed_on = d.z;
    g.first = d.w;
    g.color = asfloat(e.xyz);
    g.roughness = asfloat(e.w);
    g.half_width = asfloat(h.x);
    g.tip_width = asfloat(h.y);
    g.variation = asfloat(h.z);
    g.count = h.w;
    g.stride = b.Load(at + 128u);
    return g;
}

// A strand's point at rest, in the groom's space.
float3 rest_point(Groom g, ByteAddressBuffer b, uint strand, uint k) {
    const uint2 w = b.Load2(g.points_offset + (strand * k_strand_points + k) * k_point_bytes);
    const float3 share = float3(float(w.x & 0xffffu), float(w.x >> 16), float(w.y & 0xffffu)) * (1.0 / 65535.0);
    return g.bounds_min + share * g.bounds_size;
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

// Light as 9 bits a channel and a shared 5-bit exponent (the layout of
// DXGI's R9G9B9E5).
uint pack_hair_light(float3 c) {
    const float3 v = clamp(c, float3(0.0, 0.0, 0.0), float3(65408.0, 65408.0, 65408.0));
    const float top = max(max(v.r, v.g), max(v.b, 1e-9));
    float e = max(-16.0, floor(log2(top))) + 1.0;
    float scale = exp2(9.0 - e);
    if (floor(top * scale + 0.5) >= 512.0) {
        e += 1.0;
        scale *= 0.5;
    }
    const uint3 m = uint3(floor(v * scale + 0.5));
    return m.r | (m.g << 9) | (m.b << 18) | (uint(e + 15.0) << 27);
}

float3 unpack_hair_light(uint p) {
    const float scale = exp2(float(p >> 27) - 15.0 - 9.0);
    return float3(float(p & 0x1ffu), float((p >> 9) & 0x1ffu), float((p >> 18) & 0x1ffu)) * scale;
}

float4 to_clip(Frame f, float3 p) {
    return f.view_proj[0] * p.x + f.view_proj[1] * p.y + f.view_proj[2] * p.z + f.view_proj[3];
}

float lobe(float width, float angle) {
    return exp(-0.5 * angle * angle / (width * width)) / (2.5066283 * width);
}

float hair_fresnel(float cos_angle) {
    const float m = 1.0 - saturate(cos_angle);
    const float m2 = m * m;
    return 0.04652 + (1.0 - 0.04652) * m2 * m2 * m;
}

// The share of a light's colour a strand along `t` (root to tip) sends
// from `l` (to the light) towards `v` (to the eye): `direct` the three
// lobes, `scattered` the light that has been through many strands (which
// a shadow takes less of).
void hair_light(float3 color, float roughness, float3 t, float3 l, float3 v, out float3 direct, out float3 scattered) {
    const float sin_l = clamp(dot(t, l), -1.0, 1.0);
    const float sin_v = clamp(dot(t, v), -1.0, 1.0);
    const float cos_d = max(cos(0.5 * abs(asin(sin_v) - asin(sin_l))), 1e-3);
    const float3 lp = l - t * sin_l;
    const float3 vp = v - t * sin_v;
    const float cos_phi = dot(lp, vp) * rsqrt(dot(lp, lp) * dot(vp, vp) + 1e-4);
    const float cos_half_phi = sqrt(saturate(0.5 + 0.5 * cos_phi));
    const float r2 = roughness * roughness;
    const float shift = 0.035; // the cuticle's tilt
    const float sum = sin_l + sin_v;
    // R: off the surface, the light's own colour.
    float3 s = float3(1.0, 1.0, 1.0) *
               (lobe(r2, sum + 2.0 * shift) * 0.25 * cos_half_phi * hair_fresnel(sqrt(saturate(0.5 + 0.5 * dot(v, l)))));
    // TT: through the strand, seen against the light.
    {
        const float n_prime = 1.19 / cos_d + 0.36 * cos_d;
        const float a = 1.0 / n_prime;
        const float h = cos_half_phi * (1.0 + a * (0.6 - 0.8 * cos_phi));
        const float f = hair_fresnel(cos_d * sqrt(saturate(1.0 - h * h)));
        const float through = (1.0 - f) * (1.0 - f);
        const float3 tint = pow(max(color, float3(1e-4, 1e-4, 1e-4)), 0.5 * sqrt(saturate(1.0 - h * h * a * a)) / cos_d);
        s += tint * (lobe(0.5 * r2, sum - shift) * exp(-3.65 * cos_phi - 3.98) * through);
    }
    // TRT: in, off the far side and out again: the coloured highlight.
    {
        const float f = hair_fresnel(cos_d * 0.5);
        const float through = (1.0 - f) * (1.0 - f) * f;
        const float3 tint = pow(max(color, float3(1e-4, 1e-4, 1e-4)), 0.8 / cos_d);
        s += tint * (lobe(2.0 * r2, sum - 4.0 * shift) * exp(17.0 * cos_phi - 16.78) * through);
    }
    direct = s;
    // What comes back out of the hair after many strands: soft, the hair's colour.
    const float3 facing = safe_normalize(vp, v);
    const float wrap = saturate((dot(facing, l) + 1.0) * 0.25);
    const float along = sqrt(saturate(1.0 - sin_l * sin_l));
    scattered = sqrt(max(color, float3(0.0, 0.0, 0.0))) * (lerp(wrap, along, 0.33) / k_pi);
}

[numthreads(64, 1, 1)]
void cs_prepare(uint3 id : SV_DispatchThreadID) {
    const Groom g = load_groom();
    const uint strand = id.x;
    if (strand >= g.count) {
        return;
    }
    const Frame f = load_frame(push.frame, push.frame_offset);
    ByteAddressBuffer b = read_buffer(g.buffer);
    const uint4 follow = b.Load4(strand * k_strand_bytes);
    const uint3 guides = uint3(follow.x & 0xffffu, follow.x >> 16, follow.y & 0xffffu);
    float3 weights;
    weights.x = float(follow.y >> 16) * (1.0 / 65535.0);
    weights.y = float(follow.z & 0xffffu) * (1.0 / 65535.0);
    weights.z = saturate(1.0 - weights.x - weights.y);
    const uint seed = follow.w;
    // Posed, in the world (camera-relative).
    float3 points[16];
    {
        // (The frame's descriptor when at rest: read, never used.)
        ByteAddressBuffer posed = read_buffer(g.posed_on != 0u ? g.posed : push.frame);
        [loop] for (uint k = 0u; k < k_strand_points; ++k) {
            float3 p = rest_point(g, b, strand, k);
            if (g.posed_on != 0u) {
                [unroll] for (uint j = 0u; j < 3u; ++j) {
                    const float3 now = asfloat(posed.Load3(g.posed_offset + (guides[j] * k_strand_points + k) * 12u));
                    p += (now - rest_point(g, b, guides[j], k)) * weights[j];
                }
            }
            points[k] = float3(dot(g.to_world[0].xyz, p) + g.to_world[0].w, dot(g.to_world[1].xyz, p) + g.to_world[1].w,
                               dot(g.to_world[2].xyz, p) + g.to_world[2].w);
        }
    }
    const bool have = push.extras_on != 0u;
    // (Without, the frame's descriptor: read, never used.)
    const Extras e = load_extras(have ? push.extras : push.frame, have ? push.extras_offset : 0u);
    const bool sky_on = have && e.sky_on != 0u;
    const Sky sky = load_sky(have ? e.sky_params : push.frame, have ? e.sky_params_offset : 0u);
    const float3 l = f.sun_direction.xyz;
    // The sun's way to it, at three places along it.
    float sun[3];
    [unroll] for (uint q = 0u; q < 3u; ++q) {
        const float3 at = points[2u + q * 6u];
        float reaches = have ? sun_visibility(e.shadows, at, l, l, 0.01) : 1.0;
        if (sky_on) {
            reaches *= cloud_shadow(sky, at);
        }
        sun[q] = reaches;
    }
    // From all round, once: the sky and ground, or the voxel world's
    // bounce light where it has some.
    const float3 middle = points[8];
    const float distance = length(middle);
    const float3 from_eye = middle / max(distance, 1e-6);
    float3 below;
    float3 above;
    ambient_light2(f, float3(0.0, -1.0, 0.0), -from_eye, below, above);
    float3 round_light = lerp(below, above, 0.7);
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
        const float4 bounce = voxel_bounce(w, write_buffer(e.gi_light), middle, -from_eye);
        round_light = lerp(round_light, bounce.rgb, bounce.w);
    }
    // Its own shade, by its seed.
    const float shade = 1.0 + g.variation * (unit(mix(seed ^ 0x2f6b4c1du)) - 0.5);
    const float3 color = saturate(g.color * shade);
    const float4 clip = to_clip(f, middle);
    const float2 ndc = clip.w > 1e-6 ? clip.xy / clip.w : float2(0.0, 0.0);
    const float2 position = float2(saturate(ndc.x * 0.5 + 0.5) * f.width, saturate(0.5 - ndc.y * 0.5) * f.height);
    const uint2 pixel = uint2(min(position, float2(f.width, f.height) - 1.0));
    // The air between it and the camera, as over a surface there: what it
    // adds and what it lets through are the same for every point of the
    // strand (the air is taken at its middle), so they are found once.
    float3 air_adds = float3(0.0, 0.0, 0.0);
    float3 air_keeps = float3(1.0, 1.0, 1.0);
    {
        float3 none = float3(0.0, 0.0, 0.0);
        float3 all = float3(1.0, 1.0, 1.0);
        if (sky_on) {
            float through;
            // (Clouds and fog, not what falls: a drop or flake is a dot on
            // screen, and taken once a strand it would streak the strand.)
            none = clouds_and_fog(sky, none, from_eye, distance, pixel, through);
            all = clouds_and_fog(sky, all, from_eye, distance, pixel, through);
        }
        if (have) {
            float through = 1.0;
            none = apply_dust(e, f, none, from_eye, distance, through);
            through = 1.0;
            all = apply_dust(e, f, all, from_eye, distance, through);
        }
        air_adds = none;
        air_keeps = all - none;
    }
    RWByteAddressBuffer out_buffer = write_buffer(push.drawn);
    [loop] for (uint k = 0u; k < k_strand_points; ++k) {
        const float3 p = points[k];
        const float3 ahead = points[min(k + 1u, k_strand_points - 1u)];
        const float3 behind = points[k > 0u ? k - 1u : 0u];
        const float3 t = safe_normalize(ahead - behind, float3(0.0, -1.0, 0.0));
        const float3 v = -safe_normalize(p, f.view_rows[2].xyz);
        float3 direct;
        float3 scattered;
        hair_light(color, g.roughness, t, l, v, direct, scattered);
        // The sun at this point: between the three places it was looked for.
        const float along = float(k) - 2.0;
        const float reaches = along <= 6.0 ? lerp(sun[0], sun[1], saturate(along / 6.0))
                                           : lerp(sun[1], sun[2], saturate((along - 6.0) / 6.0));
        float3 light = f.sun_light.rgb * reaches * (direct + scattered);
        light += color * round_light;
        light = air_adds + light * air_keeps;
        out_buffer.Store4(((g.first + strand) * k_strand_points + k) * k_drawn_bytes, uint4(asuint(p), pack_hair_light(light)));
    }
}

struct RibbonVaryings {
    float4 position : SV_Position;
    float across : ACROSS;   // pixels from the strand's middle line
    float width : WIDTH;     // the strand's width there, pixels
    float3 light : LIGHT;
    float view_z : VIEW_Z;
};

RibbonVaryings vs_ribbon(uint vertex_id : SV_VertexID) {
    const Groom g = load_groom();
    const Frame f = load_frame(push.frame, push.frame_offset);
    const uint pieces = (k_strand_points - 1u) / g.stride;
    const uint strand = vertex_id / (pieces * 6u);
    const uint within = vertex_id % (pieces * 6u);
    const uint piece = within / 6u;
    const uint corner_index = within % 6u;
    // Two triangles: (0, 1, 2), (2, 1, 3) of the corners (side, end): (-1, 0), (1, 0), (-1, 1), (1, 1).
    const uint c = corner_index < 3u ? corner_index : (corner_index == 3u ? 2u : (corner_index == 4u ? 1u : 3u));
    const float side = (c & 1u) != 0u ? 1.0 : -1.0;
    const uint k = (piece + ((c & 2u) != 0u ? 1u : 0u)) * g.stride;
    ByteAddressBuffer drawn = read_buffer(push.drawn);
    const uint base = (g.first + strand) * k_strand_points;
    const uint4 here = drawn.Load4((base + k) * k_drawn_bytes);
    const float3 p = asfloat(here.xyz);
    const uint k_ahead = min(k + g.stride, k_strand_points - 1u);
    const uint k_behind = k >= g.stride ? k - g.stride : 0u;
    const float3 ahead = asfloat(drawn.Load3((base + k_ahead) * k_drawn_bytes));
    const float3 behind = asfloat(drawn.Load3((base + k_behind) * k_drawn_bytes));
    const float3 t = safe_normalize(ahead - behind, f.view_rows[1].xyz);
    // The way the strand runs on screen there, in pixels (the projection's
    // own derivative along it), and across it: the ribbon is widened on
    // screen, so it is as many pixels wide anywhere in the view.
    const float3 pv = float3(dot(f.view_rows[0].xyz, p), dot(f.view_rows[1].xyz, p), dot(f.view_rows[2].xyz, p));
    const float3 tv = float3(dot(f.view_rows[0].xyz, t), dot(f.view_rows[1].xyz, t), dot(f.view_rows[2].xyz, t));
    const float view_z = max(-pv.z, f.near_plane);
    float2 runs = float2((tv.x * -pv.z + pv.x * tv.z) * f.p00 * f.width, -(tv.y * -pv.z + pv.y * tv.z) * f.p11 * f.height);
    // The piece's own run (this point to its other end), as seen from here:
    // the two ends of a piece must take the same side, or the piece is a
    // bow tie. Where the strand turns back on screen within a piece (seen
    // end on), the piece's run decides.
    const float3 chord = (c & 2u) != 0u ? p - behind : ahead - p;
    const float3 cv = float3(dot(f.view_rows[0].xyz, chord), dot(f.view_rows[1].xyz, chord), dot(f.view_rows[2].xyz, chord));
    const float2 piece_runs = float2((cv.x * -pv.z + pv.x * cv.z) * f.p00 * f.width, -(cv.y * -pv.z + pv.y * cv.z) * f.p11 * f.height);
    if (dot(runs, piece_runs) <= 0.0) {
        runs = piece_runs;
    }
    const float run = length(runs);
    const float2 sideways = run > 1e-20 ? float2(-runs.y, runs.x) / run : float2(1.0, 0.0); // (a piece of no length as seen: any way)
    const float pixel = view_z * 2.0 / max(f.p11 * f.height, 1e-6); // metres a pixel spans there
    const float share = float(k) / float(k_strand_points - 1u);
    const float half_width = g.half_width * lerp(1.0, g.tip_width, share);
    const float drawn_half = half_width / pixel + 0.5; // pixels
    RibbonVaryings o;
    const float4 clip = to_clip(f, p);
    const float2 offset = sideways * (side * drawn_half) * float2(2.0 / f.width, -2.0 / f.height);
    o.position = float4(clip.xy + offset * clip.w, clip.z, clip.w);
    o.across = side * drawn_half;
    o.width = 2.0 * half_width / pixel;
    o.light = unpack_hair_light(here.w);
    o.view_z = view_z;
    return o;
}

// The share of the pixel the strand covers.
float strand_cover(RibbonVaryings i) {
    return clamp(0.5 * i.width + 0.5 - abs(i.across), 0.0, min(1.0, i.width));
}

void ps_front(RibbonVaryings i) {
    if (strand_cover(i) <= 0.0) {
        discard;
    }
}

struct Sums {
    float4 accum : SV_Target0;  // light x cover x weight (exposed), cover x weight
    float4 reveal : SV_Target1; // cover (the blend keeps what shows through)
};

Sums ps_main(RibbonVaryings i) {
    const Frame f = load_frame(push.frame, push.frame_offset);
    const int3 at = int3(int2(i.position.xy), 0);
    const float d = read_texture_float(push.depth).Load(at);
    const float scene_z = d > 0.0 ? f.near_plane / d : 3.0e38;
    const float alpha = strand_cover(i);
    if (alpha <= 0.0 || i.view_z > scene_z + k_scene_bias + k_scene_bias_far * i.view_z) {
        discard;
    }
    const float fd = read_texture_float(push.front).Load(at);
    const float front_z = fd > 0.0 ? f.near_plane / fd : i.view_z;
    const float pixel = i.view_z * 2.0 / max(f.p11 * f.height, 1e-6);
    const float behind = max(i.view_z - front_z, 0.0);
    const float w = max(exp(-behind / max(k_layer, 2.0 * pixel)), 1e-4);
    Sums o;
    // (Exposed, so the sums stay within 16-bit floats.)
    const float3 light = min(i.light * f.exposure, float3(1000.0, 1000.0, 1000.0));
    o.accum = float4(light * alpha, alpha) * w;
    o.reveal = float4(alpha, alpha, alpha, alpha);
    return o;
}

float4 vs_full(uint vertex_id : SV_VertexID) : SV_Position {
    const float2 p = float2(vertex_id == 1 ? 3.0 : -1.0, vertex_id == 2 ? 3.0 : -1.0);
    return float4(p, 0.0, 1.0);
}

float4 ps_composite(float4 position : SV_Position) : SV_Target0 {
    const int3 at = int3(int2(position.xy), 0);
    const float4 under = read_texture(push.light).Load(at);
    const float shows = saturate(read_texture(push.reveal).Load(at).r);
    const float4 a = read_texture(push.accum).Load(at);
    if (shows > 0.9995 || !(a.a > 0.0)) {
        return under;
    }
    const Frame f = load_frame(push.frame, push.frame_offset);
    // (Kept finite whatever the sums came to.)
    const float3 exposed = clamp(a.rgb / a.a, float3(0.0, 0.0, 0.0), float3(1000.0, 1000.0, 1000.0));
    float3 light = exposed / max(f.exposure, 1e-9);
    if (under.a <= 0.0) {
        // Over what is shown as it is: exposed and tone mapped here.
        light = f.tone_mapper == 1u ? tone_map_agx(exposed) : tone_map(exposed);
    }
    return float4(lerp(under.rgb, light, 1.0 - shows), under.a);
}

float ps_depth(float4 position : SV_Position) : SV_Depth {
    const int3 at = int3(int2(position.xy), 0);
    const float shows = read_texture(push.reveal).Load(at).r;
    const float d = read_texture_float(push.front).Load(at);
    if (shows > 0.5 || !(d > 0.0)) {
        discard;
    }
    return d;
}
