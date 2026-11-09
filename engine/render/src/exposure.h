// Auto exposure's arithmetic (RenderSettings::auto_exposure), on the CPU.
//
// post.slang's cs_histogram counts the lit pixels' brightness (luminance of
// the light before exposure) into k_exposure_bins bins: bin 0 for black and
// anything under 2^k_exposure_low, bins 1 on even in stops up to
// 2^k_exposure_high (the last also takes anything brighter). Read back a
// frame or two later, the middle of it (between two fractions of the
// pixels, darkest first) gives the scene's brightness in stops; the
// exposure that brings that to middle grey is eased towards, a few stops a
// second, as eyes adapt. Layout: the bins are u32 counts, in order.
#pragma once

#include "giant/core/types.h"

#include <span>

namespace giant::render {

inline constexpr u32 k_exposure_bins = 64;
inline constexpr float k_exposure_low = -16.0f; // stops (log2 of the light): bin 1 starts here
inline constexpr float k_exposure_high = 8.0f;  // and the last ends here
inline constexpr float k_middle_grey = 0.18f;

// The brightness in stops the bin stands for (its middle; bin 0: k_exposure_low).
float exposure_bin_stops(u32 bin);

// The average brightness in stops of the pixels from fraction `low` to
// `high` of them (0 to 1, darkest first), bin 0 (black) left out. False
// with nothing counted past it (all black: the exposure is held).
bool histogram_stops(std::span<const u32> bins, float low, float high, float& stops);

// The exposure (in stops: log2 of the multiplier) that brings a scene
// `scene_stops` bright to middle grey, raised by `compensation` stops and
// kept within [min_stops, max_stops].
float exposure_target(float scene_stops, float compensation, float min_stops, float max_stops);

// `current` moved towards `target` by at most `up` stops a second when
// brightening and `down` when darkening, over `seconds`.
float ease_exposure(float current, float target, float up, float down, float seconds);

} // namespace giant::render
