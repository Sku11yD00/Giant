#include "subsurface.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace giant::render {

namespace {

// d'Eon and Luebke's six Gaussians for skin: variances (mm^2) and each
// channel's weight.
constexpr float k_variance[6] = {0.0064f, 0.0484f, 0.187f, 0.567f, 1.99f, 7.41f};
constexpr float k_weight[6][3] = {{0.233f, 0.455f, 0.649f}, {0.100f, 0.336f, 0.344f}, {0.118f, 0.198f, 0.0f},
                                  {0.113f, 0.007f, 0.007f}, {0.358f, 0.004f, 0.0f}, {0.078f, 0.0f, 0.0f}};

float gaussian(float variance, float x) {
    return std::exp(-x * x / (2.0f * variance)) / std::sqrt(2.0f * std::numbers::pi_v<float> * variance);
}

float finite_in(float v, float lo, float hi, float fallback) {
    return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
}

float offset_at(u32 i) {
    const float t = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(k_subsurface_taps - 1);
    return t * std::fabs(t);
}

} // namespace

float skin_profile_reach_mm() { return 3.0f * std::sqrt(k_variance[5]); }

std::array<float, 3> skin_profile(float mm) {
    std::array<float, 3> out{};
    for (u32 g = 0; g < 6; ++g) {
        const float v = gaussian(k_variance[g], mm);
        for (u32 c = 0; c < 3; ++c) out[c] += k_weight[g][c] * v;
    }
    return out;
}

std::array<float, 3> skin_transmittance(float mm) {
    std::array<float, 3> out{};
    for (u32 g = 0; g < 6; ++g) {
        const float e = std::exp(-mm * mm / k_variance[g]);
        for (u32 c = 0; c < 3; ++c) out[c] += k_weight[g][c] * e;
    }
    return out;
}

gpu::SkinLook to_gpu(const SkinLook& look, u32 detail) {
    gpu::SkinLook out{};
    out.scatter = finite_in(look.scatter, 0.0005f, 0.1f, 0.012f);
    out.strength = finite_in(look.strength, 0.0f, 1.0f, 1.0f);
    out.lobe_roughness[0] = finite_in(look.lobe_roughness[0], 0.1f, 4.0f, 0.75f);
    out.lobe_roughness[1] = finite_in(look.lobe_roughness[1], 0.1f, 4.0f, 1.3f);
    out.lobe_mix = finite_in(look.lobe_mix, 0.0f, 1.0f, 0.15f);
    out.transmission = finite_in(look.transmission, 0.0f, 1.0f, 1.0f);
    out.detail_normal = detail;
    out.detail_tiling = finite_in(look.detail_tiling, 1.0f, 256.0f, 16.0f);
    return out;
}

gpu::EyeLook to_gpu(const EyeLook& look) {
    const EyeLook d;
    gpu::EyeLook out{};
    out.iris_radius = finite_in(look.iris_radius, 0.02f, 0.5f, d.iris_radius);
    out.pupil_radius = finite_in(look.pupil_radius, 0.0f, 0.95f, d.pupil_radius);
    // (Deeper, the iris would fold over itself at its near rim seen from the side.)
    out.iris_depth = finite_in(look.iris_depth, 0.0f, 0.45f, d.iris_depth);
    out.limbal = finite_in(look.limbal, 0.0f, 0.5f, d.limbal);
    out.iris_color[0] = finite_in(look.iris_color.x, 0.0f, 1.0f, d.iris_color.x);
    out.iris_color[1] = finite_in(look.iris_color.y, 0.0f, 1.0f, d.iris_color.y);
    out.iris_color[2] = finite_in(look.iris_color.z, 0.0f, 1.0f, d.iris_color.z);
    out.painted = look.painted ? 1.0f : 0.0f;
    out.limbal_darkness = finite_in(look.limbal_darkness, 0.0f, 1.0f, d.limbal_darkness);
    out.cornea_roughness = finite_in(look.cornea_roughness, 0.045f, 1.0f, d.cornea_roughness);
    out.bulge = finite_in(look.bulge, 0.0f, 1.0f, d.bulge);
    out.shade = finite_in(look.shade, 0.0f, 1.0f, d.shade);
    return out;
}

SubsurfaceKernel subsurface_kernel() {
    // Each tap stands for the stretch halfway to its neighbours; the
    // profile is integrated over it (a few samples), so the narrow
    // Gaussians (blue's, mostly) land in the middle tap whole.
    std::array<std::array<float, 3>, k_subsurface_taps> w{};
    const float reach = skin_profile_reach_mm();
    for (u32 i = 0; i < k_subsurface_taps; ++i) {
        const float lo = i == 0 ? -1.0f : 0.5f * (offset_at(i - 1) + offset_at(i));
        const float hi = i + 1 == k_subsurface_taps ? 1.0f : 0.5f * (offset_at(i) + offset_at(i + 1));
        constexpr u32 k_steps = 64;
        for (u32 s = 0; s < k_steps; ++s) {
            const float x = lo + (hi - lo) * (static_cast<float>(s) + 0.5f) / static_cast<float>(k_steps);
            const std::array<float, 3> p = skin_profile(x * reach);
            for (u32 c = 0; c < 3; ++c) w[i][c] += p[c] * (hi - lo) / static_cast<float>(k_steps);
        }
    }
    std::array<float, 3> sum{};
    for (const auto& t : w) {
        for (u32 c = 0; c < 3; ++c) sum[c] += t[c];
    }
    SubsurfaceKernel out{};
    // The middle tap first, then the rest in order.
    constexpr u32 middle = k_subsurface_taps / 2;
    u32 at = 0;
    const auto put = [&](u32 i) {
        SubsurfaceTap& t = out[at++];
        for (u32 c = 0; c < 3; ++c) t.weight[c] = w[i][c] / sum[c];
        t.offset = offset_at(i);
    };
    put(middle);
    for (u32 i = 0; i < k_subsurface_taps; ++i) {
        if (i != middle) put(i);
    }
    return out;
}

} // namespace giant::render
