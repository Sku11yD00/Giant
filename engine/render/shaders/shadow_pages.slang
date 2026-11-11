// Sun shadows from virtual shadow pages (src/shadow_pages.h), for shading.
//
// The sun's clipmap: level L is a window of 16 x 16 pages of 128 x 128
// texels in light space (texels of shadow_texel * 2^L metres), centred on
// the camera. A point uses the finest level whose window holds it with
// room for the filter, no finer than its pixel needs; it asks for that
// page, and where the page is not drawn yet falls back to coarser levels
// (asking for each) until one is, and asks for the level above that one
// too, to fall back on later. Nothing drawn: lit.
//
// Depths: 0.5 - (light z - anchor) / depth range, larger nearer the sun,
// from a fixed anchor (cached pages stay right as the camera moves); 0 is
// nothing (lit). Layouts: ShadowLevel and ResolveExtras in src/gpu_layout.h.
//
// A new view (the sun turned a step, or a new anchor) keeps the one before
// as the previous: where a level's page of the new view is not drawn yet,
// the previous view's page on that level stands in (its own axes, windows,
// table and depths, after the current's in the same buffers) before a
// coarser level is tried.
#pragma once
#include "common.slang"

static const uint k_shadow_texels = 128;      // a page's side
static const uint k_shadow_window = 16 * 128; // a level's side, texels
static const uint k_shadow_no_page = 0xFFFFFFFFu;
static const uint k_shadow_drawn_bit = 1u << 30;
static const uint k_shadow_max_levels = 16;

struct Shadows {
    uint atlas;
    uint table;
    uint table_offset;
    uint levels;
    uint levels_offset;
    uint level_count; // 0: off
    uint requests;
    float texel;      // level 0's texel, metres
    float3 x;         // the light's axes
    float3 y;
    float3 z;
    float depth_scale; // 1 / depth range
    float depth_offset; // depths are 0.5 - z * depth_scale + depth_offset (from the CPU's anchor)
    uint prev_on;       // 1: a previous view stands in
    float3 prev_x;      // its axes
    float3 prev_y;
    float3 prev_z;
    float prev_depth_offset;
};

struct ShadowLevel {
    float2 origin;  // the window's minimum corner, light space, camera-relative
    float page_size;
    float texel_size;
    int2 page;      // the window's minimum page
};

ShadowLevel load_shadow_level(ByteAddressBuffer b, uint offset, uint level) {
    const uint at = offset + level * 32;
    const float4 o = asfloat(b.Load4(at));
    ShadowLevel l;
    l.origin = o.xy;
    l.page_size = o.z;
    l.texel_size = o.w;
    l.page = asint(b.Load2(at + 16));
    return l;
}

int shadow_wrap16(int v) {
    const int m = v % 16;
    return m < 0 ? m + 16 : m;
}

// The page slot (0..255) of a window texel (0..2047 on each axis).
uint shadow_slot(ShadowLevel l, int2 texel) {
    const int2 page = l.page + texel / int(k_shadow_texels);
    return uint(shadow_wrap16(page.x) + 16 * shadow_wrap16(page.y));
}

// Asks the CPU for a page (read back a few frames later). Most pixels ask
// for a page already asked for: a plain read first spares the atomic.
void shadow_request(Shadows s, uint level, uint slot) {
    // Most of a wave asks for the same page: one lane asks for all.
    const uint key = level * 256 + slot;
    if (WaveActiveAllEqual(key) && !WaveIsFirstLane()) {
        return;
    }
    RWByteAddressBuffer r = write_buffer(s.requests);
    const uint at = (level * 8 + slot / 32) * 4;
    const uint bit = 1u << (slot % 32);
    if ((r.Load(at) & bit) == 0u) {
        uint ignored;
        r.InterlockedOr(at, bit, ignored);
    }
}

// 1 where the texel of a window holds nothing nearer the sun than `depth`
// (or its page is not drawn: then it does not count), 0 where it does.
// `valid` is 0 when its page is not drawn.
// `level_at`: the level's first table entry, bytes.
float shadow_tap(ByteAddressBuffer table, ByteAddressBuffer atlas, uint level_at, ShadowLevel l, int2 texel,
                 float depth, out float valid) {
    valid = 0.0;
    if (any(texel < int2(0, 0)) || any(texel >= int2(k_shadow_window, k_shadow_window))) {
        return 1.0;
    }
    const uint entry = table.Load(level_at + shadow_slot(l, texel) * 4);
    if (entry == k_shadow_no_page || (entry & k_shadow_drawn_bit) == 0u) {
        return 1.0;
    }
    valid = 1.0;
    const uint2 local = uint2(texel) % k_shadow_texels;
    const uint index = (entry & 0xFFFFu) * (k_shadow_texels * k_shadow_texels) + local.y * k_shadow_texels + local.x;
    const float stored = asfloat(atlas.Load(index * 4));
    return stored > depth ? 0.0 : 1.0;
}

