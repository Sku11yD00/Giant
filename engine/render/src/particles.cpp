#include "particles.h"

#include "giant/core/assert.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace giant::render {

namespace {

// E1 = (1 - e^-kt) / k and E2 = (t - E1) / k, by series where kt is under
// a half (where the divisions would lose the digits: t and t^2/2 with no
// drag; the first term left out is about 1e-8 of the sum, under a float's step).
struct Fall {
    float e1 = 0.0f;
    float e2 = 0.0f;
    float decay = 1.0f; // e^-kt
};

Fall fall(float k, float t) {
    const float x = k * t;
    Fall f;
    if (x < 0.5f) {
        // E1/t = sum (-x)^n / (n+1)!, E2/t^2 = sum (-x)^n / (n+2)!, to x^7.
        f.e1 = t * (1.0f - x * (1.0f / 2.0f - x * (1.0f / 6.0f - x * (1.0f / 24.0f - x * (1.0f / 120.0f -
                     x * (1.0f / 720.0f - x * (1.0f / 5040.0f - x * (1.0f / 40320.0f))))))));
        f.e2 = t * t * (1.0f / 2.0f - x * (1.0f / 6.0f - x * (1.0f / 24.0f - x * (1.0f / 120.0f - x * (1.0f / 720.0f -
                         x * (1.0f / 5040.0f - x * (1.0f / 40320.0f - x * (1.0f / 362880.0f))))))));
        f.decay = 1.0f - k * f.e1;
    } else {
        f.decay = std::exp(-x);
        f.e1 = (1.0f - f.decay) / k;
        f.e2 = (t - f.e1) / k;
    }
    return f;
}

float height(const ParticleMotion& m, float t) {
    const Fall f = fall(m.drag, t);
    return m.x0.y + m.air.y * t + (m.v0.y - m.air.y) * f.e1 + m.lift * f.e2;
}

bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool at_least(const Vec3& v, float lo) { return v.x >= lo && v.y >= lo && v.z >= lo; }

// Integer hash (lowbias32): the seeds.
u32 mix(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

struct Dice {
    u32 state;
    float next() { // [0, 1)
        state = mix(state + 0x9e3779b9U);
        return static_cast<float>(state >> 8) * (1.0f / 16777216.0f);
    }
};

Vec3 any_way(Dice& d) {
    const float z = 1.0f - 2.0f * d.next();
    const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    const float phi = 2.0f * k_pi * d.next();
    return {r * std::cos(phi), r * std::sin(phi), z};
}

void store(float (&out)[3], const Vec3& v) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
}

Vec3 load(const float (&in)[3]) { return {in[0], in[1], in[2]}; }

u32 saturate_add(u32 a, u32 b) { return a > ~0u - b ? ~0u : a + b; }

} // namespace

Vec3 particle_position(const ParticleMotion& m, float age) {
    const float t = std::clamp(age, 0.0f, std::max(m.stop, 0.0f));
    const Fall f = fall(m.drag, t);
    return m.x0 + m.air * t + (m.v0 - m.air) * f.e1 + Vec3{0.0f, m.lift, 0.0f} * f.e2;
}

Vec3 particle_velocity(const ParticleMotion& m, float age) {
    if (age >= m.stop) return {};
    const float t = std::max(age, 0.0f);
    const Fall f = fall(m.drag, t);
    return m.air + (m.v0 - m.air) * f.decay + Vec3{0.0f, m.lift, 0.0f} * f.e1;
}

