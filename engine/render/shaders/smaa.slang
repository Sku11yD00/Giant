// SMAA 1x: edge anti-aliasing from the finished image alone, so it is as
// sharp in motion as standing still (no history, no smear). A port of the
// reference SMAA (Jimenez et al., "SMAA: Enhanced Subpixel Morphological
// Antialiasing", 2012; github.com/iryoku/smaa, MIT licence) at its High
// preset, without diagonal detection:
//   1. edges: where luma jumps between neighbours (with local contrast
//      adaptation, so a strong edge masks weaker ones beside it);
//   2. weights: for each edge, how far it runs each way and how it ends,
//      looked up in the precomputed area texture;
//   3. blending: each pixel mixes with its neighbour by those weights.
// Every sampler is linear and clamped; the searches lean on bilinear
// filtering to read two edges in one fetch.
#include "giant/bindless.slang"

struct SmaaPush {
    float4 metrics; // 1 / width, 1 / height, width, height
    uint color;     // read descriptor: the shaded image (sRGB, read as linear)
    uint edges;     // the edges pass's output
    uint weights;   // the weights pass's output
    uint area;      // area texture (rg in rgba8)
    uint search;    // search texture (r in rgba8)
    uint pad0;
    uint pad1;
    uint pad2;
};
ConstantBuffer<SmaaPush> push : register(b0);

static const float k_threshold = 0.1;
static const float k_max_search_steps = 16.0;
static const float k_corner_rounding = 0.25;
static const float k_local_contrast = 2.0;
static const float k_area_max_distance = 16.0;
static const float2 k_area_pixel = float2(1.0 / 160.0, 1.0 / 560.0);
static const float2 k_search_size = float2(66.0, 33.0);
static const float2 k_search_packed = float2(64.0, 16.0);

[shader("vertex")]
float4 vs_main(uint vertex_id : SV_VertexID) : SV_Position {
    const float2 p = float2(vertex_id == 1 ? 3.0 : -1.0, vertex_id == 2 ? 3.0 : -1.0);
    return float4(p, 0.0, 1.0);
}

float4 sample_zero(uint texture, float2 coord) {
    return read_texture(texture).SampleLevel(get_sampler(k_sampler_linear_clamp), coord, 0.0);
}

float4 sample_offset(uint texture, float2 coord, int2 offset) {
    return sample_zero(texture, coord + float2(offset) * push.metrics.xy);
}

// Perceptual luma: the image is read as linear light; its square root is
// close to the gamma-encoded value SMAA's threshold is tuned for.
float luma(float2 coord) {
    const float3 c = read_texture(push.color).SampleLevel(get_sampler(k_sampler_point_clamp), coord, 0.0).rgb;
    return sqrt(max(dot(c, float3(0.2126, 0.7152, 0.0722)), 0.0));
}

// --- 1. Edges ---------------------------------------------------------------

[shader("pixel")]
float4 ps_edges(float4 position : SV_Position) : SV_Target {
    const float2 uv = position.xy * push.metrics.xy;
    const float4 o0 = uv.xyxy + push.metrics.xyxy * float4(-1.0, 0.0, 0.0, -1.0);
    const float4 o1 = uv.xyxy + push.metrics.xyxy * float4(1.0, 0.0, 0.0, 1.0);
    const float4 o2 = uv.xyxy + push.metrics.xyxy * float4(-2.0, 0.0, 0.0, -2.0);
    const float l = luma(uv);
    const float l_left = luma(o0.xy);
    const float l_top = luma(o0.zw);
    float4 delta;
    delta.xy = abs(l - float2(l_left, l_top));
    float2 edges = step(float2(k_threshold, k_threshold), delta.xy);
    if (dot(edges, float2(1.0, 1.0)) == 0.0) {
        discard;
    }
    delta.zw = abs(l - float2(luma(o1.xy), luma(o1.zw)));
    float2 max_delta = max(delta.xy, delta.zw);
    delta.zw = abs(float2(l_left, l_top) - float2(luma(o2.xy), luma(o2.zw)));
    max_delta = max(max_delta, delta.zw);
    const float final_delta = max(max_delta.x, max_delta.y);
    edges *= step(float2(final_delta, final_delta), k_local_contrast * delta.xy);
    return float4(edges, 0.0, 0.0);
}

