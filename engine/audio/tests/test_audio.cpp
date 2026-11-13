// Audio: the mixer plays taps late, from their side, duller through a
// lowpass, with a tail; propagation finds the direct way and echoes off
// what the rays hit; made sounds and WAV files.
#include "giant/audio/mixer.h"
#include "giant/audio/propagation.h"
#include "giant/audio/shot.h"
#include "giant/audio/sounds.h"

#include "giant_test.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <limits>
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
    GIANT_CHECK(m.stats().stolen == 1 && m.stats().dropped == 1);
    GIANT_CHECK(m.stats().voices == k_max_voices);
    GIANT_CHECK(m.stats().peak > 1.5f); // loud before the limiter...
    for (float x : out) GIANT_CHECK(std::fabs(x) <= 1.0f); // ...not after
    m.stop_all();
    out = render(m, 0.05);
    GIANT_CHECK(m.stats().voices == 0);
    GIANT_CHECK(first_over(out, 0, 1e-6f) == -1);
}

GIANT_TEST("audio mixer: voices whose loud part is over give way to new ones (sustained fire keeps sounding)") {
    Mixer m;
    const SoundId click = m.add_sound(synth_click());
    // Each a loud direct click now and a faint echo 15 s on: their loud part is soon over.
    for (usize k = 0; k < k_max_voices; ++k) m.play({click, {Tap{0.0, 1.0f}, Tap{15.0, 0.01f}}, 0.0f, 1.0f});
    render(m, 0.1);
    GIANT_CHECK(m.stats().voices == k_max_voices);
    m.play({click, {Tap{0.0, 1.0f, 20000.0f, {1, 0, 0}}}, 0.0f, 1.0f});
    const std::vector<float> out = render(m, 0.05);
    GIANT_CHECK(m.stats().stolen == 1 && m.stats().dropped == 0);
    GIANT_CHECK(first_over(out, 1, 0.5f) >= 0); // heard
}

GIANT_TEST("audio mixer: a voice cut short before it was heard makes no sound; many cut at once all fade") {
    Mixer m;
    m.set_listener({1, 0, 0}, {0, 1, 0});
    const SoundId tone = m.add_sound(synth_tone(300.0f, 1.0f, 0.5f));
    m.play({tone, {Tap{0.0, 4.0f, 20000.0f, {-1, 0, 0}}}, 0.0f, 1.0f}); // hard left, loud, then over
    render(m, 1.1);
    // 65 at once (one batch), hard right: the 65th takes a voice none has heard yet.
    for (usize k = 0; k <= k_max_voices; ++k) m.play({tone, {Tap{0.0, 0.5f, 20000.0f, {1, 0, 0}}}, 0.0f, 1.0f});
    std::vector<float> out = render(m, 0.05);
    float left = 0.0f;
    for (usize f = 0; f < out.size() / 2; ++f) left = std::max(left, std::fabs(out[f * 2]));
    GIANT_CHECK(left < 1e-3f);
    // A batch cutting 12 short (heard ones), of a steady level: it falls smoothly, never by a voice at once.
    Mixer d;
    d.set_listener({1, 0, 0}, {0, 1, 0});
    Sound level;
    level.samples.assign(k_sample_rate, 0.01f);
    const SoundId steady = d.add_sound(std::move(level));
    for (usize k = 0; k < k_max_voices; ++k) d.play({steady, {Tap{0.0, 0.5f, 20000.0f, {1, 0, 0}}}, 0.0f, 1.0f});
    render(d, 0.05);
    // Their own sound starts 10 ms on, once the cut ones have faded.
    for (usize k = 0; k < 12; ++k) d.play({steady, {Tap{0.01, 0.6f, 20000.0f, {1, 0, 0}}}, 0.0f, 1.0f});
    out = render(d, 0.008);
    GIANT_CHECK(d.stats().stolen == 12);
    GIANT_CHECK(std::fabs(out[1] - 0.32f) < 1e-3f && std::fabs(out[out.size() - 1] - 0.26f) < 1e-3f);
    float step = 0.0f;
    for (usize f = 1; f < out.size() / 2; ++f) step = std::max(step, std::fabs(out[f * 2 + 1] - out[f * 2 - 1]));
    GIANT_CHECK(step < 0.001f); // a cut voice would step by 0.005
}

