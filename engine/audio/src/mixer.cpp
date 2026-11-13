// The mixer: voices of taps (each a delayed, filtered, panned copy of one
// sound) and a reverb tail, rendered on the audio thread from work the
// game thread hands it under a lock the audio thread only tries.
#include "giant/audio/mixer.h"

#include <algorithm>
#include <cmath>

namespace giant::audio {

namespace {

constexpr usize k_max_pending = 256;         // plays waiting for the audio thread
constexpr u32 k_chunk = 1024;                // frames rendered at a time
constexpr usize k_sound_reserve = 4096;      // sounds the audio thread holds before it must grow
constexpr float k_min_reverb = 0.1f, k_max_reverb = 12.0f;
constexpr u32 k_fade_frames = 240;           // 5 ms: a voice cut short fades out over this
constexpr usize k_max_fading = k_max_voices; // as many as one batch of plays can cut short
constexpr float k_bed_out = 0.025f;          // a going bed's ease (s): 0.6 to silence in ~0.23 s
constexpr float k_tiny = 1e-20f;             // below this a decaying state is 0 (no denormals)

float lowpass_coefficient(float hz) {
    if (!(hz < 18000.0f)) return 1.0f; // open: the filter passes all
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
    std::array<Ambience, k_max_ambience> ambience{};
    float muffle = 20000.0f;
};

struct Mixer::Bed {
    SoundId id = k_no_sound;      // playing
    const Sound* sound = nullptr;
    u64 at = 0;                   // the next sample (round and round)
    SoundId next = k_no_sound;    // asked for (a change waits for this one to fade out)
    float target = 0.0f;          // gain asked for
    Vec3 direction{0, 0, 1};
    float spread = 1.0f;
    float a = 1.0f, a_target = 1.0f; // lowpass coefficient, eased
    float z = 0.0f;
    float left = 0.0f, right = 0.0f; // the pan gains now (gain in them)
};

struct Mixer::Voice {
    struct TapState {
        u64 start = 0; // the clock frame its first sample plays at
        float left = 0.0f, right = 0.0f;           // pan gains wanted
        float was_left = 0.0f, was_right = 0.0f;   // at the last render's end (ramped from)
        bool panned = false;
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
    u64 started = 0; // the clock when it was started
    u64 end = 0;     // the frame after its last tap's last sample
    u64 fade_end = 0; // fading out (cut short for another): silent from here
    // How loud what it has still to play is: its loudest tap not over, and the tail it still feeds.
    float remaining(u64 clock) const {
        const u64 len = sound->samples.size();
        float r = 0.0f;
        for (u32 k = 0; k < count; ++k)
            if (taps[k].start + len > clock) r = std::max(r, taps[k].gain);
        if (reverb_start + len > clock) r = std::max(r, reverb_send);
        return r;
    }
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
            if (std::fabs(damp_state) < k_tiny) damp_state = 0.0f;
            const float in = x + damp_state * feedback;
            line[at] = std::fabs(in) < k_tiny ? 0.0f : in;
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
            const float in = x + b * 0.5f;
            line[at] = std::fabs(in) < k_tiny ? 0.0f : in;
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
        std::array<float, 2> sums{};
        for (usize ear = 0; ear < 2; ++ear) {
            float sum = 0.0f;
            for (Comb& c : combs[ear]) sum += c.process(x * k_gain, damp);
            for (Allpass& a : allpasses[ear]) sum = a.process(sum);
            sums[ear] = sum;
        }
        if (!std::isfinite(sums[0]) || !std::isfinite(sums[1])) {
            clear(); // gone wrong (it never should: samples are bounded): heard as silence, not for good
            return;
        }
        left += sums[0];
        right += sums[1];
    }
};

Mixer::Mixer()
    : m_shared(std::make_unique<Shared>()), m_voices(k_max_voices), m_fading(k_max_fading),
      m_reverb(std::make_unique<Reverb>()), m_beds(std::make_unique<std::array<Bed, k_max_ambience>>()),
      m_send(k_chunk, 0.0f) {
    m_audio_sounds.reserve(k_sound_reserve);
    m_shared->added.reserve(64);
}

Mixer::~Mixer() = default;

SoundId Mixer::add_sound(Sound sound) {
    for (float& s : sound.samples) s = std::isfinite(s) ? std::clamp(s, -4.0f, 4.0f) : 0.0f;
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
    // Too loud as a whole: all of it scaled alike (the taps keep their balance).
    const float loudest = p.count > 0 ? play.taps[0].gain : 0.0f;
    const float scale = loudest > 4.0f ? 4.0f / loudest : 1.0f;
    for (u32 k = 0; k < p.count; ++k) {
        Tap t = play.taps[k];
        t.gain = std::min(t.gain * scale, 4.0f);
        const float len = finite(t.direction) ? length(t.direction) : 0.0f;
        t.direction = len > 1e-6f ? t.direction / len : Vec3{0, 0, 1};
        p.taps[k] = t;
        p.loudness = std::max(p.loudness, t.gain);
    }
    p.reverb_send = std::isfinite(play.reverb_send) ? std::clamp(play.reverb_send * scale, 0.0f, 4.0f) : 0.0f;
    p.reverb_seconds = std::isfinite(play.reverb_seconds) ? std::clamp(play.reverb_seconds, k_min_reverb, k_max_reverb)
                                                          : 1.0f;
    p.loudness = std::max(p.loudness, p.reverb_send); // as Voice::remaining weighs it
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
    m_shared->ambience.fill(Ambience{});
}

namespace {
Ambience cleaned(const Ambience& ambience, bool known) {
    Ambience a = ambience;
    if (!known) a.sound = k_no_sound;
    a.gain = std::isfinite(a.gain) ? std::clamp(a.gain, 0.0f, 4.0f) : 0.0f;
    a.lowpass = std::isfinite(a.lowpass) ? a.lowpass : 20000.0f;
    a.spread = std::isfinite(a.spread) ? std::clamp(a.spread, 0.0f, 1.0f) : 1.0f;
    const float len = finite(a.direction) ? length(a.direction) : 0.0f;
    if (len > 1e-6f) {
        a.direction = a.direction / len;
    } else {
        a.direction = {0, 0, 1};
        a.spread = 1.0f; // from nowhere in particular: all round
    }
    return a;
}
float cleaned_muffle(float hz) { return std::isfinite(hz) ? std::clamp(hz, 20.0f, 20000.0f) : 20000.0f; }
} // namespace

void Mixer::set_ambience(u32 slot, const Ambience& ambience) {
    if (slot >= k_max_ambience) return;
    const Ambience a = cleaned(ambience, sound(ambience.sound) != nullptr);
    std::lock_guard<std::mutex> lock(m_shared->mutex);
    m_shared->ambience[slot] = a;
}

void Mixer::set_ambience(u32 first, std::span<const Ambience> slots, float muffle_hz) {
    std::array<Ambience, k_max_ambience> a{};
    const usize count = first < k_max_ambience ? std::min<usize>(slots.size(), k_max_ambience - first) : 0;
    for (usize k = 0; k < count; ++k) a[k] = cleaned(slots[k], sound(slots[k].sound) != nullptr);
    const float m = cleaned_muffle(muffle_hz);
    std::lock_guard<std::mutex> lock(m_shared->mutex);
    for (usize k = 0; k < count; ++k) m_shared->ambience[first + k] = a[k];
    m_shared->muffle = m;
}

void Mixer::set_muffle(float hz) {
    const float h = cleaned_muffle(hz);
    std::lock_guard<std::mutex> lock(m_shared->mutex);
    m_shared->muffle = h;
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
        for (Voice& v : m_fading) v.active = false;
        m_reverb->clear();
        // (The beds: their ambience cleared with the stop, each fades out quickly below, not cut: no click.)
    }
    for (usize k = 0; k < k_max_ambience; ++k) {
        const Ambience& a = s.ambience[k];
        Bed& b = (*m_beds)[k];
        const bool there = a.sound != k_no_sound && a.sound <= m_audio_sounds.size() &&
                           !m_audio_sounds[a.sound - 1]->samples.empty();
        b.next = there ? a.sound : k_no_sound;
        b.target = there && b.next == b.id ? a.gain : 0.0f; // (another sound: this one fades out first)
        b.direction = a.direction;
        b.spread = a.spread;
        b.a_target = lowpass_coefficient(a.lowpass);
    }
    m_muffle_target = s.muffle;
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
        // The one with the least still to play (the oldest of those alike), if no louder than this.
        Voice* quietest = nullptr;
        float least = 0.0f;
        for (Voice& v : m_voices) {
            const float r = v.remaining(m_clock);
            if (quietest == nullptr || r < least || (r == least && v.started < quietest->started)) {
                quietest = &v;
                least = r;
            }
        }
        if (least > p.loudness) {
            m_stat_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        m_stat_stolen.fetch_add(1, std::memory_order_relaxed);
        // It fades out over 5 ms rather than stopping (a click); one not heard yet just goes.
        if (quietest->started != m_clock) {
            Voice* fade = &m_fading[0];
            for (Voice& f : m_fading)
                if (!f.active || (fade->active && f.fade_end < fade->fade_end)) fade = &f;
            *fade = *quietest;
            fade->fade_end = m_clock + k_fade_frames;
            fade->end = std::min(fade->end, fade->fade_end);
        }
        slot = quietest;
    }
    Voice& v = *slot;
    v.active = true;
    v.sound = m_audio_sounds[p.sound - 1].get();
    v.count = p.count;
    v.loudness = p.loudness;
    v.reverb_send = p.reverb_send;
    v.started = m_clock;
    v.fade_end = 0;
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
        ts.panned = false;
        ts.left = ts.right = ts.was_left = ts.was_right = 0.0f;
        v.end = std::max(v.end, ts.start + length_frames);
        first = std::min(first, t.delay);
    }
    v.reverb_start = m_clock + static_cast<u64>(std::llround(first * k_sample_rate));
    v.end = std::max(v.end, v.reverb_start + length_frames);
    if (p.reverb_send > 0.0f) m_reverb->target = p.reverb_seconds; // only what feeds the tail sets its length
}

