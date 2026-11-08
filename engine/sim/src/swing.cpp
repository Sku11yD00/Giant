#include "giant/sim/swing.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace giant::sim {

namespace {

constexpr float k_drag = 1.0f;          // a box's
constexpr float k_pivot_damping = 0.3f; // share of the speed the pivot takes a second (besides the air)
constexpr float k_max_speed = 50.0f;    // m/s at most, however hard it is struck
constexpr float k_still = 2e-5f;        // metres a step: not moving
constexpr u32 k_still_steps = 60;       // steps that still before it sleeps
constexpr float k_calm = 0.05f;         // m/s: a change of wind this small leaves it asleep
constexpr float k_drawn = 1e-4f;        // metres it moves before it is drawn again
constexpr usize k_looked_per_simulated = 4; // swings looked at a frame (asleep or not), per one that may move
const Vec3 k_down{0.0f, -1.0f, 0.0f};

bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool finite(const DVec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

float clamp_or(float v, float lo, float hi, float otherwise) {
    return std::isfinite(v) ? std::clamp(v, lo, hi) : otherwise;
}

Vec3 to_f(const DVec3& v) { return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)}; }
DVec3 to_d(const Vec3& v) { return {v.x, v.y, v.z}; }

// The turn taking straight down to `u` (unit; not straight up).
Quat turn_to(const Vec3& u) {
    const Vec3 axis = cross(k_down, u);
    return normalize(Quat{axis.x, axis.y, axis.z, 1.0f + dot(k_down, u)});
}

// `bob` put back on its rod: in the hinge's plane, `length` from the
// pivot (`rod` metres off), at most `limit` radians from straight down.
Vec3 constrain(Vec3 bob, const Vec3& hinge, float rod, float limit) {
    bob = bob - hinge * dot(bob, hinge); // hinge is 0 when it swings any way
    const float l = length(bob);
    Vec3 u = l > 1e-6f ? bob * (1.0f / l) : k_down;
    if (std::acos(std::clamp(dot(u, k_down), -1.0f, 1.0f)) > limit) {
        Vec3 out = u - k_down * dot(u, k_down); // level, away from straight down
        const float o = length(out);
        if (o > 1e-6f) {
            out = out * (1.0f / o);
        } else {
            out = hinge == Vec3{} ? Vec3{1.0f, 0.0f, 0.0f} : cross(hinge, k_down); // straight up: any side
        }
        u = k_down * std::cos(limit) + out * std::sin(limit);
    }
    return u * rod;
}

} // namespace

world::SwingSettings clean(const world::SwingSettings& s) {
    const world::SwingSettings d;
    world::SwingSettings c = s;
    c.length = clamp_or(s.length, 0.02f, 20.0f, d.length);
    c.weight = clamp_or(s.weight, 0.05f, 1000.0f, d.weight);
    c.limit = clamp_or(s.limit, 5.0f, 175.0f, d.limit);
    if (c.hinge != world::SwingHinge::free && c.hinge != world::SwingHinge::x) c.hinge = d.hinge;
    return c;
}

bool can_swing(world::Shape shape) {
    switch (shape) {
    case world::Shape::cube:
    case world::Shape::sphere:
    case world::Shape::plane:
    case world::Shape::mesh:
    case world::Shape::kit: return true;
    case world::Shape::decal:
    case world::Shape::cloth: return false;
    }
    return false;
}

float Swings::Swing::angle() const {
    const float l = length(bob);
    if (!(l > 0.0f)) return 0.0f;
    return radians_to_degrees(std::acos(std::clamp(dot(bob * (1.0f / l), k_down), -1.0f, 1.0f)));
}

void Swings::set(world::EntityId id, const world::SwingSettings& settings, const world::Transform& rest,
                 const Vec3& size) {
    const world::SwingSettings c = clean(settings);
    world::Transform r = rest;
    if (!finite(r.position)) r.position = {};
    const Vec3 box{clamp_or(std::fabs(size.x), 0.0f, 1000.0f, 1.0f), clamp_or(std::fabs(size.y), 0.0f, 1000.0f, 1.0f),
                   clamp_or(std::fabs(size.z), 0.0f, 1000.0f, 1.0f)};
    const auto [it, added] = m_swings.try_emplace(id);
    Swing& s = it->second;
    const bool fresh = added || !(s.settings == c);
    s.settings = c;
    s.rest = r;
    s.size = box;
    s.pivot = r.position + DVec3{0.0, c.length, 0.0};
    s.hinge = {};
    if (c.hinge == world::SwingHinge::x) {
        const Vec3 x = rotate(r.rotation, Vec3{1.0f, 0.0f, 0.0f});
        const Vec3 level{x.x, 0.0f, x.z};
        const float l = length(level);
        if (l > 0.1f) s.hinge = level * (1.0f / l); // an x axis standing up swings any way
    }
    if (fresh) {
        s.bob = k_down * c.length;
        s.velocity = {};
        s.time = 0.0f;
    } else {
        s.bob = constrain(s.bob, s.hinge, c.length, degrees_to_radians(c.limit));
    }
    wake(s);
    s.drawn_bob = Vec3{1e9f, 0.0f, 0.0f}; // drawn again now
    place(id, s);
}