GIANT_TEST("audio mixer: a huge sample never makes the output NaN; a dry play leaves the tail's length") {
    Mixer m;
    Sound huge;
    huge.samples = {3e38f, -3e38f, 1.0f};
    const SoundId h = m.add_sound(std::move(huge));
    GIANT_CHECK(m.sound(h)->samples[0] == 4.0f); // bounded on the way in
    const SoundId click = m.add_sound(synth_click());
    m.play({h, {Tap{0.0, 4.0f}}, 4.0f, 8.0f});
    std::vector<float> out = render(m, 0.5);
    m.play({click, {Tap{0.0, 0.5f}}, 0.0f, 1.0f}); // dry: the tail stays 8 s
    out = render(m, 1.5);
    for (float x : out) GIANT_CHECK(std::isfinite(x) && std::fabs(x) <= 1.0f);
    GIANT_CHECK(m.stats().reverb_seconds > 7.9f);
    // Many voices too loud at once: limited, finite.
    Sound tone = synth_tone(100.0f, 0.2f, 1e30f);
    GIANT_CHECK(std::fabs(tone.samples[5000]) <= 4.0f);
}

GIANT_TEST("audio mixer: a long tail dies to silence (no endless denormals)") {
    Mixer m;
    const SoundId shot = m.add_sound(synth_gunshot());
    m.play({shot, {Tap{0.0, 1.0f}}, 1.0f, 0.5f});
    std::vector<float> out = render(m, 20.0);
    usize nonzero = 0;
    for (usize k = out.size() - 9600; k < out.size(); ++k) nonzero += out[k] != 0.0f ? 1u : 0u;
    GIANT_CHECK(nonzero == 0); // the last 0.1 s exactly 0
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
        while (!done.load() || m.stats().frames == 0) m.render(out.data(), 512);
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

    // A wall 4 m high between: heard over its top, later by the detour, quieter, duller, from above.
    PlaneWorld walled = w;
    walled.planes.push_back({{0, 0, 1}, 1.0, 0.9f, {-5, 0, 0.5}, {5, 4, 1.5}});
    const Heard m = trace({0.0, 1.7, 2.0}, here, walled.cast(), s);
    GIANT_CHECK(m.occluded && m.diffracted);
    GIANT_CHECK(m.detour > 0.5 && m.detour < 8.0);
    GIANT_CHECK(std::fabs(m.taps[0].delay - (2.0 + m.detour) / k_speed_of_sound) < 1e-9);
    GIANT_CHECK(m.taps[0].gain < h.taps[0].gain * 0.5f);
    GIANT_CHECK(m.taps[0].lowpass < 3000.0f);
    GIANT_CHECK(m.taps[0].direction.y > 0.3f || std::fabs(m.taps[0].direction.x) > 0.3f);
    // A higher wall: a longer detour, quieter yet.
    PlaneWorld high = w;
    high.planes.push_back({{0, 0, 1}, 1.0, 0.9f, {-100, 0, 0.5}, {100, 30, 1.5}});
    const Heard hi = trace({0.0, 1.7, 2.0}, here, high.cast(), s);
    GIANT_CHECK(hi.occluded && hi.taps[0].gain < m.taps[0].gain);
    // ...so long a way over (about 57 m round) that through it is louder: heard through, not bent.
    GIANT_CHECK(!hi.diffracted && hi.taps[0].lowpass <= s.occluded_lowpass);
    GIANT_CHECK(std::fabs(hi.taps[0].gain - h.taps[0].gain * s.occluded_gain) < 1e-6f);
    // A wall just grazing the line (its top 10 cm over it): bent round by a hair, nearly as loud as open.
    PlaneWorld low = w;
    low.planes.push_back({{0, 0, 1}, 1.0, 0.9f, {-5, 0, 0.5}, {5, 1.8, 1.5}});
    const Heard g = trace({0.0, 1.7, 2.0}, here, low.cast(), s);
    GIANT_CHECK(g.occluded && g.diffracted && g.detour < 0.1);
    GIANT_CHECK(g.taps[0].gain > m.taps[0].gain && g.taps[0].gain > h.taps[0].gain * 0.2f);
    GIANT_CHECK(g.taps[0].direction.y > 0.0f);
    // No way round (a wall across everything): heard through it.
    PlaneWorld sealed = w;
    sealed.planes.push_back({{0, 0, 1}, 1.0, 0.9f});
    const Heard t = trace({0.0, 1.7, 2.0}, here, sealed.cast(), s);
    GIANT_CHECK(t.occluded && !t.diffracted && t.taps[0].lowpass <= s.occluded_lowpass);
    GIANT_CHECK(t.casts <= 1 + 84 + 2 * s.rays);
    EchoSettings flat = s;
    flat.diffract = false;
    GIANT_CHECK(!trace({0.0, 1.7, 2.0}, here, walled.cast(), flat).diffracted);
    GIANT_CHECK(diffraction_db(0.0) > 4.7f && diffraction_db(0.0) < 4.8f && diffraction_db(1e9) == 30.0f);

    // Wet air (air 3): the far cliff's answer duller, quieter.
    EchoSettings wet = s;
    wet.air = 3.0f;
    const Heard r = trace({0.0, 1.7, 2.0}, here, w.cast(), wet);
    GIANT_CHECK(r.taps[0].lowpass < h.taps[0].lowpass);
    bool wet_cliff = false;
    for (usize k = 1; k < r.taps.size(); ++k) {
        const Tap& e = r.taps[k];
        if (e.delay > 1.9 && e.delay < 2.3 && e.direction.x > 0.8f) {
            for (usize j = 1; j < h.taps.size(); ++j) {
                const Tap& dry = h.taps[j];
                if (std::fabs(dry.delay - e.delay) < 1e-6) {
                    wet_cliff = true;
                    GIANT_CHECK(e.lowpass < dry.lowpass && e.gain < dry.gain);
                }
            }
        }
    }
    GIANT_CHECK(wet_cliff);
    GIANT_CHECK(air_lowpass(1000.0, 3.0f) == air_lowpass(3000.0));
    GIANT_CHECK(distance_gain(1000.0, 1.0, 2.0f) < distance_gain(1000.0));
}