void Mixer::mix(Voice& v, u64 from, u64 to, u64 render_from, u32 render_frames, float* chunk) {
    const std::vector<float>& samples = v.sound->samples;
    const u64 len = samples.size();
    const float span = static_cast<float>(std::max<u32>(render_frames, 1));
    for (u32 k = 0; k < v.count; ++k) {
        Voice::TapState& t = v.taps[k];
        if (t.start >= to || t.start + len <= from) continue;
        const u64 a = std::max(from, t.start), b = std::min({to, t.start + len, v.fade_end ? v.fade_end : to});
        for (u64 c = a; c < b; ++c) {
            t.z += t.a * (samples[c - t.start] - t.z);
            // The pan ramped across the render from where it was (turning is heard smooth).
            const float f = static_cast<float>(c - render_from + 1) / span;
            float l = t.was_left + (t.left - t.was_left) * f, r = t.was_right + (t.right - t.was_right) * f;
            if (v.fade_end != 0) {
                const float g = static_cast<float>(v.fade_end - c) / static_cast<float>(k_fade_frames);
                l *= g;
                r *= g;
            }
            const usize i = static_cast<usize>(c - from) * 2;
            chunk[i] += t.z * l;
            chunk[i + 1] += t.z * r;
        }
    }
    if (v.reverb_send > 0.0f && v.reverb_start < to && v.reverb_start + len > from) {
        const u64 a = std::max(from, v.reverb_start),
                  b = std::min({to, v.reverb_start + len, v.fade_end ? v.fade_end : to});
        for (u64 c = a; c < b; ++c) {
            const float g = v.fade_end != 0 ? static_cast<float>(v.fade_end - c) / static_cast<float>(k_fade_frames) : 1.0f;
            m_send[c - from] += samples[c - v.reverb_start] * v.reverb_send * g;
        }
    }
    if (v.end <= to) v.active = false;
}

