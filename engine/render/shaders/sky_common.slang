// The sky, the clouds and the fog (sky.slang draws them; resolve.slang
// puts them in the image): the parameters (gpu::SkyParams, in the frame's
// transient memory), an atmosphere the sun shines through (Rayleigh air and
// Mie haze, single scattering), a layer of clouds over a round earth, their
// shadows on the ground and height fog.
//
// Positions are camera-relative, as everywhere else; the camera stands
// `cam_alt` metres above y = 0, and the clouds' pattern is looked up from
// the camera's place in it (`cam_xz`, kept small on the CPU).
#pragma once
#include "common.slang"

static const float k_pi_sky = 3.14159265358979;
static const float k_earth_radius = 6360000.0;
static const float k_atmosphere_top = 100000.0; // metres above the ground
static const float3 k_rayleigh = float3(5.802e-6, 13.558e-6, 33.1e-6); // scattering per metre at sea level
static const float k_rayleigh_height = 8000.0;
static const float k_mie = 4.4e-6;            // scattering per metre at sea level, haze 1
static const float k_mie_extinction = 1.11;   // extinction / scattering
static const float k_mie_height = 1200.0;
static const float k_mie_g = 0.76;
// How much light a fully dense cloud takes out, per metre.
static const float k_cloud_extinction = 0.05;
// The sky view table and the cloud shadow map (sizes the CPU makes them).
static const uint k_sky_view_width = 192;
static const uint k_sky_view_height = 108;
static const uint k_cloud_shadow_size = 256;
static const uint k_cloud_map_size = 512;
// Where a pixel has no cloud in front of it (km, as the cloud target stores it).
static const float k_no_cloud_km = 60000.0;

// gpu::SkyParams.
struct Sky {
    float3 sun_direction; // unit, towards the sun
    float cloud_cover;
    float3 sun_light;     // at the ground, through the air
    float cloud_density;
    float3 sky_light;
    float cloud_base;     // metres above y = 0
    float3 ground_light;
    float cloud_top;
    float2 cam_xz;        // the camera's place in the cloud pattern (metres, within one tile)
    float cam_alt;        // the camera's height above y = 0
    float cloud_tile;     // metres a tile of the pattern repeats
    float fog_density;    // per metre at y = 0
    float fog_falloff;    // per metre up
    float haze;
    float cloud_steps;
    float2 shadow_origin; // the cloud shadow map's minimum corner, camera-relative x and z
    float shadow_size;    // metres it covers
    uint flags;           // bit 0: clouds; bit 1: cloud shadows
    float2 half_size;     // the cloud target
    float2 full_size;     // the image
    uint sky_view;        // read descriptors
    uint cloud_map;
    uint cloud_shadow;
    uint clouds_a;        // radiance (rgb) and transmittance (a) of the clouds before each pixel
    uint clouds_b;        // x: km to the first cloud (k_no_cloud_km: none); y: km to what the pixel shows z: km where the march ended
    uint depth;
    uint pad0;
    uint pad1;
    float3 sun_top;       // the sun above the air (colour times intensity)
    float pad2;
    // What falls and the ground under it.
    float rain;           // 0 to 1
    float snow;           // 0 to 1
    float wetness;        // the ground, 0 to 1
    float snow_cover;     // 0 to 1
    float2 wind;          // x, z metres a second
    float time;           // seconds
    uint cover;           // read descriptor: camera-relative height of what is on top (r32)
    float2 cover_origin;  // camera-relative x, z of the cover map's corner
    float cover_size;     // metres
    uint weather_flags;   // bit 0: falling; bit 1: wet and snowy surfaces; bit 2: the cover map
    float2 place;         // the camera's x, z wrapped at k_weather_wrap metres
    float pixel_angle;    // radians a pixel spans
    uint pad3;
    uint voxel_pool;      // the voxel world (VoxelWorld), for the cover map
    uint voxel_table;
    uint voxel_levels;
    uint voxel_levels_offset;
    uint voxel_level_count;
    uint voxel_bricks_per_axis;
    uint pad4;
    uint pad5;
};
static const uint k_sky_params_bytes = 272;
static const uint k_cover_size = 256;       // texels a side (gpu: renderer.cpp k_cover_size)
static const float k_weather_wrap = 4096.0; // metres: ground patterns repeat this far

