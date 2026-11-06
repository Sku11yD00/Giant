// The sky's passes (sky_common.slang has the model): the sky view table,
// the clouds' pattern (made once), the cloud shadow map and the clouds
// themselves at half resolution, every frame and without history (each
// frame stands alone, AGENTS.md rule 2: the march's start is dithered by a
// fixed pattern, not one that changes per frame).
//
// Layouts: SkyPush and SkyParams in src/gpu_layout.h.
#include "sky_common.slang"
#include "voxel_march.slang"

struct SkyPush {
    uint params; // read descriptor of the SkyParams
    uint params_offset;
    uint frame;
    uint frame_offset;
    uint target; // write descriptor of what the pass fills
    uint target_b; // the clouds' second target
    uint pad0;
    uint pad1;
};
ConstantBuffer<SkyPush> push : register(b0);

// The sky's light per direction from the camera, this frame's sun.
[shader("compute")]
[numthreads(8, 8, 1)]
void cs_sky_view(uint3 id : SV_DispatchThreadID) {
    if (id.x >= k_sky_view_width || id.y >= k_sky_view_height) {
        return;
    }
    const Sky s = load_sky(push.params, push.params_offset);
    const float2 uv = (float2(id.xy) + 0.5) / float2(k_sky_view_width, k_sky_view_height);
    const float3 dir = sky_view_dir(uv, s.sun_direction);
    const float3 c = sky_radiance(dir, max(s.cam_alt, 1.0), s.sun_direction, s.sun_top, s.haze);
    write_texture(push.target)[id.xy] = float4(min(c, float3(60000.0, 60000.0, 60000.0)), 1.0);
}

// --- The clouds' pattern: tiling noise, made once -------------------------------

uint hash2(uint2 p, uint seed) {
    uint h = p.x * 0x8da6b343u ^ p.y * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    h *= 0x297a2d39u;
    h ^= h >> 15;
    return h;
}

float hash_unit(uint2 p, uint seed) { return float(hash2(p, seed) >> 8) / 16777216.0; }

// Value noise that repeats every `period` lattice cells across the unit square.
float tiling_noise(float2 uv, uint period, uint seed) {
    const float2 x = uv * float(period);
    const float2 i = floor(x);
    const float2 f = x - i;
    const float2 w = f * f * (3.0 - 2.0 * f);
    const uint2 c = uint2(int2(i)) % period;
    const uint2 n = (c + 1u) % period;
    const float a = hash_unit(c, seed);
    const float b = hash_unit(uint2(n.x, c.y), seed);
    const float d = hash_unit(uint2(c.x, n.y), seed);
    const float e = hash_unit(n, seed);
    return lerp(lerp(a, b, w.x), lerp(d, e, w.x), w.y);
}

float tiling_fbm(float2 uv, uint period, uint seed, uint octaves) {
    float sum = 0.0;
    float amp = 0.5;
    float total = 0.0;
    uint p = period;
    for (uint o = 0; o < octaves; ++o) {
        sum += tiling_noise(uv, p, seed + o * 17u) * amp;
        total += amp;
        amp *= 0.5;
        p *= 2u;
    }
    return sum / total;
}

// Cellular noise (1 at a cell's point, falling off): puffy, round shapes.
float tiling_cells(float2 uv, uint period, uint seed) {
    const float2 x = uv * float(period);
    const float2 i = floor(x);
    const float2 f = x - i;
    float nearest = 1.0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int2 cell = int2(i) + int2(dx, dy);
            const uint2 wrapped = uint2((cell + int(period) * 4) % int(period));
            const float2 spot = float2(hash_unit(wrapped, seed), hash_unit(wrapped, seed + 101u));
            const float2 d = float2(dx, dy) + spot - f;
            nearest = min(nearest, dot(d, d));
        }
    }
    return saturate(1.0 - sqrt(nearest));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_cloud_map(uint3 id : SV_DispatchThreadID) {
    if (id.x >= k_cloud_map_size || id.y >= k_cloud_map_size) {
        return;
    }
    const float2 uv = (float2(id.xy) + 0.5) / float(k_cloud_map_size);
    const float big = tiling_fbm(uv, 6u, 11u, 5u);
    const float puffs = tiling_cells(uv, 12u, 23u) * 0.6 + tiling_cells(uv, 24u, 29u) * 0.4;
    const float detail = tiling_fbm(uv, 32u, 41u, 3u) * 0.5 + tiling_cells(uv, 48u, 43u) * 0.5;
    const float tall = tiling_fbm(uv, 4u, 53u, 3u);
    // Stretch the contrast so cover maps onto the sky's share.
    const float r = saturate((big - 0.25) / 0.5);
    write_texture(push.target)[id.xy] = float4(r, puffs, detail, tall);
}

