#include "editor_commands.h"

#include <algorithm>
#include <cmath>

namespace giant::editor {

using ai::Json;

namespace {

// Milliseconds to two decimals: what timing needs, a short reply.
double ms2(float ms) { return std::round(static_cast<double>(ms) * 100.0) / 100.0; }

} // namespace

// The frames since `since` (counted from 0, ever) still kept: each phase's
// median ms over them and their main thread's CPU time (`typical`), and,
// for those over `over_ms`, the frame's phases (those of 0.05 ms or more).
// Only the slow frames in full, so the reply stays small: it is written
// inside a frame being timed.
Json frame_phases(const FrameTimes& times, usize since, float over_ms) {
    std::vector<usize> slots;
    for (usize f = std::max(since, times.count > FrameTimes::k_frames ? times.count - FrameTimes::k_frames : usize{0});
         f < times.count; ++f) {
        usize slot = 0;
        if (times.slot_of(f, slot)) slots.push_back(slot);
    }
    Json typical = Json::object();
    std::vector<float> column;
    const auto median = [&column] {
        if (column.empty()) return 0.0f;
        std::nth_element(column.begin(), column.begin() + static_cast<std::ptrdiff_t>(column.size() / 2), column.end());
        return column[column.size() / 2];
    };
    for (usize p = 0; p < k_frame_phases; ++p) {
        column.clear();
        for (const usize slot : slots) column.push_back(times.phase_ms[slot][p]);
        typical[frame_phase_name(p)] = ms2(median());
    }
    column.clear();
    for (const usize slot : slots) column.push_back(times.cpu_ms[slot]);
    const float typical_cpu = median();
    Json slow = Json::array();
    for (usize f = times.count - slots.size(); f < times.count; ++f) {
        const usize slot = f % FrameTimes::k_frames;
        if (!(times.ms[slot] > over_ms)) continue;
        Json phases = Json::object();
        for (usize p = 0; p < k_frame_phases; ++p) {
            if (times.phase_ms[slot][p] >= 0.05f) phases[frame_phase_name(p)] = ms2(times.phase_ms[slot][p]);
        }
        slow.push_back({{"frame", f}, {"ms", ms2(times.ms[slot])}, {"cpu_ms", ms2(times.cpu_ms[slot])},
                        {"phases", std::move(phases)}});
    }
    return {{"since", times.count - slots.size()}, {"frames", slots.size()}, {"typical", std::move(typical)},
            {"typical_cpu_ms", ms2(typical_cpu)}, {"slow", std::move(slow)}};
}

} // namespace giant::editor
