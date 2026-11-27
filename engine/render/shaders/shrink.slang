// Supersampling's last step (RenderSettings::supersampling): the frame,
// drawn larger than the target, averaged down into it. Each target pixel
// takes the source texels its square covers, each by the share of it
// covered (a box, exact for any scale from 1 to 2: at most 3 x 3 texels).
// The source is sRGB read as linear light and the target sRGB, so light
// is what is averaged. Layout: ShrinkPush in src/gpu_layout.h.
#include "common.slang"

struct ShrinkPush {
    float2 scale;  // source texels a target pixel spans, each way (1 to 2)
    float2 size;   // the source, texels
    uint color;    // read descriptor: the source
    uint pad0;
    uint pad1;
    uint pad2;
};
ConstantBuffer<ShrinkPush> push : register(b0);

[shader("vertex")]
float4 vs_main(uint vertex_id : SV_VertexID) : SV_Position {
    const float2 p = float2(vertex_id == 1 ? 3.0 : -1.0, vertex_id == 2 ? 3.0 : -1.0);
    return float4(p, 0.0, 1.0);
}

// How much of [from, to) the texel [at, at + 1) covers.
float shrink_share(float from, float to, float at) {
    return saturate(min(to, at + 1.0) - max(from, at));
}

[shader("pixel")]
float4 ps_main(float4 position : SV_Position) : SV_Target {
    const float2 from = floor(position.xy) * push.scale;
    const float2 to = from + push.scale;
    const float2 first = floor(from);
    const int2 last = int2(push.size) - int2(1, 1);
    Texture2D<float4> source = read_texture(push.color);
    float3 sum = float3(0.0, 0.0, 0.0);
    float weight = 0.0;
    [unroll] for (int j = 0; j < 3; ++j) {
        const float y = first.y + float(j);
        const float wy = shrink_share(from.y, to.y, y);
        [unroll] for (int i = 0; i < 3; ++i) {
            const float x = first.x + float(i);
            const float w = wy * shrink_share(from.x, to.x, x);
            const int2 texel = min(int2(int(x), int(y)), last);
            sum += source.Load(int3(texel, 0)).rgb * w;
            weight += w;
        }
    }
    return float4(sum / max(weight, 1e-6), 1.0);
}
