// Builds the hierarchical depth buffer (HZB) from scene depth: each texel
// holds the farthest depth (reverse-Z: the smallest value) of its area.
// See common.slang for the buffer layout.
#include "common.slang"

struct HzbPush {
    uint depth;      // read descriptor of the depth texture (cs_first)
    uint hzb;        // write descriptor
    uint width;      // depth texture size
    uint height;
    uint hzb_width;  // mip 0
    uint hzb_height;
    uint mip;        // the mip this dispatch writes (cs_down: 1 and up)
    uint pad;
};
ConstantBuffer<HzbPush> push : register(b0);

// Mip 0: the farthest depth over each texel's footprint (1 to 3 pixels a
// side, since mip 0 is between half and all of the viewport's size).
[shader("compute")]
[numthreads(8, 8, 1)]
void cs_first(uint3 id : SV_DispatchThreadID) {
    if (id.x >= push.hzb_width || id.y >= push.hzb_height) {
        return;
    }
    const uint x0 = id.x * push.width / push.hzb_width;
    const uint x1 = min(((id.x + 1) * push.width + push.hzb_width - 1) / push.hzb_width, push.width);
    const uint y0 = id.y * push.height / push.hzb_height;
    const uint y1 = min(((id.y + 1) * push.height + push.hzb_height - 1) / push.hzb_height, push.height);
    Texture2D<float> depth = read_texture_float(push.depth);
    float farthest = 1.0;
    for (uint y = y0; y < y1; ++y) {
        for (uint x = x0; x < x1; ++x) {
            farthest = min(farthest, depth.Load(int3(int(x), int(y), 0)));
        }
    }
    write_buffer(push.hzb).Store((id.y * push.hzb_width + id.x) * 4, asuint(farthest));
}

// Mip n from mip n - 1: the farthest of the 2x2 texels below.
[shader("compute")]
[numthreads(8, 8, 1)]
void cs_down(uint3 id : SV_DispatchThreadID) {
    const uint2 size = uint2(max(push.hzb_width >> push.mip, 1u), max(push.hzb_height >> push.mip, 1u));
    if (id.x >= size.x || id.y >= size.y) {
        return;
    }
    const uint2 below = uint2(max(push.hzb_width >> (push.mip - 1), 1u),
                              max(push.hzb_height >> (push.mip - 1), 1u));
    const uint from = hzb_mip_offset(push.hzb_width, push.hzb_height, push.mip - 1);
    const uint to = hzb_mip_offset(push.hzb_width, push.hzb_height, push.mip);
    RWByteAddressBuffer hzb = write_buffer(push.hzb);
    float farthest = 1.0;
    for (uint dy = 0; dy < 2; ++dy) {
        for (uint dx = 0; dx < 2; ++dx) {
            const uint2 p = min(id.xy * 2 + uint2(dx, dy), below - 1);
            farthest = min(farthest, asfloat(hzb.Load((from + p.y * below.x + p.x) * 4)));
        }
    }
    hzb.Store((to + id.y * size.x + id.x) * 4, asuint(farthest));
}
