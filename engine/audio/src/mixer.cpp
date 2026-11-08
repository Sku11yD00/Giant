// The mixer: voices of taps (each a delayed, filtered, panned copy of one
// sound) and a reverb tail, rendered on the audio thread from work the
// game thread hands it under a lock the audio thread only tries.
#include "giant/audio/mixer.h"

#include <algorithm>
#include <cmath>

namespace giant::audio {

namespace {

constexpr usize k_max_pending = 256;         // plays waiting for the audio thread
constexpr double k_max_delay = 60.0;         // seconds: 20 km away
constexpr u32 k_chunk = 1024;                // frames rendered at a time
constexpr usize k_sound_reserve = 4096;      // sounds the audio thread holds before it must grow
constexpr float k_min_reverb = 0.1f, k_max_reverb = 12.0f;

float lowpass_coefficient(float hz) {
    const float fc = std::clamp(hz, 20.0f, 20000.0f);
    return 1.0f - std::exp(-2.0f * k_pi * fc / static_cast<float>(k_sample_rate));
}

bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// Past 0.8 the level bends towards 1 (never past it).
float soft_limit(float x) {
    const float a = std::fabs(x);
    if (a <= 0.8f) return x;
    const float bent = 0.8f + 0.2f * std::tanh((a - 0.8f) / 0.2f);
    return x < 0.0f ? -bent : bent;
}

} // namespace

struct Mixer::Pending {
    SoundId sound = k_no_sound;
    std::array<Tap, k_max_taps> taps{};
    u32 count = 0;
    float reverb_send = 0.0f;
    float reverb_seconds = 1.0f;
    float loudness = 0.0f; // the loudest tap's gain
};

struct Mixer::Shared {
    std::mutex mutex;
    std::array<Pending, k_max_pending> pending{};
    usize pending_count = 0;
    u64 refused = 0; // plays the queue had no room for
    std::vector<std::shared_ptr<const Sound>> added; // sounds the audio thread has not taken
    Vec3 right{1, 0, 0}, up{0, 1, 0};
    bool stop = false;
};

struct Mixer::Voice {
    struct TapState {
        u64 start = 0; // the clock frame its first sample plays at
        float left = 0.0f, right = 0.0f;
        float a = 1.0f; // lowpass coefficient
        float z = 0.0f; // lowpass state
        Vec3 direction{0, 0, 1};
        float gain = 0.0f;
    };
    bool active = false;
    const Sound* sound = nullptr;
    std::array<TapState, k_max_taps> taps{};
    u32 count = 0;
    u64 reverb_start = 0; // the direct tap's start
    float reverb_send = 0.0f;
    float loudness = 0.0f;
    u64 end = 0; // the frame after its last tap's last sample
};

// A Schroeder-Moorer tail: four damped combs into two allpasses, a pair
// per ear with their lengths a little apart so the two do not match.
struct Mixer::Reverb {
    struct Comb {
        std::vector<float> line;
        usize at = 0;
        float feedback = 0.0f;
        float damp_state = 0.0f;
        float process(float x, float damping) {
            const float y = line[at];
            damp_state = y * (1.0f - damping) + damp_state * damping;
            line[at] = x + damp_state * feedback;
            at = at + 1 == line.size() ? 0 : at + 1;
            return y;
        }
    };
    struct Allpass {
        std::vector<float> line;
        usize at = 0;
        float process(float x) {
            const float b = line[at];
            const float y = b - x;
            line[at] = x + b * 0.5f;
            at = at + 1 == line.size() ? 0 : at + 1;
            return y;
        }
    };
    std::array<std::array<Comb, 4>, 2> combs;
    std::array<std::array<Allpass, 2>, 2> allpasses;
    float seconds = 0.0f;  // what the combs are set for
    float target = 1.0f;   // what plays ask for (the latest)
    float damp = 0.25f;

