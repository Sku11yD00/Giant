// rhi tests: a mesh shader drawing horizontal strips of the screen, one
// strip (two triangles) per group, top to bottom, in a flat colour.
#include "giant/bindless.slang"

struct MeshPush {
    float4 color;
    uint strips; // how many strips the screen is cut into
    float depth; // reverse-Z: 1 is near
    uint pad0;
    uint pad1;
};
ConstantBuffer<MeshPush> push : register(b0);

struct MsVertex {
    float4 position : SV_Position;
};

[shader("mesh")]
[numthreads(4, 1, 1)]
[outputtopology("triangle")]
void ms_main(uint tid : SV_GroupThreadID, uint gid : SV_GroupID,
             OutputVertices<MsVertex, 4> verts, OutputIndices<uint3, 2> tris) {
    SetMeshOutputCounts(4, 2);
    const float top = 1.0 - 2.0 * float(gid) / float(push.strips);
    const float bottom = 1.0 - 2.0 * float(gid + 1) / float(push.strips);
    const float x = (tid & 1u) != 0 ? 1.0 : -1.0;
    const float y = (tid & 2u) != 0 ? bottom : top;
    MsVertex v;
    v.position = float4(x, y, push.depth, 1.0);
    verts[tid] = v;
    if (tid == 0) {
        tris[0] = uint3(0, 2, 1); // counter-clockwise
    } else if (tid == 1) {
        tris[1] = uint3(2, 3, 1);
    }
}

[shader("pixel")]
float4 ps_main() : SV_Target {
    return push.color;
}
