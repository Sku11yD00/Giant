// After shading (and the screen reflections), lit views: the light before
// exposure (rgba16_float; a: 1 to expose and tone map, 0 shown as it is)
// is measured, spread round bright things, exposed and tone mapped into
// the sRGB image anti-aliasing then smooths.
//
// - cs_histogram_clear, cs_histogram: the lit pixels' brightness counted
//   into src/exposure.h's bins, for auto exposure (read back on the CPU).
// - cs_bloom_down: the light halved in size (Jimenez's 13 taps; on the
//   first, from the full image, each tap's light held to k_bloom_cap times
//   white once exposed, so a lone blazing pixel cannot flicker the whole
//   bloom), six times.
// - cs_bloom_up: back up, each level plus the one below spread by a tent.
// - ps_main: bloom_strength of each pixel's light given over to the bloom
//   (the levels' average), exposure, the tone mapper.
//
// Nothing from earlier frames. Layout: PostPush in src/gpu_layout.h.
#include "common.slang"
#include "tone_map.slang"

struct PostPush {
    uint frame;
    uint frame_offset;
    uint source; // read: the light (histogram, the first level down, ps_main) or the level before
    uint add;    // read: cs_bloom_up, the level down at this size; ps_main, the bloom's top level
    uint target; // write: the level made, or the histogram's bins
    uint width;  // of what is made (the histogram: the image)
    uint height;
    uint flags;  // bit 0: the first level down; bit 1: AgX; bit 2: bloom
    float strength; // bloom
    float levels;   // bloom: how many levels make its top
    uint pad[2];
};
ConstantBuffer<PostPush> push : register(b0);

static const uint k_bins = 64;          // src/exposure.h's k_exposure_bins
static const float k_bins_low = -16.0;  // k_exposure_low
static const float k_bins_high = 8.0;   // k_exposure_high
static const float3 k_luminance = float3(0.2126, 0.7152, 0.0722);

