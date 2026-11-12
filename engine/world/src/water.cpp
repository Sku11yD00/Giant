#include "giant/world/water.h"

#include "giant/world/terrain.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace giant::world {

namespace {

constexpr double k_max_coordinate = 1e7; // metres from the origin

bool finite_point(double x, double z) {
    return std::isfinite(x) && std::isfinite(z) && std::fabs(x) <= k_max_coordinate &&
           std::fabs(z) <= k_max_coordinate;
}

bool in_unit(const Vec3& v, float maximum) {
    for (const float c : {v.x, v.y, v.z}) {
        if (!std::isfinite(c) || c < 0.0f || c > maximum) return false;
    }
    return true;
}

bool level_ok(float level) { return std::isfinite(level) && std::fabs(level) <= k_max_terrain_height; }

// Even-odd: inside the shore.
bool inside(std::span<const WaterPoint> shore, double x, double z) {
    bool in = false;
    for (usize i = 0, j = shore.size() - 1; i < shore.size(); j = i++) {
        const WaterPoint& a = shore[i];
        const WaterPoint& b = shore[j];
        if ((a.z > z) != (b.z > z)) {
            const double cross = a.x + (z - a.z) / (b.z - a.z) * (b.x - a.x);
            if (x < cross) in = !in;
        }
    }
    return in;
}

} // namespace

std::string_view water_kind_name(WaterKind kind) {
    switch (kind) {
    case WaterKind::lake: return "lake";
    case WaterKind::river: return "river";
    case WaterKind::sea: return "sea";
    }
    return "lake";
}

std::optional<WaterKind> water_kind_from_name(std::string_view name) {
    if (name == "lake") return WaterKind::lake;
    if (name == "river") return WaterKind::river;
    if (name == "sea") return WaterKind::sea;
    return std::nullopt;
}

std::optional<std::string> check_water(const Water& w) {
    if (w.name.empty() || w.name.size() > 256) return "name must be 1 to 256 bytes";
    if (!level_ok(w.level)) return "level must be a finite height within the terrain's range";
    if (!std::isfinite(w.flow) || w.flow < 0.0f || w.flow > 20.0f) return "flow must be from 0 to 20 metres a second";
    if (!in_unit(w.color, 1.0f)) return "color values must be from 0 to 1";
    if (!in_unit(w.absorption, 10.0f)) return "absorption values must be from 0 to 10 a metre";
    if (!std::isfinite(w.turbidity) || w.turbidity < 0.0f || w.turbidity > 1.0f) return "turbidity must be from 0 to 1";
    if (!std::isfinite(w.waves) || w.waves < 0.0f || w.waves > 4.0f) return "waves must be from 0 to 4";
    if (w.outline.size() > k_max_water_points || w.path.size() > k_max_water_points) {
        return "at most " + std::to_string(k_max_water_points) + " points";
    }
    for (const WaterPoint& p : w.outline) {
        if (!finite_point(p.x, p.z)) return "outline points must be finite and within 10,000 km";
    }
    for (const RiverPoint& p : w.path) {
        if (!finite_point(p.x, p.z)) return "path points must be finite and within 10,000 km";
        if (!std::isfinite(p.width) || p.width < 0.5f || p.width > 2000.0f) return "path widths must be from 0.5 to 2000 metres";
        if (!level_ok(p.level)) return "path levels must be finite heights within the terrain's range";
    }
    switch (w.kind) {
    case WaterKind::lake:
        if (w.outline.size() < 3) return "a lake needs an outline of 3 or more points";
        if (!w.path.empty()) return "a lake has no path";
        break;
    case WaterKind::river:
        if (w.path.size() < 2) return "a river needs a path of 2 or more points";
        if (!w.outline.empty()) return "a river has no outline";
        for (usize i = 1; i < w.path.size(); ++i) {
            if (w.path[i].x == w.path[i - 1].x && w.path[i].z == w.path[i - 1].z) return "path points must differ from the one before";
        }
        break;
    case WaterKind::sea:
        if (!w.outline.empty() || !w.path.empty()) return "the sea has no outline or path";
        break;
    }
    return std::nullopt;
}

bool water_bounds(const Water& w, double& min_x, double& min_z, double& max_x, double& max_z) {
    if (w.kind == WaterKind::sea) return false;
    min_x = min_z = std::numeric_limits<double>::infinity();
    max_x = max_z = -std::numeric_limits<double>::infinity();
    for (const WaterPoint& p : w.outline) {
        min_x = std::min(min_x, p.x);
        max_x = std::max(max_x, p.x);
        min_z = std::min(min_z, p.z);
        max_z = std::max(max_z, p.z);
    }
    for (const RiverPoint& p : w.path) {
        const double r = p.width * 0.5;
        min_x = std::min(min_x, p.x - r);
        max_x = std::max(max_x, p.x + r);
        min_z = std::min(min_z, p.z - r);
        max_z = std::max(max_z, p.z + r);
    }
    return min_x <= max_x;
}

