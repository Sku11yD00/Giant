// Multisampling's sorting pass (RenderSettings::multisampling): the
// visibility buffer and depth are drawn with four or eight samples a pixel
// (RenderSettings::multisampling_samples), and this finds, per pixel, the
// surfaces its samples see.
//
// Samples are of one surface when they show the same triangle, or the same
// instance at depths that run on from one another (a mesh's inner triangle
// edges: shading them apart would cost every pixel of a dense mesh, for
// colours that hardly differ). "Run on": a sample's depth is where the
// other's would be, carried over by that one's slope across the screen
// (its change to the neighbouring pixels: depth is linear across the screen
// on a flat face, so ground seen at a low angle, whose depth changes a lot
// inside one pixel, is still one surface). Where a neighbour shows
// something else the slope is no use and the samples are taken apart: more
// shading there, never a wrong picture. The surface with the most samples is the
// pixel's own: its triangle and depth go to the one-sample visibility
// buffer and depth that every later pass reads, so the rest of the frame is
// as it was. The other surfaces (at most three; the ones with the most
// samples first, as many as RenderSettings::multisampling_surfaces allows)
// are listed in `groups` for resolve.slang, which shades each once more and
// mixes them as their samples stand to each other: an edge is as smooth as
// the samples make it, and a pixel inside a surface is shaded once, as
// before.
//
// `groups`, a word a pixel (sample_groups.slang reads it): 0 where every
// sample sees one surface. Else bits 0 to 3 the pixel's own surface's
// samples (1 to 7), then six bits for each other surface listed, from bit
// 4 (low three: one of its samples; high three: how many it has, 0 ends
// the list).
#include "common.slang"
#include "sample_groups.slang"

struct SamplesPush {
    uint vis;            // read: the visibility buffer, four or eight samples (rg32_uint)
    uint depth;          // read: its depth, as many
    uint visible;        // read: the visible list ((instance, cluster) entries)
    uint visible_offset;
    uint surfaces;       // of a pixel that are listed to be shaded, its own with them (2 to 4)
    uint samples;        // a pixel in `vis` and `depth`: 4 or 8
};
ConstantBuffer<SamplesPush> push : register(b0);

// Depths (reverse-Z: near over the distance) this near where the slope puts
// them, as a share of the nearer's, run on: half a percent of the distance.
static const float k_same_surface = 0.005;
static const uint k_most_samples = 8;

[shader("vertex")]
float4 vs_main(uint vertex_id : SV_VertexID) : SV_Position {
    const float2 p = float2(vertex_id == 1 ? 3.0 : -1.0, vertex_id == 2 ? 3.0 : -1.0);
    return float4(p, 0.0, 1.0);
}

struct Sorted {
    uint2 vis : SV_Target0;
    uint groups : SV_Target1;
    float depth : SV_Depth;
};

