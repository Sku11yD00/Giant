// Skin's light spread (src/subsurface.h): the separable kernel from the
// measured skin profile.
#include "subsurface.h"

#include "giant_test.h"

#include "giant/rhi/rhi.h"

#include <cmath>
#include <limits>

using namespace giant;
using namespace giant::render;

GIANT_TEST("subsurface: skin's kernel keeps the light, the middle first, red spreading furthest") {
    const SubsurfaceKernel k = subsurface_kernel();
    float sum[3] = {};
    float spread[3] = {}; // second moments
    for (const SubsurfaceTap& t : k) {
        GIANT_CHECK(std::isfinite(t.offset) && t.offset >= -1.0f && t.offset <= 1.0f);
        for (u32 c = 0; c < 3; ++c) {
            GIANT_CHECK(std::isfinite(t.weight[c]) && t.weight[c] >= 0.0f);
            sum[c] += t.weight[c];
            spread[c] += t.weight[c] * t.offset * t.offset;
        }
    }
    for (u32 c = 0; c < 3; ++c) GIANT_CHECK(std::fabs(sum[c] - 1.0f) < 1e-4f); // nothing gained or lost
    GIANT_CHECK(spread[0] > spread[1] && spread[1] > spread[2]);
    // The middle tap first, the heaviest; the rest in order, mirrored.
    GIANT_CHECK(k[0].offset == 0.0f);
    for (u32 i = 1; i < k_subsurface_taps; ++i) {
        for (u32 c = 0; c < 3; ++c) GIANT_CHECK(k[i].weight[c] <= k[0].weight[c]);
        if (i + 1 < k_subsurface_taps) GIANT_CHECK(k[i].offset < k[i + 1].offset);
        const SubsurfaceTap& mirror = k[k_subsurface_taps - i];
        GIANT_CHECK(std::fabs(k[i].offset + mirror.offset) < 1e-6f);
        for (u32 c = 0; c < 3; ++c) GIANT_CHECK(std::fabs(k[i].weight[c] - mirror.weight[c]) < 1e-6f);
    }
    GIANT_CHECK(k[1].offset == -1.0f && k[k_subsurface_taps - 1].offset == 1.0f);
    // The profile: falls away from the middle, red's slowest.
    const auto at0 = skin_profile(0.0f);
    const auto at2 = skin_profile(2.0f);
    for (u32 c = 0; c < 3; ++c) GIANT_CHECK(at2[c] < at0[c]);
    GIANT_CHECK(at2[0] / at0[0] > at2[1] / at0[1] && at2[1] / at0[1] >= at2[2] / at0[2]);
    GIANT_CHECK(std::fabs(skin_profile_reach_mm() - 3.0f * std::sqrt(7.41f)) < 1e-4f);
}

GIANT_TEST("subsurface: light through skin: all at none, less the deeper, red getting through furthest") {
    const std::array<float, 3> none = skin_transmittance(0.0f);
    for (u32 c = 0; c < 3; ++c) GIANT_CHECK(std::fabs(none[c] - 1.0f) < 1e-3f);
    std::array<float, 3> before = none;
    for (float mm = 0.25f; mm <= 8.0f; mm += 0.25f) {
        const std::array<float, 3> t = skin_transmittance(mm);
        for (u32 c = 0; c < 3; ++c) {
            GIANT_CHECK(t[c] <= before[c] + 1e-6f && t[c] >= 0.0f);
        }
        GIANT_CHECK(t[0] >= t[1] && t[1] >= t[2]);
        before = t;
    }
    // A millimetre lets some red through and almost no blue; ten, none.
    const std::array<float, 3> one = skin_transmittance(1.0f);
    GIANT_CHECK(one[0] > 0.25f && one[2] < 0.01f);
    const std::array<float, 3> ten = skin_transmittance(10.0f);
    GIANT_CHECK(ten[0] < 0.01f);
}

GIANT_TEST("subsurface: a skin look for the GPU keeps its values in range, its detail map's descriptor as given") {
    SkinLook look;
    const gpu::SkinLook plain = to_gpu(look, 7u);
    GIANT_CHECK(plain.scatter == 0.012f && plain.strength == 1.0f && plain.lobe_mix == 0.15f);
    GIANT_CHECK(plain.lobe_roughness[0] == 0.75f && plain.lobe_roughness[1] == 1.3f);
    GIANT_CHECK(plain.transmission == 1.0f && plain.detail_normal == 7u && plain.detail_tiling == 16.0f);
    look.scatter = 5.0f;
    look.strength = -1.0f;
    look.lobe_roughness[0] = 0.0f;
    look.lobe_roughness[1] = 99.0f;
    look.lobe_mix = 2.0f;
    look.transmission = 3.0f;
    look.detail_tiling = 0.0f;
    const gpu::SkinLook kept = to_gpu(look, rhi::k_no_descriptor);
    GIANT_CHECK(kept.scatter == 0.1f && kept.strength == 0.0f && kept.lobe_mix == 1.0f);
    GIANT_CHECK(kept.lobe_roughness[0] == 0.1f && kept.lobe_roughness[1] == 4.0f);
    GIANT_CHECK(kept.transmission == 1.0f && kept.detail_tiling == 1.0f && kept.detail_normal == rhi::k_no_descriptor);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    look.scatter = nan;
    look.transmission = nan;
    look.detail_tiling = std::numeric_limits<float>::infinity();
    const gpu::SkinLook fixed = to_gpu(look, 3u);
    GIANT_CHECK(fixed.scatter == 0.012f && fixed.transmission == 1.0f && fixed.detail_tiling == 16.0f);
    look.transmission = -0.5f;
    look.detail_tiling = 1000.0f;
    const gpu::SkinLook low = to_gpu(look, 3u);
    GIANT_CHECK(low.transmission == 0.0f && low.detail_tiling == 256.0f);
}
