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
    uint shade;         // rhi::UiShade: 0 plain, 1 signed distance
    float outline;      // sdf: distance units the outline reaches out
    uint outline_color; // sdf: RGBA8
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
    const float4 t = read_texture(push.texture).Sample(get_sampler(k_sampler_linear_clamp), i.uv);
    if (push.shade != 1) return i.color * t;
    // Distance to coverage over about a pixel on screen, whatever the size drawn.
    const float d = t.a;
    const float w = max(fwidth(d), 1e-4);
    const float fill = saturate((d - 0.5) / w + 0.5);
    if (push.outline <= 0.0) return float4(i.color.rgb, i.color.a * fill);
    // (At most a pixel short of the field's reach: else the glyph's box shows.)
    const float outer = saturate((d - (0.5 - min(push.outline, 0.5 - w))) / w + 0.5);
    const float4 o = unpack_rgba8(push.outline_color);
    const float a_fill = i.color.a * fill;
    const float a_out = o.a * outer * (1.0 - a_fill);
    const float a = a_fill + a_out;
    if (a <= 0.0) return float4(0.0, 0.0, 0.0, 0.0);
    return float4((i.color.rgb * a_fill + o.rgb * a_out) / a, a);
}
