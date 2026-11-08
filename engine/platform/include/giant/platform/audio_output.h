// The speakers: a stereo float stream the system pulls from its own
// thread, filled by a render callback (audio::Mixer::render). SDL3
// underneath. Null when there is no device (a headless machine, a test):
// the caller then renders itself, or not at all.
#pragma once

#include "giant/core/types.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace giant::platform {

// Fills `frames` stereo frames (interleaved floats). Called on the audio thread.
using AudioRender = std::function<void(float* out, u32 frames)>;

class AudioOutput {
public:
    // Opens the default playback device at `rate`; null (and logs why) if it cannot.
    static std::unique_ptr<AudioOutput> open(u32 rate, AudioRender render);
    ~AudioOutput();
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    const std::string& device() const { return m_device; }
    u64 frames() const { return m_frames.load(std::memory_order_relaxed); } // pulled so far

    struct Impl;

private:
    struct ConstructKey {};

public:
    AudioOutput(ConstructKey, std::unique_ptr<Impl> impl);

private:
    std::unique_ptr<Impl> m_impl;
    std::string m_device;
    std::atomic<u64> m_frames{0};
};

} // namespace giant::platform