Sky load_sky(uint descriptor, uint offset) {
    ByteAddressBuffer b = read_buffer(descriptor);
    const float4 r0 = load4(b, offset);
    const float4 r1 = load4(b, offset + 16);
    const float4 r2 = load4(b, offset + 32);
    const float4 r3 = load4(b, offset + 48);
    const float4 r4 = load4(b, offset + 64);
    const float4 r5 = load4(b, offset + 80);
    const float4 r6 = load4(b, offset + 96);
    const float4 r7 = load4(b, offset + 112);
    const uint4 r8 = b.Load4(offset + 128);
    const uint4 r9 = b.Load4(offset + 144);
    const float4 r10 = load4(b, offset + 160);
    Sky s;
    s.sun_direction = r0.xyz;
    s.cloud_cover = r0.w;
    s.sun_light = r1.xyz;
    s.cloud_density = r1.w;
    s.sky_light = r2.xyz;
    s.cloud_base = r2.w;
    s.ground_light = r3.xyz;
    s.cloud_top = r3.w;
    s.cam_xz = float2(r4.x, r4.z);
    s.cam_alt = r4.y;
    s.cloud_tile = r4.w;
    s.fog_density = r5.x;
    s.fog_falloff = r5.y;
    s.haze = r5.z;
    s.cloud_steps = r5.w;
    s.shadow_origin = r6.xy;
    s.shadow_size = r6.z;
    s.flags = asuint(r6.w);
    s.half_size = r7.xy;
    s.full_size = r7.zw;
    s.sky_view = r8.x;
    s.cloud_map = r8.y;
    s.cloud_shadow = r8.z;
    s.clouds_a = r8.w;
    s.clouds_b = r9.x;
    s.depth = r9.y;
    s.pad0 = r9.z;
    s.pad1 = r9.w;
    s.sun_top = r10.xyz;
    s.pad2 = r10.w;
    const float4 r11 = load4(b, offset + 176);
    const float4 r12 = load4(b, offset + 192);
    const float4 r13 = load4(b, offset + 208);
    const float4 r14 = load4(b, offset + 224);
    const uint4 r15 = b.Load4(offset + 240);
    const uint4 r16 = b.Load4(offset + 256);
    s.rain = r11.x;
    s.snow = r11.y;
    s.wetness = r11.z;
    s.snow_cover = r11.w;
    s.wind = r12.xy;
    s.time = r12.z;
    s.cover = asuint(r12.w);
    s.cover_origin = r13.xy;
    s.cover_size = r13.z;
    s.weather_flags = asuint(r13.w);
    s.place = r14.xy;
    s.pixel_angle = r14.z;
    s.pad3 = asuint(r14.w);
    s.voxel_pool = r15.x;
    s.voxel_table = r15.y;
    s.voxel_levels = r15.z;
    s.voxel_levels_offset = r15.w;
    s.voxel_level_count = r16.x;
    s.voxel_bricks_per_axis = r16.y;
    s.pad4 = r16.z;
    s.pad5 = r16.w;
    return s;
}

// --- Geometry over a round earth ----------------------------------------------

// Where a ray from the camera (`alt` metres up, direction's y `dir_y`)
// meets the sphere `height` metres up: (near, far) distances; far < 0:
// it does not. Written so the earth's radius does not swamp float
// precision (the product of the roots is exact, and the larger root comes
// from no cancellation).
float2 sphere_hits(float alt, float height, float dir_y) {
    const float c = (alt - height) * (2.0 * k_earth_radius + alt + height);
    const float b = (k_earth_radius + alt) * dir_y;
    const float disc = b * b - c;
    if (disc < 0.0) {
        return float2(-1.0, -1.0);
    }
    const float sq = sqrt(disc);
    const float q = -(b + (b >= 0.0 ? sq : -sq));
    const float other = q != 0.0 ? c / q : 0.0;
    return float2(min(q, other), max(q, other));
}

// The height above y = 0 of a camera-relative point (the earth curving away).
float height_at(float alt, float3 p) {
    const float3 centre_to = p + float3(0.0, k_earth_radius + alt, 0.0);
    return length(centre_to) - k_earth_radius;
}

// --- Atmosphere ---------------------------------------------------------------