float particle_ground_stop(const ParticleMotion& m, float ground, float life) {
    const float never = life * 2.0f + 1.0f;
    if (!(life > 0.0f)) return never;
    if (height(m, 0.0f) <= ground) return 0.0f;
    // Where it turns (its climb is zero), if inside its life: the height
    // is monotonic on each side of that. The climb is
    // v_term + (v0 - v_term) e^-kt with v_term = air + lift / k (v0 + lift t
    // with no drag).
    float turn = 0.0f;
    if (m.drag * life < 1e-3f) {
        if (m.lift != 0.0f) turn = -m.v0.y / m.lift;
    } else {
        const float v_term = m.air.y + m.lift / m.drag;
        const float r = m.v0.y != v_term ? -v_term / (m.v0.y - v_term) : 0.0f;
        if (r > 0.0f && r < 1.0f) turn = -std::log(r) / m.drag;
    }
    turn = turn > 0.0f && turn < life ? turn : 0.0f;
    float lo = 0.0f;
    float hi = 0.0f;
    if (turn > 0.0f && height(m, turn) <= ground) {
        hi = turn;
    } else if (height(m, life) <= ground) {
        lo = turn;
        hi = life;
    } else {
        return never;
    }
    // Bracketed (above at lo, at or below at hi): Newton from the end where
    // the height curves away from the ground (it then closes in from that
    // side, never overshooting), halving should a step leave the bracket.
    const float bend = m.lift - m.drag * (m.v0.y - m.air.y); // the acceleration's sign
    float t = bend < 0.0f ? hi : lo;
    // 0.1 mm, or a few steps of a float that high.
    const float tolerance = 1e-4f + 5e-7f * std::max(std::abs(ground), std::abs(m.x0.y));
    for (int i = 0; i < 48 && hi - lo > 1e-6f * std::max(1.0f, hi); ++i) {
        const Fall f = fall(m.drag, t);
        const float y = m.x0.y + m.air.y * t + (m.v0.y - m.air.y) * f.e1 + m.lift * f.e2 - ground;
        if (std::abs(y) < tolerance) return t; // on it
        (y <= 0.0f ? hi : lo) = t;
        const float v = m.air.y + (m.v0.y - m.air.y) * f.decay + m.lift * f.e1;
        float next = v != 0.0f ? t - y / v : -1.0f;
        if (!(next > lo && next < hi)) next = 0.5f * (lo + hi);
        t = next;
    }
    return hi; // the bracket closed: the first age found at or below
}

bool particle_style_ok(const ParticleStyle& s) {
    const float numbers[] = {s.life_min, s.life_max, s.size_start, s.size_end,  s.opacity_start, s.opacity_end,
                             s.emissive_end, s.gravity, s.buoyancy, s.drag,     s.wind,          s.wobble,
                             s.soft};
    for (const float n : numbers) {
        if (!std::isfinite(n)) return false;
    }
    if (!finite(s.color_start) || !finite(s.color_end) || !finite(s.emissive)) return false;
    if (!(s.life_min > 0.0f) || s.life_max < s.life_min) return false;
    // Bounded, so no motion or look overflows a float within a life.
    if (std::abs(s.gravity) > k_particle_max_gravity || std::abs(s.buoyancy) > k_particle_max_gravity * k_particle_gravity ||
        std::abs(s.wind) > k_particle_max_gravity) {
        return false;
    }
    if (s.size_start > k_particle_max_size || s.size_end > k_particle_max_size || s.wobble > k_particle_max_size ||
        s.soft > k_particle_max_size) {
        return false;
    }
    const float brightest = std::max({s.emissive.x, s.emissive.y, s.emissive.z, s.color_start.x, s.color_start.y,
                                      s.color_start.z, s.color_end.x, s.color_end.y, s.color_end.z});
    if (brightest > k_particle_max_emissive || s.emissive_end > k_particle_max_emissive) return false;
    if (s.size_start < 0.0f || s.size_end < 0.0f || s.drag < 0.0f || s.wobble < 0.0f || s.soft < 0.0f) return false;
    if (s.opacity_start < 0.0f || s.opacity_end < 0.0f || s.emissive_end < 0.0f) return false;
    if (!at_least(s.color_start, 0.0f) || !at_least(s.color_end, 0.0f) || !at_least(s.emissive, 0.0f)) return false;
    return s.shape == ParticleShape::puff || s.shape == ParticleShape::streak;
}

bool particle_burst_ok(const ParticleBurst& b) {
    const float numbers[] = {b.spread, b.cone_degrees, b.speed_min, b.speed_max, b.size_scale};
    for (const float n : numbers) {
        if (!std::isfinite(n)) return false;
    }
    if (!std::isfinite(b.position.x) || !std::isfinite(b.position.y) || !std::isfinite(b.position.z)) return false;
    if (!finite(b.direction) || !finite(b.velocity) || std::isnan(b.ground)) return false;
    if (length_squared(b.direction) < 1e-12f) return false;
    if (b.spread < 0.0f || b.spread > 1000.0f || b.speed_min < 0.0f || b.speed_max < b.speed_min) return false;
    if (b.speed_max > k_particle_max_speed || length(b.velocity) > k_particle_max_speed) return false;
    return b.size_scale > 0.0f && b.size_scale <= 1000.0f;
}

