// The voxel debug view: marches each pixel's view ray through the voxel
// clipmap (src/voxel_scene.h) and shades the first voxel it meets with the
// sun and sky. Finest level first; where the ray leaves a level's window
// the next level up takes over, out to the coarsest.
//
// Layouts: VoxelPush and VoxelLevel in src/gpu_layout.h; bricks are
// voxel::Brick (16 u32 of occupancy bits, then one packed u32 per voxel).
#include "common.slang"

struct VoxelPush {
    uint frame;
    uint frame_offset;
    uint pool;          // bricks
    uint table;         // u32 per slot, level after level: brick index or k_no_brick
    uint levels;        // VoxelLevel per level
    uint levels_offset;
    uint level_count;
    uint bricks_per_axis;
    float4 background;  // linear
};
ConstantBuffer<VoxelPush> push : register(b0);

static const uint k_brick_bytes = 2112;
static const uint k_no_brick = 0xffffffff;
static const uint k_max_steps = 768;

struct Level {
    float3 origin;     // the window's minimum corner, camera-relative
    float brick_size;  // metres
    int3 brick;        // the window's minimum brick
};

Level load_level(ByteAddressBuffer b, uint level) {
    const uint at = push.levels_offset + level * 32;
    const float4 o = load4(b, at);
    Level l;
    l.origin = o.xyz;
    l.brick_size = o.w;
    l.brick = asint(b.Load3(at + 16));
    return l;
}

// A triangle covering the screen.
[shader("vertex")]
float4 vs_main(uint vertex_id : SV_VertexID) : SV_Position {
    const float2 p = float2(vertex_id == 1 ? 3.0 : -1.0, vertex_id == 2 ? 3.0 : -1.0);
    return float4(p, 0.0, 1.0);
}

// Distance along the ray from `p` (inside the box) to where it leaves the
// box [lo, lo + size].
float exit_distance(float3 p, float3 inv_dir, float3 lo, float size) {
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

    float t = f.near_plane;
    // The ray starts at the camera, inside every window, and leaves each
    // (a box) once for good: the level only goes up.
    uint first = 0;
    for (uint step = 0; step < k_max_steps; ++step) {
        const float3 p = dir * t;
        // The finest level whose window holds the point.
        uint level = push.level_count;
        Level l = load_level(levels, 0);
        int3 local = int3(0, 0, 0);
        for (uint i = first; i < push.level_count; ++i) {
            const Level c = load_level(levels, i);
            const int3 b = int3(floor((p - c.origin) / c.brick_size));
            if (all(b >= int3(0, 0, 0)) && all(b < int3(n, n, n))) {
                level = i;
                l = c;
                local = b;
                break;
            }
        }
        if (level == push.level_count) {
            break; // past the coarsest level: nothing more to hit
        }
        first = level;
        const int3 brick = l.brick + local;
        const uint slot = uint(wrap(brick.x, n) + n * (wrap(brick.y, n) + n * wrap(brick.z, n)));
        const uint index = table.Load((level * slots + slot) * 4);
        const float3 brick_min = l.origin + float3(local) * l.brick_size;
        if (index == k_no_brick) {
            t += exit_distance(p, inv_dir, brick_min, l.brick_size) + l.brick_size * 1e-4;
            continue;
        }
        const float voxel = l.brick_size / 8.0;
        const int3 v = clamp(int3(floor((p - brick_min) / voxel)), int3(0, 0, 0), int3(7, 7, 7));
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
        t += exit_distance(p, inv_dir, brick_min + float3(v) * voxel, voxel) + voxel * 1e-4;
    }
    return float4(push.background.rgb, 1.0);
}
