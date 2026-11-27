// Skin's light spread under its surface (characters part C4a): the
// separable blur of src/subsurface.h over the skin pixels resolve.slang
// set aside (rgb their diffuse light, past the fog; a their scatter in
// metres, 0 elsewhere). Across the screen first (into the second
// target), then down, adding the result to the rest of the pixels' light
// (radiance) into the first target, which then stands for radiance.
//
// A tap's reach on screen is its offset times the scatter at the pixel's
// distance (at most k_max_spread_pixels). A tap on a pixel that is not
// skin, or further in depth than the scatter, takes the middle's light
// instead (Jimenez 2015), so light never leaks across a silhouette or
// into what lies behind.
#include "common.slang"

struct SubsurfacePush {
    uint frame;
    uint frame_offset;
    uint source;   // read: rgb the light to spread, a the scatter in metres (0: not skin)
    uint depth;    // read: reverse-Z
    uint radiance; // read (direction 1): the rest of the light
    uint target;   // write
    uint kernel;   // float4 per tap: rgb weight, a offset (the middle tap first)
    uint kernel_offset;
    uint width;
    uint height;
    uint direction; // 0 across, 1 down
    uint taps;
};
ConstantBuffer<SubsurfacePush> push : register(b0);

static const float k_max_spread_pixels = 64.0;

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_spread(uint3 id : SV_DispatchThreadID) {
    if (id.x >= push.width || id.y >= push.height) {
        return;
    }
    Texture2D<float4> source = read_texture(push.source);
    RWTexture2D<float4> target = write_texture(push.target);
    const int2 px = int2(id.xy);
    const float4 middle = source.Load(int3(px, 0));
    const bool down = push.direction != 0u;
    float4 rest = float4(0.0, 0.0, 0.0, 0.0);
    if (down) {
        rest = read_texture(push.radiance).Load(int3(px, 0));
    }
    if (!(middle.a > 0.0)) {
        target[id.xy] = down ? rest : float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    const Frame f = load_frame(push.frame, push.frame_offset);
    Texture2D<float> depth = read_texture_float(push.depth);
    const float d = depth.Load(int3(px, 0));
    const float z = d > 0.0 ? f.near_plane / d : 1.0e6;
    // The scatter's reach in pixels here.
    const float reach = min(middle.a * f.p11 * float(f.height) / (2.0 * z), k_max_spread_pixels * shown_pixels(f));
    const int2 axis = down ? int2(0, 1) : int2(1, 0);
    const int2 size = int2(int(push.width), int(push.height));
    ByteAddressBuffer kernel = read_buffer(push.kernel);
    float3 sum = float3(0.0, 0.0, 0.0);
    [loop] for (uint i = 0; i < push.taps; ++i) {
        const float4 tap = asfloat(kernel.Load4(push.kernel_offset + i * 16u));
        float3 c = middle.rgb;
        if (i > 0u) {
            const int2 q = clamp(px + axis * int(round(tap.a * reach)), int2(0, 0), size - 1);
            const float4 there = source.Load(int3(q, 0));
            const float dq = depth.Load(int3(q, 0));
            const float zq = dq > 0.0 ? f.near_plane / dq : 1.0e6;
            if (there.a > 0.0) {
                // Further in depth than the scatter: the middle's instead.
                c = lerp(there.rgb, middle.rgb, saturate(abs(zq - z) / middle.a));
            }
        }
        sum += c * tap.rgb;
    }
    if (down) {
        target[id.xy] = float4(rest.rgb + sum, rest.a);
    } else {
        target[id.xy] = float4(sum, middle.a);
    }
}