gpu::ParticleStyle to_gpu(const ParticleStyle& s, bool usable) {
    gpu::ParticleStyle g{};
    if (!usable) return g; // flags 0: draws nothing
    g.size_start = s.size_start;
    g.size_end = s.size_end;
    g.opacity_start = std::clamp(s.opacity_start, 0.0f, 1.0f);
    g.opacity_end = std::clamp(s.opacity_end, 0.0f, 1.0f);
    store(g.color_start, s.color_start);
    store(g.color_end, s.color_end);
    store(g.emissive, s.emissive);
    g.emissive_end = s.emissive_end;
    g.soft = s.soft;
    g.wobble = s.wobble;
    g.flags = gpu::k_particle_style_usable | (s.shape == ParticleShape::streak ? gpu::k_particle_style_streak : 0u) |
              (s.lit ? gpu::k_particle_style_lit : 0u);
    return g;
}

ParticleMotion ParticleRing::motion(const gpu::Particle& p) {
    ParticleMotion m;
    m.x0 = load(p.position);
    m.v0 = load(p.velocity);
    m.air = load(p.air);
    m.lift = p.lift;
    m.drag = p.drag;
    m.stop = p.stop;
    return m;
}

float ParticleRing::age(const gpu::Particle& p) const {
    return static_cast<float>(static_cast<u32>(static_cast<u32>(m_now_us) - p.born)) * 1e-6f;
}

ParticleRing::ParticleRing() {
    m_born.reserve(k_max_particles_born);
    m_spans.resize(k_max_particles); // a span holds one particle at least: never more
}

void ParticleRing::push_span(const Span& span) {
    GIANT_ASSERT(m_span_count < m_spans.size(), "more spans than particles");
    m_spans[(m_span_first + m_span_count) % m_spans.size()] = span;
    ++m_span_count;
}

void ParticleRing::pop_span() {
    m_span_first = (m_span_first + 1) % m_spans.size();
    --m_span_count;
}

void ParticleRing::clear() {
    m_frame.cut += alive();
    m_tail = m_head;
    m_span_first = 0;
    m_span_count = 0;
}

void ParticleRing::cut_to(u64 tail) {
    if (tail <= m_tail) return;
    m_frame.cut += static_cast<u32>(tail - m_tail);
    m_tail = tail;
    while (m_span_count > 0 && front().end <= m_tail) pop_span();
}

void ParticleRing::forget_births() {
    if (m_frame.born == 0) return;
    m_head -= m_frame.born;
    --m_span_count; // this update's span, the newest
    if (m_tail > m_head) m_tail = m_head;
    m_frame.dropped = saturate_add(m_frame.dropped, m_frame.born);
    m_frame.born = 0;
    m_frame.count = alive();
    m_frame.first = static_cast<u32>(m_tail % k_max_particles);
    m_born.clear();
}