float rayleigh_phase(float mu) { return 3.0 / (16.0 * k_pi_sky) * (1.0 + mu * mu); }

float hg_phase(float mu, float g) {
    const float g2 = g * g;
    return (1.0 - g2) / (4.0 * k_pi_sky * pow(max(1.0 + g2 - 2.0 * g * mu, 1e-4), 1.5));
}

// Extinction per metre at a height (air and haze).
float3 air_extinction(float h, float haze) {
    const float rho_r = exp(-max(h, 0.0) / k_rayleigh_height);
    const float rho_m = exp(-max(h, 0.0) / k_mie_height);
    return k_rayleigh * rho_r + k_mie * k_mie_extinction * haze * rho_m;
}

// The fraction of sunlight reaching a height `h` along `sun` (a few steps
// to the top of the air; none below the horizon there).
float3 sun_transmittance(float h, float3 sun, float haze) {
    const float2 ground = sphere_hits(h, 0.0, sun.y);
    if (ground.x > 0.0) {
        return float3(0.0, 0.0, 0.0);
    }
    const float2 top = sphere_hits(h, k_atmosphere_top, sun.y);
    const float length_up = max(top.y, 0.0);
    float3 depth = float3(0.0, 0.0, 0.0);
    const uint steps = 6;
    const float ds = length_up / float(steps);
    for (uint i = 0; i < steps; ++i) {
        const float t = (float(i) + 0.5) * ds;
        depth += air_extinction(height_at(h, sun * t), haze) * ds;
    }
    return exp(-depth);
}

// The sky's light along `dir` from a camera `alt` metres up: sunlight
// scattered once by the air and the haze on the way.
float3 sky_radiance(float3 dir, float alt, float3 sun, float3 sun_top, float haze) {
    const float2 top = sphere_hits(alt, k_atmosphere_top, dir.y);
    const float2 ground = sphere_hits(alt, 0.0, dir.y);
    float far_end = max(top.y, 0.0);
    if (ground.x > 0.0) {
        far_end = min(far_end, ground.x);
    }
    const float mu = dot(dir, sun);
    const float pr = rayleigh_phase(mu);
    const float pm = hg_phase(mu, k_mie_g);
    float3 sum = float3(0.0, 0.0, 0.0);
    float3 view_depth = float3(0.0, 0.0, 0.0);
    const uint steps = 24;
    float prev = 0.0;
    for (uint i = 0; i < steps; ++i) {
        // Steps bunch up near the camera, where the air is densest.
        const float f = (float(i) + 0.5) / float(steps);
        const float t = far_end * f * f;
        const float ds = t - prev;
        prev = t;
        const float h = height_at(alt, dir * t);
        const float rho_r = exp(-max(h, 0.0) / k_rayleigh_height);
        const float rho_m = exp(-max(h, 0.0) / k_mie_height);
        const float3 ext = k_rayleigh * rho_r + k_mie * k_mie_extinction * haze * rho_m;
        view_depth += ext * ds;
        const float3 scatter = k_rayleigh * rho_r * pr + k_mie * haze * rho_m * pm;
        sum += exp(-view_depth) * sun_transmittance(h, sun, haze) * scatter * ds;
    }
    return sum * sun_top;
}

// The sky view table: azimuth from the sun (0 to pi) across, elevation
// down (more rows near the horizon).
float2 sky_view_uv(float3 dir, float3 sun) {
    const float elevation = asin(clamp(dir.y, -1.0, 1.0));
    const float v = 0.5 - 0.5 * sign(elevation) * sqrt(abs(elevation) / (0.5 * k_pi_sky));
    const float a = atan2(dir.z, dir.x) - atan2(sun.z, sun.x);
    const float u = abs(atan2(sin(a), cos(a))) / k_pi_sky;
    const float2 size = float2(k_sky_view_width, k_sky_view_height);
    return (float2(u, v) * (size - 1.0) + 0.5) / size;
}

float3 sky_view_dir(float2 uv, float3 sun) {
    const float2 size = float2(k_sky_view_width, k_sky_view_height);
    const float2 f = saturate((uv * size - 0.5) / (size - 1.0));
    const float s = 0.5 - f.y; // 0.5 up, -0.5 down
    const float elevation = sign(s) * (2.0 * s) * (2.0 * s) * 0.5 * k_pi_sky;
    const float azimuth = atan2(sun.z, sun.x) + f.x * k_pi_sky;
    const float c = cos(elevation);
    return float3(c * cos(azimuth), sin(elevation), c * sin(azimuth));
}

