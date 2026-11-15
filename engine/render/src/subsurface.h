// Skin's light spread under its surface (characters part C4a): the
// separable kernel subsurface.slang blurs skin pixels' diffuse light
// with, along each screen axis in turn (Jimenez et al. 2015, "Separable
// Subsurface Scattering").
//
// The profile is skin's measured diffusion (d'Eon and Luebke 2007, GPU
// Gems 3 ch. 14): a sum of six Gaussians, red reaching furthest and blue
// least. A Gaussian's 1D profile has its own variance, so each axis
// blurs by the same sum. Taps lie at offsets t * |t| for t evenly from -1
// to 1 (closer near the middle, where the profile changes fastest),
// in units of the profile's widest reach (three standard deviations of
// its widest Gaussian), which SkinLook::scatter sets in metres; each
// channel's weights sum to 1, the middle tap first.
//
// Light through thin parts (C4b, Jimenez et al. 2010, "Real-Time Realistic
// Skin Translucency"): the same Gaussians, each dimming with the depth of
// skin crossed, sum to how much of each channel gets through.
#pragma once

#include "giant/core/types.h"
#include "giant/render/renderer.h"
#include "gpu_layout.h"

#include <array>

namespace giant::render {

inline constexpr u32 k_subsurface_taps = 17;

struct SubsurfaceTap {
    float weight[3]; // red, green, blue
    float offset;    // in widest reaches (-1 to 1)
};
static_assert(sizeof(SubsurfaceTap) == 16);

using SubsurfaceKernel = std::array<SubsurfaceTap, k_subsurface_taps>;

// The kernel (the same every time).
SubsurfaceKernel subsurface_kernel();

// Skin's profile at `mm` millimetres from where light went in (1D: one
// axis of it), per channel.
std::array<float, 3> skin_profile(float mm);

// The profile's widest reach in millimetres (three standard deviations of
// its widest Gaussian): an offset of 1.
float skin_profile_reach_mm();

// How much of each channel passes through `mm` millimetres of skin (1 at
// 0, red furthest): resolve.slang's skin_transmittance.
std::array<float, 3> skin_transmittance(float mm);

// A skin look for the GPU, its values kept in range (and non-finite ones
// taken as the defaults); `detail`: its detail normal map's read
// descriptor (rhi::k_no_descriptor: none).
gpu::SkinLook to_gpu(const SkinLook& look, u32 detail);

// An eye look for the GPU, its values kept in range (non-finite ones
// taken as the defaults).
gpu::EyeLook to_gpu(const EyeLook& look);

} // namespace giant::render
