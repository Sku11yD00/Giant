// The voxel debug view: marches each pixel's view ray through the voxel
// clipmap (voxel_march.slang) and shades the first voxel it meets with the
// sun and sky.
//
// Layout: VoxelPush in src/gpu_layout.h.
#include "voxel_march.slang"

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

// A triangle covering the screen.
[shader("vertex")]
float4 vs_main(uint vertex_id : SV_VertexID) : SV_Position {
    const float2 p = float2(vertex_id == 1 ? 3.0 : -1.0, vertex_id == 2 ? 3.0 : -1.0);
    return float4(p, 0.0, 1.0);
}

[shader("pixel")]
float4 ps_main(float4 position : SV_Position) : SV_Target {
    const Frame f = load_frame(push.frame, push.frame_offset);
    const float2 ndc = float2(position.x / f.width * 2.0 - 1.0, 1.0 - position.y / f.height * 2.0);
    const float3 dir = normalize(f.ray_x.xyz * ndc.x + f.ray_y.xyz * ndc.y + f.ray_z.xyz);
    VoxelWorld w;
    w.pool = push.pool;
    w.table = push.table;
    w.levels = push.levels;
    w.levels_offset = push.levels_offset;
    w.level_count = push.level_count;
    w.bricks_per_axis = push.bricks_per_axis;
    float t;
    uint packed;
    if (march_voxels(w, dir, f.near_plane, 3.0e38, t, packed)) {
        const float3 normal = unpack_normal(packed);
        const float3 albedo = unpack_color(packed);
        const float3 sun = f.sun_light.xyz * max(dot(normal, f.sun_direction.xyz), 0.0);
        const float3 ambient = lerp(f.ground_light.xyz, f.sky_light.xyz, normal.y * 0.5 + 0.5);
        const float3 lit = albedo * (sun + ambient) * f.exposure;
        return float4(lit / (1.0 + lit), 1.0); // simple tone map: a debug view
    }
    return float4(push.background.rgb, 1.0);
}
