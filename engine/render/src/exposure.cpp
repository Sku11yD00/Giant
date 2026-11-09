#include "exposure.h"

#include <algorithm>
#include <cmath>

namespace giant::render {

float exposure_bin_stops(u32 bin) {
    if (bin == 0) {
        return k_exposure_low;
    }
    const float width = (k_exposure_high - k_exposure_low) / static_cast<float>(k_exposure_bins - 1);
    return k_exposure_low + (static_cast<float>(std::min(bin, k_exposure_bins - 1)) - 0.5f) * width;
}

bool histogram_stops(std::span<const u32> bins, float low, float high, float& stops) {
    double total = 0.0;
    for (const u32 b : bins) {
        total += b;
    }
    if (!(total > 0.0)) {
        return false;
    }
    low = std::isfinite(low) ? std::clamp(low, 0.0f, 1.0f) : 0.0f;
    high = std::isfinite(high) ? std::clamp(high, low, 1.0f) : 1.0f;
    const double from = total * low;
    const double to = total * high;
    double sum = 0.0;
    double weight = 0.0;
    double before = 0.0;
    for (usize i = 0; i < bins.size(); ++i) {
        const double after = before + bins[i];
        const double inside = std::min(after, to) - std::max(before, from);
        if (inside > 0.0) {
            sum += inside * exposure_bin_stops(static_cast<u32>(i));
            weight += inside;
        }
        before = after;
    }
    if (!(weight > 0.0)) {
        // An empty slice (low == high): the bin it falls in.
        before = 0.0;
        for (usize i = 0; i < bins.size(); ++i) {
            before += bins[i];
            if (bins[i] != 0 && before >= from) {
                stops = exposure_bin_stops(static_cast<u32>(i));
                return true;
            }
        }
        return false;
    }
    stops = static_cast<float>(sum / weight);
    return true;
}

float exposure_target(float scene_stops, float compensation, float min_stops, float max_stops) {
    const float lo = std::isfinite(min_stops) ? min_stops : -16.0f;
    const float hi = std::isfinite(max_stops) ? std::max(max_stops, lo) : std::max(16.0f, lo);
    const float c = std::isfinite(compensation) ? compensation : 0.0f;
    const float s = std::isfinite(scene_stops) ? scene_stops : 0.0f;
    return std::clamp(std::log2(k_middle_grey) - s + c, lo, hi);
}

float ease_exposure(float current, float target, float up, float down, float seconds) {
    if (!std::isfinite(target)) {
        return current;
    }
    if (!std::isfinite(current)) {
        return target;
    }
    const float t = std::isfinite(seconds) ? std::clamp(seconds, 0.0f, 1.0f) : 0.0f;
    const float rise = std::isfinite(up) ? std::max(up, 0.0f) * t : 0.0f;
    const float fall = std::isfinite(down) ? std::max(down, 0.0f) * t : 0.0f;
    return current + std::clamp(target - current, -fall, rise);
}

} // namespace giant::render