// Bin 0: black and under 2^k_bins_low; bins 1 to k_bins - 1 even in stops.
uint histogram_bin(float luminance) {
    if (!(luminance > exp2(k_bins_low))) {
        return 0u;
    }
    const float at = (log2(luminance) - k_bins_low) / (k_bins_high - k_bins_low) * float(k_bins - 1u);
    return 1u + uint(clamp(at, 0.0, float(k_bins - 2u)));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_histogram_clear(uint3 id : SV_DispatchThreadID) {
    if (id.x < k_bins) {
        write_buffer(push.target).Store(id.x * 4, 0u);
    }
}

groupshared uint g_bins[k_bins];

[shader("compute")]
[numthreads(16, 16, 1)]
void cs_histogram(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex) {
    if (index < k_bins) {
        g_bins[index] = 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    if (id.x < push.width && id.y < push.height) {
        const float4 c = read_texture(push.source).Load(int3(id.xy, 0));
        if (c.a > 0.0) {
            uint ignored;
            InterlockedAdd(g_bins[histogram_bin(dot(max(c.rgb, float3(0.0, 0.0, 0.0)), k_luminance))], 1u, ignored);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (index < k_bins && g_bins[index] != 0u) {
        uint ignored;
        write_buffer(push.target).InterlockedAdd(index * 4, g_bins[index], ignored);
    }
}

static const float k_bloom_cap = 64.0; // times white, exposed: the brightest a tap of the first level counts

// The light at `uv`, kept finite: on the first level, only the lit
// pixels' (the background and debug views shown as they are spread
// nothing), held to k_bloom_cap once exposed.
float3 bloom_tap(float2 uv, bool first, float exposure) {
    const float4 c = read_texture(push.source).SampleLevel(get_sampler(k_sampler_linear_clamp), uv, 0.0);
    float3 rgb = clamp(c.rgb, float3(0.0, 0.0, 0.0), float3(60000.0, 60000.0, 60000.0));
    rgb = select(rgb == rgb, rgb, float3(0.0, 0.0, 0.0)); // NaN to black
    if (first) {
        const float shown = dot(rgb, k_luminance) * exposure;
        rgb *= saturate(c.a) * (shown > k_bloom_cap ? k_bloom_cap / shown : 1.0);
    }
    return rgb;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_bloom_down(uint3 id : SV_DispatchThreadID) {
    if (id.x >= push.width || id.y >= push.height) {
        return;
    }
    uint sw;
    uint sh;
    read_texture(push.source).GetDimensions(sw, sh);
    const float2 t = 1.0 / float2(max(sw, 1u), max(sh, 1u));
    const float2 uv = (float2(id.xy) + 0.5) / float2(push.width, push.height);
    const bool first = (push.flags & 1u) != 0u;
    const float exposure = first ? max(load_frame(push.frame, push.frame_offset).exposure, 0.0) : 1.0;
    const float3 a = bloom_tap(uv + t * float2(-2.0, -2.0), first, exposure);
    const float3 b = bloom_tap(uv + t * float2(0.0, -2.0), first, exposure);
    const float3 c = bloom_tap(uv + t * float2(2.0, -2.0), first, exposure);
    const float3 d = bloom_tap(uv + t * float2(-2.0, 0.0), first, exposure);
    const float3 e = bloom_tap(uv, first, exposure);
    const float3 f = bloom_tap(uv + t * float2(2.0, 0.0), first, exposure);
    const float3 g = bloom_tap(uv + t * float2(-2.0, 2.0), first, exposure);
    const float3 h = bloom_tap(uv + t * float2(0.0, 2.0), first, exposure);
    const float3 i = bloom_tap(uv + t * float2(2.0, 2.0), first, exposure);
    const float3 j = bloom_tap(uv + t * float2(-1.0, -1.0), first, exposure);
    const float3 k = bloom_tap(uv + t * float2(1.0, -1.0), first, exposure);
    const float3 l = bloom_tap(uv + t * float2(-1.0, 1.0), first, exposure);
    const float3 m = bloom_tap(uv + t * float2(1.0, 1.0), first, exposure);
    const float3 down = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
    write_texture(push.target)[id.xy] = float4(down, 1.0);
}

// The level at `uv` spread by a 3 by 3 tent of its own texels.
float3 tent(uint descriptor, float2 uv) {
    Texture2D<float4> s = read_texture(descriptor);
    uint w;
    uint h;
    s.GetDimensions(w, h);
    const float2 t = 1.0 / float2(max(w, 1u), max(h, 1u));
    const SamplerState linear = get_sampler(k_sampler_linear_clamp);
    float3 sum = s.SampleLevel(linear, uv, 0.0).rgb * 4.0;
    sum += (s.SampleLevel(linear, uv + float2(-t.x, 0.0), 0.0).rgb + s.SampleLevel(linear, uv + float2(t.x, 0.0), 0.0).rgb +
            s.SampleLevel(linear, uv + float2(0.0, -t.y), 0.0).rgb + s.SampleLevel(linear, uv + float2(0.0, t.y), 0.0).rgb) *
           2.0;
    sum += s.SampleLevel(linear, uv + float2(-t.x, -t.y), 0.0).rgb + s.SampleLevel(linear, uv + float2(t.x, -t.y), 0.0).rgb +
           s.SampleLevel(linear, uv + float2(-t.x, t.y), 0.0).rgb + s.SampleLevel(linear, uv + float2(t.x, t.y), 0.0).rgb;
    return sum * (1.0 / 16.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_bloom_up(uint3 id : SV_DispatchThreadID) {
    if (id.x >= push.width || id.y >= push.height) {
        return;
    }
    const float2 uv = (float2(id.xy) + 0.5) / float2(push.width, push.height);
    const float3 here = read_texture(push.add).Load(int3(id.xy, 0)).rgb;
    write_texture(push.target)[id.xy] = float4(here + tent(push.source, uv), 1.0);
}

[shader("vertex")]
float4 vs_main(uint vertex_id : SV_VertexID) : SV_Position {
    const float2 p = float2(vertex_id == 1 ? 3.0 : -1.0, vertex_id == 2 ? 3.0 : -1.0);
    return float4(p, 0.0, 1.0);
}

[shader("pixel")]
float4 ps_main(float4 position : SV_Position) : SV_Target {
    const int2 pixel = int2(position.xy);
    const float4 c = read_texture(push.source).Load(int3(pixel, 0));
    if (c.a <= 0.0) {
        return float4(c.rgb, 1.0); // the background and debug views, as they are
    }
    const Frame f = load_frame(push.frame, push.frame_offset);
    float3 light = max(c.rgb, float3(0.0, 0.0, 0.0));
    if ((push.flags & 4u) != 0u) {
        const float2 uv = position.xy / float2(max(f.width, 1.0), max(f.height, 1.0));
        const float3 bloom = tent(push.add, uv) / max(push.levels, 1.0);
        light = lerp(light, bloom, saturate(push.strength));
    }
    light *= f.exposure;
    const float3 shown = (push.flags & 2u) != 0u ? tone_map_agx(light) : tone_map(light);
    return float4(shown, 1.0);
}
