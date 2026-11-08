// Audio: the mixer plays taps late, from their side, duller through a
// lowpass, with a tail; propagation finds the direct way and echoes off
// what the rays hit; made sounds and WAV files.
#include "giant/audio/mixer.h"
#include "giant/audio/propagation.h"
#include "giant/audio/sounds.h"

#include "giant_test.h"

#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

using namespace giant;
using namespace giant::audio;

namespace {

std::vector<float> render(Mixer& m, double seconds) {
    std::vector<float> out(static_cast<usize>(seconds * k_sample_rate) * 2);
    // In audio-callback-sized pieces.
    for (usize at = 0; at < out.size() / 2;) {
        const u32 n = static_cast<u32>(std::min<usize>(480, out.size() / 2 - at));
        m.render(out.data() + at * 2, n);
        at += n;
    }
    return out;
}

// The first frame on `channel` past `level`, or -1.
long first_over(const std::vector<float>& out, int channel, float level) {
    for (usize f = 0; f < out.size() / 2; ++f)
        if (std::fabs(out[f * 2 + static_cast<usize>(channel)]) > level) return static_cast<long>(f);
    return -1;
}

double energy(const std::vector<float>& out, double from, double to) {
    double e = 0.0;
    for (usize f = static_cast<usize>(from * k_sample_rate); f < std::min(out.size() / 2, static_cast<usize>(to * k_sample_rate)); ++f)
        e += out[f * 2] * out[f * 2] + out[f * 2 + 1] * out[f * 2 + 1];
    return e;
}

// A world of planes: each point p with n . p = offset is a surface.
struct PlaneWorld {
    struct Plane {
        DVec3 normal;
        double offset;
        float hardness;
        // Only within this box (for walls of a size).
        DVec3 lo{-1e9, -1e9, -1e9}, hi{1e9, 1e9, 1e9};
    };
    std::vector<Plane> planes;
    mutable u32 casts = 0;
    RayCast cast() const {
        return [this](const DVec3& from, const Vec3& dir, double max_distance) {
            ++casts;
            RayHit best;
            for (const Plane& p : planes) {
                const DVec3 d{dir.x, dir.y, dir.z};
                const double denom = dot(p.normal, d);
                if (std::fabs(denom) < 1e-9) continue;
                const double t = (p.offset - dot(p.normal, from)) / denom;
                if (t < 1e-6 || t > max_distance || (best.hit && t >= best.distance)) continue;
                const DVec3 at = from + d * t;
                if (at.x < p.lo.x || at.y < p.lo.y || at.z < p.lo.z || at.x > p.hi.x || at.y > p.hi.y || at.z > p.hi.z)
                    continue;
                best.hit = true;
                best.distance = t;
                const DVec3 n = denom < 0.0 ? p.normal : -p.normal;
                best.normal = {static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z)};
                best.hardness = p.hardness;
            }
            return best;
        };
    }
};

} // namespace

GIANT_TEST("audio mixer: a tap plays late by its delay, from its side; the lowpass dulls it") {
    Mixer m;
    const SoundId click = m.add_sound(synth_click());
    GIANT_CHECK(click != k_no_sound && m.sound(click) != nullptr && m.sound(99) == nullptr);
    m.set_listener({1, 0, 0}, {0, 1, 0});
    // From the right, half a second late.
    m.play({click, {Tap{0.5, 0.5f, 20000.0f, {1, 0, 0}}}, 0.0f, 1.0f});
    std::vector<float> out = render(m, 1.0);
    const long right = first_over(out, 1, 0.05f);
    GIANT_CHECK(right >= 23990 && right <= 24010);
    GIANT_CHECK(first_over(out, 0, 0.01f) == -1); // nothing on the left
    GIANT_CHECK(m.stats().frames == k_sample_rate);
    GIANT_CHECK(m.stats().voices == 0); // done

    // The same from the front with a 300 Hz lowpass: on both sides, its edge softened.
    m.play({click, {Tap{0.0, 0.5f, 300.0f, {0, 0, 1}}}, 0.0f, 1.0f});
    out = render(m, 0.1);
    float peak_l = 0, peak_r = 0;
    for (usize f = 0; f < out.size() / 2; ++f) {
        peak_l = std::max(peak_l, std::fabs(out[f * 2]));
        peak_r = std::max(peak_r, std::fabs(out[f * 2 + 1]));
    }
    GIANT_CHECK(std::fabs(peak_l - peak_r) < 1e-4f);
    GIANT_CHECK(peak_l > 0.0f && peak_l < 0.5f * 0.71f * 0.2f);

    // Bad taps are dropped, not played.
    m.play({click, {Tap{-1.0, 1.0f}, Tap{std::nan(""), 1.0f}, Tap{0.0, std::numeric_limits<float>::infinity()}}, 0.0f, 1.0f});
    out = render(m, 0.05);
    GIANT_CHECK(first_over(out, 0, 1e-6f) == -1);
}