GIANT_TEST("audio propagation: near echoes group by the first of them; too loud a play is scaled down alike") {
    // A wall 20 m off and a rifle at 5 m (loudness 100): direct and echo keep their balance.
    PlaneWorld w;
    w.planes.push_back({{1, 0, 0}, 20.0, 0.9f});
    const Heard h = trace({5, 0, 0}, {0, 0, 0}, w.cast(), {});
    GIANT_CHECK(h.taps.size() >= 2);
    const Play p = to_play(1, h, 100.0f);
    float loudest = 0;
    for (const Tap& t : p.taps) loudest = std::max(loudest, t.gain);
    GIANT_CHECK(std::fabs(loudest - 4.0f) < 1e-4f);
    GIANT_CHECK(std::fabs(p.taps[1].gain / p.taps[0].gain - h.taps[1].gain / h.taps[0].gain) < 1e-4f);

    // Eight echoes 5 ms apart, each twice as loud as the one before: grouped by the first of each
    // 12 ms (three groups), not chained after the loudest into one.
    const DVec3 here{0, 0, 0};
    u32 echo_rays = 0;
    const RayCast staircase = [&](const DVec3& from, const Vec3& dir, double) {
        RayHit r;
        if (!(from == here)) return r; // the listener sees every spot
        const u32 k = echo_rays++;
        const double d = 10.0 + 0.8575 * k, d7 = 10.0 + 0.8575 * 7;
        r.hit = true;
        r.distance = d;
        r.normal = -dir;
        r.hardness = static_cast<float>(std::pow(2.0, static_cast<double>(k) - 7.0) * (d / d7) * (d / d7));
        return r;
    };
    EchoSettings eight;
    eight.rays = 8;
    const Heard g = trace(here, here, staircase, eight);
    GIANT_CHECK(echo_rays == 8);
    GIANT_CHECK(g.taps.size() == 1 + 3);
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
    PlaneWorld valley; // open: ground and one far slope
    valley.planes.push_back({{0, 1, 0}, -1.7, 0.3f});
    valley.planes.push_back({{0.7071, 0.7071, 0}, 300.0, 0.7f, {200, -2, -300}, {500, 300, 300}});
    const Heard v = trace({1, 0, 0}, {-1, 0, 0}, valley.cast(), {});
    GIANT_CHECK(v.openness > 0.2f);
    const Heard a = trace({1, 0, 0}, {-1, 0, 0}, hall.cast(), {});
    const Heard b = trace({1, 0, 0}, {-1, 0, 0}, padded.cast(), {});
    const Heard c = trace({1, 0, 0}, {-1, 0, 0}, big.cast(), {});
    GIANT_CHECK(a.openness == 0.0f);
    GIANT_CHECK(a.mean_free_path > 9.0 && a.mean_free_path < 18.0);
    GIANT_CHECK(a.reverb_seconds > b.reverb_seconds);
    GIANT_CHECK(c.reverb_seconds > a.reverb_seconds);
    GIANT_CHECK(a.reverb_send > b.reverb_send);
    GIANT_CHECK(a.reverb_send > v.reverb_send * 3.0f);
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
    // 32-bit float, after an odd-sized chunk (padded to even): read, its values bounded.
    {
        std::vector<u8> f;
        auto put32 = [&](u32 v) { for (int k = 0; k < 4; ++k) f.push_back(static_cast<u8>(v >> (8 * k))); };
        auto put16 = [&](u16 v) { f.push_back(static_cast<u8>(v)); f.push_back(static_cast<u8>(v >> 8)); };
        const std::vector<float> values{0.25f, -0.5f, 1e30f};
        f.insert(f.end(), {'R', 'I', 'F', 'F'});
        put32(0);
        f.insert(f.end(), {'W', 'A', 'V', 'E', 'L', 'I', 'S', 'T'});
        put32(3);
        f.insert(f.end(), {'a', 'b', 'c', 0}); // 3 bytes and the pad
        f.insert(f.end(), {'f', 'm', 't', ' '});
        put32(16);
        put16(3);
        put16(1);
        put32(k_sample_rate);
        put32(k_sample_rate * 4);
        put16(4);
        put16(32);
        f.insert(f.end(), {'d', 'a', 't', 'a'});
        put32(12);
        for (float v : values) {
            u32 raw;
            std::memcpy(&raw, &v, 4);
            put32(raw);
        }
        const std::optional<Sound> got = decode_wav(f);
        GIANT_CHECK(got.has_value() && got->samples.size() == 3);
        GIANT_CHECK(got && got->samples[0] == 0.25f && got->samples[1] == -0.5f && got->samples[2] == 4.0f);
    }
    GIANT_CHECK(encode_wav(stereo, 9).empty() && encode_wav(stereo, 0).empty());
    // Not WAV, or cut short.
    GIANT_CHECK(!decode_wav(std::vector<u8>{1, 2, 3}).has_value());
    const std::vector<u8> cut(bytes.begin(), bytes.begin() + 30);
    GIANT_CHECK(!decode_wav(cut).has_value());
    const std::vector<u8> short_data(bytes.begin(), bytes.begin() + 44 + 400);
    GIANT_CHECK(decode_wav(short_data).has_value() && decode_wav(short_data)->samples.size() == 100);
}

