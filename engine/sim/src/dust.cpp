#include "giant/sim/dust.h"

#include <algorithm>
#include <cmath>

namespace giant::sim {

namespace {

bool finite(const DVec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// What is left of a puff to see: its density now over the rest of its life.
float strength(const DustPuff& p) {
    return p.density * p.radius * std::max(0.0f, 1.0f - p.age / std::max(p.life, 1e-3f));
}

} // namespace

void Dust::emit(const DVec3& at, float radius, float density, const Vec3& color, DustSource source, float raised) {
    if (!finite(at) || !std::isfinite(radius) || !std::isfinite(density) || radius <= 0.0f || density <= 0.0f) return;
    if (m_settings.max_puffs == 0) return;
    if (m_emits >= m_settings.emits_per_step) {
        ++m_dropped;
        return;
    }
    ++m_emits;
    radius = std::min(radius, m_settings.max_radius);
    density = std::min(density, 10.0f);
    const Vec3 c{std::clamp(color.x, 0.0f, 1.0f), std::clamp(color.y, 0.0f, 1.0f), std::clamp(color.z, 0.0f, 1.0f)};
    // Inside a young puff: it swells to take this one in.
    for (DustPuff& p : m_puffs) {
        if (p.age > m_settings.merge_age) continue;
        const DVec3 d = at - p.position;
        const double dist = std::sqrt(dot(d, d));
        if (dist > static_cast<double>(p.radius)) continue;
        const float reach = std::min(m_settings.max_radius, std::max(p.radius, static_cast<float>(dist) + radius));
        // As much dust as both, spread over the bigger ball (by its cross-section).
        const float r2 = reach * reach;
        const float dust = p.density * p.radius * p.radius + density * radius * radius;
        p.density = std::min(10.0f, std::max(p.density, dust / r2));
        p.radius = reach;
        // Its growth goes on from here: as if it had started as big as
        // this needs at its age (not grown again on top of it).
        const float t = std::clamp(p.age / std::max(p.life, 1e-3f), 0.0f, 1.0f);
        p.start_radius = reach / (1.0f + m_settings.grow * std::sqrt(t));
        const float spread = p.start_radius / reach;
        p.start_density = p.density / std::max(spread * spread, 1e-6f);
        p.life = std::max(p.life, std::clamp(6.0f + reach, 8.0f, 30.0f));
        ++m_merged;
        return;
    }
    // A new puff (not one swelling a young one): an event.
    if (m_events.size() < k_max_dust_events) {
        const float lift = std::isfinite(raised) ? std::clamp(raised, 0.0f, 1000.0f) : 0.0f;
        m_events.push_back({at - DVec3{0.0, static_cast<double>(lift), 0.0}, radius, source});
    } else {
        ++m_events_dropped;
    }
    DustPuff p;
    p.position = at;
    p.radius = radius;
    p.density = density;
    p.color = c;
    p.life = std::clamp(6.0f + radius, 8.0f, 30.0f);
    p.start_radius = radius;
    p.start_density = density;
    if (m_puffs.size() < m_settings.max_puffs) {
        m_puffs.push_back(p);
        ++m_emitted;
        return;
    }
    // Full: the faintest gives way, if fainter than this one.
    auto weakest = std::min_element(m_puffs.begin(), m_puffs.end(),
                                    [](const DustPuff& a, const DustPuff& b) { return strength(a) < strength(b); });
    if (strength(*weakest) >= strength(p)) {
        ++m_dropped;
        return;
    }
    *weakest = p;
    ++m_emitted;
}

void Dust::step(float seconds, const Vec3& wind) {
    m_emits = 0;
    if (!std::isfinite(seconds) || seconds <= 0.0f) return;
    const Vec3 w{std::isfinite(wind.x) ? wind.x : 0.0f, 0.0f, std::isfinite(wind.z) ? wind.z : 0.0f};
    const DVec3 move{static_cast<double>(w.x * m_settings.drift * seconds),
                     static_cast<double>(m_settings.rise * seconds),
                     static_cast<double>(w.z * m_settings.drift * seconds)};
    for (DustPuff& p : m_puffs) {
        p.age += seconds;
        p.position = p.position + move;
        const float t = std::clamp(p.age / std::max(p.life, 1e-3f), 0.0f, 1.0f);
        // Grows fast at first, then slower; thins as it spreads (the dust
        // across it kept), and fades away over the last 40% of its life.
        const float grown = std::min(m_settings.max_radius, p.start_radius * (1.0f + m_settings.grow * std::sqrt(t)));
        p.radius = std::max(p.radius, grown);
        const float spread = p.start_radius / p.radius;
        const float fade = t < 0.6f ? 1.0f : std::max(0.0f, 1.0f - (t - 0.6f) / 0.4f);
        p.density = p.start_density * spread * spread * fade;
    }
    std::erase_if(m_puffs, [](const DustPuff& p) { return p.age >= p.life; });
}

void Dust::clear() {
    m_puffs.clear();
    m_events.clear();
    m_emits = 0;
}

void Dust::nearest(const DVec3& camera, u32 count, std::vector<DustPuff>& out) const {
    out.clear();
    if (count == 0 || m_puffs.empty()) return;
    std::vector<std::pair<double, u32>>& by = m_by; // kept: no allocation a frame
    by.clear();
    for (u32 i = 0; i < m_puffs.size(); ++i) {
        const DVec3 d = m_puffs[i].position - camera;
        by.push_back({std::max(0.0, std::sqrt(dot(d, d)) - static_cast<double>(m_puffs[i].radius)), i});
    }
    const usize n = std::min<usize>(count, by.size());
    std::partial_sort(by.begin(), by.begin() + static_cast<std::ptrdiff_t>(n), by.end());
    out.reserve(n);
    for (usize k = 0; k < n; ++k) out.push_back(m_puffs[by[k].second]);
}

DustStats Dust::stats() const {
    DustStats s;
    s.puffs = static_cast<u32>(m_puffs.size());
    s.emitted = m_emitted;
    s.merged = m_merged;
    s.dropped = m_dropped;
    s.events_dropped = m_events_dropped;
    return s;
}

} // namespace giant::sim
