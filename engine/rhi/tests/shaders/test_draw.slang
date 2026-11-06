// rhi tests: flat-coloured triangles whose 2D positions are pulled from a
// buffer (no vertex buffers), at a fixed depth.
#include "giant/bindless.slang"

struct DrawPush {
    float4 color;
    uint positions;     // read descriptor of float2 clip-space positions
    uint position_base; // byte offset of position 0
    float depth;        // reverse-Z: 1 is near
    uint pad;
};
ConstantBuffer<DrawPush> push : register(b0);

[shader("vertex")]
float4 vs_main(uint vertex_id : SV_VertexID) : SV_Position {
    const float2 p = asfloat(read_buffer(push.positions).Load2(push.position_base + vertex_id * 8));
    return float4(p, push.depth, 1.0);
}

[shader("pixel")]
float4 ps_main() : SV_Target {
    return push.color;
}

// A pass without targets: each pixel keeps the nearest depth drawn over it
// in a buffer of floats, 8 a row (push.pad: its write descriptor).
[shader("pixel")]
void ps_write(float4 position : SV_Position) {
    uint ignored;
    write_buffer(push.pad).InterlockedMax((uint(position.y) * 8 + uint(position.x)) * 4, asuint(position.z), ignored);
}
