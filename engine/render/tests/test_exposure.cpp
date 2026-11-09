// Auto exposure's arithmetic (src/exposure.h): the histogram's middle in
// stops, the exposure that brings it to middle grey, and the easing.
#include "exposure.h"
#include "giant_test.h"

#include <array>
#include <cmath>

using namespace giant;
using namespace giant::render;

GIANT_TEST("exposure: the histogram's middle is the brightness of the bins it falls in") {
    std::array<u32, k_exposure_bins> bins{};
    float stops = 123.0f;
    GIANT_CHECK(!histogram_stops(bins, 0.1f, 0.9f, stops)); // nothing counted
    GIANT_CHECK(stops == 123.0f);
    bins[20] = 1000;
    GIANT_CHECK(histogram_stops(bins, 0.1f, 0.9f, stops));
    GIANT_CHECK(std::abs(stops - exposure_bin_stops(20)) < 1e-4f);
    // Bins are even in stops from k_exposure_low; bin 0 stands for black.
    const float width = (k_exposure_high - k_exposure_low) / static_cast<float>(k_exposure_bins - 1);
    GIANT_CHECK(exposure_bin_stops(0) == k_exposure_low);
    GIANT_CHECK(std::abs(exposure_bin_stops(1) - (k_exposure_low + 0.5f * width)) < 1e-5f);
    GIANT_CHECK(std::abs(exposure_bin_stops(k_exposure_bins - 1) - (k_exposure_high - 0.5f * width)) < 1e-4f);
    // Half the pixels in each of two bins: the average between them.
    bins[40] = 1000;
    GIANT_CHECK(histogram_stops(bins, 0.1f, 0.9f, stops));
    GIANT_CHECK(std::abs(stops - 0.5f * (exposure_bin_stops(20) + exposure_bin_stops(40))) < 1e-3f);
}

GIANT_TEST("exposure: the darkest and brightest tenth are left out") {
    std::array<u32, k_exposure_bins> bins{};
    bins[0] = 100;   // a tenth black
    bins[30] = 800;  // the middle
    bins[63] = 100;  // a tenth blazing (the sun)
    float stops = 0.0f;
    GIANT_CHECK(histogram_stops(bins, 0.1f, 0.9f, stops));
    GIANT_CHECK(std::abs(stops - exposure_bin_stops(30)) < 1e-3f);
    // Taken whole, they pull it.
    GIANT_CHECK(histogram_stops(bins, 0.0f, 1.0f, stops));
    GIANT_CHECK(std::abs(stops - exposure_bin_stops(30)) > 0.1f);
    // Nonsense fractions are kept sane; an empty slice gives its bin.
    GIANT_CHECK(histogram_stops(bins, std::nanf(""), 2.0f, stops));
    GIANT_CHECK(std::isfinite(stops));
    GIANT_CHECK(histogram_stops(bins, 0.5f, 0.5f, stops));
    GIANT_CHECK(std::abs(stops - exposure_bin_stops(30)) < 1e-3f);
}

GIANT_TEST("exposure: the target brings the scene to middle grey, compensated, within its limits") {
    // A scene at middle grey needs no change; one a stop brighter, a stop less.
    GIANT_CHECK(std::abs(exposure_target(std::log2(k_middle_grey), 0.0f, -16.0f, 16.0f)) < 1e-5f);
    GIANT_CHECK(std::abs(exposure_target(std::log2(k_middle_grey) + 1.0f, 0.0f, -16.0f, 16.0f) + 1.0f) < 1e-5f);
    GIANT_CHECK(std::abs(exposure_target(std::log2(k_middle_grey), 1.5f, -16.0f, 16.0f) - 1.5f) < 1e-5f);
    // A black scene is not brightened past the limit, nor the sun darkened.
    GIANT_CHECK(exposure_target(-40.0f, 0.0f, -4.0f, 6.0f) == 6.0f);
    GIANT_CHECK(exposure_target(40.0f, 0.0f, -4.0f, 6.0f) == -4.0f);
    GIANT_CHECK(std::isfinite(exposure_target(std::nanf(""), std::nanf(""), std::nanf(""), std::nanf(""))));
    GIANT_CHECK(exposure_target(0.0f, 0.0f, 3.0f, 1.0f) == 3.0f); // max under min: min wins
}

GIANT_TEST("exposure: easing moves at most the speed given, faster brightening than darkening") {
    GIANT_CHECK(std::abs(ease_exposure(0.0f, 10.0f, 3.0f, 1.0f, 0.5f) - 1.5f) < 1e-5f);
    GIANT_CHECK(std::abs(ease_exposure(0.0f, -10.0f, 3.0f, 1.0f, 0.5f) + 0.5f) < 1e-5f);
    GIANT_CHECK(ease_exposure(0.0f, 0.2f, 3.0f, 1.0f, 0.5f) == 0.2f); // arrives, no overshoot
    GIANT_CHECK(ease_exposure(2.0f, 5.0f, 3.0f, 1.0f, 0.0f) == 2.0f); // no time: held
    GIANT_CHECK(std::abs(ease_exposure(0.0f, 10.0f, 3.0f, 1.0f, 30.0f) - 3.0f) < 1e-5f); // at most a second counts
    GIANT_CHECK(ease_exposure(1.0f, std::nanf(""), 3.0f, 1.0f, 0.5f) == 1.0f);
    GIANT_CHECK(ease_exposure(std::nanf(""), 4.0f, 3.0f, 1.0f, 0.5f) == 4.0f);
    GIANT_CHECK(ease_exposure(0.0f, 10.0f, std::nanf(""), -1.0f, std::nanf("")) == 0.0f);
}