// --- Cloud shadows: the sunlight through the layer, from above the ground -------

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_cloud_shadow(uint3 id : SV_DispatchThreadID) {
    if (id.x >= k_cloud_shadow_size || id.y >= k_cloud_shadow_size) {
        return;
    }
    const Sky s = load_sky(push.params, push.params_offset);
    const float2 xz = s.shadow_origin + (float2(id.xy) + 0.5) / float(k_cloud_shadow_size) * s.shadow_size;
    const float3 sun = s.sun_direction;
    const float up = max(sun.y, 0.05);
    // From the base, up through the layer along the sun.
    const float3 start = float3(xz.x, s.cloud_base - s.cam_alt, xz.y);
    const float3 dir = normalize(float3(sun.x, up, sun.z));
    const float length_through = (s.cloud_top - s.cloud_base) / dir.y;
    const uint steps = 8;
    const float ds = length_through / float(steps);
    float depth = 0.0;
    for (uint i = 0; i < steps; ++i) {
        const float3 p = start + dir * ((float(i) + 0.5) * ds);
        depth += cloud_shape(s, p, false) * ds;
    }
    const float t = exp(-depth * s.cloud_density * k_cloud_extinction);
    // Light through many clouds is never quite black (it scatters through).
    write_texture(push.target)[id.xy] = float4(max(t, 0.06), 0.0, 0.0, 1.0);
}

// --- The clouds, at half resolution ------------------------------------------------

// A fixed dither (interleaved gradient noise), the same every frame.
float dither(uint2 p) { return frac(52.9829189 * frac(dot(float2(p), float2(0.06711056, 0.00583715)))); }

