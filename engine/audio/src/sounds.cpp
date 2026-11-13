// Made sounds and WAV files.
#include "giant/audio/sounds.h"

#include "giant/core/math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

namespace giant::audio {

namespace {

// Small deterministic noise, -1 to 1.
struct Noise {
    u32 state;
    explicit Noise(u32 seed) : state((seed * 747796405u + 2891336453u) | 1u) {} // never 0 (xorshift stays there)
    float next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float>(state) / 2147483648.0f - 1.0f;
    }
};

void normalize(Sound& s, float peak) {
    float m = 0.0f;
    for (float x : s.samples) m = std::max(m, std::fabs(x));
    if (m > 0.0f)
        for (float& x : s.samples) x *= peak / m;
}

// Made `n` samples long with `fade` more, whose end is cross-faded (equal
// power) into its start: played round and round, no seam. Then set to
// `rms` (and no peak past 0.95).
Sound looped(std::vector<float> made, usize n, usize fade, float rms) {
    Sound s;
    s.samples.assign(made.begin(), made.begin() + static_cast<std::ptrdiff_t>(n));
    for (usize i = 0; i < fade; ++i) {
        const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(fade);
        s.samples[i] = s.samples[i] * std::sin(t * k_pi * 0.5f) + made[n + i] * std::cos(t * k_pi * 0.5f);
    }
    double sum = 0.0;
    for (const float x : s.samples) sum += static_cast<double>(x) * x;
    const float now = static_cast<float>(std::sqrt(sum / static_cast<double>(std::max<usize>(n, 1))));
    if (now > 0.0f)
        for (float& x : s.samples) x *= rms / now;
    float peak = 0.0f;
    for (const float x : s.samples) peak = std::max(peak, std::fabs(x));
    if (peak > 0.95f)
        for (float& x : s.samples) x *= 0.95f / peak;
    return s;
}

// A one-pole lowpass's coefficient at `hz`.
float pole(float hz) { return 1.0f - std::exp(-2.0f * k_pi * hz / static_cast<float>(k_sample_rate)); }

u32 read_u32(std::span<const u8> b, usize at) {
    return static_cast<u32>(b[at]) | static_cast<u32>(b[at + 1]) << 8 | static_cast<u32>(b[at + 2]) << 16 |
           static_cast<u32>(b[at + 3]) << 24;
}
u16 read_u16(std::span<const u8> b, usize at) { return static_cast<u16>(b[at] | b[at + 1] << 8); }
void put_u32(std::vector<u8>& b, u32 v) {
    for (int k = 0; k < 4; ++k) b.push_back(static_cast<u8>(v >> (8 * k)));
}
void put_u16(std::vector<u8>& b, u16 v) {
    b.push_back(static_cast<u8>(v));
    b.push_back(static_cast<u8>(v >> 8));
}

constexpr usize k_max_wav_frames = static_cast<usize>(k_sample_rate) * 600; // ten minutes

} // namespace

Sound synth_gunshot(u32 seed) {
    Sound s;
    const usize n = k_sample_rate * 6 / 10; // 0.6 s
    s.samples.resize(n);
    Noise noise(seed);
    float low = 0.0f, band = 0.0f;
    for (usize i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / k_sample_rate;
        // The blast: noise, its highs fading faster than its body.
        const float w = noise.next();
        low += 0.25f * (w - low);
        band += 0.6f * (w - band);
        const float blast = band * std::exp(-t / 0.012f) + low * 1.6f * std::exp(-t / 0.08f);
        // The thump: a falling low sine.
        const float thump = std::sin(2.0f * k_pi * (70.0f * t - 40.0f * t * t)) * std::exp(-t / 0.06f) * 0.8f;
        // The edge: the first half millisecond.
        const float edge = t < 0.0005f ? 1.0f - t / 0.0005f : 0.0f;
        s.samples[i] = blast + thump + edge * 2.0f;
    }
    normalize(s, 1.0f);
    return s;
}