// --- 2. Weights -------------------------------------------------------------

float search_length(float2 e, float offset) {
    float2 scale = k_search_size * float2(0.5, -1.0);
    float2 bias = k_search_size * float2(offset, 1.0);
    scale += float2(-1.0, 1.0);
    bias += float2(0.5, -0.5);
    scale /= k_search_packed;
    bias /= k_search_packed;
    return sample_zero(push.search, scale * e + bias).r;
}

float search_x_left(float2 coord, float end) {
    float2 e = float2(0.0, 1.0);
    while (coord.x > end && e.g > 0.8281 && e.r == 0.0) {
        e = sample_zero(push.edges, coord).rg;
        coord -= float2(2.0, 0.0) * push.metrics.xy;
    }
    const float offset = -(255.0 / 127.0) * search_length(e, 0.0) + 3.25;
    return push.metrics.x * offset + coord.x;
}

float search_x_right(float2 coord, float end) {
    float2 e = float2(0.0, 1.0);
    while (coord.x < end && e.g > 0.8281 && e.r == 0.0) {
        e = sample_zero(push.edges, coord).rg;
        coord += float2(2.0, 0.0) * push.metrics.xy;
    }
    const float offset = -(255.0 / 127.0) * search_length(e, 0.5) + 3.25;
    return -push.metrics.x * offset + coord.x;
}

float search_y_up(float2 coord, float end) {
    float2 e = float2(1.0, 0.0);
    while (coord.y > end && e.r > 0.8281 && e.g == 0.0) {
        e = sample_zero(push.edges, coord).rg;
        coord -= float2(0.0, 2.0) * push.metrics.xy;
    }
    const float offset = -(255.0 / 127.0) * search_length(e.gr, 0.0) + 3.25;
    return push.metrics.y * offset + coord.y;
}

float search_y_down(float2 coord, float end) {
    float2 e = float2(1.0, 0.0);
    while (coord.y < end && e.r > 0.8281 && e.g == 0.0) {
        e = sample_zero(push.edges, coord).rg;
        coord += float2(0.0, 2.0) * push.metrics.xy;
    }
    const float offset = -(255.0 / 127.0) * search_length(e.gr, 0.5) + 3.25;
    return -push.metrics.y * offset + coord.y;
}

float2 area(float2 dist, float e1, float e2) {
    float2 coord = float2(k_area_max_distance, k_area_max_distance) * round(4.0 * float2(e1, e2)) + dist;
    coord = k_area_pixel * coord + 0.5 * k_area_pixel;
    // SMAA 1x reads subtexture 0 (the others serve its temporal modes).
    return sample_zero(push.area, coord).rg;
}

void corner_horizontal(inout float2 weights, float4 coord, float2 d) {
    const float2 left_right = step(d.xy, d.yx);
    float2 rounding = (1.0 - k_corner_rounding) * left_right;
    rounding /= left_right.x + left_right.y;
    float2 factor = float2(1.0, 1.0);
    factor.x -= rounding.x * sample_offset(push.edges, coord.xy, int2(0, 1)).r;
    factor.x -= rounding.y * sample_offset(push.edges, coord.zw, int2(1, 1)).r;
    factor.y -= rounding.x * sample_offset(push.edges, coord.xy, int2(0, -2)).r;
    factor.y -= rounding.y * sample_offset(push.edges, coord.zw, int2(1, -2)).r;
    weights *= saturate(factor);
}

