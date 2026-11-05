// rhi tests: a compute shader writing a buffer, and one writing a texture.
#include "giant/bindless.slang"

struct FillPush {
    uint output; // write descriptor
    uint count;
    uint value;
    uint pad;
};
ConstantBuffer<FillPush> push : register(b0);

// output[i] = value + 2 * i
[shader("compute")]
[numthreads(64, 1, 1)]
void cs_fill(uint3 id : SV_DispatchThreadID) {
    if (id.x >= push.count) {
        return;
    }
    write_buffer(push.output).Store(id.x * 4, push.value + id.x * 2);
}

// A horizontal ramp: red = x / (width - 1), with width in `count`.
[shader("compute")]
[numthreads(8, 8, 1)]
void cs_ramp(uint3 id : SV_DispatchThreadID) {
    if (id.x >= push.count || id.y >= push.value) {
        return;
    }
    const float red = float(id.x) / float(max(push.count, 2u) - 1u);
    write_texture(push.output)[id.xy] = float4(red, 0.0, 1.0, 1.0);
}