Sound synth_crack() {
    Sound s;
    const usize n = k_sample_rate / 50; // 20 ms with its ring
    s.samples.assign(n, 0.0f);
    const usize wave = std::max<usize>(k_sample_rate / 2000, 4); // 0.5 ms
    for (usize i = 0; i <= wave && i < n; ++i) s.samples[i] = 1.0f - 2.0f * static_cast<float>(i) / wave;
    // A short ring after it (the ear and the ground), so it is heard as more than a tick.
    for (usize i = wave + 1; i < n; ++i) {
        const float t = static_cast<float>(i - wave) / k_sample_rate;
        s.samples[i] = 0.15f * std::sin(2.0f * k_pi * 3000.0f * t) * std::exp(-t / 0.003f);
    }
    return s;
}

Sound synth_impact(u32 seed) {
    Sound s;
    const usize n = k_sample_rate / 25; // 40 ms
    s.samples.resize(n);
    Noise noise(seed);
    float low = 0.0f;
    for (usize i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / k_sample_rate;
        low += 0.35f * (noise.next() - low);
        s.samples[i] = (low * 1.5f + std::sin(2.0f * k_pi * 180.0f * t) * 0.6f) * std::exp(-t / 0.008f);
    }
    normalize(s, 1.0f);
    return s;
}

Sound synth_splash(u32 seed) {
    Sound s;
    s.samples.resize(k_sample_rate * 6 / 5); // 1.2 s
    Noise noise(seed);
    Noise when(seed * 29u + 5u);
    float lo = 0.0f, hi = 0.0f, spray_lo = 0.0f;
    const float a_lo = pole(300.0f), a_hi = pole(8000.0f), a_spray = pole(1500.0f);
    // Bubbles: each a rising tone from silence (2 ms in), ringing down, gone smoothly by 50 ms; a few at once.
    struct Bubble {
        float start = -1.0f, pitch = 0.0f, phase = 0.0f;
    };
    std::array<Bubble, 4> bubbles{};
    usize next_bubble = 0;
    double next = 0.06;
    float thump = 0.0f;
    for (usize i = 0; i < s.samples.size(); ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(k_sample_rate);
        const float x = noise.next();
        hi += a_hi * (x - hi);
        lo += a_lo * (hi - lo);
        spray_lo += a_spray * (hi - spray_lo);
        // The slap: broad noise, in over 3 ms, gone in about 60 ms.
        const float slap = (hi - 0.5f * lo) * std::min(t / 0.003f, 1.0f) * std::exp(-t / 0.06f);
        // The spray falling back: brighter noise, rising to 0.1 s, falling over 0.35 s.
        const float spray = (hi - spray_lo) * (t / 0.1f) * std::exp(1.0f - t / 0.1f) * std::exp(-t / 0.35f) * 1.8f;
        // The hole closing: a low thump at 60 ms (120 Hz falling to 70).
        const float since = t - 0.06f;
        if (since > 0.0f) thump += 2.0f * k_pi * (120.0f - 50.0f * std::min(since / 0.2f, 1.0f)) / k_sample_rate;
        const float low = since > 0.0f ? std::sin(thump) * std::min(since / 0.004f, 1.0f) * std::exp(-since / 0.09f) : 0.0f;
        // Bubbles: many at first, fewer later, none started after 0.9 s.
        if (static_cast<double>(t) >= next && t < 0.9f) {
            Bubble& b = bubbles[next_bubble];
            next_bubble = (next_bubble + 1) % bubbles.size();
            b.start = t;
            b.pitch = 400.0f + 1100.0f * (when.next() * 0.5f + 0.5f);
            b.phase = 0.0f;
            next += (0.015 + 0.06 * (when.next() * 0.5 + 0.5)) * (1.0 + 4.0 * static_cast<double>(t));
        }
        float bubble = 0.0f;
        for (Bubble& b : bubbles) {
            const float age = t - b.start;
            if (b.start < 0.0f || age >= 0.05f) continue;
            b.phase += 2.0f * k_pi * b.pitch * (1.0f + age * 15.0f) / static_cast<float>(k_sample_rate);
            const float shape = std::min(age / 0.002f, 1.0f) * std::exp(-age / 0.012f) * (1.0f - age / 0.05f);
            bubble += std::sin(b.phase) * shape * std::exp(-t / 0.5f);
        }
        s.samples[i] = slap * 1.2f + spray + low * 0.9f + bubble * 0.6f;
    }
    // The end let down to silence (no cut).
    const usize tail = k_sample_rate / 10;
    for (usize k = 0; k < tail; ++k) s.samples[s.samples.size() - 1 - k] *= static_cast<float>(k) / tail;
    normalize(s, 1.0f);
    return s;
}

