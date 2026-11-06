// Global illumination from the lit voxel world (src/voxel_light.h).
//
// Every filled voxel has a light word in the light pool (one u32 per voxel,
// brick after brick, beside the brick pool): the bounce and sky light
// reaching it, and whether the sun does. voxel_light.slang fills them a
// budgeted number of bricks a frame, by tracing cones from each voxel
// through the clipmap (its coarser levels serve as the cones' wider
// samples); what the cones meet is lit from those same words, so light
// bounces once more each time a brick is lit again.
//
// Shading then looks the light up at each pixel (voxel_bounce): no rays,
// no noise and nothing carried over from earlier frames, so the picture is
// as sharp moving as standing still.
//
// Light words: 9-bit R, G, B mantissas over a shared 4-bit exponent (1-15:
// m / 512 * 2^(e - 9); 0: not lit yet), then the sun bit (voxel::pack_light).
#pragma once
#include "voxel_march.slang"

static const uint k_brick_voxels = 512;

bool light_lit(uint w) { return ((w >> 27) & 15u) != 0u; }
bool light_sun(uint w) { return (w >> 31) != 0u; }

float3 unpack_light(uint w) {
    const uint e = (w >> 27) & 15u;
    if (e == 0u) {
        return float3(0.0, 0.0, 0.0);
    }
    const float scale = exp2(float(e) - 9.0) / 512.0;
    return float3(float(w & 511u), float((w >> 9) & 511u), float((w >> 18) & 511u)) * scale;
}

uint pack_light(float3 c, bool sun) {
    const float3 v = clamp(c, float3(0.0, 0.0, 0.0), float3(63.875, 63.875, 63.875)); // NaN: 0 (clamp's max(0, NaN))
    const float peak = max(v.x, max(v.y, v.z));
    uint e = 1u;
    [loop]
    while (e < 15u && peak >= exp2(float(e) - 9.0)) {
        ++e;
    }
    float scale = 512.0 / exp2(float(e) - 9.0);
    if (uint(round(peak * scale)) > 511u && e < 15u) {
        ++e;
        scale *= 0.5;
    }
    const uint3 m = min(uint3(round(v * scale)), uint3(511u, 511u, 511u));
    return m.x | (m.y << 9) | (m.z << 18) | (e << 27) | (sun ? 0x80000000u : 0u);
}

// The light from the sky along a direction: the hemisphere colours.
float3 sky_radiance(Frame f, float3 dir) {
    return lerp(f.ground_light.rgb, f.sky_light.rgb, dir.y * 0.5 + 0.5);
}

// What a filled voxel sends back (diffuse, the same every way): its colour
// times the sun (if it reaches it) and the light reaching it. Not lit yet:
// the sky colours stand in.
float3 voxel_radiance(Frame f, uint packed, uint light) {
    const float3 albedo = unpack_color(packed);
    const float3 n = unpack_normal(packed);
    if (!light_lit(light)) {
        return albedo * sky_radiance(f, n);
    }
    const float sun = light_sun(light) ? saturate(dot(n, f.sun_direction.xyz)) : 0.0;
    return albedo * (f.sun_light.rgb * sun + unpack_light(light));
}

// The voxel at a camera-relative point on the finest level from `min_level`
// up whose window holds it (a slot not built yet: the next coarser level).
// False past the coarsest window. `level` is the level looked at; `index`
// and `voxel` place a filled one (pool index, voxel in the brick).
bool voxel_at(VoxelWorld w, ByteAddressBuffer levels, ByteAddressBuffer pool, ByteAddressBuffer table, float3 p,
              uint min_level, out uint level, out bool filled, out uint index, out uint voxel) {
    level = 0u;
    filled = false;
    index = k_no_brick;
    voxel = 0u;
    const int n = int(w.bricks_per_axis);
    const uint slots = uint(n * n * n);
    const uint count = min(w.level_count, k_max_levels);
    for (uint i = min_level; i < count; ++i) {
        const Level l = load_level(levels, w.levels_offset, i);
        const int3 b = int3(floor((p - l.origin) / l.brick_size));
        if (any(b < int3(0, 0, 0)) || any(b >= int3(n, n, n))) {
            continue;
        }
        const uint found = table.Load((i * slots + slot_of(l.brick + b, n)) * 4);
        if (found == k_unbuilt) {
            continue;
        }
        level = i;
        if (found == k_no_brick) {
            return true;
        }
        const float size = l.brick_size / 8.0;
        const float3 brick_min = l.origin + float3(b) * l.brick_size;
        const int3 v = clamp(int3(floor((p - brick_min) / size)), int3(0, 0, 0), int3(7, 7, 7));
        const uint at = uint(v.x + 8 * (v.y + 8 * v.z));
        const uint word = pool.Load(found * k_brick_bytes + (at >> 5) * 4);
        filled = ((word >> (at & 31u)) & 1u) != 0u;
        index = found;
        voxel = at;
        return true;
    }
    return false;
}

