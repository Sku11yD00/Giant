#include "giant/nav/follow.h"

#include <algorithm>
#include <cmath>

namespace giant::nav {

namespace {

double flat_distance(const DVec3& a, const DVec3& b) { return std::hypot(b.x - a.x, b.z - a.z); }

float clean(float v, float lo, float hi, float otherwise) { return std::isfinite(v) ? std::clamp(v, lo, hi) : otherwise; }

} // namespace

void PathFollower::set_path(std::vector<DVec3> path) {
    m_path = std::move(path);
    m_corner = m_path.size() > 1 ? 1 : 0;
}

double PathFollower::remaining(const DVec3& at) const {
    if (m_path.empty()) return 0.0;
    const usize c = std::min(m_corner, m_path.size() - 1);
    double total = flat_distance(at, m_path[c]);
    for (usize k = c + 1; k < m_path.size(); ++k) total += flat_distance(m_path[k - 1], m_path[k]);
    return total;
}

Steering PathFollower::steer(const DVec3& at, std::span<const Neighbour> near, const FollowSettings& settings) {
    const FollowSettings d;
    const float speed = clean(settings.speed, 0.0f, 20.0f, d.speed);
    const float radius = clean(settings.radius, 0.05f, 5.0f, d.radius);
    const float corner = clean(settings.corner, 0.05f, 10.0f, d.corner);
    const float arrive = clean(settings.arrive, 0.01f, 10.0f, d.arrive);
    const float slow = clean(settings.slow, 0.0f, 50.0f, d.slow);
    const float avoid = clean(settings.avoid, 0.0f, 20.0f, d.avoid);
    Steering out;
    if (m_path.empty() || !std::isfinite(at.x) || !std::isfinite(at.z)) {
        out.arrived = m_path.empty();
        return out;
    }
    // On to the next corner once near this one, or past it (beyond it
    // along the way it was walked to: a hairpin is walked round, not cut).
    while (m_corner + 1 < m_path.size()) {
        const DVec3& c = m_path[m_corner];
        const DVec3& prev = m_corner > 0 ? m_path[m_corner - 1] : c;
        const double past = (at.x - c.x) * (c.x - prev.x) + (at.z - c.z) * (c.z - prev.z);
        if (flat_distance(at, c) <= corner || past > 0.0) {
            ++m_corner;
        } else {
            break;
        }
    }
    const DVec3& target = m_path[m_corner];
    const double left = remaining(at);
    const bool last = m_corner + 1 == m_path.size();
    const double to_end = flat_distance(at, m_path.back());
    if (last && to_end <= arrive) {
        out.arrived = true;
        return out;
    }
    // Another at the end (standing there, or crowding it too): as near as it lets it come.
    if (last) {
        for (const Neighbour& n : near) {
            if (!std::isfinite(n.position.x) || !std::isfinite(n.position.z)) continue;
            const float touching = radius + clean(n.radius, 0.0f, 5.0f, d.radius);
            const double gap = flat_distance(n.position, m_path.back());
            // Both crowding the end: both there (else each holds the other off).
            if (gap <= touching + arrive && to_end <= touching + arrive) {
                out.arrived = true;
                return out;
            }
            if (length(n.velocity) > 0.1f) continue;
            // It stands on the end, and this one is as near as its push allows.
            if (gap <= touching + arrive && to_end <= arrive + touching + avoid &&
                flat_distance(at, n.position) <= touching + avoid) {
                out.arrived = true;
                return out;
            }
        }
    }
    const double tx = target.x - at.x, tz = target.z - at.z;
    const double tl = std::hypot(tx, tz);
    Vec3 dir = tl > 1e-6 ? Vec3{static_cast<float>(tx / tl), 0.0f, static_cast<float>(tz / tl)} : Vec3{};
    float want = speed;
    if (slow > 0.0f && left < slow) want = speed * std::max(0.25f, static_cast<float>(left / slow));
    // Others near: the nearest few, by distance.
    struct Near {
        double d;
        const Neighbour* n;
    };
    Near nearest[k_max_neighbours];
    usize count = 0;
    for (const Neighbour& n : near) {
        if (!std::isfinite(n.position.x) || !std::isfinite(n.position.z)) continue;
        const double dist = flat_distance(at, n.position);
        const float reach = radius + clean(n.radius, 0.0f, 5.0f, d.radius) + avoid;
        if (dist > reach || dist < 1e-9) continue;
        if (count < k_max_neighbours) {
            nearest[count++] = {dist, &n};
        } else {
            const auto far = std::max_element(nearest, nearest + count, [](const Near& a, const Near& b) { return a.d < b.d; });
            if (dist < far->d) *far = {dist, &n};
        }
    }
    Vec3 push{};
    for (usize k = 0; k < count; ++k) {
        const Neighbour& n = *nearest[k].n;
        const double dist = nearest[k].d;
        const float touching = radius + clean(n.radius, 0.0f, 5.0f, d.radius);
        const Vec3 away{static_cast<float>((at.x - n.position.x) / dist), 0.0f, static_cast<float>((at.z - n.position.z) / dist)};
        // Closer is stronger: 1 at touching (more inside it), 0 at the edge of `avoid`.
        const float w = avoid > 0.0f ? std::clamp(1.0f - (static_cast<float>(dist) - touching) / avoid, 0.0f, 2.0f) : 0.0f;
        push = push + away * w;
        // One ahead, coming at it or slower: step to the right of it (both
        // do, so two meeting pass left side to left side).
        const float ahead = -dot(away, dir);
        if (ahead > 0.3f) {
            const Vec3 right{-dir.z, 0.0f, dir.x};
            const float closing = dot(n.velocity - dir * want, dir * -1.0f);
            if (closing > -0.1f) push = push + right * (w * ahead);
        }
    }
    Vec3 v = dir * want + push * std::max(want, 0.5f);
    const float vl = length(v);
    if (vl > speed && vl > 0.0f) v = v * (speed / vl);
    out.velocity = {v.x, 0.0f, v.z};
    return out;
}

} // namespace giant::nav