float3 sample_sky(Sky s, float3 dir) {
    const float2 uv = sky_view_uv(dir, s.sun_direction);
    return read_texture(s.sky_view).SampleLevel(get_sampler(k_sampler_linear_clamp), uv, 0.0).rgb;
}

// What the sky shows along `dir`: the air, and the sun's disc.
float3 sky_background(Sky s, float3 dir) {
    float3 c = sample_sky(s, dir);
    const float mu = dot(dir, s.sun_direction);
    const float disc = smoothstep(0.99996, 0.99999, mu); // about half a degree across
    c += s.sun_light * (disc * 2000.0);
    return c;
}

// --- Clouds ---------------------------------------------------------------------

// The clouds' 2D pattern at a camera-relative point: r and g shape the
// coverage, b is detail, a varies how tall they grow.
float4 cloud_pattern(Sky s, float2 xz, float scale) {
    const float2 uv = (s.cam_xz + xz) / s.cloud_tile * scale;
    return read_texture(s.cloud_map).SampleLevel(get_sampler(k_sampler_linear_wrap), uv, 0.0);
}

// How dense the clouds are (0 to 1) at a camera-relative point; `detail`
// erodes their edges (the view's march), off for the light's.
float cloud_shape(Sky s, float3 p, bool detail) {
    const float h = (height_at(s.cam_alt, p) - s.cloud_base) / max(s.cloud_top - s.cloud_base, 1.0);
    if (h <= 0.0 || h >= 1.0) {
        return 0.0;
    }
    const float4 m = cloud_pattern(s, p.xz, 1.0);
    const float base = m.r * 0.65 + m.g * 0.35;
    const float cover = saturate(s.cloud_cover);
    float coverage = saturate((base - (1.0 - cover)) / max(cover, 0.05));
    if (cover >= 0.999) {
        coverage = max(coverage, 0.6); // overcast: no gaps
    }
    // Flat bases, rounded tops that reach higher where the pattern says.
    const float tall = lerp(0.45, 1.0, m.a);
    const float profile = smoothstep(0.0, 0.08, h) * (1.0 - smoothstep(tall * 0.55, tall, h));
    float shape = coverage * profile;
    if (detail && shape > 0.0) {
        // Two slices of the detail pattern slid by height stand in for 3D noise.
        const float4 d0 = cloud_pattern(s, p.xz + float2(h * 900.0, h * 400.0), 7.0);
        const float4 d1 = cloud_pattern(s, p.xz - float2(h * 500.0, h * 1100.0), 13.0);
        const float erode = d0.b * 0.6 + d1.b * 0.4;
        shape = saturate((shape - erode * 0.35 * (1.0 - shape)) / 0.75);
    }
    return shape;
}

// The cloud layer's span along a ray from the camera: (enter, leave), or
// leave <= enter where it misses.
float2 cloud_span(Sky s, float3 dir) {
    const float alt = s.cam_alt;
    const float2 low = sphere_hits(alt, s.cloud_base, dir.y);
    const float2 high = sphere_hits(alt, s.cloud_top, dir.y);
    float enter = 0.0;
    float leave = 0.0;
    if (alt < s.cloud_base) {
        enter = low.y;
        leave = high.y;
    } else if (alt <= s.cloud_top) {
        enter = 0.0;
        leave = low.x > 0.0 ? low.x : high.y;
    } else {
        if (high.y <= 0.0) {
            return float2(0.0, 0.0);
        }
        enter = max(high.x, 0.0);
        leave = low.x > 0.0 ? low.x : high.y;
    }
    return float2(max(enter, 0.0), max(leave, 0.0));
}

// --- Cloud shadows and fog ----------------------------------------------------

