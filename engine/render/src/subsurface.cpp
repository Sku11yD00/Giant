#include "subsurface.h"

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
