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
    // The device's counts: a typical frame's, and per slow frame its own
    // and the two before, summed (begin_frame waits for the frame two back:
    // memory made or freed in those is what the GPU may be held up by).
    const auto device_json = [](u32 created, u32 allocations, u32 released, float wait_ms, float release_ms) {
        return Json{{"created", created},   {"allocations", allocations}, {"released", released},
                    {"wait_ms", ms2(wait_ms)}, {"release_ms", ms2(release_ms)}};
    };
    const auto median_of = [&](auto field) {
        column.clear();
        for (const usize slot : slots) column.push_back(static_cast<float>(field(times.device[slot])));
        return median();
    };
    const Json typical_device = device_json(
        static_cast<u32>(median_of([](const FrameDevice& d) { return d.created; })),
        static_cast<u32>(median_of([](const FrameDevice& d) { return d.allocations; })),
        static_cast<u32>(median_of([](const FrameDevice& d) { return d.released; })),
        median_of([](const FrameDevice& d) { return d.wait_ms; }),
        median_of([](const FrameDevice& d) { return d.release_ms; }));
    Json slow = Json::array();
    for (usize f = times.count - slots.size(); f < times.count; ++f) {
        const usize slot = f % FrameTimes::k_frames;
        if (!(times.ms[slot] > over_ms)) continue;
        Json phases = Json::object();
        for (usize p = 0; p < k_frame_phases; ++p) {
            if (times.phase_ms[slot][p] >= 0.05f) phases[frame_phase_name(p)] = ms2(times.phase_ms[slot][p]);
        }
        const FrameDevice& d = times.device[slot];
        FrameDevice before;
        for (usize back = 1; back <= 2 && back <= f; ++back) {
            usize earlier = 0;
            if (!times.slot_of(f - back, earlier)) break;
            before.created += times.device[earlier].created;
            before.allocations += times.device[earlier].allocations;
            before.released += times.device[earlier].released;
        }
        slow.push_back({{"frame", f}, {"ms", ms2(times.ms[slot])}, {"cpu_ms", ms2(times.cpu_ms[slot])},
                        {"phases", std::move(phases)},
                        {"device", device_json(d.created, d.allocations, d.released, d.wait_ms, d.release_ms)},
                        {"device_before", Json{{"created", before.created},
                                               {"allocations", before.allocations},
                                               {"released", before.released}}}});
    }
    return {{"since", times.count - slots.size()}, {"frames", slots.size()}, {"typical", std::move(typical)},
            {"typical_cpu_ms", ms2(typical_cpu)}, {"typical_device", typical_device}, {"slow", std::move(slow)}};
}

} // namespace giant::editor