// The fraction of sunlight the clouds let through to a camera-relative
// point (from the cloud shadow map: the point carried along the sun to
// the clouds' base).
float cloud_shadow(Sky s, float3 p) {
    if ((s.flags & 2u) == 0u) {
        return 1.0;
    }
    const float3 sun = s.sun_direction;
    const float up = max(sun.y, 0.05);
    const float rise = s.cloud_base - (s.cam_alt + p.y);
    const float2 q = p.xz + sun.xz / up * max(rise, 0.0);
    const float2 uv = (q - s.shadow_origin) / s.shadow_size;
    // Past the map (far off, or a low sun's long shadows), the clouds' mean
    // shade, faded into over the map's outer tenth: no hard edge.
    const float mean = 1.0 - 0.65 * s.cloud_cover * s.cloud_density;
    const float2 edge = min(uv, 1.0 - uv);
    const float inside = saturate(min(edge.x, edge.y) * 10.0);
    if (inside <= 0.0) {
        return mean;
    }
    const float map = read_texture(s.cloud_shadow).SampleLevel(get_sampler(k_sampler_linear_clamp), uv, 0.0).r;
    return lerp(mean, map, inside);
}

// Height fog along a ray from the camera to `distance`: what is left of
// what lies there (transmittance), from fog thinning exponentially upwards.
float fog_transmittance(Sky s, float3 dir, float distance) {
    if (s.fog_density <= 0.0) {
        return 1.0;
    }
    // Optical depth of density * exp(-b h) along the ray, from h0 to h1,
    // in one difference of exponents each clamped (a high camera looking
    // down must neither underflow the camera's term nor overflow the far one).
    const float b = s.fog_falloff;
    const float h0 = max(s.cam_alt, -1000.0);
    const float h1 = max(h0 + dir.y * distance, -1000.0);
    if (min(-b * h0, -b * h1) > 80.0) {
        return 0.0; // the whole ray deep in fog thicker than floats hold: opaque
    }
    const float e0 = exp(clamp(-b * h0, -80.0, 80.0));
    const float k = b * (h1 - h0);
    float depth;
    if (abs(k) > 1e-4) {
        const float e1 = exp(clamp(-b * h1, -80.0, 80.0));
        depth = s.fog_density * distance * (e0 - e1) / k;
    } else {
        depth = s.fog_density * distance * e0 * (1.0 - 0.5 * k);
    }
    return isfinite(depth) ? exp(-max(depth, 0.0)) : 0.0;
}

// The colour fog fades towards, looking along `dir`: the horizon's.
float3 fog_color(Sky s, float3 dir) {
    const float3 level = normalize(float3(dir.x, max(dir.y, 0.03), dir.z));
    return sample_sky(s, level) * 0.6 + s.sky_light * 0.4 +
           s.sun_light * (hg_phase(dot(dir, s.sun_direction), 0.6) * 0.15);
}

// Fog over what a pixel shows `distance` away: it fades into the horizon's colour.
float3 apply_fog(Sky s, float3 color, float3 dir, float distance) {
    const float t = fog_transmittance(s, dir, distance);
    if (t >= 0.9999) {
        return color;
    }
    return color * t + fog_color(s, dir) * (1.0 - t);
}

// The clouds in front of a full-resolution pixel `distance` metres away,
// from the half-resolution cloud targets: the texel of the four nearest
// whose own distance matches best (so the clouds keep to silhouettes).
// Returns their light (rgb) and transmittance (a), and in `first` the
// distance to the first of them (metres).
float4 clouds_at(Sky s, uint2 pixel, float distance, out float first) {
    first = 3.0e38;
    if ((s.flags & 1u) == 0u) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    const int2 hsize = int2(s.half_size);
    const float2 at = (float2(pixel) + 0.5) * 0.5 - 0.5;
    const int2 base = int2(floor(at));
    Texture2D<float4> ta = read_texture(s.clouds_a);
    Texture2D<float4> tb = read_texture(s.clouds_b);
    const float want = log2(max(distance, 1.0) * 0.001 + 1.0);
    float best = 3.0e38;
    int2 pick = clamp(base, int2(0, 0), hsize - 1);
    [unroll] for (int k = 0; k < 4; ++k) {
        const int2 q = clamp(base + int2(k & 1, k >> 1), int2(0, 0), hsize - 1);
        const float d = tb.Load(int3(q, 0)).y;
        const float score = abs(log2(d + 1.0) - want);
        if (score < best) {
            best = score;
            pick = q;
        }
    }
    const float4 b = tb.Load(int3(pick, 0));
    first = b.x * 1000.0;
    if (distance < first) {
        return float4(0.0, 0.0, 0.0, 1.0); // the pixel is in front of the clouds
    }
    float4 a = ta.Load(int3(pick, 0));
    // Nearer than where the march ended (the far field's voxels are not in
    // depth): only the share of the clouds in front of it, by distance.
    const float end = b.z * 1000.0;
    if (distance < end) {
        const float share = saturate((distance - first) / max(end - first, 1.0));
        a = float4(a.rgb * share, lerp(1.0, a.a, share));
    }
    return a;
}

