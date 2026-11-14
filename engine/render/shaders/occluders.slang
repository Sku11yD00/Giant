// Characters in the light (characters plan C2): each character's body as
// capsules round its bones (FrameInput::occluders) shuts the sky's and the
// bounced light out of what is near it, and gives some back in its own
// colour. Worked out per pixel from the capsules themselves, so it follows
// the pose with no lag and nothing carried over from earlier frames.
//
// cs_occlusion, per 8 x 8 tile of the full-size frame: the box (view space)
// round the points the tile shows, then the capsules whose reach (reach
// radii round the axis) touches it, listed in group memory (at most
// k_tile_occluders: the nearest, as they come nearest first); then
// per pixel, from its point and normal (from depth, as ao.slang's), what
// each listed capsule hides of the cosine-weighted sky: for a sphere
// wholly above the horizon cos(angle to it) x (radius / distance)^2, the
// sphere taken on the axis nearest the point as far out along its normal
// as the axis is from it, faded out over the last quarter of the reach. The visibilities multiply; what they hide comes
// back as the light the capsules bounce, by their colour.
//
// Output (rgba8_unorm): rgb the bounced light (a share of the ambient
// light), a the ambient light let through (1: open). Resolve multiplies its
// ambient diffuse light by a, adds rgb of it, and dims the ambient
// reflections as it does for its other occlusion.
//
// Layout: gpu::OccluderPush and gpu::GpuOccluder in src/gpu_layout.h.
#include "common.slang"

struct OccluderPush {
    uint frame;
    uint frame_offset;
    uint depth;     // read: the scene depth (reverse-Z, full size)
    uint target;    // write: rgba8_unorm, full size
    uint occluders; // read: count GpuOccluder (32 bytes, view space) at offset
    uint offset;
    uint count;
    uint width;
    uint height;
    float reach;
    uint pad0;
    uint pad1;
};
ConstantBuffer<OccluderPush> push : register(b0);

static const uint k_tile_occluders = 64;
// One capsule hides at most this share (a point right against it is not
// quite black: the light comes in round it).
static const float k_most_hidden = 0.9;

groupshared float3 g_low[64];
groupshared float3 g_high[64];
groupshared uint g_count;
groupshared uint g_touches[64];
groupshared float4 g_a[k_tile_occluders];      // view space, radius
groupshared float4 g_b[k_tile_occluders];      // view space, albedo (packed)

// The view-space point of a full-size pixel (z negative ahead), and
// whether anything is drawn there (off the screen: nothing).
float3 occluder_view_point(Frame f, Texture2D<float> depth, int2 px, out bool drawn) {
    drawn = false;
    if (any(px < int2(0, 0)) || px.x >= int(f.width) || px.y >= int(f.height)) {
        return float3(0.0, 0.0, -1.0e6);
    }
    const float d = depth.Load(int3(px, 0));
    drawn = d > 0.0;
    if (!drawn) return float3(0.0, 0.0, -1.0e6);
    const float z = f.near_plane / d;
    const float2 uv = (float2(px) + 0.5) / float2(f.width, f.height);
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    return float3(ndc.x * z / f.p00, ndc.y * z / f.p11, -z);
}

