#include "giant/sim/ballistics.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace giant::sim {

namespace {

bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool finite(const DVec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

DVec3 along(const DVec3& from, const Vec3& dir, double metres) {
    return {from.x + static_cast<double>(dir.x) * metres, from.y + static_cast<double>(dir.y) * metres,
            from.z + static_cast<double>(dir.z) * metres};
}

// How far bullets start past a surface they leave (so the next ray does
// not meet it again): a millimetre.
constexpr double k_clear = 1e-3;
// Thinner is no far side (bodies are a millimetre thick at least).
constexpr float k_min_thickness = 5e-4f;

} // namespace

const char* to_string(HitOutcome outcome) {
    switch (outcome) {
    case HitOutcome::stopped: return "stopped";
    case HitOutcome::passed: return "passed";
    case HitOutcome::ricochet: return "ricochet";
    }
    return "stopped";
}

std::array<SurfaceBallistics, world::k_surface_count> default_surfaces() {
    std::array<SurfaceBallistics, world::k_surface_count> s{};
    const auto set = [&](world::Surface surface, float resistance, float ricochet = 0.0f, float keep = 0.6f) {
        s[static_cast<usize>(surface)] = {resistance, ricochet, keep};
    };
    // A 9 mm pistol round carries about 500 J, a 5.45 mm rifle round 1300,
    // a 7.62 mm one 2000 to 3300.
    set(world::Surface::standard, 15000.0f);
    set(world::Surface::flesh, 2000.0f);
    set(world::Surface::wood, 3000.0f, 5.0f, 0.5f);
    set(world::Surface::glass, 300.0f);
    set(world::Surface::dirt, 12000.0f, 4.0f, 0.4f);
    set(world::Surface::foliage, 100.0f);
    set(world::Surface::brick, 30000.0f, 10.0f, 0.5f);
    set(world::Surface::concrete, 50000.0f, 15.0f, 0.5f);
    set(world::Surface::metal, 400000.0f, 25.0f, 0.7f);
    return s;
}

Ballistics::Ballistics(const PhysicsWorld& physics, const BallisticsSettings& settings)
    : m_physics(physics), m_settings(settings) {}

BulletId Ballistics::fire(const Shot& shot) {
    const float speed = length(shot.velocity);
    if (!finite(shot.origin) || !finite(shot.velocity) || !(shot.mass > 0.0f) || !std::isfinite(shot.mass) ||
        !(shot.penetration > 0.0f) || !std::isfinite(shot.penetration) || !(speed > 0.0f) ||
        m_bullets.size() >= m_settings.max_bullets) {
        ++m_stats.refused;
        return k_no_bullet;
    }
    Bullet b;
    b.id = m_next++;
    if (m_next == k_no_bullet) m_next = 1;
    b.shot = shot;
    b.position = shot.origin;
    b.previous = shot.origin;
    b.velocity = shot.velocity;
    m_bullets.push_back(b);
    ++m_stats.fired;
    return b.id;
}

std::vector<BulletHit> Ballistics::resolve(const Shot& shot) {
    std::vector<BulletHit> out;
    const float speed = length(shot.velocity);
    if (!finite(shot.origin) || !finite(shot.velocity) || !(shot.mass > 0.0f) || !std::isfinite(shot.mass) ||
        !(shot.penetration > 0.0f) || !std::isfinite(shot.penetration) || !(speed > 0.0f)) {
        ++m_stats.refused;
        return out;
    }
    Bullet b;
    b.id = k_no_bullet;
    b.shot = shot;
    b.position = shot.origin;
    b.previous = shot.origin;
    b.velocity = shot.velocity;
    ++m_stats.fired;
    while (!b.done) advance(b, out);
    m_stats.hits += out.size();
    return out;
}

u32 Ballistics::step(double seconds) {
    const auto start = std::chrono::steady_clock::now();
    m_hits.clear();
    if (!std::isfinite(seconds) || seconds <= 0.0) {
        m_stats.last_step_ms = 0.0;
        return 0;
    }
    m_carried += seconds;
    u32 run = 0;
    const u64 rays_before = m_stats.rays;
    bool over_rays = false;
    while (m_carried >= k_step_seconds && run < max_steps_per_call) {
        if (m_stats.rays - rays_before >= max_rays_per_call) {
            over_rays = true;
            break;
        }
        m_carried -= k_step_seconds;
        ++run;
        for (Bullet& b : m_bullets) advance(b, m_hits);
        std::erase_if(m_bullets, [](const Bullet& b) { return b.done; });
        ++m_stats.steps;
    }
    // Past the ray budget the steps owed wait (at most a call's worth);
    // past the step cap they are dropped.
    if (over_rays) {
        m_carried = std::min(m_carried, k_step_seconds * max_steps_per_call);
    } else if (m_carried >= k_step_seconds) {
        m_carried = 0.0;
    }
    m_stats.hits += m_hits.size();
    m_stats.last_step_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return run;
}

void Ballistics::advance(Bullet& b, std::vector<BulletHit>& out) {
    if (b.done) return;
    const float dt = static_cast<float>(k_step_seconds);
    const Vec3 v0 = b.velocity;
    // Quadratic drag along the way it goes (exact for drag alone over the
    // step), then gravity.
    const float speed = length(v0);
    const float slowed = speed / (1.0f + m_settings.drag * speed * dt);
    const Vec3 v1 = (speed > 0.0f ? v0 * (slowed / speed) : v0) + m_settings.gravity * dt;
    const Vec3 move = (v0 + v1) * (0.5f * dt);
    const float metres = length(move);
    b.previous = b.position;
    b.velocity = v1;
    b.seconds += k_step_seconds;
    if (metres > 0.0f) fly(b, b.position, move * (1.0f / metres), static_cast<double>(metres), out);
    if (!b.done && (length(b.velocity) < m_settings.min_speed || b.distance >= m_settings.max_range ||
                    b.seconds >= m_settings.max_seconds || !finite(b.position))) {
        b.done = true;
    }
}

void Ballistics::fly(Bullet& b, DVec3 from, Vec3 dir, double left, std::vector<BulletHit>& out) {
    const BodyId ignore = b.shot.ignore;
    PhysicsWorld::RayFilter any;
    if (ignore != k_no_body) {
        any.accept = [ignore](BodyId body, u64) { return body != ignore; };
    }
    const float mass = b.shot.mass;
    u32 layers = 0;
    while (left > 1e-6) {
        if (layers >= m_settings.max_layers) break; // the rest of this step's way dropped
        ++m_stats.rays;
        const std::optional<RayHit> hit = m_physics.raycast(from, dir, left, any);
        if (!hit) {
            from = along(from, dir, left);
            b.distance += left;
            left = 0.0;
            break;
        }
        ++layers;
        left -= hit->distance;
        b.distance += hit->distance;
        const float speed = length(b.velocity);
        const float energy = 0.5f * mass * speed * speed;
        const Target target = target_of ? target_of(*hit) : Target{};
        const world::Surface surface = target.surface;
        const usize index = static_cast<usize>(surface) < world::k_surface_count ? static_cast<usize>(surface) : 0;
        const SurfaceBallistics& props = m_settings.surfaces[index];
        Vec3 normal = hit->normal;
        if (!(length(normal) > 0.5f) || !finite(normal)) normal = -dir;

        BulletHit h;
        h.bullet = b.id;
        h.tag = b.shot.tag;
        h.body = hit->body;
        h.user = hit->user;
        h.part = hit->part;
        h.owner = target.owner;
        h.piece = target.piece;
        h.point = hit->point;
        h.normal = normal;
        h.direction = dir;
        h.surface = surface;
        h.speed_in = speed;
        h.distance = b.distance;

        // Glancing off: the angle between its way and the surface. A ray
        // that starts inside (met at once, or a face it is leaving) is in
        // it already: it never glances off.
        const bool inside = hit->distance < 1e-6 || dot(dir, normal) >= 0.0f;
        const float facing = std::clamp(-dot(dir, normal), 0.0f, 1.0f);
        const float grazing = radians_to_degrees(std::asin(facing));
        if (!inside && props.ricochet_degrees > 0.0f && grazing < props.ricochet_degrees) {
            Vec3 away = normalize_or_zero(dir + normal * (2.0f * facing));
            if (!(length(away) > 0.5f)) away = normal;
            const float keep = std::clamp(props.ricochet_keep, 0.0f, 1.0f);
            const float out_speed = speed * keep;
            h.outcome = HitOutcome::ricochet;
            h.speed_out = out_speed;
            h.energy = energy - 0.5f * mass * out_speed * out_speed;
            h.impulse = (dir * speed - away * out_speed) * mass;
            h.exit = hit->point;
            out.push_back(h);
            b.velocity = away * out_speed;
            dir = away;
            from = along(hit->point, normal, k_clear);
            if (out_speed < m_settings.min_speed) {
                b.done = true;
                b.position = hit->point;
                return;
            }
            continue;
        }

        // Going in: as far as its energy takes it, and whether the part it
        // went into ends before that (a ray back at that part alone, from
        // that far in, finds its far side; one starting inside the part, or
        // none, means it goes on further than the bullet can).
        const float resistance = std::max(props.resistance / b.shot.penetration, 1e-3f);
        const float reach = std::min(energy / resistance, m_settings.max_thickness);
        float thickness = -1.0f;
        if (reach > 1e-4f) {
            const double probe = static_cast<double>(reach) + k_clear;
            PhysicsWorld::RayFilter same;
            same.body = hit->body;
            same.part = hit->part;
            ++m_stats.rays;
            const std::optional<RayHit> back = m_physics.raycast(along(hit->point, dir, probe), -dir, probe, same);
            // A far side as near as the entry is the entry seen from
            // behind (the ground's height field): no far side at all.
            if (back && back->distance > 1e-5) {
                const float through = static_cast<float>(probe - back->distance);
                if (through > k_min_thickness && through <= reach) thickness = through;
            }
        }
        if (thickness >= 0.0f) {
            const float left_energy = std::max(energy - resistance * thickness, 0.0f);
            const float out_speed = std::sqrt(2.0f * left_energy / mass);
            h.outcome = HitOutcome::passed;
            h.speed_out = out_speed;
            h.energy = energy - left_energy;
            h.impulse = dir * ((speed - out_speed) * mass);
            h.thickness = thickness;
            h.exit = along(hit->point, dir, static_cast<double>(thickness));
            out.push_back(h);
            b.velocity = speed > 0.0f ? b.velocity * (out_speed / speed) : b.velocity;
            b.distance += thickness;
            left -= thickness;
            from = along(h.exit, dir, k_clear);
            if (out_speed < m_settings.min_speed) {
                b.done = true;
                b.position = h.exit;
                return;
            }
            continue;
        }
        h.outcome = HitOutcome::stopped;
        h.speed_out = 0.0f;
        h.energy = energy;
        h.impulse = b.velocity * mass;
        h.thickness = std::max(reach, 0.0f);
        h.exit = along(hit->point, dir, static_cast<double>(h.thickness));
        out.push_back(h);
        b.velocity = {};
        b.position = h.exit;
        b.done = true;
        return;
    }
    b.position = from;
}

std::vector<BulletState> Ballistics::bullets() const {
    std::vector<BulletState> out;
    out.reserve(m_bullets.size());
    for (const Bullet& b : m_bullets) {
        out.push_back({b.id, b.shot.tag, b.position, b.previous, b.velocity, b.distance, b.seconds});
    }
    return out;
}

void Ballistics::clear() {
    m_bullets.clear();
    m_hits.clear();
    m_carried = 0.0;
}

BallisticsStats Ballistics::stats() const {
    BallisticsStats s = m_stats;
    s.flying = static_cast<u32>(m_bullets.size());
    return s;
}

} // namespace giant::sim