void Swings::remove(world::EntityId id) {
    const auto it = m_swings.find(id);
    if (it == m_swings.end()) return;
    if (!it->second.queued) m_moved.push_back(id); // drawn where the scene has it
    m_swings.erase(it);
    m_poses.erase(id);
}

void Swings::clear() {
    for (const auto& [id, s] : m_swings) {
        if (!s.queued) m_moved.push_back(id);
    }
    m_swings.clear();
    m_poses.clear();
}

void Swings::place(world::EntityId id, Swing& s) {
    if (length(s.bob - s.drawn_bob) <= k_drawn) return;
    s.drawn_bob = s.bob;
    const float l = length(s.bob);
    s.turn = l > 0.0f ? turn_to(s.bob * (1.0f / l)) : Quat{};
    world::Transform& pose = m_poses[id];
    pose.position = s.pivot + to_d(s.bob);
    pose.rotation = normalize(s.turn * s.rest.rotation);
    pose.scale = s.rest.scale;
    s.pose = pose;
    if (!s.queued) {
        s.queued = true;
        m_moved.push_back(id);
    }
}

void Swings::step(double seconds, const WindState& wind, const DVec3& camera) {
    m_stats.simulated = 0;
    m_stats.asleep = 0;
    m_stats.steps = 0;
    if (!std::isfinite(seconds) || seconds <= 0.0) return;
    const float dt = std::clamp(m_settings.step_seconds, 1.0f / 480.0f, 1.0f / 15.0f);
    const float most = dt * static_cast<float>(std::max(m_settings.max_steps, 1u));
    const float added = static_cast<float>(std::min(seconds, static_cast<double>(most)));
    const double reach = std::isfinite(m_settings.distance) ? std::max(m_settings.distance, 0.0) : 0.0;
    const float g = clamp_or(m_settings.gravity, 0.0f, 100.0f, 9.81f);
    const float air = clamp_or(m_settings.air_density, 0.0f, 10.0f, 1.2f);
    m_near.clear();
    for (auto& [id, s] : m_swings) {
        const DVec3 d = s.rest.position - camera;
        const double d2 = d.x * d.x + d.y * d.y + d.z * d.z;
        if (d2 <= reach * reach) {
            m_near.push_back({d2, id});
        } else {
            s.time = std::min(s.time, dt); // out of reach: the time it would have stepped dropped
        }
    }
    // The nearest looked at (k_looked_per_simulated times as many as move,
    // so sleepers ahead count too), nearest first; the rest wait.
    const usize looked = std::min(m_near.size(), static_cast<usize>(m_settings.max_simulated) * k_looked_per_simulated);
    if (looked < m_near.size()) {
        std::nth_element(m_near.begin(), m_near.begin() + static_cast<std::ptrdiff_t>(looked), m_near.end());
        for (usize k = looked; k < m_near.size(); ++k) {
            Swing& s = m_swings.at(m_near[k].second);
            s.time = std::min(s.time, dt);
            if (s.asleep) ++m_stats.asleep;
        }
        m_near.resize(looked);
    }
    std::sort(m_near.begin(), m_near.end());
    for (const auto& [d2, id] : m_near) {
        Swing& s = m_swings.at(id);
        if (m_stats.simulated >= m_settings.max_simulated) {
            s.time = std::min(s.time, dt);
            if (s.asleep) ++m_stats.asleep;
            continue;
        }
        Vec3 w = wind_at(wind, s.pose.position);
        if (!finite(w)) w = {}; // a bad wind is none
        if (s.asleep && length(w - s.slept_wind) > k_calm) wake(s);
        if (s.asleep) {
            ++m_stats.asleep;
            continue;
        }
        ++m_stats.simulated;
        const float rod = s.settings.length;
        const float limit = degrees_to_radians(s.settings.limit);
        const float mass = s.settings.weight;
        s.time = std::min(s.time + added, most);
        u32 taken = 0;
        while (s.time >= dt) {
            s.time -= dt;
            ++taken;
            // The wind's push on the box as it is turned now, never more
            // than takes it to the wind's own speed in a step.
            Vec3 v = s.velocity;
            const Vec3 relative = w - v;
            const float speed = length(relative);
            if (speed > 1e-4f) {
                const Vec3 dir = relative * (1.0f / speed);
                const Vec3 l = rotate(conjugate(normalize(s.turn * s.rest.rotation)), dir);
                const float area = std::fabs(l.x) * s.size.y * s.size.z + std::fabs(l.y) * s.size.x * s.size.z +
                                   std::fabs(l.z) * s.size.x * s.size.y;
                const float push = 0.5f * air * k_drag * area * speed * speed / mass;
                v = v + dir * std::min(push * dt, speed);
            }
            v = v + Vec3{0.0f, -g * dt, 0.0f};
            v = v * (1.0f / (1.0f + k_pivot_damping * dt));
            const float fast = length(v);
            if (fast > k_max_speed) v = v * (k_max_speed / fast);
            const Vec3 was = s.bob;
            const Vec3 now = constrain(was + v * dt, s.hinge, rod, limit);
            if (!finite(now) || !finite(v)) {
                s.bob = k_down * rod;
                s.velocity = {};
                s.time = 0.0f;
                ++m_stats.resets;
                break;
            }
            s.velocity = (now - was) * (1.0f / dt); // what the rod let it do
            s.bob = now;
            // Turned with it (the wind's push next step meets it so).
            const float lb = length(s.bob);
            s.turn = lb > 0.0f ? turn_to(s.bob * (1.0f / lb)) : Quat{};
            if (length(now - was) < k_still) {
                if (++s.still_steps >= k_still_steps) {
                    s.asleep = true;
                    s.slept_wind = w;
                    s.velocity = {};
                    s.time = 0.0f;
                    break;
                }
            } else {
                s.still_steps = 0;
            }
        }
        m_stats.steps = std::max(m_stats.steps, taken);
        if (taken > 0) place(id, s);
    }
}

