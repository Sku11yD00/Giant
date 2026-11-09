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

// Samples, at the middle, layer i / 2 at mip i % 2 of a texture array
// (`count`: its read descriptor) for i in 0..3, then mip 0 and 1 of a plain
// texture (`value`), each as 4 floats into `output`.
[shader("compute")]
[numthreads(8, 1, 1)]
void cs_sample(uint3 id : SV_DispatchThreadID) {
    if (id.x >= 6) {
        return;
    }
    const SamplerState s = get_sampler(k_sampler_point_clamp);
    float4 c;
    if (id.x < 4) {
        c = read_texture_array(push.count).SampleLevel(s, float3(0.5, 0.5, float(id.x / 2u)), float(id.x % 2u));
    } else {
        c = read_texture(push.value).SampleLevel(s, float2(0.5, 0.5), float(id.x - 4u));
    }
    write_buffer(push.output).Store4(id.x * 16, asuint(c));
}
