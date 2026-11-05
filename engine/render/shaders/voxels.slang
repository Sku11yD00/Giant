// The voxel debug view: marches each pixel's view ray through the voxel
// clipmap (src/voxel_scene.h) and shades the first voxel it meets with the
// sun and sky. Finest level first; where the ray leaves a level's window
// the next level up takes over, out to the coarsest, and bricks not built
// yet (just come into view) are stood in for by the next coarser level.
//
// Layouts: VoxelPush and VoxelLevel in src/gpu_layout.h; bricks are
// voxel::Brick (16 u32 of occupancy bits, then one packed u32 per voxel).
#include "common.slang"

struct VoxelPush {
    uint frame;
    uint frame_offset;
    uint pool;          // bricks
    uint table;         // u32 per slot, level after level: brick index, k_no_brick or k_unbuilt
    uint levels;        // VoxelLevel per level
    uint levels_offset;
    uint level_count;
    uint bricks_per_axis;
    float4 background;  // linear
};
ConstantBuffer<VoxelPush> push : register(b0);

static const uint k_brick_bytes = 2112;
static const uint k_no_brick = 0xFFFFFFFFu;
static const uint k_unbuilt = 0xFFFFFFFEu; // not built yet: use the next coarser level
static const uint k_max_steps = 1024;
static const uint k_max_levels = 16;

struct Level {
    float3 origin;     // the window's minimum corner, camera-relative
    float brick_size;  // metres
    int3 brick;        // the window's minimum brick
    float3 filled_min; // around the level's bricks (min > max: none)
    float3 filled_max;
};

Level load_level(ByteAddressBuffer b, uint level) {
    const uint at = push.levels_offset + level * 64;
    const float4 o = load4(b, at);
    Level l;
    l.origin = o.xyz;
    l.brick_size = o.w;
    l.brick = asint(b.Load3(at + 16));
    l.filled_min = load4(b, at + 32).xyz;
    l.filled_max = load4(b, at + 48).xyz;
    return l;
}

// Where the ray from p enters the box [lo, hi], as a distance from p
// (0 if p is inside); negative if it misses the box.
float enter_distance(float3 p, float3 inv_dir, float3 lo, float3 hi) {
    const float3 a = (lo - p) * inv_dir;
    const float3 b = (hi - p) * inv_dir;
    const float3 near3 = min(a, b);
    const float3 far3 = max(a, b);
    const float t_in = max(max(near3.x, near3.y), max(near3.z, 0.0));
    const float t_out = min(far3.x, min(far3.y, far3.z));
    return t_in <= t_out ? t_in : -1.0;
}

// A triangle covering the screen.
[shader("vertex")]
float4 vs_main(uint vertex_id : SV_VertexID) : SV_Position {
    const float2 p = float2(vertex_id == 1 ? 3.0 : -1.0, vertex_id == 2 ? 3.0 : -1.0);
    return float4(p, 0.0, 1.0);
}

// Distance along the ray from `p` (inside the box) to where it leaves the
// box [lo, lo + size].
float exit_distance(float3 p, float3 inv_dir, float3 lo, float3 size) {
    const float3 hi = lo + size;
    const float tx = ((inv_dir.x > 0.0 ? hi.x : lo.x) - p.x) * inv_dir.x;
    const float ty = ((inv_dir.y > 0.0 ? hi.y : lo.y) - p.y) * inv_dir.y;
    const float tz = ((inv_dir.z > 0.0 ? hi.z : lo.z) - p.z) * inv_dir.z;
    return max(min(tx, min(ty, tz)), 0.0);
}

float3 unpack_color(uint v) {
    const float3 c = float3(float((v >> 11) & 31u) / 31.0, float((v >> 5) & 63u) / 63.0, float(v & 31u) / 31.0);
    return c * c;
}

float sign_not_zero(float v) { return v < 0.0 ? -1.0 : 1.0; }

float3 unpack_normal(uint v) {
    const float u = float((v >> 16) & 255u) / 255.0 * 2.0 - 1.0;
    const float w = float((v >> 24) & 255u) / 255.0 * 2.0 - 1.0;
    float3 n = float3(u, w, 1.0 - abs(u) - abs(w));
    if (n.z < 0.0) {
        n.x = (1.0 - abs(w)) * sign_not_zero(u);
        n.y = (1.0 - abs(u)) * sign_not_zero(w);
    }
    return safe_normalize(n, float3(0.0, 1.0, 0.0));
}

int wrap(int v, int n) {
    const int m = v % n;
    return m < 0 ? m + n : m;
}

// A brick's slot in its level's table (voxel::Clipmap::slot).
uint slot_of(int3 brick, int n) {
    return uint(wrap(brick.x, n) + n * (wrap(brick.y, n) + n * wrap(brick.z, n)));
}

