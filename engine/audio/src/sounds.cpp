// Made sounds and WAV files.
#include "giant/audio/sounds.h"

#include "giant/core/math.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

namespace giant::audio {

namespace {

// Small deterministic noise, -1 to 1.
struct Noise {
    u32 state;
    explicit Noise(u32 seed) : state(seed * 747796405u + 2891336453u) {}
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
    const usize frames = data.size() / frame_bytes;
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
                sum += std::isfinite(v) ? v : 0.0f;
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
    const u32 ch = std::clamp<u32>(channels, 1, 8);
    const usize samples = interleaved.size() / ch * ch;
    const u32 data_bytes = static_cast<u32>(std::min<usize>(samples * 2, 0xFFFFFFFFu - 64));
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
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

} // namespace giant::audio