// Cones: tan of the half angle, and how opaque a filled voxel is (a
// surface voxel at its level's size; a little light gets past, as coarse
// voxels are fuller than what they hold).
static const float k_cone_tan = 0.577; // 30 degrees
static const float k_cone_alpha = 0.85;
static const uint k_cone_steps = 24;

// The light arriving along a cone from `origin` (camera-relative) along
// `dir` (unit), from `t` on, sampling levels `min_level` and up with
// samples as wide as the cone (at most half its width, so a sample never
// reaches back over the surface the cone leaves). Whatever gets through
// comes from the sky.
float3 trace_cone(VoxelWorld w, RWByteAddressBuffer light, Frame f, float3 origin, float3 dir, uint min_level,
                  float t) {
    ByteAddressBuffer levels = read_buffer(w.levels);
    ByteAddressBuffer pool = read_buffer(w.pool);
    ByteAddressBuffer table = read_buffer(w.table);
    const uint count = min(w.level_count, k_max_levels);
    const float voxel0 = load_level(levels, w.levels_offset, 0).brick_size / 8.0;
    float3 color = float3(0.0, 0.0, 0.0);
    float a = 0.0;
    for (uint s = 0; s < k_cone_steps && a < 0.95; ++s) {
        const float width = 2.0 * t * k_cone_tan;
        const float wanted = floor(log2(max(width * 0.5 / voxel0, 1.0)));
        const uint level = min(max(uint(wanted), min_level), count - 1u);
        uint used;
        bool filled;
        uint index;
        uint voxel;
        if (!voxel_at(w, levels, pool, table, origin + dir * t, level, used, filled, index, voxel)) {
            break; // past the coarsest window
        }
        if (filled) {
            const uint packed = pool.Load(index * k_brick_bytes + 64 + voxel * 4);
            const uint lw = light.Load((index * k_brick_voxels + voxel) * 4);
            color += (1.0 - a) * k_cone_alpha * voxel_radiance(f, packed, lw);
            a += (1.0 - a) * k_cone_alpha;
        }
        const float size = load_level(levels, w.levels_offset, used).brick_size / 8.0;
        t += max(width * 0.5, size * 0.5);
    }
    return color + (1.0 - a) * sky_radiance(f, dir);
}

