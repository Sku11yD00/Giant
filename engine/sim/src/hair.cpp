#include "giant/sim/hair.h"

#include <algorithm>
#include <cmath>

namespace giant::sim {

namespace {

// A move of the head further than this, and faster than this since the
// last step, carries the hair whole.
constexpr float k_jump = 5.0f;        // metres
constexpr float k_jump_speed = 100.0f; // m/s
// How the share of a strict length fix that the point above takes off its
// speed falls with the step's fall over the piece's length (see advance).
constexpr float k_give_back = 0.7f;
// The furthest out a rest point lies on each axis (metres).
constexpr float k_reach = 1.0e6f;
// A move smaller than this (metres, the groom's space) is not shown.
constexpr float k_still = 1.0e-5f;

Quat between(const Quat& a, const Quat& b, float share) {
    if (share >= 1.0f) return b;
    const float side = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0f ? -1.0f : 1.0f;
    const float from = (1.0f - share) * side;
    return normalize(Quat{a.x * from + b.x * share, a.y * from + b.y * share, a.z * from + b.z * share,
                          a.w * from + b.w * share});
}

bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

float safe(float v, float low, float high, float otherwise) {
    return std::isfinite(v) ? std::clamp(v, low, high) : otherwise;
}

// The point of the bar a to b nearest `p`.
Vec3 nearest_on(const Vec3& a, const Vec3& b, const Vec3& p) {
    const Vec3 ab = b - a;
    const float along = length_squared(ab);
    const float t = along > 0.0f ? std::clamp(dot(p - a, ab) / along, 0.0f, 1.0f) : 0.0f;
    return a + ab * t;
}

} // namespace

HairSettings clean(const HairSettings& s) {
    const HairSettings d;
    HairSettings c;
    c.root_stiffness = safe(s.root_stiffness, 0.0f, 60.0f, d.root_stiffness);
    c.tip_stiffness = safe(s.tip_stiffness, 0.0f, 60.0f, d.tip_stiffness);
    c.damping = safe(s.damping, 0.0f, 60.0f, d.damping);
    c.gravity = safe(s.gravity, 0.0f, 100.0f, d.gravity);
    c.wind_response = safe(s.wind_response, 0.0f, 100.0f, d.wind_response);
    c.thickness = safe(s.thickness, 0.0f, 0.1f, d.thickness);
    c.friction = safe(s.friction, 0.0f, 1.0f, d.friction);
    c.step_seconds = safe(s.step_seconds, 1.0f / 240.0f, 1.0f / 30.0f, d.step_seconds);
    c.max_steps = std::clamp(s.max_steps, 1u, 16u);
    return c;
}

void HairSim::set_motion(float root_stiffness, float tip_stiffness, float damping) {
    HairSettings wanted = m_settings;
    wanted.root_stiffness = root_stiffness;
    wanted.tip_stiffness = tip_stiffness;
    wanted.damping = damping;
    m_settings = clean(wanted);
}

HairSim::HairSim(std::span<const Vec3> rest_guides, const HairSettings& settings) : m_settings(clean(settings)) {
    m_guides = static_cast<u32>(rest_guides.size() / k_hair_points);
    const usize count = static_cast<usize>(m_guides) * k_hair_points;
    m_rest.assign(rest_guides.begin(), rest_guides.begin() + static_cast<std::ptrdiff_t>(count));
    for (Vec3& p : m_rest) {
        // No numbers become the origin; nothing lies further out than a
        // groom reaches (geometry::k_max_groom_reach).
        p = {safe(p.x, -k_reach, k_reach, 0.0f), safe(p.y, -k_reach, k_reach, 0.0f), safe(p.z, -k_reach, k_reach, 0.0f)};
    }
    m_lengths.assign(count, 0.0f);
    m_reach.assign(m_guides, 0.0f);
    for (usize i = 0; i < count; ++i) {
        if (i % k_hair_points != 0) {
            m_lengths[i] = length(m_rest[i] - m_rest[i - 1]);
            m_reach[i / k_hair_points] += m_lengths[i];
        }
    }
    m_points = m_rest;
    m_previous = m_rest;
    m_local = m_rest;
    m_targets = m_rest;
    m_bound.assign(count * k_max_hair_capsules, 0.0f);
    m_give.assign(count, 0.0f);
    m_near.assign(m_guides, 0);
    m_stats.guides = m_guides;
}

void HairSim::reset() {
    m_placed = false;
    m_time = 0.0f;
    if (m_local != m_rest) {
        m_local = m_rest;
        ++m_version;
    }
}

void HairSim::bind(const HairFrame& frame, u32 capsules) {
    // How near each capsule's axis each point rests, as the hair is placed:
    // a point is never kept further out than that (a head's capsule does
    // not lift hair off the scalp it grows from).
    for (usize i = 0; i < m_rest.size(); ++i) {
        const Vec3 rests = rotate(frame.rotation, m_rest[i]) * frame.scale;
        for (u32 c = 0; c < capsules; ++c) {
            const HairCapsule& capsule = frame.capsules[c];
            const float distance = length(rests - nearest_on(capsule.a, capsule.b, rests));
            m_bound[i * k_max_hair_capsules + c] = std::isfinite(distance) ? distance : 0.0f;
        }
    }
    m_bound_capsules = capsules;
}

void HairSim::place_at_rest(const HairFrame& frame, u32 capsules) {
    for (usize i = 0; i < m_rest.size(); ++i) {
        m_points[i] = rotate(frame.rotation, m_rest[i]) * frame.scale;
        m_previous[i] = m_points[i];
    }
    m_position = frame.position;
    m_rotation = frame.rotation;
    m_last_position = frame.position;
    m_last_rotation = frame.rotation;
    m_scale = frame.scale;
    m_placed = true;
    m_time = 0.0f;
    bind(frame, capsules);
    weigh_give();
    if (m_local != m_rest) {
        m_local = m_rest;
        ++m_version;
    }
}

void HairSim::weigh_give() {
    // The share of a strict length fix that the point above takes off its
    // speed (see advance): less the further a point falls in a step
    // against its piece's length.
    const float dt = m_settings.step_seconds;
    for (usize i = 0; i < m_give.size(); ++i) {
        const float fallen = m_settings.gravity * dt * dt / std::max(m_lengths[i] * m_scale, 1e-9f);
        m_give[i] = std::clamp(1.0f - k_give_back * std::cbrt(fallen), 0.0f, 1.0f);
    }
}

void HairSim::to_local() {
    const Quat back = conjugate(m_rotation);
    const float shrink = 1.0f / m_scale;
    // Moves too small to see are not moves (hair at rest stays the same
    // version); they are not lost, only shown once they add up.
    bool changed = false;
    for (usize i = 0; i < m_points.size() && !changed; ++i) {
        const Vec3 off = rotate(back, m_points[i]) * shrink - m_local[i];
        changed = !(std::abs(off.x) <= k_still && std::abs(off.y) <= k_still && std::abs(off.z) <= k_still);
    }
    if (!changed) return;
    for (usize i = 0; i < m_points.size(); ++i) m_local[i] = rotate(back, m_points[i]) * shrink;
    ++m_version;
}

void HairSim::step(double seconds, const HairFrame& given) {
    m_stats.steps = 0;
    if (!(seconds >= 0.0) || !std::isfinite(seconds) || m_guides == 0) return;
    HairFrame frame = given;
    frame.scale = safe(given.scale, 1e-3f, 1e3f, 1.0f);
    const Quat& q = given.rotation;
    const float turn = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (!std::isfinite(turn) || turn < 1e-12f || !std::isfinite(given.position.x) || !std::isfinite(given.position.y) ||
        !std::isfinite(given.position.z)) {
        return; // nowhere to hang, or no way round: as it was
    }
    frame.rotation = normalize(given.rotation);
    if (!finite(frame.wind)) frame.wind = {};
    const u32 capsules = static_cast<u32>(std::min<usize>(frame.capsules.size(), k_max_hair_capsules));

    if (!m_placed) {
        place_at_rest(frame, capsules);
        return;
    }
    // A jump (further and faster than anything runs) carries the hair whole.
    const float passed = static_cast<float>(std::min(seconds, 1.0));
    const DVec3 jump = frame.position - m_last_position;
    const double far = std::sqrt(jump.x * jump.x + jump.y * jump.y + jump.z * jump.z);
    if (!(far <= static_cast<double>(k_jump + k_jump_speed * passed))) {
        place_at_rest(frame, capsules);
        ++m_stats.resets;
        return;
    }
    if (capsules != m_bound_capsules) bind(frame, capsules);
    // A new size: the hair as it hangs, that much bigger or smaller.
    if (frame.scale != m_scale) {
        const float grown = frame.scale / m_scale;
        for (usize i = 0; i < m_points.size(); ++i) {
            m_points[i] *= grown;
            m_previous[i] *= grown;
        }
        for (float& bound : m_bound) bound *= grown;
        m_scale = frame.scale;
        weigh_give();
        ++m_version; // points() is the same but stands for another size
    }

    // The hair is simulated up to the last whole step before now and
    // rides the head from there: its points are about where the head was
    // at that time (between the last frame's place and this one's). A
    // frame shorter than a step takes none and changes nothing, so frames
    // of any length neither stretch the hair nor show it a head faster or
    // slower than it is.
    const float dt = m_settings.step_seconds;
    const float waiting = m_time + passed;
    const u32 steps = std::min(static_cast<u32>(waiting / dt), m_settings.max_steps);
    const bool dropped = waiting - static_cast<float>(steps) * dt >= dt; // a slow frame: the time past max_steps
    const Quat to_groom = conjugate(frame.rotation);
    bool fine = true;
    for (u32 s = 1; s <= steps && fine; ++s) {
        const float share = dropped ? static_cast<float>(s) / static_cast<float>(steps)
                                    : std::clamp((static_cast<float>(s) * dt - m_time) / passed, 0.0f, 1.0f);
        const DVec3 position = m_last_position + (frame.position - m_last_position) * static_cast<double>(share);
        const Quat rotation = between(m_last_rotation, frame.rotation, share);
        // The points stay where they are in the world as the place moves on.
        const DVec3 moved_far = position - m_position;
        const Vec3 moved{static_cast<float>(moved_far.x), static_cast<float>(moved_far.y), static_cast<float>(moved_far.z)};
        for (usize i = 0; i < m_points.size(); ++i) {
            m_points[i] -= moved;
            m_previous[i] -= moved;
        }
        m_position = position;
        m_rotation = rotation;
        // The capsules ride the head: where they were at this step's time.
        for (u32 c = 0; c < capsules; ++c) {
            const HairCapsule& capsule = frame.capsules[c];
            HairCapsule& at = m_capsules[c];
            if (!(capsule.radius >= 0.0f) || !std::isfinite(capsule.radius) || !finite(capsule.a) || !finite(capsule.b)) {
                at.radius = -1.0f; // no capsule at all
                continue;
            }
            at.a = rotate(rotation, rotate(to_groom, capsule.a));
            at.b = rotate(rotation, rotate(to_groom, capsule.b));
            at.radius = capsule.radius;
        }
        // Which of them each guide could reach at all, by its length.
        for (u32 g = 0; g < m_guides; ++g) {
            const Vec3 root = rotate(rotation, m_rest[static_cast<usize>(g) * k_hair_points]) * frame.scale;
            const float reach = m_reach[g] * frame.scale * 1.25f + m_settings.thickness + 1e-3f;
            u32 near = 0;
            for (u32 c = 0; c < capsules; ++c) {
                const HairCapsule& at = m_capsules[c];
                if (at.radius < 0.0f) continue;
                if (length(root - nearest_on(at.a, at.b, root)) <= reach + at.radius) near |= 1u << c;
            }
            m_near[g] = static_cast<u16>(near);
        }
        fine = advance(dt, frame);
    }
    m_time = dropped ? 0.0f : waiting - static_cast<float>(steps) * dt;
    m_last_position = frame.position;
    m_last_rotation = frame.rotation;
    m_stats.steps = steps;
    if (!fine) {
        place_at_rest(frame, capsules);
        ++m_stats.resets;
        return;
    }
    if (steps > 0) to_local();
}

bool HairSim::advance(float dt, const HairFrame& frame) {
    const Vec3 fall{0.0f, -m_settings.gravity * dt * dt, 0.0f};
    const float keep = std::exp(-m_settings.damping * dt);
    const float drag = m_settings.wind_response * dt;
    const float inverse_dt = 1.0f / dt;
    const float stand_off = m_settings.thickness;
    const float grip = m_settings.friction;
    // The pull back to the style is a spring of the stiffness's frequency,
    // taken implicitly (steady whatever the step): the share of the way
    // back a point goes this step.
    const auto pull = [&](float hertz) {
        const float w = 2.0f * k_pi * hertz * dt;
        return w * w / (1.0f + w * w);
    };
    const float root_pull = pull(m_settings.root_stiffness);
    const float tip_pull = pull(m_settings.tip_stiffness);

    // `rub`: the capsule holds the point back as it slides along it (by
    // the friction, times how hard it was pressed in). True if it moved.
    // `hard`: out of the capsule itself only, and only for a point that
    // rested outside it (one that grows inside a head's capsule has no
    // place there to be put out to without stretching its piece).
    const auto keep_out = [&](usize i, u32 near, bool rub, bool hard) {
        Vec3& p = m_points[i];
        bool pushed = false;
        for (u32 c = 0; near != 0; ++c, near >>= 1u) {
            if ((near & 1u) == 0) continue;
            const HairCapsule& capsule = m_capsules[c];
            const float bound = m_bound[i * k_max_hair_capsules + c];
            if (hard && bound < capsule.radius) continue;
            const float allowed = hard ? capsule.radius : std::min(capsule.radius + stand_off, bound);
            if (allowed <= 0.0f) continue;
            const Vec3 on_axis = nearest_on(capsule.a, capsule.b, p);
            const Vec3 out = p - on_axis;
            const float distance = length(out);
            if (distance >= allowed) continue;
            pushed = true;
            if (distance > 1e-6f) {
                const Vec3 normal = out * (1.0f / distance);
                p = on_axis + normal * allowed;
                if (rub && grip > 0.0f) {
                    const Vec3 slid = p - m_previous[i];
                    const Vec3 along = slid - normal * dot(slid, normal);
                    const float slide = length(along);
                    const float held = grip * (allowed - distance);
                    if (slide > 0.0f) p -= along * std::min(1.0f, held / slide);
                }
            } else {
                // On the axis itself: out the way it rests.
                const Vec3 way = normalize_or_zero(m_targets[i] - nearest_on(capsule.a, capsule.b, m_targets[i]));
                p = on_axis + (length_squared(way) > 0.0f ? way : Vec3{0.0f, 1.0f, 0.0f}) * allowed;
            }
        }
        return pushed;
    };

    bool fine = true;
    for (u32 g = 0; g < m_guides; ++g) {
        const usize root = static_cast<usize>(g) * k_hair_points;
        const u32 near = m_near[g];
        for (u32 k = 0; k < k_hair_points; ++k) {
            m_targets[root + k] = rotate(m_rotation, m_rest[root + k]) * frame.scale;
        }
        // Damping is the hair's own (strand on strand): it slows a point
        // against its root's move, not against the world, so hair on a
        // running head is not held back by it.
        const Vec3 carried = m_targets[root] - m_points[root];
        m_points[root] = m_targets[root];
        m_previous[root] = m_targets[root];
        for (u32 k = 1; k < k_hair_points; ++k) {
            const usize i = root + k;
            const float t = static_cast<float>(k) / static_cast<float>(k_hair_points - 1);
            Vec3 velocity = carried + (m_points[i] - m_previous[i] - carried) * keep; // metres a step
            // The air drags it toward its own speed, harder the faster it
            // blows past (never past the air's speed: the share is capped).
            const Vec3 passing = frame.wind - velocity * inverse_dt;
            const float share = std::min(length(passing) * drag, 1.0f);
            velocity = velocity + passing * (share * dt);
            m_previous[i] = m_points[i];
            Vec3 p = m_points[i] + velocity + fall;
            // Back toward the style.
            p = p + (m_targets[i] - p) * (root_pull + (tip_pull - root_pull) * t);
            m_points[i] = p;
        }
        // From root to tip, each piece set to exactly its length by moving
        // its lower point: hair does not stretch. That alone would feed the
        // swing (a strand let go whips up higher than it began), so the
        // point above gives up speed for the move (Mueller et al., "Fast
        // simulation of inextensible hair and fur"). All of it is right for
        // fine steps; the further a point falls in a step against the
        // piece's length, the less it may give before hanging hair starts
        // to wriggle and never stops (weigh_give: measured to do neither
        // for even and uneven pieces of 3 mm to 6 cm at steps of 1/30 to
        // 1/240 s; sharing the error between a piece's ends first, as
        // cloth does, made the wriggle worse).
        for (u32 k = 1; k < k_hair_points; ++k) {
            const usize i = root + k;
            const float want = m_lengths[i] * frame.scale;
            const Vec3 was = m_points[i];
            // Against a capsule: out of it and the length again, a few times
            // over. The hair's thickness is the room for what is left: the
            // piece keeps its length and may lie a little into the stand-off.
            const auto to_length = [&] {
                const Vec3 piece = m_points[i] - m_points[i - 1];
                const float now = length(piece);
                if (now > 1e-9f) {
                    m_points[i] = m_points[i - 1] + piece * (want / now);
                } else if (want > 0.0f) {
                    m_points[i] = m_points[i - 1] + normalize_or_zero(m_targets[i] - m_targets[i - 1]) * want;
                }
            };
            to_length();
            const Vec3 drawn = m_points[i] - was; // the length's own move: what the point above answers for
            if (near != 0) {
                bool pressed = keep_out(i, near, true, false);
                for (u32 again = 0; again < 3 && pressed; ++again) {
                    to_length();
                    pressed = keep_out(i, near, false, false);
                    if (pressed && again < 2) continue;
                    // Still pressed after all that: its length it keeps, but
                    // never inside the capsule itself (then it stretches).
                    if (pressed) {
                        to_length();
                        keep_out(i, near, false, true);
                    }
                }
            }
            if (k > 1) m_previous[i - 1] += drawn * m_give[i];
            fine = fine && finite(m_points[i]);
        }
    }
    return fine;
}

} // namespace giant::sim