[shader("pixel")]
Sorted ps_main(float4 position : SV_Position) {
    const int2 pixel = int2(position.xy);
    Texture2DMS<uint2> vis = read_texture_ms_uint2(push.vis);
    Texture2DMS<float> depth = read_texture_ms_float(push.depth);
    const uint n = push.samples; // 4 or 8
    uint2 v[k_most_samples];
    float z[k_most_samples];
    float2 slope[k_most_samples]; // each sample's depth's change a pixel right and down
    float2 place[k_most_samples]; // and where in the pixel it is, from the middle
    bool one = true;
    float sum_all = 0.0;
    // (Past the count: the first sample again, not read twice, which joins
    // its own surface below and is not counted. With four samples the pass
    // reads four.)
    const uint2 v_first = vis.Load(pixel, 0);
    const float z_first = depth.Load(pixel, 0);
    const float2 place_first = depth.GetSamplePosition(0);
    [unroll] for (uint i = 0; i < k_most_samples; ++i) {
        v[i] = v_first;
        z[i] = z_first;
        place[i] = place_first;
        if (i != 0u && (i < 4u || n > 4u)) {
            v[i] = vis.Load(pixel, int(i));
            z[i] = depth.Load(pixel, int(i));
            place[i] = depth.GetSamplePosition(int(i));
        }
        slope[i] = float2(ddx(z[i]), ddy(z[i]));
        one = one && all(v[i] == v[0]);
        sum_all += i < n ? z[i] : 0.0;
    }
    Sorted o;
    if (one) {
        // One triangle (or nothing) over the whole pixel. Depth is linear
        // across the screen and the samples lie evenly round the middle:
        // their mean is the middle's.
        o.vis = v[0];
        o.groups = 0u;
        o.depth = sum_all / float(n);
        return o;
    }
    // Each sample's instance (nothing drawn: none).
    uint instance[k_most_samples];
    [unroll] for (uint i = 0; i < k_most_samples; ++i) {
        instance[i] = 0xFFFFFFFFu;
        if (v[i].x != 0u && (i < 4u || n > 4u)) { // (past the count: the first sample's triangle, the same without it)
            instance[i] = read_buffer(push.visible).Load(push.visible_offset + (v[i].x - 1u) * 8u);
        }
    }
    // Each sample joins the first earlier surface it is of (told by that
    // surface's first sample), or starts one.
    uint leader[k_most_samples];
    uint count[k_most_samples];
    [unroll] for (uint i = 0; i < k_most_samples; ++i) {
        leader[i] = i;
        count[i] = 0u;
        [unroll] for (uint j = 0; j < i; ++j) {
            if (leader[i] == i && leader[j] == j) {
                bool same = all(v[i] == v[j]);
                if (!same && instance[i] == instance[j] && v[i].x != 0u) {
                    const float carried = z[j] + dot(slope[j], place[i] - place[j]);
                    same = abs(z[i] - carried) <= k_same_surface * max(z[i], z[j]);
                }
                if (same) {
                    leader[i] = j;
                }
            }
        }
    }
    [unroll] for (uint i = 0; i < k_most_samples; ++i) {
        [unroll] for (uint j = 0; j < k_most_samples; ++j) {
            count[j] += i < n && leader[i] == j ? 1u : 0u;
        }
    }
    // The pixel's own: the surface with the most samples (the earliest of equals).
    uint own = 0u;
    [unroll] for (uint i = 1; i < k_most_samples; ++i) {
        own = count[i] > count[own] ? i : own;
    }
    // Shown by the triangle most of its samples see (the earliest of
    // equals), at the mean of its samples' depths.
    uint shown = own;
    uint most = 0u;
    float sum = 0.0;
    [unroll] for (uint i = 0; i < k_most_samples; ++i) {
        if (i < n && leader[i] == own) {
            sum += z[i];
            uint alike = 0u;
            [unroll] for (uint j = 0; j < k_most_samples; ++j) {
                alike += j < n && leader[j] == own && all(v[j] == v[i]) ? 1u : 0u;
            }
            if (alike > most) {
                most = alike;
                shown = i;
            }
        }
    }
    o.vis = v[shown];
    o.depth = sum / float(count[own]);
    o.groups = 0u;
    if (count[own] < n) {
        // The others, the one with the most samples first (the earliest of
        // equals), as many as are to be shaded.
        uint word = count[own];
        uint shift = k_group_first;
        uint taken = 1u << own;
        [unroll] for (uint k = 1u; k < 4u; ++k) {
            uint best = 0u;
            uint best_count = 0u;
            [unroll] for (uint i = 0; i < k_most_samples; ++i) {
                if ((taken & (1u << i)) == 0u && count[i] > best_count) {
                    best = i;
                    best_count = count[i];
                }
            }
            if (best_count != 0u && k < push.surfaces) {
                word |= (best | (best_count << 3u)) << shift;
                shift += k_group_bits;
                taken |= 1u << best;
            }
        }
        o.groups = word;
    }
    return o;
}
