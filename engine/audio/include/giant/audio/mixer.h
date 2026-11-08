// Audio: sounds heard as the world shapes them. A sound plays as taps, each
// a copy of it heard later, quieter and duller from some direction: the
// direct one (late by its distance over the speed of sound, highs taken
// by the air, muffled through a wall) and its echoes off what is round it
// (propagation.h), plus a share sent into one reverb tail whose length is
// set by the place. The mixer renders on the audio thread; the game thread
// only hands it work.
//
//   audio::Mixer mixer;
//   const audio::SoundId shot = mixer.add_sound(audio::synth_gunshot());
//   mixer.play({shot, taps, reverb_send, reverb_seconds});  // game thread
//   mixer.render(out, frames);                               // audio thread (or a test)
//
// Threads: play, add_sound and set_listener take a short lock the audio
// thread only ever tries (try_lock: if it cannot have it, it renders with
// what it has, and takes the work next time), so neither waits on the
// other (rule 9). Work a render does is bounded by k_max_voices x
// k_max_taps taps.
#pragma once

#include "giant/core/math.h"
#include "giant/core/types.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace giant::audio {

inline constexpr double k_speed_of_sound = 343.0; // metres a second, in air at 20 C
inline constexpr u32 k_sample_rate = 48000;
inline constexpr double k_max_delay = 60.0; // seconds a tap may be late (about 20 km): later ones are not played

// A mono sound at k_sample_rate, -1 to 1.
struct Sound {
    std::vector<float> samples;
    double seconds() const { return static_cast<double>(samples.size()) / k_sample_rate; }
};
using SoundId = u32;
inline constexpr SoundId k_no_sound = 0;

// One copy of a sound as heard.
struct Tap {
    double delay = 0.0;          // seconds after it was made
    float gain = 1.0f;           // 0 to 4
    float lowpass = 20000.0f;    // Hz: highs above it fade (air, a wall)
    Vec3 direction{0, 0, 1};     // world: from the listener towards where it is heard from
};

inline constexpr usize k_max_taps = 16;
inline constexpr usize k_max_voices = 64;

struct Play {
    SoundId sound = k_no_sound;
    std::vector<Tap> taps; // at most k_max_taps (the loudest kept)
    float reverb_send = 0.0f;    // of the direct sound's level, into the tail
    float reverb_seconds = 1.0f; // the tail's length (to -60 dB) where it was made
};

struct MixerStats {
    u32 voices = 0;      // playing now (or waiting for their first tap)
    u32 taps = 0;        // in those
    u64 frames = 0;      // rendered, ever
    u64 dropped = 0;     // plays refused: every voice had more still to play, or the queue was full
    u64 stolen = 0;      // voices cut short (faded over 5 ms) for a play no quieter than what they had left
    float peak = 0.0f;   // of the last render, before the limiter
    float reverb_seconds = 0.0f;
};

class Mixer {
public:
    Mixer();
    ~Mixer();
    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;

    // Game thread.
    SoundId add_sound(Sound sound);
    const Sound* sound(SoundId id) const; // (game thread)
    void play(Play play);
    // Where the listener faces: its right and up (world, unit) for panning.
    void set_listener(const Vec3& right, const Vec3& up);
    void stop_all();
    std::atomic<float> volume{1.0f}; // any thread

    // Audio thread: `frames` stereo frames into `out` (interleaved, 2 x frames).
    void render(float* out, u32 frames);

    MixerStats stats() const; // any thread (a moment's copy)

private:
    struct Voice;
    struct Shared;
    struct Reverb;
    void take_work();
    struct Pending;
    void start(const Pending& p);
    void mix(Voice& v, u64 from, u64 to, u64 render_from, u32 render_frames, float* chunk);

    std::unique_ptr<Shared> m_shared; // game thread -> audio thread
    std::vector<std::shared_ptr<const Sound>> m_sounds; // game thread's (index = id - 1)
    // Audio thread's.
    std::vector<Voice> m_voices; // k_max_voices, made up front (no allocation on the audio thread)
    std::vector<Voice> m_fading; // voices cut short, fading out
    std::vector<std::shared_ptr<const Sound>> m_audio_sounds;
    Vec3 m_right{1, 0, 0}, m_up{0, 1, 0};
    u64 m_clock = 0; // frames rendered
    std::unique_ptr<Reverb> m_reverb;
    std::vector<float> m_send; // scratch: the reverb's input, per frame
    // Stats, written by the audio thread.
    std::atomic<u32> m_stat_voices{0}, m_stat_taps{0};
    std::atomic<u64> m_stat_frames{0}, m_stat_dropped{0}, m_stat_stolen{0};
    std::atomic<float> m_stat_peak{0.0f}, m_stat_reverb{0.0f};
};

} // namespace giant::audio
