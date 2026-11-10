// The split-sum table of the specular BRDF (photoreal part 4a), made on
// the CPU once at init and read by resolve.slang.
//
// For a GGX surface (height-correlated Smith visibility, Schlick Fresnel,
// as resolve.slang's d_ggx/v_smith/f_schlick), the light it reflects from
// a white surround, integrated over the hemisphere, is f0 * a + b: a and b
// depend only on n.v and the roughness (Karis 2013). a + b is how much a
// single bounce off the microfacets keeps (Ess); the rest is lost to
// light that bounces again, which multi-scatter compensation puts back
// (Fdez-Aguera 2019; Filament for the sun).
//
// Layout: k_dfg_size x k_dfg_size float2 (a, b), roughness rows of n.v
// columns, both from 0 to 1 at the grid points (n.v = column / (size - 1),
// roughness = row / (size - 1), perceptual: alpha = roughness^2).
#pragma once

#include "giant/core/types.h"

#include <vector>

namespace giant::render {

inline constexpr u32 k_dfg_size = 32;
inline constexpr float k_dfg_min_roughness = 0.045f; // resolve.slang's clamp

// size * size * 2 floats; `samples` importance-sampled directions a point.
std::vector<float> make_dfg_table(u32 size = k_dfg_size, u32 samples = 512);

// One point of it: (a, b) for n.v and perceptual roughness.
void dfg_point(float n_dot_v, float roughness, u32 samples, float& a, float& b);

} // namespace giant::render