GIANT_TEST("audio shot: a rifle round passing 20 m off at 300 m cracks well before the report, from upstream") {
    // Fired along +x from the origin at 880 m/s; the listener 300 m down range, 20 m to the side.
    const Flight f{{0, 0, 0}, {880, 0, 0}, 0.0007f, 4000.0};
    const DVec3 here{300, 0, 20};
    const Crack c = trace_crack(f, here);
    GIANT_CHECK(c.heard);
    const double report = std::hypot(300.0, 20.0) / k_speed_of_sound;
    GIANT_CHECK(c.delay < report - 0.3);
    // The bullet reached x = 300 at about 0.37 s; the crack comes a little after, from upstream.
    GIANT_CHECK(c.distance < 300.0 && c.distance > 250.0);
    GIANT_CHECK(c.direction.x < -0.3f && c.direction.z < -0.5f);
    GIANT_CHECK(c.speed > k_speed_of_sound && c.miss > 20.0 && c.miss < 60.0);
    // The Mach angle holds where it left: cos = c / v between the flight and the way to the listener.
    const double cosine = (here.x - c.from.x) / c.miss;
    GIANT_CHECK(std::fabs(cosine - k_speed_of_sound / c.speed) < 1e-3);
    // Nearer passes crack louder.
    GIANT_CHECK(trace_crack(f, {300, 0, 2}).gain > c.gain * 4.0f);

    // Behind the shooter, a subsonic round, one stopped short (a wall at 100 m): no crack.
    GIANT_CHECK(!trace_crack(f, {-50, 0, 5}).heard);
    GIANT_CHECK(!trace_crack({{0, 0, 0}, {300, 0, 0}}, here).heard);
    GIANT_CHECK(!trace_crack({{0, 0, 0}, {880, 0, 0}, 0.0007f, 100.0}, here).heard);
    const Flight draggy{{0, 0, 0}, {880, 0, 0}, 0.002f, 4000.0};
    // Drag 0.002: supersonic for 471 m. Far past that, ahead, the shock comes from where it slowed under
    // sound. Far out to the side near the muzzle (wider than the Mach cone there): none reaches.
    GIANT_CHECK(std::fabs(supersonic_range(draggy) - std::log(880.0 / k_speed_of_sound) / 0.002) < 0.5);
    const Crack ahead = trace_crack(draggy, {900, 0, 20});
    GIANT_CHECK(ahead.heard && ahead.distance > supersonic_range(draggy) - 10.0);
    GIANT_CHECK(!trace_crack(draggy, {100, 0, 400}).heard);
    GIANT_CHECK(trace_crack(draggy, {300, 0, 20}).heard);
    GIANT_CHECK(!trace_crack({{0, 0, 0}, {std::nanf(""), 0, 0}}, here).heard);
    // No end to the flight: as far as it stays fast (1000 km with no drag); a NaN end: none.
    const double inf = std::numeric_limits<double>::infinity();
    GIANT_CHECK(supersonic_range({{0, 0, 0}, {880, 0, 0}, 0.0f, inf}) == 1e6);
    GIANT_CHECK(std::fabs(supersonic_range({{0, 0, 0}, {880, 0, 0}, 0.002f, inf}) - supersonic_range(draggy)) < 1e-9);
    GIANT_CHECK(supersonic_range({{0, 0, 0}, {880, 0, 0}, 0.0f, std::nan("")}) == 0.0);
    // Stood on the line downrange: the crack from back up it.
    const Crack on = trace_crack(f, {200, 0, 0});
    GIANT_CHECK(on.heard && on.direction.x < -0.99f);
}