GIANT_TEST("audio mixer: past k_max_voices the quietest go; the limiter keeps it under 1; stop_all") {
    Mixer m;
    const SoundId tone = m.add_sound(synth_tone(200.0f, 2.0f, 0.5f));
    for (usize k = 0; k < k_max_voices; ++k) m.play({tone, {Tap{0.0, 0.01f}}, 0.0f, 1.0f});
    std::vector<float> out = render(m, 0.01);
    GIANT_CHECK(m.stats().voices == k_max_voices);
    GIANT_CHECK(m.stats().dropped == 0);
    // A loud one takes a quiet one's place; a quiet one is refused.
    m.play({tone, {Tap{0.0, 4.0f}, Tap{0.01, 4.0f}}, 0.0f, 1.0f});
    m.play({tone, {Tap{0.0, 0.001f}}, 0.0f, 1.0f});
    out = render(m, 0.2);
    GIANT_CHECK(m.stats().dropped == 2);
    GIANT_CHECK(m.stats().voices == k_max_voices);
    GIANT_CHECK(m.stats().peak > 1.5f); // loud before the limiter...
    for (float x : out) GIANT_CHECK(std::fabs(x) <= 1.0f); // ...not after
    m.stop_all();
    out = render(m, 0.05);
    GIANT_CHECK(m.stats().voices == 0);
    GIANT_CHECK(first_over(out, 0, 1e-6f) == -1);
}

GIANT_TEST("audio mixer: the reverb send rings on after the sound, longer for a longer tail") {
    auto tail = [](float seconds) {
        Mixer m;
        const SoundId click = m.add_sound(synth_gunshot());
        m.play({click, {Tap{0.0, 0.2f}}, 0.5f, seconds});
        const std::vector<float> out = render(m, 3.0);
        GIANT_CHECK(std::fabs(m.stats().reverb_seconds - seconds) < 0.05f);
        return energy(out, 1.0, 3.0);
    };
    const double dry = [] {
        Mixer m;
        const SoundId s = m.add_sound(synth_gunshot());
        m.play({s, {Tap{0.0, 0.2f}}, 0.0f, 1.0f});
        return energy(render(m, 3.0), 1.0, 3.0);
    }();
    const double short_tail = tail(0.5f), long_tail = tail(4.0f);
    GIANT_CHECK(dry < 1e-9);
    GIANT_CHECK(short_tail > 0.0 && long_tail > short_tail * 10.0);
}

GIANT_TEST("audio mixer: played from one thread, rendered on another") {
    Mixer m;
    const SoundId click = m.add_sound(synth_click());
    std::atomic<bool> done{false};
    std::thread audio([&] {
        std::vector<float> out(512 * 2);
        while (!done.load()) m.render(out.data(), 512);
    });
    for (int k = 0; k < 2000; ++k) {
        m.play({click, {Tap{0.001 * (k % 10), 0.1f, 8000.0f, {0, 0, 1}}}, 0.1f, 1.5f});
        if (k % 100 == 0) m.set_listener({1, 0, 0}, {0, 1, 0});
        if (k % 500 == 0) m.add_sound(synth_click());
        m.volume = 0.8f;
    }
    done = true;
    audio.join();
    GIANT_CHECK(m.stats().frames > 0);
}

