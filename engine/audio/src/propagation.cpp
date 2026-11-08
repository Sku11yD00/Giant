// Tracing a sound through the world: the direct way, first-order echoes
// off what rays from the source hit (each spot heard only where the
// listener can see it), and the place's openness and size for the tail.
#include "giant/audio/propagation.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace giant::audio {

namespace {

constexpr double k_merge_seconds = 0.012; // echoes closer than this are heard as one
constexpr double k_lift = 0.05;           // metres off a surface before casting from it
constexpr int k_edge_halvings = 6;        // the corner found, brought nearer the edge

DVec3 to_d(const Vec3& v) { return {v.x, v.y, v.z}; }
Vec3 to_f(const DVec3& v) { return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)}; }
double dlength(const DVec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

// The k-th of n directions spread evenly over the sphere.
Vec3 fibonacci(u32 k, u32 n) {
    const double golden = 3.14159265358979323846 * (3.0 - std::sqrt(5.0));
    const double y = 1.0 - 2.0 * (k + 0.5) / n;
    const double r = std::sqrt(std::max(0.0, 1.0 - y * y));
    const double a = golden * k;
    return {static_cast<float>(std::cos(a) * r), static_cast<float>(y), static_cast<float>(std::sin(a) * r)};
}

bool usable(const RayHit& h, double max_distance) {
    return h.hit && std::isfinite(h.distance) && h.distance >= 0.0 && h.distance <= max_distance &&
           std::isfinite(h.normal.x) && std::isfinite(h.normal.y) && std::isfinite(h.normal.z) &&
           std::isfinite(h.hardness);
}

struct Echo {
    double delay = 0.0;
    double energy = 0.0;
    DVec3 direction{0, 0, 0}; // energy weighted
    double lowpass = 0.0;     // energy weighted
};

} // namespace

float air_lowpass(double metres, float air) {
    const double a = std::isfinite(air) ? std::clamp(static_cast<double>(air), 0.0, 100.0) : 1.0;
    metres *= a;
    if (!std::isfinite(metres) || metres <= 0.0) return 20000.0f;
    // About 4 kHz at 1 km, 1.5 kHz at 3 km.
    return static_cast<float>(std::clamp(20000.0 / (1.0 + metres / 250.0), 80.0, 20000.0));
}

float distance_gain(double metres, double reference, float air) {
    const double a = std::isfinite(air) ? std::clamp(static_cast<double>(air), 0.0, 100.0) : 1.0;
    const double ref = std::max(reference, 1e-3);
    const double d = std::isfinite(metres) ? std::max(metres, ref) : 1e12;
    // Spreading, and 0.5 dB a 100 m for what the air takes on the whole.
    const double air_db = 0.005 * (d - ref) * a;
    return static_cast<float>(ref / d * std::pow(10.0, -air_db / 20.0));
}

float diffraction_db(double detour) {
    if (!std::isfinite(detour)) return 30.0f;
    const double n = 2.0 * std::max(detour, 0.0) / (k_speed_of_sound / 500.0); // Fresnel number at 500 Hz
    return static_cast<float>(std::min(10.0 * std::log10(3.0 + 20.0 * n), 30.0));
}

