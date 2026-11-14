// Skin's light spread (src/subsurface.h): the separable kernel from the
// measured skin profile.
#include "subsurface.h"

#include "giant_test.h"

#include <cmath>

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