Sound synth_plop(u32 seed) {
    Sound s;
    s.samples.resize(k_sample_rate / 4);
    Noise noise(seed);
    float hi = 0.0f, phase = 0.0f;
    const float a_hi = pole(6000.0f);
    const float pitch = 700.0f + 200.0f * noise.next();
    for (usize i = 0; i < s.samples.size(); ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(k_sample_rate);
        hi += a_hi * (noise.next() - hi);
        const float click = hi * std::min(t / 0.001f, 1.0f) * std::exp(-t / 0.004f);
        // The bubble: from 15 ms, rising an octave as it rings down.
        const float age = t - 0.015f;
        float bubble = 0.0f;
        if (age > 0.0f) {
            phase += 2.0f * k_pi * pitch * (1.0f + std::min(age / 0.06f, 1.0f)) / static_cast<float>(k_sample_rate);
            bubble = std::sin(phase) * std::min(age / 0.002f, 1.0f) * std::exp(-age / 0.05f);
        }
        s.samples[i] = click * 0.5f + bubble;
    }
    const usize tail = k_sample_rate / 20; // (let down to silence: no cut)
    for (usize k = 0; k < tail; ++k) s.samples[s.samples.size() - 1 - k] *= static_cast<float>(k) / tail;
    normalize(s, 1.0f);
    return s;
}

Sound make_loop(std::vector<float> made, usize n, usize fade, float rms) {
    n = std::min(n, made.size());
    fade = std::min({fade, made.size() - n, n});
    return looped(std::move(made), n, fade, std::isfinite(rms) ? std::max(rms, 0.0f) : 0.0f);
}

Sound synth_lapping(u32 seed) {
    // 8 s: a wavelet washing in every 0.7 to 1.9 s (a swell of dull noise,
    // in over 80 ms, back over about half a second, with a splash of
    // brighter noise at its top), over a low murmur.
    const usize n = k_sample_rate * 8, fade = k_sample_rate / 2;
    std::vector<float> out(n + fade, 0.0f);
    Noise noise(seed);
    Noise when(seed * 31u + 7u);
    float low = 0.0f, mid = 0.0f, murmur = 0.0f;
    const float a_low = pole(500.0f), a_mid = pole(2500.0f), a_murmur = pole(150.0f);
    double next = 0.2;
    double start = -10.0;
    float strength = 0.0f;
    for (usize i = 0; i < out.size(); ++i) {
        const double t = static_cast<double>(i) / k_sample_rate;
        if (t >= next) {
            start = next;
            strength = 0.6f + 0.4f * (when.next() * 0.5f + 0.5f);
            next += 0.7 + 1.2 * (when.next() * 0.5 + 0.5);
        }
        const float x = noise.next();
        low += a_low * (x - low);
        mid += a_mid * (x - mid);
        murmur += a_murmur * (x - murmur);
        const float age = static_cast<float>(t - start);
        const float wash = age < 0.08f ? age / 0.08f : std::exp(-(age - 0.08f) / 0.25f);
        const float top = age > 0.05f && age < 0.25f ? std::sin((age - 0.05f) / 0.2f * k_pi) : 0.0f;
        out[i] = strength * (wash * low * 2.0f + top * mid * 0.6f) + murmur * 1.5f;
    }
    return looped(std::move(out), n, fade, 0.12f);
}

Sound synth_river(u32 seed) {
    // 6 s: rushing water (noise from 200 Hz to 6 kHz, its level wandering a
    // little) and bubbles (short rising tones, 300 to 1200 Hz, a few a second).
    const usize n = k_sample_rate * 6, fade = k_sample_rate / 2;
    std::vector<float> out(n + fade, 0.0f);
    Noise noise(seed);
    Noise when(seed * 17u + 3u);
    float lo = 0.0f, hi = 0.0f, drift = 0.0f;
    const float a_lo = pole(200.0f), a_hi = pole(6000.0f), a_drift = pole(1.5f);
    double next = 0.05, start = -10.0;
    float pitch = 600.0f, phase = 0.0f;
    for (usize i = 0; i < out.size(); ++i) {
        const double t = static_cast<double>(i) / k_sample_rate;
        const float x = noise.next();
        hi += a_hi * (x - hi);
        lo += a_lo * (hi - lo);
        drift += a_drift * (noise.next() - drift);
        if (t >= next) {
            start = next;
            pitch = 300.0f + 900.0f * (when.next() * 0.5f + 0.5f);
            next += 0.05 + 0.3 * (when.next() * 0.5 + 0.5);
        }
        const float age = static_cast<float>(t - start);
        float bubble = 0.0f;
        if (age < 0.03f) {
            phase += 2.0f * k_pi * pitch * (1.0f + age * 20.0f) / static_cast<float>(k_sample_rate);
            bubble = std::sin(phase) * std::exp(-age / 0.008f);
        }
        out[i] = (hi - lo) * (1.0f + drift * 3.0f) + bubble * 0.15f;
    }
    return looped(std::move(out), n, fade, 0.12f);
}

