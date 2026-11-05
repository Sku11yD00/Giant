// Draws chosen clusters into the visibility buffer: per pixel, which
// cluster (its index in the visible list, plus one; 0 is nothing) and which
// of its triangles. Materials are shaded afterwards (resolve.slang).
//
// Mesh shader path: one group per cluster, a thread per vertex and per
// triangle. Fallback for GPUs without mesh shaders: a draw of 384 vertices
// per cluster (instance i is cluster base + i); unused triangles collapse
// to a point, which the rasterizer drops.
#include "common.slang"

struct RasterPush {
    uint frame;
    uint frame_offset;
    uint instances;
    uint instance_offset;
    uint meshes;
    uint mesh_offset;
    uint visible;        // read descriptor of (instance, cluster) entries
    uint visible_offset;
    uint range;          // read descriptor of (first entry, count) to draw
    uint range_offset;
    uint pad0;
    uint pad1;
};
ConstantBuffer<RasterPush> push : register(b0);

struct Drawn {
    Instance inst;
    MeshEntry mesh;
    Cluster cluster;
    uint entry; // into the visible list
    bool valid; // false past the end of the pass's range
};

// The id-th cluster of this pass.
Drawn drawn_cluster(uint id) {
    Drawn d = {};
    const uint2 range = read_buffer(push.range).Load2(push.range_offset);
    d.entry = range.x + id;
    d.valid = id < range.y;
    if (!d.valid) {
        return d;
    }
    const uint2 e = read_buffer(push.visible).Load2(push.visible_offset + d.entry * 8);
    d.inst = load_instance(read_buffer(push.instances), push.instance_offset + e.x * k_instance_bytes);
    d.mesh = load_mesh(read_buffer(push.meshes), push.mesh_offset + d.inst.mesh * k_mesh_bytes);
    d.cluster = load_cluster(read_buffer_nonuniform(d.mesh.clusters), e.y);
    return d;
}

float4 clip_position(Frame f, Drawn d, uint local_vertex) {
    const uint vertex = read_buffer_nonuniform(d.mesh.vertex_ids).Load((d.cluster.first_vertex + local_vertex) * 4);
    const float3 p = asfloat(read_buffer_nonuniform(d.mesh.vertices).Load3(vertex * 24));
    return transform(f.view_proj[0], f.view_proj[1], f.view_proj[2], f.view_proj[3],
                     float4(to_world(d.inst, p), 1.0));
}

struct MsVertex {
    float4 position : SV_Position;
};
struct MsPrimitive {
    uint2 vis : VIS_ID;
};

[shader("mesh")]
[numthreads(128, 1, 1)]
[outputtopology("triangle")]
void ms_main(uint tid : SV_GroupThreadID, uint3 gid : SV_GroupID,
             OutputVertices<MsVertex, 128> verts, OutputIndices<uint3, 128> tris,
             OutputPrimitives<MsPrimitive, 128> prims) {
    const Drawn d = drawn_cluster(gid.y * 65535 + gid.x);
    if (!d.valid) {
        SetMeshOutputCounts(0, 0);
        return;
    }
    SetMeshOutputCounts(d.cluster.vertex_count, d.cluster.triangle_count);
    const Frame f = load_frame(push.frame, push.frame_offset);
    if (tid < d.cluster.vertex_count) {
        MsVertex v;
        v.position = clip_position(f, d, tid);
        verts[tid] = v;
    }
    if (tid < d.cluster.triangle_count) {
        tris[tid] = unpack_triangle(read_buffer_nonuniform(d.mesh.triangles).Load((d.cluster.first_triangle + tid) * 4));
        MsPrimitive p;
        p.vis = uint2(d.entry + 1, tid);
        prims[tid] = p;
    }
}

struct VsOut {
    float4 position : SV_Position;
    nointerpolation uint2 vis : VIS_ID;
};

[shader("vertex")]
VsOut vs_main(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
    VsOut o;
    o.position = float4(0.0, 0.0, 0.0, 1.0);
    o.vis = uint2(0, 0);
    const Drawn d = drawn_cluster(instance_id);
    const uint triangle = vertex_id / 3;
    if (!d.valid || triangle >= d.cluster.triangle_count) {
        return o; // all three corners at one point: nothing drawn
    }
    const uint3 corners = unpack_triangle(read_buffer_nonuniform(d.mesh.triangles).Load((d.cluster.first_triangle + triangle) * 4));
    const uint corner = vertex_id % 3;
    const uint local = corner == 0 ? corners.x : corner == 1 ? corners.y : corners.z;
    const Frame f = load_frame(push.frame, push.frame_offset);
    o.position = clip_position(f, d, local);
    o.vis = uint2(d.entry + 1, triangle);
    return o;
}

struct PsIn {
    float4 position : SV_Position;
    nointerpolation uint2 vis : VIS_ID;
};

[shader("pixel")]
uint2 ps_main(PsIn i) : SV_Target {
    return i.vis;
}