void corner_vertical(inout float2 weights, float4 coord, float2 d) {
    const float2 left_right = step(d.xy, d.yx);
    float2 rounding = (1.0 - k_corner_rounding) * left_right;
    rounding /= left_right.x + left_right.y;
    float2 factor = float2(1.0, 1.0);
    factor.x -= rounding.x * sample_offset(push.edges, coord.xy, int2(1, 0)).g;
    factor.x -= rounding.y * sample_offset(push.edges, coord.zw, int2(1, 1)).g;
    factor.y -= rounding.x * sample_offset(push.edges, coord.xy, int2(-2, 0)).g;
    factor.y -= rounding.y * sample_offset(push.edges, coord.zw, int2(-2, 1)).g;
    weights *= saturate(factor);
}

[shader("pixel")]
float4 ps_weights(float4 position : SV_Position) : SV_Target {
    const float2 uv = position.xy * push.metrics.xy;
    const float2 pixcoord = uv * push.metrics.zw;
    const float4 o0 = uv.xyxy + push.metrics.xyxy * float4(-0.25, -0.125, 1.25, -0.125);
    const float4 o1 = uv.xyxy + push.metrics.xyxy * float4(-0.125, -0.25, -0.125, 1.25);
    const float4 o2 = float4(o0.xz, o1.yw) + push.metrics.xxyy * float4(-2.0, 2.0, -2.0, 2.0) * k_max_search_steps;

    float4 weights = float4(0.0, 0.0, 0.0, 0.0);
    const float2 e = sample_zero(push.edges, uv).rg;
    [branch]
    if (e.g > 0.0) { // an edge on the pixel's top
        float3 coords;
        coords.x = search_x_left(o0.xy, o2.x);
        coords.y = o1.y;
        float2 d;
        d.x = coords.x;
        const float e1 = sample_zero(push.edges, coords.xy).r;
        coords.z = search_x_right(o0.zw, o2.y);
        d.y = coords.z;
        d = abs(round(push.metrics.zz * d - pixcoord.xx));
        const float e2 = sample_offset(push.edges, coords.zy, int2(1, 0)).r;
        weights.rg = area(sqrt(d), e1, e2);
        coords.y = uv.y;
        corner_horizontal(weights.rg, coords.xyzy, d);
    }
    [branch]
    if (e.r > 0.0) { // an edge on the pixel's left
        float3 coords;
        coords.y = search_y_up(o1.xy, o2.z);
        coords.x = o0.x;
        float2 d;
        d.x = coords.y;
        const float e1 = sample_zero(push.edges, coords.xy).g;
        coords.z = search_y_down(o1.zw, o2.w);
        d.y = coords.z;
        d = abs(round(push.metrics.ww * d - pixcoord.yy));
        const float e2 = sample_offset(push.edges, coords.xz, int2(0, 1)).g;
        weights.ba = area(sqrt(d), e1, e2);
        coords.x = uv.x;
        corner_vertical(weights.ba, coords.xyxz, d);
    }
    return weights;
}

// --- 3. Blending ------------------------------------------------------------

[shader("pixel")]
float4 ps_blend(float4 position : SV_Position) : SV_Target {
    const float2 uv = position.xy * push.metrics.xy;
    const float4 offset = uv.xyxy + push.metrics.xyxy * float4(1.0, 0.0, 0.0, 1.0);
    float4 a;
    a.x = sample_zero(push.weights, offset.xy).a; // right
    a.y = sample_zero(push.weights, offset.zw).g; // below
    a.wz = sample_zero(push.weights, uv).xz;      // this pixel: top, left
    if (dot(a, float4(1.0, 1.0, 1.0, 1.0)) < 1e-5) {
        return sample_zero(push.color, uv);
    }
    const bool h = max(a.x, a.z) > max(a.y, a.w); // the horizontal edge is stronger
    const float4 blend_offset = h ? float4(a.x, 0.0, a.z, 0.0) : float4(0.0, a.y, 0.0, a.w);
    float2 blend_weight = h ? a.xz : a.yw;
    blend_weight /= dot(blend_weight, float2(1.0, 1.0));
    const float4 coord = blend_offset * float4(push.metrics.xy, -push.metrics.xy) + uv.xyxy;
    float4 color = blend_weight.x * sample_zero(push.color, coord.xy);
    color += blend_weight.y * sample_zero(push.color, coord.zw);
    return color;
}
