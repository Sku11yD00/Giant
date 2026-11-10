// The split-sum table of the specular BRDF (src/brdf.h).
#include "brdf.h"

#include <algorithm>
#include <cmath>

namespace giant::render {

namespace {

constexpr double k_pi = 3.14159265358979323846;

// Radical inverse in base 2 (the Hammersley set's second coordinate).
double radical_inverse(u32 bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<double>(bits) * 2.3283064365386963e-10;
}

// resolve.slang's v_smith: height-correlated, with 1 / (4 n.l n.v).
double v_smith(double n_dot_v, double n_dot_l, double alpha) {
    const double a2 = alpha * alpha;
    const double gv = n_dot_l * std::sqrt(n_dot_v * n_dot_v * (1.0 - a2) + a2);
    const double gl = n_dot_v * std::sqrt(n_dot_l * n_dot_l * (1.0 - a2) + a2);
    return 0.5 / std::max(gv + gl, 1e-12);
}

} // namespace

void dfg_point(float n_dot_v_in, float roughness_in, u32 samples, float& a_out, float& b_out) {
    const double n_dot_v = std::clamp(static_cast<double>(n_dot_v_in), 1e-4, 1.0);
    const double roughness = std::clamp(static_cast<double>(roughness_in), double{k_dfg_min_roughness}, 1.0);
    const double alpha = roughness * roughness;
    // View in the x-z plane, normal +z.
    const double vx = std::sqrt(1.0 - n_dot_v * n_dot_v);
    const double vz = n_dot_v;
    samples = std::max(samples, 1u);
    double a = 0.0;
    double b = 0.0;
    // Directions from GGX's visible normals (Heitz 2018): low noise even
    // at grazing angles. Its weight for the BRDF times n.l is F G2 / G1(v).
    const double hx_v = alpha * vx; // the view stretched to alpha 1
    const double hz_v = vz;
    const double len = std::sqrt(hx_v * hx_v + hz_v * hz_v);
    const double sx = hx_v / len;
    const double sz = hz_v / len;
    // A frame about it: t1 = +y, t2 = s x t1.
    const double t2x = -sz;
    const double t2z = sx;
    const double g1_v = 2.0 * n_dot_v / (n_dot_v + std::sqrt(alpha * alpha + (1.0 - alpha * alpha) * n_dot_v * n_dot_v));
    for (u32 i = 0; i < samples; ++i) {
        const double u1 = (static_cast<double>(i) + 0.5) / samples;
        const double u2 = radical_inverse(i);
        const double r = std::sqrt(u1);
        const double phi = 2.0 * k_pi * u2;
        const double p1 = r * std::cos(phi);
        double p2 = r * std::sin(phi);
        const double blend = 0.5 * (1.0 + sz);
        p2 = (1.0 - blend) * std::sqrt(std::max(1.0 - p1 * p1, 0.0)) + blend * p2;
        const double p3 = std::sqrt(std::max(1.0 - p1 * p1 - p2 * p2, 0.0));
        // On the stretched hemisphere, then back to alpha.
        const double nx = p2 * t2x + p3 * sx;
        const double ny = p1;
        const double nz = p2 * t2z + p3 * sz;
        double hx = alpha * nx;
        double hy = alpha * ny;
        double hz = std::max(nz, 0.0);
        const double hl = std::sqrt(hx * hx + hy * hy + hz * hz);
        if (hl <= 0.0) continue;
        hx /= hl;
        hy /= hl;
        hz /= hl;
        const double v_dot_h = vx * hx + vz * hz;
        const double n_dot_l = 2.0 * v_dot_h * hz - vz; // l = reflect(-v, h)
        if (n_dot_l <= 0.0 || v_dot_h <= 0.0) continue;
        const double g = 4.0 * n_dot_l * n_dot_v * v_smith(n_dot_v, n_dot_l, alpha) / g1_v;
        const double fc = std::pow(1.0 - v_dot_h, 5.0);
        a += (1.0 - fc) * g;
        b += fc * g;
    }
    a_out = static_cast<float>(a / samples);
    b_out = static_cast<float>(b / samples);
}

std::vector<float> make_dfg_table(u32 size, u32 samples) {
    size = std::max(size, 2u);
    std::vector<float> table(usize{size} * size * 2);
    for (u32 row = 0; row < size; ++row) {
        for (u32 column = 0; column < size; ++column) {
            const float n_dot_v = static_cast<float>(column) / static_cast<float>(size - 1);
            const float roughness = static_cast<float>(row) / static_cast<float>(size - 1);
            const usize at = (usize{row} * size + column) * 2;
            dfg_point(n_dot_v, roughness, samples, table[at], table[at + 1]);
        }
    }
    return table;
}

} // namespace giant::render
