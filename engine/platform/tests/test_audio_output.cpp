// The speakers, on SDL's dummy driver: it pulls from the render callback
// on its own thread.
#include "giant/platform/audio_output.h"

#include "giant_test.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace giant;
using namespace giant::platform;

GIANT_TEST("platform audio: the device pulls frames from the render callback; closes cleanly") {
    for (int round = 0; round < 2; ++round) {
        std::atomic<u64> rendered{0};
        auto out = AudioOutput::open(48000, [&](float* samples, u32 frames) {
            for (u32 k = 0; k < frames * 2; ++k) samples[k] = 0.0f;
            rendered.fetch_add(frames);
        });
        GIANT_CHECK(out != nullptr);
        if (out == nullptr) return;
        GIANT_CHECK(!out->device().empty());
        for (int k = 0; k < 2000 && rendered.load() == 0; ++k) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        GIANT_CHECK(rendered.load() > 0);
        for (int k = 0; k < 2000 && out->frames() == 0; ++k) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        GIANT_CHECK(out->frames() > 0); // counted as pulled
    }
    GIANT_CHECK(AudioOutput::open(48000, {}) == nullptr); // nothing to render
}