std::optional<WaterHere> water_part_at(const Water& w, usize segment, double x, double z, double& nearness) {
    nearness = 0.0;
    if (!w.visible) return std::nullopt;
    if (w.kind == WaterKind::sea) return WaterHere{&w, w.level};
    if (w.kind == WaterKind::lake) {
        if (w.outline.size() >= 3 && inside(w.outline, x, z)) return WaterHere{&w, w.level};
        return std::nullopt;
    }
    // A river's stretch: measured against its half width there (so a wide
    // stretch beside a narrow one is found where it reaches).
    if (segment + 1 >= w.path.size()) return std::nullopt;
    const RiverPoint& a = w.path[segment];
    const RiverPoint& b = w.path[segment + 1];
    const double dx = b.x - a.x, dz = b.z - a.z;
    const double len2 = dx * dx + dz * dz;
    if (!(len2 > 0.0)) return std::nullopt;
    const double t = std::clamp(((x - a.x) * dx + (z - a.z) * dz) / len2, 0.0, 1.0);
    const double px = a.x + dx * t - x, pz = a.z + dz * t - z;
    const double half = 0.5 * (a.width + (b.width - a.width) * t);
    if (!(half > 0.0)) return std::nullopt; // (unchecked data: no width, no water)
    const double d = std::sqrt(px * px + pz * pz) / half;
    if (!(d <= 1.0)) return std::nullopt;
    nearness = d;
    const double len = std::sqrt(len2);
    WaterHere here;
    here.water = &w;
    here.level = static_cast<float>(a.level + (b.level - a.level) * t);
    here.flow_x = static_cast<float>(dx / len * w.flow);
    here.flow_z = static_cast<float>(dz / len * w.flow);
    here.width = static_cast<float>(2.0 * half);
    return here;
}

std::optional<WaterHere> water_at(std::span<const Water> waters, double x, double z) {
    std::optional<WaterHere> best;
    for (const Water& w : waters) {
        if (!w.visible) continue;
        double nearness = 0.0;
        std::optional<WaterHere> here;
        if (w.kind == WaterKind::sea) {
            here = water_part_at(w, 0, x, z, nearness);
        } else {
            double x0, z0, x1, z1;
            if (!water_bounds(w, x0, z0, x1, z1) || x < x0 || x > x1 || z < z0 || z > z1) continue;
            if (w.kind == WaterKind::lake) {
                here = water_part_at(w, 0, x, z, nearness);
            } else {
                // The nearest stretch whose half width reaches the point.
                double nearest = std::numeric_limits<double>::infinity();
                for (usize i = 0; i + 1 < w.path.size(); ++i) {
                    const std::optional<WaterHere> h = water_part_at(w, i, x, z, nearness);
                    if (h && nearness < nearest) {
                        nearest = nearness;
                        here = h;
                    }
                }
            }
        }
        if (here && (!best || here->level > best->level)) best = here;
    }
    return best;
}

Swell sea_swell(const Vec3& velocity) {
    double wx = std::isfinite(velocity.x) ? velocity.x : 0.0;
    double wz = std::isfinite(velocity.z) ? velocity.z : 0.0;
    double speed = std::sqrt(wx * wx + wz * wz);
    if (!(speed > 1e-3)) {
        wx = 1.0;
        wz = 0.0;
        speed = 0.0;
    }
    speed = std::min(speed, 40.0);
    const double angle = std::atan2(wz, wx);
    constexpr double two_pi = 6.283185307179586;
    // The sea this wind grows (Pierson-Moskowitz): its peak's length and its
    // significant height (four times the surface's standard deviation).
    const double longest = std::clamp(two_pi * speed * speed / (0.877 * 0.877 * k_swell_gravity), 40.0, 250.0);
    const double significant = std::clamp(0.21 * speed * speed / k_swell_gravity, 0.3, 4.0);
    // Three waves running with the wind, one from far off (a calm's swell).
    static constexpr double k_lengths[k_swell_set] = {1.0, 0.73, 0.57, 1.4};
    static constexpr double k_turns[k_swell_set] = {0.0, 0.5, -0.42, 2.1};
    static constexpr double k_shares[k_swell_set] = {0.62, 0.42, 0.33, 0.25};
    static constexpr double k_phases[k_swell_set] = {0.0, 2.1, 4.4, 1.3};
    // Shares scaled so their variance (half the amplitudes squared, summed)
    // is the significant height's (a quarter of it, squared).
    double variance = 0.0;
    for (const double s : k_shares) variance += 0.5 * s * s;
    const double scale = significant / 4.0 / std::sqrt(variance);
    Swell out;
    for (u32 i = 0; i < k_swell_set; ++i) {
        const double length = longest * k_lengths[i];
        const double k = two_pi / length;
        SwellWave& w = out.waves[i];
        w.dir_x = static_cast<float>(std::cos(angle + k_turns[i]));
        w.dir_z = static_cast<float>(std::sin(angle + k_turns[i]));
        w.k = static_cast<float>(k);
        // No steeper than a wave keeps its shape (k a 0.12).
        w.amplitude = static_cast<float>(std::min(k_shares[i] * scale, 0.12 / k));
        w.phase = static_cast<float>(k_phases[i]);
    }
    return out;
}