// --- Rain and snow ---------------------------------------------------------------

uint weather_hash(uint3 v) {
    v = v * uint3(1664525u, 1013904223u, 2654435761u) + uint3(1013904223u, 1664525u, 374761393u);
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    return v.x ^ (v.x >> 13u);
}

float weather_unit(uint3 v) { return float(weather_hash(v) & 0xFFFFFFu) / 16777216.0; }

// Value noise over the ground, 0 to 1, in cells of `cell` metres, the same
// again every k_weather_wrap metres (so the camera's wrapped place moves
// it on without a jump). `seed` picks the pattern.
float weather_noise(float2 at, float cell, uint seed) {
    const float2 v = at / cell;
    const float2 i = floor(v);
    const float2 u = v - i;
    const float2 w = u * u * (3.0 - 2.0 * u);
    const int period = int(k_weather_wrap / cell);
    const int2 a = int2(i);
    const uint2 c0 = uint2(((a % period) + period) % period);
    const uint2 c1 = uint2((((a + 1) % period) + period) % period);
    const float n00 = weather_unit(uint3(c0.x, c0.y, seed));
    const float n10 = weather_unit(uint3(c1.x, c0.y, seed));
    const float n01 = weather_unit(uint3(c0.x, c1.y, seed));
    const float n11 = weather_unit(uint3(c1.x, c1.y, seed));
    return lerp(lerp(n00, n10, w.x), lerp(n01, n11, w.x), w.y);
}

// How open to the sky a camera-relative point on a surface with normal `n`
// is (1: rain and snow reach it; 0: something above shelters it), from the
// cover map. A wall is judged from the open air just in front of it.
// Without the map (or past it): open.
float open_sky(Sky s, float3 p, float3 n) {
    if ((s.weather_flags & 4u) == 0u) {
        return 1.0;
    }
    const float2 q = p.xz + n.xz * 0.75;
    const float2 uv = (q - s.cover_origin) / s.cover_size;
    if (any(uv < 0.0) || any(uv >= 1.0)) {
        return 1.0;
    }
    // The nearest texel: heights do not blend across a roof's edge.
    const int2 t = clamp(int2(uv * float(k_cover_size)), int2(0, 0), int2(int(k_cover_size) - 1, int(k_cover_size) - 1));
    const float h = read_texture_float(s.cover).Load(int3(t, 0));
    // At (or above) what is on top (the map is a voxel low already): open.
    return saturate((p.y + max(n.y, 0.0) * 0.25 - h + 0.25) / 0.5);
}

// The weather on a surface at camera-relative `p`: wet (darker where it is
// porous, glossier, puddles in the hollows of flat ground) and snow on
// what faces up, where the sky is open above it.
void weather_surface(Sky s, float3 p, inout float3 base, inout float rough, inout float metal, inout float3 n) {
    if ((s.weather_flags & 2u) == 0u) {
        return;
    }
    const float open = open_sky(s, p, n);
    if (open <= 0.0) {
        return;
    }
    const float2 at = s.place + p.xz;
    const float up = n.y;
    // Wet: water fills the pores (rough, non-metal surfaces darken most)
    // and films the surface (glossier).
    const float wet = s.wetness * open;
    const float porous = saturate(rough * 1.3) * (1.0 - saturate(metal));
    base *= lerp(1.0, 0.45, wet * porous);
    rough = lerp(rough, min(rough, 0.2), wet);
    // Puddles: in the hollows of a noise on flat ground, spreading as it gets wetter.
    const float hollows = weather_noise(at, 8.0, 7u) * 0.7 + weather_noise(at, 2.0, 11u) * 0.3;
    const float flat = smoothstep(0.9, 0.98, up);
    const float puddle = flat * smoothstep(0.62, 0.56, hollows - (s.wetness - 0.6) * 0.3) * saturate(s.wetness * 2.5 - 1.2) * open;
    base = lerp(base, base * 0.6, puddle);
    rough = lerp(rough, 0.03, puddle);
    metal = lerp(metal, 0.0, puddle);
    n = normalize(lerp(n, float3(0.0, 1.0, 0.0), puddle));
    // Snow: on what faces up, thin first in lumps, then all over.
    const float lumps = weather_noise(at, 1.5, 23u);
    const float facing = smoothstep(0.3, 0.7, up + (lumps - 0.5) * 0.2);
    const float snow = saturate(s.snow_cover * 1.4 - lumps * 0.4) * facing * open;
    base = lerp(base, float3(0.86, 0.88, 0.92), snow);
    rough = lerp(rough, 0.7, snow);
    metal = lerp(metal, 0.0, snow);
    n = normalize(lerp(n, float3(0.0, 1.0, 0.0), snow * 0.5));
}

