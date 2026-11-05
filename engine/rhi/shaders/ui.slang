// 2D UI: textured, vertex-coloured triangles in pixel coordinates, blended
// in gamma space. Indices and vertices are pulled from this frame's
// transient memory (no input assembler).
#include "giant/bindless.slang"

struct UiPush {
    float2 scale;       // pixels to clip space: (2 / width, -2 / height)
    float2 offset;      // (-1, 1)
    uint vertices;      // descriptor of the vertex memory
    uint vertex_base;   // byte offset of vertex 0 (the command's vertex offset applied)
    uint indices;       // descriptor of the index memory
    uint index_base;    // byte offset of the command's first index
    uint texture;       // descriptor
    uint pad0;
    uint pad1;
    uint pad2;
};
ConstantBuffer<UiPush> push : register(b0);

static const uint k_vertex_bytes = 20; // rhi::UiVertex

struct VsOut {
    float4 position : SV_Position;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VsOut vs_main(uint vertex_id : SV_VertexID) {
    const uint index = read_buffer(push.indices).Load(push.index_base + vertex_id * 4);
    ByteAddressBuffer vb = read_buffer(push.vertices);
    const uint at = push.vertex_base + index * k_vertex_bytes;
    const float2 pos = asfloat(vb.Load2(at));
    VsOut o;
    o.position = float4(pos * push.scale + push.offset, 0.0, 1.0);
    o.uv = asfloat(vb.Load2(at + 8));
    o.color = unpack_rgba8(vb.Load(at + 16));
    return o;
}

[shader("pixel")]
float4 ps_main(VsOut i) : SV_Target {
    return i.color * read_texture(push.texture).Sample(get_sampler(k_sampler_linear_clamp), i.uv);
}