[shader("pixel")]
float4 ps_main(float4 position : SV_Position) : SV_Target {
    const Frame f = load_frame(push.frame, push.frame_offset);
    const float2 ndc = float2(position.x / f.width * 2.0 - 1.0, 1.0 - position.y / f.height * 2.0);
    const float3 dir = normalize(f.ray_x.xyz * ndc.x + f.ray_y.xyz * ndc.y + f.ray_z.xyz);
    // No division by zero: axis-parallel rays get a tiny slope.
    const float3 safe = float3(abs(dir.x) > 1e-7 ? dir.x : 1e-7, abs(dir.y) > 1e-7 ? dir.y : 1e-7,
                               abs(dir.z) > 1e-7 ? dir.z : 1e-7);
    const float3 inv_dir = 1.0 / safe;
    ByteAddressBuffer levels = read_buffer(push.levels);
    ByteAddressBuffer pool = read_buffer(push.pool);
    ByteAddressBuffer table = read_buffer(push.table);
    const int n = int(push.bricks_per_axis);
    const uint slots = uint(n * n * n);
    const uint level_count = min(push.level_count, k_max_levels);
    Level lv[k_max_levels];
    for (uint li = 0; li < k_max_levels; ++li) {
        lv[li] = load_level(levels, min(li, max(level_count, 1u) - 1u));
    }

    float t = f.near_plane;
    // The ray starts at the camera, inside every window, and leaves each
    // (a box) once for good: the level only goes up.
    uint first = 0;
    for (uint step = 0; step < k_max_steps; ++step) {
        const float3 p = dir * t;
        // The finest level whose window holds the point.
        uint level = level_count;
        Level l = lv[0];
        int3 local = int3(0, 0, 0);
        for (uint i = first; i < level_count; ++i) {
            const Level c = lv[i];
            const int3 b = int3(floor((p - c.origin) / c.brick_size));
            if (all(b >= int3(0, 0, 0)) && all(b < int3(n, n, n))) {
                level = i;
                l = c;
                local = b;
                break;
            }
        }
        if (level == level_count) {
            break; // past the coarsest level: nothing more to hit
        }
        first = level;
        // Skip straight to the level's bricks, or out of its window when
        // the ray misses them all (the sky, mostly).
        const float window = l.brick_size * float(n);
        const float leave = exit_distance(p, inv_dir, l.origin, float3(window, window, window));
        const float reach = enter_distance(p, inv_dir, l.filled_min, l.filled_max);
        if (reach < 0.0 || reach >= leave) {
            t += leave + l.brick_size * 1e-4;
            continue;
        }
        if (reach > 0.0) {
            t += reach + l.brick_size * 1e-4;
            continue;
        }
        const int3 brick = l.brick + local;
        const float3 brick_min = l.origin + float3(local) * l.brick_size;
        const float brick_exit =
            exit_distance(p, inv_dir, brick_min, float3(l.brick_size, l.brick_size, l.brick_size));
        uint index = table.Load((level * slots + slot_of(brick, n)) * 4);
        // A brick not built yet: the next coarser level that has one stands
        // in (stepping no further than this brick, whose neighbours may be built).
        float sample_size = l.brick_size;
        float3 sample_min = brick_min;
        if (index == k_unbuilt) {
            index = k_no_brick;
            for (uint j = level + 1; j < level_count; ++j) {
                const Level c = lv[j];
                const int3 b = int3(floor((p - c.origin) / c.brick_size));
                if (any(b < int3(0, 0, 0)) || any(b >= int3(n, n, n))) {
                    break;
                }
                const uint found = table.Load((j * slots + slot_of(c.brick + b, n)) * 4);
                if (found != k_unbuilt) {
                    index = found;
                    sample_size = c.brick_size;
                    sample_min = c.origin + float3(b) * c.brick_size;
                    break;
                }
            }
        }
        if (index == k_no_brick) {
            t += brick_exit + l.brick_size * 1e-4;
            continue;
        }
        const float voxel = sample_size / 8.0;
        const int3 v = clamp(int3(floor((p - sample_min) / voxel)), int3(0, 0, 0), int3(7, 7, 7));
        const uint i = uint(v.x + 8 * (v.y + 8 * v.z));
        const uint base = index * k_brick_bytes;
        const uint word = pool.Load(base + (i >> 5) * 4);
        if (((word >> (i & 31u)) & 1u) != 0u) {
            const uint packed = pool.Load(base + 64 + i * 4);
            const float3 normal = unpack_normal(packed);
            const float3 albedo = unpack_color(packed);
            const float3 sun = f.sun_light.xyz * max(dot(normal, f.sun_direction.xyz), 0.0);
            const float3 ambient = lerp(f.ground_light.xyz, f.sky_light.xyz, normal.y * 0.5 + 0.5);
            const float3 lit = albedo * (sun + ambient) * f.exposure;
            return float4(lit / (1.0 + lit), 1.0); // simple tone map: a debug view
        }
        float step_to;
        if (word == 0u) {
            // The word's 32 voxels (a row block: all of x, 4 of y, 1 of z) are empty.
            const float3 block_min = sample_min + float3(0.0, float(v.y & ~3), float(v.z)) * voxel;
            step_to = exit_distance(p, inv_dir, block_min, float3(8.0, 4.0, 1.0) * voxel);
        } else {
            step_to = exit_distance(p, inv_dir, sample_min + float3(v) * voxel, float3(voxel, voxel, voxel));
        }
        t += min(step_to, brick_exit) + l.brick_size / 8.0 * 1e-4;
    }
    return float4(push.background.rgb, 1.0);
}
