// Lights voxel bricks (src/voxel_light.h): for each filled voxel of the
// bricks listed, whether the sun reaches it (a march through the voxel
// world towards the sun) and the bounce and sky light reaching it (six
// cones, voxel_gi.slang), into its light word. Bricks just put in the pool
// are first cleared to "not lit" (cs_clear), so nothing reads what the pool
// index held before.
//
// Lamps (src/voxel_lamps.h): each brick lists the lamps reaching it; a
// voxel adds each one's light (with a march towards it for its shadow)
// into its lamp word.
//
// Layouts: LightPush and LightJob in src/gpu_layout.h.
#include "voxel_gi.slang"
#include "lights.slang"

struct LightPush {
    uint frame;
    uint frame_offset;
    uint jobs;     // LightJob each (cs_light), or u32 pool indices (cs_clear)
    uint jobs_offset;
    uint count;
    uint pool;
    uint table;
    uint levels;
    uint levels_offset;
    uint level_count;
    uint bricks_per_axis;
    uint light;    // write descriptor of the light pool
    uint lamps;    // write descriptor of the lamp words (lamps_on 0: none)
    uint lamps_on;
    uint lamp_lights; // gpu::Light each, then the jobs' entries
    uint lamp_lights_offset;
    uint lamp_entries_offset;
    uint3 pad;
};
ConstantBuffer<LightPush> push : register(b0);

// One group row per brick: 2 groups of 64 threads, 4 words each.
[shader("compute")]
[numthreads(64, 1, 1)]
void cs_clear(uint3 id : SV_DispatchThreadID) {
    if (id.y >= push.count || id.x * 4 >= k_brick_voxels) {
        return;
    }
    const uint index = read_buffer(push.jobs).Load(push.jobs_offset + id.y * 4);
    write_buffer(push.light).Store4((index * k_brick_voxels + id.x * 4) * 4, uint4(0, 0, 0, 0));
    if (push.lamps_on != 0u) {
        write_buffer(push.lamps).Store4((index * k_brick_voxels + id.x * 4) * 4, uint4(0, 0, 0, 0));
    }
}

// The light a lamp gives a voxel (centre, normal, size; `origin` a voxel out
// along the normal), over pi as light words hold it; 0 where the voxel
// world stands between them. Coarse voxels are much bigger than a lamp's
// reach: the light is taken over the voxel (from its nearest point for the
// reach, at least half a voxel away, the normal wrapped as the lamp gets
// inside it) so a distant town's lamps still light the coarse ground.
float3 lamp_on_voxel(VoxelWorld w, LocalLight lamp, float3 centre, float3 n, float size, float3 origin, uint level) {
    const float3 to_light = lamp.position - centre;
    const float len = length(to_light);
    const float reach = max(len - 0.87 * size, 0.0);
    if (!(reach < lamp.range) || len <= 0.0) {
        return float3(0.0, 0.0, 0.0);
    }
    const float3 l = to_light / len;
    const float q = reach * reach / (lamp.range * lamp.range);
    const float window = saturate(1.0 - q * q);
    const float d2 = max(len * len, 0.25 * size * size) + lamp.source_radius * lamp.source_radius;
    const float inside = saturate(size / len - 0.5);
    const float n_dot_l = lerp(saturate(dot(n, l)), saturate(dot(n, l) * 0.5 + 0.5), inside);
    const float cone = saturate(dot(-l, lamp.direction) * lamp.spot_scale + lamp.spot_offset);
    const float3 light = lamp.color * (window * window / d2 * cone * cone * n_dot_l / k_gi_pi);
    if (max(light.x, max(light.y, light.z)) < 1e-5 || inside > 0.0) {
        return inside > 0.0 ? light : float3(0.0, 0.0, 0.0); // (in the lamp's own voxel: no march)
    }
    // Its shadow: the voxel world between (the lamp's own fitting, within a
    // couple of voxels of it, left out; the voxel's own surface too).
    const float3 from = origin;
    const float3 to = lamp.position - from;
    const float dist = length(to);
    if (dist <= size) {
        return light;
    }
    float hit_t;
    uint hit;
    uint hit_at;
    const bool blocked = march_voxels_from(w, from, to / dist, level, size * 0.5, dist, float4(lamp.position, 0.1),
                                           float4(n, dot(n, centre) + size), hit_t, hit, hit_at);
    return blocked ? float3(0.0, 0.0, 0.0) : light;
}