// The bilinear 3 x 3 texel test about `uv` (16 taps): soft by a texel,
// without the blocks of nearest-texel tests. `ask`: neighbouring pages the
// filter reaches are asked for (the current view only). -1: nothing drawn
// under it.
float shadow_filter(Shadows s, ByteAddressBuffer table, ByteAddressBuffer atlas, uint level, uint level_at,
                    ShadowLevel lv, float2 uv, uint slot, float depth, bool ask) {
    const float2 start = uv - 0.5;
    const int2 base = int2(floor(start)) - int2(1, 1);
    const float2 f = start - floor(start);
    float lit = 0.0;
    float weight = 0.0;
    [unroll] for (int j = 0; j < 4; ++j) {
        const float wy = j == 0 ? 1.0 - f.y : j == 3 ? f.y : 1.0;
        [unroll] for (int i = 0; i < 4; ++i) {
            const float wx = i == 0 ? 1.0 - f.x : i == 3 ? f.x : 1.0;
            const int2 texel = base + int2(i, j);
            float valid;
            const float v = shadow_tap(table, atlas, level_at, lv, texel, depth, valid);
            lit += v * wx * wy * valid;
            weight += wx * wy * valid;
            // A neighbouring page the filter reaches: asked for too.
            if (ask) {
                const uint tap_slot = shadow_slot(lv, texel);
                if (tap_slot != slot && i != 1 && i != 2 && j != 1 && j != 2) {
                    shadow_request(s, level, tap_slot);
                }
            }
        }
    }
    return weight > 0.0 ? lit / weight : 1.0;
}

// The previous view's light on a level, where its page there is drawn
// (else -1: not there either).
float shadow_previous(Shadows s, ByteAddressBuffer levels, ByteAddressBuffer table, ByteAddressBuffer atlas,
                      uint count, uint level, float3 world, float3 n, float tangent) {
    const ShadowLevel lv = load_shadow_level(levels, s.levels_offset, count + level);
    const float3 p = world + n * (lv.texel_size * 1.5);
    const float2 uv = (float2(dot(p, s.prev_x), dot(p, s.prev_y)) - lv.origin) / lv.texel_size;
    if (any(uv < float2(2.0, 2.0)) || any(uv >= float2(k_shadow_window - 2, k_shadow_window - 2))) {
        return -1.0;
    }
    const uint level_at = s.table_offset + ((count + level) * 256) * 4;
    const uint slot = shadow_slot(lv, int2(floor(uv)));
    const uint entry = table.Load(level_at + slot * 4);
    if (entry == k_shadow_no_page || (entry & k_shadow_drawn_bit) == 0u) {
        return -1.0;
    }
    const float depth = max(0.5 - dot(p, s.prev_z) * s.depth_scale + s.prev_depth_offset +
                                lv.texel_size * (1.0 + 2.0 * tangent) * s.depth_scale,
                            0.0);
    return shadow_filter(s, table, atlas, level, level_at, lv, uv, slot, depth, false);
}

// How much of the sun reaches a camera-relative point with normal `n`
// (unit) and sun direction `l`, whose pixel covers `pixel_size` metres.
float sun_visibility(Shadows s, float3 world, float3 n, float3 l, float pixel_size) {
    if (s.level_count == 0u) {
        return 1.0;
    }
    ByteAddressBuffer levels = read_buffer(s.levels);
    ByteAddressBuffer table = read_buffer(s.table);
    ByteAddressBuffer atlas = read_buffer(s.atlas);
    const uint count = min(s.level_count, k_shadow_max_levels);
    // No finer than the pixel: a texel of about the pixel's size.
    const float wanted = log2(max(pixel_size / s.texel, 1.0));
    uint level = min(uint(wanted), count - 1);
    const float n_dot_l = saturate(dot(n, l));
    const float tangent = min(sqrt(max(1.0 - n_dot_l * n_dot_l, 0.0)) / max(n_dot_l, 1e-3), 4.0);
    for (; level < count; ++level) {
        const ShadowLevel lv = load_shadow_level(levels, s.levels_offset, level);
        // Pushed off the surface along its normal (more where the sun
        // grazes it), and a depth margin for the filter's slope.
        const float3 p = world + n * (lv.texel_size * 1.5);
        const float2 uv = (float2(dot(p, s.x), dot(p, s.y)) - lv.origin) / lv.texel_size;
        // Room for the filter (2 texels each side) inside the window.
        if (any(uv < float2(2.0, 2.0)) || any(uv >= float2(k_shadow_window - 2, k_shadow_window - 2))) {
            continue;
        }
        const int2 centre = int2(floor(uv));
        const uint slot = shadow_slot(lv, centre);
        shadow_request(s, level, slot);
        const uint entry = table.Load(s.table_offset + (level * 256 + slot) * 4);
        if (entry == k_shadow_no_page || (entry & k_shadow_drawn_bit) == 0u) {
            // Not drawn yet: the previous view's page on this level stands
            // in, else a coarser level.
            if (s.prev_on != 0u) {
                const float before = shadow_previous(s, levels, table, atlas, count, level, world, n, tangent);
                if (before >= 0.0) {
                    return before;
                }
            }
            continue;
        }
        // The next level up is asked for too, so a fallback stays there for
        // when this page is drawn again (a new sun: coarse levels first).
        if (level + 1 < count) {
            const ShadowLevel up = load_shadow_level(levels, s.levels_offset, level + 1);
            const float2 up_uv = (float2(dot(world, s.x), dot(world, s.y)) - up.origin) / up.texel_size;
            if (all(up_uv >= float2(0.0, 0.0)) && all(up_uv < float2(k_shadow_window, k_shadow_window))) {
                shadow_request(s, level + 1, shadow_slot(up, int2(floor(up_uv))));
            }
        }
        // Past the far end of the depth range nothing is stored (0): kept
        // at 0 there, so empty texels never shadow it.
        const float depth = max(0.5 - dot(p, s.z) * s.depth_scale + s.depth_offset +
                                    lv.texel_size * (1.0 + 2.0 * tangent) * s.depth_scale,
                                0.0);
        return shadow_filter(s, table, atlas, level, s.table_offset + (level * 256) * 4, lv, uv, slot, depth, true);
    }
    return 1.0;
}
