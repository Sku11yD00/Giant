// The crack: the first arrival of a supersonic bullet's shock wave.
#include "giant/audio/shot.h"

#include "giant/audio/mixer.h"

#include <algorithm>
#include <cmath>

namespace giant::audio {

namespace {

double dlength(const DVec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

} // namespace

double supersonic_range(const Flight& f) {
    const double v0 = std::sqrt(static_cast<double>(dot(f.velocity, f.velocity)));
    if (!std::isfinite(v0) || v0 <= k_speed_of_sound) return 0.0;
    const double k = std::isfinite(f.drag) ? std::max(static_cast<double>(f.drag), 0.0) : 0.0;
    const double range = std::isfinite(f.range) ? std::clamp(f.range, 0.0, 1e6) : 0.0;
    if (k <= 1e-12) return range;
    return std::min(std::log(v0 / k_speed_of_sound) / k, range);
}

Crack trace_crack(const Flight& f, const DVec3& listener, float air) {
    Crack out;
    const double v0 = std::sqrt(static_cast<double>(dot(f.velocity, f.velocity)));
    const double end = supersonic_range(f);
    if (!(end > 0.0) || !std::isfinite(listener.x) || !std::isfinite(listener.y) || !std::isfinite(listener.z) ||
        !std::isfinite(f.muzzle.x) || !std::isfinite(f.muzzle.y) || !std::isfinite(f.muzzle.z))
        return out;
    const double k = std::isfinite(f.drag) ? std::max(static_cast<double>(f.drag), 0.0) : 0.0;
    const DVec3 dir{f.velocity.x / v0, f.velocity.y / v0, f.velocity.z / v0};
    const auto point = [&](double s) { return f.muzzle + dir * s; };
    // Seconds to fly s metres, and its speed there.
    const auto flown = [&](double s) { return k > 1e-12 ? std::expm1(k * s) / (k * v0) : s / v0; };
    const auto speed = [&](double s) { return v0 * std::exp(-k * s); };
    // When the shock left at s reaches the listener: convex in s, so its least is found by narrowing.
    const auto arrives = [&](double s) { return flown(s) + dlength(listener - point(s)) / k_speed_of_sound; };
    // At either end the slope says whether the cone sweeps over them within the flight at all.
    const auto slope = [&](double s) {
        const DVec3 to = listener - point(s);
        const double r = dlength(to);
        const double along = r > 1e-9 ? dot(to, dir) / r : 0.0;
        return 1.0 / speed(s) - along / k_speed_of_sound;
    };
    if (slope(0.0) >= 0.0 || slope(end) <= 0.0) return out; // behind the shooter, or past where it cracks
    double lo = 0.0, hi = end;
    for (int i = 0; i < 80; ++i) {
        const double a = lo + (hi - lo) / 3.0, b = hi - (hi - lo) / 3.0;
        if (arrives(a) < arrives(b)) hi = b;
        else lo = a;
    }
    const double s = 0.5 * (lo + hi);
    const DVec3 from = point(s);
    const DVec3 to = from - listener;
    out.miss = dlength(to);
    out.heard = true;
    out.delay = arrives(s);
    out.from = from;
    out.distance = s;
    out.speed = speed(s);
    if (out.miss > 1e-6)
        out.direction = {static_cast<float>(to.x / out.miss), static_cast<float>(to.y / out.miss),
                         static_cast<float>(to.z / out.miss)};
    const double m = std::max(out.miss, 1.0);
    const double a = std::isfinite(air) ? std::clamp(static_cast<double>(air), 0.0, 100.0) : 1.0;
    out.gain = static_cast<float>(std::pow(m, -0.75) * std::pow(10.0, -0.005 * m * a / 20.0));
    if (out.delay > k_max_delay) out = Crack{};
    return out;
}

} // namespace giant::audio