groupshared uint g_filled[k_brick_voxels];
groupshared uint g_count;

// One group per brick. Its filled voxels are listed first, then shared out
// evenly (a brick is mostly empty: a surface's shell).
[shader("compute")]
[numthreads(64, 1, 1)]
void cs_light(uint3 group : SV_GroupID, uint lane : SV_GroupIndex) {
    if (group.x >= push.count) {
        return;
    }
    ByteAddressBuffer jobs = read_buffer(push.jobs);
    const uint at = push.jobs_offset + group.x * 32;
    const uint4 head = jobs.Load4(at);
    const int3 brick = asint(jobs.Load4(at + 16)).xyz;
    const uint index = head.x;
    const uint level = head.y;
    ByteAddressBuffer pool = read_buffer(push.pool);
    RWByteAddressBuffer light = write_buffer(push.light);
    const uint base = index * k_brick_bytes;

    if (lane == 0) {
        g_count = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint k = 0; k < k_brick_voxels / 64; ++k) {
        const uint i = lane + k * 64;
        const uint word = pool.Load(base + (i >> 5) * 4);
        if (((word >> (i & 31u)) & 1u) != 0u) {
            uint slot;
            InterlockedAdd(g_count, 1u, slot);
            g_filled[slot] = i;
        } else {
            light.Store((index * k_brick_voxels + i) * 4, 0u); // empty: never read
        }
    }
    GroupMemoryBarrierWithGroupSync();

    const Frame f = load_frame(push.frame, push.frame_offset);
    VoxelWorld w;
    w.pool = push.pool;
    w.table = push.table;
    w.levels = push.levels;
    w.levels_offset = push.levels_offset;
    w.level_count = push.level_count;
    w.bricks_per_axis = push.bricks_per_axis;
    w.stand_ins = 0u; // everything casts and bounces
    w.lamps = push.lamps;
    w.lamps_on = push.lamps_on;
    const Level l = load_level(read_buffer(push.levels), push.levels_offset, level);
    const float3 brick_min = l.origin + float3(brick - l.brick) * l.brick_size;
    const float size = l.brick_size / 8.0;
    const float3 sun_dir = f.sun_direction.xyz;
    const uint count = g_count;
    for (uint j = lane; j < count; j += 64) {
        const uint i = g_filled[j];
        const uint packed = pool.Load(base + 64 + i * 4);
        const float3 n = unpack_normal(packed);
        const float3 centre = brick_min + (float3(float(i & 7u), float((i >> 3) & 7u), float(i >> 6)) + 0.5) * size;
        // Out of its own voxel first, along the normal.
        const float3 origin = centre + n * size;
        bool sun = false;
        if (dot(n, sun_dir) > 0.0 && dot(sun_dir, sun_dir) > 0.5) {
            float hit_t;
            uint hit;
            uint hit_at;
            // The voxel's own surface is left out: a plane a voxel above its
            // centre, as a surface on a voxel boundary fills a voxel on each
            // side, and the inner one's must not shade it.
            sun = !march_voxels_from(w, origin, sun_dir, level, size * 0.5, 3.0e38, float4(0.0, 0.0, 0.0, -1.0),
                                     float4(n, dot(n, centre) + size), hit_t, hit, hit_at);
        }
        const float3 bounce = gather_cones(w, light, f, origin, n, centre + n * size, level, size);
        light.Store((index * k_brick_voxels + i) * 4, pack_light(bounce, sun));
        if (push.lamps_on != 0u) {
            float3 lamp = float3(0.0, 0.0, 0.0);
            ByteAddressBuffer lamps = read_buffer(push.lamp_lights);
            for (uint k = 0; k < head.w; ++k) {
                const uint li = lamps.Load(push.lamp_entries_offset + (head.z + k) * 4);
                lamp += lamp_on_voxel(w, load_light(lamps, push.lamp_lights_offset + li * k_light_bytes), centre, n,
                                      size, origin, level);
            }
            write_buffer(push.lamps).Store((index * k_brick_voxels + i) * 4, pack_light(lamp, false));
        }
    }
}
