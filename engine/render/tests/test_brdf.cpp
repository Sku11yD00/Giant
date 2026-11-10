// The split-sum table of the specular BRDF (src/brdf.h).
#include "brdf.h"
#include "giant_test.h"

#include <cmath>

using namespace giant;
using namespace giant::render;

GIANT_TEST("brdf: a mirror keeps all the light; rougher surfaces keep less in one bounce") {
    float a = 0.0f;
    float b = 0.0f;
    for (const float n_dot_v : {0.3f, 0.6f, 1.0f}) {
        dfg_point(n_dot_v, 0.0f, 1024, a, b); // (clamped to resolve's 0.045)
        GIANT_CHECK(std::abs(a + b - 1.0f) < 0.02f);
    }
    // Head on, a smooth surface's Fresnel is f0 itself: b (the 1 - f0
    // part's weight) is near 0.
    dfg_point(1.0f, 0.0f, 1024, a, b);
    GIANT_CHECK(b < 0.01f && a > 0.98f);
    // Single scattering loses energy as roughness grows.
    float last = 2.0f;
    for (int i = 0; i <= 10; ++i) {
        dfg_point(1.0f, static_cast<float>(i) / 10.0f, 1024, a, b);
        const float ess = a + b;
        GIANT_CHECK(ess <= last + 1e-3f && ess > 0.0f && ess <= 1.001f);
        last = ess;
    }
    // Head on at alpha 1 a single bounce keeps about 0.31 (checked against
    // a brute-force sum over the hemisphere).
    GIANT_CHECK(std::abs(last - 0.307f) < 0.005f);
    dfg_point(0.05f, 0.5f, 1024, a, b);
    GIANT_CHECK(std::abs(a + b - 0.933f) < 0.005f);
    // At a grazing angle Fresnel rises: on a smooth surface b outweighs a.
    dfg_point(0.05f, 0.0f, 1024, a, b);
    GIANT_CHECK(b > a);
}

GIANT_TEST("brdf: the table holds the points it names and has converged") {
    const std::vector<float> table = make_dfg_table(8, 256);
    GIANT_CHECK(table.size() == 8u * 8u * 2u);
    float a = 0.0f;
    float b = 0.0f;
    dfg_point(3.0f / 7.0f, 5.0f / 7.0f, 256, a, b);
    GIANT_CHECK(table[(5 * 8 + 3) * 2] == a && table[(5 * 8 + 3) * 2 + 1] == b);
    for (const float n_dot_v : {0.0f, 0.1f, 0.5f, 0.9f}) {
        for (const float roughness : {0.2f, 0.6f, 1.0f}) {
            float a1 = 0.0f, b1 = 0.0f, a2 = 0.0f, b2 = 0.0f;
            dfg_point(n_dot_v, roughness, 512, a1, b1);
            dfg_point(n_dot_v, roughness, 8192, a2, b2);
            GIANT_CHECK(std::abs(a1 - a2) < 0.005f && std::abs(b1 - b2) < 0.005f);
        }
    }
}