GIANT_TEST("audio propagation: 343 m off is heard a second late, quieter and duller") {
    PlaneWorld none;
    const Heard h = trace({343.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, none.cast(), {});
    GIANT_CHECK(h.taps.size() == 1);
    GIANT_CHECK(std::fabs(h.taps[0].delay - 1.0) < 1e-9);
    GIANT_CHECK(h.taps[0].gain < 1.0f / 343.0f && h.taps[0].gain > 0.5f / 343.0f);
    GIANT_CHECK(h.taps[0].lowpass < 10000.0f);
    GIANT_CHECK(h.taps[0].direction.x > 0.999f);
    GIANT_CHECK(!h.occluded);
    GIANT_CHECK(h.openness == 1.0f && h.reverb_send == 0.0f);
    GIANT_CHECK(air_lowpass(3000.0) < air_lowpass(1000.0) && air_lowpass(1000.0) < air_lowpass(10.0));
    GIANT_CHECK(distance_gain(0.1) == 1.0f && std::fabs(distance_gain(2.0) - 0.5f) < 1e-3f);
}

GIANT_TEST("audio propagation: a cliff 343 m off answers 2 s later from its side; a wall between muffles") {
    // Flat soft ground and a hard cliff face at x = 343.
    PlaneWorld w;
    w.planes.push_back({{0, 1, 0}, 0.0, 0.3f});
    w.planes.push_back({{1, 0, 0}, 343.0, 0.8f});
    const DVec3 here{0.0, 1.7, 0.0};
    EchoSettings s;
    s.rays = 128;
    const Heard h = trace({0.0, 1.7, 2.0}, here, w.cast(), s);
    GIANT_CHECK(h.taps.size() >= 2);
    GIANT_CHECK(h.taps.size() <= k_max_taps);
    GIANT_CHECK(h.casts == w.casts);
    GIANT_CHECK(h.casts <= 1 + 2 * s.rays);
    // The cliff's answer: round 686 m, from +x.
    bool cliff = false;
    for (usize k = 1; k < h.taps.size(); ++k) {
        const Tap& t = h.taps[k];
        if (t.delay > 1.9 && t.delay < 2.3 && t.direction.x > 0.8f) {
            cliff = true;
            GIANT_CHECK(t.gain < h.taps[0].gain);
            GIANT_CHECK(t.lowpass < h.taps[0].lowpass);
        }
    }
    GIANT_CHECK(cliff);
    GIANT_CHECK(h.openness > 0.2f && h.openness < 0.8f); // the sky, over half the ground and cliff
    GIANT_CHECK(h.reverb_send > 0.0f);

    // A wall between: muffled, quieter.
    PlaneWorld walled = w;
    walled.planes.push_back({{0, 0, 1}, 1.0, 0.9f, {-5, 0, 0.5}, {5, 4, 1.5}});
    const Heard m = trace({0.0, 1.7, 2.0}, here, walled.cast(), s);
    GIANT_CHECK(m.occluded);
    GIANT_CHECK(m.taps[0].gain < h.taps[0].gain * 0.5f);
    GIANT_CHECK(m.taps[0].lowpass <= s.occluded_lowpass);
}

GIANT_TEST("audio propagation: a hard small room rings longer and louder than a big open valley") {
    auto room = [](double half, float hardness) {
        PlaneWorld w;
        for (int axis = 0; axis < 3; ++axis)
            for (int sgn = -1; sgn <= 1; sgn += 2) {
                DVec3 n{0, 0, 0};
                (axis == 0 ? n.x : axis == 1 ? n.y : n.z) = 1.0;
                w.planes.push_back({n, sgn * half, hardness});
            }
        return w;
    };
    const PlaneWorld hall = room(10.0, 0.9f), padded = room(10.0, 0.2f), big = room(100.0, 0.9f);
    const Heard a = trace({1, 0, 0}, {-1, 0, 0}, hall.cast(), {});
    const Heard b = trace({1, 0, 0}, {-1, 0, 0}, padded.cast(), {});
    const Heard c = trace({1, 0, 0}, {-1, 0, 0}, big.cast(), {});
    GIANT_CHECK(a.openness == 0.0f);
    GIANT_CHECK(a.mean_free_path > 9.0 && a.mean_free_path < 18.0);
    GIANT_CHECK(a.reverb_seconds > b.reverb_seconds);
    GIANT_CHECK(c.reverb_seconds > a.reverb_seconds);
    GIANT_CHECK(a.reverb_send > b.reverb_send);
    GIANT_CHECK(a.reverb_seconds >= 0.5f);
    // A play from it: the loudness scales every tap and the send.
    const Play p = to_play(1, a, 2.0f);
    GIANT_CHECK(p.taps.size() == a.taps.size() && std::fabs(p.taps[0].gain - a.taps[0].gain * 2.0f) < 1e-6f);
    GIANT_CHECK(std::fabs(p.reverb_send - a.reverb_send * 2.0f) < 1e-6f);
    // No cast: only the direct way.
    const Heard d = trace({10, 0, 0}, {0, 0, 0}, RayCast{}, {});
    GIANT_CHECK(d.taps.size() == 1 && d.casts == 0);
}

GIANT_TEST("audio sounds: made sounds peak where they should; WAV round trips and resamples") {
    const Sound shot = synth_gunshot();
    float peak = 0.0f;
    for (float x : shot.samples) peak = std::max(peak, std::fabs(x));
    GIANT_CHECK(std::fabs(peak - 1.0f) < 1e-4f);
    GIANT_CHECK(shot.seconds() > 0.5 && shot.seconds() < 0.7);
    // Most of it is in the first 0.2 s.
    double first = 0, rest = 0;
    for (usize i = 0; i < shot.samples.size(); ++i)
        (i < k_sample_rate / 5 ? first : rest) += shot.samples[i] * shot.samples[i];
    GIANT_CHECK(first > rest * 10.0);
    GIANT_CHECK(synth_gunshot(1).samples == synth_gunshot(1).samples);
    GIANT_CHECK(synth_gunshot(1).samples != synth_gunshot(2).samples);
    const Sound crack = synth_crack();
    GIANT_CHECK(crack.samples[0] == 1.0f && crack.samples[24] < -0.9f);

    // Stereo out, mono back (the ears averaged).
    std::vector<float> stereo(2000);
    for (usize f = 0; f < 1000; ++f) {
        stereo[f * 2] = 0.5f;
        stereo[f * 2 + 1] = -0.25f;
    }
    const std::vector<u8> bytes = encode_wav(stereo, 2);
    GIANT_CHECK(bytes.size() == 44 + 4000);
    const std::optional<Sound> back = decode_wav(bytes);
    GIANT_CHECK(back.has_value() && back->samples.size() == 1000);
    GIANT_CHECK(std::fabs(back->samples[500] - 0.125f) < 1e-3f);
    // At 24 kHz: twice the samples at 48.
    std::vector<u8> half = encode_wav(std::vector<float>(1000, 0.3f), 1);
    const u32 rate = 24000, bytes_per_second = 48000;
    for (int k = 0; k < 4; ++k) {
        half[static_cast<usize>(24 + k)] = static_cast<u8>(rate >> (8 * k));
        half[static_cast<usize>(28 + k)] = static_cast<u8>(bytes_per_second >> (8 * k));
    }
    const std::optional<Sound> up = decode_wav(half);
    GIANT_CHECK(up.has_value() && up->samples.size() == 2000 && std::fabs(up->samples[777] - 0.3f) < 1e-3f);
    // Not WAV, or cut short.
    GIANT_CHECK(!decode_wav(std::vector<u8>{1, 2, 3}).has_value());
    const std::vector<u8> cut(bytes.begin(), bytes.begin() + 30);
    GIANT_CHECK(!decode_wav(cut).has_value());
    const std::vector<u8> short_data(bytes.begin(), bytes.begin() + 44 + 400);
    GIANT_CHECK(decode_wav(short_data).has_value() && decode_wav(short_data)->samples.size() == 100);
}