void Mixer::mix_beds(u32 n, float* chunk) {
    const float ease = 1.0f - std::exp(-static_cast<float>(n) / (0.25f * static_cast<float>(k_sample_rate)));
    // Going (another sound asked for, or none): faded out sooner, in about a quarter second all told.
    const float ease_out = 1.0f - std::exp(-static_cast<float>(n) / (k_bed_out * static_cast<float>(k_sample_rate)));
    for (Bed& b : *m_beds) {
        // Faded out: the sound asked for (if another) starts from silence.
        if (b.next != b.id && std::max(b.left, b.right) < 1e-4f) {
            b.id = b.next;
            b.sound = b.id != k_no_sound ? m_audio_sounds[b.id - 1].get() : nullptr;
            b.at = 0;
            b.z = 0.0f;
            b.left = b.right = 0.0f;
        }
        if (b.sound == nullptr) continue;
        // Equal power left to right, towards both alike as it spreads.
        const float side = std::clamp(dot(b.direction, m_right), -1.0f, 1.0f);
        const float angle = (side + 1.0f) * (k_pi / 4.0f);
        const float l_want = b.target * (std::cos(angle) + (0.70710678f - std::cos(angle)) * b.spread);
        const float r_want = b.target * (std::sin(angle) + (0.70710678f - std::sin(angle)) * b.spread);
        const float l0 = b.left, r0 = b.right;
        const float e = b.next != b.id ? ease_out : ease;
        const float l1 = l0 + (l_want - l0) * e, r1 = r0 + (r_want - r0) * e;
        b.a += (b.a_target - b.a) * ease;
        if (std::max({l0, r0, l1, r1}) > 0.0f) {
            const std::vector<float>& samples = b.sound->samples;
            const u64 len = samples.size();
            const float span = static_cast<float>(std::max<u32>(n, 1));
            for (u32 c = 0; c < n; ++c) {
                b.z += b.a * (samples[b.at] - b.z);
                if (std::fabs(b.z) < k_tiny) b.z = 0.0f;
                b.at = b.at + 1 == len ? 0 : b.at + 1;
                const float f = static_cast<float>(c + 1) / span;
                chunk[c * 2] += b.z * (l0 + (l1 - l0) * f);
                chunk[c * 2 + 1] += b.z * (r0 + (r1 - r0) * f);
            }
        }
        b.left = l1 < 1e-6f ? 0.0f : l1;
        b.right = r1 < 1e-6f ? 0.0f : r1;
    }
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
            t.was_left = t.left;
            t.was_right = t.right;
            t.left = std::cos(angle) * t.gain;
            t.right = std::sin(angle) * t.gain;
            if (!t.panned) {
                t.was_left = t.left;
                t.was_right = t.right;
                t.panned = true;
            }
        }
    }
    for (Voice& v : m_fading)
        for (u32 k = 0; k < v.count; ++k) {
            v.taps[k].was_left = v.taps[k].left;
            v.taps[k].was_right = v.taps[k].right;
        }
    const u64 render_from = m_clock;
    const float volume_read = volume.load(std::memory_order_relaxed);
    const float volume_now = std::isfinite(volume_read) ? std::clamp(volume_read, 0.0f, 4.0f) : 0.0f;
    float peak = 0.0f;
    for (u32 done = 0; done < frames;) {
        const u32 n = std::min(k_chunk, frames - done);
        float* chunk = out + static_cast<usize>(done) * 2;
        std::fill(chunk, chunk + static_cast<usize>(n) * 2, 0.0f);
        std::fill(m_send.begin(), m_send.begin() + n, 0.0f);
        const u64 from = m_clock, to = m_clock + n;
        for (Voice& v : m_voices)
            if (v.active) mix(v, from, to, render_from, frames, chunk);
        for (Voice& v : m_fading)
            if (v.active) mix(v, from, to, render_from, frames, chunk);
        mix_beds(n, chunk);
        m_reverb->follow(n);
        m_muffle_hz += (m_muffle_target - m_muffle_hz) *
                       (1.0f - std::exp(-static_cast<float>(n) / (0.1f * static_cast<float>(k_sample_rate))));
        const float muffle_a = lowpass_coefficient(m_muffle_hz);
        for (u32 c = 0; c < n; ++c) {
            float& l = chunk[c * 2];
            float& r = chunk[c * 2 + 1];
            m_reverb->process(m_send[c], l, r);
            if (muffle_a < 1.0f) {
                m_muffle_z[0] += muffle_a * (l - m_muffle_z[0]);
                m_muffle_z[1] += muffle_a * (r - m_muffle_z[1]);
                if (std::fabs(m_muffle_z[0]) < k_tiny) m_muffle_z[0] = 0.0f;
                if (std::fabs(m_muffle_z[1]) < k_tiny) m_muffle_z[1] = 0.0f;
                l = m_muffle_z[0];
                r = m_muffle_z[1];
            } else {
                m_muffle_z = {l, r}; // (followed while open, so closing it does not click)
            }
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
    m_stat_muffle.store(m_muffle_hz, std::memory_order_relaxed);
    u32 beds = 0;
    for (const Bed& b : *m_beds) beds += b.sound != nullptr && std::max(b.left, b.right) > 1e-4f ? 1u : 0u;
    m_stat_ambience.store(beds, std::memory_order_relaxed);
}

MixerStats Mixer::stats() const {
    MixerStats s;
    s.voices = m_stat_voices.load(std::memory_order_relaxed);
    s.taps = m_stat_taps.load(std::memory_order_relaxed);
    s.frames = m_stat_frames.load(std::memory_order_relaxed);
    s.dropped = m_stat_dropped.load(std::memory_order_relaxed);
    s.stolen = m_stat_stolen.load(std::memory_order_relaxed);
    s.peak = m_stat_peak.load(std::memory_order_relaxed);
    s.reverb_seconds = m_stat_reverb.load(std::memory_order_relaxed);
    s.ambience = m_stat_ambience.load(std::memory_order_relaxed);
    s.muffle = m_stat_muffle.load(std::memory_order_relaxed);
    return s;
}

} // namespace giant::audio