const ParticleRing::Frame& ParticleRing::update(std::span<const ParticleBurst> bursts,
                                                std::span<const ParticleStyle> styles, double delta_seconds,
                                                const DVec3& camera, const WindState& wind) {
    m_frame = {};
    m_born.clear();
    const double step = delta_seconds > 0.0 ? std::min(delta_seconds, 1.0) : 0.0; // (NaN: 0)
    m_now_us += static_cast<u64>(std::llround(step * 1e6));

    // Spans that died, from the oldest.
    while (m_span_count > 0 && front().expiry <= m_now_us) {
        m_tail = std::max(m_tail, front().end);
        pop_span();
    }
    if (m_span_count == 0) m_tail = m_head;
    // Positions are kept relative to an anchor near the camera (floats).
    const bool finite_camera = std::isfinite(camera.x) && std::isfinite(camera.y) && std::isfinite(camera.z);
    if (finite_camera) {
        if (alive() == 0) {
            m_anchor = camera;
            m_anchored = true;
        } else if (!m_anchored || length(camera - m_anchor) > k_particle_reanchor) {
            clear();
            m_anchor = camera;
            m_anchored = true;
        }
    }

    const usize style_count = std::min<usize>(styles.size(), k_max_particle_styles);
    m_style_ok.resize(style_count);
    for (usize i = 0; i < style_count; ++i) m_style_ok[i] = particle_style_ok(styles[i]) ? 1 : 0;

    Vec3 wind_air = finite(wind.velocity) ? wind.velocity : Vec3{};
    if (length(wind_air) > k_particle_max_speed) wind_air = normalize_or_zero(wind_air) * k_particle_max_speed;
    u64 expiry = 0;
    u32 grounded_born = 0;
    for (const ParticleBurst& b : bursts) {
        const bool grounded = b.ground > -1.0e29;
        const u32 room = std::min(k_max_particles_born - static_cast<u32>(m_born.size()),
                                  grounded ? k_max_particles_grounded - grounded_born : k_max_particles_born);
        const u32 count = std::min(b.count, room);
        m_frame.dropped = saturate_add(m_frame.dropped, b.count - count);
        if (count == 0) continue;
        if (!finite_camera || b.style >= style_count || m_style_ok[b.style] == 0 || !particle_burst_ok(b) ||
            length(b.position - camera) > k_particle_reach) {
            m_frame.dropped = saturate_add(m_frame.dropped, count);
            continue;
        }
        const ParticleStyle& s = styles[b.style];
        const Vec3 dir = normalize_or_zero(b.direction);
        // A frame around the direction, for the cone.
        const Vec3 side = normalize_or_zero(std::abs(dir.y) < 0.9f ? cross(dir, Vec3{0.0f, 1.0f, 0.0f})
                                                                    : cross(dir, Vec3{1.0f, 0.0f, 0.0f}));
        const Vec3 up = cross(side, dir);
        const float cos_max = std::cos(degrees_to_radians(std::clamp(b.cone_degrees, 0.0f, 180.0f)));
        Vec3 air = wind_air * s.wind;
        if (length(air) > k_particle_max_speed) air = normalize_or_zero(air) * k_particle_max_speed;
        const float lift = s.buoyancy - k_particle_gravity * s.gravity;
        const float drag = std::min(s.drag, k_particle_max_drag);
        if (grounded) grounded_born += count;
        for (u32 i = 0; i < count; ++i) {
            Dice d{mix(b.seed ^ mix(i + 0x632be5abU))};
            const Vec3 offset = b.spread > 0.0f ? any_way(d) * (b.spread * std::cbrt(d.next())) : Vec3{};
            const float cos_t = 1.0f - d.next() * (1.0f - cos_max);
            const float sin_t = std::sqrt(std::max(0.0f, 1.0f - cos_t * cos_t));
            const float phi = 2.0f * k_pi * d.next();
            const Vec3 way = dir * cos_t + (side * std::cos(phi) + up * std::sin(phi)) * sin_t;
            const float speed = b.speed_min + (b.speed_max - b.speed_min) * d.next();
            const float life =
                std::min(s.life_min + (s.life_max - s.life_min) * d.next(), k_max_particle_life);
            const DVec3 at = b.position + DVec3{offset.x, offset.y, offset.z};

            gpu::Particle p{};
            store(p.position, to_camera_relative(at, m_anchor));
            store(p.velocity, way * speed + b.velocity);
            store(p.air, air);
            p.born = static_cast<u32>(m_now_us);
            p.life = life;
            p.lift = lift;
            p.drag = drag;
            p.size = b.size_scale;
            p.seed_style = (mix(d.state) << 8) | b.style;
            p.stop = 1.0e30f;
            if (grounded) {
                ParticleMotion m = motion(p);
                m.x0 = {};
                const float stop = particle_ground_stop(m, static_cast<float>(std::max(b.ground - at.y, -1.0e30)), life);
                if (stop <= life) p.stop = stop;
            }
            expiry = std::max(expiry, m_now_us + static_cast<u64>(std::ceil(static_cast<double>(life) * 1e6)));
            m_born.push_back(p);
        }
    }

    const u32 born = static_cast<u32>(m_born.size());
    if (born > 0) {
        if (alive() + born > k_max_particles) cut_to(m_head + born - k_max_particles);
        m_frame.born_first = static_cast<u32>(m_head % k_max_particles);
        m_head += born;
        push_span({m_head, expiry});
    }
    m_frame.now_us = m_now_us;
    m_frame.anchor = m_anchor;
    m_frame.first = static_cast<u32>(m_tail % k_max_particles);
    m_frame.count = alive();
    m_frame.born = born;
    return m_frame;
}

} // namespace giant::render
