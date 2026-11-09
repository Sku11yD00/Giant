// Exposure's last step: the tone mappers (RenderSettings::tone_mapper),
// for the finished image (post.slang) and the dust over a flat background
// (resolve.slang).
#pragma once

// Khronos PBR Neutral tone mapping.
float3 tone_map(float3 color) {
    const float start = 0.8 - 0.04;
    const float desaturation = 0.15;
    const float x = min(color.r, min(color.g, color.b));
    const float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    const float peak = max(color.r, max(color.g, color.b));
    if (peak < start) {
        return color;
    }
    const float d = 1.0 - start;
    const float new_peak = 1.0 - d * d / (peak + d - start);
    color *= new_peak / peak;
    const float g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
    return lerp(color, float3(new_peak, new_peak, new_peak), g);
}

// AgX (Troy Sobotka's), in Benjamin Wrensch's fitted form: the light
// moved a little towards the middle of the colours, encoded in stops from
// -12.5 to +4 around middle grey, a sigmoid over them, then back out to
// the screen's linear colours, so very bright colours go gently over to
// white instead of clipping in hue.
float3 agx_contrast(float3 x) {
    const float3 x2 = x * x;
    const float3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

float3 tone_map_agx(float3 color) {
    const float k_low = -12.47393;
    const float k_high = 4.026069;
    color = max(color, float3(0.0, 0.0, 0.0));
    float3 v = float3(dot(color, float3(0.842479062253094, 0.0784335999999992, 0.0792237451477643)),
                      dot(color, float3(0.0423282422610123, 0.878468636469772, 0.0791661274605434)),
                      dot(color, float3(0.0423756549057051, 0.0784336, 0.879142973793104)));
    v = clamp(log2(max(v, float3(1e-10, 1e-10, 1e-10))), float3(k_low, k_low, k_low), float3(k_high, k_high, k_high));
    v = agx_contrast((v - k_low) / (k_high - k_low));
    v = float3(dot(v, float3(1.19687900512017, -0.0980208811401368, -0.0990297440797205)),
               dot(v, float3(-0.0528968517574562, 1.15190312990417, -0.0989611768448433)),
               dot(v, float3(-0.0529716355144438, -0.0980434501171241, 1.15107367264116)));
    return pow(max(v, float3(0.0, 0.0, 0.0)), float3(2.2, 2.2, 2.2));
}