// Rain or snow falling in front of what a pixel shows `distance` metres
// away along `dir`: drops on cylinders round the camera (1.5 to 24 m out),
// each a cell's worth of falling streaks or flakes from a hash, moving
// with the time and the wind. None where it is sheltered (the cover map).
float3 apply_falling(Sky s, float3 color, float3 dir, float distance) {
    if ((s.weather_flags & 1u) == 0u) {
        return color;
    }
    const float amount = s.rain + s.snow;
    if (amount <= 0.0) {
        return color;
    }
    const float snowy = s.snow / amount;
    const float flat = length(dir.xz);
    const float2 dh = flat > 1e-4 ? dir.xz / flat : float2(0.0, -1.0);
    const float angle = atan2(dh.x, -dh.y);
    const float2 tangent = float2(-dh.y, dh.x);
    const float speed = lerp(8.0, 1.2, snowy);         // metres a second down
    const float drift = dot(s.wind, tangent) * 0.8;     // along the cylinder
    const float3 lit = s.sky_light * 0.7 + s.sun_light * (0.08 + 0.25 * snowy) * saturate(s.sun_direction.y * 2.0 + 0.2);
    float3 c = color;
    // Far to near, so nearer drops are over farther ones.
    [unroll] for (int k = 4; k >= 0; --k) {
        const float radius = 1.5 * exp2(float(k));
        const float t = radius / max(flat, 0.3);
        if (t >= distance || t > 80.0) {
            continue;
        }
        const float3 p = dir * t;
        if (open_sky(s, p, float3(0.0, 1.0, 0.0)) <= 0.5) {
            continue;
        }
        const float px = t * s.pixel_angle;               // metres a pixel spans there
        const float cell = max(px * 18.0, 0.05);           // drops about 18 pixels apart
        // The falling frame: where this point was when the drops started.
        const float2 q = float2(angle * radius - drift * s.time, s.cam_alt + p.y + speed * s.time);
        const float2 cell_size = float2(cell, cell * lerp(4.0, 1.0, snowy));
        const float2 g = q / cell_size;
        const float2 gi = floor(g);
        const uint3 key = uint3(uint(int(gi.x) & 0xFFFF) | (uint(k) << 16), uint(int(gi.y) & 0xFFFF), 0x5EEDu);
        if (weather_unit(key) > amount * 0.9) {
            continue; // no drop in this cell
        }
        float2 centre = float2(weather_unit(key + uint3(0u, 0u, 1u)), weather_unit(key + uint3(0u, 0u, 2u)));
        if (snowy > 0.5) {
            centre.x += sin(s.time * 1.3 + weather_unit(key + uint3(0u, 0u, 3u)) * 6.28) * 0.15;
        }
        const float2 d = (g - gi - centre) * cell_size; // metres from the drop's centre
        float alpha;
        if (snowy > 0.5) {
            const float r = max(px * 0.9, 0.006);
            alpha = 0.85 * saturate(1.0 - length(d) / r);
        } else {
            // A streak along its motion (down, with the drift), about 0.3 m long.
            const float2 v = normalize(float2(drift, -speed));
            const float along = dot(d, v);
            const float across = abs(d.x * v.y - d.y * v.x);
            alpha = 0.3 * saturate(1.0 - across / max(px * 0.7, 0.0015)) * saturate(1.0 - abs(along) / 0.15);
        }
        c = lerp(c, lit, alpha);
    }
    return c;
}
