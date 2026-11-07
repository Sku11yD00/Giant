// Skinned meshes (renderer.h: add_skinned_mesh, set_pose): poses a mesh's
// vertices into its geometry in the pool, from its bind-space copy, and
// fits each cluster's culling sphere around what it became. One thread
// group per cluster, a thread per vertex.
//
// The source (k_skin_vertex_bytes each, cluster after cluster from
// Cluster::skin_first): bind position, bind normal, four joints (u16) and
// four weights (u8, in 255ths summing to 255). The matrices: one per joint,
// three rows of float4 (a 3x4 affine matrix), bind space to the mesh's.
// Layouts match engine/render/src/gpu_layout.h (SkinPush, Cluster).
#include "common.slang"

static const uint k_skin_vertex_bytes = 36;
static const uint k_skin_matrix_bytes = 48;
static const uint k_skin_dispatch_width = 65535; // groups along x

struct SkinPush {
    uint source;        // read descriptor
    uint clusters;      // write descriptor: the mesh's clusters (their bounds are refit)
    uint residency;     // read descriptor: the mesh's residency table
    uint pool;          // write descriptor: the geometry pool
    uint matrices;      // read descriptor (transient memory)
    uint matrix_offset;
    uint cluster_count;
    uint joint_count;
};
ConstantBuffer<SkinPush> push : register(b0);

// Floats ordered as uints (for atomics): larger floats give larger uints.
uint ordered(float f) {
    const uint u = asuint(f);
    return (u & 0x80000000u) != 0 ? ~u : u | 0x80000000u;
}
float unordered(uint u) {
    return asfloat((u & 0x80000000u) != 0 ? u & 0x7FFFFFFFu : ~u);
}

groupshared uint g_lo[3];
groupshared uint g_hi[3];
groupshared uint g_radius;

float3 row_apply(float4 r0, float4 r1, float4 r2, float3 p, float w) {
    const float4 h = float4(p, w);
    return float3(dot(r0, h), dot(r1, h), dot(r2, h));
}

[shader("compute")]
[numthreads(128, 1, 1)]
void cs_skin(uint3 group_id : SV_GroupID, uint tid : SV_GroupIndex) {
    // Whole groups leave together (before any barrier).
    const uint index = group_id.x + group_id.y * k_skin_dispatch_width;
    if (index >= push.cluster_count) {
        return;
    }
    RWByteAddressBuffer clusters = write_buffer(push.clusters);
    const uint at = index * k_cluster_bytes;
    const uint4 a = clusters.Load4(at);
    const uint4 d = clusters.Load4(at + 16);
    const uint vertex_offset = a.x;
    const uint vertex_count = min(a.y, 128u);
    const uint group = d.x;
    const uint first = d.w; // skin_first
    const uint base = read_buffer(push.residency).Load(group * 4);
    if (base == k_not_resident) {
        return; // not loaded: nothing to pose (it is posed once it is)
    }
    if (tid == 0) {
        for (uint k = 0; k < 3; ++k) {
            g_lo[k] = 0xFFFFFFFFu;
            g_hi[k] = 0u;
        }
        g_radius = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    float3 p = float3(0.0, 0.0, 0.0);
    const bool mine = tid < vertex_count;
    if (mine) {
        ByteAddressBuffer source = read_buffer(push.source);
        const uint s = (first + tid) * k_skin_vertex_bytes;
        const float3 bind = asfloat(source.Load3(s));
        const float3 bind_normal = asfloat(source.Load3(s + 12));
        const uint2 joints = source.Load2(s + 24);
        const uint weights = source.Load(s + 32);
        ByteAddressBuffer matrices = read_buffer(push.matrices);
        float3 n = float3(0.0, 0.0, 0.0);
        for (uint k = 0; k < 4; ++k) {
            const float w = float((weights >> (8 * k)) & 0xFFu) / 255.0;
            const uint joint = ((k < 2 ? joints.x : joints.y) >> (16 * (k & 1))) & 0xFFFFu;
            if (w == 0.0 || joint >= push.joint_count) {
                continue;
            }
            const uint m = push.matrix_offset + joint * k_skin_matrix_bytes;
            const float4 r0 = asfloat(matrices.Load4(m));
            const float4 r1 = asfloat(matrices.Load4(m + 16));
            const float4 r2 = asfloat(matrices.Load4(m + 32));
            p += row_apply(r0, r1, r2, bind, 1.0) * w;
            n += row_apply(r0, r1, r2, bind_normal, 0.0) * w;
        }
        const float len2 = dot(n, n);
        n = len2 > 1e-24 ? n * rsqrt(len2) : float3(0.0, 1.0, 0.0);
        if (!all(isfinite(p))) {
            p = bind; // a broken matrix: left at bind rather than spread over the screen
        }
        RWByteAddressBuffer pool = write_buffer(push.pool);
        const uint v = base + vertex_offset + tid * 24;
        pool.Store3(v, asuint(p));
        pool.Store3(v + 12, asuint(n));
        uint ignored;
        for (uint k = 0; k < 3; ++k) {
            InterlockedMin(g_lo[k], ordered(p[k]), ignored);
            InterlockedMax(g_hi[k], ordered(p[k]), ignored);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (vertex_count == 0) {
        return; // (whole group) nothing to bound
    }
    const float3 lo = float3(unordered(g_lo[0]), unordered(g_lo[1]), unordered(g_lo[2]));
    const float3 hi = float3(unordered(g_hi[0]), unordered(g_hi[1]), unordered(g_hi[2]));
    const float3 centre = (lo + hi) * 0.5;
    if (mine) {
        uint ignored;
        InterlockedMax(g_radius, asuint(length(p - centre)), ignored); // non-negative: orders as uints
    }
    GroupMemoryBarrierWithGroupSync();
    if (tid == 0) {
        // A hair over, for rounding: culling must never cut what is drawn.
        const float r = asfloat(g_radius);
        clusters.Store4(at + 32, asuint(float4(centre, r * 1.0001 + 1e-5 * max(length(centre), 1.0))));
    }
}