Sound synth_underwater(u32 seed) {
    // 6 s: the deep's low rumble (noise under 250 Hz, slowly swelling) and
    // now and then a bubble rising (a falling knock, low).
    const usize n = k_sample_rate * 6, fade = k_sample_rate / 2;
    std::vector<float> out(n + fade, 0.0f);
    Noise noise(seed);
    Noise when(seed * 13u + 5u);
    float lo = 0.0f, lo2 = 0.0f, swell = 0.0f;
    const float a_lo = pole(250.0f), a_swell = pole(0.3f);
    double next = 0.5, start = -10.0;
    float phase = 0.0f, pitch = 200.0f;
    for (usize i = 0; i < out.size(); ++i) {
        const double t = static_cast<double>(i) / k_sample_rate;
        const float x = noise.next();
        lo += a_lo * (x - lo);
        lo2 += a_lo * (lo - lo2);
        swell += a_swell * (noise.next() - swell);
        if (t >= next) {
            start = next;
            pitch = 120.0f + 200.0f * (when.next() * 0.5f + 0.5f);
            next += 0.6 + 1.5 * (when.next() * 0.5 + 0.5);
        }
        const float age = static_cast<float>(t - start);
        float bubble = 0.0f;
        if (age < 0.06f) {
            phase += 2.0f * k_pi * pitch * (1.0f - age * 4.0f) / static_cast<float>(k_sample_rate);
            bubble = std::sin(phase) * std::exp(-age / 0.02f);
        }
        out[i] = lo2 * (1.0f + swell * 8.0f) + bubble * 0.3f;
    }
    return looped(std::move(out), n, fade, 0.15f);
}

Sound synth_click() {
    Sound s;
    s.samples.assign(k_sample_rate / 100, 0.0f); // 10 ms
    s.samples[0] = 1.0f;
    for (usize i = 1; i < s.samples.size(); ++i) s.samples[i] = s.samples[i - 1] * 0.7f;
    return s;
}

Sound synth_tone(float hz, float seconds, float level) {
    Sound s;
    const double len = std::isfinite(seconds) ? std::clamp(static_cast<double>(seconds), 0.0, 60.0) : 0.0;
    const usize n = static_cast<usize>(len * k_sample_rate);
    s.samples.resize(n);
    const float f = std::isfinite(hz) ? std::clamp(hz, 1.0f, 20000.0f) : 440.0f;
    level = std::isfinite(level) ? std::clamp(level, -4.0f, 4.0f) : 0.0f;
    const usize fade = k_sample_rate / 200;
    for (usize i = 0; i < n; ++i) {
        const float ramp = std::min({1.0f, static_cast<float>(i) / fade, static_cast<float>(n - i) / fade});
        s.samples[i] = level * ramp * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * f * static_cast<double>(i) / k_sample_rate));
    }
    return s;
}