namespace {

// A wave that is there (as the renderer's water_swell keeps them).
bool wave_there(const SwellWave& w) {
    return std::isfinite(w.dir_x) && std::isfinite(w.dir_z) && std::isfinite(w.k) && std::isfinite(w.amplitude) &&
           std::isfinite(w.phase) && w.k > 0.0f && w.amplitude > 0.0f &&
           static_cast<double>(w.dir_x) * w.dir_x + static_cast<double>(w.dir_z) * w.dir_z > 1e-12;
}

} // namespace

void SwellTrack::update(const Vec3& velocity, double seconds) {
    const double t = std::isfinite(seconds) ? seconds : 0.0;
    const Swell wanted = sea_swell(velocity);
    const auto wanted_set = [&] {
        std::array<SwellWave, k_swell_set> set{};
        for (u32 i = 0; i < k_swell_set; ++i) set[i] = wanted.waves[i];
        return set;
    };
    // Heights ease towards the wind's (a sudden change of wind: no sudden swell).
    const double dt = m_started ? std::clamp(t - m_last_seconds, 0.0, 1.0e6) : 0.0;
    m_last_seconds = t;
    const float ease = static_cast<float>(1.0 - std::exp(-dt / k_swell_ease));
    const auto follow = [&] {
        for (u32 i = 0; i < k_swell_set; ++i) m_now[i].amplitude += (wanted.waves[i].amplitude - m_now[i].amplitude) * ease;
    };
    double fade = 1.0;
    if (!m_started) {
        m_started = true;
        m_now = wanted_set();
    } else if (m_fading) {
        fade = (t - m_fade_start) / k_swell_fade;
        if (!(fade >= 0.0 && fade < 1.0)) {
            m_fading = false; // (done, or the clock went back)
            fade = 1.0;
        }
    }
    if (!m_fading) {
        // Near enough the wind's: kept, its height following; else a new set fades in.
        const SwellWave& held = m_now[0];
        const SwellWave& want = wanted.waves[0];
        const double turn = static_cast<double>(held.dir_x) * want.dir_x + static_cast<double>(held.dir_z) * want.dir_z;
        const bool near = std::fabs(static_cast<double>(held.k) / static_cast<double>(want.k) - 1.0) <= 0.1 &&
                          turn >= std::cos(15.0 * 3.141592653589793 / 180.0);
        if (near) {
            follow();
        } else {
            m_old = m_now;
            m_now = wanted_set();
            m_fading = true;
            m_fade_start = t;
            fade = 0.0;
        }
    } else {
        // Fading: the new set's heights follow the wind meanwhile.
        follow();
    }
    // Smoothly (an S): the new in, the old out.
    const float in = static_cast<float>(fade * fade * (3.0 - 2.0 * fade));
    for (u32 i = 0; i < k_swell_set; ++i) {
        m_swell.waves[i] = m_now[i];
        m_swell.waves[i].amplitude *= in;
        m_swell.waves[k_swell_set + i] = m_old[i];
        m_swell.waves[k_swell_set + i].amplitude = m_fading ? m_old[i].amplitude * (1.0f - in) : 0.0f;
    }
}

SwellHere swell_at(const Swell& swell, double x, double z, double seconds) {
    SwellHere out;
    if (!std::isfinite(x) || !std::isfinite(z) || !std::isfinite(seconds)) return out;
    for (const SwellWave& w : swell.waves) {
        if (!wave_there(w)) continue;
        const double k = w.k;
        const double theta = k * (static_cast<double>(w.dir_x) * x + static_cast<double>(w.dir_z) * z) -
                             std::sqrt(k_swell_gravity * k) * seconds + static_cast<double>(w.phase);
        const double a = w.amplitude;
        out.height += a * std::sin(theta);
        const double rise = a * k * std::cos(theta);
        out.slope_x += rise * static_cast<double>(w.dir_x);
        out.slope_z += rise * static_cast<double>(w.dir_z);
    }
    return out;
}

double swell_height(const Swell& swell, double x, double z, double seconds) {
    return swell_at(swell, x, z, seconds).height;
}

float swell_most(const Swell& swell) {
    float most = 0.0f;
    for (const SwellWave& w : swell.waves) {
        if (wave_there(w)) most += w.amplitude;
    }
    return most;
}

float swell_scale(const Water& water) {
    return std::isfinite(water.waves) ? std::clamp(water.waves, 0.0f, 4.0f) : 0.0f;
}

} // namespace giant::world