// The flatter of two differences (so edges do not bend the normal).
float3 occluder_flatter(float3 here, float3 a, float3 b, bool a_drawn, bool b_drawn) {
    const float3 da = here - a;
    const float3 db = b - here;
    if (!a_drawn && !b_drawn) return float3(0.0, 0.0, 0.0);
    if (!a_drawn) return db;
    if (!b_drawn) return da;
    return abs(da.z) < abs(db.z) ? da : db;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_occlusion(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    const Frame f = load_frame(push.frame, push.frame_offset);
    Texture2D<float> depth = read_texture_float(push.depth);
    const bool inside = id.x < push.width && id.y < push.height;
    const int2 px = int2(id.xy);
    bool drawn = false;
    float3 here = float3(0.0, 0.0, -1.0e6);
    if (inside) {
        here = occluder_view_point(f, depth, px, drawn);
    }
    // The tile's box round what it shows.
    g_low[gi] = drawn ? here : float3(1.0e30, 1.0e30, 1.0e30);
    g_high[gi] = drawn ? here : float3(-1.0e30, -1.0e30, -1.0e30);
    if (gi == 0u) g_count = 0u;
    GroupMemoryBarrierWithGroupSync();
    [unroll]
    for (uint s = 32u; s > 0u; s >>= 1u) {
        if (gi < s) {
            g_low[gi] = min(g_low[gi], g_low[gi + s]);
            g_high[gi] = max(g_high[gi], g_high[gi + s]);
        }
        GroupMemoryBarrierWithGroupSync();
    }
    const float3 low = g_low[0];
    const float3 high = g_high[0];
    if (low.x <= high.x) {
        // 64 capsules a round, listed in their order (nearest first), so
        // a full list always keeps the nearest, the same every frame.
        ByteAddressBuffer list = read_buffer(push.occluders);
        for (uint first = 0u; first < push.count; first += 64u) {
            const uint i = first + gi;
            bool touches = false;
            float4 ar = float4(0.0, 0.0, 0.0, 0.0);
            uint4 bc = uint4(0u, 0u, 0u, 0u);
            if (i < push.count) {
                const uint at = push.offset + i * 32u;
                ar = asfloat(list.Load4(at));
                bc = list.Load4(at + 16u);
                const float3 b = asfloat(bc.xyz);
                const float3 mid = (ar.xyz + b) * 0.5;
                const float r = length(b - ar.xyz) * 0.5 + ar.w * push.reach;
                const float3 out_of = max(low - mid, 0.0) + max(mid - high, 0.0);
                touches = dot(out_of, out_of) <= r * r;
            }
            g_touches[gi] = touches ? 1u : 0u;
            GroupMemoryBarrierWithGroupSync();
            const uint listed = g_count;
            if (touches) {
                uint slot = listed;
                for (uint k = 0u; k < gi; ++k) slot += g_touches[k];
                if (slot < k_tile_occluders) {
                    g_a[slot] = ar;
                    g_b[slot] = float4(asfloat(bc.xyz), asfloat(bc.w));
                }
            }
            GroupMemoryBarrierWithGroupSync();
            if (gi == 0u) {
                uint total = 0u;
                for (uint k = 0u; k < 64u; ++k) total += g_touches[k];
                g_count = listed + total;
            }
            GroupMemoryBarrierWithGroupSync();
            if (g_count >= k_tile_occluders) break; // (the same for the whole group)
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (!inside) return;
    RWTexture2D<float4> target = write_texture(push.target);
    const uint count = min(g_count, k_tile_occluders);
    if (!drawn || count == 0u) {
        target[id.xy] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    bool l_drawn, r_drawn, u_drawn, d_drawn;
    const float3 l = occluder_view_point(f, depth, px - int2(1, 0), l_drawn);
    const float3 r = occluder_view_point(f, depth, px + int2(1, 0), r_drawn);
    const float3 u = occluder_view_point(f, depth, px - int2(0, 1), u_drawn);
    const float3 dn = occluder_view_point(f, depth, px + int2(0, 1), d_drawn);
    const float3 v = safe_normalize(-here, float3(0.0, 0.0, 1.0));
    float3 n = safe_normalize(cross(occluder_flatter(here, u, dn, u_drawn, d_drawn),
                                    occluder_flatter(here, l, r, l_drawn, r_drawn)), v);
    if (dot(n, v) < 0.0) n = -n;
    float open = 1.0;
    float3 bounce = float3(0.0, 0.0, 0.0);
    [loop]
    for (uint i = 0u; i < count; ++i) {
        const float4 ar = g_a[i];
        const float4 bc = g_b[i];
        const float3 ab = bc.xyz - ar.xyz;
        const float inv = 1.0 / max(dot(ab, ab), 1e-12);
        const float radius = ar.w;
        const float reach = radius * push.reach;
        // The axis's nearest point, then the nearest to the point that far
        // out along the normal: the part of the capsule over the surface
        // (a leg standing on the ground shades it from above, not from
        // its foot alone).
        const float nearest = length(ar.xyz + ab * saturate(dot(here - ar.xyz, ab) * inv) - here);
        if (nearest >= reach) continue;
        // A point inside it (another part of the body where the capsules
        // overlap at a joint, or ground a sunk capsule reaches under) is
        // shaded less the deeper it is: never blacked out, and no hard
        // edge where the capsule meets the ground.
        const float inside = nearest < radius ? (nearest * nearest) / (radius * radius) : 1.0;
        const float3 above = here + n * max(nearest, radius);
        const float t = saturate(dot(above - ar.xyz, ab) * inv);
        const float3 to = ar.xyz + ab * t - here; // to the sphere there
        const float distance = length(to);
        if (distance >= reach || distance <= 1e-6) continue;
        const float d = max(distance, radius);
        const float fade = saturate((reach - distance) / (0.25 * reach));
        const float hidden = saturate(dot(n, to / distance)) * (radius * radius) / (d * d) * fade * inside * k_most_hidden;
        if (hidden <= 0.0) continue;
        open *= 1.0 - hidden;
        const uint c = asuint(bc.w);
        const float3 albedo = float3(float(c & 255u), float((c >> 8) & 255u), float((c >> 16) & 255u)) / 255.0;
        // (Its side towards the point sees about half the light from all round.)
        bounce += albedo * (0.5 * hidden);
    }
    // What came back is at most what was hidden.
    const float shut = 1.0 - open;
    const float most = max(max(bounce.r, bounce.g), max(bounce.b, 1e-6));
    bounce *= min(1.0, shut / most);
    target[id.xy] = float4(saturate(bounce), saturate(open));
}