std::optional<Sound> decode_wav(std::span<const u8> b) {
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0)
        return std::nullopt;
    u16 format = 0, channels = 0, bits = 0;
    u32 rate = 0;
    std::span<const u8> data;
    bool have_fmt = false, have_data = false;
    for (usize at = 12; at + 8 <= b.size();) {
        const u32 size = read_u32(b, at + 4);
        const usize body = at + 8;
        if (size > b.size() - body) {
            // A truncated last chunk: take what there is of the data.
            if (std::memcmp(b.data() + at, "data", 4) == 0) {
                data = b.subspan(body);
                have_data = true;
            }
            break;
        }
        if (std::memcmp(b.data() + at, "fmt ", 4) == 0 && size >= 16) {
            format = read_u16(b, body);
            channels = read_u16(b, body + 2);
            rate = read_u32(b, body + 4);
            bits = read_u16(b, body + 14);
            if (format == 0xFFFE && size >= 26) format = read_u16(b, body + 24); // extensible: its sub-format
            have_fmt = true;
        } else if (std::memcmp(b.data() + at, "data", 4) == 0) {
            data = b.subspan(body, size);
            have_data = true;
        }
        at = body + size + (size & 1u);
    }
    if (!have_fmt || !have_data || channels == 0 || channels > 8 || rate < 1000 || rate > 384000) return std::nullopt;
    const bool pcm16 = format == 1 && bits == 16, float32 = format == 3 && bits == 32;
    if (!pcm16 && !float32) return std::nullopt;
    const usize frame_bytes = static_cast<usize>(channels) * (bits / 8);
    // Past ten minutes (at its own rate) is never read: not allocated either.
    const usize cap = static_cast<usize>(std::ceil(static_cast<double>(k_max_wav_frames) * rate / k_sample_rate)) + 1;
    const usize frames = std::min(data.size() / frame_bytes, cap);
    std::vector<float> mono(frames);
    for (usize f = 0; f < frames; ++f) {
        float sum = 0.0f;
        for (u16 c = 0; c < channels; ++c) {
            const usize at = f * frame_bytes + c * (bits / 8);
            if (pcm16) {
                sum += static_cast<float>(static_cast<i16>(read_u16(data, at))) / 32768.0f;
            } else {
                const u32 raw = read_u32(data, at);
                float v;
                std::memcpy(&v, &raw, 4);
                sum += std::isfinite(v) ? std::clamp(v, -4.0f, 4.0f) : 0.0f;
            }
        }
        mono[f] = sum / channels;
    }
    Sound s;
    if (rate == k_sample_rate) {
        s.samples = std::move(mono);
    } else if (frames > 0) {
        const double step = static_cast<double>(rate) / k_sample_rate;
        const usize out = std::min(static_cast<usize>(static_cast<double>(frames) / step), k_max_wav_frames);
        s.samples.resize(out);
        for (usize i = 0; i < out; ++i) {
            const double p = static_cast<double>(i) * step;
            const usize a = std::min(static_cast<usize>(p), frames - 1), c = std::min(a + 1, frames - 1);
            const float t = static_cast<float>(p - static_cast<double>(a));
            s.samples[i] = mono[a] + (mono[c] - mono[a]) * t;
        }
    }
    if (s.samples.size() > k_max_wav_frames) s.samples.resize(k_max_wav_frames);
    return s;
}

std::optional<Sound> load_wav(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    const std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return decode_wav(bytes);
}

std::vector<u8> encode_wav(std::span<const float> interleaved, u32 channels) {
    if (channels < 1 || channels > 8) return {};
    const u32 ch = channels;
    const usize frame_bytes = 2 * ch;
    const usize most = (0xFFFFFFFFu - 64) / frame_bytes * frame_bytes;
    const u32 data_bytes = static_cast<u32>(std::min(interleaved.size() / ch * frame_bytes, most));
    std::vector<u8> b;
    b.reserve(44 + data_bytes);
    b.insert(b.end(), {'R', 'I', 'F', 'F'});
    put_u32(b, 36 + data_bytes);
    b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put_u32(b, 16);
    put_u16(b, 1);
    put_u16(b, static_cast<u16>(ch));
    put_u32(b, k_sample_rate);
    put_u32(b, k_sample_rate * ch * 2);
    put_u16(b, static_cast<u16>(ch * 2));
    put_u16(b, 16);
    b.insert(b.end(), {'d', 'a', 't', 'a'});
    put_u32(b, data_bytes);
    for (usize k = 0; k < data_bytes / 2; ++k) {
        const float x = std::isfinite(interleaved[k]) ? std::clamp(interleaved[k], -1.0f, 1.0f) : 0.0f;
        put_u16(b, static_cast<u16>(static_cast<i16>(std::lround(x * 32767.0f))));
    }
    return b;
}

bool save_wav(const std::string& path, std::span<const float> interleaved, u32 channels) {
    const std::vector<u8> bytes = encode_wav(interleaved, channels);
    if (bytes.empty()) return false;
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

} // namespace giant::audio