    Reverb() {
        // Freeverb's lengths (at 44.1 kHz), scaled to the rate; the right ear 23 samples on.
        constexpr std::array<u32, 4> comb_lengths{1557, 1617, 1491, 1422};
        constexpr std::array<u32, 2> allpass_lengths{556, 441};
        const double scale = static_cast<double>(k_sample_rate) / 44100.0;
        for (usize ear = 0; ear < 2; ++ear) {
            for (usize k = 0; k < 4; ++k)
                combs[ear][k].line.assign(static_cast<usize>(static_cast<double>(comb_lengths[k] + 23 * ear) * scale), 0.0f);
            for (usize k = 0; k < 2; ++k)
                allpasses[ear][k].line.assign(static_cast<usize>(static_cast<double>(allpass_lengths[k] + 23 * ear) * scale), 0.0f);
        }
        set(1.0f);
    }
    // Each comb's feedback so a loop decays 60 dB in `t` seconds.
    void set(float length) {
        seconds = std::clamp(length, k_min_reverb, k_max_reverb);
        for (auto& ear : combs)
            for (Comb& c : ear) {
                const double loop = static_cast<double>(c.line.size()) / static_cast<double>(k_sample_rate);
                c.feedback = static_cast<float>(std::pow(10.0, -3.0 * loop / seconds));
            }
    }
    void clear() {
        for (auto& ear : combs)
            for (Comb& c : ear) {
                std::fill(c.line.begin(), c.line.end(), 0.0f);
                c.damp_state = 0.0f;
            }
        for (auto& ear : allpasses)
            for (Allpass& a : ear) std::fill(a.line.begin(), a.line.end(), 0.0f);
    }
    // Moves toward the target a little each chunk, so a change is heard as the tail turning, not a click.
    void follow(u32 frames) {
        if (std::fabs(seconds - target) < 1e-3f) return;
        const float step = 1.0f - std::exp(-static_cast<float>(frames) / (0.25f * k_sample_rate));
        set(seconds + (target - seconds) * step);
    }
    void process(float x, float& left, float& right) {
        constexpr float k_gain = 0.015f; // the combs ring up loud; this sets the tail level near the dry one
        for (usize ear = 0; ear < 2; ++ear) {
            float sum = 0.0f;
            for (Comb& c : combs[ear]) sum += c.process(x * k_gain, damp);
            for (Allpass& a : allpasses[ear]) sum = a.process(sum);
            (ear == 0 ? left : right) += sum;
        }
    }
};

Mixer::Mixer()
    : m_shared(std::make_unique<Shared>()), m_voices(k_max_voices), m_reverb(std::make_unique<Reverb>()),
      m_send(k_chunk, 0.0f) {
    m_audio_sounds.reserve(k_sound_reserve);
    m_shared->added.reserve(64);
}

Mixer::~Mixer() = default;

SoundId Mixer::add_sound(Sound sound) {
    for (float& s : sound.samples)
        if (!std::isfinite(s)) s = 0.0f;
    auto shared = std::make_shared<const Sound>(std::move(sound));
    m_sounds.push_back(shared);
    {
        std::lock_guard<std::mutex> lock(m_shared->mutex);
        m_shared->added.push_back(std::move(shared));
    }
    return static_cast<SoundId>(m_sounds.size());
}

const Sound* Mixer::sound(SoundId id) const {
    if (id == k_no_sound || id > m_sounds.size()) return nullptr;
    return m_sounds[id - 1].get();
}

void Mixer::play(Play play) {
    if (sound(play.sound) == nullptr) return;
    Pending p;
    p.sound = play.sound;
    // The loudest k_max_taps that make sense.
    std::erase_if(play.taps, [](const Tap& t) {
        return !std::isfinite(t.delay) || !std::isfinite(t.gain) || !std::isfinite(t.lowpass) || t.gain <= 0.0f ||
               t.delay < 0.0f || t.delay > k_max_delay;
    });
    std::sort(play.taps.begin(), play.taps.end(), [](const Tap& a, const Tap& b) { return a.gain > b.gain; });
    p.count = static_cast<u32>(std::min(play.taps.size(), k_max_taps));
    for (u32 k = 0; k < p.count; ++k) {
        Tap t = play.taps[k];
        t.gain = std::min(t.gain, 4.0f);
        const float len = finite(t.direction) ? length(t.direction) : 0.0f;
        t.direction = len > 1e-6f ? t.direction / len : Vec3{0, 0, 1};
        p.taps[k] = t;
        p.loudness = std::max(p.loudness, t.gain);
    }
    p.reverb_send = std::isfinite(play.reverb_send) ? std::clamp(play.reverb_send, 0.0f, 4.0f) : 0.0f;
    p.reverb_seconds = std::isfinite(play.reverb_seconds) ? std::clamp(play.reverb_seconds, k_min_reverb, k_max_reverb)
                                                          : 1.0f;
    if (p.count == 0 && p.reverb_send <= 0.0f) return;
    std::lock_guard<std::mutex> lock(m_shared->mutex);
    if (m_shared->pending_count == k_max_pending) {
        ++m_shared->refused;
        return;
    }
    m_shared->pending[m_shared->pending_count++] = p;
}

void Mixer::set_listener(const Vec3& right, const Vec3& up) {
    if (!finite(right) || !finite(up) || length(right) < 1e-6f || length(up) < 1e-6f) return;
    std::lock_guard<std::mutex> lock(m_shared->mutex);
    m_shared->right = right / length(right);
    m_shared->up = up / length(up);
}

void Mixer::stop_all() {
    std::lock_guard<std::mutex> lock(m_shared->mutex);
    m_shared->stop = true;
    m_shared->pending_count = 0;
}

void Mixer::take_work() {
    std::unique_lock<std::mutex> lock(m_shared->mutex, std::try_to_lock);
    if (!lock.owns_lock()) return; // the game thread has it: next time
    Shared& s = *m_shared;
    for (auto& added : s.added) m_audio_sounds.push_back(std::move(added));
    s.added.clear();
    m_right = s.right;
    m_up = s.up;
    if (s.stop) {
        s.stop = false;
        for (Voice& v : m_voices) v.active = false;
        m_reverb->clear();
    }
    m_stat_dropped.fetch_add(s.refused, std::memory_order_relaxed);
    s.refused = 0;
    for (usize k = 0; k < s.pending_count; ++k) start(s.pending[k]);
    s.pending_count = 0;
}

void Mixer::start(const Pending& p) {
    if (p.sound == k_no_sound || p.sound > m_audio_sounds.size()) return;
    // A free voice, or the quietest if it is quieter than this one.
    Voice* slot = nullptr;
    for (Voice& v : m_voices)
        if (!v.active) {
            slot = &v;
            break;
        }
    if (slot == nullptr) {
        Voice* quietest = &m_voices[0];
        for (Voice& v : m_voices)
            if (v.loudness < quietest->loudness) quietest = &v;
        m_stat_dropped.fetch_add(1, std::memory_order_relaxed);
        if (quietest->loudness >= p.loudness) return;
        slot = quietest;
    }
    Voice& v = *slot;
    v.active = true;
    v.sound = m_audio_sounds[p.sound - 1].get();
    v.count = p.count;
    v.loudness = p.loudness;
    v.reverb_send = p.reverb_send;
    const u64 length_frames = v.sound->samples.size();
    v.end = m_clock;
    double first = p.count > 0 ? k_max_delay : 0.0;
    for (u32 k = 0; k < p.count; ++k) {
        const Tap& t = p.taps[k];
        Voice::TapState& ts = v.taps[k];
        ts.start = m_clock + static_cast<u64>(std::llround(t.delay * k_sample_rate));
        ts.a = lowpass_coefficient(t.lowpass);
        ts.z = 0.0f;
        ts.direction = t.direction;
        ts.gain = t.gain;
        v.end = std::max(v.end, ts.start + length_frames);
        first = std::min(first, t.delay);
    }
    v.reverb_start = m_clock + static_cast<u64>(std::llround(first * k_sample_rate));
    v.end = std::max(v.end, v.reverb_start + length_frames);
    m_reverb->target = p.reverb_seconds;
}

void Mixer::render(float* out, u32 frames) {
    take_work();
    // Pan from where each tap is heard: equal power, left to right.
    for (Voice& v : m_voices) {
        if (!v.active) continue;
        for (u32 k = 0; k < v.count; ++k) {
            Voice::TapState& t = v.taps[k];
            const float side = std::clamp(dot(t.direction, m_right), -1.0f, 1.0f);
            const float angle = (side + 1.0f) * (k_pi / 4.0f);
            t.left = std::cos(angle) * t.gain;
            t.right = std::sin(angle) * t.gain;
        }
    }
    const float volume_read = volume.load(std::memory_order_relaxed);
    const float volume_now = std::isfinite(volume_read) ? std::clamp(volume_read, 0.0f, 4.0f) : 0.0f;
    float peak = 0.0f;
    for (u32 done = 0; done < frames;) {
        const u32 n = std::min(k_chunk, frames - done);
        float* chunk = out + static_cast<usize>(done) * 2;
        std::fill(chunk, chunk + static_cast<usize>(n) * 2, 0.0f);
        std::fill(m_send.begin(), m_send.begin() + n, 0.0f);
        const u64 from = m_clock, to = m_clock + n;
        for (Voice& v : m_voices) {
            if (!v.active) continue;
            const std::vector<float>& samples = v.sound->samples;
            const u64 len = samples.size();
            for (u32 k = 0; k < v.count; ++k) {
                Voice::TapState& t = v.taps[k];
                if (t.start >= to || t.start + len <= from) continue;
                const u64 a = std::max(from, t.start), b = std::min(to, t.start + len);
                for (u64 c = a; c < b; ++c) {
                    t.z += t.a * (samples[c - t.start] - t.z);
                    const usize i = static_cast<usize>(c - from) * 2;
                    chunk[i] += t.z * t.left;
                    chunk[i + 1] += t.z * t.right;
                }
            }
            if (v.reverb_send > 0.0f && v.reverb_start < to && v.reverb_start + len > from) {
                const u64 a = std::max(from, v.reverb_start), b = std::min(to, v.reverb_start + len);
                for (u64 c = a; c < b; ++c) m_send[c - from] += samples[c - v.reverb_start] * v.reverb_send;
            }
            if (v.end <= to) v.active = false;
        }
        m_reverb->follow(n);
        for (u32 c = 0; c < n; ++c) {
            float& l = chunk[c * 2];
            float& r = chunk[c * 2 + 1];
            m_reverb->process(m_send[c], l, r);
            peak = std::max({peak, std::fabs(l), std::fabs(r)});
            l = soft_limit(l * volume_now);
            r = soft_limit(r * volume_now);
        }
        m_clock = to;
        done += n;
    }
    u32 voices = 0, taps = 0;
    for (const Voice& v : m_voices)
        if (v.active) {
            ++voices;
            taps += v.count;
        }
    m_stat_voices.store(voices, std::memory_order_relaxed);
    m_stat_taps.store(taps, std::memory_order_relaxed);
    m_stat_frames.fetch_add(frames, std::memory_order_relaxed);
    m_stat_peak.store(peak, std::memory_order_relaxed);
    m_stat_reverb.store(m_reverb->seconds, std::memory_order_relaxed);
}

MixerStats Mixer::stats() const {
    MixerStats s;
    s.voices = m_stat_voices.load(std::memory_order_relaxed);
    s.taps = m_stat_taps.load(std::memory_order_relaxed);
    s.frames = m_stat_frames.load(std::memory_order_relaxed);
    s.dropped = m_stat_dropped.load(std::memory_order_relaxed);
    s.peak = m_stat_peak.load(std::memory_order_relaxed);
    s.reverb_seconds = m_stat_reverb.load(std::memory_order_relaxed);
    return s;
}

} // namespace giant::audio