// Six cones over the hemisphere around `n`: one up it, five around at 60
// degrees, weighted by the cosine (a quarter, and 0.15 each).
float3 gather_cones(VoxelWorld w, RWByteAddressBuffer light, Frame f, float3 origin, float3 n, uint min_level,
                    float t) {
    const float3 helper = abs(n.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    const float3 tx = normalize(cross(helper, n));
    const float3 ty = cross(n, tx);
    float3 sum = 0.25 * trace_cone(w, light, f, origin, n, min_level, t);
    for (uint k = 0; k < 5; ++k) {
        const float phi = float(k) * (6.2831853 / 5.0);
        const float3 dir = n * 0.5 + (tx * cos(phi) + ty * sin(phi)) * 0.8660254;
        sum += 0.15 * trace_cone(w, light, f, origin, dir, min_level, t);
    }
    return sum;
}

// --- Shading's lookup -----------------------------------------------------

// The light around a surface point (camera-relative, normal `n`) on one
// level: the lit voxels of the 2 x 2 x 2 around it (half a voxel out along
// the normal), weighted by distance and by how much they face the same way
// (a wall's other side, or the floor under a wall, does not count). rgb:
// the weighted sum, a: the weights' (0: nothing lit there).
float4 bounce_on_level(VoxelWorld w, RWByteAddressBuffer light, ByteAddressBuffer levels, ByteAddressBuffer pool,
                       ByteAddressBuffer table, uint level, float3 p, float3 n) {
    const Level l = load_level(levels, w.levels_offset, level);
    const int n_axis = int(w.bricks_per_axis);
    const uint slots = uint(n_axis * n_axis * n_axis);
    const float size = l.brick_size / 8.0;
    const float3 g = (p + n * (0.5 * size) - l.origin) / size - 0.5;
    const int3 base = int3(floor(g));
    const float3 within = g - float3(base);
    float3 sum = float3(0.0, 0.0, 0.0);
    float weight = 0.0;
    const int edge = n_axis * 8;
    for (uint c = 0; c < 8; ++c) {
        const int3 o = int3(int(c & 1u), int((c >> 1) & 1u), int(c >> 2));
        const int3 v = base + o;
        if (any(v < int3(0, 0, 0)) || any(v >= int3(edge, edge, edge))) {
            continue;
        }
        const int3 b = v >> 3;
        const uint index = table.Load((level * slots + slot_of(l.brick + b, n_axis)) * 4);
        if (index >= k_unbuilt) {
            continue; // nothing there, or not built yet
        }
        const int3 local = v & 7;
        const uint at = uint(local.x + 8 * (local.y + 8 * local.z));
        const uint word = pool.Load(index * k_brick_bytes + (at >> 5) * 4);
        if (((word >> (at & 31u)) & 1u) == 0u) {
            continue;
        }
        const uint lw = light.Load((index * k_brick_voxels + at) * 4);
        if (!light_lit(lw)) {
            continue;
        }
        const float3 vn = unpack_normal(pool.Load(index * k_brick_bytes + 64 + at * 4));
        const float3 t3 = lerp(1.0 - within, within, float3(o));
        const float wt = t3.x * t3.y * t3.z * saturate(dot(n, vn) * 1.25 - 0.25);
        sum += wt * unpack_light(lw);
        weight += wt;
    }
    return float4(sum, weight);
}

// How far a point is inside a level's window (metres; negative: outside).
float inside_window(Level l, float3 p, int n) {
    const float window = l.brick_size * float(n);
    const float3 lo = p - l.origin;
    const float3 hi = l.origin + window - p;
    return min(min(min(lo.x, lo.y), lo.z), min(min(hi.x, hi.y), hi.z));
}

static const float k_bounce_enough = 0.05; // the least weight trusted on its own

// The bounce and sky light reaching a surface point, from the finest level
// that holds it (two voxels in), or the next ones up where that has
// nothing lit; blended with the next level up over the last brick before
// the window's edge, so moving windows show no seams. rgb: the light, a:
// how much to trust it (0: nothing lit around here, use the sky colours).
float4 voxel_bounce(VoxelWorld w, RWByteAddressBuffer light, float3 p, float3 n) {
    ByteAddressBuffer levels = read_buffer(w.levels);
    ByteAddressBuffer pool = read_buffer(w.pool);
    ByteAddressBuffer table = read_buffer(w.table);
    const int n_axis = int(w.bricks_per_axis);
    const uint count = min(w.level_count, k_max_levels);
    if (count == 0u || n_axis <= 0) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    uint level = count;
    for (uint i = 0; i < count; ++i) {
        const Level l = load_level(levels, w.levels_offset, i);
        if (inside_window(l, p, n_axis) >= l.brick_size / 4.0) {
            level = i;
            break;
        }
    }
    if (level == count) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    float4 got = bounce_on_level(w, light, levels, pool, table, level, p, n);
    for (uint up = 0; up < 2 && got.w < k_bounce_enough && level + 1u < count; ++up) {
        ++level;
        got = bounce_on_level(w, light, levels, pool, table, level, p, n);
    }
    if (got.w < k_bounce_enough) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    float3 color = got.rgb / got.w;
    float trust = saturate(got.w * 4.0);
    const Level l = load_level(levels, w.levels_offset, level);
    const float inside = inside_window(l, p, n_axis);
    if (level + 1u < count && inside < 2.0 * l.brick_size) {
        const float4 up = bounce_on_level(w, light, levels, pool, table, level + 1u, p, n);
        if (up.w >= k_bounce_enough) {
            const float keep = saturate((inside - l.brick_size) / l.brick_size);
            color = lerp(up.rgb / up.w, color, keep);
            trust = lerp(saturate(up.w * 4.0), trust, keep);
        }
    }
    return float4(color, trust);
}
