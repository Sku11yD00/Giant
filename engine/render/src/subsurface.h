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
#pragma once

#include "giant/core/types.h"

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

} // namespace giant::render