Heard trace(const DVec3& source, const DVec3& listener, const RayCast& cast, const EchoSettings& settings) {
    Heard out;
    const DVec3 to_listener = listener - source;
    const double d = dlength(to_listener);
    if (!std::isfinite(d)) return out;
    const double max_distance =
        std::isfinite(settings.max_distance) ? std::clamp(settings.max_distance, 1.0, 50000.0) : 3000.0;

    // The direct way, cast from the listener towards the source.
    Tap direct;
    direct.delay = d / k_speed_of_sound;
    const float air = std::isfinite(settings.air) ? std::clamp(settings.air, 0.0f, 100.0f) : 1.0f;
    direct.gain = distance_gain(d, settings.reference, air);
    direct.lowpass = air_lowpass(d, air);
    direct.direction = d > 1e-6 ? to_f(-to_listener / d) : Vec3{0, 0, 1};
    if (d > 0.1 && cast) {
        const RayHit h = cast(listener, direct.direction, d - 0.1);
        ++out.casts;
        if (usable(h, d - 0.1)) {
            out.occluded = true;
            // Through it: what the wall lets by.
            Tap through = direct;
            through.gain *= std::clamp(settings.occluded_gain, 0.0f, 1.0f);
            through.lowpass = std::min(through.lowpass, settings.occluded_lowpass);
            direct = through;
            // Each leg cast out from the corner (its far ends may lie on surfaces; the corner may not).
            const auto clear = [&](const DVec3& from, const DVec3& to) {
                const DVec3 ab = to - from;
                const double l = dlength(ab);
                if (l < 0.2) return true;
                ++out.casts;
                return !usable(cast(from, to_f(ab / l), l - 0.1), l - 0.1);
            };
            const auto sees_both = [&](const DVec3& p) { return clear(p, listener) && clear(p, source); };
            if (settings.diffract) {
                // Off the face where the ray met it (along its normal, a little), so a corner is never in it.
                const float nl = length(h.normal);
                const DVec3 off = nl > 1e-6f ? to_d(h.normal / nl) * 0.05 : -to_d(direct.direction) * 0.05;
                const DVec3 met = listener + to_d(direct.direction) * h.distance + off;
                const DVec3 up{0, 1, 0};
                DVec3 side = cross(to_d(direct.direction), up);
                const double sl = dlength(side);
                const std::array<DVec3, 3> axes{up, sl > 1e-6 ? side / sl : DVec3{1, 0, 0},
                                                sl > 1e-6 ? -side / sl : DVec3{-1, 0, 0}};
                // The least reach (doubling from 1/8 m) with a corner that sees both ends; the shortest such.
                bool found = false;
                double best = 0.0, at = 0.0, below = 0.0;
                usize axis = 0;
                for (double reach = 0.125; reach <= 256.0 && !found; reach *= 2.0) {
                    for (usize a = 0; a < axes.size(); ++a) {
                        const DVec3 p = met + axes[a] * reach;
                        const double path = dlength(p - listener) + dlength(source - p);
                        if (found && path >= best) continue;
                        if (sees_both(p)) {
                            found = true;
                            best = path;
                            at = reach;
                            axis = a;
                        }
                    }
                    if (!found) below = reach;
                }
                if (found) {
                    // Then nearer the edge: halved between the last reach that did not see and the first that did.
                    double lo = below, hi = at;
                    for (int k = 0; k < k_edge_halvings && hi - lo > 0.02; ++k) {
                        const double mid = 0.5 * (lo + hi);
                        if (sees_both(met + axes[axis] * mid)) hi = mid;
                        else lo = mid;
                    }
                    const DVec3 corner = met + axes[axis] * hi;
                    best = dlength(corner - listener) + dlength(source - corner);
                    Tap bent = direct;
                    const double detour = std::max(best - d, 0.0);
                    bent.delay = best / k_speed_of_sound;
                    bent.gain = distance_gain(best, settings.reference, air) *
                                static_cast<float>(std::pow(10.0, -diffraction_db(detour) / 20.0));
                    // The longer the detour, the more only the lows bend round.
                    const float bend = static_cast<float>(std::clamp(1500.0 / std::sqrt(std::max(detour, 0.05)), 250.0, 10000.0));
                    bent.lowpass = std::min(air_lowpass(best, air), bend);
                    const DVec3 toward = corner - listener;
                    const double tl = dlength(toward);
                    if (tl > 1e-6) bent.direction = to_f(toward / tl);
                    // Heard the louder way: bent round, or through.
                    if (bent.gain > through.gain) {
                        direct = bent;
                        out.diffracted = true;
                        out.detour = detour;
                    }
                }
            }
        }
    }
    if (direct.gain > 0.0f && direct.delay <= k_max_delay) out.taps.push_back(direct);

    // Echoes: rays from the source; each hit is heard where the listener can see it.
    const u32 rays = cast ? std::min<u32>(settings.rays, 256) : 0;
    std::vector<Echo> echoes;
    echoes.reserve(rays);
    u32 hits = 0;
    double path_sum = 0.0, hardness_sum = 0.0, diffuse = 0.0;
    for (u32 k = 0; k < rays; ++k) {
        const Vec3 dir = fibonacci(k, rays);
        const RayHit h = cast(source, dir, max_distance);
        ++out.casts;
        if (!usable(h, max_distance)) continue;
        ++hits;
        const float hardness = std::clamp(h.hardness, 0.0f, 1.0f);
        path_sum += h.distance;
        hardness_sum += hardness;
        const DVec3 spot = source + to_d(dir) * h.distance;
        const float nl = length(h.normal);
        const DVec3 normal = nl > 1e-6f ? to_d(h.normal / nl) : to_d(-dir);
        const DVec3 back = listener - spot;
        const double r2 = dlength(back);
        if (r2 < 1e-3) continue;
        const DVec3 back_dir = back / r2;
        const double cos_out = dot(back_dir, normal);
        // A diffuse patch of 4 pi / n of the source's sphere sends its share to the listener (Lambert):
        // amplitude sqrt(4 h cos / n) / r2, against the direct 1 / d.
        const double energy_at_unit = 4.0 * hardness * std::max(cos_out, 0.0) / rays;
        const double ref = std::max(settings.reference, 1e-3);
        const double spread = ref / std::max(r2, ref);
        diffuse += energy_at_unit * spread * spread; // heard or not, it feeds the tail
        if (cos_out <= 0.0) continue;                // the listener is behind the surface
        const RayHit seen = cast(spot + normal * k_lift, to_f(back_dir), r2 - k_lift);
        ++out.casts;
        if (usable(seen, r2 - k_lift) && seen.distance < r2 - k_lift - 0.5) continue; // hidden from the listener
        const double path = h.distance + r2;
        if (path / k_speed_of_sound > k_max_delay) continue; // too late to be played
        const float gain = static_cast<float>(std::sqrt(energy_at_unit)) * distance_gain(r2, settings.reference, air) *
                           static_cast<float>(std::pow(10.0, -0.005 * h.distance * static_cast<double>(air) / 20.0)); // the air on the way out
        if (!(gain > 0.0f)) continue;
        Echo e;
        e.delay = path / k_speed_of_sound;
        e.energy = static_cast<double>(gain) * gain;
        e.direction = -back_dir * e.energy;
        e.lowpass = air_lowpass(path, air) * (0.4 + 0.6 * hardness) * e.energy;
        echoes.push_back(e);
    }

    // Near ones heard as one (their energies add), then the loudest kept.
    std::sort(echoes.begin(), echoes.end(), [](const Echo& a, const Echo& b) { return a.delay < b.delay; });
    std::vector<Echo> merged;
    double first = 0.0;
    for (const Echo& e : echoes) {
        // Against the group's first (so a run of near ones never chains into one long group).
        if (!merged.empty() && e.delay - first < k_merge_seconds) {
            Echo& m = merged.back();
            m.delay = (m.delay * m.energy + e.delay * e.energy) / (m.energy + e.energy);
            m.energy += e.energy;
            m.direction += e.direction;
            m.lowpass += e.lowpass;
        } else {
            merged.push_back(e);
            first = e.delay;
        }
    }
    std::sort(merged.begin(), merged.end(), [](const Echo& a, const Echo& b) { return a.energy > b.energy; });
    const usize keep = std::min<usize>({merged.size(), settings.echoes, k_max_taps - out.taps.size()});
    for (usize k = 0; k < keep; ++k) {
        const Echo& m = merged[k];
        Tap t;
        t.delay = m.delay;
        t.gain = std::min(static_cast<float>(std::sqrt(m.energy)), 4.0f);
        t.lowpass = static_cast<float>(std::clamp(m.lowpass / m.energy, 80.0, 20000.0));
        const double dl = dlength(m.direction);
        t.direction = dl > 1e-9 ? to_f(m.direction / dl) : Vec3{0, 0, 1};
        out.taps.push_back(t);
    }

    // The place: open (rays to the sky are gone), how far its surfaces are, how hard.
    if (rays > 0) {
        out.openness = 1.0f - static_cast<float>(hits) / static_cast<float>(rays);
        if (hits > 0) {
            out.mean_free_path = path_sum / hits;
            out.hardness = static_cast<float>(hardness_sum / hits);
        }
        // Eyring: T60 = 0.161 V / (-S ln(1 - a)), V / S = mean free path / 4; sky absorbs all.
        const double absorb = std::clamp(out.openness + (1.0 - out.openness) * (1.0 - out.hardness), 0.0, 0.99);
        const double t = hits > 0 ? 0.161 * (out.mean_free_path / 4.0) / -std::log(1.0 - absorb) : 0.0;
        out.reverb_seconds = static_cast<float>(std::clamp(t, 0.1, 8.0));
        out.reverb_send = static_cast<float>(std::clamp((1.0 - absorb) * std::sqrt(diffuse), 0.0, 2.0));
    }
    return out;
}

Play to_play(SoundId sound, const Heard& heard, float loudness) {
    Play p;
    p.sound = sound;
    const float l = std::isfinite(loudness) ? std::max(loudness, 0.0f) : 0.0f;
    p.taps = heard.taps;
    float loudest = 0.0f;
    for (Tap& t : p.taps) {
        t.gain *= l;
        loudest = std::max(loudest, t.gain);
    }
    // Past the mixer's 4 the whole of it is scaled down alike (so near, the echoes keep their balance).
    const float scale = loudest > 4.0f ? 4.0f / loudest : 1.0f;
    for (Tap& t : p.taps) t.gain *= scale;
    p.reverb_send = heard.reverb_send * l * scale;
    p.reverb_seconds = heard.reverb_seconds;
    // The tail starts with the first sound heard (the mixer starts it at the earliest tap).
    return p;
}

} // namespace giant::audio