namespace {
// RMS of one channel over frames [from, to).
float rms_of(const std::vector<float>& out, int channel, usize from, usize to) {
    double sum = 0.0;
    to = std::min(to, out.size() / 2);
    for (usize f = from; f < to; ++f) sum += static_cast<double>(out[f * 2 + static_cast<usize>(channel)]) * out[f * 2 + static_cast<usize>(channel)];
    return to > from ? static_cast<float>(std::sqrt(sum / static_cast<double>(to - from))) : 0.0f;
}
// RMS of the change from one frame to the next on a channel: how much high there is.
float edge_of(const std::vector<float>& out, int channel, usize from, usize to) {
    double sum = 0.0;
    to = std::min(to, out.size() / 2);
    for (usize f = from + 1; f < to; ++f) {
        const double d = out[f * 2 + static_cast<usize>(channel)] - out[(f - 1) * 2 + static_cast<usize>(channel)];
        sum += d * d;
    }
    return to > from + 1 ? static_cast<float>(std::sqrt(sum / static_cast<double>(to - from - 1))) : 0.0f;
}
} // namespace

GIANT_TEST("audio sounds: make_loop cross-fades the end into the start: round the seam, no jump") {
    // A ramp from 0 to 1 over 1.5 s, looped at 1 s with 0.5 s of fade: cut plainly it would jump by 2/3.
    const usize n = k_sample_rate, fade = k_sample_rate / 2;
    std::vector<float> ramp(n + fade);
    for (usize i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<float>(i) / static_cast<float>(ramp.size());
    const Sound loop = make_loop(ramp, n, fade, 0.3f);
    GIANT_CHECK(loop.samples.size() == n);
    float step = 0.0f;
    for (usize i = 1; i < n; ++i) step = std::max(step, std::fabs(loop.samples[i] - loop.samples[i - 1]));
    const float seam = std::fabs(loop.samples.front() - loop.samples.back());
    std::printf("    ramp loop: seam %.5f, largest step in it %.5f\n", seam, step);
    GIANT_CHECK(seam < 1e-3f && step < 1e-3f);
    // Too short a `made`: cut to what there is, still a loop of finite samples.
    const Sound shorter = make_loop(std::vector<float>(100, 0.5f), 200, 50, 0.1f);
    GIANT_CHECK(shorter.samples.size() == 100);
    for (const float x : shorter.samples) GIANT_CHECK(std::isfinite(x));
}

GIANT_TEST("audio sounds: water loops play round with no seam and are as loud as each other") {
    for (const Sound& s : {synth_lapping(), synth_river(), synth_underwater()}) {
        GIANT_CHECK(s.samples.size() >= k_sample_rate * 6);
        double sum = 0.0;
        float peak = 0.0f;
        std::vector<float> steps;
        steps.reserve(s.samples.size());
        for (usize i = 0; i < s.samples.size(); ++i) {
            sum += static_cast<double>(s.samples[i]) * s.samples[i];
            peak = std::max(peak, std::fabs(s.samples[i]));
            if (i > 0) steps.push_back(std::fabs(s.samples[i] - s.samples[i - 1]));
        }
        const float rms = static_cast<float>(std::sqrt(sum / static_cast<double>(s.samples.size())));
        // Round the seam: an ordinary step, not a click (no bigger than 199 in 200 of its own steps).
        const usize at = steps.size() * 199 / 200;
        std::nth_element(steps.begin(), steps.begin() + static_cast<std::ptrdiff_t>(at), steps.end());
        const float seam = std::fabs(s.samples.front() - s.samples.back());
        std::printf("    loop of %.1f s: rms %.3f, seam step %.4f (199 in 200 under %.4f)\n",
                    static_cast<double>(s.samples.size()) / k_sample_rate, rms, seam, steps[at]);
        GIANT_CHECK(rms > 0.08f && rms < 0.2f && peak <= 0.95f && seam <= steps[at]);
    }
}

GIANT_TEST("audio mixer: an ambience plays round and round, eased in, from its side, eased out; muffled under water") {
    Mixer m;
    const SoundId river = m.add_sound(synth_river());
    const SoundId lapping = m.add_sound(synth_lapping());
    m.set_listener({1, 0, 0}, {0, 1, 0});
    // From the right, not spread: louder in the right ear; eased in over about a quarter second.
    Ambience a;
    a.sound = river;
    a.gain = 1.0f;
    a.direction = {1, 0, 0};
    a.spread = 0.0f;
    m.set_ambience(0, a);
    const auto out = render(m, 14.0); // (past the 6 s loop, twice)
    const float early = rms_of(out, 1, 0, k_sample_rate / 20);
    const float right = rms_of(out, 1, k_sample_rate * 2, k_sample_rate * 14);
    const float left = rms_of(out, 0, k_sample_rate * 2, k_sample_rate * 14);
    std::printf("    river: right %.3f left %.4f (first 50 ms %.3f)\n", right, left, early);
    GIANT_CHECK(right > 0.08f && left < right * 0.05f && early < right * 0.5f);
    GIANT_CHECK(m.stats().ambience == 1);
    // Spread all round: both ears alike.
    a.spread = 1.0f;
    m.set_ambience(0, a);
    const auto round = render(m, 2.0);
    const float l2 = rms_of(round, 0, k_sample_rate, k_sample_rate * 2), r2 = rms_of(round, 1, k_sample_rate, k_sample_rate * 2);
    GIANT_CHECK(std::fabs(l2 - r2) < 0.1f * r2 && l2 > 0.05f);
    // Under water: the highs go (the change frame to frame drops far more than the level).
    const float edge_open = edge_of(round, 0, k_sample_rate, k_sample_rate * 2);
    m.set_muffle(300.0f);
    const auto muffled = render(m, 2.0);
    const float edge_shut = edge_of(muffled, 0, k_sample_rate, k_sample_rate * 2);
    const float level_shut = rms_of(muffled, 0, k_sample_rate, k_sample_rate * 2);
    std::printf("    muffled: edge %.4f -> %.4f, level %.3f -> %.3f (muffle %.0f Hz)\n", edge_open, edge_shut, l2,
                level_shut, m.stats().muffle);
    GIANT_CHECK(edge_shut < edge_open * 0.15f && level_shut > 0.0f && m.stats().muffle < 320.0f);
    m.set_muffle(20000.0f);
    // Another sound in the slot: this one fades out first (in about a quarter second), then the new one in.
    a.sound = lapping;
    m.set_ambience(0, a);
    const auto changed = render(m, 3.0);
    // A dip to silence between (some 50 ms in the first 0.35 s all but silent), then the new one heard.
    float dip = 1.0f;
    for (usize w = k_sample_rate / 20; w + k_sample_rate / 20 <= k_sample_rate * 35 / 100; w += k_sample_rate / 100) {
        dip = std::min(dip, rms_of(changed, 0, w, w + k_sample_rate / 20));
    }
    const float soon = rms_of(changed, 0, k_sample_rate * 6 / 10, k_sample_rate);
    std::printf("    switched: quietest 50 ms %.4f, %.3f at 0.6-1 s, %.3f at 2-3 s\n", dip, soon,
                rms_of(changed, 0, k_sample_rate * 2, k_sample_rate * 3));
    GIANT_CHECK(dip < 0.005f && soon > 0.02f && rms_of(changed, 0, k_sample_rate * 2, k_sample_rate * 3) > 0.02f);
    // And it is the lapping now, not the river kept on: far fewer highs (frame to frame change) than the river's.
    const float edge_now = edge_of(changed, 0, k_sample_rate * 2, k_sample_rate * 3);
    std::printf("    edge: river %.4f, now %.4f\n", edge_open, edge_now);
    GIANT_CHECK(edge_now < edge_open * 0.6f);
    // Gone: eased out to silence; bad numbers are none.
    a.sound = k_no_sound;
    m.set_ambience(0, a);
    Ambience bad;
    bad.sound = river;
    bad.gain = std::numeric_limits<float>::quiet_NaN();
    bad.direction = {std::numeric_limits<float>::infinity(), 0, 0};
    m.set_ambience(1, bad);
    m.set_ambience(99, a); // (no such slot: nothing)
    const auto gone = render(m, 3.0);
    GIANT_CHECK(rms_of(gone, 0, k_sample_rate * 2, k_sample_rate * 3) < 1e-4f && m.stats().ambience == 0);
    for (const float x : gone) GIANT_CHECK(std::isfinite(x));
    // stop_all ends them in a moment, faded (no click: checked muffled, where a click would show).
    a.sound = river;
    m.set_ambience(2, a);
    m.set_muffle(400.0f);
    const auto before = render(m, 1.0);
    m.stop_all();
    const auto stopped = render(m, 0.5);
    float jump = std::fabs(stopped[0] - before[before.size() - 2]);
    for (usize f = 1; f < 480; ++f) jump = std::max(jump, std::fabs(stopped[f * 2] - stopped[(f - 1) * 2]));
    std::printf("    stopped: largest step in its first 10 ms %.4f, after 0.3 s %.6f\n", jump,
                rms_of(stopped, 0, k_sample_rate * 3 / 10, k_sample_rate / 2));
    GIANT_CHECK(jump < 0.02f && rms_of(stopped, 0, k_sample_rate * 3 / 10, k_sample_rate / 2) < 1e-4f);
    // Muffled in silence: the filter dies to exactly 0 (no denormals left ringing for ever).
    const auto silent = render(m, 5.0);
    for (usize i = silent.size() / 2; i < silent.size(); ++i) GIANT_CHECK(silent[i] == 0.0f);
    GIANT_CHECK(m.stats().peak == 0.0f);
}