usize Swings::push(const DVec3& at, float radius, const Vec3& velocity) {
    if (!std::isfinite(radius) || radius <= 0.0f || !finite(velocity) || !finite(at)) return 0;
    usize count = 0;
    for (auto& [id, s] : m_swings) {
        const float d = static_cast<float>(length(s.pose.position - at));
        if (!(d < radius)) continue;
        s.velocity = s.velocity + velocity * (1.0f - d / radius);
        wake(s);
        ++count;
    }
    return count;
}

usize Swings::blast(const DVec3& at, float radius, float impulse) {
    if (!std::isfinite(radius) || radius <= 0.0f || !std::isfinite(impulse) || impulse <= 0.0f || !finite(at)) {
        return 0;
    }
    const float most = clamp_or(max_knock, 0.0f, k_max_speed, 20.0f);
    usize count = 0;
    for (auto& [id, s] : m_swings) {
        const Vec3 away = to_f(s.pose.position - at);
        const float d = length(away);
        if (!(d < radius)) continue;
        const Vec3 dir = d > 1e-4f ? away * (1.0f / d) : Vec3{0.0f, 1.0f, 0.0f};
        const float speed = std::min(impulse * (1.0f - d / radius) / s.settings.weight, most);
        s.velocity = s.velocity + dir * speed;
        wake(s);
        ++count;
    }
    return count;
}

bool Swings::knock(world::EntityId id, const Vec3& impulse) {
    const auto it = m_swings.find(id);
    if (it == m_swings.end()) return false;
    if (!finite(impulse)) return true;
    Swing& s = it->second;
    Vec3 dv = impulse * (1.0f / s.settings.weight);
    const float l = length(dv);
    const float most = clamp_or(max_knock, 0.0f, k_max_speed, 20.0f);
    if (l > most) dv = dv * (most / l);
    s.velocity = s.velocity + dv;
    wake(s);
    return true;
}

const Swings::Swing* Swings::find(world::EntityId id) const {
    const auto it = m_swings.find(id);
    return it == m_swings.end() ? nullptr : &it->second;
}

std::vector<world::EntityId> Swings::take_moved() {
    std::vector<world::EntityId> out = std::move(m_moved);
    m_moved.clear();
    std::sort(out.begin(), out.end()); // removed and added again: once
    out.erase(std::unique(out.begin(), out.end()), out.end());
    for (const world::EntityId id : out) {
        const auto it = m_swings.find(id);
        if (it != m_swings.end()) it->second.queued = false;
    }
    return out;
}

SwingStats Swings::stats() const {
    SwingStats s = m_stats;
    s.swings = static_cast<u32>(m_swings.size());
    return s;
}

} // namespace giant::sim