// Sunlight reaching a point in the clouds, a few steps towards the sun
// (longer each time): x straight through, y the light scattered many times
// on the way, which fades far more slowly (what keeps clouds white inside).
float2 sun_through_clouds(Sky s, float3 p) {
    const float thickness = max(s.cloud_top - s.cloud_base, 1.0);
    float ds = thickness * 0.05;
    float t = ds * 0.5;
    float depth = 0.0;
    for (uint i = 0; i < 5; ++i) {
        depth += cloud_shape(s, p + s.sun_direction * t, false) * ds;
        ds *= 1.8;
        t += ds;
    }
    const float tau = depth * s.cloud_density * k_cloud_extinction;
    return float2(exp(-tau), exp(-tau * 0.12));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_clouds(uint3 id : SV_DispatchThreadID) {
    const Sky s = load_sky(push.params, push.params_offset);
    if (float(id.x) >= s.half_size.x || float(id.y) >= s.half_size.y) {
        return;
    }
    const Frame f = load_frame(push.frame, push.frame_offset);
    // The full-resolution pixel this texel stands for: its ray and depth.
    const uint2 full = min(id.xy * 2u, uint2(s.full_size) - 1u);
    // The ray through the texel's centre (the corner its 2x2 pixels share,
    // where the upsample in clouds_at puts it).
    const float2 centre = min(float2(id.xy * 2u) + 1.0, s.full_size);
    const float2 ndc = float2((centre.x / f.width) * 2.0 - 1.0, 1.0 - (centre.y / f.height) * 2.0);
    const float3 ray = f.ray_x.xyz * ndc.x + f.ray_y.xyz * ndc.y + f.ray_z.xyz;
    const float ray_length = length(ray);
    const float3 dir = ray / ray_length;
    const float depth = read_texture_float(s.depth).Load(int3(int2(full), 0));
    // Reverse-Z with no far plane: view depth is near / depth.
    const float scene = depth > 0.0 ? f.near_plane / depth * ray_length : 3.0e38;
    const float scene_km = min(scene * 0.001, k_no_cloud_km);

    float3 light = float3(0.0, 0.0, 0.0);
    float transmittance = 1.0;
    float first = k_no_cloud_km;
    const float2 span = cloud_span(s, dir);
    const float enter = span.x;
    const float leave = min(min(span.y, scene), enter + 40000.0);
    if (leave > enter && enter < 60000.0 && s.cloud_cover > 0.0 && s.cloud_density > 0.0) {
        const uint steps = clamp(uint(s.cloud_steps), 8u, 256u);
        const float ds = (leave - enter) / float(steps);
        const float mu = dot(dir, s.sun_direction);
        // Forward scattering (the silver lining) with some back.
        const float phase = lerp(hg_phase(mu, 0.8), hg_phase(mu, -0.25), 0.3);
        const float sigma_scale = s.cloud_density * k_cloud_extinction;
        // Under a broad deck the light from all round is the deck's own (the
        // sun's, scattered through it many times): grey, not the blue sky's.
        const float3 lum = float3(0.2126, 0.7152, 0.0722);
        const float deck = dot(s.sky_light, lum) + dot(s.sun_light, lum) * 0.15 * saturate(s.sun_direction.y * 2.0);
        const float3 deck_light = float3(deck, deck, deck);
        float t = enter + ds * dither(id.xy);
        for (uint i = 0; i < steps && transmittance > 0.01; ++i) {
            const float3 p = dir * t;
            const float shape = cloud_shape(s, p, true);
            if (shape > 0.0) {
                if (first >= k_no_cloud_km) {
                    first = max(t - ds, 0.0) * 0.001;
                }
                const float sigma = shape * sigma_scale;
                const float h = saturate((height_at(s.cam_alt, p) - s.cloud_base) /
                                         max(s.cloud_top - s.cloud_base, 1.0));
                const float3 ambient = lerp(lerp(s.ground_light * 0.5 + s.sky_light * 0.3, s.sky_light, h),
                                            deck_light, s.cloud_cover);
                // The sky's light from all round, scattered evenly (a third reaches the eye).
                const float2 sun = sun_through_clouds(s, p);
                const float3 in_light = s.sun_light * (sun.x * phase + sun.y * 0.2) + ambient * 0.33;
                const float step_t = exp(-sigma * ds);
                // Energy-conserving: the light scattered in over the step, as it is attenuated.
                light += transmittance * in_light * (1.0 - step_t);
                transmittance *= step_t;
            }
            t += ds;
        }
        // Far clouds fade into the sky behind them.
        const float fade = saturate((enter - 25000.0) / 30000.0);
        light *= 1.0 - fade;
        transmittance = lerp(transmittance, 1.0, fade);
        if (transmittance > 0.999) {
            first = k_no_cloud_km;
        }
    }
    write_texture(push.target)[id.xy] = float4(min(light, float3(60000.0, 60000.0, 60000.0)), transmittance);
    // .z: where the march ended (km), so a pixel nearer than that (the far
    // field's voxels, not in depth) takes only the clouds in front of it.
    const float end_km = leave > enter ? min(leave * 0.001, k_no_cloud_km) : k_no_cloud_km;
    write_texture(push.target_b)[id.xy] = float4(first, scene_km, end_km, 0.0);
}

// --- Cover from above (rain and snow) ------------------------------------------

// Metres a voxel spans on the finest level whose window holds camera-relative
// `p` (the level a march finds things on there; the coarsest past them all).
float voxel_size_at(ByteAddressBuffer levels, VoxelWorld w, float3 p) {
    const float n = float(w.bricks_per_axis);
    float size = 1.0;
    for (uint i = 0; i < w.level_count; ++i) {
        const Level c = load_level(levels, w.levels_offset, i);
        size = c.brick_size / 8.0; // 8 voxels a brick side
        const float3 local = (p - c.origin) / (n * c.brick_size);
        if (all(local >= 0.0) && all(local < 1.0)) {
            break;
        }
    }
    return size;
}

// What is on top over each metre round the camera: the camera-relative
// height where a ray straight down first meets the voxel world (-1e9:
// nothing), less a voxel of the level it met there (the surface is
// somewhere in that voxel: so what is on top is never judged under
// itself). Rain and snow reach only what is at that height; what is under
// a roof, a bridge or a tree stays dry. Marched in two parts: from the top
// of the coarsest window (at most 800 m up) down to the top of the finest
// window holding the column, then down that window (a march only goes to
// coarser levels, so the second part finds the ground near the camera on
// its finest voxels).
[shader("compute")]
[numthreads(8, 8, 1)]
void cs_weather_cover(uint3 id : SV_DispatchThreadID) {
    if (id.x >= k_cover_size || id.y >= k_cover_size) {
        return;
    }
    const Sky s = load_sky(push.params, push.params_offset);
    VoxelWorld w;
    w.pool = s.voxel_pool;
    w.table = s.voxel_table;
    w.levels = s.voxel_levels;
    w.levels_offset = s.voxel_levels_offset;
    w.level_count = min(s.voxel_level_count, k_max_levels);
    w.bricks_per_axis = s.voxel_bricks_per_axis;
    w.stand_ins = 0u; // everything shelters
    float height = -1.0e9;
    if (w.level_count > 0u) {
        const float texel = s.cover_size / float(k_cover_size);
        const float2 xz = s.cover_origin + (float2(id.xy) + 0.5) * texel;
        ByteAddressBuffer levels = read_buffer(w.levels);
        const float n = float(w.bricks_per_axis);
        const Level coarsest = load_level(levels, w.levels_offset, w.level_count - 1u);
        const float top = min(coarsest.origin.y + n * coarsest.brick_size - 0.001, 800.0);
        // The finest window holding this column.
        uint fine = w.level_count;
        float fine_top = top;
        for (uint i = 0; i < w.level_count; ++i) {
            const Level c = load_level(levels, w.levels_offset, i);
            const float size = n * c.brick_size;
            if (xz.x >= c.origin.x && xz.x < c.origin.x + size && xz.y >= c.origin.z && xz.y < c.origin.z + size) {
                fine = i;
                fine_top = min(c.origin.y + size - 0.001, top);
                break;
            }
        }
        const float3 down = float3(0.0, -1.0, 0.0);
        const float4 no_skip = float4(0.0, 0.0, 0.0, -1.0);
        const float4 no_ground = float4(0.0, 0.0, 0.0, 0.0);
        float hit_t;
        uint packed;
        uint hit_at;
        float3 hit = float3(0.0, 0.0, 0.0);
        bool found = false;
        if (fine < w.level_count) {
            if (fine_top < top && march_voxels_from(w, float3(xz.x, top, xz.y), down, 0u, 0.0, top - fine_top, no_skip,
                                                    no_ground, hit_t, packed, hit_at)) {
                hit = float3(xz.x, top - hit_t, xz.y);
                found = true;
            } else if (march_voxels_from(w, float3(xz.x, fine_top, xz.y), down, fine, 0.0, 4000.0, no_skip, no_ground,
                                         hit_t, packed, hit_at)) {
                hit = float3(xz.x, fine_top - hit_t, xz.y);
                found = true;
            }
        }
        if (found) {
            // Half a voxel into what it met: the level that holds it.
            const float v = voxel_size_at(levels, w, hit - float3(0.0, 0.01, 0.0));
            height = hit.y - v;
        }
    }
    write_texture_float(push.target)[id.xy] = height;
}
